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
*/

#include "quakedef.h"
#include "gl_model.h"
#include "pmove.h"
#include "r_local.h"
#include "r_renderer.h"
#include "r_sprite3d.h"
#include "r_texture.h"
#include "qsound.h"
#include "image.h"

cvar_t cl_spray_show = {"cl_spray_show", "0.5"};
cvar_t cl_spray_debug = {"cl_spray_debug", "0"};
cvar_t cl_spray_colorize = {"cl_spray_colorize", "", CVAR_COLOR};
cvar_t cl_spray_image = {"cl_spray_image", "spray"};
cvar_t cl_spray_image_path = {"cl_spray_image_path", "sprays"};
cvar_t cl_spray_distance = {"cl_spray_distance", "512"};
cvar_t cl_spray_size = {"cl_spray_size", "64"};
cvar_t cl_spray_preview_alpha = {"cl_spray_preview_alpha", "0.125"};
cvar_t cl_spray_sound = {"cl_spray_sound", "builtin/spray.flac"};
cvar_t cl_spray_reject_sound = {"cl_spray_reject_sound", "builtin/reject.flac"};

// Client-local only: these decals are visual state, not network/protocol state.
#define CL_MAX_SPRAYS 64
#define CL_MAX_SPRAY_TEXTURES 64
#define CL_MAX_SERVER_SPRAY_IMAGES 64
#define CL_SPRAY_HASH_OFFSET 14695981039346656037ULL
#define CL_SPRAY_HASH_PRIME 1099511628211ULL

// Keep geometry tolerances tight. The final footprint still has to sit on
// actual flat/coplanar BSP polygons; these absorb only float/edge imprecision.
#define CL_SPRAY_SURFACE_EPSILON 1.0f
#define CL_SPRAY_PLANE_EPSILON 0.999f
#define CL_SPRAY_FIT_STEP 4.0f
#define CL_SPRAY_PLACEMENT_ALPHA 1.0f
#define CL_SPRAY_UPLOAD_CHUNK_BYTES 512

// Keep client uploads aligned with MVDSV's default spray send budget. Two
// 512-byte chunks recover 1024-byte throughput while still checking space.
#define CL_SPRAY_UPLOAD_CHUNKS_PER_FRAME 2
#define CL_SPRAY_RELIABLE_RESERVE 256
#define CL_SPRAY_RECEIVE_BLOCK_BYTES 512

typedef struct cl_spray_texture_s {
	char name[MAX_QPATH];

	// These dimensions are the normalized image dimensions used by the renderer
	// and network upload. Source files may be larger, but are downsampled before
	// they reach this cache.
	int width;
	int height;

	// Hash identity for the normalized RGBA payload. Cached pixels and hash are
	// created together; placement/upload/server cache checks rely on that.
	unsigned long long hash;
	qbool have_hash;

	// Classic OpenGL can render the 2D texture directly. Modern OpenGL sprite
	// shaders sample texture arrays, so each image also gets a one-layer array.
	texture_ref texture;
	texture_ref texture_array;

	// Keep the normalized RGBA payload used to create the renderer texture.
	// Uploads copy from here instead of reloading and rescaling the source file.
	byte *pixels;
	int byte_count;
} cl_spray_texture_t;

typedef struct cl_spray_s {
	qbool active;

	// Captured at placement time so changing cl_spray_image only affects future
	// sprays and previews, not decals that are already on the map.
	texture_ref texture;
	int texture_index;
	int upload_id;

	// A spray is an oriented world-space quad. right/up are tangent to the
	// target surface and are player-view relative when placed.
	vec3_t origin;
	vec3_t right;
	vec3_t up;
	float half_width;
	float half_height;

	// Placement alpha is intentionally separate from the image pixels/hash.
	// Changing opacity should not create another copy of the same image bytes.
	float alpha;
} cl_spray_t;

typedef struct cl_server_spray_image_s {
	qbool active;
	qbool have_begin;
	qbool silent;

	// Complete entries are an image cache, not just receive buffers. MVDSV may
	// later send only placement data if it knows this client has the hash.
	qbool complete;
	int id;
	unsigned long long hash;

	// Server-provided image dimensions and byte count are validated before
	// chunks are accepted. The receive buffer is max-sized, but only byte_count
	// bytes are meaningful for this image.
	int width;
	int height;
	int byte_count;
	int received;

	// Demo/QTV hidden blocks can expose spray payload chunks out of order.
	// Track which fixed-size ranges are present so playback can complete the
	// image once all chunks arrive, without weakening live reliable parsing.
	unsigned long long received_blocks[2];

	vec3_t origin;
	vec3_t right;
	vec3_t up;
	float half_width;
	float half_height;

	// Alpha is placement metadata from the sprayer, not part of image identity.
	float alpha;

	// Server-provided sprays arrive as raw RGBA. Keep the pixels until the full
	// image is assembled and uploaded to the renderer/cache.
	byte pixels[spraynet_max_bytes];
	texture_ref texture;
	texture_ref texture_array;
} cl_server_spray_image_t;

typedef struct cl_spray_upload_s {
	qbool active;
	qbool sent_begin;
	qbool accepted;

	// The server accepts placement first, then says whether it still needs the
	// RGBA payload. False means it already has this hash cached.
	qbool send_pixels;
	int id;
	int placed_index;
	int offset;
	unsigned long long hash;

	// The upload stores the normalized image, not necessarily the source file's
	// original dimensions. This is the exact payload MVDSV verifies and caches.
	int width;
	int height;
	int byte_count;
	byte *pixels;

	vec3_t origin;
	vec3_t right;
	vec3_t up;
	float half_width;
	float half_height;
	float alpha;
} cl_spray_upload_t;

static cl_spray_t cl_sprays[CL_MAX_SPRAYS];
static cl_spray_texture_t cl_spray_textures[CL_MAX_SPRAY_TEXTURES];
static cl_server_spray_image_t cl_server_spray_images[CL_MAX_SERVER_SPRAY_IMAGES];
static cl_spray_upload_t cl_spray_upload;
static int cl_spray_texture_count;
static int cl_spray_next;
static int cl_spray_upload_next_id = 1;
static qbool cl_spray_preview_active;
static model_t *cl_spray_worldmodel;
static qbool cl_spray_warning;
static sfx_t *cl_spray_sfx;
static char cl_spray_sfx_name[MAX_QPATH];
static sfx_t *cl_spray_reject_sfx;
static char cl_spray_reject_sfx_name[MAX_QPATH];

static void CL_SprayCancelUpload(void);
static void CL_SprayClearServerImageCache(void);
static void CL_SprayPlayRejectSound(void);
static cl_spray_texture_t *CL_SprayTextureCacheForName(const char *filename);
void FS_EnumerateFiles(char *match, int (*func)(char *, int, void *), void *parm);

#ifdef RENDERER_OPTION_MODERN_OPENGL
void GL_AddTextureToArray(texture_ref arrayTexture, int index, texture_ref tex2dname, qbool tile);
#endif

static void CL_SprayDebugHash(const char *action, int id, unsigned long long hash, const char *details)
{
	if (!cl_spray_debug.value) {
		return;
	}

	Con_Printf("spray debug: %s id=%d hash=%08x%08x %s\n",
			action,
			id,
			(unsigned int)(hash >> 32),
			(unsigned int)(hash & 0xffffffffULL),
			details ? details : "");
}

static unsigned long long CL_SprayHashBytes(const byte *pixels, int width, int height, int byte_count)
{
	unsigned long long hash = CL_SPRAY_HASH_OFFSET;
	int i;
	unsigned int dimensions[2];

	// FNV-1a is used only as a fast image cache key. The server verifies the
	// uploaded bytes match the claimed hash, but this is not a security hash.
	dimensions[0] = (unsigned int)width;
	dimensions[1] = (unsigned int)height;
	for (i = 0; i < (int)sizeof(dimensions); ++i) {
		hash ^= ((byte *)dimensions)[i];
		hash *= CL_SPRAY_HASH_PRIME;
	}
	for (i = 0; i < byte_count; ++i) {
		hash ^= pixels[i];
		hash *= CL_SPRAY_HASH_PRIME;
	}
	return hash;
}

// Store normalized image bytes and their identity as one operation. A cache
// entry with pixels but no hash would break metadata-only spray reuse.
static void CL_SprayCacheSetPixels(cl_spray_texture_t *cached, byte *pixels, int width, int height, int byte_count)
{
	cached->pixels = pixels;
	cached->width = width;
	cached->height = height;
	cached->byte_count = byte_count;
	cached->hash = CL_SprayHashBytes(cached->pixels, cached->width, cached->height, cached->byte_count);
	cached->have_hash = true;
}

static qbool CL_SprayValidImageSize(int width, int height, int byte_count)
{
	// Network spray images are RGBA and capped by qwprot. Source images may be
	// larger; CL_SprayLoadNormalizedPixels scales them down before upload.
	return width > 0 && height > 0 && width <= spraynet_max_width && height <= spraynet_max_height && byte_count == width * height * spraynet_bpp;
}

static int CL_SprayReceiveBlockCount(int byte_count)
{
	// The protocol caps sprays at 128x128 RGBA, so two 64-bit masks cover every
	// 512-byte range in the largest possible payload.
	return (byte_count + CL_SPRAY_RECEIVE_BLOCK_BYTES - 1) / CL_SPRAY_RECEIVE_BLOCK_BYTES;
}

static qbool CL_SprayReceiveBlockMarked(const cl_server_spray_image_t *image, int block)
{
	if (block < 0 || block >= CL_SprayReceiveBlockCount(image->byte_count)) {
		return false;
	}
	return (image->received_blocks[block / 64] & (1ULL << (block % 64))) != 0;
}

