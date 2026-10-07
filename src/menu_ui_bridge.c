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

// Engine side of the Dear ImGui menus: everything menu_imgui.cpp needs from the
// client (server browser, demos, settings, key bindings, textures) exposed as
// plain C data, plus the glue between menu.c's states and the new menus.

#include "quakedef.h"
#include "keys.h"
#include "menu.h"
#include "menu_options.h"
#include "settings.h"
#include "settings_page.h"
#include "EX_browser.h"
#include "EX_qtvlist.h"
#include "qsound.h"
#include "r_texture.h"
#include "menu_scene.h"
#include "menu_ui_bridge.h"

#define MUI_MAX_PREVIEWS 32

extern cvar_t cl_confirmquit;
extern cvar_t menu_advanced;
extern cvar_t sb_autoupdate;
extern cvar_t name;
extern int ping_phase;
extern double ping_pos;
extern int updating_sources;
extern int rebuild_servers_list;
extern unsigned d_8to24table[256];

unsigned int GL_TextureNameFromReference(texture_ref ref);
char *CL_DemoDirectory(void);
float VARFVAL(const cvar_t *v);
char *VARSVAL(const cvar_t *v);
int Sbar_ColorForMap(int m);

static void OnChange_menu_classic(cvar_t *var, char *value, qbool *cancel);
// 0 = new menus, 1 = classic menus. Only read at startup, changing it needs a restart.
cvar_t menu_classic = {"menu_classic", "0", 0, OnChange_menu_classic};
// size of the new menus relative to the automatic scale (based on window height)
cvar_t menu_scale = {"menu_scale", "1"};

static qbool mui_video_ready;
// menu_classic as it was when the client started
static qbool mui_classic;
static qbool mui_classic_latched;

//=============================================================================
// client
//=============================================================================

// Quake's charset has coloured/special glyphs in the upper half, the menus only want plain text.
static void MUI_PlainText(char *dst, const char *src, size_t size)
{
	size_t len = 0;

	if (size == 0) {
		return;
	}

	for ( ; src && *src && len + 1 < size; src++) {
		unsigned char c = (unsigned char)*src & 127;

		if (c >= 0x12 && c <= 0x1b) {
			c = '0' + (c - 0x12);
		}
		else if (c == 0x10) {
			c = '[';
		}
		else if (c == 0x11) {
			c = ']';
		}
		else if (c == 0x1c || c == 0x05 || c == 0x0e || c == 0x0f) {
			c = '.';
		}
		else if (c < 32 || c == 127) {
			continue;
		}
		dst[len++] = c;
	}
	dst[len] = '\0';
}

void MUI_GetClientState(mui_client_state_t *out)
{
	memset(out, 0, sizeof(*out));

	out->connected = cls.state >= ca_connected || cls.demoplayback;
	out->active = cls.state == ca_active;
	out->demoplayback = cls.demoplayback;
	out->background_scene = MenuScene_Active();
	out->console_down = key_dest == key_console;
	out->realtime = cls.realtime;
	if (out->active) {
		strlcpy(out->map, host_mapname.string, sizeof(out->map));
	}
	if (cls.demoplayback) {
		strlcpy(out->server, COM_SkipPath(cls.demoname), sizeof(out->server));
	}
	else if (cls.state >= ca_connected) {
		strlcpy(out->server, cls.servername, sizeof(out->server));
	}
	MUI_PlainText(out->player_name, name.string, sizeof(out->player_name));
	strlcpy(out->scene_map, MenuScene_MapName(), sizeof(out->scene_map));
}

void MUI_Command(const char *text)
{
	Cbuf_AddText((char *)text);
	Cbuf_AddText("\n");
}

void MUI_PlaySound(const char *sample)
{
	S_LocalSound((char *)sample);
}

void MUI_CloseMenu(void)
{
	M_LeaveMenus();
}

void MUI_ToggleConsole(void)
{
	Con_ToggleConsole_f();
}

void MUI_Quit(void)
{
	Host_Quit();
}

