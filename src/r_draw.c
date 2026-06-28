/*
Copyright (C) 1996-1997 Id Software, Inc.

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

$Id: gl_draw.c,v 1.104 2007-10-18 05:28:23 dkure Exp $
*/

#include "quakedef.h"
#include "gl_model.h"
#include "wad.h"
#include "stats_grid.h"
#include "utils.h"
#include "sbar.h"
#include "common_draw.h"
#include "tr_types.h"
#include "gl_framebuffer.h"
#include "r_texture.h"
#include "r_matrix.h"
#include "r_local.h"
#include "r_draw.h"
#include "r_state.h"
#include "r_trace.h"
#include "r_renderer.h"

void CachePics_Init(void);
void Draw_InitCharset(void);
void CachePics_LoadAmmoPics(mpic_t* ibar);
void Draw_SetCrosshairTextMode(qbool enabled);

void GLC_DrawDisc(void);

extern cvar_t crosshair, cl_crossx, cl_crossy, crosshaircolor, crosshairsize;
extern cvar_t con_shift, hud_faderankings;

cvar_t	scr_conalpha		= {"scr_conalpha", "0.8"};
cvar_t	scr_conback			= {"scr_conback", "1"};
void OnChange_scr_conpicture(cvar_t *, char *, qbool *);
cvar_t  scr_conpicture      = {"scr_conpicture", "conback", 0, OnChange_scr_conpicture};
cvar_t	scr_menualpha		= {"scr_menualpha", "0.7"};
cvar_t	scr_menudrawhud		= {"scr_menudrawhud", "0"};

cvar_t  r_smoothtext        = { "r_smoothtext",      "1" };
cvar_t  r_smoothcrosshair   = { "r_smoothcrosshair", "1" };
cvar_t  r_smoothimages      = { "r_smoothimages",    "1" };
cvar_t  r_smoothalphahack   = { "r_smoothalphahack", "0" };

void OnChange_crosshairimage(cvar_t *, char *, qbool *);
static cvar_t crosshairimage          = {"crosshairimage", "", 0, OnChange_crosshairimage};

cvar_t crosshairalpha                 = {"crosshairalpha", "1"};

static cvar_t crosshairscalemethod         = {"crosshairscalemethod", "0"};
static cvar_t crosshairscale               = {"crosshairscale", "0"};
static int    current_crosshair_pixel_size = 0;

// --- Custom (drawn) crosshair ----------------------------------------------
// A crosshair defined entirely by floating-point parameters and rasterized with
// sub-pixel anti-aliasing at the on-screen size, instead of an 8x8 bitmap or a
// PNG. Resolution-independent and crisp at any scale. Gated behind crosshairvec
// so the existing crosshair/crosshairimage/crosshair.txt paths are untouched.
// The four lines are independent, so any subset works (just left, left+right, a T, ...).
// Lengths/radii are fractions of the crosshair's half-extent (so they're scale-free).
static void OnChange_crosshairvec(cvar_t *var, char *value, qbool *cancel);

static cvar_t crosshairvec           = {"crosshairvec", "0"}; // 1 = use the custom (drawn) crosshair
// Shape of the custom crosshair: lengths/radii as fractions of the crosshair half-extent,
// 0 = that element off. Each re-rasterizes the texture on change via OnChange_crosshairvec;
// see help_variables.json (or /describe crosshairvec_*) for what each one does.
static cvar_t crosshairvec_up        = {"crosshairvec_up",        "0.25",   0, OnChange_crosshairvec};
static cvar_t crosshairvec_down      = {"crosshairvec_down",      "0.25",   0, OnChange_crosshairvec};
static cvar_t crosshairvec_left      = {"crosshairvec_left",      "0.25",   0, OnChange_crosshairvec};
static cvar_t crosshairvec_right     = {"crosshairvec_right",     "0.25",   0, OnChange_crosshairvec};
static cvar_t crosshairvec_gap       = {"crosshairvec_gap",       "0.5",    0, OnChange_crosshairvec};
static cvar_t crosshairvec_thickness = {"crosshairvec_thickness", "0.125",  0, OnChange_crosshairvec};
static cvar_t crosshairvec_dot       = {"crosshairvec_dot",       "0",      0, OnChange_crosshairvec};
static cvar_t crosshairvec_ring      = {"crosshairvec_ring",      "0.75",   0, OnChange_crosshairvec};
static cvar_t crosshairvec_innerring = {"crosshairvec_innerring", "0",      0, OnChange_crosshairvec};
static cvar_t crosshairvec_rotate        = {"crosshairvec_rotate",        "0",   0, OnChange_crosshairvec}; // degrees clockwise; rotates the lines (+ -> x)
static cvar_t crosshairvec_segments      = {"crosshairvec_segments",      "0",   0, OnChange_crosshairvec}; // cut both rings into N arc segments (0 = full rings)
static cvar_t crosshairvec_segmentgap    = {"crosshairvec_segmentgap",    "0.4", 0, OnChange_crosshairvec}; // empty fraction of each segment's slot
static cvar_t crosshairvec_segmentrotate = {"crosshairvec_segmentrotate", "0",   0, OnChange_crosshairvec}; // degrees clockwise; rotates the brackets
static cvar_t crosshairvec_outline   = {"crosshairvec_outline",   "0.0625", 0, OnChange_crosshairvec};
static cvar_t crosshairvec_outline_color = {"crosshairvec_outline_color", "0 0 0 255", CVAR_COLOR}; // RGBA, applied at draw (no rebuild)

#define PARAMETRIC_CROSSHAIR_MAXSIZE 1024
static texture_ref crosshair_parametric_fill;            // white shape mask, tinted by crosshaircolor at draw
static texture_ref crosshair_parametric_outline;         // white dilated mask, tinted by crosshairvec_outline_color
static qbool       crosshair_parametric_valid     = false;
static int         crosshair_parametric_last_size = -1;

mpic_t			*draw_disc;
static mpic_t	*draw_backtile;

static mpic_t	conback;

mpic_t      crosshairtexture_txt;
mpic_t      crosshairpic;
mpic_t      crosshairs_builtin[NUMCROSSHAIRS];

static byte customcrosshairdata[64];

#define CROSSHAIR_NONE	0
#define CROSSHAIR_TXT	1
#define CROSSHAIR_IMAGE	2
static int customcrosshair_loaded = CROSSHAIR_NONE;

#define CH_POINT(x,y,size) ((x) + (y) * size)

static void CreateBuiltinCrosshair(byte* data, int size, int format)
{
	int i = 0;
	int middle = (size / 2) - 1;

	memset(data, 0xff, size * size);

	switch (format) {
	case 2:
		// simple cross, alternating stipple effect
		for (i = 0; i < size; i += 2) {
			// vertical
			data[CH_POINT(middle, i, size)] = 0xfe;

			// horizontal
			data[CH_POINT(i, middle, size)] = 0xfe;
		}
		break;
	case 3:
		// small cross in center
		{
			int length = max(0, size / 8);

			data[CH_POINT(middle, middle, size)] = 0xfe;
			for (i = 1; i <= length; ++i) {
				int p1 = CH_POINT(middle - i, middle, size);
				int p2 = CH_POINT(middle + i, middle, size);
				int p3 = CH_POINT(middle, middle - i, size);
				int p4 = CH_POINT(middle, middle + i, size);

				data[p1] = 0xfe;
				data[p2] = 0xfe;
				data[p3] = 0xfe;
				data[p4] = 0xfe;
			}
		}
		break;
	case 4:
		// just a dot (scale to make square then circle?)
		data[CH_POINT(middle, middle, size)] = 0xfe;
		break;
	case 5:
		// diagonals (middle missing)
		for (i = 0; i < size; ++i) {
			if (i >= middle - 1 && i <= middle + 1) {
				continue;
			}
			data[CH_POINT(i, i, size)] = 0xfe;
			data[CH_POINT(size - 1 - i, i, size)] = 0xfe;
		}
		break;
	case 6:
		// Supposed to be a smiley face, not converting that...
	case 7:
		// Square with dot in centre
		data[CH_POINT(middle, middle, size)] = 0xfe;
		for (i = 2; i <= size - 4; ++i) {
			data[CH_POINT(i, 0, size)] = 0xfe;
			data[CH_POINT(0, i, size)] = 0xfe;
			data[CH_POINT(i, size - 2, size)] = 0xfe;
			data[CH_POINT(size - 2, i, size)] = 0xfe;
		}
		break;
	}
}

// Analytic coverage of the 1x1 pixel box at (px,py) by an axis-aligned rect.
// Returns 0..1; partial overlap along an edge yields a fractional value, which
// is exactly what gives the parametric crosshair anti-aliased (non-jaggy) edges.
static float ParametricRectCoverage(float px, float py, float rx0, float ry0, float rx1, float ry1)
{
	float ox = min(px + 1.0f, rx1) - max(px, rx0);
	float oy = min(py + 1.0f, ry1) - max(py, ry0);

	if (ox <= 0.0f || oy <= 0.0f) {
		return 0.0f;
	}
	return (ox > 1.0f ? 1.0f : ox) * (oy > 1.0f ? 1.0f : oy);
}