static void CL_SprayMarkReceivedBytes(cl_server_spray_image_t *image, int offset, int len)
{
	int first = offset / CL_SPRAY_RECEIVE_BLOCK_BYTES;
	int last = (offset + len - 1) / CL_SPRAY_RECEIVE_BLOCK_BYTES;
	int block;

	// Chunks are currently aligned to this block size, but marking the covered
	// range keeps the bookkeeping correct if that ever changes.
	for (block = first; block <= last; ++block) {
		if (block >= 0 && block < CL_SprayReceiveBlockCount(image->byte_count)) {
			image->received_blocks[block / 64] |= 1ULL << (block % 64);
		}
	}
}

static void CL_SprayUpdateContiguousReceive(cl_server_spray_image_t *image)
{
	// Advance the ordered frontier over any blocks that arrived early. The hash
	// check still gates final use of the reconstructed image.
	while (image->received < image->byte_count) {
		int block = image->received / CL_SPRAY_RECEIVE_BLOCK_BYTES;
		int next = min((block + 1) * CL_SPRAY_RECEIVE_BLOCK_BYTES, image->byte_count);

		if (!CL_SprayReceiveBlockMarked(image, block)) {
			return;
		}
		image->received = next;
	}
}

static qbool CL_SprayReliableCanWrite(int needed)
{
	return cls.netchan.message.cursize <= cls.netchan.message.maxsize - needed - CL_SPRAY_RELIABLE_RESERVE;
}

static byte CL_SprayFloatToByte(double value)
{
	if (value <= 0) {
		return 0;
	}
	if (value >= 255) {
		return 255;
	}
	return (byte)(value + 0.5);
}

static void CL_SprayDownscalePixels(const byte *source_pixels, int source_width, int source_height, byte *scaled_pixels, int scaled_width, int scaled_height)
{
	int y;
	double x_scale = (double)source_width / scaled_width;
	double y_scale = (double)source_height / scaled_height;

	// Spray sources can be much larger than the network payload. Use an area
	// filter instead of point/bilinear sampling so each output pixel represents
	// the full source region it covers.
	for (y = 0; y < scaled_height; ++y) {
		int x;
		double source_y0 = y * y_scale;
		double source_y1 = (y + 1) * y_scale;
		int sy_start = (int)source_y0;
		int sy_end = (int)source_y1;

		if (sy_end < source_y1) {
			++sy_end;
		}
		if (sy_end > source_height) {
			sy_end = source_height;
		}

		for (x = 0; x < scaled_width; ++x) {
			double source_x0 = x * x_scale;
			double source_x1 = (x + 1) * x_scale;
			int sx_start = (int)source_x0;
			int sx_end = (int)source_x1;
			int sx, sy;
			double r = 0;
			double g = 0;
			double b = 0;
			double a = 0;
			double covered = 0;
			byte *out = scaled_pixels + (y * scaled_width + x) * spraynet_bpp;

			if (sx_end < source_x1) {
				++sx_end;
			}
			if (sx_end > source_width) {
				sx_end = source_width;
			}

			for (sy = sy_start; sy < sy_end; ++sy) {
				double overlap_y0 = max(source_y0, (double)sy);
				double overlap_y1 = min(source_y1, (double)(sy + 1));
				double y_weight = overlap_y1 - overlap_y0;

				if (y_weight <= 0) {
					continue;
				}

				for (sx = sx_start; sx < sx_end; ++sx) {
					double overlap_x0 = max(source_x0, (double)sx);
					double overlap_x1 = min(source_x1, (double)(sx + 1));
					double weight = (overlap_x1 - overlap_x0) * y_weight;
					const byte *in;
					double alpha;

					if (weight <= 0) {
						continue;
					}

					in = source_pixels + (sy * source_width + sx) * spraynet_bpp;
					alpha = in[3];

					// Premultiply while filtering, then unpremultiply below.
					// This keeps transparent borders from bleeding arbitrary RGB
					// into partially transparent output pixels.
					r += in[0] * alpha * weight;
					g += in[1] * alpha * weight;
					b += in[2] * alpha * weight;
					a += alpha * weight;
					covered += weight;
				}
			}

			if (a > 0) {
				out[0] = CL_SprayFloatToByte(r / a);
				out[1] = CL_SprayFloatToByte(g / a);
				out[2] = CL_SprayFloatToByte(b / a);
			}
			else {
				out[0] = out[1] = out[2] = 0;
			}
			out[3] = CL_SprayFloatToByte(a / covered);
		}
	}
}

static byte *CL_SprayLoadNormalizedPixels(const char *filename, int *width, int *height, int *byte_count, qbool quiet)
{
	int source_width, source_height;
	byte *source_pixels;

	*width = 0;
	*height = 0;
	*byte_count = 0;

	// Load the user's actual image without forcing protocol dimensions. This
	// permits high-resolution source art while keeping network payloads
	// small and deterministic.
	source_pixels = R_LoadImagePixels(filename, 0, 0, TEX_ALPHA | TEX_NOSCALE | TEX_NO_TEXTUREMODE, &source_width, &source_height);
	if (!source_pixels) {
		if (!quiet) {
			Com_Printf("spray: couldn't load image \"%s\"\n", filename);
			CL_SprayPlayRejectSound();
		}
		return NULL;
	}
	if (cl_spray_debug.value) {
		Con_Printf("spray debug: image load name=\"%s\" source=%dx%d\n", filename, source_width, source_height);
	}

	if (source_width <= 0 || source_height <= 0) {
		Q_free(source_pixels);
		if (!quiet) {
			Com_Printf("spray: image \"%s\" has invalid dimensions\n", filename);
			CL_SprayPlayRejectSound();
		}
		return NULL;
	}

	if (source_width > spraynet_max_width || source_height > spraynet_max_height) {
		float scale = min((float)spraynet_max_width / source_width, (float)spraynet_max_height / source_height);

		*width = max(1, (int)(source_width * scale + 0.5f));
		*height = max(1, (int)(source_height * scale + 0.5f));
	}
	else {
		*width = source_width;
		*height = source_height;
	}
	*byte_count = *width * *height * spraynet_bpp;

	if (source_width != *width || source_height != *height) {
		byte *scaled_pixels = (byte *)Q_malloc(*byte_count);

		// The normalized pixels are the canonical client representation: local
		// rendering, hashing, and upload all use this scaled copy.
		if (cl_spray_debug.value) {
			Con_Printf("spray debug: image scale name=\"%s\" source=%dx%d normalized=%dx%d bytes=%d\n",
					filename, source_width, source_height, *width, *height, *byte_count);
		}
		CL_SprayDownscalePixels(source_pixels, source_width, source_height, scaled_pixels, *width, *height);
		Q_free(source_pixels);
		return scaled_pixels;
	}

	if (cl_spray_debug.value) {
		Con_Printf("spray debug: image scale skipped name=\"%s\" source=%dx%d normalized=%dx%d bytes=%d\n",
				filename, source_width, source_height, *width, *height, *byte_count);
	}
	return source_pixels;
}

static void CL_SprayMSGWriteHash(sizebuf_t *msg, unsigned long long hash)
{
	MSG_WriteLong(msg, (int)(hash & 0xffffffffULL));
	MSG_WriteLong(msg, (int)(hash >> 32));
}

static unsigned long long CL_SprayMSGReadHash(void)
{
	unsigned int lo = (unsigned int)MSG_ReadLong();
	unsigned int hi = (unsigned int)MSG_ReadLong();

	return ((unsigned long long)hi << 32) | lo;
}

// Network sprays are sent as raw RGBA bytes, not as image files. Upload a copy
// because R_LoadTexturePixels may premultiply/gamma-adjust the buffer in place.
static texture_ref CL_SprayTextureFromPixels(int id, byte *pixels, int width, int height, int byte_count, texture_ref *texture_array)
{
	byte *upload_pixels;
	texture_ref texture;

	*texture_array = null_texture_reference;

	upload_pixels = (byte *) Q_malloc(byte_count);
	memcpy(upload_pixels, pixels, byte_count);
	texture = R_LoadTexturePixels(upload_pixels, va("server-spray:%d", id), width, height, TEX_ALPHA | TEX_PREMUL_ALPHA | TEX_NOSCALE | TEX_NO_TEXTUREMODE);
	Q_free(upload_pixels);
	if (!R_TextureReferenceIsValid(texture)) {
		return null_texture_reference;
	}
	renderer.TextureSetFiltering(texture, texture_minification_linear, texture_magnification_linear);

#ifdef RENDERER_OPTION_MODERN_OPENGL
	if (R_UseModernOpenGL()) {
		*texture_array = R_CreateTextureArray(va("server-spray-array:%d", id), width, height, 1, TEX_ALPHA | TEX_PREMUL_ALPHA | TEX_NOSCALE | TEX_NO_TEXTUREMODE);
		if (R_TextureReferenceIsValid(*texture_array)) {
			GL_AddTextureToArray(*texture_array, 0, texture, false);
			renderer.TextureSetFiltering(*texture_array, texture_minification_linear, texture_magnification_linear);
			return *texture_array;
		}
	}
#endif

	return texture;
}