float MUI_MenuScale(void)
{
	return bound(0.5f, menu_scale.value, 3.0f);
}

mui_bool MUI_ConfirmQuit(void)
{
	return cl_confirmquit.integer != 0;
}

void MUI_Disconnect(void)
{
	Cbuf_AddText("disconnect\n");
}

void MUI_SetClipboard(const char *text)
{
	Sys_CopyToClipboard(text);
}

void *MUI_LoadFile(const char *path, int *size)
{
	vfsfile_t *f = FS_OpenVFS(path, "rb", FS_ANY);
	vfserrno_t err;
	byte *data;
	int length;

	*size = 0;
	if (!f) {
		return NULL;
	}

	length = VFS_GETLEN(f);
	if (length <= 0) {
		VFS_CLOSE(f);
		return NULL;
	}

	data = Q_malloc(length);
	if (VFS_READ(f, data, length, &err) != length) {
		Q_free(data);
		VFS_CLOSE(f);
		return NULL;
	}
	VFS_CLOSE(f);

	*size = length;
	return data;
}

void MUI_FreeFile(void *data)
{
	Q_free(data);
}

//=============================================================================
// map previews
//=============================================================================

typedef struct mui_preview_s {
	char map[64];
	texture_ref texture;
	qbool missing;
	double last_used;
} mui_preview_t;

static mui_preview_t mui_previews[MUI_MAX_PREVIEWS];

unsigned int MUI_MapPreviewTexture(const char *map, int *width, int *height)
{
	mui_preview_t *preview = NULL, *oldest = &mui_previews[0];
	int i;

	*width = *height = 0;
	if (!map || !map[0] || !mui_video_ready || FS_UnsafeFilename(map)) {
		return 0;
	}

	for (i = 0; i < MUI_MAX_PREVIEWS; i++) {
		if (!strcmp(mui_previews[i].map, map)) {
			preview = &mui_previews[i];
			break;
		}
		if (mui_previews[i].last_used < oldest->last_used) {
			oldest = &mui_previews[i];
		}
	}

	if (!preview) {
		// load once, also remembering if there's no levelshot so the disk isn't hit every frame
		preview = oldest;
		if (R_TextureReferenceIsValid(preview->texture)) {
			R_DeleteTexture(&preview->texture);
		}
		memset(preview, 0, sizeof(*preview));
		strlcpy(preview->map, map, sizeof(preview->map));
		preview->texture = R_LoadTextureImage(va("textures/levelshots/%s", map), va("menu:levelshot:%s", map), 0, 0, TEX_NOCOMPRESS | TEX_NOSCALE);
		preview->missing = !R_TextureReferenceIsValid(preview->texture);
	}

	preview->last_used = cls.realtime;
	if (preview->missing) {
		return 0;
	}

	*width = R_TextureWidth(preview->texture);
	*height = R_TextureHeight(preview->texture);
	return GL_TextureNameFromReference(preview->texture);
}

void MUI_FlushTextures(void)
{
	int i;

	for (i = 0; i < MUI_MAX_PREVIEWS; i++) {
		if (R_TextureReferenceIsValid(mui_previews[i].texture)) {
			R_DeleteTexture(&mui_previews[i].texture);
		}
	}
	memset(mui_previews, 0, sizeof(mui_previews));
}

//=============================================================================
// server browser
//=============================================================================

static unsigned int mui_browser_revision = 1;
static int mui_last_phase = -1, mui_last_updating = -1, mui_last_count = -1;
static qbool mui_browser_refreshed;

static int MUI_KeyInt(server_data *s, char *key)
{
	char *value = ValueForKey(s, key);

	return value ? atoi(value) : 0;
}

