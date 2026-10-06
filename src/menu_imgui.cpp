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
*/

// Main menu built with Dear ImGui.
//
// Layout: a top bar with Quick Play / Servers / Demos / Settings / Quit, and
// below it either nothing (the home screen, which shows the background map
// rendered by menu_scene.c) or the selected page. All engine access goes
// through menu_ui_bridge.h, engine headers are C only.
//
// The menu is drawn after the engine has finished its frame, straight to the
// window's framebuffer. ImGui's OpenGL backends save and restore the GL state
// they touch, so the engine's state caching isn't disturbed.

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_opengl3.h"

#include "menu_ui_bridge.h"

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS 0x84E2
#endif
#ifndef GL_SAMPLER_BINDING
#define GL_SAMPLER_BINDING 0x8919
#endif

namespace {

//=============================================================================
// state
//=============================================================================

typedef void (APIENTRY *glBindFramebuffer_t)(GLenum target, GLuint framebuffer);
typedef void (APIENTRY *glBindBuffer_t)(GLenum target, GLuint buffer);
typedef void (APIENTRY *glUseProgram_t)(GLuint program);
typedef void (APIENTRY *glBindVertexArray_t)(GLuint array);
typedef void (APIENTRY *glActiveTexture_t)(GLenum texture);
typedef void (APIENTRY *glBindSampler_t)(GLuint unit, GLuint sampler);

struct Backend {
	bool initialised = false;
	bool modern = false;           // OpenGL3 backend (GL 3.0+), otherwise OpenGL2 fixed function
	bool was_open = false;
	glBindFramebuffer_t BindFramebuffer = nullptr;
	glBindBuffer_t BindBuffer = nullptr;
	glUseProgram_t UseProgram = nullptr;
	glBindVertexArray_t BindVertexArray = nullptr;
	glActiveTexture_t ActiveTexture = nullptr;
	glActiveTexture_t ClientActiveTexture = nullptr;
	glBindSampler_t BindSampler = nullptr;
	ImFont *font = nullptr;
	float scale = 1.0f;
};

Backend backend;

int current_page = MUI_PAGE_HOME;
mui_client_state_t client;

// server browser cache
struct BrowserCache {
	std::vector<mui_server_t> servers;
	unsigned int revision = 0;
	double last_snapshot = -1000.0;
	bool have_snapshot = false;
	int previous_count = 0;
	mui_browser_status_t status;
};
BrowserCache browser;
std::string selected_server;

// servers page filters
char server_search[128];
bool hide_empty = false;
bool hide_full = false;
bool show_proxies = false;
char add_server_address[128];

// demos
std::vector<mui_file_t> demo_files;
std::string demo_subdir;
bool demo_dir_initialised = false;
bool demo_list_dirty = true;
char demo_search[128];

// settings
int settings_page = 0;
char settings_search[128];
ImGuiID string_edit_id = 0;
char string_edit_buffer[512];
const char *binding_label = "";

// config file popups
enum ConfigPopup { CONFIG_POPUP_NONE, CONFIG_POPUP_IMPORT, CONFIG_POPUP_EXPORT, CONFIG_POPUP_SCRIPT };
ConfigPopup config_popup = CONFIG_POPUP_NONE;
bool config_popup_opening = false;
std::vector<mui_file_t> config_files;
char export_name[128];

//=============================================================================
// look
//=============================================================================

const ImVec4 COLOR_ACCENT        = ImVec4(0.91f, 0.47f, 0.17f, 1.00f);
const ImVec4 COLOR_ACCENT_HOVER  = ImVec4(1.00f, 0.58f, 0.27f, 1.00f);
const ImVec4 COLOR_ACCENT_ACTIVE = ImVec4(0.78f, 0.38f, 0.11f, 1.00f);
const ImVec4 COLOR_TEXT          = ImVec4(0.94f, 0.93f, 0.91f, 1.00f);
const ImVec4 COLOR_TEXT_DIM      = ImVec4(0.60f, 0.61f, 0.64f, 1.00f);
const ImVec4 COLOR_PANEL         = ImVec4(0.06f, 0.065f, 0.075f, 0.90f);
const ImVec4 COLOR_DANGER        = ImVec4(0.86f, 0.25f, 0.22f, 1.00f);
const ImVec4 COLOR_GOOD          = ImVec4(0.40f, 0.80f, 0.42f, 1.00f);

// base font sizes (before menu scaling)
const float FONT_BODY  = 17.0f;
const float FONT_SMALL = 14.5f;
const float FONT_LARGE = 21.0f;
const float FONT_TITLE = 30.0f;
const float FONT_HUGE  = 56.0f;

ImU32 Col(const ImVec4 &c, float alpha_mul = 1.0f)
{
	return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha_mul));
}

void ApplyStyle(float scale)
{
	ImGuiStyle &style = ImGui::GetStyle();

	style = ImGuiStyle();
	ImGui::StyleColorsDark(&style);

	style.WindowPadding = ImVec2(18, 16);
	style.FramePadding = ImVec2(10, 6);
	style.ItemSpacing = ImVec2(10, 8);
	style.ItemInnerSpacing = ImVec2(8, 6);
	style.CellPadding = ImVec2(8, 5);
	style.ScrollbarSize = 12;
	style.GrabMinSize = 12;
	style.WindowRounding = 10;
	style.ChildRounding = 8;
	style.FrameRounding = 6;
	style.PopupRounding = 8;
	style.ScrollbarRounding = 6;
	style.GrabRounding = 6;
	style.TabRounding = 6;
	style.WindowBorderSize = 0;
	style.ChildBorderSize = 1;
	style.PopupBorderSize = 1;
	style.FrameBorderSize = 0;
	style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

	ImVec4 *c = style.Colors;
	c[ImGuiCol_Text]                 = COLOR_TEXT;
	c[ImGuiCol_TextDisabled]         = COLOR_TEXT_DIM;
	c[ImGuiCol_WindowBg]             = COLOR_PANEL;
	c[ImGuiCol_ChildBg]              = ImVec4(1, 1, 1, 0.025f);
	c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.085f, 0.095f, 0.98f);
	c[ImGuiCol_Border]               = ImVec4(1, 1, 1, 0.08f);
	c[ImGuiCol_FrameBg]              = ImVec4(1, 1, 1, 0.06f);
	c[ImGuiCol_FrameBgHovered]       = ImVec4(1, 1, 1, 0.10f);
	c[ImGuiCol_FrameBgActive]        = ImVec4(1, 1, 1, 0.14f);
	c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.085f, 0.095f, 1);
	c[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.105f, 0.12f, 1);
	c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab]        = ImVec4(1, 1, 1, 0.14f);
	c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
	c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1, 1, 1, 0.30f);
	c[ImGuiCol_CheckMark]            = COLOR_ACCENT;
	c[ImGuiCol_SliderGrab]           = COLOR_ACCENT;
	c[ImGuiCol_SliderGrabActive]     = COLOR_ACCENT_HOVER;
	c[ImGuiCol_Button]               = ImVec4(1, 1, 1, 0.07f);
	c[ImGuiCol_ButtonHovered]        = ImVec4(1, 1, 1, 0.13f);
	c[ImGuiCol_ButtonActive]         = ImVec4(1, 1, 1, 0.18f);
	c[ImGuiCol_Header]               = ImVec4(COLOR_ACCENT.x, COLOR_ACCENT.y, COLOR_ACCENT.z, 0.22f);
	c[ImGuiCol_HeaderHovered]        = ImVec4(1, 1, 1, 0.08f);
	c[ImGuiCol_HeaderActive]         = ImVec4(COLOR_ACCENT.x, COLOR_ACCENT.y, COLOR_ACCENT.z, 0.32f);
	c[ImGuiCol_Separator]            = ImVec4(1, 1, 1, 0.08f);
	c[ImGuiCol_TableHeaderBg]        = ImVec4(1, 1, 1, 0.04f);
	c[ImGuiCol_TableBorderStrong]    = ImVec4(1, 1, 1, 0.08f);
	c[ImGuiCol_TableBorderLight]     = ImVec4(1, 1, 1, 0.04f);
	c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.02f);
	c[ImGuiCol_PlotHistogram]        = COLOR_ACCENT;
	c[ImGuiCol_NavCursor]            = COLOR_ACCENT;
	c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.60f);
	c[ImGuiCol_TextSelectedBg]       = ImVec4(COLOR_ACCENT.x, COLOR_ACCENT.y, COLOR_ACCENT.z, 0.35f);

	style.ScaleAllSizes(scale);
	style.FontSizeBase = FONT_BODY;
	style.FontScaleMain = scale;
}

float S(float v)
{
	return v * backend.scale;
}

struct FontSize {
	explicit FontSize(float size) { ImGui::PushFont(nullptr, size); }
	~FontSize() { ImGui::PopFont(); }
};

void TextDim(const char *fmt, ...) IM_FMTARGS(1);
void TextDim(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	ImGui::PushStyleColor(ImGuiCol_Text, COLOR_TEXT_DIM);
	ImGui::TextV(fmt, args);
	ImGui::PopStyleColor();
	va_end(args);
}

ImVec4 PingColor(int ping)
{
	if (ping < 0) {
		return COLOR_TEXT_DIM;
	}
	if (ping < 40) {
		return COLOR_GOOD;
	}
	if (ping < 80) {
		return ImVec4(0.75f, 0.82f, 0.35f, 1);
	}
	if (ping < 140) {
		return ImVec4(0.95f, 0.65f, 0.25f, 1);
	}
	return COLOR_DANGER;
}