// Lazily load and renderer-adapt a cached spray image. This keeps placement
// cheap after first use while preserving identity for already placed sprays.
static texture_ref CL_SprayCacheTexture(cl_spray_texture_t *cached, int *texture_index, qbool quiet)
{
	*texture_index = 0;

	if (!R_TextureReferenceIsValid(cached->texture)) {
		int width, height;
		int byte_count;
		byte *texture_pixels;

		if (!cached->pixels) {
			byte *pixels = CL_SprayLoadNormalizedPixels(cached->name, &width, &height, &byte_count, quiet);
			if (!pixels) {
				return null_texture_reference;
			}

			// Cache normalized bytes, dimensions, and hash together. Modern
			// OpenGL texture arrays use these dimensions, not source file size.
			CL_SprayCacheSetPixels(cached, pixels, width, height, byte_count);
		}

		// R_LoadTexturePixels can premultiply/mutate its input. Keep the cached
		// CPU payload canonical for hashing/upload and texture rebuilds.
		texture_pixels = (byte *)Q_malloc(cached->byte_count);
		memcpy(texture_pixels, cached->pixels, cached->byte_count);
		cached->texture = R_LoadTexturePixels(texture_pixels, va("spray:%s", cached->name), cached->width, cached->height, TEX_ALPHA | TEX_PREMUL_ALPHA | TEX_NOSCALE | TEX_NO_TEXTUREMODE);
		Q_free(texture_pixels);
		if (!R_TextureReferenceIsValid(cached->texture)) {
			if (!quiet) {
				Com_Printf("spray: couldn't load image \"%s\"\n", cached->name);
				CL_SprayPlayRejectSound();
			}
			return null_texture_reference;
		}
		if (cl_spray_debug.value) {
			Con_Printf("spray debug: image renderer texture created name=\"%s\" size=%dx%d\n",
					cached->name, cached->width, cached->height);
		}
		renderer.TextureSetFiltering(cached->texture, texture_minification_linear, texture_magnification_linear);
	}

#ifdef RENDERER_OPTION_MODERN_OPENGL
	if (R_UseModernOpenGL()) {
		if (!R_TextureReferenceIsValid(cached->texture_array)) {
			cached->texture_array = R_CreateTextureArray(va("spray-array:%s", cached->name), cached->width, cached->height, 1, TEX_ALPHA | TEX_PREMUL_ALPHA | TEX_NOSCALE | TEX_NO_TEXTUREMODE);
			if (R_TextureReferenceIsValid(cached->texture_array)) {
				GL_AddTextureToArray(cached->texture_array, 0, cached->texture, false);
				renderer.TextureSetFiltering(cached->texture_array, texture_minification_linear, texture_magnification_linear);
				if (cl_spray_debug.value) {
					Con_Printf("spray debug: image renderer texture array created name=\"%s\" size=%dx%d\n",
							cached->name, cached->width, cached->height);
				}
			}
		}
		return cached->texture_array;
	}
#endif

	return cached->texture;
}

// Placed sprays are map-local. Local file textures are intentionally kept
// because they may be reused on the next map and texture references remain
// valid. Server image cache lifetime is controlled by clear_server_images.
static void CL_SprayClearPlaced(qbool clear_server_images)
{
	memset(cl_sprays, 0, sizeof(cl_sprays));
	if (clear_server_images) {
		CL_SprayClearServerImageCache();
	}
	CL_SprayCancelUpload();
	cl_spray_next = 0;
	cl_spray_warning = false;
	cl_spray_worldmodel = cl.worldmodel;
}

static void CL_SprayNormalizeImageDirectory(char *path, size_t path_size)
{
	const char *directory = cl_spray_image_path.string;
	const char *slash;
	size_t first_len;

	while (*directory == '/' || *directory == '\\') {
		++directory;
	}

	slash = strpbrk(directory, "/\\");
	first_len = slash ? (size_t)(slash - directory) : strlen(directory);

	// cl_spray_image_path is relative to the active game dir. Accept values
	// that include that dir name, such as "qw/sprays", by dropping it.
	if (first_len == strlen(com_gamedirfile) && !strncmp(directory, com_gamedirfile, first_len)) {
		directory += first_len;
		while (*directory == '/' || *directory == '\\') {
			++directory;
		}
	}

	strlcpy(path, directory, path_size);
}

static qbool CL_SprayImagePath(char *path, size_t path_size)
{
	const char *filename = COM_SkipPath(cl_spray_image.string);
	char directory[MAX_QPATH];
	int written;

	if (!filename[0]) {
		path[0] = '\0';
		return false;
	}

	CL_SprayNormalizeImageDirectory(directory, sizeof(directory));
	if (directory[0]) {
		written = snprintf(path, path_size, "%s/%s/%s", com_gamedirfile, directory, filename);
	}
	else {
		written = snprintf(path, path_size, "%s/%s", com_gamedirfile, filename);
	}

	return written > 0 && (size_t)written < path_size;
}

// Look up a spray image path, adding it to the cache on first use. Cache keys
// deliberately omit the extension, matching normal R_LoadImagePixels behavior
// and the cl_spray_image cvar.
static texture_ref CL_SprayTextureForPath(const char *filename, int *texture_index, int *width, int *height, qbool quiet)
{
	int i;
	int local_texture_index;
	cl_spray_texture_t *cached;

	if (!texture_index) {
		texture_index = &local_texture_index;
	}
	*texture_index = 0;
	if (width) {
		*width = 0;
	}
	if (height) {
		*height = 0;
	}

	for (i = 0; i < cl_spray_texture_count; ++i) {
		if (!strcmp(cl_spray_textures[i].name, filename)) {
			texture_ref texture = CL_SprayCacheTexture(&cl_spray_textures[i], texture_index, quiet);

			// Report if reusing an already cached image.
			if (R_TextureReferenceIsValid(texture)) {
				if (cl_spray_debug.value) {
					Con_Printf("spray debug: image cache hit name=\"%s\" size=%dx%d hash=%s%08x%08x\n",
							filename,
							cl_spray_textures[i].width,
							cl_spray_textures[i].height,
							cl_spray_textures[i].have_hash ? "" : "none/",
							cl_spray_textures[i].have_hash ? (unsigned int)(cl_spray_textures[i].hash >> 32) : 0,
							cl_spray_textures[i].have_hash ? (unsigned int)(cl_spray_textures[i].hash & 0xffffffffULL) : 0);
				}
				if (width) {
					*width = cl_spray_textures[i].width;
				}
				if (height) {
					*height = cl_spray_textures[i].height;
				}
			}
			return texture;
		}
	}

	if (cl_spray_texture_count == CL_MAX_SPRAY_TEXTURES) {
		if (!quiet) {
			Com_Printf("spray: too many spray images loaded\n");
			CL_SprayPlayRejectSound();
		}
		return null_texture_reference;
	}

	cached = &cl_spray_textures[cl_spray_texture_count++];
	memset(cached, 0, sizeof(*cached));
	strlcpy(cached->name, filename, sizeof(cached->name));
	if (cl_spray_debug.value) {
		Con_Printf("spray debug: image cache add name=\"%s\" slot=%d\n", filename, cl_spray_texture_count - 1);
	}

	{
		texture_ref texture = CL_SprayCacheTexture(cached, texture_index, quiet);

		if (R_TextureReferenceIsValid(texture)) {
			if (cl_spray_debug.value) {
				Con_Printf("spray debug: image cache ready name=\"%s\" size=%dx%d\n", filename, cached->width, cached->height);
			}
			if (width) {
				*width = cached->width;
			}
			if (height) {
				*height = cached->height;
			}
		}
		return texture;
	}
}

// Look up the current cvar image, adding it to the cache on first use.
static texture_ref CL_SprayTextureForRenderer(int *texture_index, int *width, int *height, qbool quiet)
{
	char filename[MAX_QPATH];

	if (!CL_SprayImagePath(filename, sizeof(filename))) {
		if (texture_index) {
			*texture_index = 0;
		}
		if (width) {
			*width = 0;
		}
		if (height) {
			*height = 0;
		}
		return null_texture_reference;
	}

	return CL_SprayTextureForPath(filename, texture_index, width, height, quiet);
}

typedef struct cl_spray_preload_s {
	char cache_path[MAX_QPATH];
	int loaded;
} cl_spray_preload_t;

static int CL_SprayPreloadFile(char *name, int size, void *parm)
{
	char filename[MAX_QPATH];
	char stripped[MAX_QPATH];
	cl_spray_preload_t *preload = (cl_spray_preload_t *)parm;
	cl_spray_texture_t *cached;

	(void)size;

	COM_StripExtension(COM_SkipPath(name), stripped, sizeof(stripped));
	snprintf(filename, sizeof(filename), "%s/%s", preload->cache_path, stripped);
	cached = CL_SprayTextureCacheForName(filename);
	if (!cached) {
		if (cl_spray_texture_count == CL_MAX_SPRAY_TEXTURES) {
			return 1;
		}

		cached = &cl_spray_textures[cl_spray_texture_count++];
		memset(cached, 0, sizeof(*cached));
		strlcpy(cached->name, filename, sizeof(cached->name));
		if (cl_spray_debug.value) {
			Con_Printf("spray debug: image cache add name=\"%s\" slot=%d\n", filename, cl_spray_texture_count - 1);
		}
	}

	if (!cached->pixels) {
		int width, height, byte_count;
		byte *pixels = CL_SprayLoadNormalizedPixels(cached->name, &width, &height, &byte_count, true);
		if (!pixels) {
			return 1;
		}
		CL_SprayCacheSetPixels(cached, pixels, width, height, byte_count);
	}

	if (cl_spray_debug.value) {
		Con_Printf("spray debug: image preload ready name=\"%s\" size=%dx%d bytes=%d hash=%08x%08x\n",
				filename, cached->width, cached->height, cached->byte_count,
				(unsigned int)(cached->hash >> 32),
				(unsigned int)(cached->hash & 0xffffffffULL));
	}
	++preload->loaded;

	return 1;
}

void CL_SpraysPreloadImages(void)
{
	static const char *extensions[] = { "png", "jpg", "jpeg", "tga", "pcx" };
	cl_spray_preload_t preload;
	char directory[MAX_QPATH];
	char match[MAX_QPATH];
	size_t i;

	CL_SprayNormalizeImageDirectory(directory, sizeof(directory));
	if (!directory[0]) {
		return;
	}

	memset(&preload, 0, sizeof(preload));
	if (directory[0]) {
		snprintf(preload.cache_path, sizeof(preload.cache_path), "%s/%s", com_gamedirfile, directory);
	}
	else {
		strlcpy(preload.cache_path, com_gamedirfile, sizeof(preload.cache_path));
	}

	for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
		snprintf(match, sizeof(match), "%s/*.%s", directory, extensions[i]);
		FS_EnumerateFiles(match, CL_SprayPreloadFile, &preload);
	}

	if (cl_spray_debug.value) {
		Con_Printf("spray debug: preload path=\"%s\" enumerate=\"%s\" images=%d\n", preload.cache_path, directory, preload.loaded);
	}
	Con_Printf("Spray decals loaded and cached from: %s\n", preload.cache_path);
}