static void MUI_ServerMode(server_data *s, char *mode, size_t size)
{
	char *value = ValueForKey(s, "mode");
	int teamplay = MUI_KeyInt(s, "teamplay");
	int maxclients = MUI_KeyInt(s, "maxclients");

	if (value && value[0]) {
		MUI_PlainText(mode, value, size);
	}
	else if (!strcasecmp(s->display.gamedir, "fortress")) {
		strlcpy(mode, "TF", size);
	}
	else if (MUI_KeyInt(s, "coop")) {
		strlcpy(mode, "coop", size);
	}
	else if (teamplay) {
		switch (maxclients) {
			case 4:  strlcpy(mode, "2on2", size); break;
			case 6:  strlcpy(mode, "3on3", size); break;
			case 8:  strlcpy(mode, "4on4", size); break;
			default: strlcpy(mode, "team", size); break;
		}
	}
	else {
		strlcpy(mode, maxclients == 2 ? "1on1" : "ffa", size);
	}
}

static qbool MUI_ServerInProgress(const mui_server_t *server)
{
	if (server->proxy || server->players <= 0) {
		return false;
	}
	if (server->status[0]) {
		// KTX: "Standby", "Countdown", "3 min left", ...
		return strstr(server->status, "left") || strstr(server->status, "Countdown") || strstr(server->status, "overtime");
	}
	return server->players >= 2;
}

void MUI_Browser_Status(mui_browser_status_t *out)
{
	memset(out, 0, sizeof(*out));
	out->updating_sources = updating_sources != 0;
	out->refreshing = ping_phase != 0 || updating_sources;
	out->progress = (float)bound(0, ping_pos, 1);
	out->server_count = serversn;

	if (ping_phase != mui_last_phase || updating_sources != mui_last_updating || serversn != mui_last_count) {
		mui_last_phase = ping_phase;
		mui_last_updating = updating_sources;
		mui_last_count = serversn;
		mui_browser_revision++;
	}
	out->revision = mui_browser_revision;
}

int MUI_Browser_Snapshot(mui_server_t *out, int max)
{
	int i, j, count = 0;

	// The refresh thread rewrites the server list while updating sources and the server
	// infos while querying them (phase 2). While it only pings (phase 1), reading is fine,
	// the classic browser does it too.
	if (ping_phase > 1 || updating_sources || (ping_phase && rebuild_servers_list)) {
		return -1;
	}

	if (rebuild_servers_list) {
		Rebuild_Servers_List();
	}

	SB_ServerList_Lock();
	for (i = 0; i < serversn && count < max; i++) {
		server_data *s = servers[i];
		mui_server_t *server = &out[count];
		char *value;

		if (!s) {
			continue;
		}

		memset(server, 0, sizeof(*server));
		strlcpy(server->address, s->display.ip, sizeof(server->address));
		value = ValueForKey(s, "hostname");
		MUI_PlainText(server->name, value && value[0] ? value : s->display.name, sizeof(server->name));
		MUI_PlainText(server->map, s->display.map, sizeof(server->map));
		MUI_PlainText(server->gamedir, s->display.gamedir, sizeof(server->gamedir));
		MUI_PlainText(server->status, ValueForKey(s, "status"), sizeof(server->status));
		MUI_ServerMode(s, server->mode, sizeof(server->mode));
		server->ping = s->ping;
		server->players = s->playersn;
		server->spectators = s->spectatorsn;
		server->max_players = MUI_KeyInt(s, "maxclients");
		server->max_spectators = MUI_KeyInt(s, "maxspectators");
		server->timelimit = MUI_KeyInt(s, "timelimit");
		server->fraglimit = MUI_KeyInt(s, "fraglimit");
		// QTV relays show up in the master lists as servers with no map and lots of slots
		server->proxy = s->qizmo || s->qwfwd || ValueForKey(s, "*QTV") || ValueForKey(s, "*qtv") || !server->map[0];

		for (j = 0; j < s->playersn + s->spectatorsn && server->player_count < MUI_MAX_SERVER_PLAYERS; j++) {
			playerinfo *p = s->players[j];
			mui_player_t *player = &server->player[server->player_count];

			if (!p) {
				continue;
			}
			MUI_PlainText(player->name, p->name, sizeof(player->name));
			MUI_PlainText(player->team, p->team, sizeof(player->team));
			player->frags = p->frags;
			player->ping = p->ping;
			player->time = p->time;
			// the browser already turned the player's colors into palette indexes
			player->top_color = p->top;
			player->bottom_color = p->bottom;
			player->spectator = p->spec;
			server->player_count++;
		}

		server->in_progress = MUI_ServerInProgress(server);
		server->has_qtv = !server->proxy && server->players > 0 && qtvlist_has_stream(server->address);
		count++;
	}
	SB_ServerList_Unlock();

	return count;
}