bool AccentButton(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, COLOR_ACCENT);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COLOR_ACCENT_HOVER);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, COLOR_ACCENT_ACTIVE);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.03f, 1));
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return pressed;
}

bool DangerButton(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(COLOR_DANGER.x, COLOR_DANGER.y, COLOR_DANGER.z, 0.85f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COLOR_DANGER);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.18f, 0.16f, 1));
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(3);
	return pressed;
}

// small rounded label drawn on a draw list, returns its size
ImVec2 DrawBadge(ImDrawList *dl, ImVec2 pos, const char *text, ImU32 bg, ImU32 fg)
{
	FontSize fs(FONT_SMALL);
	ImVec2 ts = ImGui::CalcTextSize(text);
	ImVec2 pad(S(7), S(3));
	ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);

	dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, S(5));
	dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), fg, text);
	return size;
}

// inline badge as an ImGui item
void Badge(const char *text, const ImVec4 &bg, const ImVec4 &fg)
{
	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImVec2 size = DrawBadge(ImGui::GetWindowDrawList(), pos, text, Col(bg), Col(fg));
	ImGui::Dummy(size);
}

void SectionTitle(const char *title, const char *subtitle = nullptr)
{
	{
		FontSize fs(FONT_TITLE);
		ImGui::TextUnformatted(title);
	}
	if (subtitle) {
		TextDim("%s", subtitle);
	}
}

std::string ToUpper(const char *s)
{
	std::string out(s ? s : "");
	for (char &c : out) {
		c = (char)toupper((unsigned char)c);
	}
	return out;
}

int CompareNoCase(const char *a, const char *b)
{
	for ( ; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) {
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

bool ContainsNoCase(const char *haystack, const char *needle)
{
	if (!needle || !needle[0]) {
		return true;
	}
	if (!haystack) {
		return false;
	}
	for (const char *h = haystack; *h; h++) {
		const char *a = h, *b = needle;
		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
			a++;
			b++;
		}
		if (!*b) {
			return true;
		}
	}
	return false;
}

unsigned int HashString(const char *s)
{
	unsigned int h = 2166136261u;
	for ( ; s && *s; s++) {
		h = (h ^ (unsigned char)*s) * 16777619u;
	}
	return h;
}

// Levelshot if there is one, otherwise a placeholder card in a colour derived from the map name.
void DrawMapPreview(ImDrawList *dl, const char *map, ImVec2 min, ImVec2 max, float rounding, ImDrawFlags corners, bool show_name)
{
	int width = 0, height = 0;
	unsigned int texture = MUI_MapPreviewTexture(map, &width, &height);
	ImVec2 size(max.x - min.x, max.y - min.y);

	if (texture && width > 0 && height > 0 && size.x > 0 && size.y > 0) {
		// crop to fill the box
		float box_aspect = size.x / size.y, tex_aspect = (float)width / height;
		ImVec2 uv0(0, 0), uv1(1, 1);

		if (tex_aspect > box_aspect) {
			float w = box_aspect / tex_aspect;
			uv0.x = (1 - w) * 0.5f;
			uv1.x = 1 - uv0.x;
		}
		else {
			float h = tex_aspect / box_aspect;
			uv0.y = (1 - h) * 0.5f;
			uv1.y = 1 - uv0.y;
		}
		dl->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)texture), min, max, uv0, uv1, IM_COL32_WHITE, rounding, corners);
	}
	else {
		unsigned int h = HashString(map);
		float hue = (h % 360) / 360.0f;
		ImVec4 top, bottom;

		ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.30f, top.x, top.y, top.z);
		ImGui::ColorConvertHSVtoRGB(fmodf(hue + 0.08f, 1.0f), 0.55f, 0.12f, bottom.x, bottom.y, bottom.z);
		top.w = bottom.w = 1.0f;
		dl->AddRectFilled(min, max, Col(bottom), rounding, corners);
		dl->PushClipRect(min, max, true);
		// subtle diagonal stripes as placeholder art
		for (float x = min.x - size.y; x < max.x; x += S(18)) {
			dl->AddLine(ImVec2(x, max.y), ImVec2(x + size.y, min.y), Col(top, 0.35f), S(6));
		}
		dl->PopClipRect();

		if (show_name && map && map[0]) {
			std::string upper = ToUpper(map);
			float font_size = std::min(size.y * 0.30f, S(FONT_TITLE * 1.3f));
			ImVec2 ts = backend.font->CalcTextSizeA(font_size, FLT_MAX, 0, upper.c_str());
			if (ts.x > size.x * 0.9f) {
				font_size *= size.x * 0.9f / ts.x;
				ts = backend.font->CalcTextSizeA(font_size, FLT_MAX, 0, upper.c_str());
			}
			dl->AddText(backend.font, font_size, ImVec2(min.x + (size.x - ts.x) * 0.5f, min.y + (size.y - ts.y) * 0.5f),
				IM_COL32(255, 255, 255, 70), upper.c_str());
		}
	}
}

void ProgressLine(const char *label)
{
	const mui_browser_status_t &st = browser.status;
	char overlay[64];

	snprintf(overlay, sizeof(overlay), "%s %d%%", label, (int)(st.progress * 100));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.05f));
	ImGui::ProgressBar(st.updating_sources ? -1.0f * (float)ImGui::GetTime() : st.progress, ImVec2(S(220), S(22)), st.updating_sources ? "Updating sources" : overlay);
	ImGui::PopStyleColor();
}

//=============================================================================
// server browser data
//=============================================================================

void UpdateBrowser()
{
	MUI_Browser_Status(&browser.status);

	// while refreshing, keep showing what's known and pick up new pings every second
	double interval = browser.status.refreshing ? 1.0 : 3.0;

	if (!browser.have_snapshot || browser.status.revision != browser.revision || client.realtime - browser.last_snapshot > interval) {
		browser.servers.resize(1000);
		int count = MUI_Browser_Snapshot(browser.servers.data(), (int)browser.servers.size());
		if (count >= 0) {
			browser.servers.resize(count);
			browser.revision = browser.status.revision;
			browser.last_snapshot = client.realtime;
			browser.have_snapshot = true;
		}
		else {
			// list is being rewritten: keep the previous snapshot
			browser.servers.resize(browser.previous_count);
		}
		browser.previous_count = (int)browser.servers.size();
	}
}

const mui_server_t *FindServer(const std::string &address)
{
	for (const mui_server_t &s : browser.servers) {
		if (address == s.address) {
			return &s;
		}
	}
	return nullptr;
}

bool ServerIsFull(const mui_server_t &s)
{
	return s.max_players > 0 && s.players >= s.max_players;
}

void FormatPlayers(char *buf, size_t size, const mui_server_t &s)
{
	if (s.max_players > 0) {
		snprintf(buf, size, "%d/%d", s.players, s.max_players);
	}
	else {
		snprintf(buf, size, "%d", s.players);
	}
}

void ServerActionButtons(const mui_server_t &s, bool show_qtv)
{
	if (AccentButton("Join")) {
		MUI_Browser_Join(s.address);
	}
	ImGui::SameLine();
	if (ImGui::Button("Spectate")) {
		MUI_Browser_Observe(s.address);
	}
	if (show_qtv) {
		ImGui::SameLine();
		ImGui::BeginDisabled(!s.has_qtv);
		if (ImGui::Button("Watch QTV")) {
			MUI_Browser_WatchQTV(s.address);
		}
		ImGui::EndDisabled();
		if (!s.has_qtv) {
			ImGui::SetItemTooltip("No QTV stream is listed for this server");
		}
	}
}

// player list with shirt/pants colour chips
void PlayerTable(const mui_server_t &s, const char *id)
{
	if (s.player_count == 0) {
		TextDim("Nobody is playing.");
		return;
	}

	std::vector<const mui_player_t *> players;
	for (int i = 0; i < s.player_count; i++) {
		players.push_back(&s.player[i]);
	}
	std::stable_sort(players.begin(), players.end(), [](const mui_player_t *a, const mui_player_t *b) {
		if (a->spectator != b->spectator) {
			return !a->spectator;
		}
		return a->frags > b->frags;
	});

	if (ImGui::BeginTable(id, 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("Frags", ImGuiTableColumnFlags_WidthFixed, S(46));
		ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, S(40));
		ImGui::TableHeadersRow();

		for (const mui_player_t *p : players) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (!p->spectator) {
				ImDrawList *dl = ImGui::GetWindowDrawList();
				ImVec2 pos = ImGui::GetCursorScreenPos();
				float h = ImGui::GetTextLineHeight();
				unsigned int top = MUI_PaletteColor(p->top_color), bottom = MUI_PaletteColor(p->bottom_color);
				dl->AddRectFilled(pos, ImVec2(pos.x + S(10), pos.y + h * 0.5f), IM_COL32((top >> 16) & 255, (top >> 8) & 255, top & 255, 255), S(2), ImDrawFlags_RoundCornersTop);
				dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.5f), ImVec2(pos.x + S(10), pos.y + h), IM_COL32((bottom >> 16) & 255, (bottom >> 8) & 255, bottom & 255, 255), S(2), ImDrawFlags_RoundCornersBottom);
				ImGui::Dummy(ImVec2(S(10), h));
				ImGui::SameLine();
				ImGui::TextUnformatted(p->name);
			}
			else {
				TextDim("%s", p->name);
				ImGui::SameLine();
				TextDim("(spec)");
			}
			ImGui::TableSetColumnIndex(1);
			TextDim("%s", p->team);
			ImGui::TableSetColumnIndex(2);
			if (!p->spectator) {
				ImGui::Text("%d", p->frags);
			}
			ImGui::TableSetColumnIndex(3);
			ImGui::TextColored(PingColor(p->ping), "%d", p->ping);
		}
		ImGui::EndTable();
	}
}