void CL_SpraysRefreshRendererTextures(void)
{
	int i;
	int refreshed = 0;

	for (i = 0; i < cl_spray_texture_count; ++i) {
		texture_ref texture;
		int texture_index;

		if (!cl_spray_textures[i].pixels) {
			continue;
		}

		texture = CL_SprayCacheTexture(&cl_spray_textures[i], &texture_index, true);
		if (R_TextureReferenceIsValid(texture)) {
			++refreshed;
		}
	}

	if (cl_spray_debug.value) {
		Con_Printf("spray debug: refreshed renderer textures for %d cached images\n", refreshed);
	}
}

// Find the named local texture entry after CL_SprayTextureForRenderer has
// created it. This is used to attach the freshly computed upload hash.
static cl_spray_texture_t *CL_SprayTextureCacheForName(const char *filename)
{
	int i;

	for (i = 0; i < cl_spray_texture_count; ++i) {
		if (!strcmp(cl_spray_textures[i].name, filename)) {
			return &cl_spray_textures[i];
		}
	}
	return NULL;
}

// Local images can satisfy placement-only messages too. That case matters for
// the original sender: MVDSV knows the sender has the image even though it did
// not send the pixels back to that client.
static cl_spray_texture_t *CL_SprayTextureCacheForHash(unsigned long long hash)
{
	int i;

	for (i = 0; i < cl_spray_texture_count; ++i) {
		if (cl_spray_textures[i].have_hash && cl_spray_textures[i].hash == hash) {
			return &cl_spray_textures[i];
		}
	}
	return NULL;
}

// Render surfaces can use the back side of their stored plane. Normalize that
// so all coplanar and dot-product tests use visible-surface orientation.
static void CL_SpraySurfaceNormal(msurface_t *surf, vec3_t normal)
{
	if (surf->flags & SURF_PLANEBACK) {
		VectorNegate(surf->plane->normal, normal);
	}
	else {
		VectorCopy(surf->plane->normal, normal);
	}
}

// Build player-relative decal axes on the target surface. Projecting view-up
// and view-right onto the plane keeps floor/wall sprays upright for the player
// who placed them instead of inheriting arbitrary map texture orientation.
static void CL_SprayViewAxis(vec3_t normal, vec3_t right, vec3_t up)
{
	float dot;
	vec3_t view_forward, view_right, view_up;

	AngleVectors(r_refdef.viewangles, view_forward, view_right, view_up);

	// Start with the player's visual "up", then remove any component pointing
	// out of the wall/floor so the vector lies exactly on the spray plane.
	VectorCopy(view_up, up);
	dot = DotProduct(up, normal);
	VectorMA(up, -dot, normal, up);
	if (!VectorNormalize(up)) {
		// Looking straight into a floor/ceiling can collapse view_up after
		// projection, so fall back to projected forward direction.
		VectorCopy(view_forward, up);
		dot = DotProduct(up, normal);
		VectorMA(up, -dot, normal, up);
	}
	if (!VectorNormalize(up)) {
		// Last-resort stable tangent if both view vectors are degenerate.
		PerpendicularVector(up, normal);
	}
	if (DotProduct(up, view_up) < 0) {
		VectorNegate(up, up);
	}

	// Build right independently, then make it orthogonal to up to avoid a
	// slightly skewed quad after projection onto angled surfaces.
	VectorCopy(view_right, right);
	dot = DotProduct(right, normal);
	VectorMA(right, -dot, normal, right);
	dot = DotProduct(up, right);
	VectorMA(right, -dot, up, right);
	if (!VectorNormalize(right)) {
		CrossProduct(up, normal, right);
		VectorNormalize(right);
	}
	if (DotProduct(right, view_right) < 0) {
		VectorNegate(right, right);
	}
}

// Strict point-in-polygon test for the actual BSP render face. Texture extents
// are not enough: they can accept points beyond clipped polygon edges, which
// would let a spray float outside the visible surface.
static qbool CL_SprayPointOnSurface(msurface_t *surf, vec3_t point)
{
	int i;
	vec3_t normal;
	vec3_t center;

	if (!cl.worldmodel || surf->numedges < 3) {
		return false;
	}

	CL_SpraySurfaceNormal(surf, normal);
	VectorClear(center);

	// Use the polygon centroid to determine the inside side of each edge.
	for (i = 0; i < surf->numedges; ++i) {
		int edge_index = cl.worldmodel->surfedges[surf->firstedge + i];
		medge_t *edge = &cl.worldmodel->edges[abs(edge_index)];
		mvertex_t *vertex = &cl.worldmodel->vertexes[edge_index >= 0 ? edge->v[0] : edge->v[1]];

		VectorAdd(center, vertex->position, center);
	}
	VectorScale(center, 1.0f / surf->numedges, center);

	// Convex BSP faces should have every inside point on the same side of every
	// directed edge as the centroid. The small tolerance prevents edge flicker.
	for (i = 0; i < surf->numedges; ++i) {
		int edge_index = cl.worldmodel->surfedges[surf->firstedge + i];
		medge_t *edge = &cl.worldmodel->edges[abs(edge_index)];
		mvertex_t *v0 = &cl.worldmodel->vertexes[edge_index >= 0 ? edge->v[0] : edge->v[1]];
		mvertex_t *v1 = &cl.worldmodel->vertexes[edge_index >= 0 ? edge->v[1] : edge->v[0]];
		vec3_t edge_vec, center_vec, point_vec, center_cross, point_cross;
		float center_side, point_side;

		VectorSubtract(v1->position, v0->position, edge_vec);
		VectorSubtract(center, v0->position, center_vec);
		VectorSubtract(point, v0->position, point_vec);
		CrossProduct(edge_vec, center_vec, center_cross);
		CrossProduct(edge_vec, point_vec, point_cross);
		center_side = DotProduct(center_cross, normal);
		point_side = DotProduct(point_cross, normal);

		if (center_side >= 0) {
			if (point_side < -0.5f) {
				return false;
			}
		}
		else if (point_side > 0.5f) {
			return false;
		}
	}

	return true;
}

// A visual wall/floor can be split into multiple coplanar BSP faces. Treat
// adjacent faces as one valid spray area, while still rejecting sky/turb/sprite
// surfaces and anything not on the same plane.
static qbool CL_SprayPointOnCoplanarSurface(vec3_t point, vec3_t normal)
{
	int i;

	if (!cl.worldmodel) {
		return false;
	}

	for (i = cl.worldmodel->firstmodelsurface; i < cl.worldmodel->firstmodelsurface + cl.worldmodel->nummodelsurfaces; ++i) {
		msurface_t *surf = &cl.worldmodel->surfaces[i];
		vec3_t surf_normal;
		float dist;

		if ((surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWSPRITE)) || !surf->texinfo) {
			continue;
		}

		CL_SpraySurfaceNormal(surf, surf_normal);
		if (DotProduct(surf_normal, normal) < CL_SPRAY_PLANE_EPSILON) {
			continue;
		}

		dist = fabs(DotProduct(point, surf_normal) - (surf->flags & SURF_PLANEBACK ? -surf->plane->dist : surf->plane->dist));
		if (dist > CL_SPRAY_SURFACE_EPSILON) {
			continue;
		}

		if (CL_SprayPointOnSurface(surf, point)) {
			return true;
		}
	}

	return false;
}

// Sample the full decal footprint, not just corners. This catches narrow gaps
// and clipped geometry that would otherwise leave a floating section.
static qbool CL_SprayPointFits(vec3_t point, vec3_t normal, vec3_t right, vec3_t up, float half_width, float half_height)
{
	int x, y;

	for (x = -4; x <= 4; ++x) {
		for (y = -4; y <= 4; ++y) {
			vec3_t test;

			// The 9x9 grid is intentionally denser than corners/perimeter; it
			// catches small clipped holes and BSP splits inside the decal area.
			VectorMA(point, x * half_width / 4.0f, right, test);
			VectorMA(test, y * half_height / 4.0f, up, test);
			if (!CL_SprayPointOnCoplanarSurface(test, normal)) {
				return false;
			}
		}
	}

	return true;
}

// After shifting near an edge, verify the whole perimeter still sits on the
// coplanar surface set before accepting the placement.
static qbool CL_SprayEdgeFits(vec3_t point, vec3_t normal, vec3_t right, vec3_t up, float half_width, float half_height)
{
	int i;

	for (i = -4; i <= 4; ++i) {
		vec3_t test;
		float horizontal_offset = i * half_width / 4.0f;
		float vertical_offset = i * half_height / 4.0f;

		VectorMA(point, -half_width, right, test);
		VectorMA(test, vertical_offset, up, test);
		if (!CL_SprayPointOnCoplanarSurface(test, normal)) {
			return false;
		}

		VectorMA(point, half_width, right, test);
		VectorMA(test, vertical_offset, up, test);
		if (!CL_SprayPointOnCoplanarSurface(test, normal)) {
			return false;
		}

		VectorMA(point, -half_height, up, test);
		VectorMA(test, horizontal_offset, right, test);
		if (!CL_SprayPointOnCoplanarSurface(test, normal)) {
			return false;
		}

		VectorMA(point, half_height, up, test);
		VectorMA(test, horizontal_offset, right, test);
		if (!CL_SprayPointOnCoplanarSurface(test, normal)) {
			return false;
		}
	}

	return true;
}