// The crosshair's one anti-aliasing primitive: turn a signed distance in texels (negative inside the
// shape) into 0..1 coverage with a 1px ramp centred on the edge. Every primitive below -- rings, the
// rotated lines, the dot and the arc-segment caps -- runs its edge through this, so the edge softness
// is identical everywhere and lives in exactly one place.
static float ParametricEdge(float sd)
{
	return bound(0.0f, 0.5f - sd, 1.0f);
}

// Coverage of a stroked circle (annulus) for a pixel whose centre is `dist` texels
// from the crosshair centre. `radius` is the circle radius, `halfwidth` half the stroke width.
static float ParametricRingCoverage(float dist, float radius, float halfwidth)
{
	if (radius <= 0.0f) {
		return 0.0f;
	}
	return ParametricEdge(fabsf(dist - radius) - halfwidth); // sd < 0 inside the stroke band
}

// Geometry of the custom crosshair, in texels (filled once, sampled per pixel).
typedef struct {
	float cx, cy;                 // centre
	float hw;                     // stroke half-thickness
	float inner;                  // gap radius (where the lines start)
	float up, down, left, right;  // per-line tip radius (== inner means that line is off)
	float ring1, ring2;           // ring radii (0 = off)
	float dotr;                   // centre dot radius (0 = off)
	float ct, st;                 // cos/sin of the line rotation (1, 0 when unrotated)
	qbool line_sdf;               // lines are rotated -> use the SDF box path instead of analytic rects
	int   segs;                   // arc segments per ring (0 = full rings)
	float seg_period;             // angular period of one segment slot, 2*pi/segs (radians)
	float seg_half;               // lit half-span of each segment (radians)
	float seg_phase;              // bracket rotation (radians), applied as a phase offset on the angle
} parametric_geom_t;

// Coverage of an axis-aligned box, evaluated in the crosshair's (already rotated) local frame, with
// the same 1px ramp as the rings. Used for the lines when crosshairvec_rotate is in effect: a rigid
// rotation preserves distance, so the box signed distance at the rotated sample point is the exact
// world signed distance and the anti-aliasing stays one pixel wide at any angle (the analytic
// ParametricRectCoverage above is only correct for axis-aligned lines). (bx,by) is the box centre and
// (hx,hy) its half-extents, both relative to the crosshair centre.
static float ParametricBoxCoverage(float lx, float ly, float bx, float by, float hx, float hy)
{
	float qx = fabsf(lx - bx) - hx;
	float qy = fabsf(ly - by) - hy;
	float ox = max(qx, 0.0f);
	float oy = max(qy, 0.0f);
	float sd = sqrtf(ox * ox + oy * oy) + min(max(qx, qy), 0.0f); // < 0 inside the box
	return ParametricEdge(sd);
}

// Coverage of one crosshair line. The line is described once, as the axis-aligned rectangle
// (rx0,ry0)-(rx1,ry1): an unrotated crosshair samples it with the exact analytic box-area coverage
// (byte-identical to the original), and a rotated one re-derives the same rectangle as an SDF box in
// the rotated local frame (lx,ly). Describing the rectangle in a single place keeps the rotated and
// unrotated line geometry from drifting apart.
static float ParametricLineCoverage(const parametric_geom_t* gm, float fx, float fy, float lx, float ly,
                                    float rx0, float ry0, float rx1, float ry1)
{
	if (!gm->line_sdf) {
		return ParametricRectCoverage(fx, fy, rx0, ry0, rx1, ry1);
	}
	return ParametricBoxCoverage(lx, ly,
		0.5f * (rx0 + rx1) - gm->cx, 0.5f * (ry0 + ry1) - gm->cy,  // box centre, relative to the crosshair centre
		0.5f * (rx1 - rx0), 0.5f * (ry1 - ry0));                   // half-extents
}

// Angular mask that turns a full ring into `gm->segs` evenly spaced arc segments (brackets). The
// anti-aliasing is done in arc-length (texel) space so the segment ends stay one pixel wide at any
// radius; the lit span is widened by `grow` texels per side so the outline pass caps the bracket ends.
// Returns 1 (a solid ring) when segments are off, the ring is sub-pixel, or a segment is too short to
// resolve two clean edges -- so a full ring stays byte-identical to before. `phi` is the pixel angle
// atan2f(dy,dx); the bracket rotation is applied here as a phase offset, independent of the line rotation.
static float ParametricArcMask(const parametric_geom_t* gm, float phi, float radius, float grow)
{
	float a, seghalf;

	if (gm->segs <= 0 || radius < 0.5f) {
		return 1.0f;
	}
	if (2.0f * gm->seg_half * radius < 2.0f) {                          // too small to resolve two clean edges -> solid ring
		return 1.0f;                                                    // test the un-grown span so fill and outline agree
	}
	seghalf = gm->seg_half + grow / radius;                            // outline pass extends the caps by `grow` texels
	phi    -= gm->seg_phase;                                            // bracket rotation = phase offset on the angle
	a       = phi - gm->seg_period * floorf(phi / gm->seg_period + 0.5f); // signed angular distance to the nearest segment centre
	return ParametricEdge((fabsf(a) - seghalf) * radius);              // 1px ramp; sd = arc-length distance outside the lit span
}

// Union coverage of every primitive at one pixel, each grown outward by `grow` texels.
// grow=0 yields the shape; grow=outline yields the dilated shape used for the outline.
static float ParametricCoverage(const parametric_geom_t* gm, float fx, float fy, float dx, float dy, float dist, float grow)
{
	float h   = gm->hw + grow;
	float in  = gm->inner - grow;
	float cov = 0.0f;

	float lx  = 0.0f, ly = 0.0f;
	float phi = 0.0f;

	if (gm->line_sdf) {           // sample point in the line-rotated local frame (only when rotation is on)
		lx =  dx * gm->ct + dy * gm->st;
		ly = -dx * gm->st + dy * gm->ct;
	}
	// Each line is drawn only if it has length. ParametricLineCoverage picks the exact analytic rect
	// (unrotated, byte-identical to before) or the SDF box (rotated) from the one rectangle given here.
	if (gm->up > gm->inner) {
		cov = max(cov, ParametricLineCoverage(gm, fx, fy, lx, ly, gm->cx - h, gm->cy - (gm->up + grow), gm->cx + h, gm->cy - in));
	}
	if (gm->down > gm->inner) {
		cov = max(cov, ParametricLineCoverage(gm, fx, fy, lx, ly, gm->cx - h, gm->cy + in, gm->cx + h, gm->cy + (gm->down + grow)));
	}
	if (gm->left > gm->inner) {
		cov = max(cov, ParametricLineCoverage(gm, fx, fy, lx, ly, gm->cx - (gm->left + grow), gm->cy - h, gm->cx - in, gm->cy + h));
	}
	if (gm->right > gm->inner) {
		cov = max(cov, ParametricLineCoverage(gm, fx, fy, lx, ly, gm->cx + in, gm->cy - h, gm->cx + (gm->right + grow), gm->cy + h));
	}

	// Rings, optionally cut into arc segments. ParametricArcMask returns exactly 1 when segments are off,
	// so these two lines are the full-ring case too (x * 1 is exact, so the default stays byte-identical).
	if (gm->segs > 0) {
		phi = atan2f(dy, dx);     // pixel angle, computed once and shared by both rings (same dx,dy)
	}
	cov = max(cov, ParametricRingCoverage(dist, gm->ring1, h) * ParametricArcMask(gm, phi, gm->ring1, grow));
	cov = max(cov, ParametricRingCoverage(dist, gm->ring2, h) * ParametricArcMask(gm, phi, gm->ring2, grow));

	if (gm->dotr > 0.0f) {
		cov = max(cov, ParametricEdge(dist - (gm->dotr + grow)));  // centre dot
	}
	return cov;
}