//=============================================================================
// top bar
//=============================================================================

const struct {
	int page;
	const char *label;
} nav_items[] = {
	{ MUI_PAGE_QUICKPLAY, "QUICK PLAY" },
	{ MUI_PAGE_SERVERS,   "SERVERS" },
	{ MUI_PAGE_DEMOS,     "DEMOS" },
	{ MUI_PAGE_SETTINGS,  "SETTINGS" },
	{ MUI_PAGE_QUIT,      "QUIT" },
};

float TopBarHeight()
{
	return S(68);
}

bool NavItem(const char *label, bool active, bool danger, float height)
{
	FontSize fs(FONT_LARGE);
	ImVec2 ts = ImGui::CalcTextSize(label);
	ImVec2 size(ts.x + S(36), height);
	ImVec2 pos = ImGui::GetCursorScreenPos();

	bool pressed = ImGui::InvisibleButton(label, size);
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *dl = ImGui::GetWindowDrawList();

	if (hovered && !active) {
		dl->AddRectFilled(ImVec2(pos.x, pos.y + S(12)), ImVec2(pos.x + size.x, pos.y + size.y - S(12)), IM_COL32(255, 255, 255, 14), S(6));
	}

	ImVec4 color = active ? COLOR_TEXT : (hovered ? (danger ? COLOR_DANGER : COLOR_TEXT) : COLOR_TEXT_DIM);
	dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), Col(color), label);

	if (active) {
		float w = ts.x * 0.6f;
		float x = pos.x + (size.x - w) * 0.5f;
		dl->AddRectFilled(ImVec2(x, pos.y + size.y - S(5)), ImVec2(x + w, pos.y + size.y - S(2)), Col(COLOR_ACCENT), S(2));
	}

	if (pressed) {
		MUI_PlaySound("misc/menu1.wav");
	}
	return pressed;
}

void DrawLogo(ImDrawList *dl, ImVec2 pos, float height)
{
	// wordmark only
	float font_size = S(FONT_LARGE * 1.15f);
	float x = pos.x;
	ImVec2 unez = backend.font->CalcTextSizeA(font_size, FLT_MAX, 0, "UNEZ");
	float y = pos.y + (height - unez.y) * 0.5f;
	dl->AddText(backend.font, font_size, ImVec2(x, y), Col(COLOR_TEXT_DIM), "UNEZ");
	dl->AddText(backend.font, font_size, ImVec2(x + unez.x, y), Col(COLOR_TEXT), "QUAKE");
}

void SetPage(int page);

void DrawTopBar()
{
	ImGuiIO &io = ImGui::GetIO();
	float height = TopBarHeight();

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, height));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.037f, 0.043f, 0.86f));
	ImGui::Begin("##topbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 wpos = ImGui::GetWindowPos();
	dl->AddLine(ImVec2(wpos.x, wpos.y + height - 1), ImVec2(wpos.x + io.DisplaySize.x, wpos.y + height - 1), IM_COL32(255, 255, 255, 18));

	// logo (click: home)
	ImGui::SetCursorPos(ImVec2(S(24), 0));
	ImVec2 logo_pos = ImGui::GetCursorScreenPos();
	if (ImGui::InvisibleButton("##logo", ImVec2(S(190), height))) {
		SetPage(MUI_PAGE_HOME);
	}
	DrawLogo(dl, logo_pos, height);

	// navigation is right aligned, everything else has to fit left of it
	float nav_width = 0;
	{
		FontSize fs(FONT_LARGE);
		for (const auto &item : nav_items) {
			nav_width += ImGui::CalcTextSize(item.label).x + S(36);
		}
	}
	nav_width += ImGui::GetStyle().ItemSpacing.x * (IM_ARRAYSIZE(nav_items) - 1);
	float nav_x = io.DisplaySize.x - nav_width - S(24);

	// in game: resume / disconnect
	if (client.connected) {
		float button_y = (height - ImGui::GetFrameHeight()) * 0.5f;

		ImGui::SameLine(0, S(18));
		ImGui::SetCursorPosY(button_y);
		if (AccentButton(client.demoplayback ? "Resume demo" : "Resume")) {
			MUI_CloseMenu();
		}
		ImGui::SameLine();
		ImGui::SetCursorPosY(button_y);
		if (ImGui::Button(client.demoplayback ? "Stop demo" : "Disconnect")) {
			MUI_Disconnect();
		}
		ImGui::SameLine(0, S(14));
		char where[256];
		snprintf(where, sizeof(where), client.map[0] ? "%s  %s" : "%s", client.server, client.map);
		ImVec2 text_pos(ImGui::GetCursorScreenPos().x, wpos.y + (height - ImGui::GetTextLineHeight()) * 0.5f);
		ImVec4 clip(text_pos.x, wpos.y, wpos.x + nav_x - S(16), wpos.y + height);
		if (clip.z > clip.x) {
			dl->AddText(backend.font, ImGui::GetFontSize(), text_pos, Col(COLOR_TEXT_DIM), where, nullptr, 0, &clip);
		}
	}

	// navigation
	ImGui::SameLine();
	ImGui::SetCursorPos(ImVec2(std::max(ImGui::GetCursorPosX(), nav_x), 0));
	for (size_t i = 0; i < IM_ARRAYSIZE(nav_items); i++) {
		if (i > 0) {
			ImGui::SameLine();
		}
		if (NavItem(nav_items[i].label, current_page == nav_items[i].page, nav_items[i].page == MUI_PAGE_QUIT, height)) {
			SetPage(current_page == nav_items[i].page ? MUI_PAGE_HOME : nav_items[i].page);
		}
	}

	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

// Begins the panel below the top bar used by every page.
bool BeginPage(const char *id, float max_width = 0)
{
	ImGuiIO &io = ImGui::GetIO();
	float top = TopBarHeight() + S(22);
	float margin = S(28);
	float width = io.DisplaySize.x - margin * 2;

	if (max_width > 0 && width > S(max_width)) {
		width = S(max_width);
	}

	ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - width) * 0.5f, top));
	ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y - top - margin));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(22)));
	bool open = ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ImGui::PopStyleVar();
	return open;
}

void EndPage()
{
	ImGui::End();
}

// Title on the left, refresh status/buttons on the right.
// Right edge of the content area, in window coordinates (call at the start of a line).
float ContentRight()
{
	return ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
}

void BrowserHeader(const char *title, const char *subtitle)
{
	float start_y = ImGui::GetCursorPosY();
	float right = ContentRight();
	SectionTitle(title, subtitle);
	float end_y = ImGui::GetCursorPosY();

	float button_w = S(110);
	ImGui::SetCursorPos(ImVec2(right - button_w, start_y + S(6)));
	if (browser.status.refreshing) {
		ImGui::SetCursorPosX(right - S(220));
		ProgressLine("Pinging");
	}
	else if (ImGui::Button("Refresh", ImVec2(button_w, 0))) {
		MUI_Browser_Refresh(false);
	}
	ImGui::SetCursorPosY(std::max(end_y, ImGui::GetCursorPosY()) + S(6));
}

//=============================================================================
// quick play
//=============================================================================