// Walk from the desired center in one tangent direction until the connected
// coplanar surface ends. This supports clamping placement against nearby edges.
static float CL_SprayMeasureDirection(vec3_t point, vec3_t normal, vec3_t dir, float max_distance)
{
	float distance;
	float last_good = 0;

	for (distance = CL_SPRAY_FIT_STEP; distance <= max_distance; distance += CL_SPRAY_FIT_STEP) {
		vec3_t test;

		VectorMA(point, distance, dir, test);
		if (!CL_SprayPointOnCoplanarSurface(test, normal)) {
			break;
		}
		last_good = distance;
	}

	return last_good;
}

// If the player points near an edge, slide the spray center just enough to fit
// locally. If the local area is truly too small, report failure to the caller.
static qbool CL_SprayShiftIntoBounds(vec3_t point, vec3_t normal, vec3_t right, vec3_t up, float half_width, float half_height)
{
	float left, right_extent, down, up_extent;
	float shift_right = 0;
	float shift_up = 0;
	vec3_t inv_right, inv_up;

	VectorNegate(right, inv_right);
	VectorNegate(up, inv_up);

	left = CL_SprayMeasureDirection(point, normal, inv_right, half_width * 4);
	right_extent = CL_SprayMeasureDirection(point, normal, right, half_width * 4);
	down = CL_SprayMeasureDirection(point, normal, inv_up, half_height * 4);
	up_extent = CL_SprayMeasureDirection(point, normal, up, half_height * 4);

	// If the connected area is smaller than the full decal in either axis,
	// shifting cannot make it fit.
	if (left + right_extent < half_width * 2 || down + up_extent < half_height * 2) {
		return false;
	}

	if (left < half_width) {
		shift_right = half_width - left;
	}
	else if (right_extent < half_width) {
		shift_right = right_extent - half_width;
	}

	if (down < half_height) {
		shift_up = half_height - down;
	}
	else if (up_extent < half_height) {
		shift_up = up_extent - half_height;
	}

	VectorMA(point, shift_right, right, point);
	VectorMA(point, shift_up, up, point);

	// Measurement is axis-aligned, so finish with a perimeter check before
	// trusting the shifted center.
	return CL_SprayEdgeFits(point, normal, right, up, half_width, half_height);
}

// The trace hits collision hulls, but decals must attach to visible world
// geometry. Find the render face under the hit point whose plane and polygon
// match the trace result.
static msurface_t *CL_SprayFindSurface(vec3_t point, vec3_t trace_normal)
{
	int i;
	msurface_t *best = NULL;
	float best_dist = CL_SPRAY_SURFACE_EPSILON;

	if (!cl.worldmodel) {
		return NULL;
	}

	for (i = cl.worldmodel->firstmodelsurface; i < cl.worldmodel->firstmodelsurface + cl.worldmodel->nummodelsurfaces; ++i) {
		msurface_t *surf = &cl.worldmodel->surfaces[i];
		vec3_t surf_normal;
		float dist;

		if ((surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWSPRITE)) || !surf->texinfo) {
			continue;
		}

		CL_SpraySurfaceNormal(surf, surf_normal);
		if (DotProduct(surf_normal, trace_normal) < CL_SPRAY_PLANE_EPSILON) {
			continue;
		}

		dist = fabs(DotProduct(point, surf_normal) - (surf->flags & SURF_PLANEBACK ? -surf->plane->dist : surf->plane->dist));
		if (dist > best_dist) {
			continue;
		}

		if (!CL_SprayPointOnSurface(surf, point)) {
			continue;
		}

		best = surf;
		best_dist = dist;
	}

	return best;
}

// Shared placement routine for immediate spray, preview, and release-to-commit.
// It computes a complete cl_spray_t but does not insert it into cl_sprays[].
static qbool CL_SprayCanUse(qbool quiet)
{
	if (cls.state != ca_active || !cl.worldmodel) {
		if (!quiet) {
			Com_Printf("spray: not in a map\n");
			CL_SprayPlayRejectSound();
		}
		return false;
	}

	if (cl.spectator) {
		if (!quiet) {
			Com_Printf("spectators cannot spray\n");
			CL_SprayPlayRejectSound();
		}
		return false;
	}

	return true;
}

static qbool CL_SprayCreateAtCrosshair(cl_spray_t *spray, qbool quiet)
{
	int texture_index;
	texture_ref texture;
	vec3_t forward, end;
	trace_t trace;
	msurface_t *surf;
	vec3_t normal, right, up;
	vec3_t spray_center;
	int image_width, image_height;
	float size = bound(1, cl_spray_size.value, 512);
	float image_scale;
	float half_width, half_height;

	if (!CL_SprayCanUse(quiet)) {
		return false;
	}

	texture = CL_SprayTextureForRenderer(&texture_index, &image_width, &image_height, quiet);
	if (!R_TextureReferenceIsValid(texture)) {
		return false;
	}
	image_scale = size / max(image_width, image_height);
	half_width = image_width * image_scale * 0.5f;
	half_height = image_height * image_scale * 0.5f;

	AngleVectors(r_refdef.viewangles, forward, NULL, NULL);
	VectorMA(r_refdef.vieworg, bound(64, cl_spray_distance.value, 2048), forward, end);
	trace = PM_TraceLine(r_refdef.vieworg, end);
	if (trace.fraction == 1 || trace.e.entnum != 0) {
		if (!quiet) {
			Com_Printf("spray: no flat map surface under crosshair\n");
			CL_SprayPlayRejectSound();
		}
		return false;
	}

	surf = CL_SprayFindSurface(trace.endpos, trace.plane.normal);
	if (!surf) {
		if (!quiet) {
			Com_Printf("spray: no flat map surface under crosshair\n");
			CL_SprayPlayRejectSound();
		}
		return false;
	}
	CL_SpraySurfaceNormal(surf, normal);

	CL_SprayViewAxis(normal, right, up);

	// The trace point is the desired center. If it is near an edge, clamp the
	// center inward and then re-run the full footprint test.
	VectorCopy(trace.endpos, spray_center);
	if (!CL_SprayPointFits(spray_center, normal, right, up, half_width, half_height)) {
		CL_SprayShiftIntoBounds(spray_center, normal, right, up, half_width, half_height);
	}
	if (!CL_SprayPointFits(spray_center, normal, right, up, half_width, half_height)) {
		if (!quiet) {
			Com_Printf("spray: surface is too small\n");
			CL_SprayPlayRejectSound();
		}
		return false;
	}

	spray->active = true;
	spray->texture = texture;
	spray->texture_index = texture_index;

	// Lift the quad slightly off the surface to avoid z-fighting with the map.
	VectorMA(spray_center, 0.5f, normal, spray->origin);
	VectorCopy(right, spray->right);
	VectorCopy(up, spray->up);
	spray->half_width = half_width;
	spray->half_height = half_height;
	spray->alpha = CL_SPRAY_PLACEMENT_ALPHA;

	return true;
}

// Keep the feature bounded without treating every placement as a blind ring
// write. Server clear messages leave holes; reusing them prevents one player's
// high spray churn from overwriting unrelated active sprays locally.
static int CL_SprayCommit(cl_spray_t *spray)
{
	int i, index;
	cl_spray_t *placed;

	if (!spray->active) {
		return -1;
	}

	index = -1;

	// Authoritative server ids should occupy at most one visible client slot.
	// This also protects against duplicate metadata/payload delivery.
	if (spray->upload_id > 0) {
		for (i = 0; i < CL_MAX_SPRAYS; ++i) {
			if (cl_sprays[i].active && cl_sprays[i].upload_id == spray->upload_id) {
				index = i;
				break;
			}
		}
	}

	if (index < 0) {
		for (i = 0; i < CL_MAX_SPRAYS; ++i) {
			if (!cl_sprays[i].active) {
				index = i;
				break;
			}
		}
	}

	if (index < 0) {
		index = cl_spray_next++ % CL_MAX_SPRAYS;
	}
	placed = &cl_sprays[index];
	*placed = *spray;
	cl_spray_worldmodel = cl.worldmodel;
	return index;
}

static void CL_SprayCancelUpload(void)
{
	if (cl_spray_upload.pixels) {
		Q_free(cl_spray_upload.pixels);
	}
	memset(&cl_spray_upload, 0, sizeof(cl_spray_upload));
}

static void CL_SprayRejectUpload(int id)
{
	if (cl_spray_upload.active && cl_spray_upload.id == id) {
		int placed_index = cl_spray_upload.placed_index;

		// The local spray was optimistic; server denial means it must vanish
		// instead of becoming a client-only decal in a networked match.
		if (placed_index >= 0 && placed_index < CL_MAX_SPRAYS && cl_sprays[placed_index].upload_id == id) {
			memset(&cl_sprays[placed_index], 0, sizeof(cl_sprays[placed_index]));
		}
		CL_SprayCancelUpload();
		Com_Printf("spray: server denied spray\n");
		CL_SprayPlayRejectSound();
	}
}

static sfx_t *CL_SprayResolveSound(cvar_t *cvar, sfx_t **cache, char cache_name[MAX_QPATH])
{
	const char *sound = cvar->string;

	if (!sound[0]) {
		return NULL;
	}

	// Cache the configured sound lazily so changing the cvar takes effect
	// without precaching every possible user-provided sample at startup.
	if (!*cache || strcmp(cache_name, sound)) {
		strlcpy(cache_name, sound, MAX_QPATH);
		*cache = S_PrecacheSound((char *)sound);
	}
	return *cache;
}

static void CL_SprayPlayPlacementSound(vec3_t origin)
{
	sfx_t *sfx = CL_SprayResolveSound(&cl_spray_sound, &cl_spray_sfx, cl_spray_sfx_name);

	if (sfx) {
		S_StartSound(-1, 0, sfx, origin, 3, 1);
	}
}

