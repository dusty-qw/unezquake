/*
Copyright (C) 2026 unezQuake team

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

// Chat emoticons: replaces ":)", "^^", ":fire:", ":br:" ... in chat messages with small
// images. The images are drawn like the tracker weapon images: as glyphs of private-use
// charset pages, so they go through the normal console/notify text path.
//
// The default emoticons.pk3 is embedded as a fallback. Installed files override it:
//   - textures/emoticons/emoticons.json (emoticons.pk3), same layout as the scoreboard flags
//   - the scoreboard login flags (scoreboard_flags.pk3), as ":xx:" with the country code
//
// cl_emoticons 0 leaves the text untouched, which is also the fallback when nothing is loaded.

#include "quakedef.h"
#include <jansson.h>
#include "r_texture.h"
#include "r_renderer.h"

cvar_t cl_emoticons = {"cl_emoticons", "0"};

#define EMOTICON_PAGES			2
#define EMOTICON_MAX_CODE		24
#define EMOTICON_MAX_ALIAS_LEN	8
#define EMOTICON_MAX			(EMOTICON_PAGES * 256)
#define EMOTICON_MAX_ALIASES	256
#define EMOTICON_FLAG_SCALE_X	1.5f	// flags are wider than tall, the glyph cell is 8x8

typedef struct {
	char code[EMOTICON_MAX_CODE];
	wchar glyph;
} emoticon_t;

typedef struct {
	char text[EMOTICON_MAX_ALIAS_LEN];
	int length;
	int emoticon;
} emoticon_alias_t;

static emoticon_t emoticons[EMOTICON_MAX];
static int emoticon_count;
static emoticon_alias_t emoticon_aliases[EMOTICON_MAX_ALIASES];
static int emoticon_alias_count;
static int emoticon_next_glyph[EMOTICON_PAGES];	// 0: square images, 1: wide images (flags)

int CL_LoginImageCount(void);
const char *CL_LoginImageName(int index);
const mpic_t *CL_LoginImagePic(int index);
const char *CL_LoginImageFile(int index);

static int CL_EmoticonPage(int kind)
{
	return PRIVATE_USE_EMOTICONS_CHARSET + kind;
}

// Returns the emoticon index, or -1
static int CL_EmoticonAdd(const char *code, const mpic_t *pic, int kind)
{
	charset_t *charset;
	int slot, i;

	if (!code[0] || strlen(code) >= EMOTICON_MAX_CODE || emoticon_count >= EMOTICON_MAX || emoticon_next_glyph[kind] >= 256) {
		return -1;
	}
	for (i = 0; i < emoticon_count; ++i) {
		if (!strcasecmp(emoticons[i].code, code)) {
			return i;	// first one wins
		}
	}

	charset = &char_textures[CL_EmoticonPage(kind)];
	charset->custom_scale_x = (kind ? EMOTICON_FLAG_SCALE_X : 1);
	charset->custom_scale_y = 1;

	slot = emoticon_next_glyph[kind]++;
	charset->glyphs[slot] = *pic;

	strlcpy(emoticons[emoticon_count].code, code, sizeof(emoticons[emoticon_count].code));
	emoticons[emoticon_count].glyph = (wchar)((CL_EmoticonPage(kind) << 8) + slot);
	return emoticon_count++;
}

static void CL_EmoticonAddAlias(const char *text, int emoticon)
{
	size_t len = strlen(text);

	if (!len || len >= EMOTICON_MAX_ALIAS_LEN || emoticon_alias_count >= EMOTICON_MAX_ALIASES) {
		return;
	}
	strlcpy(emoticon_aliases[emoticon_alias_count].text, text, sizeof(emoticon_aliases[0].text));
	emoticon_aliases[emoticon_alias_count].length = (int)len;
	emoticon_aliases[emoticon_alias_count].emoticon = emoticon;
	++emoticon_alias_count;
}

static int CL_EmoticonAliasCompare(const void *a, const void *b)
{
	// longest text first
	return ((const emoticon_alias_t *)b)->length - ((const emoticon_alias_t *)a)->length;
}

static const char *CL_EmoticonJSONString(json_t *json)
{
	return json_is_string(json) ? json_string_value(json) : "";
}

// textures/emoticons/emoticons.json: [{"code": "fire", "file": "fire.png", "aliases": [":)"]}, ...]
static void CL_EmoticonsLoadPack(void)
{
	const char *json_path = "textures/emoticons/emoticons.json";
	json_error_t error;
	json_t *json, *val;
	char *bytes;
	int length;
	size_t i;

	bytes = (char *) FS_LoadHeapFile(json_path, &length);
	if (!bytes) {
		return;
	}
	json = json_loadb(bytes, length, 0, &error);
	Q_free(bytes);
	if (!json || !json_is_array(json)) {
		Com_Printf("Invalid json file %s\n", json_path);
		if (json) {
			json_decref(json);
		}
		return;
	}

	json_array_foreach(json, i, val) {
		const char *code = CL_EmoticonJSONString(json_object_get(val, "code"));
		const char *file = CL_EmoticonJSONString(json_object_get(val, "file"));
		json_t *aliases = json_object_get(val, "aliases");
		char path[MAX_OSPATH];
		texture_ref texture;
		mpic_t pic;
		int index;

		if (!code[0] || !file[0]) {
			continue;
		}

		strlcpy(path, "textures/emoticons/", sizeof(path));
		strlcat(path, file, sizeof(path));
		COM_StripExtension(path, path, sizeof(path));

		texture = R_LoadTextureImage(path, path, 0, 0, TEX_ALPHA | TEX_PREMUL_ALPHA | TEX_NOSCALE);
		if (!R_TextureReferenceIsValid(texture)) {
			continue;
		}
		renderer.TextureSetFiltering(texture, texture_minification_linear, texture_magnification_linear);

		memset(&pic, 0, sizeof(pic));
		pic.texnum = texture;
		pic.width = R_TextureWidth(texture);
		pic.height = R_TextureHeight(texture);
		pic.sl = pic.tl = 0;
		pic.sh = pic.th = 1;

		index = CL_EmoticonAdd(code, &pic, 0);
		if (index >= 0 && json_is_array(aliases)) {
			size_t j;
			json_t *alias;

			json_array_foreach(aliases, j, alias) {
				CL_EmoticonAddAlias(CL_EmoticonJSONString(alias), index);
			}
		}
	}
	json_decref(json);
}

// The scoreboard flags (scr_scoreboard_login_flagfile) are reused as ":br:", ":us:" ...
// The atlas deletes the textures of charset glyphs once they are copied, so each flag is
// loaded again under its own identifier instead of sharing the scoreboard's texture.
static void CL_EmoticonsLoadFlags(void)
{
	int i, count = CL_LoginImageCount();

	for (i = 0; i < count; ++i) {
		const mpic_t *flag = CL_LoginImagePic(i);
		const char *file = CL_LoginImageFile(i);
		texture_ref texture;
		mpic_t pic;

		if (!flag || !file[0]) {
			continue;
		}
		texture = R_LoadTextureImage(file, va("emoticon-flag:%s", file), 0, 0, TEX_ALPHA | TEX_PREMUL_ALPHA | TEX_NOSCALE);
		if (!R_TextureReferenceIsValid(texture)) {
			continue;
		}
		renderer.TextureSetFiltering(texture, texture_minification_linear, texture_magnification_linear);

		pic = *flag;
		pic.texnum = texture;
		CL_EmoticonAdd(CL_LoginImageName(i), &pic, 1);
	}
}

// Called on client and renderer (re)initialisation, textures do not survive a video restart
void CL_Emoticons_LoadImages(void)
{
	int kind;

	emoticon_count = 0;
	emoticon_alias_count = 0;
	for (kind = 0; kind < EMOTICON_PAGES; ++kind) {
		memset(char_textures[CL_EmoticonPage(kind)].glyphs, 0, sizeof(char_textures[0].glyphs));
		emoticon_next_glyph[kind] = 0;
	}

	CL_EmoticonsLoadPack();
	CL_EmoticonsLoadFlags();

	qsort(emoticon_aliases, emoticon_alias_count, sizeof(emoticon_aliases[0]), CL_EmoticonAliasCompare);

	Com_DPrintf("emoticons: %d images, %d text triggers\n", emoticon_count, emoticon_alias_count);
}

// Chat text usually has the high bit set ("white text"), compare it as plain ASCII
static wchar CL_EmoticonBaseChar(wchar c)
{
	return (c >= 0x80 && c <= 0xFF) ? (wchar)(c & 0x7F) : c;
}

static qbool CL_EmoticonIsWordChar(wchar c)
{
	c = CL_EmoticonBaseChar(c);
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// ":code:" -> emoticon index, or -1
static int CL_EmoticonMatchCode(const wchar *s, int *length)
{
	char code[EMOTICON_MAX_CODE];
	int n = 0, i;

	if (CL_EmoticonBaseChar(s[0]) != ':') {
		return -1;
	}
	for (;;) {
		wchar c = CL_EmoticonBaseChar(s[1 + n]);

		if (c == ':') {
			break;
		}
		if (n >= EMOTICON_MAX_CODE - 1 || !(CL_EmoticonIsWordChar(c) || c == '_' || c == '-' || c == '+')) {
			return -1;
		}
		code[n++] = (char) c;
	}
	if (!n) {
		return -1;
	}
	code[n] = 0;

	for (i = 0; i < emoticon_count; ++i) {
		if (!strcasecmp(emoticons[i].code, code)) {
			*length = n + 2;
			return i;
		}
	}
	return -1;
}

// Text triggers such as ":)", longest first
static int CL_EmoticonMatchAlias(const wchar *s, int *length)
{
	int i;

	for (i = 0; i < emoticon_alias_count; ++i) {
		const char *t = emoticon_aliases[i].text;
		int len = 0;

		while (t[len] && CL_EmoticonBaseChar(s[len]) == (wchar)(unsigned char)t[len]) {
			++len;
		}
		if (!t[len]) {
			*length = len;
			return emoticon_aliases[i].emoticon;
		}
	}
	return -1;
}

// Replaces emoticon texts in a chat message. Returns src when nothing changes.
// The result is only valid until the next call.
const wchar *CL_Emoticons_Replace(const wchar *src)
{
	static wchar out[4096];
	size_t o = 0;
	int i;
	qbool colored = false, replaced = false;

	if (!cl_emoticons.integer || !emoticon_count) {
		return src;
	}

	for (i = 0; src[i]; ) {
		int t = -1, length = 0;

		if (src[i] == '&' && src[i + 1] == 'c') {
			colored = true;
		}
		else if (src[i] == '&' && src[i + 1] == 'r') {
			colored = false;
		}

		// Only whole "words": "re:Dave" or "xD3" are not emoticons
		if (i == 0 || !CL_EmoticonIsWordChar(src[i - 1])) {
			t = CL_EmoticonMatchCode(src + i, &length);
			if (t < 0) {
				t = CL_EmoticonMatchAlias(src + i, &length);
			}
			if (t >= 0 && CL_EmoticonIsWordChar(src[i + length])) {
				t = -1;
			}
		}

		if (t >= 0) {
			// Images are tinted by the current text color, go back to white first
			size_t need = colored ? 3 : 1;

			if (o + need + 1 >= sizeof(out) / sizeof(out[0])) {
				return src;
			}
			if (colored) {
				out[o++] = '&';
				out[o++] = 'r';
				colored = false;
			}
			out[o++] = emoticons[t].glyph;
			i += length;
			replaced = true;
		}
		else {
			if (o + 2 >= sizeof(out) / sizeof(out[0])) {
				return src;
			}
			out[o++] = src[i++];
		}
	}
	out[o] = 0;

	return replaced ? out : src;
}

// The text decoder turns "^^" into "^" (FTE escape), so the "^^" smiley never reaches
// CL_Emoticons_Replace. Keep an isolated "^^" intact by doubling it before decoding.
char *CL_Emoticons_PreDecode(char *src, char *dst, size_t size)
{
	size_t o = 0;
	int i;
	qbool changed = false;

	if (!cl_emoticons.integer || !emoticon_count || !strstr(src, "^^")) {
		return src;
	}

	for (i = 0; src[i]; ) {
		if (src[i] == '^' && src[i + 1] == '^'
			&& (i == 0 || !CL_EmoticonIsWordChar((wchar)(unsigned char)src[i - 1]))
			&& !CL_EmoticonIsWordChar((wchar)(unsigned char)src[i + 2])) {
			if (o + 5 >= size) {
				return src;
			}
			dst[o++] = '^';
			dst[o++] = '^';
			dst[o++] = '^';
			dst[o++] = '^';
			i += 2;
			changed = true;
		}
		else {
			if (o + 2 >= size) {
				return src;
			}
			dst[o++] = src[i++];
		}
	}
	dst[o] = 0;

	return changed ? dst : src;
}

void CL_Emoticons_Init(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_CHAT);
	Cvar_Register(&cl_emoticons);
	Cvar_ResetCurrentGroup();
}