void QuickPlayCard(const mui_server_t &s, float width, float height)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float preview_h = width * 9.0f / 16.0f;
	float rounding = S(10);
	ImVec2 max(pos.x + width, pos.y + height);

	// A group, so the buttons drawn over the card don't disturb the grid layout. The card
	// itself isn't an item that takes clicks, so presses always go to Play/Spectate.
	ImGui::PushID(s.address);
	ImGui::BeginGroup();
	ImGui::Dummy(ImVec2(width, height));
	// AllowWhenBlockedByActiveItem: while Play/Spectate is held down it is the active item, the
	// buttons must stay up until the click completes
	bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(pos, max);

	dl->AddRectFilled(pos, max, hovered ? IM_COL32(255, 255, 255, 20) : IM_COL32(255, 255, 255, 10), rounding);
	DrawMapPreview(dl, s.map, pos, ImVec2(max.x, pos.y + preview_h), rounding, ImDrawFlags_RoundCornersTop, true);
	dl->AddRectFilledMultiColor(ImVec2(pos.x, pos.y + preview_h * 0.55f), ImVec2(max.x, pos.y + preview_h),
		IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));
	if (hovered) {
		dl->AddRect(pos, max, Col(COLOR_ACCENT, 0.9f), rounding, S(2));
	}

	// badges on the preview
	ImVec2 badge_pos(pos.x + S(10), pos.y + S(10));
	std::string mode = ToUpper(s.mode);
	badge_pos.x += DrawBadge(dl, badge_pos, mode.c_str(), Col(COLOR_ACCENT), IM_COL32(20, 12, 6, 255)).x + S(6);
	if (s.in_progress) {
		DrawBadge(dl, badge_pos, "LIVE", IM_COL32(200, 40, 40, 230), IM_COL32_WHITE);
	}
	char ping[32];
	snprintf(ping, sizeof(ping), "%d ms", s.ping);
	{
		FontSize fs(FONT_SMALL);
		float pw = ImGui::CalcTextSize(ping).x + S(14);
		DrawBadge(dl, ImVec2(max.x - pw - S(10), pos.y + S(10)), ping, IM_COL32(0, 0, 0, 170), Col(PingColor(s.ping)));
	}

	// map name over the bottom of the preview
	{
		std::string map = ToUpper(s.map);
		float fs = S(FONT_LARGE * 1.1f);
		dl->AddText(backend.font, fs, ImVec2(pos.x + S(12), pos.y + preview_h - fs - S(10)), IM_COL32_WHITE, map.c_str());
	}

	// info below the preview
	float y = pos.y + preview_h + S(10);
	{
		float fs = S(FONT_BODY);
		ImVec4 clip(pos.x + S(12), y, max.x - S(12), y + fs * 1.4f);
		dl->AddText(backend.font, fs, ImVec2(pos.x + S(12), y), Col(COLOR_TEXT), s.name, nullptr, 0, &clip);
		y += fs * 1.35f;
	}
	{
		char players[64], line[160];
		FormatPlayers(players, sizeof(players), s);
		snprintf(line, sizeof(line), "%s players%s%s", players, s.status[0] ? "  -  " : "", s.status);
		float fs = S(FONT_SMALL);
		ImVec4 clip(pos.x + S(12), y, max.x - S(12), y + fs * 1.4f);
		dl->AddText(backend.font, fs, ImVec2(pos.x + S(12), y), Col(ServerIsFull(s) ? COLOR_DANGER : COLOR_TEXT_DIM), line, nullptr, 0, &clip);
		y += fs * 1.5f;

		// fill bar
		if (s.max_players > 0) {
			float frac = std::min(1.0f, (float)s.players / s.max_players);
			ImVec2 a(pos.x + S(12), y), b(max.x - S(12), y + S(4));
			dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 25), S(2));
			dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * frac, b.y), Col(ServerIsFull(s) ? COLOR_DANGER : COLOR_ACCENT), S(2));
		}
	}

	// hover actions
	if (hovered) {
		float bw = S(96);
		ImGui::SetCursorScreenPos(ImVec2(max.x - bw * 2 - S(18), pos.y + preview_h - ImGui::GetFrameHeight() - S(10)));
		if (AccentButton("Play", ImVec2(bw, 0))) {
			MUI_Browser_Join(s.address);
		}
		ImGui::SameLine(0, S(6));
		if (ImGui::Button("Spectate", ImVec2(bw, 0))) {
			MUI_Browser_Observe(s.address);
		}
	}
	ImGui::EndGroup();

	// double-click on the card (not on its buttons) joins
	if (hovered && !ImGui::IsAnyItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		MUI_Browser_Join(s.address);
	}
	ImGui::PopID();
}

void DrawQuickPlay()
{
	if (!BeginPage("##quickplay")) {
		EndPage();
		return;
	}

	std::vector<const mui_server_t *> list;
	int players_online = 0;
	for (const mui_server_t &s : browser.servers) {
		if (!s.proxy && s.ping >= 0) {
			players_online += s.players;
		}
		if (!s.proxy && s.ping >= 0 && s.players > 0) {
			list.push_back(&s);
		}
	}
	std::stable_sort(list.begin(), list.end(), [](const mui_server_t *a, const mui_server_t *b) {
		return a->ping < b->ping;
	});

	char subtitle[128];
	snprintf(subtitle, sizeof(subtitle), "%d servers with players  -  %d players online  -  closest first", (int)list.size(), players_online);
	BrowserHeader("Quick Play", subtitle);

	// best match: closest server with room
	const mui_server_t *best = nullptr;
	for (const mui_server_t *s : list) {
		if (!ServerIsFull(*s)) {
			best = s;
			break;
		}
	}
	if (best) {
		char label[160];
		snprintf(label, sizeof(label), "Play now: %s  (%s, %d ms)", best->map, best->mode, best->ping);
		if (AccentButton(label, ImVec2(0, S(36)))) {
			MUI_Browser_Join(best->address);
		}
		ImGui::Spacing();
	}

	ImGui::BeginChild("##cards", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	if (list.empty()) {
		ImGui::Dummy(ImVec2(0, S(40)));
		FontSize fs(FONT_LARGE);
		if (browser.status.refreshing || !browser.have_snapshot) {
			TextDim("Looking for servers...");
		}
		else {
			TextDim("No servers with players right now. Try refreshing, or browse all servers.");
			if (ImGui::Button("Browse servers")) {
				SetPage(MUI_PAGE_SERVERS);
			}
		}
	}
	else {
		float spacing = S(16);
		float avail = ImGui::GetContentRegionAvail().x;
		int columns = std::max(1, (int)((avail + spacing) / (S(300) + spacing)));
		float card_w = (avail - spacing * (columns - 1)) / columns;
		float card_h = card_w * 9.0f / 16.0f + S(78);

		for (size_t i = 0; i < list.size(); i++) {
			if (i % columns != 0) {
				ImGui::SameLine(0, spacing);
			}
			QuickPlayCard(*list[i], card_w, card_h);
			if (i % columns == (size_t)columns - 1) {
				ImGui::Dummy(ImVec2(0, spacing - ImGui::GetStyle().ItemSpacing.y));
			}
		}
	}
	ImGui::EndChild();

	EndPage();
}

//=============================================================================
// servers
//=============================================================================

enum ServerColumn { COL_PING, COL_NAME, COL_MAP, COL_MODE, COL_PLAYERS, COL_ADDRESS };

bool ServerPassesFilter(const mui_server_t &s)
{
	if (s.proxy && !show_proxies) {
		return false;
	}
	if (hide_empty && s.players == 0) {
		return false;
	}
	if (hide_full && ServerIsFull(s)) {
		return false;
	}
	if (server_search[0]) {
		if (ContainsNoCase(s.name, server_search) || ContainsNoCase(s.map, server_search) || ContainsNoCase(s.address, server_search)
			|| ContainsNoCase(s.mode, server_search)) {
			return true;
		}
		for (int i = 0; i < s.player_count; i++) {
			if (ContainsNoCase(s.player[i].name, server_search)) {
				return true;
			}
		}
		return false;
	}
	return true;
}

void ServerDetails(const mui_server_t &s)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x;
	float h = std::min(w * 9.0f / 16.0f, S(220));

	DrawMapPreview(dl, s.map, pos, ImVec2(pos.x + w, pos.y + h), S(8), ImDrawFlags_RoundCornersAll, true);
	ImGui::Dummy(ImVec2(w, h));

	{
		FontSize fs(FONT_LARGE);
		ImGui::TextWrapped("%s", s.name);
	}
	TextDim("%s", s.address);
	ImGui::SameLine();
	if (ImGui::SmallButton("Copy")) {
		MUI_SetClipboard(s.address);
	}

	std::string mode = ToUpper(s.mode);
	Badge(mode.c_str(), COLOR_ACCENT, ImVec4(0.08f, 0.05f, 0.03f, 1));
	ImGui::SameLine();
	ImGui::Text("%s", s.map);
	if (s.status[0]) {
		ImGui::SameLine();
		TextDim("- %s", s.status);
	}

	char players[32];
	FormatPlayers(players, sizeof(players), s);
	TextDim("Players %s   Spectators %d/%d   Ping ", players, s.spectators, s.max_spectators);
	ImGui::SameLine(0, 0);
	ImGui::TextColored(PingColor(s.ping), "%d", s.ping);
	if (s.timelimit || s.fraglimit) {
		TextDim("Timelimit %d   Fraglimit %d", s.timelimit, s.fraglimit);
	}

	ImGui::Spacing();
	ServerActionButtons(s, true);
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	PlayerTable(s, "##details_players");
}