static void CL_SprayPlayRejectSound(void)
{
	sfx_t *sfx = CL_SprayResolveSound(&cl_spray_reject_sound, &cl_spray_reject_sfx, cl_spray_reject_sfx_name);

	if (sfx) {
		S_StartSound(cl.playernum + 1, -1, sfx, vec3_origin, 1, 0);
	}
}

static void CL_SprayClearServerId(int id)
{
	int i;

	for (i = 0; i < CL_MAX_SPRAYS; ++i) {
		// upload_id becomes the authoritative server spray id after accept.
		if (cl_sprays[i].active && cl_sprays[i].upload_id == id) {
			memset(&cl_sprays[i], 0, sizeof(cl_sprays[i]));
		}
	}
}

// Queue the current local spray image for server relay. This sends raw RGBA
// pixels only; the server never sees the source filename and never reads a file.
static void CL_SprayQueueUpload(int placed_index)
{
	int width, height, byte_count;
	char filename[MAX_QPATH];
	byte *pixels;
	cl_spray_t *spray;

	if (placed_index < 0 || placed_index >= CL_MAX_SPRAYS) {
		return;
	}
	spray = &cl_sprays[placed_index];
	if (cls.state != ca_active || cls.demoplayback || !spray->active) {
		return;
	}

	if (!(cls.mvdprotocolextensions1 & MVD_PEXT1_SPRAYS)) {
		if (!cl_spray_warning) {
			Com_Printf("WARNING: spray decals not supported on this server. Only you can see them.\n");
			cl_spray_warning = true;
		}
		CL_SprayPlayPlacementSound(spray->origin);
		return;
	}

	if (cl_spray_upload.active) {
		Com_Printf("spray: network upload already in progress\n");
		CL_SprayPlayRejectSound();
		memset(spray, 0, sizeof(*spray));
		return;
	}

	if (!CL_SprayImagePath(filename, sizeof(filename))) {
		memset(spray, 0, sizeof(*spray));
		return;
	}

	{
		cl_spray_texture_t *cached = CL_SprayTextureCacheForName(filename);

		if (!cached || !cached->pixels || cached->byte_count <= 0) {
			// This should be rare because placement already resolved the same
			// image. Fall back to the loader if upload is asked to run alone.
			pixels = CL_SprayLoadNormalizedPixels(filename, &width, &height, &byte_count, false);
		}
		else {
			width = cached->width;
			height = cached->height;
			byte_count = cached->byte_count;
			pixels = (byte *)Q_malloc(byte_count);
			memcpy(pixels, cached->pixels, byte_count);
			if (cl_spray_debug.value) {
				Con_Printf("spray debug: image upload cache hit name=\"%s\" size=%dx%d bytes=%d\n",
						filename, width, height, byte_count);
			}
		}
	}
	if (!pixels) {
		memset(spray, 0, sizeof(*spray));
		return;
	}

	memset(&cl_spray_upload, 0, sizeof(cl_spray_upload));
	cl_spray_upload.active = true;
	// The client id is only for matching accept/deny to this pending upload.
	// The server returns a separate authoritative id on accept.
	cl_spray_upload.id = cl_spray_upload_next_id++;
	if (cl_spray_upload_next_id > 255) {
		cl_spray_upload_next_id = 1;
	}
	cl_spray_upload.placed_index = placed_index;
	cl_spray_upload.pixels = pixels;
	cl_spray_upload.width = width;
	cl_spray_upload.height = height;
	cl_spray_upload.byte_count = byte_count;

	// Hash the normalized payload plus dimensions. Placement values like size
	// and alpha are excluded so one cached image can be reused many times.
	{
		cl_spray_texture_t *cached = CL_SprayTextureCacheForName(filename);

		if (cached && cached->pixels) {
			if (!cached->have_hash) {
				Com_Printf("spray: internal cache error, image has no hash\n");
				CL_SprayPlayRejectSound();
				Q_free(pixels);
				memset(&cl_spray_upload, 0, sizeof(cl_spray_upload));
				memset(spray, 0, sizeof(*spray));
				return;
			}
			cl_spray_upload.hash = cached->hash;
		}
		else {
			cl_spray_upload.hash = CL_SprayHashBytes(pixels, width, height, cl_spray_upload.byte_count);
		}
	}
	spray->upload_id = cl_spray_upload.id;
	VectorCopy(spray->origin, cl_spray_upload.origin);
	VectorCopy(spray->right, cl_spray_upload.right);
	VectorCopy(spray->up, cl_spray_upload.up);
	cl_spray_upload.half_width = spray->half_width;
	cl_spray_upload.half_height = spray->half_height;

	// Alpha travels with the placement metadata and is applied by receivers at
	// draw time. It never mutates pixels or changes the image hash.
	cl_spray_upload.alpha = spray->alpha;
}

// One-shot command: optionally change image, then immediately place a decal.
static void CL_Spray_f(void)
{
	cl_spray_t spray;
	int placed_index;

	if (!CL_SprayCanUse(false)) {
		return;
	}

	if (Cmd_Argc() > 1) {
		char filename[MAX_QPATH];

		strlcpy(filename, COM_SkipPath(Cmd_Argv(1)), sizeof(filename));
		Cvar_Set(&cl_spray_image, filename);
	}

	memset(&spray, 0, sizeof(spray));
	if (CL_SprayCreateAtCrosshair(&spray, false)) {
		placed_index = CL_SprayCommit(&spray);
		CL_SprayQueueUpload(placed_index);
	}
}

// Show preview of spray position on keydown
static void CL_SprayDown_f(void)
{
	int texture_index;

	if (!CL_SprayCanUse(false)) {
		cl_spray_preview_active = false;
		return;
	}

	cl_spray_preview_active = R_TextureReferenceIsValid(CL_SprayTextureForRenderer(&texture_index, NULL, NULL, false));
}

// Recompute placement and commit spray on release
static void CL_SprayUp_f(void)
{
	cl_spray_t spray;
	int placed_index;

	if (!cl_spray_preview_active) {
		return;
	}

	memset(&spray, 0, sizeof(spray));
	if (CL_SprayCreateAtCrosshair(&spray, false)) {
		placed_index = CL_SprayCommit(&spray);
		CL_SprayQueueUpload(placed_index);
	}
	cl_spray_preview_active = false;
}

// Append a spray quad to the shared 3D sprite batch. The batch is configured
// for per-entry textures, so each placed spray can keep its original image.
static void CL_SprayAddToScene(cl_spray_t *spray, byte color[4])
{
	r_sprite3d_vert_t *vert;
	vec3_t points[4];
	byte spray_color[4];
	float alpha;

	if (!spray->active || !R_TextureReferenceIsValid(spray->texture)) {
		return;
	}

	// The decal batch was initialized with a null texture, which tells the
	// sprite renderer to use this entry-specific texture reference.
	vert = R_Sprite3DAddEntrySpecific(SPRITE3D_DECALS, 4, spray->texture, spray->texture_index);
	if (!vert) {
		return;
	}

	memcpy(spray_color, color, sizeof(spray_color));
	alpha = bound(0, spray->alpha, 1) * bound(0, cl_spray_show.value, 1);
	if (alpha <= 0) {
		return;
	}

	// Decals use premultiplied-alpha blending. Modulating only the alpha byte
	// leaves the RGB contribution at full strength, so scale the whole vertex
	// color by both the placement opacity and the local display multiplier.
	spray_color[0] = bound(0, (int)(spray_color[0] * alpha), 255);
	spray_color[1] = bound(0, (int)(spray_color[1] * alpha), 255);
	spray_color[2] = bound(0, (int)(spray_color[2] * alpha), 255);
	spray_color[3] = bound(0, (int)(255.0f * alpha), 255);

	VectorMA(spray->origin, -spray->half_height, spray->up, points[0]);
	VectorMA(points[0], -spray->half_width, spray->right, points[0]);
	VectorMA(spray->origin, spray->half_height, spray->up, points[1]);
	VectorMA(points[1], -spray->half_width, spray->right, points[1]);
	VectorMA(spray->origin, -spray->half_height, spray->up, points[2]);
	VectorMA(points[2], spray->half_width, spray->right, points[2]);
	VectorMA(spray->origin, spray->half_height, spray->up, points[3]);
	VectorMA(points[3], spray->half_width, spray->right, points[3]);

	// Triangle strip order is bottom-left, top-left, bottom-right, top-right in
	// the spray's local right/up basis.
	// V is flipped to match image-space orientation after projecting the quad
	// into world space.
	R_Sprite3DSetVert(vert++, points[0][0], points[0][1], points[0][2], 0, 1, spray_color, spray->texture_index);
	R_Sprite3DSetVert(vert++, points[1][0], points[1][1], points[1][2], 0, 0, spray_color, spray->texture_index);
	R_Sprite3DSetVert(vert++, points[2][0], points[2][1], points[2][2], 1, 1, spray_color, spray->texture_index);
	R_Sprite3DSetVert(vert++, points[3][0], points[3][1], points[3][2], 1, 0, spray_color, spray->texture_index);
}