// Rasterize the parametric crosshair into two white premultiplied coverage masks (the shape,
// and the dilated shape for the outline) and upload them. The colours are applied at draw time
// (a free GPU modulate), so changing a colour never rebuilds the texture.
static void BuildParametricCrosshair(int size)
{
	const float R    = size * 0.5f;   // half-extent, in texels
	byte* fillbuf = (byte*)Q_malloc(size * size * 4);
	byte* outbuf  = NULL;                       // built only when the outline is enabled
	parametric_geom_t gm;
	float ow;
	int x, y;

	// all geometry is floating point -> the shape is exact, not quantised to a grid
	float t  = bound(0.01f, crosshairvec_thickness.value, 1.0f);
	float g  = bound(0.0f,  crosshairvec_gap.value,       0.95f);
	float lu = bound(0.0f,  crosshairvec_up.value,        1.0f);
	float ld = bound(0.0f,  crosshairvec_down.value,      1.0f);
	float ll = bound(0.0f,  crosshairvec_left.value,      1.0f);
	float lr = bound(0.0f,  crosshairvec_right.value,     1.0f);
	float d  = bound(0.0f,  crosshairvec_dot.value,       1.0f);
	float r1 = bound(0.0f,  crosshairvec_ring.value,      1.0f);
	float r2 = bound(0.0f,  crosshairvec_innerring.value, 1.0f);

	gm.cx    = gm.cy = R;                          // centre
	gm.hw    = 0.5f * t * R;                       // stroke half-thickness (texels)
	gm.inner = g * R;                              // gap radius
	gm.up    = min(gm.inner + lu * R, R - 1.0f);   // per-line tip radius (== inner => line off)
	gm.down  = min(gm.inner + ld * R, R - 1.0f);
	gm.left  = min(gm.inner + ll * R, R - 1.0f);
	gm.right = min(gm.inner + lr * R, R - 1.0f);
	gm.dotr  = 0.5f * d * R;                        // dot radius (0 when dot off)
	gm.ring1 = min(r1 * R, R - gm.hw - 1.0f);       // outer ring radius (clamped so stroke fits)
	gm.ring2 = min(r2 * R, R - gm.hw - 1.0f);       // inner ring radius
	ow       = bound(0.0f, crosshairvec_outline.value, 0.5f) * R; // outline width (texels)

	{
		// Rotation and ring segmentation. Both are baked into the texture (shape params, rebuilt on
		// change), so they cost nothing per frame. theta rotates the lines; the bracket rotation is a
		// separate phase. At the defaults (rotate 0, segments 0) line_sdf is false and segs is 0, so the
		// analytic line path and full-ring path run unchanged and the mask is byte-identical to before.
		float theta = DEG2RAD(fmodf(crosshairvec_rotate.value, 360.0f));
		int   nseg  = (int)bound(0.0f, crosshairvec_segments.value, 64.0f);
		float sgap  = bound(0.0f, crosshairvec_segmentgap.value, 0.95f);

		gm.ct         = cosf(theta);
		gm.st         = sinf(theta);
		// Any rotation switches the lines to the SDF path; rotate 0 keeps the exact analytic coverage, so
		// the default is unchanged and only a rotated crosshair pays the sub-pixel SDF-vs-analytic
		// difference at the line ends (the rings and dot are circular, so they are unaffected either way).
		gm.line_sdf   = (theta != 0.0f);                                            // y-down: +theta is clockwise
		gm.segs       = nseg;
		gm.seg_period = nseg ? (2.0f * (float)M_PI / (float)nseg) : 0.0f;
		gm.seg_half   = nseg ? (0.5f * gm.seg_period * (1.0f - sgap)) : 0.0f;
		gm.seg_phase  = DEG2RAD(fmodf(crosshairvec_segmentrotate.value, 360.0f));   // rotates the brackets, independent of the lines
	}

	if (ow > 0.0f) {                              // no outline mask when there's no outline to draw
		outbuf = (byte*)Q_malloc(size * size * 4);
	}

	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			float fx = (float)x, fy = (float)y;
			float dx = (fx + 0.5f) - gm.cx, dy = (fy + 0.5f) - gm.cy;
			float dist = sqrtf(dx * dx + dy * dy);
			float fill = ParametricCoverage(&gm, fx, fy, dx, dy, dist, 0.0f);
			int   o    = (y * size + x) * 4;

			// white premultiplied fill mask (rgb == alpha == coverage); tinted by crosshaircolor at draw.
			fillbuf[o + 0] = fillbuf[o + 1] = fillbuf[o + 2] = fillbuf[o + 3] = (byte)(bound(0.0f, fill, 1.0f) * 255.0f + 0.5f);

			if (outbuf) {
				// outline = the dilated shape minus the fill (the rim only), so it never shows
				// through a translucent fill. Tinted by crosshairvec_outline_color at draw.
				float line   = ParametricCoverage(&gm, fx, fy, dx, dy, dist, ow);
				float border = line * (1.0f - fill);
				outbuf[o + 0] = outbuf[o + 1] = outbuf[o + 2] = outbuf[o + 3] = (byte)(bound(0.0f, border, 1.0f) * 255.0f + 0.5f);
			}
		}
	}

	// TEX_ALPHA keeps the alpha channel and TEX_LUMA skips gamma (it would touch rgb but not
	// alpha and break the premultiplied invariant). ezquake's 2D blend is premultiplied.
	crosshair_parametric_fill = R_LoadTexturePixels(fillbuf, "cross:vec_fill", size, size, TEX_ALPHA | TEX_NOSCALE | TEX_LUMA);
	renderer.TextureWrapModeClamp(crosshair_parametric_fill);
	if (outbuf) {
		crosshair_parametric_outline = R_LoadTexturePixels(outbuf, "cross:vec_outline", size, size, TEX_ALPHA | TEX_NOSCALE | TEX_LUMA);
		renderer.TextureWrapModeClamp(crosshair_parametric_outline);
		Q_free(outbuf);
	}

	Q_free(fillbuf);
}

// A shape cvar changed -> drop the cached texture; the next draw rebuilds it. Same
// OnChange-invalidation idiom as OnChange_crosshairimage above.
static void OnChange_crosshairvec(cvar_t *var, char *value, qbool *cancel)
{
	crosshair_parametric_valid = false;
}

// Rebuild the masks only when invalidated (a shape cvar changed, or a renderer reload) or when
// the on-screen size changed. Colours are applied at draw, so they never trigger a rebuild.
static void EnsureParametricCrosshair(int size)
{
	if (crosshair_parametric_valid && crosshair_parametric_last_size == size) {
		return;
	}

	BuildParametricCrosshair(size);

	crosshair_parametric_last_size = size;
	crosshair_parametric_valid = true;
}

/*
* Draw_CopyMPICKeepSize
* Copy data from src to dst but keep unchanged dst->width and dst->height
*/
static void Draw_CopyMPICKeepSize(mpic_t *dst, mpic_t *src)
{
	byte width[sizeof(dst->width)];
	byte height[sizeof(dst->height)];

	// remember particular fields
	memcpy(width, (byte*)&dst->width, sizeof(width));
	memcpy(height, (byte*)&dst->height, sizeof(height));

	// bit by bit copy
	*dst = *src;

	// restore fields
	memcpy((byte*)&dst->width, width, sizeof(width));
	memcpy((byte*)&dst->height, height, sizeof(height));
}

void OnChange_scr_conpicture(cvar_t *v, char *s, qbool *cancel)
{
	mpic_t *pic_24bit;

	if (!s[0])
		return;

	if (!(pic_24bit = R_LoadPicImage(va("gfx/%s", s), "conback", 0, 0, 0)))
	{
		Com_Printf("Couldn't load image gfx/%s\n", s);
		return;
	}

	Draw_CopyMPICKeepSize(&conback, pic_24bit);
	Draw_AdjustConback();
}

void OnChange_crosshairimage(cvar_t *v, char *s, qbool *cancel)
{
	mpic_t *pic;

	customcrosshair_loaded &= ~CROSSHAIR_IMAGE;

	if (!s[0])
		return;

	if (!(pic = Draw_CachePicSafe(va("crosshairs/%s", s), false, true)))
	{
		Com_Printf("Couldn't load image %s\n", s);
		return;
	}

	crosshairpic = *pic;
	customcrosshair_loaded |= CROSSHAIR_IMAGE;
	CachePics_MarkAtlasDirty();
}

void customCrosshair_Init(void)
{
	char ch;
	vfsfile_t *f;
	vfserrno_t err;
	int i = 0, c;

	customcrosshair_loaded = CROSSHAIR_NONE;
	R_TextureReferenceInvalidate(crosshairtexture_txt.texnum);

	if (!(f = FS_OpenVFS("crosshairs/crosshair.txt", "rb", FS_ANY))) {
		return;
	}

	while (i < 64) {
		VFS_READ(f, &ch, sizeof(char), &err);
		if (err == VFSERR_EOF) {
			Com_Printf("Invalid format in crosshair.txt (Need 64 X's and O's)\n");
			VFS_CLOSE(f);
			return;
		}
		c = ch;

		if (isspace(c))
			continue;

		if (tolower(c) != 'x' && tolower(c) != 'o') {
			Com_Printf("Invalid format in crosshair.txt (Only X's and O's and whitespace permitted)\n");
			VFS_CLOSE(f);
			return;
		}
		customcrosshairdata[i++] = (c == 'x' || c == 'X') ? 0xfe : 0xff;
	}

	VFS_CLOSE(f);
	crosshairtexture_txt.texnum = R_LoadTexture("cross:custom", 8, 8, customcrosshairdata, TEX_ALPHA, 1);
	crosshairtexture_txt.sl = crosshairtexture_txt.tl = 0;
	crosshairtexture_txt.sh = crosshairtexture_txt.th = 1;
	customcrosshair_loaded |= CROSSHAIR_TXT;
	CachePics_MarkAtlasDirty();
}

static int CrosshairPixelSize(void)
{
	// 0 = 8, 1 = 16 etc
	return pow(2, 3 + (int)bound(0, crosshairscale.integer, 5));
}