void MUI_Browser_Refresh(mui_bool full)
{
	// first visit: only refresh if the classic browser would have (sb_autoupdate)
	if (full < 0) {
		if (mui_browser_refreshed || (!sb_autoupdate.integer && serversn > 0)) {
			return;
		}
		full = true;
	}

	mui_browser_refreshed = true;
	if (!ping_phase && !updating_sources) {
		GetServerPingsAndInfos(full);
	}
}

static void MUI_Browser_Connect(const char *address, qbool spectator)
{
	netadr_t adr;

	if (!address || !address[0]) {
		return;
	}

	if (sb_findroutes.integer && NET_StringToAdr(address, &adr)) {
		Cbuf_AddText(spectator ? "spectator 1\n" : "spectator 0\n");
		SB_PingTree_ConnectBestPath(&adr);
	}
	else {
		Cbuf_AddText(va("%s %s\n", spectator ? "observe" : "join", address));
	}
	// the menu closes once connected (see M_ImGui_Frame), and stays if connecting fails
}

void MUI_Browser_Join(const char *address)
{
	MUI_Browser_Connect(address, false);
}

void MUI_Browser_Observe(const char *address)
{
	MUI_Browser_Connect(address, true);
}

void MUI_Browser_WatchQTV(const char *address)
{
	Cbuf_AddText(va("qtv %s\n", address));
}

void MUI_Browser_AddServer(const char *address)
{
	if (address && address[0] && !strchr(address, ';') && !strchr(address, '"')) {
		Cbuf_AddText(va("addserver \"%s\"\n", address));
	}
}

//=============================================================================
// demos & files
//=============================================================================

static qbool MUI_IsDemoFile(const char *name)
{
	static const char *extensions[] = { ".qwd", ".qwz", ".mvd", ".dem", ".mvd.gz", ".qwd.gz", ".dem.gz" };
	size_t len = strlen(name);
	int i;

	for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		size_t extlen = strlen(extensions[i]);

		if (len > extlen && !strcasecmp(name + len - extlen, extensions[i])) {
			return true;
		}
	}
	return false;
}

static const char *MUI_JoinPath(const char *dir, const char *subdir)
{
	static char path[MAX_OSPATH];

	if (subdir && subdir[0]) {
		snprintf(path, sizeof(path), "%s/%s", dir, subdir);
	}
	else {
		strlcpy(path, dir, sizeof(path));
	}
	return path;
}

static qbool MUI_SafeSubdir(const char *subdir)
{
	return !subdir || (!strstr(subdir, "..") && subdir[0] != '/' && subdir[0] != '\\');
}

const char *MUI_Demos_Root(void)
{
	return CL_DemoDirectory();
}

int MUI_Demos_List(const char *subdir, mui_file_t *out, int max)
{
	dir_t dir;
	int i, count = 0;

	if (!MUI_SafeSubdir(subdir)) {
		return 0;
	}

	dir = Sys_listdir(MUI_JoinPath(CL_DemoDirectory(), subdir), ".*", SORT_BY_DATE);
	for (i = 0; i < dir.numfiles && count < max; i++) {
		file_t *f = &dir.files[i];

		if (!f->isdir && !MUI_IsDemoFile(f->name)) {
			continue;
		}
		strlcpy(out[count].name, f->name, sizeof(out[count].name));
		out[count].size = f->size;
		out[count].time = f->time;
		out[count].is_dir = f->isdir;
		count++;
	}
	return count;
}

