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

#ifndef EZQUAKE_CL_SESSION_H
#define EZQUAKE_CL_SESSION_H

/* Slot numbers are stable, one-based, and suitable for HUDs and bindings. */
#define CL_MAX_SESSIONS 9
#define SESSION_INTERNAL_ARGVS 10
struct SDL_Window;
typedef struct {
	int slot;
	qbool exists;
	qbool ready;
	qbool selected;
	qbool connecting;
	qbool spectator;
	qbool demo;
	cactive_t state;
	int players, max_players;
	char server[256];
	char name[128];
	char map[64];
} cl_session_info_t;

int CL_SessionSelected(void);
int CL_SessionNumber(void);
int CL_SessionCount(void);
qbool CL_SessionInfo(int slot, cl_session_info_t *info);
void CL_SessionSelect(int slot);
qbool CL_SessionIsWorker(void);
qbool CL_SessionIsCoordinator(void);
qbool CL_SessionsInitCoordinator(void);
qbool CL_SessionIsActive(void);
qbool CL_SessionVideoSuspended(void);
qbool CL_SessionWindowIsFullscreen(void);
qbool CL_SessionWindowIsMinimized(void);
qbool CL_SessionRequestWindowAction(qbool restore);
qbool CL_SessionSetCaption(const char *caption);
void CL_SessionDropFile(const char *path);
qbool CL_SessionRequestQuit(qbool window_close);
/* Fixed-size, numeric video settings exchanged only over private session IPC. */
#define SESSION_WINDOW_SETTINGS 20
typedef struct {
	char values[SESSION_WINDOW_SETTINGS][64];
} session_video_settings_t;
qbool CL_SessionRestartVideo(void);
void CL_SessionsDetachWindow(void);
void VID_SessionWindowSettings(session_video_settings_t *settings, qbool pending_only);
void VID_SessionSyncWindowSettings(const session_video_settings_t *settings);
void VID_SessionSyncWindowGeometry(int x, int y, int width, int height);
void VID_SessionRestart(const session_video_settings_t *settings, qbool apply_pending);
void VID_CoordinatorInit(void);
void VID_CoordinatorFrame(void);
void CL_SessionsEarlyInit(void);
void CL_SessionsInit(void);
void CL_SessionsFrame(void);
void CL_SessionsWait(void);
#ifdef _WIN32
union SDL_Event;
typedef struct {
	qbool grab, raw, keyboard_grab, show_cursor, text_entry;
	int disable_win_keys;
} session_input_settings_t;
qbool CL_SessionWindowIsFocused(void);
qbool CL_SessionsForwardInput(const union SDL_Event *event);
void CL_SessionInputSettings(const session_input_settings_t *settings);
void VID_SessionInputSettings(const session_input_settings_t *settings);
void VID_SessionInputEvent(const union SDL_Event *event);
void VID_SessionInputState(qbool focused, qbool minimized, qbool reset);
#endif
void CL_SessionsShutdown(void);
void CL_SessionsAttachWindow(struct SDL_Window *window);
struct SDL_Window *CL_SessionCreateWindow(void);
void VID_SessionRelease(void);
void VID_SessionActivate(void);
void CL_ClearSessionInput(void);

#endif