void DrawServers()
{
	if (!BeginPage("##servers")) {
		EndPage();
		return;
	}

	std::vector<const mui_server_t *> list;
	for (const mui_server_t &s : browser.servers) {
		if (ServerPassesFilter(s)) {
			list.push_back(&s);
		}
	}

	char subtitle[96];
	snprintf(subtitle, sizeof(subtitle), "%d of %d servers", (int)list.size(), (int)browser.servers.size());
	BrowserHeader("Servers", subtitle);

	// toolbar
	ImGui::SetNextItemWidth(S(280));
	ImGui::InputTextWithHint("##search", "Search servers, maps, players...", server_search, sizeof(server_search));
	ImGui::SameLine(0, S(18));
	ImGui::Checkbox("Hide empty", &hide_empty);
	ImGui::SameLine();
	ImGui::Checkbox("Hide full", &hide_full);
	ImGui::SameLine();
	ImGui::Checkbox("Proxies", &show_proxies);
	ImGui::SameLine(0, S(18));
	if (ImGui::Button("Add server...")) {
		add_server_address[0] = '\0';
		ImGui::OpenPopup("##addserver");
	}
	if (ImGui::BeginPopup("##addserver")) {
		ImGui::Text("Server address");
		ImGui::SetNextItemWidth(S(260));
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		bool enter = ImGui::InputTextWithHint("##address", "host:port", add_server_address, sizeof(add_server_address), ImGuiInputTextFlags_EnterReturnsTrue);
		if ((AccentButton("Add") || enter) && add_server_address[0]) {
			MUI_Browser_AddServer(add_server_address);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Connect") && add_server_address[0]) {
			MUI_Browser_Join(add_server_address);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::Spacing();

	const mui_server_t *selected = FindServer(selected_server);
	float avail_w = ImGui::GetContentRegionAvail().x;
	bool side_by_side = avail_w > S(900);
	float details_w = side_by_side && selected ? std::max(S(340), avail_w * 0.32f) : 0;
	float table_h = side_by_side ? 0 : (selected ? ImGui::GetContentRegionAvail().y * 0.55f : 0);

	ImGui::BeginChild("##serverlist", ImVec2(details_w > 0 ? avail_w - details_w - S(16) : 0, table_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
		| ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Hideable;
	if (ImGui::BeginTable("##servertable", 6, flags)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, S(56), COL_PING);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0f, COL_NAME);
		ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch, 1.3f, COL_MAP);
		ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, S(64), COL_MODE);
		ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, S(72), COL_PLAYERS);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 1.6f, COL_ADDRESS);
		ImGui::TableHeadersRow();

		if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
			if (specs->SpecsCount > 0) {
				const ImGuiTableColumnSortSpecs spec = specs->Specs[0];
				bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
				std::stable_sort(list.begin(), list.end(), [&spec, asc](const mui_server_t *a, const mui_server_t *b) {
					int cmp = 0;
					switch (spec.ColumnUserID) {
						case COL_PING: {
							// unreachable servers always last
							int pa = a->ping < 0 ? 100000 : a->ping, pb = b->ping < 0 ? 100000 : b->ping;
							if ((a->ping < 0) != (b->ping < 0)) {
								return a->ping >= 0;
							}
							cmp = pa - pb;
							break;
						}
						case COL_NAME:    cmp = CompareNoCase(a->name, b->name); break;
						case COL_MAP:     cmp = CompareNoCase(a->map, b->map); break;
						case COL_MODE:    cmp = CompareNoCase(a->mode, b->mode); break;
						case COL_PLAYERS: cmp = a->players - b->players; break;
						case COL_ADDRESS: cmp = CompareNoCase(a->address, b->address); break;
					}
					return asc ? cmp < 0 : cmp > 0;
				});
			}
		}

		if (list.empty()) {
			ImGui::TableNextRow(ImGuiTableRowFlags_None, S(40));
			ImGui::TableSetColumnIndex(COL_NAME);
			ImGui::AlignTextToFramePadding();
			TextDim(browser.status.refreshing || !browser.have_snapshot ? "Looking for servers..." : "No servers match the filters.");
		}

		ImGuiListClipper clipper;
		clipper.Begin((int)list.size());
		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				const mui_server_t &s = *list[row];
				bool is_selected = selected_server == s.address;
				char players[32];

				ImGui::TableNextRow(ImGuiTableRowFlags_None, S(28));
				ImGui::PushID(s.address);

				ImGui::TableSetColumnIndex(COL_PING);
				ImGui::AlignTextToFramePadding();
				if (ImGui::Selectable("##row", is_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, S(26)))) {
					selected_server = s.address;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						MUI_Browser_Join(s.address);
					}
				}
				if (ImGui::BeginPopupContextItem("##ctx")) {
					selected_server = s.address;
					if (ImGui::MenuItem("Join")) MUI_Browser_Join(s.address);
					if (ImGui::MenuItem("Spectate")) MUI_Browser_Observe(s.address);
					if (ImGui::MenuItem("Watch QTV", nullptr, false, s.has_qtv)) MUI_Browser_WatchQTV(s.address);
					ImGui::Separator();
					if (ImGui::MenuItem("Copy address")) MUI_SetClipboard(s.address);
					ImGui::EndPopup();
				}
				ImGui::SameLine(0, 0);
				if (s.ping < 0) {
					TextDim("--");
				}
				else {
					ImGui::TextColored(PingColor(s.ping), "%d", s.ping);
				}

				ImGui::TableSetColumnIndex(COL_NAME);
				ImGui::AlignTextToFramePadding();
				if (s.players > 0) {
					ImGui::TextUnformatted(s.name);
				}
				else {
					TextDim("%s", s.name);
				}
				ImGui::TableSetColumnIndex(COL_MAP);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(s.map);
				ImGui::TableSetColumnIndex(COL_MODE);
				ImGui::AlignTextToFramePadding();
				TextDim("%s", s.mode);
				ImGui::TableSetColumnIndex(COL_PLAYERS);
				ImGui::AlignTextToFramePadding();
				FormatPlayers(players, sizeof(players), s);
				if (ServerIsFull(s)) {
					ImGui::TextColored(COLOR_DANGER, "%s", players);
				}
				else if (s.players > 0) {
					ImGui::TextColored(COLOR_ACCENT_HOVER, "%s", players);
				}
				else {
					TextDim("%s", players);
				}
				ImGui::TableSetColumnIndex(COL_ADDRESS);
				ImGui::AlignTextToFramePadding();
				TextDim("%s", s.address);

				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::EndChild();

	if (selected) {
		if (side_by_side) {
			ImGui::SameLine(0, S(16));
		}
		ImGui::BeginChild("##serverdetails", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
		ServerDetails(*selected);
		ImGui::EndChild();
	}

	EndPage();
}

//=============================================================================
// watch
//=============================================================================

void RefreshDemos()
{
	if (!demo_dir_initialised) {
		// most installs keep demos in <gamedir>/demos
		std::vector<mui_file_t> probe(1);
		demo_dir_initialised = true;
		if (MUI_Demos_List("demos", probe.data(), 1) > 0) {
			demo_subdir = "demos";
		}
	}

	demo_files.resize(4096);
	int count = MUI_Demos_List(demo_subdir.c_str(), demo_files.data(), (int)demo_files.size());
	demo_files.resize(std::max(0, count));
	std::stable_sort(demo_files.begin(), demo_files.end(), [](const mui_file_t &a, const mui_file_t &b) {
		if (a.is_dir != b.is_dir) {
			return a.is_dir != 0;
		}
		if (a.is_dir) {
			return CompareNoCase(a.name, b.name) < 0;
		}
		return a.time > b.time;
	});
	demo_list_dirty = false;
}

void FormatSize(char *buf, size_t size, int bytes)
{
	if (bytes >= 1024 * 1024) {
		snprintf(buf, size, "%.1f MB", bytes / (1024.0 * 1024.0));
	}
	else {
		snprintf(buf, size, "%d KB", std::max(1, bytes / 1024));
	}
}

void FormatTime(char *buf, size_t size, int unix_time)
{
	time_t t = (time_t)unix_time;
	struct tm *tm = localtime(&t);

	if (!tm || unix_time <= 0) {
		snprintf(buf, size, "-");
		return;
	}
	strftime(buf, size, "%Y-%m-%d %H:%M", tm);
}

void DrawDemos()
{
	if (!BeginPage("##demos_page", 1300)) {
		EndPage();
		return;
	}

	if (demo_list_dirty) {
		RefreshDemos();
	}

	ImGui::BeginChild("##demos", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	{
		float start_y = ImGui::GetCursorPosY();
		float right = ContentRight();
		SectionTitle("Demos", "Select a demo to play it");
		float end_y = ImGui::GetCursorPosY();
		ImGui::SetCursorPos(ImVec2(right - S(110), start_y + S(6)));
		if (ImGui::Button("Refresh", ImVec2(S(110), 0))) {
			demo_list_dirty = true;
		}
		ImGui::SetCursorPosY(end_y + S(6));

		// breadcrumb
		if (!demo_subdir.empty()) {
			if (ImGui::SmallButton("Up")) {
				size_t slash = demo_subdir.find_last_of('/');
				demo_subdir = slash == std::string::npos ? "" : demo_subdir.substr(0, slash);
				demo_list_dirty = true;
			}
			ImGui::SameLine();
		}
		{
			// the root is usually a long absolute path, its last component is enough
			const char *root = MUI_Demos_Root();
			const char *slash = strrchr(root, '/');
			TextDim("%s%s%s", slash && slash[1] ? slash + 1 : root, demo_subdir.empty() ? "" : "/", demo_subdir.c_str());
			ImGui::SetItemTooltip("%s", root);
		}

		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##demosearch", "Filter demos...", demo_search, sizeof(demo_search));
		ImGui::Spacing();

		ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##demotable", 3, flags)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0f);
			ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000-00-00 00:00").x);
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000.0 MB").x);
			ImGui::TableHeadersRow();

			std::string enter_dir;
			for (const mui_file_t &f : demo_files) {
				if (!f.is_dir && !ContainsNoCase(f.name, demo_search)) {
					continue;
				}

				ImGui::TableNextRow(ImGuiTableRowFlags_None, S(26));
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(f.name);
				char label[256];
				snprintf(label, sizeof(label), f.is_dir ? "%s/" : "%s", f.name);
				ImGui::AlignTextToFramePadding();
				if (f.is_dir) {
					ImGui::PushStyleColor(ImGuiCol_Text, COLOR_ACCENT_HOVER);
				}
				if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, S(24)))) {
					if (f.is_dir) {
						enter_dir = f.name;
					}
					else {
						MUI_Demos_Play(demo_subdir.c_str(), f.name);
					}
				}
				if (f.is_dir) {
					ImGui::PopStyleColor();
				}
				ImGui::PopID();

				if (!f.is_dir) {
					char buf[64];
					ImGui::TableSetColumnIndex(1);
					ImGui::AlignTextToFramePadding();
					FormatTime(buf, sizeof(buf), f.time);
					TextDim("%s", buf);
					ImGui::TableSetColumnIndex(2);
					ImGui::AlignTextToFramePadding();
					FormatSize(buf, sizeof(buf), f.size);
					TextDim("%s", buf);
				}
			}
			ImGui::EndTable();

			if (!enter_dir.empty()) {
				demo_subdir = demo_subdir.empty() ? enter_dir : demo_subdir + "/" + enter_dir;
				demo_list_dirty = true;
			}
		}
		if (demo_files.empty()) {
			TextDim("No demos here. Demos you record (or download) into this folder show up here.");
		}
	}
	ImGui::EndChild();

	EndPage();
}

//=============================================================================
// settings
//=============================================================================