void MUI_Demos_Play(const char *subdir, const char *name)
{
	char path[MAX_OSPATH];

	if (!MUI_SafeSubdir(subdir) || !name || strchr(name, '"') || strchr(name, '/') || strchr(name, '\\')) {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", MUI_JoinPath(CL_DemoDirectory(), subdir), name);
	Cbuf_AddText(va("playdemo \"%s\"\n", path));
}

const char *MUI_ConfigsDir(void)
{
	static char dir[MAX_OSPATH];

	strlcpy(dir, Menu_Options_ConfigsDir(), sizeof(dir));
	return dir;
}

const char *MUI_ScriptsDir(void)
{
	return Menu_Options_ScriptsDir();
}

int MUI_ListFiles(const char *dir, const char *extensions_regex, mui_file_t *out, int max)
{
	dir_t listing = Sys_listdir(dir, extensions_regex, SORT_BY_NAME);
	int i, count = 0;

	for (i = 0; i < listing.numfiles && count < max; i++) {
		if (listing.files[i].isdir) {
			continue;
		}
		strlcpy(out[count].name, listing.files[i].name, sizeof(out[count].name));
		out[count].size = listing.files[i].size;
		out[count].time = listing.files[i].time;
		out[count].is_dir = false;
		count++;
	}
	return count;
}

//=============================================================================
// settings
//=============================================================================

#define MUI_ENUM_NAME(s, i)  ((s)->named_ints[(i) * 2])
#define MUI_ENUM_VALUE(s, i) ((s)->named_ints[(i) * 2 + 1])

static const char *mui_color_names[17] = {
	"White", "Brown", "Lavender", "Khaki", "Red", "Light Brown", "Peach", "Light Peach", "Purple",
	"Dark Purple", "Tan", "Green", "Yellow", "Blue", "Orange", "Bright Red", "Black"
};

static struct {
	qbool active;
	int page, index;
} mui_binding;

static setting *MUI_Setting(int page, int index)
{
	settings_page *p = Menu_Options_Page(page);

	if (!p || index < 0 || index >= p->count) {
		return NULL;
	}
	return &p->settings[index];
}

int MUI_Settings_PageCount(void)
{
	return Menu_Options_PageCount();
}

const char *MUI_Settings_PageName(int page)
{
	return Menu_Options_PageName(page);
}

int MUI_Settings_Count(int page)
{
	settings_page *p = Menu_Options_Page(page);

	return p ? p->count : 0;
}

void MUI_Settings_Info(int page, int index, mui_setting_info_t *out)
{
	setting *s = MUI_Setting(page, index);

	memset(out, 0, sizeof(*out));
	out->type = MUI_SETTING_IGNORED;
	if (!s) {
		return;
	}

	out->label = s->label ? s->label : "";
	out->description = s->description;
	out->advanced = s->advanced;
	out->min = s->min;
	out->max = s->max;
	out->step = s->step;
	out->available = true;

	switch (s->type) {
		case stt_separator:   out->type = MUI_SETTING_SEPARATOR; break;
		case stt_num:         out->type = MUI_SETTING_NUMBER; break;
		case stt_intnum:      out->type = MUI_SETTING_INTNUMBER; break;
		case stt_bool:        out->type = MUI_SETTING_BOOL; break;
		case stt_custom:      out->type = MUI_SETTING_CUSTOM; break;
		case stt_named:       out->type = MUI_SETTING_NAMED; out->option_count = (int)s->max + 1; break;
		case stt_enum:        out->type = MUI_SETTING_ENUM; out->option_count = (int)s->max + 1; break;
		case stt_action:      out->type = MUI_SETTING_ACTION; break;
		case stt_string:      out->type = MUI_SETTING_STRING; break;
		case stt_playercolor: out->type = MUI_SETTING_COLOR; out->option_count = 17; break;
		case stt_skin:        out->type = MUI_SETTING_SKIN; break;
		case stt_bind:        out->type = MUI_SETTING_BIND; out->variable = s->varname; break;
		default:              out->type = MUI_SETTING_IGNORED; break;
	}

	if (s->type != stt_intnum && s->type != stt_bind && s->cvar) {
		out->variable = s->cvar->name;
	}

	switch (s->type) {
		case stt_num: case stt_bool: case stt_named: case stt_enum: case stt_string: case stt_playercolor: case stt_skin:
			out->available = s->cvar != NULL;
			break;
		case stt_intnum:
			out->available = s->cvar != NULL;
			break;
		case stt_custom:
			out->available = s->readfnc != NULL;
			break;
		case stt_action:
			out->available = s->actionfnc != NULL;
			break;
		default:
			break;
	}

	if (s->type == stt_action) {
		if (s->actionfnc == MOpt_ImportConfig) {
			out->action = MUI_ACTION_IMPORT_CONFIG;
		}
		else if (s->actionfnc == MOpt_ExportConfig) {
			out->action = MUI_ACTION_EXPORT_CONFIG;
		}
		else if (s->actionfnc == MOpt_LoadScript) {
			out->action = MUI_ACTION_LOAD_SCRIPT;
		}
	}
}

mui_bool MUI_Settings_Advanced(void)
{
	return menu_advanced.integer != 0;
}

void MUI_Settings_SetAdvanced(mui_bool advanced)
{
	Cvar_SetValue(&menu_advanced, advanced ? 1 : 0);
}

float MUI_Settings_GetValue(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (!s || !s->cvar) {
		return 0;
	}
	if (s->type == stt_intnum) {
		return (float)*((int *)s->cvar);
	}
	return VARFVAL(s->cvar);
}

void MUI_Settings_SetValue(int page, int index, float value)
{
	setting *s = MUI_Setting(page, index);

	if (!s || !s->cvar) {
		return;
	}

	if (s->type == stt_intnum) {
		*((int *)s->cvar) = (int)bound(s->min, value, s->max);
	}
	else if (s->type == stt_bool) {
		Cvar_Set(s->cvar, value ? "1" : "0");
	}
	else {
		Cvar_SetValue(s->cvar, value);
	}
}

const char *MUI_Settings_GetString(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (!s || !s->cvar || s->type == stt_intnum) {
		return "";
	}
	return VARSVAL(s->cvar);
}

void MUI_Settings_SetString(int page, int index, const char *value)
{
	setting *s = MUI_Setting(page, index);

	if (s && s->cvar && s->type != stt_intnum) {
		Cvar_Set(s->cvar, (char *)value);
	}
}

mui_bool MUI_Settings_CanReset(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (!s || !s->cvar) {
		return false;
	}
	switch (s->type) {
		case stt_num: case stt_string: case stt_named: case stt_bool: case stt_enum: case stt_playercolor: case stt_skin:
			return s->cvar->defaultvalue != NULL;
		default:
			return false;
	}
}

mui_bool MUI_Settings_IsDefault(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (!MUI_Settings_CanReset(page, index)) {
		return true;
	}
	return !strcmp(VARSVAL(s->cvar), s->cvar->defaultvalue);
}

void MUI_Settings_Reset(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (MUI_Settings_CanReset(page, index)) {
		Cvar_ResetVar(s->cvar);
	}
}

int MUI_Settings_GetOption(int page, int index)
{
	setting *s = MUI_Setting(page, index);
	int i;

	if (!s || !s->cvar) {
		return -1;
	}

	switch (s->type) {
		case stt_named:
			return (int)bound(s->min, VARFVAL(s->cvar), s->max);
		case stt_playercolor:
			i = (int)VARFVAL(s->cvar);
			return (i >= 0 && i <= 16) ? i : -1;
		case stt_enum:
			for (i = 0; i <= s->max; i++) {
				if (!strcasecmp(VARSVAL(s->cvar), MUI_ENUM_VALUE(s, i))) {
					return i;
				}
			}
			return -1;
		default:
			return -1;
	}
}

void MUI_Settings_SetOption(int page, int index, int option)
{
	setting *s = MUI_Setting(page, index);

	if (!s || !s->cvar || option < 0 || option > s->max) {
		return;
	}

	switch (s->type) {
		case stt_named:
		case stt_playercolor:
			Cvar_SetValue(s->cvar, option);
			break;
		case stt_enum:
			Cvar_Set(s->cvar, va("%s", MUI_ENUM_VALUE(s, option)));
			break;
		default:
			break;
	}
}

const char *MUI_Settings_OptionName(int page, int index, int option)
{
	setting *s = MUI_Setting(page, index);

	if (!s || option < 0) {
		return "custom";
	}

	switch (s->type) {
		case stt_named:
			return option <= s->max && s->named_ints ? s->named_ints[option] : "";
		case stt_enum:
			return option <= s->max && s->named_ints ? MUI_ENUM_NAME(s, option) : "";
		case stt_playercolor:
			return option <= 16 ? mui_color_names[option] : "";
		default:
			return "";
	}
}

unsigned int MUI_PaletteColor(int index)
{
	unsigned int rgba = LittleLong(d_8to24table[index & 255]);

	// d_8to24table holds R, G, B, A in memory order
	return ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
}

unsigned int MUI_Settings_PlayerColor(int color)
{
	if (color < 0 || color > 16) {
		return 0;
	}
	return MUI_PaletteColor(Sbar_ColorForMap(color));
}

const char *MUI_Settings_CustomValue(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	return s && s->readfnc ? s->readfnc() : "";
}

void MUI_Settings_CustomToggle(int page, int index, mui_bool back)
{
	setting *s = MUI_Setting(page, index);

	if (s && s->togglefnc) {
		s->togglefnc(back);
	}
}

void MUI_Settings_Action(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (s && s->type == stt_action && s->actionfnc) {
		s->actionfnc();
	}
}

void MUI_Settings_BindKeys(int page, int index, char *key1, char *key2, int size)
{
	setting *s = MUI_Setting(page, index);
	int keys[2];

	key1[0] = key2[0] = '\0';
	if (!s || s->type != stt_bind || !s->varname) {
		return;
	}

	M_FindKeysForCommand(s->varname, keys);
	if (keys[0] != -1) {
		strlcpy(key1, Key_KeynumToString(keys[0]), size);
	}
	if (keys[1] != -1) {
		strlcpy(key2, Key_KeynumToString(keys[1]), size);
	}
}

void MUI_Settings_Unbind(int page, int index)
{
	setting *s = MUI_Setting(page, index);
	int j, l;

	if (!s || s->type != stt_bind || !s->varname) {
		return;
	}

	l = strlen(s->varname) + 1;
	for (j = 0; j < (sizeof(keybindings) / sizeof(*keybindings)); j++) {
		if (keybindings[j] && !strncmp(keybindings[j], s->varname, l)) {
			Key_Unbind(j);
		}
	}
}

void MUI_Settings_StartBinding(int page, int index)
{
	setting *s = MUI_Setting(page, index);

	if (s && s->type == stt_bind && s->varname) {
		mui_binding.active = true;
		mui_binding.page = page;
		mui_binding.index = index;
	}
}

mui_bool MUI_Settings_IsBinding(void)
{
	return mui_binding.active && key_dest == key_menu && m_state == m_imgui;
}

void MUI_Settings_CancelBinding(void)
{
	mui_binding.active = false;
}

static void MUI_Settings_BindKey(int key)
{
	setting *s = MUI_Setting(mui_binding.page, mui_binding.index);

	mui_binding.active = false;
	if (key == K_ESCAPE || !s || !s->varname) {
		return;
	}

	Key_SetBinding(key, s->varname);
	S_LocalSound("misc/menu1.wav");
}

//=============================================================================
// glue with menu.c, vid_sdl2.c and cl_screen.c
//=============================================================================

static void OnChange_menu_classic(cvar_t *var, char *value, qbool *cancel)
{
	if (mui_classic_latched && (Q_atoi(value) != 0) != mui_classic) {
		Com_Printf("%s will take effect after you restart the client\n", var->name);
	}
}

// Before VID_Init(), which decides whether the new menus get set up.
// The config (and +set from the command line) has been executed by now.
void M_ClassicMenus_Init(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_MENU);
	Cvar_Register(&menu_classic);
	Cvar_ResetCurrentGroup();

	mui_classic = menu_classic.integer != 0;
	mui_classic_latched = true;
}