void CL_SpraysUploadNext(void)
{
	int needed;
	int chunk;
	int budget;

	if (!cl_spray_upload.active) {
		return;
	}
	if (cls.state < ca_onserver) {
		CL_SprayCancelUpload();
		return;
	}

	if (!cl_spray_upload.sent_begin) {
		needed = 1 + 1 + 2 + spraynet_hash_bytes + 2 + 2 + 4 + 12 * 4;
		if (!CL_SprayReliableCanWrite(needed)) {
			return;
		}

		// First send only metadata/placement. The server can deny here without
		// making us spend reliable bandwidth on the RGBA payload.
		MSG_WriteByte(&cls.netchan.message, clc_spray);
		MSG_WriteByte(&cls.netchan.message, spraynet_begin);
		MSG_WriteShort(&cls.netchan.message, cl_spray_upload.id);
		CL_SprayMSGWriteHash(&cls.netchan.message, cl_spray_upload.hash);
		MSG_WriteShort(&cls.netchan.message, cl_spray_upload.width);
		MSG_WriteShort(&cls.netchan.message, cl_spray_upload.height);
		MSG_WriteLong(&cls.netchan.message, cl_spray_upload.byte_count);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.origin[0]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.origin[1]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.origin[2]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.right[0]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.right[1]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.right[2]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.up[0]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.up[1]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.up[2]);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.half_width);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.half_height);
		MSG_WriteFloat(&cls.netchan.message, cl_spray_upload.alpha);
		cl_spray_upload.sent_begin = true;
		CL_SprayDebugHash("send metadata",
				cl_spray_upload.id,
				cl_spray_upload.hash,
				va("size=%dx%d bytes=%d", cl_spray_upload.width, cl_spray_upload.height, cl_spray_upload.byte_count));
		return;
	}

	if (!cl_spray_upload.accepted) {
		// Wait for spraynet_accept before sending any raw pixels.
		return;
	}
	if (!cl_spray_upload.send_pixels) {
		CL_SprayDebugHash("send complete",
				cl_spray_upload.id,
				cl_spray_upload.hash,
				"metadata-only");
		CL_SprayCancelUpload();
		return;
	}

	budget = CL_SPRAY_UPLOAD_CHUNKS_PER_FRAME;
	while (budget-- > 0 && cl_spray_upload.offset < cl_spray_upload.byte_count) {
		chunk = min(CL_SPRAY_UPLOAD_CHUNK_BYTES, cl_spray_upload.byte_count - cl_spray_upload.offset);
		needed = 1 + 1 + 2 + 4 + 2 + chunk;
		if (!CL_SprayReliableCanWrite(needed)) {
			return;
		}

		MSG_WriteByte(&cls.netchan.message, clc_spray);
		MSG_WriteByte(&cls.netchan.message, spraynet_pixels);
		MSG_WriteShort(&cls.netchan.message, cl_spray_upload.id);
		MSG_WriteLong(&cls.netchan.message, cl_spray_upload.offset);
		MSG_WriteShort(&cls.netchan.message, chunk);
		SZ_Write(&cls.netchan.message, cl_spray_upload.pixels + cl_spray_upload.offset, chunk);
		CL_SprayDebugHash("send payload",
				cl_spray_upload.id,
				cl_spray_upload.hash,
				va("offset=%d len=%d total=%d", cl_spray_upload.offset, chunk, cl_spray_upload.byte_count));
		cl_spray_upload.offset += chunk;

		if (cl_spray_upload.offset == cl_spray_upload.byte_count) {
			// The server now owns distribution. Keep the local decal, but release
			// the upload buffer and state.
			CL_SprayDebugHash("send complete",
					cl_spray_upload.id,
					cl_spray_upload.hash,
					"full-payload");
			CL_SprayCancelUpload();
			return;
		}
	}
}

void CL_SpraysDisconnect(void)
{
	// Clear connection/map state, but keep local image hashes. They describe
	// normalized image bytes, not any particular server's cache state.
	CL_SprayClearPlaced(true);
}

static cl_server_spray_image_t *CL_SprayServerImageForId(int id)
{
	int i;
	cl_server_spray_image_t *first_free = NULL;

	for (i = 0; i < CL_MAX_SERVER_SPRAY_IMAGES; ++i) {
		if (cl_server_spray_images[i].active && cl_server_spray_images[i].id == id) {
			return &cl_server_spray_images[i];
		}
		if (!cl_server_spray_images[i].active && !first_free) {
			first_free = &cl_server_spray_images[i];
		}
	}

	if (!first_free) {
		// This is only a temporary receive buffer. If many incomplete network
		// sprays are in flight, reuse a deterministic slot rather than growing.
		// Completed entries may be evicted here; future placement-only reuse is
		// an optimization, not a correctness requirement.
		first_free = &cl_server_spray_images[abs(id) % CL_MAX_SERVER_SPRAY_IMAGES];
	}

	memset(first_free, 0, sizeof(*first_free));
	first_free->active = true;
	first_free->id = id;
	return first_free;
}

static cl_server_spray_image_t *CL_SprayServerImageForHash(unsigned long long hash)
{
	int i;

	for (i = 0; i < CL_MAX_SERVER_SPRAY_IMAGES; ++i) {
		if (cl_server_spray_images[i].active && cl_server_spray_images[i].complete && cl_server_spray_images[i].hash == hash && R_TextureReferenceIsValid(cl_server_spray_images[i].texture)) {
			return &cl_server_spray_images[i];
		}
	}
	return NULL;
}

static void CL_SprayClearServerImageCache(void)
{
	int i;

	for (i = 0; i < CL_MAX_SERVER_SPRAY_IMAGES; ++i) {
		// During signon, a server spray begin can arrive just before the
		// renderer observes the new worldmodel. Preserve incomplete receives so
		// the following pixel chunks still have their begin state.
		if (cl_server_spray_images[i].active && cl_server_spray_images[i].have_begin && !cl_server_spray_images[i].complete) {
			continue;
		}
		memset(&cl_server_spray_images[i], 0, sizeof(cl_server_spray_images[i]));
	}
}

static void CL_SprayCommitServerTexture(int id, texture_ref texture, vec3_t origin, vec3_t right, vec3_t up, float half_width, float half_height, float alpha, qbool silent)
{
	cl_spray_t spray;

	// Server-originated and local sprays share the same render path after the
	// image has been resolved to a texture reference.
	memset(&spray, 0, sizeof(spray));
	spray.active = true;
	spray.texture = texture;
	spray.texture_index = 0;
	spray.upload_id = id;
	VectorCopy(origin, spray.origin);
	VectorCopy(right, spray.right);
	VectorCopy(up, spray.up);
	spray.half_width = half_width;
	spray.half_height = half_height;
	spray.alpha = alpha;
	CL_SprayCommit(&spray);

	// Existing-map backfill can finish long after the normal signon reaches
	// ca_active, so sound suppression must come from the spray message itself.
	if (!silent) {
		CL_SprayPlayPlacementSound(spray.origin);
	}
}

static void CL_SprayPlaceServerImage(cl_server_spray_image_t *image)
{
	texture_ref texture;

	texture = image->texture;
	if (!R_TextureReferenceIsValid(texture)) {
		texture = CL_SprayTextureFromPixels(image->id, image->pixels, image->width, image->height, image->byte_count, &image->texture_array);
		if (!R_TextureReferenceIsValid(texture)) {
			return;
		}
		image->texture = texture;
	}

	// Once the raw bytes are complete, convert the receive buffer into the same
	// renderable spray representation used by local decals.
	image->complete = true;
	CL_SprayCommitServerTexture(image->id, texture, image->origin, image->right, image->up, image->half_width, image->half_height, image->alpha, image->silent);
}

static void CL_SprayTrackResolvedServerImage(int id, unsigned long long hash, int width, int height, int byte_count, texture_ref texture, qbool silent)
{
	cl_server_spray_image_t *image = CL_SprayServerImageForId(id);

	// The client may already have this image by hash, but the server might not
	// know that yet. Keep a completed entry for this id so any redundant pixel
	// chunks can be consumed without being mistaken for an out-of-order upload.
	image->active = true;
	image->have_begin = true;
	image->silent = silent;
	image->complete = true;
	image->id = id;
	image->hash = hash;
	image->width = width;
	image->height = height;
	image->byte_count = byte_count;
	image->received = 0;
	image->texture = texture;
	image->texture_array = null_texture_reference;
}

