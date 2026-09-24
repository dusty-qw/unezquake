#ifndef EZQUAKE_CL_SESSION_H
#define EZQUAKE_CL_SESSION_H

/* Slot numbers are stable, one-based, and suitable for HUDs and bindings. */
#define CL_MAX_SESSIONS 9
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
qbool CL_SessionIsActive(void);
qbool CL_SessionVideoSuspended(void);
qbool CL_SessionWindowIsFullscreen(void);
qbool CL_SessionWindowIsMinimized(void);
qbool CL_SessionRequestWindowAction(qbool restore);
/* Fixed-size, numeric video settings exchanged only over private session IPC. */
#define SESSION_WINDOW_SETTINGS 16
typedef struct {
	char values[SESSION_WINDOW_SETTINGS][64];
} session_video_settings_t;
qbool CL_SessionRestartVideo(void);
void CL_SessionsDetachWindow(void);
void VID_SessionWindowSettings(session_video_settings_t *settings, qbool pending_only);
void VID_SessionSyncWindowSettings(const session_video_settings_t *settings);
void VID_SessionRestart(const session_video_settings_t *settings, qbool apply_pending);
void CL_SessionsEarlyInit(void);
void CL_SessionsInit(void);
void CL_SessionsFrame(void);
void CL_SessionsShutdown(void);
void CL_SessionsAttachWindow(struct SDL_Window *window);
struct SDL_Window *CL_SessionCreateWindow(void);
void VID_SessionRelease(void);
void VID_SessionActivate(void);
void CL_ClearSessionInput(void);

#endif