void M_ImGui_Init(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_MENU);
	Cvar_Register(&menu_scale);
	Cvar_ResetCurrentGroup();
}

qbool M_ClassicMenus(void)
{
	return mui_classic;
}

qbool M_ImGui_Enabled(void)
{
	return !mui_classic && mui_video_ready;
}

qbool M_ImGui_IsOpen(void)
{
	return key_dest == key_menu && m_state == m_imgui && mui_video_ready;
}

mui_bool MUI_MenuIsOpen(void)
{
	return M_ImGui_IsOpen();
}

void M_ImGui_Open(int page)
{
	if (!(key_dest == key_menu && m_state == m_imgui)) {
		M_EnterMenu(m_imgui);
	}
	MenuUI_SetPage(page);
}

void M_ImGui_Key(int key)
{
	if (mui_binding.active) {
		MUI_Settings_BindKey(key);
		return;
	}

	// everything else reaches the menus as SDL events
	if (key == '`' || key == '~') {
		Con_ToggleConsole_f();
	}
}

qbool M_ImGui_Mouse_Event(const mouse_state_t *ms)
{
	// ImGui gets the mouse from SDL directly. Let button events continue as key events so the
	// key state stays right (releases) and mouse buttons can be bound (presses while binding).
	return false;
}

// Video is going down (vid_restart, shutdown) or is back up. The menu state is kept,
// so e.g. applying a new resolution from the settings page returns to that page.
void M_ImGui_VidReady(qbool ready)
{
	mui_video_ready = ready;
	if (!ready) {
		MUI_FlushTextures();
	}
}