static void BuildBuiltinCrosshairs(void)
{
	int i;
	char str[256] = {0};
	int crosshair_size = CrosshairPixelSize();
	byte* crosshair_buffer = (byte*)Q_malloc(crosshair_size * crosshair_size);

	if (!(customcrosshair_loaded & CROSSHAIR_IMAGE)) {
		memset(&crosshairpic, 0, sizeof(crosshairpic));
	}
	for (i = 0; i < NUMCROSSHAIRS; i++) {
		snprintf(str, sizeof(str), "cross:hardcoded%d", i);
		CreateBuiltinCrosshair(crosshair_buffer, crosshair_size, i + 2);
		crosshairs_builtin[i].texnum = R_LoadTexture (str, crosshair_size, crosshair_size, crosshair_buffer, TEX_ALPHA, 1);
		crosshairs_builtin[i].sl = crosshairs_builtin[i].tl = 0;
		crosshairs_builtin[i].sh = crosshairs_builtin[i].th = 1;
		crosshairs_builtin[i].height = crosshairs_builtin[i].width = 16;

		renderer.TextureWrapModeClamp(crosshairs_builtin[i].texnum);
	}
	Q_free(crosshair_buffer);
	current_crosshair_pixel_size = crosshair_size;
	CachePics_MarkAtlasDirty();
}

void Draw_InitCrosshairs(void)
{
	char str[256] = {0};

	crosshair_parametric_valid = false; // texture handle is stale after a renderer reload; force rebuild
	BuildBuiltinCrosshairs();
	customCrosshair_Init(); // safe re-init

	snprintf(str, sizeof(str), "%s", crosshairimage.string);
	Cvar_Set(&crosshairimage, str);
}

float overall_alpha = 1.0;

void Draw_SetOverallAlpha(float alpha)
{
	clamp(alpha, 0.0, 1.0);
	overall_alpha = alpha;
}

float Draw_MultiplyOverallAlpha(float alpha)
{
	float old = overall_alpha;

	overall_alpha *= alpha;

	return old;
}

void Draw_EnableScissorRectangle(float x, float y, float width, float height)
{
	float resdif_w = (glwidth / (float)vid.conwidth);
	float resdif_h = (glheight / (float)vid.conheight);

	R_EnableScissorTest(
		Q_rint(x * resdif_w),
		Q_rint((vid.conheight - (y + height)) * resdif_h),
		Q_rint(width * resdif_w),
		Q_rint(height * resdif_h)
	);
}

void Draw_EnableScissor(float left, float right, float top, float bottom)
{
	Draw_EnableScissorRectangle(left, top, (right - left), (bottom - top));
}

void Draw_DisableScissor(void)
{
	R_DisableScissorTest();
}

//=============================================================================
// Support Routines
wadpic_t wad_pictures[WADPIC_PIC_COUNT];

mpic_t *Draw_CacheWadPic(char *name, int code)
{
	qpic_t *p;
	mpic_t *pic, *pic_24bit;
	wadpic_t* wadpic = NULL;

	if (code >= 0 && code < WADPIC_PIC_COUNT) {
		wadpic = &wad_pictures[code];
	}

	p = W_GetLumpName (name);
	pic = (mpic_t *)p;
	if (wadpic) {
		strlcpy(wadpic->name, name, sizeof(wadpic->name));
		wadpic->pic = pic;
	}

	if ((pic_24bit = R_LoadPicImage(va("textures/wad/%s", name), name, 0, 0, TEX_ALPHA)) ||
		(pic_24bit = R_LoadPicImage(va("gfx/%s", name), name, 0, 0, TEX_ALPHA)))
	{
		// Only keep the size info from the lump. The other stuff is copied from the 24 bit image.
		pic->sh		= pic_24bit->sh;
		pic->sl		= pic_24bit->sl;
		pic->texnum = pic_24bit->texnum;
		pic->th		= pic_24bit->th;
		pic->tl		= pic_24bit->tl;

		if (code == WADPIC_SB_IBAR) {
			CachePics_LoadAmmoPics(pic);
		}

		return pic;
	}

	R_LoadPicTexture(name, pic, p->data);

	if (code == WADPIC_SB_IBAR) {
		CachePics_LoadAmmoPics(pic);
	}

	return pic;
}

//
// Loads an image into the cache.
// Variables:
//		crash - Crash the client if loading fails.
//		only24bit - Don't fall back to loading the normal 8-bit texture if
//					loading the 24-bit version fails.
//
mpic_t *Draw_CachePicSafe(const char *path, qbool crash, qbool only24bit)
{
	char stripped_path[MAX_PATH];
	char lmp_path[MAX_PATH];
	mpic_t *fpic;
	mpic_t *pic_24bit;
	qbool lmp_found = false;
	qpic_t *dat = NULL;
	vfsfile_t *v = NULL;

	// Check if the picture was already cached, if so inc refcount.
	if ((fpic = CachePic_Find(path, true))) {
		return fpic;
	}

	// Get the filename without extension.
	COM_StripExtension(path, stripped_path, sizeof(stripped_path));
	snprintf(lmp_path, MAX_PATH, "%s.lmp", stripped_path);

	// Try loading the pic from disk.

	// Only load the 24-bit version of the picture.
	if (only24bit) {
		if (!(pic_24bit = R_LoadPicImage(path, NULL, 0, 0, TEX_ALPHA))) {
			if(crash) {
				Sys_Error ("Draw_CachePicSafe: failed to load %s", path);
			}
			return NULL;
		}

		/* This will make a copy of the pic struct */
		return CachePic_Add(path, pic_24bit);
	}

	// Load the ".lmp" file.
	if ((v = FS_OpenVFS(lmp_path, "rb", FS_ANY))) {
		VFS_CLOSE(v);

		if (!(dat = (qpic_t *)FS_LoadTempFile(lmp_path, NULL))) {
			if(crash) {
				Sys_Error ("Draw_CachePicSafe: failed to load %s", lmp_path);
			}
			return NULL;
		}
		lmp_found = true;

		// Make sure the width and height are correct.
		SwapPic (dat);
	}

	// Try loading the 24-bit picture.
	// If that fails load the data for the lmp instead.
	if ((pic_24bit = R_LoadPicImage(path, NULL, 0, 0, TEX_ALPHA))) {
		// Only use the lmp-data if there was one.
		if (lmp_found) {
			pic_24bit->width = dat->width;
			pic_24bit->height = dat->height;
		}
		return CachePic_Add(path, pic_24bit);
	}
	else if (dat) {
		mpic_t tmp = {0};
		tmp.width = dat->width;
		tmp.height = dat->height;
		R_LoadPicTexture(path, &tmp, dat->data);
		return CachePic_Add(path, &tmp);
	}
	else {
		if(crash) {
			Sys_Error ("Draw_CachePicSafe: failed to load %s", path);
		}
		return NULL;
	}
}

static const char* cache_pic_paths[] = {
	"gfx/pause.lmp",
	"gfx/loading.lmp",
	"gfx/box_tl.lmp",
	"gfx/box_ml.lmp",
	"gfx/box_bl.lmp",
	"gfx/box_tm.lmp",
	"gfx/box_mm.lmp",
	"gfx/box_mm2.lmp",
	"gfx/box_bm.lmp",
	"gfx/box_tr.lmp",
	"gfx/box_mr.lmp",
	"gfx/box_br.lmp",
	"gfx/ttl_main.lmp",
	"gfx/mainmenu.lmp",
	"gfx/menudot1.lmp",
	"gfx/menudot2.lmp",
	"gfx/menudot3.lmp",
	"gfx/menudot4.lmp",
	"gfx/menudot5.lmp",
	"gfx/menudot6.lmp",
	"gfx/ttl_sgl.lmp",
	"gfx/qplaque.lmp",
	"gfx/sp_menu.lmp",
	"gfx/p_load.lmp",
	"gfx/p_save.lmp",
	"gfx/p_multi.lmp",
	"gfx/ranking.lmp",
	"gfx/complete.lmp",
	"gfx/inter.lmp",
	"gfx/finale.lmp"
};

qbool Draw_KeepOffAtlas(const char* path)
{
	int i;
	qbool result = false;

	// Tiled backgrounds: atlas not suitable for tiling, so keep off atlas
	for (i = CACHEPIC_BOX_TL; i <= CACHEPIC_BOX_BR && !result; ++i) {
		result |= !strcmp(path, cache_pic_paths[i]);
	}

	// Single-player & main menu items - take up too much space for no high-performance path
	for (i = CACHEPIC_TTL_MAIN; i <= CACHEPIC_P_MULTI && !result; ++i) {
		result |= !strcmp(path, cache_pic_paths[i]);
	}

	// Single-player intermission titles
	for (i = CACHEPIC_COMPLETE; i <= CACHEPIC_FINALE && !result; ++i) {
		result |= !strcmp(path, cache_pic_paths[i]);
	}

	R_TraceAPI("Draw_KeepOffAtlas(%s) = %s\n", path, result ? "true" : "false");
	return result;
}

mpic_t *Draw_CachePic(cache_pic_id_t id)
{
	if (id < 0 || id >= CACHEPIC_NUM_OF_PICS) {
		Sys_Error("Draw_CachePic(%d) - out of range", id);
	}

	return Draw_CachePicSafe(cache_pic_paths[id], true, false);
}