const char *NumberFormat(float step)
{
	if (step >= 1.0f || step <= 0.0f) {
		return "%.0f";
	}
	if (step >= 0.1f) {
		return "%.1f";
	}
	return "%.2f";
}

bool ToggleSwitch(const char *id, bool *value)
{
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float h = ImGui::GetFrameHeight() * 0.8f;
	float w = h * 1.9f;
	ImDrawList *dl = ImGui::GetWindowDrawList();

	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + (ImGui::GetFrameHeight() - h) * 0.5f));
	bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
	if (pressed) {
		*value = !*value;
	}
	bool hovered = ImGui::IsItemHovered();
	ImVec2 p = ImGui::GetItemRectMin();

	ImU32 bg = *value ? Col(hovered ? COLOR_ACCENT_HOVER : COLOR_ACCENT) : (hovered ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 32));
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
	float r = h * 0.5f - S(3);
	float cx = *value ? p.x + w - h * 0.5f : p.x + h * 0.5f;
	dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32(245, 245, 245, 255));
	return pressed;
}

void ResetButton(int page, int index)
{
	if (!MUI_Settings_CanReset(page, index) || MUI_Settings_IsDefault(page, index)) {
		ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Text, COLOR_TEXT_DIM);
	if (ImGui::Button("R##reset", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
		MUI_Settings_Reset(page, index);
	}
	ImGui::PopStyleColor(2);
	ImGui::SetItemTooltip("Reset to default");
}

void OpenConfigPopup(ConfigPopup popup)
{
	config_popup = popup;
	config_popup_opening = true;
	config_files.resize(1024);
	int count = 0;
	if (popup == CONFIG_POPUP_IMPORT) {
		count = MUI_ListFiles(MUI_ConfigsDir(), "\\.(cfg|txt)$", config_files.data(), (int)config_files.size());
	}
	else if (popup == CONFIG_POPUP_SCRIPT) {
		count = MUI_ListFiles(MUI_ScriptsDir(), "\\.(cfg|txt)$", config_files.data(), (int)config_files.size());
	}
	config_files.resize(std::max(0, count));
	export_name[0] = '\0';
}

bool SafeFileName(const char *name)
{
	if (!name[0]) {
		return false;
	}
	for (const char *c = name; *c; c++) {
		if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') {
			return false;
		}
	}
	return strstr(name, "..") == nullptr;
}

void DrawConfigPopups()
{
	const char *titles[] = { "", "Import config", "Export config", "Load script" };

	if (config_popup == CONFIG_POPUP_NONE) {
		return;
	}
	if (config_popup_opening) {
		ImGui::OpenPopup("##configpopup");
		config_popup_opening = false;
	}

	ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(S(460), 0));
	if (ImGui::BeginPopupModal("##configpopup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
		SectionTitle(titles[config_popup]);
		ImGui::Spacing();

		if (config_popup == CONFIG_POPUP_EXPORT) {
			ImGui::Text("File name");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			bool enter = ImGui::InputTextWithHint("##exportname", "myconfig.cfg", export_name, sizeof(export_name), ImGuiInputTextFlags_EnterReturnsTrue);
			bool valid = SafeFileName(export_name);
			ImGui::Spacing();
			ImGui::BeginDisabled(!valid);
			if (AccentButton("Export") || (enter && valid)) {
				char cmd[192];
				snprintf(cmd, sizeof(cmd), "cfg_save \"%s\"", export_name);
				MUI_Command(cmd);
				ImGui::CloseCurrentPopup();
				config_popup = CONFIG_POPUP_NONE;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
		}
		else {
			TextDim("%s", config_popup == CONFIG_POPUP_IMPORT ? MUI_ConfigsDir() : MUI_ScriptsDir());
			ImGui::BeginChild("##files", ImVec2(0, S(260)), ImGuiChildFlags_Borders);
			if (config_files.empty()) {
				TextDim("No files found.");
			}
			for (const mui_file_t &f : config_files) {
				if (ImGui::Selectable(f.name)) {
					char cmd[256];
					if (config_popup == CONFIG_POPUP_IMPORT) {
						snprintf(cmd, sizeof(cmd), "cfg_load \"%s\"", f.name);
					}
					else {
						snprintf(cmd, sizeof(cmd), "exec \"cfg/%s\"", f.name);
					}
					MUI_Command(cmd);
					ImGui::CloseCurrentPopup();
					config_popup = CONFIG_POPUP_NONE;
				}
			}
			ImGui::EndChild();
			ImGui::Spacing();
		}

		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			ImGui::CloseCurrentPopup();
			config_popup = CONFIG_POPUP_NONE;
		}
		ImGui::EndPopup();
	}
	else {
		config_popup = CONFIG_POPUP_NONE;
	}
}

void SettingControl(int page, int index, const mui_setting_info_t &info)
{
	float control_w = ImGui::GetContentRegionAvail().x;

	switch (info.type) {
		case MUI_SETTING_BOOL: {
			bool v = MUI_Settings_GetValue(page, index) != 0;
			if (ToggleSwitch("##bool", &v)) {
				MUI_Settings_SetValue(page, index, v ? 1.0f : 0.0f);
			}
			break;
		}
		case MUI_SETTING_NUMBER:
		case MUI_SETTING_INTNUMBER: {
			float v = MUI_Settings_GetValue(page, index);
			float shown = std::max(info.min, std::min(info.max, v));
			ImGui::SetNextItemWidth(control_w);
			if (ImGui::SliderFloat("##num", &shown, info.min, info.max, NumberFormat(info.step), ImGuiSliderFlags_AlwaysClamp)) {
				if (info.step > 0) {
					shown = info.min + roundf((shown - info.min) / info.step) * info.step;
				}
				if (shown != v) {
					MUI_Settings_SetValue(page, index, shown);
				}
			}
			if (v < info.min || v > info.max) {
				ImGui::SetItemTooltip("Current value: %g (outside the slider's range)", v);
			}
			break;
		}
		case MUI_SETTING_NAMED:
		case MUI_SETTING_ENUM:
		case MUI_SETTING_COLOR: {
			int option = MUI_Settings_GetOption(page, index);
			char preview[96];
			if (option < 0 && info.type == MUI_SETTING_COLOR) {
				snprintf(preview, sizeof(preview), "off");
			}
			else {
				snprintf(preview, sizeof(preview), "%s", MUI_Settings_OptionName(page, index, option));
			}

			if (info.type == MUI_SETTING_COLOR && option >= 0) {
				unsigned int rgb = MUI_Settings_PlayerColor(option);
				ImVec2 p = ImGui::GetCursorScreenPos();
				float h = ImGui::GetFrameHeight();
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + S(3)), ImVec2(p.x + h, p.y + h - S(3)),
					IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255), S(4));
				ImGui::Dummy(ImVec2(h, h));
				ImGui::SameLine();
				control_w -= h + ImGui::GetStyle().ItemSpacing.x;
			}

			ImGui::SetNextItemWidth(control_w);
			if (ImGui::BeginCombo("##combo", preview, ImGuiComboFlags_HeightLarge)) {
				for (int i = 0; i < info.option_count; i++) {
					ImGui::PushID(i);
					if (info.type == MUI_SETTING_COLOR) {
						unsigned int rgb = MUI_Settings_PlayerColor(i);
						ImGui::ColorButton("##swatch", ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1),
							ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
						ImGui::SameLine();
					}
					if (ImGui::Selectable(MUI_Settings_OptionName(page, index, i), i == option)) {
						MUI_Settings_SetOption(page, index, i);
					}
					if (i == option) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			break;
		}
		case MUI_SETTING_CUSTOM: {
			float arrow = ImGui::GetFrameHeight();
			if (ImGui::Button("<", ImVec2(arrow, 0))) {
				MUI_Settings_CustomToggle(page, index, true);
			}
			ImGui::SameLine(0, S(4));
			if (ImGui::Button(MUI_Settings_CustomValue(page, index), ImVec2(control_w - arrow * 2 - S(8), 0))) {
				MUI_Settings_CustomToggle(page, index, false);
			}
			ImGui::SameLine(0, S(4));
			if (ImGui::Button(">", ImVec2(arrow, 0))) {
				MUI_Settings_CustomToggle(page, index, false);
			}
			break;
		}
		case MUI_SETTING_ACTION: {
			if (ImGui::Button(info.label, ImVec2(control_w, 0))) {
				switch (info.action) {
					case MUI_ACTION_IMPORT_CONFIG: OpenConfigPopup(CONFIG_POPUP_IMPORT); break;
					case MUI_ACTION_EXPORT_CONFIG: OpenConfigPopup(CONFIG_POPUP_EXPORT); break;
					case MUI_ACTION_LOAD_SCRIPT:   OpenConfigPopup(CONFIG_POPUP_SCRIPT); break;
					default:                       MUI_Settings_Action(page, index); break;
				}
			}
			break;
		}
		case MUI_SETTING_STRING:
		case MUI_SETTING_SKIN: {
			ImGuiID id = ImGui::GetID("##str");
			char temp[512];
			char *buffer = temp;
			if (id == string_edit_id) {
				buffer = string_edit_buffer;
			}
			else {
				snprintf(temp, sizeof(temp), "%s", MUI_Settings_GetString(page, index));
			}
			ImGui::SetNextItemWidth(control_w);
			ImGui::InputText("##str", buffer, sizeof(temp));
			if (ImGui::IsItemActivated()) {
				snprintf(string_edit_buffer, sizeof(string_edit_buffer), "%s", buffer);
				string_edit_id = id;
			}
			if (ImGui::IsItemDeactivated()) {
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					MUI_Settings_SetString(page, index, string_edit_buffer);
				}
				string_edit_id = 0;
			}
			break;
		}
		case MUI_SETTING_BIND: {
			char key1[32], key2[32];
			MUI_Settings_BindKeys(page, index, key1, key2, sizeof(key1));
			char label[96];
			if (key1[0] && key2[0]) {
				snprintf(label, sizeof(label), "%s  or  %s", key1, key2);
			}
			else if (key1[0]) {
				snprintf(label, sizeof(label), "%s", key1);
			}
			else {
				snprintf(label, sizeof(label), "not bound");
			}
			float clear_w = ImGui::GetFrameHeight();
			if (!key1[0]) {
				ImGui::PushStyleColor(ImGuiCol_Text, COLOR_TEXT_DIM);
			}
			if (ImGui::Button(label, ImVec2(control_w - clear_w - S(4), 0))) {
				binding_label = info.label;
				MUI_Settings_StartBinding(page, index);
			}
			if (!key1[0]) {
				ImGui::PopStyleColor();
			}
			ImGui::SetItemTooltip("Click, then press the key to bind");
			ImGui::SameLine(0, S(4));
			ImGui::BeginDisabled(!key1[0]);
			if (ImGui::Button("X##unbind", ImVec2(clear_w, 0))) {
				MUI_Settings_Unbind(page, index);
			}
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("Remove binding");
			break;
		}
		default:
			break;
	}
}