// Called every frame once all of the engine's drawing is done.
// Classic menu screens that have a counterpart in the new menus.
static int M_ImGui_PageForState(m_state_t state)
{
	switch (state) {
		case m_main: case m_ingame:                    return MUI_PAGE_HOME;
		case m_multiplayer: case m_multiplayer_submenu: return MUI_PAGE_SERVERS;
		case m_demos:                                   return MUI_PAGE_DEMOS;
		case m_options:                                 return MUI_PAGE_SETTINGS;
		case m_quit:                                    return MUI_PAGE_QUIT;
		default:                                        return -1;
	}
}

void M_ImGui_Frame(void)
{
	static int last_state = ca_disconnected;
	static qbool last_demoplayback;
	qbool offline = cls.state == ca_disconnected && !cls.demoplayback;
	qbool started;
	int page;

	if (!M_ImGui_Enabled()) {
		return;
	}

	// a classic menu was entered directly (before video was up, or by code that doesn't know better)
	if (key_dest == key_menu && (page = M_ImGui_PageForState(m_state)) >= 0) {
		m_state = m_imgui;
		MenuUI_SetPage(page);
	}

	// A server was reached or a demo/QTV stream started (from the menus, the command line,
	// the console, a qw:// url...): get out of the way. Failed attempts leave the menu as it was.
	started = (cls.state >= ca_connected && last_state < ca_connected) || (cls.demoplayback && !last_demoplayback);
	if (started && M_ImGui_IsOpen()) {
		M_LeaveMenus();
	}
	last_state = cls.state;
	last_demoplayback = cls.demoplayback;

	// While disconnected, the menu is the home screen: closing the console brings it back.
	if (offline && m_state == m_none && key_dest == key_game) {
		M_ImGui_Open(MUI_PAGE_HOME);
	}

	if (!M_ImGui_IsOpen()) {
		mui_binding.active = false;
	}

	MenuUI_Render();
}