static void Draw_Precache(void)
{
	int i;
	for (i = 0; i < CACHEPIC_NUM_OF_PICS; ++i) {
		Draw_CachePic(i);
	}
}

void Draw_InitConback (void);

void Draw_Shutdown(void)
{
	W_FreeWadFile();
}

void Draw_Init (void)
{
	extern void HUD_Common_Reset_Group_Pics(void);
	extern void Draw_Charset_Init(void);

	Draw_Charset_Init();

	if (!host_initialized) {
		Cvar_SetCurrentGroup(CVAR_GROUP_CONSOLE);
		Cvar_Register(&scr_conalpha);
		Cvar_Register(&scr_conback);
		Cvar_Register(&scr_conpicture);
		Cvar_Register(&r_smoothtext);

		Cvar_SetCurrentGroup(CVAR_GROUP_SCREEN);
		Cvar_Register(&scr_menualpha);
		Cvar_Register(&scr_menudrawhud);
		Cvar_Register(&r_smoothimages);
		Cvar_Register(&r_smoothalphahack);

		Cvar_SetCurrentGroup(CVAR_GROUP_CROSSHAIR);
		Cvar_Register(&crosshairimage);
		Cvar_Register(&crosshairalpha);
		Cvar_Register(&crosshairscale);
		Cvar_Register(&crosshairscalemethod);
		Cvar_Register(&r_smoothcrosshair);

		Cvar_Register(&crosshairvec);
		Cvar_Register(&crosshairvec_up);
		Cvar_Register(&crosshairvec_down);
		Cvar_Register(&crosshairvec_left);
		Cvar_Register(&crosshairvec_right);
		Cvar_Register(&crosshairvec_gap);
		Cvar_Register(&crosshairvec_thickness);
		Cvar_Register(&crosshairvec_dot);
		Cvar_Register(&crosshairvec_ring);
		Cvar_Register(&crosshairvec_innerring);
		Cvar_Register(&crosshairvec_rotate);
		Cvar_Register(&crosshairvec_segments);
		Cvar_Register(&crosshairvec_segmentgap);
		Cvar_Register(&crosshairvec_segmentrotate);
		Cvar_Register(&crosshairvec_outline);
		Cvar_Register(&crosshairvec_outline_color);

		Cvar_ResetCurrentGroup();
	}

	draw_disc = draw_backtile = NULL;

	W_LoadWadFile("gfx.wad"); // Safe re-init.
	CachePics_Shutdown();
	HUD_Common_Reset_Group_Pics();

	R_Texture_Init();  // Probably safe to re-init now.

	// Clear the scrap, should be called ASAP after textures initialization
	CachePics_Init();

	// Load the console background and the charset by hand, because we need to write the version
	// string into the background before turning it into a texture.
	Draw_InitCharset(); // Safe re-init.
	Draw_InitConback(); // Safe re-init.

	// Load the crosshair pics
	Draw_InitCrosshairs();

	// Get the other pics we need.
	draw_disc     = Draw_CacheWadPic("disc", WADPIC_DISC);
	draw_backtile = Draw_CacheWadPic("backtile", WADPIC_BACKTILE);

	Draw_Precache();
}

qbool CL_MultiviewGetCrosshairCoordinates(qbool use_screen_coords, float* cross_x, float* cross_y, qbool* half_size);

void Draw_Crosshair (void)
{
	float x = 0.0, y = 0.0, ofs1, ofs2, sh, th, sl, tl;
	byte col[4];
	extern vrect_t scr_vrect;
	float crosshair_scale = (crosshairscalemethod.integer ? 1 : ((float)glwidth / 320));
	int crosshair_pixel_size = CrosshairPixelSize();
	qbool half_size = false;

	if (current_crosshair_pixel_size != crosshair_pixel_size) {
		BuildBuiltinCrosshairs();
	}

	if (crosshairvec.integer ||
		(crosshair.value >= 2 && crosshair.value <= NUMCROSSHAIRS + 1) ||
		((customcrosshair_loaded & CROSSHAIR_TXT) && crosshair.value == 1) ||
		(customcrosshair_loaded & CROSSHAIR_IMAGE)) {
		texture_ref texnum;
		int width2d = VID_RenderWidth2D();
		int height2d = VID_RenderHeight2D();

		if (!crosshairalpha.value) {
			return;
		}

		if (!CL_MultiviewGetCrosshairCoordinates(true, &x, &y, &half_size)) {
			return;
		}

		R_OrthographicProjection(0, width2d, height2d, 0, -99999, 99999);

		x += (crosshairscalemethod.integer ? 1 : (float)width2d / vid.width) * cl_crossx.value;
		y += (crosshairscalemethod.integer ? 1 : (float)height2d / vid.height) * cl_crossy.value;

		col[0] = crosshaircolor.color[0];
		col[1] = crosshaircolor.color[1];
		col[2] = crosshaircolor.color[2];
		col[3] = bound(0, crosshairalpha.value, 1) * 255;

		if (crosshairvec.integer) {
			// Rasterize at the on-screen pixel size (~1:1) so the anti-aliasing stays crisp at
			// any scale instead of bilinear-minifying a fixed-size texture. One size per frame:
			// a multiview inset draws this same texture scaled (via the half_size block below).
			float r = (crosshair_pixel_size * 0.5f) * crosshair_scale * bound(0, crosshairsize.value, 20);
			int target = (int)(2.0f * r * (float)glwidth / width2d + 0.5f);

			EnsureParametricCrosshair(bound(32, target, PARAMETRIC_CROSSHAIR_MAXSIZE));
			texnum = crosshair_parametric_fill;  // tinted by crosshaircolor via the shared col[] (like every other type)
			ofs1 = ofs2 = r;       // symmetric: the parametric texture is centred exactly
			sl = tl = 0.0f;
			sh = th = 1.0f;
		}
		else if (customcrosshair_loaded & CROSSHAIR_IMAGE) {
			texnum = crosshairpic.texnum;
			ofs1 = (crosshairpic.width * 0.5f - 0.5f) * bound(0, crosshairsize.value, 20);
			ofs2 = (crosshairpic.height * 0.5f + 0.5f) * bound(0, crosshairsize.value, 20);

			sh = crosshairpic.sh;
			sl = crosshairpic.sl;
			th = crosshairpic.th;
			tl = crosshairpic.tl;
		}
		else {
			mpic_t* pic = ((crosshair.value >= 2) ? &crosshairs_builtin[(int)crosshair.value - 2] : &crosshairtexture_txt);

			texnum = pic->texnum;
			ofs1 = (crosshair_pixel_size * 0.5f - 0.5f) * crosshair_scale * bound(0, crosshairsize.value, 20);
			ofs2 = (crosshair_pixel_size * 0.5f + 0.5f) * crosshair_scale * bound(0, crosshairsize.value, 20);

			sh = pic->sh;
			sl = pic->sl;
			th = pic->th;
			tl = pic->tl;
		}

		if (half_size) {
			ofs1 *= 0.5f;
			ofs2 *= 0.5f;
		}

		// crosshairvec draws its outline as a separate pass behind the fill, so each keeps its
		// own colour as a free draw-time modulate (no texture rebuild when a colour changes).
		if (crosshairvec.integer && crosshairvec_outline.value > 0) {
			byte ocol[4];
			ocol[0] = crosshairvec_outline_color.color[0];
			ocol[1] = crosshairvec_outline_color.color[1];
			ocol[2] = crosshairvec_outline_color.color[2];
			ocol[3] = crosshairvec_outline_color.color[3] * bound(0, crosshairalpha.value, 1);
			R_DrawImage(x - ofs1, y - ofs1, ofs1 + ofs2, ofs1 + ofs2, 0, 0, 1, 1, ocol, false, crosshair_parametric_outline, false, true);
		}

		R_DrawImage(x - ofs1, y - ofs1, ofs1 + ofs2, ofs1 + ofs2, sl, tl, sh - sl, th - tl, col, false, texnum, false, true);

		R_OrthographicProjection(0, vid.width, vid.height, 0, -99999, 99999);
	}
	else if (crosshair.value) {
		// Multiview
		Draw_SetCrosshairTextMode(true);
		if (CL_MultiviewInsetEnabled()) {
			if (CL_MultiviewInsetView()) {
				int width2d = VID_RenderWidth2D();
				int height2d = VID_RenderHeight2D();

				if (!CL_MultiviewGetCrosshairCoordinates(true, &x, &y, &half_size)) {
					return;
				}

				// convert from 3d to 2d
				x = (x * vid.width) / width2d;
				y = (y * vid.height) / height2d;

				// x = vid.width - (vid.width / 3) / 2 - 4
				// y = (vid.height / 3) / 2 - 2,
				Draw_Character(x - 4, y - 4, '+');
			}
			else {
				Draw_Character(scr_vrect.x + scr_vrect.width / 2 - 4 + cl_crossx.value, scr_vrect.y + scr_vrect.height / 2 - 4 + cl_crossy.value, '+');
			}
		}
		else if (CL_MultiviewActiveViews() == 2) {
			Draw_Character(vid.width / 2 - 4, vid.height * 3 / 4 - 2, '+');
			Draw_Character(vid.width / 2 - 4, vid.height / 4 - 2, '+');
		}
		else if (CL_MultiviewActiveViews() == 3) {
			Draw_Character(vid.width / 2 - 4, vid.height / 4 - 2, '+');
			Draw_Character(vid.width / 4 - 4, vid.height / 2 + vid.height / 4 - 2, '+');
			Draw_Character(vid.width / 2 + vid.width / 4 - 4, vid.height / 2 + vid.height / 4 - 2, '+');
		}
		else if (CL_MultiviewActiveViews() == 4) {
			Draw_Character(vid.width / 4 - 4, vid.height / 4 - 2, '+');
			Draw_Character(vid.width / 2 + vid.width / 4 - 4, vid.height / 4 - 2, '+');
			Draw_Character(vid.width / 4 - 4, vid.height / 2 + vid.height / 4 - 2, '+');
			Draw_Character(vid.width / 2 + vid.width / 4 - 4, vid.height / 2 + vid.height / 4 - 2, '+');
		}
		else {
			Draw_Character(scr_vrect.x + scr_vrect.width / 2 - 4 + cl_crossx.value, scr_vrect.y + scr_vrect.height / 2 - 4 + cl_crossy.value, '+');
		}
		Draw_SetCrosshairTextMode(false);
	}
}

