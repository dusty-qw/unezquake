/*
Copyright (C) 2018 ezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifdef RENDERER_OPTION_MODERN_OPENGL

#include "quakedef.h"
#include "gl_model.h"
#include "gl_local.h"
#include "glm_local.h"
#include "glm_brushmodel.h"
#include "gl_framebuffer.h"
#include "tr_types.h"
#include "glm_vao.h"
#include "r_buffers.h"
#include "glm_local.h"
#include "r_program.h"
#include "r_renderer.h"
#include "r_aliasmodel.h"
#include "r_renderer.h"
#include "r_sprite3d.h"
#include "r_state.h"
#include "r_matrix.h"
#include "r_texture.h"
#include "gl_texture_internal.h"

texture_ref GL_FramebufferTextureReference(framebuffer_id id, fbtex_id tex_id);
qbool GLM_CompilePostProcessVAO(void);
GLuint GL_TextureNameFromReference(texture_ref ref);
void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLenum glCheckFramebufferStatus(GLenum target);

static texture_ref model_outline_jfa_targets[2];
static GLuint model_outline_jfa_fbos[2];
static int model_outline_jfa_width;
static int model_outline_jfa_height;

static void GLM_DeleteModelOutlineTargets(void)
{
	int i;

	for (i = 0; i < sizeof(model_outline_jfa_targets) / sizeof(model_outline_jfa_targets[0]); ++i) {
		if (R_TextureReferenceIsValid(model_outline_jfa_targets[i])) {
			renderer.TextureDelete(model_outline_jfa_targets[i]);
			R_TextureReferenceInvalidate(model_outline_jfa_targets[i]);
		}
	}

	if (model_outline_jfa_fbos[0]) {
		glDeleteFramebuffers(2, model_outline_jfa_fbos);
		memset(model_outline_jfa_fbos, 0, sizeof(model_outline_jfa_fbos));
	}

	model_outline_jfa_width = 0;
	model_outline_jfa_height = 0;
}

static qbool GLM_EnsureModelOutlineTargets(int width, int height)
{
	GLint previous_framebuffer = 0;
	int i;

	if (width <= 0 || height <= 0) {
		return false;
	}

	if (model_outline_jfa_width == width && model_outline_jfa_height == height &&
		R_TextureReferenceIsValid(model_outline_jfa_targets[0]) &&
		R_TextureReferenceIsValid(model_outline_jfa_targets[1]) &&
		model_outline_jfa_fbos[0] && model_outline_jfa_fbos[1]) {
		return true;
	}

	GLM_DeleteModelOutlineTargets();

	// Ping-pong pair used by the jump-flood passes.
	for (i = 0; i < sizeof(model_outline_jfa_targets) / sizeof(model_outline_jfa_targets[0]); ++i) {
		char label[64];

		snprintf(label, sizeof(label), "glm:outline-jfa-%d", i);
		GL_CreateTexturesWithIdentifier(texture_type_2d, 1, &model_outline_jfa_targets[i], label);
		if (!R_TextureReferenceIsValid(model_outline_jfa_targets[i])) {
			GLM_DeleteModelOutlineTargets();
			return false;
		}

		// JFA stores pixel coordinates in RG channels; keep full float precision
		// to avoid coordinate quantization artifacts (striping / false near distances).
		GL_TexStorage2D(model_outline_jfa_targets[i], 1, GL_RGBA32F, width, height, false);
		renderer.TextureSetFiltering(model_outline_jfa_targets[i], texture_minification_nearest, texture_magnification_nearest);
		renderer.TextureWrapModeClamp(model_outline_jfa_targets[i]);
		R_TextureSetFlag(model_outline_jfa_targets[i], R_TextureGetFlag(model_outline_jfa_targets[i]) | TEX_NO_TEXTUREMODE);
	}

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glGenFramebuffers(2, model_outline_jfa_fbos);

	for (i = 0; i < sizeof(model_outline_jfa_fbos) / sizeof(model_outline_jfa_fbos[0]); ++i) {
		GLenum status;

		glBindFramebuffer(GL_FRAMEBUFFER, model_outline_jfa_fbos[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GL_TextureNameFromReference(model_outline_jfa_targets[i]), 0);
		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
			GLM_DeleteModelOutlineTargets();
			return false;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
	model_outline_jfa_width = width;
	model_outline_jfa_height = height;
	return true;
}

// If this returns false then the framebuffer will be blitted instead
qbool GLM_CompileWorldGeometryProgram(void)
{
	int post_process_flags = 0;

	if (R_ProgramRecompileNeeded(r_program_fx_world_geometry, post_process_flags)) {
		// Initialise program for drawing image
		R_ProgramCompile(r_program_fx_world_geometry);

		R_ProgramSetCustomOptions(r_program_fx_world_geometry, post_process_flags);
	}

	return R_ProgramReady(r_program_fx_world_geometry) && GLM_CompilePostProcessVAO();
}

qbool GLM_CompileModelOutlineSeedProgram(void)
{
	if (R_ProgramRecompileNeeded(r_program_fx_outline_seed, 0)) {
		R_ProgramCompile(r_program_fx_outline_seed);
		R_ProgramSetCustomOptions(r_program_fx_outline_seed, 0);
	}

	return R_ProgramReady(r_program_fx_outline_seed) && GLM_CompilePostProcessVAO();
}

qbool GLM_CompileModelOutlineJfaProgram(void)
{
	if (R_ProgramRecompileNeeded(r_program_fx_outline_jfa, 0)) {
		R_ProgramCompile(r_program_fx_outline_jfa);
		R_ProgramSetCustomOptions(r_program_fx_outline_jfa, 0);
	}

	return R_ProgramReady(r_program_fx_outline_jfa) && GLM_CompilePostProcessVAO();
}

qbool GLM_CompileModelOutlineEffectProgram(void)
{
	if (R_ProgramRecompileNeeded(r_program_fx_outline_effect, 0)) {
		R_ProgramCompile(r_program_fx_outline_effect);
		R_ProgramSetCustomOptions(r_program_fx_outline_effect, 0);
	}

	return R_ProgramReady(r_program_fx_outline_effect) && GLM_CompilePostProcessVAO();
}

static qbool GLM_CompileModelOutlinePrograms(void)
{
	return GLM_CompileModelOutlineSeedProgram() && GLM_CompileModelOutlineJfaProgram() && GLM_CompileModelOutlineEffectProgram();
}

static void GLM_DrawWorldOutlines(void)
{
	texture_ref normals = GL_FramebufferTextureReference(framebuffer_std, fbtex_worldnormals);

	if (R_TextureReferenceIsValid(normals) && GLM_CompileWorldGeometryProgram()) {
		int viewport[4];
		int fullscreen_viewport[4];
		extern cvar_t gl_outline_color_world, gl_outline_world_depth_threshold, gl_outline_world_normal_threshold;

		R_GetViewport(viewport);

		// If we are only rendering to a section of the screen then that is the only part of the texture that will be filled in
		if (CL_MultiviewEnabled()) {
			R_GetFullScreenViewport(fullscreen_viewport);
			R_Viewport(fullscreen_viewport[0], fullscreen_viewport[1], fullscreen_viewport[2], fullscreen_viewport[3]);
			R_EnableScissorTest(viewport[0], viewport[1], viewport[2], viewport[3]);
		} else {
			// ignore viewsize and allat crap and set the viewport size to the whole window.
			// previously the viewport was already resized, and then resized again later, making the outlines not align.
			R_Viewport(0, 0, VID_ScaledWidth3D(), VID_ScaledHeight3D());
		}

		renderer.TextureUnitBind(0, normals);

		R_ProgramUniform1f(r_program_uniform_outline_depth_threshold, bound(1, gl_outline_world_depth_threshold.value, 16));
		R_ProgramUniform1f(r_program_uniform_outline_normal_threshold, bound(0, gl_outline_world_normal_threshold.value, 0.999));
		R_ProgramUniform3f(r_program_uniform_outline_color,
                           (float)gl_outline_color_world.color[0] / 255.0f,
                           (float)gl_outline_color_world.color[1] / 255.0f,
                           (float)gl_outline_color_world.color[2] / 255.0f);

		// Scaling the outline with framebuffer resolution for consistency
		// Outline scaling factor bounds:
		// - 1:1 for upscaling a smaller FB (only one pixel needed, as they get bigger and bigger and bloat the view),
		// - 4:1 for downscaling a larger FB (max effective framebuffer scale)
		// MAX is better at high scales and mismatched x/y ratios
		// (maintains more outline at high FB scale, consistent with base res).
		// MIN/MAX makes no difference at tiny scales

		float fb_scale_x = VID_ScaledWidth3D() / glConfig.vidWidth;
		float fb_scale_y = VID_ScaledHeight3D() / glConfig.vidHeight;
		float fb_scaling = bound(1,(max(fb_scale_x, fb_scale_y)),4);
		R_ProgramUniform1f(r_program_uniform_outline_scale, fb_scaling);

		R_ProgramUse(r_program_fx_world_geometry);
		R_ApplyRenderingState(r_state_fx_world_geometry);

		GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		// Restore viewport
		R_Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		if (CL_MultiviewEnabled()) {
			R_DisableScissorTest();
		}
	}
}

static void GLM_DrawModelOutlinesJFA(void)
{
	extern cvar_t gl_outline;
	extern cvar_t gl_outline_method;
	extern cvar_t gl_outline_style;
	extern cvar_t gl_outline_color_model;
	extern cvar_t gl_outline_scale_model;
	texture_ref mask_texture;
	int viewport[4];
	int fullscreen_viewport[4];
	int width = GL_FrameBufferWidth(framebuffer_std);
	int height = GL_FrameBufferHeight(framebuffer_std);
	GLint previous_framebuffer = 0;
	float thickness;
	int src = 0;
	int dst = 1;
	int step;
	int style;
	float style_scale;
	qbool previous_scissor_enabled;
	GLint previous_scissor_box[4];

	// Model outlines only: bit 0 enables model outlines, method 1 selects the JFA path.
	if (!(gl_outline.integer & 1) || gl_outline_method.integer != 1) {
		return;
	}
	if (width <= 0 || height <= 0) {
		width = VID_ScaledWidth3D();
		height = VID_ScaledHeight3D();
	}
	if (!GLM_HasAliasModelOutlineMaskBatch()) {
		return;
	}
	if (!GLM_CompileModelOutlinePrograms()) {
		return;
	}
	if (!GL_FramebufferStartModelMask(framebuffer_std)) {
		return;
	}

	// Pass 1: render outline candidates into fbtex_modelmask (color + encoded depth).
	GLM_DrawAliasModelOutlineMaskBatch();
	GL_FramebufferEndModelMask(framebuffer_std);

	mask_texture = GL_FramebufferTextureReference(framebuffer_std, fbtex_modelmask);
	if (!R_TextureReferenceIsValid(mask_texture)) {
		return;
	}
	if (!GLM_EnsureModelOutlineTargets(width, height)) {
		return;
	}

	R_GetViewport(viewport);
	// Preserve caller scissor state. The post-process runs fullscreen.
	previous_scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, previous_scissor_box);
	R_DisableScissorTest();

	if (CL_MultiviewEnabled()) {
		R_GetFullScreenViewport(fullscreen_viewport);
		R_Viewport(fullscreen_viewport[0], fullscreen_viewport[1], fullscreen_viewport[2], fullscreen_viewport[3]);
		R_EnableScissorTest(viewport[0], viewport[1], viewport[2], viewport[3]);
	}
	else {
		R_Viewport(0, 0, width, height);
	}

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_framebuffer);

	R_ApplyRenderingState(r_state_default_2d);

	// Pass 2: seed map. Encode current pixel coordinate and signed large distance.
	// Positive distance means "outside", negative means "inside".
	glBindFramebuffer(GL_FRAMEBUFFER, model_outline_jfa_fbos[0]);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	R_ProgramUse(r_program_fx_outline_seed);
	renderer.TextureUnitBind(0, mask_texture);
	GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Pass 3: jump flood over a shrinking step size to approximate nearest seed per pixel.
	style = bound(0, gl_outline_style.integer, 2);
	style_scale = style ? 0.5f : 0.25f;		// Hard outlines (style 0) have a smaller max size
	thickness = max(1.0f, bound(0.0f, gl_outline_scale_model.value*style_scale, 0.5f) * 30.0f);
	step = min(max(width, height), (int)ceil(thickness));
	while (true) {
		R_ProgramUse(r_program_fx_outline_jfa);
		R_ProgramUniform1i(r_program_uniform_outline_jfa_step, step);
		renderer.TextureUnitBind(0, model_outline_jfa_targets[src]);

		glBindFramebuffer(GL_FRAMEBUFFER, model_outline_jfa_fbos[dst]);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		src = 1 - src;
		dst = 1 - dst;
		if (step <= 1) {
			break;
		}
		step = (step + 1) / 2;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);

	// Pass 4: composite final effect style into the scene.
	R_ProgramUse(r_program_fx_outline_effect);
	R_ApplyRenderingState(r_state_fx_world_geometry);
	renderer.TextureUnitBind(0, model_outline_jfa_targets[src]); // nearest-seed + signed distance
	renderer.TextureUnitBind(1, mask_texture); // seed color + encoded depth

	R_ProgramUniform1f(r_program_uniform_outline_effect_thickness, thickness);
	R_ProgramUniform1i(r_program_uniform_outline_effect_mode, style);
	R_ProgramUniform1i(r_program_uniform_outline_effect_inside, 1);
	R_ProgramUniform3f(r_program_uniform_outline_effect_color,
		(float)gl_outline_color_model.color[0] / 255.0f,
		(float)gl_outline_color_model.color[1] / 255.0f,
		(float)gl_outline_color_model.color[2] / 255.0f
	);
	GL_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	R_Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (previous_scissor_enabled) {
		R_EnableScissorTest(previous_scissor_box[0], previous_scissor_box[1], previous_scissor_box[2], previous_scissor_box[3]);
	}
	else {
		R_DisableScissorTest();
	}
}

void GLM_RenderView(void)
{
	GLM_UploadFrameConstants();
	R_UploadChangedLightmaps();
	GLM_PrepareWorldModelBatch();
	GLM_PrepareAliasModelBatches();
	renderer.Prepare3DSprites();

	if (GL_FramebufferStartWorldNormals(framebuffer_std)) {
		GLM_DrawWorldModelBatch(opaque_world);
		GL_FramebufferEndWorldNormals(framebuffer_std);
		GLM_DrawWorldOutlines();
	}
	else {
		GLM_DrawWorldModelBatch(opaque_world);
	}

	R_TraceEnterNamedRegion("GLM_DrawEntities");
	GLM_DrawAliasModelBatches();
	R_TraceLeaveNamedRegion();

	renderer.Draw3DSprites();

	GLM_DrawWorldModelBatch(alpha_surfaces);

	GLM_DrawAliasModelPostSceneBatches();
	// JFA outlines run as a fullscreen post-process after all model passes are queued.
	GLM_DrawModelOutlinesJFA();
}

void GLM_PrepareModelRendering(qbool vid_restart)
{
	if (vid_restart) {
		GLM_DeleteModelOutlineTargets();
	}

	GLM_BuildCommonTextureArrays(vid_restart);

	if (cls.state != ca_disconnected) {
		R_CreateInstanceVBO();
		R_CreateAliasModelVBO();
		R_BrushModelCreateVBO();

		GLM_CreateBrushModelVAO();
	}
	renderer.ProgramsInitialise();
}

#endif // #ifdef RENDERER_OPTION_MODERN_OPENGL
