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

// Plain C interface between the Dear ImGui menus (menu_imgui.cpp, C++) and the
// engine (menu_ui_bridge.c). The engine headers can't be included from C++, so
// everything the menus need goes through here as plain data.

#ifndef __MENU_UI_BRIDGE_H__
#define __MENU_UI_BRIDGE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef int mui_bool;

//
// client
//

typedef struct mui_client_state_s {
	mui_bool connected;        // connecting or connected to a server, or playing a demo
	mui_bool active;           // fully in game
	mui_bool demoplayback;
	mui_bool background_scene; // the menu background map is being rendered
	mui_bool console_down;
	char map[64];
	char server[128];
	char player_name[64];
	char scene_map[64];        // name of the background map
	double realtime;
} mui_client_state_t;

void MUI_GetClientState(mui_client_state_t *out);
mui_bool MUI_MenuIsOpen(void);

void MUI_Command(const char *text);       // appended to the command buffer, newline added
void MUI_PlaySound(const char *sample);
void MUI_CloseMenu(void);                 // leave the menu (to the game or the console)
void MUI_ToggleConsole(void);
void MUI_Quit(void);                      // quits immediately, confirmation is up to the menu
mui_bool MUI_ConfirmQuit(void);           // cl_confirmquit
float MUI_MenuScale(void);                // menu_scale
void MUI_Disconnect(void);
void MUI_SetClipboard(const char *text);

// loads a file through the quake filesystem (paks included), free with MUI_FreeFile
void *MUI_LoadFile(const char *path, int *size);
void MUI_FreeFile(void *data);

// palette index as 0xRRGGBB
unsigned int MUI_PaletteColor(int index);

// GL texture name of textures/levelshots/<map>, 0 if there is none
unsigned int MUI_MapPreviewTexture(const char *map, int *width, int *height);
void MUI_FlushTextures(void);

//
// server browser
//

#define MUI_MAX_SERVER_PLAYERS 32

typedef struct mui_player_s {
	char name[32];
	char team[16];
	int frags;
	int ping;
	int time;
	int top_color, bottom_color;  // palette indexes, see MUI_PaletteColor
	mui_bool spectator;
} mui_player_t;

typedef struct mui_server_s {
	char address[64];
	char name[96];
	char map[32];
	char gamedir[32];
	char mode[24];
	char status[48];
	int ping;                  // -1 if the server didn't answer
	int players;
	int max_players;
	int spectators;
	int max_spectators;
	int timelimit;
	int fraglimit;
	mui_bool proxy;            // qizmo / qwfwd / QTV relay: not a game server
	mui_bool in_progress;      // a match is being played (or it's an ffa with people in it)
	mui_bool has_qtv;
	int player_count;
	mui_player_t player[MUI_MAX_SERVER_PLAYERS];
} mui_server_t;

typedef struct mui_browser_status_s {
	mui_bool refreshing;       // pinging servers or updating sources
	mui_bool updating_sources;
	float progress;            // 0..1
	int server_count;
	unsigned int revision;     // changes whenever the list contents may have changed
} mui_browser_status_t;

void MUI_Browser_Status(mui_browser_status_t *out);
// copies up to max servers, returns the number copied or -1 if the list is being rewritten by a refresh
int MUI_Browser_Snapshot(mui_server_t *out, int max);
// full: also re-query the master servers; -1: refresh only if not done yet (first visit)
void MUI_Browser_Refresh(mui_bool full);
void MUI_Browser_Join(const char *address);
void MUI_Browser_Observe(const char *address);
void MUI_Browser_WatchQTV(const char *address);
void MUI_Browser_AddServer(const char *address);

//
// demos
//

typedef struct mui_file_s {
	char name[196];
	int size;
	int time;                  // unix time of last modification
	mui_bool is_dir;
} mui_file_t;

// root directory for demos (demo_dir, or the game directory)
const char *MUI_Demos_Root(void);
// lists demos and directories of <root>/<subdir>, returns the number of entries
int MUI_Demos_List(const char *subdir, mui_file_t *out, int max);
void MUI_Demos_Play(const char *subdir, const char *name);