void Draw_TextBox(float x, float y, int width, int lines)
{
	mpic_t *p;
	int cx, cy, n;

	// draw left side
	cx = x;
	cy = y;
	p = Draw_CachePic (CACHEPIC_BOX_TL);
	Draw_TransPic (cx, cy, p);
	p = Draw_CachePic (CACHEPIC_BOX_ML);
	for (n = 0; n < lines; n++)
	{
		cy += 8;
		Draw_TransPic (cx, cy, p);
	}

	p = Draw_CachePic (CACHEPIC_BOX_BL);
	Draw_TransPic (cx, cy+8, p);

	// Draw middle.
	cx += 8;
	while (width > 0)
	{
		cy = y;
		p = Draw_CachePic (CACHEPIC_BOX_TM);
		Draw_TransPic (cx, cy, p);
		p = Draw_CachePic (CACHEPIC_BOX_MM);

		for (n = 0; n < lines; n++)
		{
			cy += 8;
			if (n == 1)
				p = Draw_CachePic (CACHEPIC_BOX_MM2);
			Draw_TransPic (cx, cy, p);
		}

		p = Draw_CachePic (CACHEPIC_BOX_BM);
		Draw_TransPic (cx, cy+8, p);
		width -= 2;
		cx += 16;
	}

	// Draw right side.
	cy = y;
	p = Draw_CachePic (CACHEPIC_BOX_TR);
	Draw_TransPic (cx, cy, p);
	p = Draw_CachePic (CACHEPIC_BOX_MR);

	for (n = 0; n < lines; n++)
	{
		cy += 8;
		Draw_TransPic (cx, cy, p);
	}

	p = Draw_CachePic (CACHEPIC_BOX_BR);
	Draw_TransPic (cx, cy+8, p);
}

// This repeats a 64 * 64 tile graphic to fill the screen around a sized down refresh window.
void Draw_TileClear(int x, int y, int w, int h)
{
	byte white[4] = { 255, 255, 255, 255 };

	R_DrawImage(x, y, w, h, x / 64.0, y / 64.0, w / 64.0, h / 64.0, white, false, draw_backtile->texnum, false, false);
}

void Draw_AlphaRectangleRGB(float x, float y, float w, float h, float thickness, qbool fill, color_t color)
{
	byte bytecolor[4];

	// Is alpha 0?
	if ((byte)(color >> 24 & 0xFF) == 0) {
		return;
	}

	COLOR_TO_RGBA(color, bytecolor);
	thickness = max(0, thickness);

	R_DrawAlphaRectangleRGB(x, y, w, h, thickness, fill, bytecolor);
}

void Draw_AlphaRectangle (int x, int y, int w, int h, byte c, float thickness, qbool fill, float alpha)
{
	Draw_AlphaRectangleRGB(x, y, w, h, thickness, fill,
		RGBA_TO_COLOR(host_basepal[c * 3], host_basepal[c * 3 + 1], host_basepal[c * 3 + 2], (byte)(alpha * 255)));
}

void Draw_AlphaFillRGB(float x, float y, float w, float h, color_t color)
{
	Draw_AlphaRectangleRGB(x, y, w, h, 1, true, color);
}

void Draw_AlphaFill(float x, float y, float w, float h, byte c, float alpha)
{
	Draw_AlphaRectangle(x, y, w, h, c, 1, true, alpha);
}

void Draw_Fill (int x, int y, int w, int h, byte c)
{
	Draw_AlphaFill(x, y, w, h, c, 1);
}

void Draw_AlphaLineRGB (float x_start, float y_start, float x_end, float y_end, float thickness, color_t color)
{
	byte bytecolor[4];

	COLOR_TO_RGBA_PREMULT(color, bytecolor);

	R_Draw_LineRGB(thickness, bytecolor, x_start, y_start, x_end, y_end);
}

void Draw_AlphaLine (float x_start, float y_start, float x_end, float y_end, float thickness, byte c, float alpha)
{
	Draw_AlphaLineRGB (x_start, y_start, x_end, y_end, thickness,
		RGBA_TO_COLOR(host_basepal[c * 3], host_basepal[c * 3 + 1], host_basepal[c * 3 + 2], 255));
}

void Draw_Polygon(int x, int y, vec3_t *vertices, int num_vertices, color_t color)
{
	R_Draw_Polygon(x, y, vertices, num_vertices, color);
}

static void Draw_AlphaPieSliceRGB (int x, int y, float radius, float startangle, float endangle, float thickness, qbool fill, color_t color)
{
	R_Draw_AlphaPieSliceRGB(x, y, radius, startangle, endangle, thickness, fill, color);
}

void Draw_AlphaPieSlice (int x, int y, float radius, float startangle, float endangle, float thickness, qbool fill, byte c, float alpha)
{
	Draw_AlphaPieSliceRGB (x, y, radius, startangle, endangle, thickness, fill,
		RGBA_TO_COLOR(host_basepal[c * 3], host_basepal[c * 3 + 1], host_basepal[c * 3 + 2], (byte)Q_rint(255 * alpha)));
}

void Draw_AlphaCircleRGB(float x, float y, float radius, float thickness, qbool fill, color_t color)
{
	Draw_AlphaPieSliceRGB (x, y, radius, 0, 2 * M_PI, thickness, fill, color);
}

void Draw_AlphaCircle(float x, float y, float radius, float thickness, qbool fill, byte c, float alpha)
{
	Draw_AlphaPieSlice (x, y, radius, 0, 2 * M_PI, thickness, fill, c, alpha);
}

void Draw_AlphaCircleOutlineRGB(float x, float y, float radius, float thickness, color_t color)
{
	Draw_AlphaCircleRGB(x, y, radius, thickness, false, color);
}

void Draw_AlphaCircleOutline(float x, float y, float radius, float thickness, byte color, float alpha)
{
	Draw_AlphaCircle(x, y, radius, thickness, false, color, alpha);
}

void Draw_AlphaCircleFillRGB(float x, float y, float radius, color_t color)
{
	Draw_AlphaCircleRGB(x, y, radius, 1.0, true, color);
}

void Draw_AlphaCircleFill(float x, float y, float radius, byte color, float alpha)
{
	Draw_AlphaCircle(x, y, radius, 1.0, true, color, alpha);
}