bool SettingVisible(const mui_setting_info_t &info, bool advanced, const char *filter)
{
	if (info.type == MUI_SETTING_IGNORED || !info.available) {
		return false;
	}
	// the menu has its own switch for this one
	if (info.variable && !strcmp(info.variable, "menu_advanced")) {
		return false;
	}
	if (filter && filter[0]) {
		return info.type != MUI_SETTING_SEPARATOR && (ContainsNoCase(info.label, filter) || (info.variable && ContainsNoCase(info.variable, filter)));
	}
	return advanced || !info.advanced;
}

// Draws the settings of a page, returns the number of settings shown.
int DrawSettingsList(int page, const char *filter, bool show_page_name)
{
	bool advanced = MUI_Settings_Advanced() != 0;
	int count = MUI_Settings_Count(page);
	int shown = 0;
	bool table_open = false;
	float label_w = std::max(S(200), ImGui::GetContentRegionAvail().x * 0.42f);

	ImGui::PushID(page);
	for (int i = 0; i < count; i++) {
		mui_setting_info_t info;
		MUI_Settings_Info(page, i, &info);
		if (!SettingVisible(info, advanced, filter)) {
			continue;
		}

		if (info.type == MUI_SETTING_SEPARATOR) {
			if (table_open) {
				ImGui::EndTable();
				table_open = false;
			}
			ImGui::Dummy(ImVec2(0, S(10)));
			{
				FontSize fs(FONT_LARGE);
				ImGui::TextUnformatted(info.label);
			}
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y - S(2)), ImVec2(p.x + S(36), p.y + S(1)), Col(COLOR_ACCENT), S(1));
			ImGui::Dummy(ImVec2(0, S(4)));
			continue;
		}

		if (shown == 0 && show_page_name) {
			FontSize fs(FONT_LARGE);
			ImGui::TextColored(COLOR_ACCENT_HOVER, "%s", MUI_Settings_PageName(page));
		}

		if (!table_open) {
			table_open = ImGui::BeginTable("##settings", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg);
			if (!table_open) {
				continue;
			}
			ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_w);
			ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		}

		ImGui::PushID(i);
		ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + S(6));
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		if (info.type != MUI_SETTING_ACTION) {
			ImGui::TextUnformatted(info.label);
			if (info.description || info.variable) {
				if (ImGui::BeginItemTooltip()) {
					ImGui::PushTextWrapPos(S(360));
					if (info.description) {
						ImGui::TextUnformatted(info.description);
					}
					if (info.variable) {
						TextDim("%s", info.variable);
					}
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}
			}
		}
		else if (info.description) {
			// actions: the button carries the label, the description goes on the left (may be cut)
			TextDim("%s", info.description);
			ImGui::SetItemTooltip("%s", info.description);
		}
		ImGui::TableSetColumnIndex(1);
		SettingControl(page, i, info);
		ImGui::TableSetColumnIndex(2);
		ResetButton(page, i);
		ImGui::PopID();
		shown++;
	}
	if (table_open) {
		ImGui::EndTable();
	}
	ImGui::PopID();

	return shown;
}

void DrawSettings()
{
	if (!BeginPage("##settings", 1500)) {
		EndPage();
		return;
	}

	int pages = MUI_Settings_PageCount();
	settings_page = std::max(0, std::min(settings_page, pages - 1));

	// navigation
	ImGui::BeginChild("##settingsnav", ImVec2(S(230), 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	SectionTitle("Settings");
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##settingsearch", "Search settings...", settings_search, sizeof(settings_search));
	ImGui::Spacing();
	{
		FontSize fs(FONT_LARGE);
		for (int i = 0; i < pages; i++) {
			bool selected = i == settings_page && !settings_search[0];
			if (ImGui::Selectable(MUI_Settings_PageName(i), selected, 0, ImVec2(0, S(36)))) {
				settings_page = i;
				settings_search[0] = '\0';
			}
		}
	}

	ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + S(20), ImGui::GetWindowHeight() - ImGui::GetFrameHeight() * 2 - S(16)));
	ImGui::Separator();
	bool advanced = MUI_Settings_Advanced() != 0;
	if (ToggleSwitch("##advanced", &advanced)) {
		MUI_Settings_SetAdvanced(advanced);
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Advanced options");
	ImGui::EndChild();

	ImGui::SameLine(0, S(24));

	// content
	ImGui::BeginChild("##settingscontent", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	if (settings_search[0]) {
		int total = 0;
		for (int p = 0; p < pages; p++) {
			total += DrawSettingsList(p, settings_search, true);
		}
		if (total == 0) {
			TextDim("No settings match \"%s\".", settings_search);
		}
	}
	else {
		DrawSettingsList(settings_page, nullptr, false);
	}
	ImGui::Dummy(ImVec2(0, S(20)));
	ImGui::EndChild();

	EndPage();

	DrawConfigPopups();
}

void DrawBindingOverlay()
{
	if (!MUI_Settings_IsBinding()) {
		return;
	}

	ImGuiIO &io = ImGui::GetIO();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 170));

	char text[160];
	snprintf(text, sizeof(text), "Press a key for \"%s\"", binding_label);
	float size = S(FONT_TITLE);
	ImVec2 ts = backend.font->CalcTextSizeA(size, FLT_MAX, 0, text);
	dl->AddText(backend.font, size, ImVec2((io.DisplaySize.x - ts.x) * 0.5f, io.DisplaySize.y * 0.5f - ts.y), Col(COLOR_TEXT), text);
	const char *hint = "Escape to cancel";
	float hint_size = S(FONT_BODY);
	ImVec2 hs = backend.font->CalcTextSizeA(hint_size, FLT_MAX, 0, hint);
	dl->AddText(backend.font, hint_size, ImVec2((io.DisplaySize.x - hs.x) * 0.5f, io.DisplaySize.y * 0.5f + S(12)), Col(COLOR_TEXT_DIM), hint);
}

//=============================================================================
// quit
//=============================================================================

void DrawQuit()
{
	ImGuiIO &io = ImGui::GetIO();
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, TopBarHeight()), io.DisplaySize, IM_COL32(0, 0, 0, 120));

	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(S(440), 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(30), S(26)));
	ImGui::Begin("##quit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::PopStyleVar();

	SectionTitle("Quit unezQuake?");
	ImGui::Spacing();
	if (client.connected && !client.demoplayback) {
		TextDim("You will be disconnected from %s.", client.server);
	}
	else {
		TextDim("See you next time.");
	}
	ImGui::Dummy(ImVec2(0, S(14)));

	float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	if (DangerButton("Quit", ImVec2(bw, S(38))) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
		MUI_Quit();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(bw, S(38))) || ImGui::IsKeyPressed(ImGuiKey_N, false)) {
		SetPage(MUI_PAGE_HOME);
	}
	ImGui::End();
}

//=============================================================================
// home
//=============================================================================

void DrawHome()
{
	ImGuiIO &io = ImGui::GetIO();
	ImDrawList *dl = ImGui::GetBackgroundDrawList();

	if (!client.background_scene) {
		return;
	}

	// cinematic caption: name of the background map
	std::string map = ToUpper(client.scene_map);
	float big = S(FONT_HUGE);
	ImVec2 ts = backend.font->CalcTextSizeA(big, FLT_MAX, 0, map.c_str());
	float margin = S(40);
	dl->AddRectFilledMultiColor(ImVec2(0, io.DisplaySize.y * 0.70f), io.DisplaySize,
		IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
	dl->AddText(backend.font, big, ImVec2(margin, io.DisplaySize.y - margin - ts.y - S(24)), IM_COL32(255, 255, 255, 150), map.c_str());

	const char *hint = "Pick Quick Play to jump into a game    ~  console";
	float small = S(FONT_SMALL);
	dl->AddText(backend.font, small, ImVec2(margin + S(2), io.DisplaySize.y - margin - small), Col(COLOR_TEXT_DIM), hint);
}

//=============================================================================
// frame
//=============================================================================

void SetPage(int page)
{
	if (page == MUI_PAGE_QUIT && !MUI_ConfirmQuit()) {
		MUI_Quit();
		return;
	}

	if (page != current_page) {
		MUI_PlaySound("misc/menu2.wav");
	}
	current_page = page;
	MUI_Settings_CancelBinding();

	if (page == MUI_PAGE_QUICKPLAY || page == MUI_PAGE_SERVERS) {
		MUI_Browser_Refresh(-1);
	}
	if (page == MUI_PAGE_DEMOS) {
		demo_list_dirty = true;
	}
}

void HandleBack()
{
	if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsAnyItemActive()
		|| ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || config_popup != CONFIG_POPUP_NONE) {
		return;
	}

	if (current_page != MUI_PAGE_HOME) {
		SetPage(MUI_PAGE_HOME);
	}
	else if (client.connected) {
		MUI_CloseMenu();
	}
}