void CL_SpraysParseServerMessage(qbool demo_tolerant)
{
	int sub = MSG_ReadByte();
	int id, width, height, byte_count;
	cl_server_spray_image_t *image;

	if (sub == spraynet_begin || sub == spraynet_begin_silent) {
		unsigned long long hash;
		vec3_t origin, right, up;
		float half_width, half_height;
		float alpha;
		cl_server_spray_image_t *cached;
		cl_spray_texture_t *local_cached;
		qbool silent = (sub == spraynet_begin_silent);

		id = MSG_ReadShort();
		hash = CL_SprayMSGReadHash();
		width = MSG_ReadShort();
		height = MSG_ReadShort();
		byte_count = MSG_ReadLong();

		if (!CL_SprayValidImageSize(width, height, byte_count)) {
			Host_Error("CL_SpraysParseServerMessage: bad spray image dimensions");
		}

		// Placement arrives once, before pixel chunks. Pixels may span many
		// network messages, so keep the target quad with the receive buffer.
		origin[0] = MSG_ReadFloat();
		origin[1] = MSG_ReadFloat();
		origin[2] = MSG_ReadFloat();
		right[0] = MSG_ReadFloat();
		right[1] = MSG_ReadFloat();
		right[2] = MSG_ReadFloat();
		up[0] = MSG_ReadFloat();
		up[1] = MSG_ReadFloat();
		up[2] = MSG_ReadFloat();
		half_width = MSG_ReadFloat();
		half_height = MSG_ReadFloat();
		alpha = MSG_ReadFloat();

		if (half_width <= 0 || half_height <= 0 || half_width > 4096 || half_height > 4096) {
			Host_Error("CL_SpraysParseServerMessage: bad spray size");
		}

		if (alpha < 0 || alpha > 1) {
			Host_Error("CL_SpraysParseServerMessage: bad spray alpha");
		}

		// If this exact RGBA image was already received, the server can send
		// only a new placement. The hash is an index, not an auth boundary.
		cached = CL_SprayServerImageForHash(hash);
		if (cached) {
			CL_SprayDebugHash("recv metadata",
					id,
					hash,
					va("size=%dx%d bytes=%d cached=server-image payload=metadata-only", width, height, byte_count));
			CL_SprayTrackResolvedServerImage(id, hash, width, height, byte_count, cached->texture, silent);
			CL_SprayCommitServerTexture(id, cached->texture, origin, right, up, half_width, half_height, alpha, silent);
			return;
		}
		local_cached = CL_SprayTextureCacheForHash(hash);
		if (local_cached) {
			int texture_index;
			texture_ref texture = CL_SprayCacheTexture(local_cached, &texture_index, true);

			if (R_TextureReferenceIsValid(texture)) {
				// This is usually the original sender receiving an authoritative
				// placement for an image it loaded locally.
				CL_SprayDebugHash("recv metadata",
						id,
						hash,
						va("size=%dx%d bytes=%d cached=local-image payload=metadata-only", width, height, byte_count));
				CL_SprayTrackResolvedServerImage(id, hash, width, height, byte_count, texture, silent);
				CL_SprayCommitServerTexture(id, texture, origin, right, up, half_width, half_height, alpha, silent);
				return;
			}
		}

		CL_SprayDebugHash("recv metadata",
				id,
				hash,
				va("size=%dx%d bytes=%d cached=no payload=full-payload", width, height, byte_count));
		image = CL_SprayServerImageForId(id);
		image->received = 0;
		memset(image->received_blocks, 0, sizeof(image->received_blocks));
		image->have_begin = true;
		image->silent = silent;
		image->complete = false;
		image->hash = hash;
		image->width = width;
		image->height = height;
		image->byte_count = byte_count;
		image->texture = null_texture_reference;
		image->texture_array = null_texture_reference;
	VectorCopy(origin, image->origin);
	VectorCopy(right, image->right);
	VectorCopy(up, image->up);
	image->half_width = half_width;
	image->half_height = half_height;
	image->alpha = alpha;
	return;
	}

	if (sub == spraynet_pixels) {
		int offset, len;
		byte discarded[spraynet_chunk_bytes];

		id = MSG_ReadShort();
		offset = MSG_ReadLong();
		len = MSG_ReadShort();
		image = CL_SprayServerImageForId(id);

		if (image->complete) {
			if (!image->have_begin || offset < 0 || len < 0 || len > (int)sizeof(discarded) || offset + len > image->byte_count) {
				Host_Error("CL_SpraysParseServerMessage: bad redundant spray chunk id=%d offset=%d expected=%d len=%d have_begin=%d", id, offset, image->received, len, image->have_begin);
			}
			MSG_ReadData(discarded, len);
			CL_SprayDebugHash("recv redundant-payload",
					id,
					image->hash,
					va("offset=%d len=%d cached=server-image", offset, len));
			image->received = offset + len;
			if (image->received == image->byte_count) {
				image->received = 0;
			}
			return;
		}

		// Live reliable chunks are expected in order. Demo/QTV hidden streams
		// may expose non-contiguous hidden blocks, so the demo path stores valid
		// later chunks and waits until the byte stream is complete.
		if (!image->have_begin || offset != image->received || offset < 0 || len < 0 || offset + len > image->byte_count) {
			if (demo_tolerant && offset >= 0 && len >= 0 && len <= (int)sizeof(discarded)
				&& (!image->have_begin || offset + len <= image->byte_count)) {
				if (!image->have_begin) {
					MSG_ReadData(discarded, len);
					CL_SprayDebugHash("recv skipped-payload",
							id,
							image->hash,
							va("offset=%d expected=%d len=%d have_begin=%d", offset, image->received, len, image->have_begin));
					return;
				}
				MSG_ReadData(image->pixels + offset, len);
				CL_SprayMarkReceivedBytes(image, offset, len);
				CL_SprayUpdateContiguousReceive(image);
				CL_SprayDebugHash("recv out-of-order-payload",
						id,
						image->hash,
						va("offset=%d expected=%d len=%d have_begin=%d", offset, image->received, len, image->have_begin));
				if (image->received == image->byte_count) {
					if (CL_SprayHashBytes(image->pixels, image->width, image->height, image->byte_count) != image->hash) {
						Host_Error("CL_SpraysParseServerMessage: spray hash mismatch");
					}
					CL_SprayDebugHash("recv complete",
							id,
							image->hash,
							"full-payload");
					CL_SprayPlaceServerImage(image);
				}
				return;
			}
			Host_Error("CL_SpraysParseServerMessage: bad spray chunk id=%d offset=%d expected=%d len=%d have_begin=%d", id, offset, image->received, len, image->have_begin);
		}
		MSG_ReadData(image->pixels + offset, len);
		CL_SprayMarkReceivedBytes(image, offset, len);
		CL_SprayDebugHash("recv payload",
				id,
				image->hash,
				va("offset=%d len=%d total=%d", offset, len, image->byte_count));
		image->received = offset + len;
		CL_SprayUpdateContiguousReceive(image);

		if (image->received == image->byte_count) {
			if (CL_SprayHashBytes(image->pixels, image->width, image->height, image->byte_count) != image->hash) {
				Host_Error("CL_SpraysParseServerMessage: spray hash mismatch");
			}
			CL_SprayDebugHash("recv complete",
					id,
					image->hash,
					"full-payload");
			CL_SprayPlaceServerImage(image);
		}
		return;
	}

	if (sub == spraynet_accept) {
		int server_id;
		int flags;
		id = MSG_ReadShort();
		server_id = MSG_ReadShort();
		flags = MSG_ReadByte();
		if (cl_spray_upload.active && cl_spray_upload.id == id) {
			int placed_index = cl_spray_upload.placed_index;

			cl_spray_upload.accepted = true;
			cl_spray_upload.send_pixels = (flags & spraynet_accept_need_pixels) != 0;
			CL_SprayDebugHash("recv accept",
					id,
					cl_spray_upload.hash,
					va("server_id=%d payload=%s", server_id, cl_spray_upload.send_pixels ? "full-payload" : "metadata-only"));
			// From this point forward, clear messages refer to the server id,
			// not the short-lived local upload id.
			if (placed_index >= 0 && placed_index < CL_MAX_SPRAYS && cl_sprays[placed_index].upload_id == id) {
				cl_sprays[placed_index].upload_id = server_id;
				CL_SprayPlayPlacementSound(cl_sprays[placed_index].origin);
			}
			if (!cl_spray_upload.send_pixels) {
				CL_SprayDebugHash("send complete",
						id,
						cl_spray_upload.hash,
						"metadata-only");
				CL_SprayCancelUpload();
			}
		}
		return;
	}

	if (sub == spraynet_deny) {
		id = MSG_ReadShort();
		MSG_ReadShort();
		if (cl_spray_upload.active && cl_spray_upload.id == id) {
			CL_SprayDebugHash("recv deny", id, cl_spray_upload.hash, "");
		}
		CL_SprayRejectUpload(id);
		return;
	}

	if (sub == spraynet_clear_all) {
		// Clear-all is a visibility event, not a cache reset. The server keeps
		// its per-client known-hash table across clear-all, so the client must
		// keep received image textures available for future placement-only uses.
		if (cl_spray_debug.value) {
			Con_Printf("spray debug: recv clear-all\n");
		}
		CL_SprayClearPlaced(false);
		return;
	}

	if (sub == spraynet_clear_one) {
		id = MSG_ReadShort();
		if (cl_spray_debug.value) {
			Con_Printf("spray debug: recv clear-one id=%d\n", id);
		}
		CL_SprayClearServerId(id);
		return;
	}

	Host_Error("CL_SpraysParseServerMessage: unknown spray submessage %d", sub);
}

void CL_SpraysAddToScene(void)
{
	int i;
	qbool any_active = false;
	cl_spray_t preview;
	byte color[4] = { 255, 255, 255, 255 };
	byte preview_color[4] = { 255, 255, 255, 255 };

	if (cl_spray_worldmodel != cl.worldmodel) {
		// A worldmodel change means a new map/reload. MVDSV also resets its
		// spray image and known-hash caches there, so drop server image cache.
		CL_SprayClearPlaced(true);
	}

	if (cl_spray_show.value <= 0) {
		return;
	}

	for (i = 0; i < CL_MAX_SPRAYS; ++i) {
		any_active |= cl_sprays[i].active;
	}

	memset(&preview, 0, sizeof(preview));
	if (cl_spray_preview_active) {
		any_active |= CL_SprayCreateAtCrosshair(&preview, true);

		// Preview is relative to the final spray opacity: 0.125 means "show
		// one-eighth of the opacity that would be committed."
		preview.alpha = CL_SPRAY_PLACEMENT_ALPHA * bound(0, cl_spray_preview_alpha.value, 1);
	}

	if (!any_active) {
		return;
	}

	// null texture means the sprite renderer should use each entry's texture.
	R_Sprite3DInitialiseBatch(SPRITE3D_DECALS, r_state_decals, null_texture_reference, 0, r_primitive_triangle_strip);

	for (i = 0; i < CL_MAX_SPRAYS; ++i) {
		CL_SprayAddToScene(&cl_sprays[i], color);
	}
	if (preview.active) {
		CL_SprayAddToScene(&preview, preview_color);
	}
}

void CL_InitSprays(void)
{
	Cmd_AddCommand("spray", CL_Spray_f);
	Cmd_AddCommand("+spray", CL_SprayDown_f);
	Cmd_AddCommand("-spray", CL_SprayUp_f);

	Cvar_SetCurrentGroup(CVAR_GROUP_TEXTURES);
	Cvar_Register(&cl_spray_show);
	Cvar_Register(&cl_spray_debug);
	Cvar_Register(&cl_spray_colorize);
	Cvar_Register(&cl_spray_image);
	Cvar_Register(&cl_spray_image_path);
	Cvar_Register(&cl_spray_distance);
	Cvar_Register(&cl_spray_size);
	Cvar_Register(&cl_spray_preview_alpha);
	Cvar_Register(&cl_spray_sound);
	Cvar_Register(&cl_spray_reject_sound);
	Cvar_ResetCurrentGroup();
}