void Draw_AlphaRoundedRectangleRGB(float x, float y, float w, float h, float radius_tl, float radius_tr, float radius_br, float radius_bl, float thickness, qbool fill, color_t color)
{
	// If all radii are 0, just draw a regular rectangle
	if (radius_tl <= 0 && radius_tr <= 0 && radius_br <= 0 && radius_bl <= 0) {
		Draw_AlphaRectangleRGB(x, y, w, h, thickness, fill, color);
		return;
	}

	// Clamp radii to half the smaller dimension
	float max_radius = min(w, h) * 0.5f;
	radius_tl = min(radius_tl, max_radius);
	radius_tr = min(radius_tr, max_radius);
	radius_br = min(radius_br, max_radius);
	radius_bl = min(radius_bl, max_radius);

	// Also ensure that adjacent corners don't overlap
	if (radius_tl + radius_tr > w) {
		float scale = w / (radius_tl + radius_tr);
		radius_tl *= scale;
		radius_tr *= scale;
	}
	if (radius_bl + radius_br > w) {
		float scale = w / (radius_bl + radius_br);
		radius_bl *= scale;
		radius_br *= scale;
	}
	if (radius_tl + radius_bl > h) {
		float scale = h / (radius_tl + radius_bl);
		radius_tl *= scale;
		radius_bl *= scale;
	}
	if (radius_tr + radius_br > h) {
		float scale = h / (radius_tr + radius_br);
		radius_tr *= scale;
		radius_br *= scale;
	}

	if (fill) {
		float left_band = max(radius_tl, radius_bl);
		float right_band = max(radius_tr, radius_br);
		float top_band = max(radius_tl, radius_tr);
		float bottom_band = max(radius_bl, radius_br);
		float center_w = w - left_band - right_band;
		float center_h = h - top_band - bottom_band;
		float band_start, band_end_gap, band_height;

		// Center rectangle
		if (center_w > 0 && center_h > 0) {
			Draw_AlphaFillRGB(x + left_band, y + top_band, center_w, center_h, color);
		}

		// Top rectangle
		if (top_band > 0) {
			float top_width = w - radius_tl - radius_tr;
			if (top_width > 0) {
				Draw_AlphaFillRGB(x + radius_tl, y, top_width, top_band, color);
			}
		}

		// Bottom rectangle
		if (bottom_band > 0) {
			float bottom_width = w - radius_bl - radius_br;
			if (bottom_width > 0) {
				Draw_AlphaFillRGB(x + radius_bl, y + h - bottom_band, bottom_width, bottom_band, color);
			}
		}

		// Left rectangle - skip areas already filled by top/bottom strips when corner radius is zero.
		if (left_band > 0) {
			band_start = (radius_tl > 0) ? radius_tl : top_band;
			band_end_gap = (radius_bl > 0) ? radius_bl : bottom_band;
			band_height = h - band_start - band_end_gap;
			if (band_height > 0) {
				Draw_AlphaFillRGB(x, y + band_start, left_band, band_height, color);
			}
		}

		// Right rectangle - same treatment to avoid overdrawing non-rounded corners.
		if (right_band > 0) {
			band_start = (radius_tr > 0) ? radius_tr : top_band;
			band_end_gap = (radius_br > 0) ? radius_br : bottom_band;
			band_height = h - band_start - band_end_gap;
			if (band_height > 0) {
				Draw_AlphaFillRGB(x + w - right_band, y + band_start, right_band, band_height, color);
			}
		}

		// Fill the corners with pie slices; convert to premultiplied color so their shading matches the rectangles.
		color_t corner_color = color;
		if (radius_tl > 0 || radius_tr > 0 || radius_br > 0 || radius_bl > 0) {
			byte corner_bytes[4];
			COLOR_TO_RGBA(color, corner_bytes);
			corner_color = RGBAVECT_TO_COLOR_PREMULT(corner_bytes);
		}
		
		if (radius_tl > 0)
			Draw_AlphaPieSliceRGB(x + radius_tl, y + radius_tl, radius_tl, 0.5 * M_PI, M_PI, 1, true, corner_color);
		if (radius_tr > 0)
			Draw_AlphaPieSliceRGB(x + w - radius_tr, y + radius_tr, radius_tr, 0, 0.5 * M_PI, 1, true, corner_color);
		if (radius_br > 0)
			Draw_AlphaPieSliceRGB(x + w - radius_br, y + h - radius_br, radius_br, 1.5 * M_PI, 2 * M_PI, 1, true, corner_color);
		if (radius_bl > 0)
			Draw_AlphaPieSliceRGB(x + radius_bl, y + h - radius_bl, radius_bl, M_PI, 1.5 * M_PI, 1, true, corner_color);
	} else {
		// For outline/border, we need to draw just the curved parts
		// Since Draw_AlphaPieSliceRGB draws full pie slices with lines to center,
		// we'll use Draw_AlphaCircleRGB with appropriate angles for each corner
		
		// Draw straight lines for the edges
		Draw_AlphaLineRGB(x + radius_tl, y, x + w - radius_tr, y, thickness, color);
		Draw_AlphaLineRGB(x + radius_bl, y + h, x + w - radius_br, y + h, thickness, color);
		Draw_AlphaLineRGB(x, y + radius_tl, x, y + h - radius_bl, thickness, color);
		Draw_AlphaLineRGB(x + w, y + radius_tr, x + w, y + h - radius_br, thickness, color);
		
		// For corners, we'll draw small arc segments
		// We need to draw many small lines to approximate the curves
		int segments = 16; // Number of segments per quarter circle
		int i;
		
		// Top-left corner - arc from top to left
		if (radius_tl > 0) {
			float x1 = 0, y1 = 0;
			for (i = 0; i <= segments; i++) {
				float angle = M_PI * 0.5 + (i * 0.5 * M_PI) / segments;
				float px = x + radius_tl + radius_tl * cos(angle);
				float py = y + radius_tl - radius_tl * sin(angle);
				if (i > 0) {
					Draw_AlphaLineRGB(x1, y1, px, py, thickness, color);
				}
				x1 = px;
				y1 = py;
			}
		}
		
		// Top-right corner - arc from right to top
		if (radius_tr > 0) {
			float x1 = 0, y1 = 0;
			for (i = 0; i <= segments; i++) {
				float angle = (i * 0.5 * M_PI) / segments;
				float px = x + w - radius_tr + radius_tr * cos(angle);
				float py = y + radius_tr - radius_tr * sin(angle);
				if (i > 0) {
					Draw_AlphaLineRGB(x1, y1, px, py, thickness, color);
				}
				x1 = px;
				y1 = py;
			}
		}
		
		// Bottom-right corner - arc from bottom to right
		if (radius_br > 0) {
			float x1 = 0, y1 = 0;
			for (i = 0; i <= segments; i++) {
				float angle = 1.5 * M_PI + (i * 0.5 * M_PI) / segments;
				float px = x + w - radius_br + radius_br * cos(angle);
				float py = y + h - radius_br - radius_br * sin(angle);
				if (i > 0) {
					Draw_AlphaLineRGB(x1, y1, px, py, thickness, color);
				}
				x1 = px;
				y1 = py;
			}
		}
		
		// Bottom-left corner - arc from left to bottom  
		if (radius_bl > 0) {
			float x1 = 0, y1 = 0;
			for (i = 0; i <= segments; i++) {
				float angle = M_PI + (i * 0.5 * M_PI) / segments;
				float px = x + radius_bl + radius_bl * cos(angle);
				float py = y + h - radius_bl - radius_bl * sin(angle);
				if (i > 0) {
					Draw_AlphaLineRGB(x1, y1, px, py, thickness, color);
				}
				x1 = px;
				y1 = py;
			}
		}
	}
}

void Draw_AlphaRoundedFillRGB(float x, float y, float w, float h, float radius_tl, float radius_tr, float radius_br, float radius_bl, color_t color)
{
	Draw_AlphaRoundedRectangleRGB(x, y, w, h, radius_tl, radius_tr, radius_br, radius_bl, 1, true, color);
}

//
// SCALE versions of some functions
//

//=============================================================================
// Draw picture functions
//=============================================================================
void Draw_SAlphaSubPic2(float x, float y, mpic_t *pic, int src_x, int src_y, int src_width, int src_height, float scale_x, float scale_y, float alpha)
{
	float newsl, newtl, newsh, newth;
    float oldglwidth, oldglheight;

    oldglwidth = pic->sh - pic->sl;
    oldglheight = pic->th - pic->tl;

    newsl = pic->sl + (src_x * oldglwidth) / (float)pic->width;
    newsh = newsl + (src_width * oldglwidth) / (float)pic->width;

    newtl = pic->tl + (src_y * oldglheight) / (float)pic->height;
    newth = newtl + (src_height * oldglheight) / (float)pic->height;

	alpha *= overall_alpha;

	R_Draw_SAlphaSubPic2(x, y, pic, src_width, src_height, newsl, newtl, newsh, newth, scale_x, scale_y, alpha);
}

void Draw_SAlphaSubPic(float x, float y, mpic_t *pic, int src_x, int src_y, int src_width, int src_height, float scale, float alpha)
{
	Draw_SAlphaSubPic2(x, y, pic, src_x, src_y, src_width, src_height, scale, scale, alpha);
}

void Draw_SSubPic(float x, float y, mpic_t *gl, int srcx, int srcy, int width, int height, float scale)
{
	Draw_SAlphaSubPic(x, y, gl, srcx, srcy, width, height, scale, 1);
}

void Draw_AlphaSubPic(float x, float y, mpic_t *pic, int srcx, int srcy, int width, int height, float alpha)
{
	Draw_SAlphaSubPic(x, y, pic, srcx, srcy, width, height, 1, alpha);
}

void Draw_SubPic(float x, float y, mpic_t *pic, int srcx, int srcy, int width, int height)
{
	Draw_SAlphaSubPic(x, y, pic, srcx, srcy, width, height, 1, 1);
}

void Draw_AlphaPic(float x, float y, mpic_t *pic, float alpha)
{
	Draw_SAlphaSubPic(x , y, pic, 0, 0, pic->width, pic->height, 1, alpha);
}

void Draw_SAlphaPic(float x, float y, mpic_t *gl, float alpha, float scale)
{
	Draw_SAlphaSubPic(x ,y , gl, 0, 0, gl->width, gl->height, scale, alpha);
}