void BuildFrame()
{
	ImGuiIO &io = ImGui::GetIO();

	HandleBack();

	// in game, dim the view behind the menu
	if (client.connected) {
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, current_page == MUI_PAGE_HOME ? 110 : 150));
	}
	else if (current_page != MUI_PAGE_HOME) {
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 70));
	}

	if (current_page == MUI_PAGE_QUICKPLAY || current_page == MUI_PAGE_SERVERS) {
		UpdateBrowser();
	}

	DrawTopBar();

	switch (current_page) {
		case MUI_PAGE_QUICKPLAY: DrawQuickPlay(); break;
		case MUI_PAGE_SERVERS:   DrawServers(); break;
		case MUI_PAGE_DEMOS:     DrawDemos(); break;
		case MUI_PAGE_SETTINGS:  DrawSettings(); break;
		case MUI_PAGE_QUIT:      DrawQuit(); break;
		default:                 DrawHome(); break;
	}

	DrawBindingOverlay();
}

void UpdateScale()
{
	ImGuiIO &io = ImGui::GetIO();
	float scale = std::max(0.75f, std::min(io.DisplaySize.y / 1000.0f, 2.5f)) * MUI_MenuScale();

	if (fabsf(scale - backend.scale) > 0.001f) {
		backend.scale = scale;
		ApplyStyle(scale);
	}
}

void RenderDrawData()
{
	ImDrawData *draw_data = ImGui::GetDrawData();
	GLint framebuffer = 0;
	GLboolean srgb = GL_FALSE;

	// draw straight to the window, without sRGB conversion (colours are already in display space)
	if (backend.BindFramebuffer) {
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
		if (framebuffer) {
			backend.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		}
	}
	srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glGetError(); // GL_FRAMEBUFFER_SRGB isn't known by every legacy context
	if (srgb) {
		glDisable(GL_FRAMEBUFFER_SRGB);
	}

	if (backend.modern) {
		ImGui_ImplOpenGL3_RenderDrawData(draw_data);
	}
	else {
		// The fixed function backend uses client side arrays and texture unit 0, while the
		// classic renderer may leave buffers, programs, multitexturing, fog... set up.
		GLint array_buffer = 0, element_buffer = 0, unpack_buffer = 0, program = 0, vertex_array = 0, units = 1, sampler = 0;

		glPushAttrib(GL_ALL_ATTRIB_BITS);
		glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
		// the element buffer binding belongs to the vertex array object: switch VAO first
		if (backend.BindVertexArray) {
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
			backend.BindVertexArray(0);
		}
		if (backend.BindBuffer) {
			glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
			glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
			glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
			backend.BindBuffer(GL_ARRAY_BUFFER, 0);
			backend.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
			backend.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		}
		if (backend.UseProgram) {
			glGetIntegerv(GL_CURRENT_PROGRAM, &program);
			backend.UseProgram(0);
		}
		if (backend.ActiveTexture) {
			glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
			for (GLint unit = std::min(units, 8) - 1; unit >= 0; unit--) {
				backend.ActiveTexture(GL_TEXTURE0 + unit);
				glDisable(GL_TEXTURE_2D);
				glMatrixMode(GL_TEXTURE);
				glPushMatrix();
				glLoadIdentity();
			}
			if (backend.ClientActiveTexture) {
				backend.ClientActiveTexture(GL_TEXTURE0);
			}
		}
		// a sampler object would override the font texture's filtering (no mipmaps: incomplete)
		if (backend.BindSampler) {
			glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
			backend.BindSampler(0, 0);
		}
		glDisable(GL_FOG);
		glDisable(GL_ALPHA_TEST);
		glGetError();

		ImGui_ImplOpenGL2_RenderDrawData(draw_data);

		if (backend.BindSampler) {
			backend.BindSampler(0, sampler);
		}
		if (backend.ActiveTexture) {
			for (GLint unit = std::min(units, 8) - 1; unit >= 0; unit--) {
				backend.ActiveTexture(GL_TEXTURE0 + unit);
				glMatrixMode(GL_TEXTURE);
				glPopMatrix();
			}
		}
		if (backend.UseProgram) {
			backend.UseProgram(program);
		}
		if (backend.BindBuffer) {
			backend.BindBuffer(GL_ARRAY_BUFFER, array_buffer);
			backend.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
			backend.BindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
		}
		if (backend.BindVertexArray) {
			backend.BindVertexArray(vertex_array);
		}
		glPopClientAttrib();
		glPopAttrib();
	}

	if (srgb) {
		glEnable(GL_FRAMEBUFFER_SRGB);
	}
	if (framebuffer && backend.BindFramebuffer) {
		backend.BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
	}
}

} // namespace

//=============================================================================
// interface
//=============================================================================

void MenuUI_VidInit(void *sdl_window, void *gl_context, int gl_major_version)
{
	if (backend.initialised) {
		MenuUI_VidShutdown();
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	backend.font = io.Fonts->AddFontDefaultVector();

	if (gl_major_version <= 0) {
		const char *version = (const char *)glGetString(GL_VERSION);
		gl_major_version = version ? atoi(version) : 2;
	}
	backend.modern = gl_major_version >= 3;
	backend.BindFramebuffer = (glBindFramebuffer_t)SDL_GL_GetProcAddress("glBindFramebuffer");
	backend.BindBuffer = (glBindBuffer_t)SDL_GL_GetProcAddress("glBindBuffer");
	backend.UseProgram = (glUseProgram_t)SDL_GL_GetProcAddress("glUseProgram");
	backend.BindVertexArray = (glBindVertexArray_t)SDL_GL_GetProcAddress("glBindVertexArray");
	backend.ActiveTexture = (glActiveTexture_t)SDL_GL_GetProcAddress("glActiveTexture");
	backend.ClientActiveTexture = (glActiveTexture_t)SDL_GL_GetProcAddress("glClientActiveTexture");
	// sampler objects are GL 3.3, a legacy context may hand out a stub
	backend.BindSampler = gl_major_version >= 3 ? (glBindSampler_t)SDL_GL_GetProcAddress("glBindSampler") : nullptr;

	ImGui_ImplSDL2_InitForOpenGL((SDL_Window *)sdl_window, gl_context);
	if (!backend.modern || !ImGui_ImplOpenGL3_Init(nullptr)) {
		backend.modern = false;
		ImGui_ImplOpenGL2_Init();
	}

	backend.scale = 0;
	backend.was_open = false;
	backend.initialised = true;
}

void MenuUI_VidShutdown(void)
{
	if (!backend.initialised) {
		return;
	}

	MUI_FlushTextures();
	if (backend.modern) {
		ImGui_ImplOpenGL3_Shutdown();
	}
	else {
		ImGui_ImplOpenGL2_Shutdown();
	}
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	backend.initialised = false;
	backend.font = nullptr;
}

mui_bool MenuUI_ProcessEvent(const void *sdl_event)
{
	const SDL_Event *event = (const SDL_Event *)sdl_event;

	if (!backend.initialised || !MUI_MenuIsOpen() || MUI_Settings_IsBinding()) {
		// the game gets everything (and so does key binding)
		return false;
	}

	ImGui_ImplSDL2_ProcessEvent(event);
	ImGuiIO &io = ImGui::GetIO();

	// Releases always reach the game too, otherwise its key state would think the
	// key is still held (and ignore the next press as an auto-repeat).
	switch (event->type) {
		case SDL_KEYDOWN:
			// escape is handled by the menu (back), typing goes to text fields only
			if (event->key.keysym.sym == SDLK_ESCAPE && !(event->key.keysym.mod & KMOD_SHIFT)) {
				return true;
			}
			return io.WantTextInput;
		case SDL_TEXTINPUT:
		case SDL_TEXTEDITING:
		case SDL_MOUSEMOTION:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEWHEEL:
			return true;
		default:
			return false;
	}
}

void MenuUI_SetPage(int page)
{
	SetPage(page);
}

void MenuUI_Render(void)
{
	if (!backend.initialised) {
		return;
	}

	if (!MUI_MenuIsOpen()) {
		if (backend.was_open) {
			backend.was_open = false;
			string_edit_id = 0;
		}
		return;
	}

	ImGuiIO &io = ImGui::GetIO();
	if (!backend.was_open) {
		// events that happened while closed never reached ImGui
		io.ClearInputKeys();
		io.ClearInputMouse();
		backend.was_open = true;
	}

	MUI_GetClientState(&client);

	if (backend.modern) {
		ImGui_ImplOpenGL3_NewFrame();
	}
	else {
		ImGui_ImplOpenGL2_NewFrame();
	}
	ImGui_ImplSDL2_NewFrame();
	UpdateScale();
	ImGui::NewFrame();

	BuildFrame();

	ImGui::Render();
	RenderDrawData();
}