// directories of the classic menu's "Import config" and "Load script"
const char *MUI_ConfigsDir(void);
const char *MUI_ScriptsDir(void);

// lists files with one of the extensions (".cfg|.txt") in dir, returns the number of entries
int MUI_ListFiles(const char *dir, const char *extensions_regex, mui_file_t *out, int max);

//
// settings: the same pages as the classic options menu
//

typedef enum {
	MUI_SETTING_SEPARATOR,
	MUI_SETTING_NUMBER,
	MUI_SETTING_INTNUMBER,
	MUI_SETTING_BOOL,
	MUI_SETTING_CUSTOM,
	MUI_SETTING_NAMED,
	MUI_SETTING_ENUM,
	MUI_SETTING_ACTION,
	MUI_SETTING_STRING,
	MUI_SETTING_COLOR,
	MUI_SETTING_SKIN,
	MUI_SETTING_BIND,
	MUI_SETTING_IGNORED        // markers and blanks
} mui_setting_type_t;

typedef enum {
	MUI_ACTION_GENERIC,
	MUI_ACTION_IMPORT_CONFIG,  // the classic menu opens a file browser for these,
	MUI_ACTION_EXPORT_CONFIG,  // the new menu provides its own
	MUI_ACTION_LOAD_SCRIPT
} mui_action_kind_t;

typedef struct mui_setting_info_s {
	mui_setting_type_t type;
	const char *label;
	const char *description;   // may be NULL
	const char *variable;      // cvar name, may be NULL
	mui_bool advanced;
	mui_bool available;        // false if the cvar couldn't be found
	float min, max, step;
	int option_count;          // named, enum and color settings
	mui_action_kind_t action;
} mui_setting_info_t;

int MUI_Settings_PageCount(void);
const char *MUI_Settings_PageName(int page);
int MUI_Settings_Count(int page);
void MUI_Settings_Info(int page, int index, mui_setting_info_t *out);

mui_bool MUI_Settings_Advanced(void);
void MUI_Settings_SetAdvanced(mui_bool advanced);

float MUI_Settings_GetValue(int page, int index);
void MUI_Settings_SetValue(int page, int index, float value);
const char *MUI_Settings_GetString(int page, int index);
void MUI_Settings_SetString(int page, int index, const char *value);
mui_bool MUI_Settings_CanReset(int page, int index);
void MUI_Settings_Reset(int page, int index);
mui_bool MUI_Settings_IsDefault(int page, int index);

// named, enum and color settings: option -1 means a custom value
int MUI_Settings_GetOption(int page, int index);
void MUI_Settings_SetOption(int page, int index, int option);
const char *MUI_Settings_OptionName(int page, int index, int option);
// player colors (setting values 0..16) as 0xRRGGBB
unsigned int MUI_Settings_PlayerColor(int color);

const char *MUI_Settings_CustomValue(int page, int index);
void MUI_Settings_CustomToggle(int page, int index, mui_bool back);
void MUI_Settings_Action(int page, int index);

// key bindings: names of up to two keys bound to the command ("" if none)
void MUI_Settings_BindKeys(int page, int index, char *key1, char *key2, int size);
void MUI_Settings_Unbind(int page, int index);
// the next key press is bound to the setting (escape cancels)
void MUI_Settings_StartBinding(int page, int index);
mui_bool MUI_Settings_IsBinding(void);
void MUI_Settings_CancelBinding(void);

//
// menu.c / vid_sdl2.c side, implemented by menu_imgui.cpp
//

enum {
	MUI_PAGE_HOME,
	MUI_PAGE_QUICKPLAY,
	MUI_PAGE_SERVERS,
	MUI_PAGE_DEMOS,
	MUI_PAGE_SETTINGS,
	MUI_PAGE_QUIT
};

void MenuUI_VidInit(void *sdl_window, void *gl_context, int gl_major_version);
void MenuUI_VidShutdown(void);
// returns true if the event was consumed by the menu and must not reach the game
mui_bool MenuUI_ProcessEvent(const void *sdl_event);
// draws the menu (if open) on top of the finished frame
void MenuUI_Render(void);
void MenuUI_SetPage(int page);

#ifdef __cplusplus
}
#endif

#endif // __MENU_UI_BRIDGE_H__