void Draw_SPic(float x, float y, mpic_t *gl, float scale)
{
	Draw_SAlphaSubPic (x, y, gl, 0, 0, gl->width, gl->height, scale, 1.0);
}

void Draw_FitPic(float x, float y, int fit_width, int fit_height, mpic_t *gl)
{
    float sw, sh;
    sw = (float) fit_width / (float) gl->width;
    sh = (float) fit_height / (float) gl->height;
    Draw_SPic(x, y, gl, min(sw, sh));
}

void Draw_FitPicAlpha(float x, float y, int fit_width, int fit_height, mpic_t *gl, float alpha)
{
	float sw, sh;
	sw = (float) fit_width / (float) gl->width;
	sh = (float) fit_height / (float) gl->height;
	Draw_SAlphaPic(x, y, gl, alpha, min(sw, sh));
}

void Draw_FitPicAlphaCenter(float x, float y, int fit_width, int fit_height, mpic_t* gl, float alpha)
{
	float sw, sh, scale;
	sw = (float)fit_width / (float)gl->width;
	sh = (float)fit_height / (float)gl->height;
	scale = min(sw, sh);
	Draw_SAlphaPic(x + (fit_width - scale * gl->width) / 2.0f, y + (fit_height - scale * gl->height) / 2.0f, gl, alpha, scale);
}

void Draw_STransPic(float x, float y, mpic_t *pic, float scale)
{
    Draw_SPic(x, y, pic, scale);
}

void Draw_Pic(float x, float y, mpic_t *pic)
{
	Draw_SAlphaSubPic(x, y, pic, 0, 0, pic->width, pic->height, 1, 1);
}

void Draw_TransPic(float x, float y, mpic_t *pic)
{
	Draw_Pic(x, y, pic);
}

static char last_mapname[MAX_QPATH] = {0};
static mpic_t *last_lvlshot = NULL;

// If conwidth or conheight changes, adjust conback sizes too.
void Draw_AdjustConback(void)
{
	conback.width  = vid.conwidth;
	conback.height = vid.conheight;

	if (last_lvlshot) {
		// Resize.
		last_lvlshot->width = conback.width;
		last_lvlshot->height = conback.height;
	}
}

static void Draw_DeleteOldLevelshot(mpic_t* pic)
{
	if (pic && R_TextureReferenceIsValid(pic->texnum)) {
		R_DeleteTexture(&pic->texnum);
		if (!CachePic_RemoveByPic(pic)) {
			R_TextureReferenceInvalidate(pic->texnum);
		}
	}
}

void Draw_ClearConback(void)
{
	last_lvlshot = NULL;
	last_mapname[0] = 0;
}

void Draw_InitConback(void)
{
	qpic_t *cb;
	mpic_t *pic_24bit;

	// Level shots init. It's cache based so don't free!
	// Expect the cache to be wiped thus render the old data invalid
	Draw_DeleteOldLevelshot(last_lvlshot);
	Draw_ClearConback();

	if (!glConfig.initialized) {
		return;
	}

	if (!(cb = (qpic_t *)FS_LoadHeapFile("gfx/conback.lmp", NULL))) {
		Sys_Error("Couldn't load gfx/conback.lmp");
		return;
	}
	SwapPic (cb);

	if (cb->width != 320 || cb->height != 200) {
		Sys_Error("Draw_InitConback: conback.lmp size is not 320x200");
	}

	if ((pic_24bit = R_LoadPicImage(va("gfx/%s", scr_conpicture.string), "conback", 0, 0, TEX_ALPHA))) {
		Draw_CopyMPICKeepSize(&conback, pic_24bit);
	}
	else {
		conback.width = cb->width;
		conback.height = cb->height;
		R_LoadPicTexture("conback", &conback, cb->data);
	}

	Draw_AdjustConback();

	// Free loaded console.
	Q_free(cb);
}

void Draw_MapVote(float x, float y, int width, int height, float alpha_override)
{
	mpic_t *lvlshot = NULL;
    float alpha = alpha_override >= 0 ? alpha_override :
                  ((SCR_NEED_CONSOLE_BACKGROUND ? 1 : bound(0, scr_conalpha.value, 1)) * overall_alpha);

    if (map_vote_map[0])
    {
        // Load per-level conback once
        if (strncmp(map_vote_map, last_mapname, sizeof(last_mapname)))
        {
            char name[MAX_QPATH];
            mpic_t* old_levelshot = last_lvlshot;

            snprintf(name, sizeof(name), "textures/levelshots/%s.xxx", map_vote_map);
            if ((last_lvlshot = Draw_CachePicSafe(name, false, true)))
            {
                last_lvlshot->width  = conback.width;
                last_lvlshot->height = conback.height;
            }

            if (last_lvlshot != old_levelshot)
                Draw_DeleteOldLevelshot(old_levelshot);

            strlcpy(last_mapname, map_vote_map, sizeof(last_mapname));
        }

        lvlshot = last_lvlshot;
    }

    if (!alpha)
        return;

    if (lvlshot)
        Draw_FitPicAlphaCenter(x, y, width, height, lvlshot, alpha);
    else
        Draw_FitPicAlphaCenter(x, y, width, height, &conback, alpha);
}

void Draw_ConsoleBackground(int lines)
{
	mpic_t *lvlshot = NULL;
	float alpha = (SCR_NEED_CONSOLE_BACKGROUND ? 1 : bound(0, scr_conalpha.value, 1)) * overall_alpha;

	if (host_mapname.string[0]											// We have mapname.
		 && (    scr_conback.value == 2									// Always per level conback.
			 || (scr_conback.value == 1 && SCR_NEED_CONSOLE_BACKGROUND) // Only at load time.
			))
	{
		// Here we limit call Draw_CachePicSafe() once per level,
		// because if image not found Draw_CachePicSafe() will try open image again each frame, that cause HDD lag.
		if (strncmp(host_mapname.string, last_mapname, sizeof(last_mapname))) {
			char name[MAX_QPATH];
			mpic_t* old_levelshot = last_lvlshot;

			snprintf(name, sizeof(name), "textures/levelshots/%s.xxx", host_mapname.string);
			if ((last_lvlshot = Draw_CachePicSafe(name, false, true))) {
				// Resize.
				last_lvlshot->width  = conback.width;
				last_lvlshot->height = conback.height;
			}
			if (last_lvlshot != old_levelshot) {
				Draw_DeleteOldLevelshot(old_levelshot);
			}

			strlcpy(last_mapname, host_mapname.string, sizeof(last_mapname)); // Save.
		}

		lvlshot = last_lvlshot;
	}

	if (alpha) {
		int con_shift_value = cls.state == ca_active ? con_shift.value : 0;

		Draw_AlphaPic(0, (lines - vid.height) + con_shift_value, lvlshot ? lvlshot : &conback, alpha);
	}
}

void Draw_FadeScreen(float alpha)
{
	alpha = bound(0, alpha, 1) * overall_alpha;
	if (!alpha) {
		return;
	}

	R_Draw_FadeScreen(alpha);

	Sbar_Changed();
}

//=============================================================================
// Draws the little blue disc in the corner of the screen.
// Call before beginning any disc IO.
void Draw_BeginDisc (void)
{
	extern cvar_t r_drawdisc;

	if (!draw_disc || !r_drawdisc.integer) {
		return;
	}

	// Intel cards, most notably Intel 915GM/910GML has problems with
	// writing directly to the front buffer and then flipping the back buffer,
	// so don't draw the I/O disc on those cards, it will cause the console
	// to flicker.
	//
	// From Intels dev network:
	// "Using two dimensional data within a 3D scene is sometimes used to render
	// objects like scoreboards and road signs. When that blit request is sent 
	// to or from a buffer, the data contained within must be updated, causing 
	// a pipeline flush and disabling Zone Rendering. One easy way to generate 
	// the same effect is to use a quad or a billboard that is aligned to the 
	// view frustrum. Similarly, a flip operation while rendering to a back buffer 
	// will cause serialization. Be sure you are done altering the back buffer
	// before you flip.
#ifndef __APPLE__
	if (glConfig.hardwareType == GLHW_INTEL) {
		return;
	}
#endif

	renderer.DrawDisc();
}

// Erases the disc icon.
// Call after completing any disc IO
void Draw_EndDisc(void)
{
}

//
// Changes the projection to orthogonal (2D drawing).
//
void R_Set2D(void)
{
	renderer.Begin2DRendering();
	R_IdentityModelView();
	R_OrthographicProjection(0, vid.width, vid.height, 0, -99999, 99999);
	R_TraceResetRegion(false);
}

void Draw_2dAlphaTexture(float x, float y, float width, float height, texture_ref texture_num, float alpha)
{
	mpic_t pic;

	pic.height = height;
	pic.width = width;
	pic.th = 1;
	pic.tl = 0;
	pic.sh = 1;
	pic.sl = 0;
	pic.texnum = texture_num;

	Draw_AlphaPic(x, y, &pic, alpha);
}

qbool Draw_IsConsoleBackground(mpic_t* pic)
{
	return pic == &conback || pic == last_lvlshot;
}
