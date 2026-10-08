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

/* Independent engines, sharing one application window. No game-state pointers
 * cross the process boundary. The coordinator owns slot allocation and focus;
 * every engine publishes a copy of its own HUD metadata. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "quakedef.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include "cl_session.h"
#include "keys.h"
#include "input.h"
#include "qsound.h"
#include "teamplay.h"
void IN_DeactivateMouse(void);

static cl_session_info_t sessions[CL_MAX_SESSIONS];
static int local_slot = 1, selected_slot = 1;
static qbool session_worker, session_coordinator;
static SDL_atomic_t active = { 1 }; /* Also read by the audio callback. */
static SDL_Window *session_window;
static qbool worker_window_fullscreen;
static qbool worker_window_minimized;

int CL_SessionSelected(void) { return selected_slot; }
int CL_SessionNumber(void) { return local_slot; }
qbool CL_SessionIsWorker(void) { return session_worker; }
qbool CL_SessionIsCoordinator(void) { return session_coordinator; }
qbool CL_SessionIsActive(void) { return SDL_AtomicGet(&active) != 0; }
qbool CL_SessionVideoSuspended(void) { return CL_SessionIsWorker() && !session_window; }
qbool CL_SessionWindowIsFullscreen(void)
{
	return CL_SessionIsWorker() ? worker_window_fullscreen :
		(session_window && (SDL_GetWindowFlags(session_window) & SDL_WINDOW_FULLSCREEN));
}

qbool CL_SessionWindowIsMinimized(void)
{
	return CL_SessionIsWorker() ? worker_window_minimized :
		(session_window && (SDL_GetWindowFlags(session_window) & SDL_WINDOW_MINIMIZED));
}

static void Session_LocalInfo(void)
{
	cl_session_info_t *info;
	extern double connect_time;
	if (session_coordinator)
		return;
	info = &sessions[local_slot - 1];
	memset(info, 0, sizeof(*info));
	info->slot = local_slot;
	info->exists = true;
	info->ready = host_everything_loaded;
	info->selected = selected_slot == local_slot;
	info->state = cls.state;
	info->connecting = cls.state == ca_disconnected && connect_time != 0;
	info->spectator = cl.spectator != 0;
	info->demo = cls.demoplayback;
	if (cls.state == ca_active) {
		info->players = TP_CountPlayers();
		info->max_players = max(0, Q_atoi(Info_ValueForKey(cl.serverinfo, "maxclients")));
	}
	strlcpy(info->server, cls.servername, sizeof(info->server));
	strlcpy(info->name, Info_ValueForKey(cl.serverinfo, "hostname"), sizeof(info->name));
	strlcpy(info->map, cl.model_name[1], sizeof(info->map));
}

static void Session_LocalDropFile(const char *path)
{
	Cbuf_AddText(!strncmp(path, "qw://", 5) ? "qwurl " : "playdemo ");
	Cbuf_AddText(path);
	Cbuf_AddText("\n");
}

int CL_SessionCount(void)
{
	int i, count = 0;
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		count += sessions[i].exists != 0;
	return count;
}

qbool CL_SessionInfo(int slot, cl_session_info_t *info)
{
	if (!info || slot < 1 || slot > CL_MAX_SESSIONS)
		return false;
	if (slot == local_slot)
		Session_LocalInfo();
	*info = sessions[slot - 1];
	info->slot = slot;
	info->selected = selected_slot == slot;
	return info->exists;
}

#if defined(_WIN32) || (defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11))
#ifdef _WIN32
#include "cl_session_win.h"
#else
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 34)
#define SESSION_HAVE_SPAWN_CLOSEFROM
#endif
#endif
#endif

enum { SESSION_READY = 1, SESSION_SELECT, SESSION_SLEEP, SESSION_ASLEEP,
	SESSION_WAKE, SESSION_SNAPSHOT, SESSION_CLOSE_REQUEST, SESSION_MINIMIZE, SESSION_RESTORE,
	SESSION_VIDEO_REQUEST, SESSION_VIDEO_DETACH, SESSION_VIDEO_DETACHED,
	SESSION_VIDEO_ATTACH, SESSION_VIDEO_ATTACHED,
	SESSION_QUIT, SESSION_WINDOW_QUIT, SESSION_SHUTDOWN, SESSION_DROP_FILE,
	SESSION_QUIT_SAVE, SESSION_NOTICE, SESSION_CAPTION, SESSION_INPUT, SESSION_INPUT_SETTINGS };
#define SESSION_NOTICE_HISTORY 64
#define SESSION_NOTICE_LENGTH 1024
typedef struct {
	int type, slot;
	unsigned int generation;
	qbool fullscreen;
	qbool minimized;
#ifdef _WIN32
	qbool focused;
	session_input_settings_t input_settings;
#endif
	int x, y, width, height;
	uintptr_t window;
	char path[MAX_OSPATH];
	char notice[SESSION_NOTICE_LENGTH];
	session_video_settings_t video;
	cl_session_info_t info[CL_MAX_SESSIONS];
} session_message_t;
typedef struct {
#ifdef _WIN32
	intptr_t fd, pid;
#else
	int fd;
	pid_t pid;
#endif
	double started, closing_started;
	qbool video_sent, video_ready;
	qbool shutdown_sent;
	uint64_t next_notice;
} session_worker_t;

static session_worker_t workers[CL_MAX_SESSIONS];
static char notices[SESSION_NOTICE_HISTORY][SESSION_NOTICE_LENGTH];
static uint64_t notice_sequence;
static intptr_t coordinator_fd = -1;
static int requested_slot = 1, sleeping_slot;
static double switch_started, next_status;
static uintptr_t parent_window;
static qbool shutting_down;
static int quit_slot, local_quit_request;
static qbool quit_saving;
static double quit_started;
#ifndef _WIN32
static Display *display;
static void *xlib;
static int (*session_XSelectInput)(Display *, Window, long);
static Status (*session_XGetWindowAttributes)(Display *, Window, XWindowAttributes *);
static long session_events;
static int (*session_XSync)(Display *, Bool);
static int (*session_XPutBackEvent)(Display *, XEvent *);
#else
static char pending_caption[SESSION_NOTICE_LENGTH];
static qbool caption_pending;
#define SESSION_INPUT_BATCH 64
#define SESSION_INPUT_QUEUE 1024
/* A separate compact wire packet: never send the HUD snapshot per input event. */
typedef struct {
	int type;
	unsigned int generation, count;
	qbool focused, minimized, reset;
	SDL_Event events[SESSION_INPUT_BATCH];
} session_input_message_t;
typedef char session_input_packet_fits[(sizeof(session_input_message_t) <= sizeof(session_message_t)) ? 1 : -1];
static SDL_Event input_queue[SESSION_INPUT_QUEUE];
static unsigned int input_head, input_count, input_generation;
static qbool input_reset, input_settings_pending, input_settings_valid;
static session_input_settings_t input_settings;
static qbool worker_window_focused;

#endif

static enum { VIDEO_IDLE, VIDEO_DETACHING, VIDEO_ATTACHING } video_phase;
static unsigned int video_generation;
static int video_requester;
static double video_started;
static qbool video_failed, video_cancelled, video_detached;
static int video_ack;
static session_video_settings_t video_settings;
static qbool startup_video_restart;

static void Session_WindowState(session_message_t *message)
{
	message->fullscreen = CL_SessionWindowIsFullscreen();
	message->minimized = CL_SessionWindowIsMinimized();
#ifdef _WIN32
	message->focused = CL_SessionWindowIsFocused();
#endif
	if (session_window) {
		SDL_GetWindowPosition(session_window, &message->x, &message->y);
		SDL_GetWindowSize(session_window, &message->width, &message->height);
	}
}

static void Session_ApplyWindowState(const session_message_t *message)
{
	extern qbool Minimized, scr_skipupdate;
	static int last_x, last_y, last_width, last_height;
#ifndef _WIN32
	int x, y, width, height;
	XEvent event;
#endif
	worker_window_fullscreen = message->fullscreen;
	worker_window_minimized = message->minimized;
	if (Minimized && !worker_window_minimized)
		scr_skipupdate = false;
	Minimized = worker_window_minimized;
#ifdef _WIN32
	worker_window_focused = message->focused;
	VID_SessionInputState(worker_window_focused, worker_window_minimized, false);
#endif
	if (!message->fullscreen && !message->minimized && message->width > 0 && message->height > 0 &&
		(message->x != last_x || message->y != last_y ||
		 message->width != last_width || message->height != last_height)) {
		VID_SessionSyncWindowGeometry(message->x, message->y, message->width, message->height);
		last_x = message->x;
		last_y = message->y;
		last_width = message->width;
		last_height = message->height;
	}
	if (!session_window)
		return;
#ifdef _WIN32
	Session_WinGeometry(session_window, message->width, message->height);
#else
	SDL_GetWindowPosition(session_window, &x, &y);
	SDL_GetWindowSize(session_window, &width, &height);
	if (message->width > 0 && message->height > 0 &&
		(x != message->x || y != message->y || width != message->width || height != message->height)) {
		/* Update SDL's local geometry without moving/resizing the native window.
		 * Only ConfigureNotify is safe: map/state events invoke SDL's fullscreen
		 * mode management even for SDL_CreateWindowFrom wrappers. */
		memset(&event, 0, sizeof(event));
		event.xconfigure.type = ConfigureNotify;
		event.xconfigure.display = display;
		event.xconfigure.event = event.xconfigure.window = parent_window;
		event.xconfigure.send_event = True;
		event.xconfigure.x = message->x;
		event.xconfigure.y = message->y;
		event.xconfigure.width = message->width;
		event.xconfigure.height = message->height;
		session_XPutBackEvent(display, &event);
	}
#endif
}

static qbool Session_Send(intptr_t fd, const session_message_t *message)
{
#ifdef _WIN32
	return Session_WinSend(fd, message, sizeof(*message));
#else
	return send(fd, message, sizeof(*message), MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(*message);
#endif
}

/* Positive: one message; -1: try next frame; 0: disconnected/fatal error. */
static int Session_Receive(intptr_t fd, session_message_t *message)
{
#ifdef _WIN32
	return Session_WinReceive(fd, message, sizeof(*message));
#else
	int bytes = recv(fd, message, sizeof(*message), MSG_DONTWAIT);
	if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
		return 0;
	return bytes;
#endif
}

static void Session_ClosePipe(intptr_t fd)
{
#ifdef _WIN32
	Session_WinClose(fd);
#else
	close(fd);
#endif
}

static void Session_Terminate(intptr_t pid, qbool force)
{
#ifdef _WIN32
	Session_WinTerminate(pid);
#else
	kill(pid, force ? SIGKILL : SIGTERM);
#endif
}

static qbool Session_Reap(session_worker_t *worker, qbool wait)
{
#ifdef _WIN32
	return Session_WinReap(worker->pid, wait);
#else
	pid_t result = waitpid(worker->pid, NULL, wait ? 0 : WNOHANG);
	return result == worker->pid || (result < 0 && errno == ECHILD);
#endif
}

qbool CL_SessionSetCaption(const char *caption)
{
#ifdef _WIN32
	if (session_worker) {
		strlcpy(pending_caption, caption, sizeof(pending_caption));
		caption_pending = true;
		return true;
	}
#endif
	return false;
}

#ifdef _WIN32
qbool CL_SessionWindowIsFocused(void)
{
	return session_worker ? worker_window_focused :
		(session_window && (SDL_GetWindowFlags(session_window) & SDL_WINDOW_INPUT_FOCUS));
}

static qbool Session_InputEnabled(void)
{
	return selected_slot > 0 && !sleeping_slot && !quit_slot && video_phase == VIDEO_IDLE &&
		sessions[selected_slot - 1].ready && workers[selected_slot - 1].fd >= 0 &&
		!workers[selected_slot - 1].closing_started;
}

static void Session_ResetInput(void)
{
	input_head = input_count = 0;
	input_reset = true;
	VID_SessionInputSettings(NULL);
}

qbool CL_SessionsForwardInput(const SDL_Event *event)
{
	SDL_Event copy;
	if (!session_coordinator)
		return false;
	memset(&copy, 0, sizeof(copy));
	switch (event->type) {
	case SDL_WINDOWEVENT:
		if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
			event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
			event->window.event == SDL_WINDOWEVENT_MINIMIZED ||
			event->window.event == SDL_WINDOWEVENT_RESTORED) {
			input_head = input_count = 0;
			input_reset = true;
		}
		return false; /* The coordinator still processes its own window events. */
	case SDL_KEYDOWN: case SDL_KEYUP: copy.key = event->key; break;
	case SDL_TEXTINPUT: copy.text = event->text; break;
	case SDL_MOUSEMOTION: copy.motion = event->motion; break;
	case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: copy.button = event->button; break;
	case SDL_MOUSEWHEEL: copy.wheel = event->wheel; break;
	default: return false;
	}
	if (!Session_InputEnabled() || !CL_SessionWindowIsFocused() || CL_SessionWindowIsMinimized())
		return true;
	if (input_count == SESSION_INPUT_QUEUE) {
		/* A stalled worker must not retain pressed keys if its input overflows.
		 * Discard obsolete input and explicitly reset before delivering more. */
		input_head = input_count = 0;
		input_reset = true;
	}
	input_queue[(input_head + input_count++) % SESSION_INPUT_QUEUE] = copy;
	return true;
}

static void Session_FlushInput(void)
{
	session_input_message_t packet;
	unsigned int i;
	if (!Session_InputEnabled()) {
		input_head = input_count = 0;
		return;
	}
	while (input_count || input_reset) {
		memset(&packet, 0, sizeof(packet));
		packet.type = SESSION_INPUT;
		packet.generation = input_generation;
		packet.count = min(input_count, SESSION_INPUT_BATCH);
		packet.focused = CL_SessionWindowIsFocused();
		packet.minimized = CL_SessionWindowIsMinimized();
		packet.reset = input_reset;
		for (i = 0; i < packet.count; ++i)
			packet.events[i] = input_queue[(input_head + i) % SESSION_INPUT_QUEUE];
		if (!Session_WinSend(workers[selected_slot - 1].fd, &packet,
			offsetof(session_input_message_t, events) + packet.count * sizeof(SDL_Event)))
			break; /* Retain the complete ordered batch until the pipe has space. */
		input_head = (input_head + packet.count) % SESSION_INPUT_QUEUE;
		input_count -= packet.count;
		input_reset = false;
	}
}

void CL_SessionInputSettings(const session_input_settings_t *settings)
{
	session_message_t message;
	if (!session_worker || !CL_SessionIsActive())
		return;
	if (!input_settings_valid || memcmp(&input_settings, settings, sizeof(*settings))) {
		input_settings = *settings;
		input_settings_pending = input_settings_valid = true;
	}
	if (!input_settings_pending)
		return;
	memset(&message, 0, sizeof(message));
	message.type = SESSION_INPUT_SETTINGS;
	message.generation = input_generation;
	message.input_settings = input_settings;
	if (Session_Send(coordinator_fd, &message))
		input_settings_pending = false;
}

static void Session_ReceiveInput(const session_message_t *message, int bytes)
{
	session_input_message_t packet;
	unsigned int i;
	if (bytes < offsetof(session_input_message_t, events) || bytes > sizeof(packet))
		return;
	memset(&packet, 0, sizeof(packet));
	memcpy(&packet, message, bytes);
	if (!CL_SessionIsActive() || video_detached || packet.generation != input_generation ||
		packet.count > SESSION_INPUT_BATCH ||
		bytes != offsetof(session_input_message_t, events) + packet.count * sizeof(SDL_Event))
		return;
	worker_window_focused = packet.focused;
	worker_window_minimized = packet.minimized;
	VID_SessionInputState(packet.focused, packet.minimized, packet.reset);
	if (packet.focused && !packet.minimized)
		for (i = 0; i < packet.count; ++i)
			VID_SessionInputEvent(&packet.events[i]);
}
#endif

void CL_SessionsWait(void)
{
#ifdef _WIN32
	intptr_t processes[CL_MAX_SESSIONS];
	int i, count = 0;
	if (SDL_HasEvents(SDL_FIRSTEVENT, SDL_LASTEVENT))
		return;
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		if (workers[i].pid > 0)
			processes[count++] = workers[i].pid;
	Session_WinWait(processes, count, Session_InputEnabled() && (input_count || input_reset) ? 1 : 50);
#else
	SDL_Delay(1);
#endif
}

static void Session_Printf(const char *format, ...)
{
	char text[SESSION_NOTICE_LENGTH];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Con_Printf("%s", text);
	if (session_coordinator) {
		strlcpy(notices[notice_sequence % SESSION_NOTICE_HISTORY], text, SESSION_NOTICE_LENGTH);
		++notice_sequence;
	}
}

static void Session_FlushNotices(session_worker_t *worker)
{
	session_message_t message;
	if (worker->pid <= 0 || worker->fd < 0 || worker->next_notice == notice_sequence)
		return;
	/* Retry full IPC queues without blocking the owner. Keep memory bounded
	 * if an engine stalls; startup engines receive notices once they can poll. */
	if (notice_sequence - worker->next_notice > SESSION_NOTICE_HISTORY)
		worker->next_notice = notice_sequence - SESSION_NOTICE_HISTORY;
	memset(&message, 0, sizeof(message));
	message.type = SESSION_NOTICE;
	while (worker->next_notice < notice_sequence) {
		strlcpy(message.notice, notices[worker->next_notice % SESSION_NOTICE_HISTORY], sizeof(message.notice));
		if (!Session_Send(worker->fd, &message))
			break;
		++worker->next_notice;
	}
}

static qbool Session_Control(intptr_t fd, int type, int slot)
{
	session_message_t message;
	memset(&message, 0, sizeof(message));
	message.type = type;
	message.slot = slot;
	if (type == SESSION_WAKE || type == SESSION_QUIT_SAVE)
		Session_WindowState(&message);
	if (type == SESSION_WAKE) {
		VID_SessionWindowSettings(&message.video, false);
#ifdef _WIN32
		message.generation = input_generation;
#endif
	}
	return Session_Send(fd, &message);
}

qbool CL_SessionRequestWindowAction(qbool restore)
{
	if (!CL_SessionIsWorker())
		return false;
	if (!Session_Control(coordinator_fd, restore ? SESSION_RESTORE : SESSION_MINIMIZE, local_slot))
		Session_Printf("Window request could not be queued.\n");
	return true;
}

void CL_SessionDropFile(const char *path)
{
	session_message_t message;
	int slot = selected_slot ? selected_slot : requested_slot;
	if (!session_coordinator) {
		Session_LocalDropFile(path);
		return;
	}
	memset(&message, 0, sizeof(message));
	message.type = SESSION_DROP_FILE;
	if (strlcpy(message.path, path, sizeof(message.path)) >= sizeof(message.path) ||
		!Session_Send(workers[slot - 1].fd, &message))
		Session_Printf("File drop could not be sent to session %d.\n", slot);
}

static void Session_ReleaseInput(void)
{
	VID_SessionRelease();
	SDL_AtomicSet(&active, 0);
	Key_ClearStates();
	CL_ClearSessionInput();
	IN_DeactivateMouse();
	S_StopAllSounds();
#ifdef _WIN32
	Session_WinSetActive(session_window, false);
#else
	if (display && session_window) {
		session_XSelectInput(display, parent_window, session_events & ~ButtonPressMask);
		session_XSync(display, False);
	}
#endif
}

void CL_SessionsDetachWindow(void)
{
#ifndef _WIN32
	if (CL_SessionIsWorker() && display && session_window) {
		/* Foreign wrappers must stop receiving events before the owner replaces
		 * the window. The owner must retain its subscriptions: SDL_DestroyWindow
		 * hides the native window and waits synchronously for UnmapNotify, which
		 * requires StructureNotifyMask. Clearing that mask strands it forever. */
		session_XSelectInput(display, parent_window, 0);
		session_XSync(display, False);
	}
	display = NULL;
#endif
	session_window = NULL;
}

void CL_SessionsEarlyInit(void)
{
	int i = COM_FindParm("-session-worker");
#ifdef _WIN32
	int wake_arg = COM_FindParm("-session-wake");
#else
	int type;
	struct ucred peer;
	socklen_t size = sizeof(type);
#endif
	if (!i)
		return;
	if (i + 3 >= COM_Argc())
		exit(EXIT_FAILURE);
	coordinator_fd = (intptr_t)strtoull(COM_Argv(i + 1), NULL, 10);
	local_slot = atoi(COM_Argv(i + 2));
	parent_window = (uintptr_t)strtoull(COM_Argv(i + 3), NULL, 10);
#ifdef _WIN32
	if (local_slot < 1 || local_slot > CL_MAX_SESSIONS ||
		!wake_arg || wake_arg + 1 >= COM_Argc() ||
		!Session_WinValidateWorker(coordinator_fd, parent_window,
			(uintptr_t)strtoull(COM_Argv(wake_arg + 1), NULL, 10)))
		exit(EXIT_FAILURE);
#else
	if (coordinator_fd < 3 || local_slot < 1 || local_slot > CL_MAX_SESSIONS || !parent_window ||
		getsockopt(coordinator_fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET)
		exit(EXIT_FAILURE);
	/* The private socket identifies the actual coordinator, including the race
	 * where it exits before the worker has installed its parent-death signal.
	 * SIGKILL also covers workers blocked inside a driver or during startup. */
	size = sizeof(peer);
	if (getsockopt(coordinator_fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
		getppid() != peer.pid || prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
		getppid() != peer.pid)
		_exit(EXIT_FAILURE);
	fcntl(coordinator_fd, F_SETFD, FD_CLOEXEC);
#endif
	session_worker = true;
	SDL_AtomicSet(&active, 0);
}

SDL_Window *CL_SessionCreateWindow(void)
{
#ifdef _WIN32
	return Session_WinCreateWindow(parent_window);
#else
	/* Every engine presents to the original top-level drawable. Child windows
	 * can be composited at a different cadence even when their swap FPS is high.
	 * SDL's foreign-window wrapper never destroys the owner's native window. */
	SDL_SetHint(SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL, "1");
	return SDL_CreateWindowFrom((void *)parent_window);
#endif
}

void CL_SessionsAttachWindow(SDL_Window *window)
{
#ifdef _WIN32
	session_window = window;
	if (session_coordinator)
		parent_window = Session_WinWindowHandle(window);
#else
	SDL_SysWMinfo wm;
	XWindowAttributes attributes;
	session_window = window;
	SDL_VERSION(&wm.version);
	if (!SDL_GetWindowWMInfo(window, &wm) || wm.subsystem != SDL_SYSWM_X11) {
		if (CL_SessionIsWorker())
			Sys_Error("Shared-window sessions require SDL's X11 video driver");
		return;
	}
	if (!xlib)
		xlib = SDL_LoadObject("libX11.so.6");
#define SESSION_X_LOAD(name) do { \
	session_##name = SDL_LoadFunction(xlib, #name); \
	if (!session_##name) Sys_Error("Sessions: missing %s", #name); \
} while (0)
	if (!xlib)
		Sys_Error("Sessions: cannot load libX11");
	SESSION_X_LOAD(XSelectInput);
	SESSION_X_LOAD(XGetWindowAttributes);
	SESSION_X_LOAD(XSync);
	SESSION_X_LOAD(XPutBackEvent);
#undef SESSION_X_LOAD
	display = wm.info.x11.display;
	parent_window = wm.info.x11.window;
	if (CL_SessionIsWorker()) {
		session_events = FocusChangeMask | EnterWindowMask | LeaveWindowMask |
			ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
			KeyPressMask | KeyReleaseMask | KeymapStateMask;
		/* Only the coordinator may process map/unmap and WM-state events.
		 * SDL otherwise lets every foreign wrapper restore fullscreen using its
		 * own display/placement state, racing the real window owner. Geometry
		 * and visibility are forwarded in session messages instead. */
		/* ButtonPress is exclusive to one X client. Acquire it only on WAKE. */
		session_XSelectInput(display, parent_window, session_events & ~ButtonPressMask);
	}
	else {
		session_XGetWindowAttributes(display, parent_window, &attributes);
		session_events = attributes.your_event_mask;
		/* The owner never takes button input, including after vid_restart. */
		if (session_coordinator)
			session_XSelectInput(display, parent_window, session_events & ~ButtonPressMask);
	}
	session_XSync(display, False);
#endif
}

static qbool Session_Start(int slot)
{
	session_worker_t *worker = &workers[slot - 1];
	if (worker->pid > 0) {
		Session_Printf("Session %d is still closing.\n", slot);
		return false;
	}
#ifdef _WIN32
	if (!Session_WinStart(slot, parent_window, session_window, &worker->fd, &worker->pid)) {
		Session_Printf("Cannot start session %d: %s\n", slot, SDL_GetError());
		return false;
	}
#else
	int sockets[2], width, height, result;
	char number[24], parent[32], w[24], h[24];
	char **argv;
	char **child_env;
	int argc = 0;
	int env_count;
	posix_spawn_file_actions_t actions;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) {
		Session_Printf("Cannot create session: %s\n", strerror(errno));
		return false;
	}
	SDL_GetWindowSize(session_window, &width, &height);
	snprintf(number, sizeof(number), "%d", slot);
	snprintf(parent, sizeof(parent), "%llu", (unsigned long long)parent_window);
	snprintf(w, sizeof(w), "%d", width);
	snprintf(h, sizeof(h), "%d", height);
	argv = Q_malloc((COM_Argc() + 32) * sizeof(*argv));
	argv[argc++] = "/proc/self/exe";
	if (slot == 1) {
		/* Preserve the user's launch, including config selection, +connect,
		 * demos and terminal input. Only this engine runs startup actions. */
		for (result = 1; result < COM_Argc(); ++result)
			argv[argc++] = COM_Argv(result);
	}
	else {
		argv[argc++] = "-basedir"; argv[argc++] = com_basedir;
		argv[argc++] = "-window";
		argv[argc++] = "-width"; argv[argc++] = w;
		argv[argc++] = "-height"; argv[argc++] = h;
		argv[argc++] = "-noconinput";
		argv[argc++] = "-nostdout";
		argv[argc++] = "+set"; argv[argc++] = "cl_net_clientport"; argv[argc++] = "0";
		argv[argc++] = "+set"; argv[argc++] = "vid_renderer"; argv[argc++] = Cvar_String("vid_renderer");
	}
	/* Append internal arguments so positional demo/QTV launches keep argv[1]. */
	argv[argc++] = "-session-worker";
	argv[argc++] = "3"; argv[argc++] = number; argv[argc++] = parent;
	argv[argc++] = "-nohwgamma";
	argv[argc++] = "+set"; argv[argc++] = "sys_inactivesleep"; argv[argc++] = "0";
	argv[argc] = NULL;
	/* Only stdio and the private control socket cross exec; exclude the
	 * coordinator's X11 connection and open filesystem handles. */
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
#ifdef SESSION_HAVE_SPAWN_CLOSEFROM
	posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#else
	{
		DIR *dir = opendir("/proc/self/fd");
		struct dirent *entry;
		if (!dir) {
			Q_free(argv);
			posix_spawn_file_actions_destroy(&actions);
			close(sockets[0]); close(sockets[1]);
			Session_Printf("Cannot enumerate file descriptors for session.\n");
			return false;
		}
		while ((entry = readdir(dir))) {
			int descriptor = atoi(entry->d_name);
			if (descriptor >= 4 && descriptor != dirfd(dir))
				posix_spawn_file_actions_addclose(&actions, descriptor);
		}
		closedir(dir);
	}
#endif
	for (env_count = 0; environ[env_count]; ++env_count) { }
	child_env = Q_malloc((env_count + 2) * sizeof(*child_env));
	for (result = 0, env_count = 0; environ[result]; ++result)
		if (strncmp(environ[result], "SDL_VIDEODRIVER=", 16))
			child_env[env_count++] = environ[result];
	child_env[env_count] = "SDL_VIDEODRIVER=x11";
	child_env[env_count + 1] = NULL;
	result = posix_spawn(&worker->pid, "/proc/self/exe", &actions, NULL, argv, child_env);
	Q_free(argv);
	Q_free(child_env);
	posix_spawn_file_actions_destroy(&actions);
	close(sockets[1]);
	if (result) {
		close(sockets[0]);
		Session_Printf("Cannot start session: %s\n", strerror(result));
		return false;
	}
	worker->fd = sockets[0];
#endif
	worker->started = Sys_DoubleTime();
	worker->closing_started = 0;
	worker->shutdown_sent = false;
	memset(&sessions[slot - 1], 0, sizeof(sessions[slot - 1]));
	sessions[slot - 1].slot = slot;
	sessions[slot - 1].exists = true;
	Session_Printf("Starting session %d...\n", slot);
	/* New/reused slots skip earlier notices, including their own creation. */
	worker->next_notice = notice_sequence;
	return true;
}

qbool CL_SessionsInitCoordinator(void)
{
	int i;
	if (session_worker)
		return false;
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
		Sys_Error("Couldn't initialize SDL video: %s", SDL_GetError());
	/* Other backends retain the ordinary single-process client. */
#ifdef _WIN32
	if (strcmp(SDL_GetCurrentVideoDriver(), "windows"))
		return false;
	if (!Session_WinInit()) {
		Con_Printf("Sessions unavailable: %s\n", SDL_GetError());
		return false;
	}
#else
	if (strcmp(SDL_GetCurrentVideoDriver(), "x11"))
		return false;
#endif
	session_coordinator = true;
	local_slot = selected_slot = 0;
	SDL_AtomicSet(&active, 0);
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		workers[i].fd = -1;
	atexit(CL_SessionsShutdown);
	VID_CoordinatorInit();
	/* Config callbacks may have opened a client port during early config load.
	 * Only engines own network sockets; release it before the first spawn. */
	NET_Shutdown();
	if (!Session_Start(1))
		Sys_Error("Cannot start the initial session");
	return true;
}

static void Session_Activate(int slot)
{
#ifdef _WIN32
	Session_ResetInput();
	++input_generation;
#endif
	selected_slot = slot;
	sleeping_slot = 0;
	/* Every numbered slot is an engine worker; the owner never takes input. */
	if (slot < 1 || slot > CL_MAX_SESSIONS || !sessions[slot - 1].ready ||
		!Session_Control(workers[slot - 1].fd, SESSION_WAKE, slot)) {
		selected_slot = 0;
	}
	next_status = 0;
}

static int Session_FallbackSlot(void)
{
	int i;
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		if (workers[i].pid > 0 && workers[i].fd >= 0 && !workers[i].closing_started && sessions[i].exists)
			return i + 1;
	return 1;
}

static void Session_BeginVideoRestart(int slot, const session_video_settings_t *settings)
{
	int i;
	if (quit_slot || video_phase != VIDEO_IDLE || sleeping_slot || slot != selected_slot) {
		Session_Printf("Video restart deferred: wait for the current session operation to finish.\n");
		return;
	}
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid > 0 && (!sessions[i].ready || workers[i].fd < 0 || workers[i].closing_started)) {
			Session_Printf("Wait for sessions to finish starting or closing before vid_restart.\n");
			return;
		}
	}
#ifdef _WIN32
	Session_ResetInput();
#endif
	video_settings = *settings;
	video_requester = slot;
	requested_slot = selected_slot;
	++video_generation;
	video_phase = VIDEO_DETACHING;
	video_started = Sys_DoubleTime();
	video_failed = video_cancelled = false;
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		workers[i].video_sent = workers[i].video_ready = false;
}

qbool CL_SessionRestartVideo(void)
{
	session_message_t message;
	if (!session_worker)
		return false;
	if (!host_everything_loaded) {
		/* Autoexec/+commands run before the initial WAKE. Keep a requested
		 * shared rebuild until this engine is ready and owns the input. */
		startup_video_restart = true;
		return true;
	}
	memset(&message, 0, sizeof(message));
	/* vid_restart is an unconditional video reset, even without new settings. */
	VID_SessionWindowSettings(&message.video, true);
	message.type = SESSION_VIDEO_REQUEST;
	message.slot = local_slot;
	if (!Session_Send(coordinator_fd, &message))
		Session_Printf("Video restart request could not be queued; try vid_restart again.\n");
	return true;
}

static void Session_VideoFrame(double now)
{
	session_message_t message;
	qbool ready = true;
	int i;
	if (video_phase == VIDEO_IDLE)
		return;
	memset(&message, 0, sizeof(message));
	message.type = video_phase == VIDEO_DETACHING ? SESSION_VIDEO_DETACH : SESSION_VIDEO_ATTACH;
	message.generation = video_generation;
	message.slot = video_cancelled ? 0 : video_requester;
	message.window = parent_window;
	message.video = video_settings;
	Session_WindowState(&message);
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		session_worker_t *worker = &workers[i];
		if (worker->pid <= 0)
			continue;
		if (!worker->video_sent && worker->fd >= 0)
			worker->video_sent = Session_Send(worker->fd, &message);
		if (!worker->video_ready) {
			ready = false;
			/* A crashed or hung engine must not strand every other session.
			 * Wait for reaping before granting input to a replacement owner. */
			if (video_phase == VIDEO_ATTACHING && now - video_started > 30)
				Session_Terminate(worker->pid, now - video_started > 35);
		}
	}
	if (video_phase == VIDEO_DETACHING) {
		if (!ready && !video_failed && now - video_started < 10)
			return;
		video_cancelled = video_failed || !ready;
		if (video_cancelled) {
			Session_Printf("Shared video restart cancelled: a session could not detach. Restoring the existing window.\n");
		}
		else {
			VID_SessionRestart(&video_settings, false);
		}
		VID_SessionWindowSettings(&video_settings, false);
		video_phase = VIDEO_ATTACHING;
		video_started = Sys_DoubleTime();
		for (i = 0; i < CL_MAX_SESSIONS; ++i)
			workers[i].video_sent = workers[i].video_ready = false;
	}
	else if (ready) {
		video_phase = VIDEO_IDLE;
		if (!sessions[video_requester - 1].exists)
			video_requester = Session_FallbackSlot();
		requested_slot = video_requester;
		Session_Activate(video_requester);
	}
}

void CL_SessionSelect(int slot)
{
	if (slot < 1 || slot > CL_MAX_SESSIONS) {
		Session_Printf("Session must be between 1 and %d.\n", CL_MAX_SESSIONS);
		return;
	}
	if (CL_SessionIsWorker()) {
		if (!Session_Control(coordinator_fd, SESSION_SELECT, slot))
			Session_Printf("Session switch could not be queued.\n");
		return;
	}
	if (!session_coordinator) {
		Session_Printf("Sessions require Windows or Linux with SDL's X11 driver (including XWayland).\n");
		return;
	}
	if (quit_slot || sleeping_slot || video_phase != VIDEO_IDLE)
		return;
	if (!sessions[slot - 1].exists && !Session_Start(slot))
		return;
	requested_slot = slot;
}

static void Session_Remove(int slot)
{
	session_worker_t *worker = &workers[slot - 1];
	if (worker->fd >= 0)
		Session_ClosePipe(worker->fd);
	worker->fd = -1;
	memset(&sessions[slot - 1], 0, sizeof(sessions[slot - 1]));
	if (requested_slot == slot)
		requested_slot = Session_FallbackSlot();
	if (selected_slot == slot) {
#ifdef _WIN32
		Session_ResetInput();
#endif
		selected_slot = 0;
		if (!sleeping_slot)
			requested_slot = Session_FallbackSlot();
	}
	if (sleeping_slot == slot)
		sleeping_slot = 0;
	if (video_phase == VIDEO_DETACHING && video_requester == slot)
		video_failed = true;
	next_status = 0;
}

static void Session_Close(int slot)
{
	if (quit_slot || local_quit_request)
		return;
	if (slot <= 1 || slot > CL_MAX_SESSIONS) {
		Session_Printf("Session 1 cannot be closed. Use quit to close the client.\n");
		return;
	}
	if (CL_SessionIsWorker()) {
		if (!Session_Control(coordinator_fd, SESSION_CLOSE_REQUEST, slot))
			Session_Printf("Session close request could not be queued.\n");
	}
	else if (sessions[slot - 1].exists) {
		if (!Session_Control(workers[slot - 1].fd, SESSION_SHUTDOWN, slot))
			Session_Printf("Session close request could not be queued.\n");
		else if (!workers[slot - 1].closing_started) {
			workers[slot - 1].closing_started = Sys_DoubleTime();
#ifdef _WIN32
			if (slot == selected_slot)
				Session_ResetInput();
#endif
		}
	}
}

static void Session_BeginQuit(int slot)
{
	int i;
	if (quit_slot)
		return;
	quit_slot = slot >= 1 && slot <= CL_MAX_SESSIONS ? slot : requested_slot;
	quit_started = Sys_DoubleTime();
	quit_saving = false;
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		workers[i].shutdown_sent = false;
}

qbool CL_SessionRequestQuit(qbool window_close)
{
	int i;
	if (shutting_down)
		return true;
	if (CL_SessionIsWorker()) {
		if (!local_quit_request)
			local_quit_request = window_close ? SESSION_WINDOW_QUIT : SESSION_QUIT;
		if (Session_Control(coordinator_fd, local_quit_request, local_slot))
			local_quit_request = 0;
		return true;
	}
	if (!session_coordinator)
		return false;
	if (quit_slot)
		return true;
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid > 0) {
			Session_BeginQuit(selected_slot ? selected_slot : requested_slot);
			return true;
		}
	}
	return false; /* No workers remain, or the single-session fallback. */
}

static void Session_QuitWorker(session_worker_t *worker, int type, double now)
{
	if (!worker->shutdown_sent && worker->fd >= 0)
		worker->shutdown_sent = Session_Control(worker->fd, type, quit_slot);
	if (now - quit_started > 5)
		Session_Terminate(worker->pid, now - quit_started > 10);
}

static void Session_QuitFrame(double now)
{
	int i;
	qbool waiting = false;
	if (!quit_slot)
		return;
	/* Finish other exits first. Only the session issuing quit is allowed an
	 * automatic config save; individual closes never request one. */
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid > 0 && i + 1 != quit_slot) {
			Session_QuitWorker(&workers[i], SESSION_SHUTDOWN, now);
			waiting = true;
		}
	}
	if (waiting)
		return;
	if (!quit_saving) {
		quit_saving = true;
		quit_started = now;
	}
	if (workers[quit_slot - 1].pid > 0) {
		Session_QuitWorker(&workers[quit_slot - 1], SESSION_QUIT_SAVE, now);
	}
	else {
		/* The issuer saved before exiting, or failed; never overwrite it with
		 * coordinator configuration as a fallback. */
		Host_QuitSession(false);
	}
}

void CL_SessionsFrame(void)
{
	session_message_t message;
	double now = Sys_DoubleTime();
	int bytes;
	int i;
	if (shutting_down)
		return;
	if (CL_SessionIsWorker()) {
		while ((bytes = Session_Receive(coordinator_fd, &message)) > 0) {
#ifdef _WIN32
			if (message.type == SESSION_INPUT) {
				Session_ReceiveInput(&message, bytes);
				continue;
			}
#endif
			if (bytes != sizeof(message))
				continue;
			switch (message.type) {
			case SESSION_NOTICE:
				message.notice[sizeof(message.notice) - 1] = 0;
				Con_Printf("%s", message.notice);
				break;
			case SESSION_VIDEO_DETACH:
				if (message.generation <= video_generation)
					break;
				video_generation = message.generation;
				Session_ReleaseInput();
				/* Release GLX resources and close SDL's X connection while the
				 * owner's window is still valid. Moving a live context to another
				 * drawable does not tear down its old GLX drawable/cache entries. */
				VID_Shutdown(true);
				video_detached = true;
				video_ack = SESSION_VIDEO_DETACHED;
				break;
			case SESSION_VIDEO_ATTACH:
				if (message.generation < video_generation)
					break;
				video_generation = message.generation;
				if (video_detached) {
					parent_window = message.window;
					VID_SessionRestart(&message.video, message.slot == local_slot);
					video_detached = false;
				}
				Session_ApplyWindowState(&message);
				video_ack = SESSION_VIDEO_ATTACHED;
				break;
			case SESSION_SLEEP:
				if (video_detached)
					break;
				Session_ReleaseInput();
				Session_Control(coordinator_fd, SESSION_ASLEEP, local_slot);
				break;
			case SESSION_WAKE:
				if (video_detached)
					break;
				VID_SessionSyncWindowSettings(&message.video);
				Session_ApplyWindowState(&message);
#ifdef _WIN32
				input_generation = message.generation;
				input_settings_valid = false;
				Session_WinSetActive(session_window, true);
#endif
				VID_SessionActivate();
#ifndef _WIN32
				session_XSelectInput(display, parent_window, session_events);
				session_XSync(display, False);
#endif
				selected_slot = local_slot;
				Key_ClearStates();
				CL_ClearSessionInput();
				S_StopAllSounds();
				SDL_AtomicSet(&active, 1);
				/* Another engine may have changed the shared title since we last
				 * ran, even if our cached server status is unchanged. */
				CL_UpdateCaption(true);
				break;
			case SESSION_SNAPSHOT:
				Session_ApplyWindowState(&message);
				selected_slot = message.slot;
				memcpy(sessions, message.info, sizeof(sessions));
				break;
			case SESSION_QUIT_SAVE:
				Session_ApplyWindowState(&message);
				Host_QuitSession(true);
				return;
			case SESSION_SHUTDOWN:
				Host_QuitSession(false);
				return;
			case SESSION_DROP_FILE:
				message.path[sizeof(message.path) - 1] = 0;
				Session_LocalDropFile(message.path);
				break;
			}
		}
		if (bytes == 0) {
			Host_QuitSession(false);
			return;
		}
#ifdef _WIN32
		if (CL_SessionIsActive() && session_window) {
			if (caption_pending) {
				memset(&message, 0, sizeof(message));
				message.type = SESSION_CAPTION;
				strlcpy(message.notice, pending_caption, sizeof(message.notice));
				if (Session_Send(coordinator_fd, &message))
					caption_pending = false;
			}
		}
#endif
		if (local_quit_request && Session_Control(coordinator_fd, local_quit_request, local_slot))
			local_quit_request = 0;
		if (startup_video_restart && CL_SessionIsActive() && host_everything_loaded) {
			startup_video_restart = false;
			CL_SessionRestartVideo();
		}
		if (video_ack) {
			memset(&message, 0, sizeof(message));
			message.type = video_ack;
			message.generation = video_generation;
			if (Session_Send(coordinator_fd, &message))
				video_ack = 0;
		}
		if (now >= next_status) {
			memset(&message, 0, sizeof(message));
			message.type = SESSION_READY;
			Session_LocalInfo();
			message.info[local_slot - 1] = sessions[local_slot - 1];
			Session_Send(coordinator_fd, &message);
			next_status = now + 0.2;
		}
		return;
	}
	if (!session_coordinator) {
		Session_LocalInfo();
		return;
	}
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		session_worker_t *worker = &workers[i];
		if (worker->pid > 0 && Session_Reap(worker, false)) {
			worker->pid = 0;
			if (sessions[i].exists) {
				Session_Printf("Session %d closed.\n", i + 1);
				Session_Remove(i + 1);
			}
		}
		if (worker->pid > 0 && worker->closing_started && now - worker->closing_started > 5)
			Session_Terminate(worker->pid, now - worker->closing_started > 10);
		if (!sessions[i].exists || worker->fd < 0)
			continue;
		while ((bytes = Session_Receive(worker->fd, &message)) > 0) {
			if (bytes != sizeof(message))
				continue;
			switch (message.type) {
#ifdef _WIN32
			case SESSION_INPUT_SETTINGS:
				if (i + 1 == selected_slot && Session_InputEnabled() && message.generation == input_generation)
					VID_SessionInputSettings(&message.input_settings);
				break;
#endif
			case SESSION_CAPTION:
				if (i + 1 == selected_slot) {
					message.notice[sizeof(message.notice) - 1] = 0;
					SDL_SetWindowTitle(session_window, message.notice);
				}
				break;
			case SESSION_QUIT:
				Session_BeginQuit(i + 1);
				break;
			case SESSION_WINDOW_QUIT:
				Session_BeginQuit(selected_slot);
				break;
			case SESSION_VIDEO_REQUEST:
				Session_BeginVideoRestart(i + 1, &message.video);
				break;
			case SESSION_VIDEO_DETACHED:
				if (video_phase == VIDEO_DETACHING && message.generation == video_generation) {
					worker->video_ready = true;
				}
				break;
			case SESSION_VIDEO_ATTACHED:
				if (video_phase == VIDEO_ATTACHING && message.generation == video_generation)
					worker->video_ready = true;
				break;
			case SESSION_READY:
				sessions[i] = message.info[i];
				break;
			case SESSION_SELECT:
				CL_SessionSelect(message.slot);
				break;
			case SESSION_ASLEEP:
				if (!quit_slot && sleeping_slot == i + 1)
					Session_Activate(requested_slot);
				break;
			case SESSION_CLOSE_REQUEST:
				Session_Close(message.slot);
				break;
			case SESSION_MINIMIZE:
				VID_Minimize();
				break;
			case SESSION_RESTORE:
				VID_Restore();
				break;
			}
		}
		if (bytes == 0) {
			/* EOF precedes SDL teardown. Wait for process exit before handing
			 * input to another engine; the old SDL instance may still ungrab. */
			Session_ClosePipe(worker->fd);
			worker->fd = -1;
			if (!worker->closing_started)
				worker->closing_started = now;
#ifdef _WIN32
			if (i + 1 == selected_slot)
				Session_ResetInput();
#endif
			continue;
		}
		if (!sessions[i].ready && now - worker->started > 30) {
			/* Keep its slot until reaped: SDL must finish releasing any grab
			 * before another engine can take input ownership. */
			Session_Terminate(worker->pid, now - worker->started > 35);
		}
	}
	for (i = 0; i < CL_MAX_SESSIONS; ++i)
		Session_FlushNotices(&workers[i]);
	if (quit_slot) {
#ifdef _WIN32
		Session_ResetInput();
#endif
		Session_QuitFrame(now);
		return;
	}
	if (!CL_SessionCount()) {
		/* A failed final worker must not leave an inaccessible owner window. */
		Host_QuitSession(false);
		return;
	}
	Session_VideoFrame(now);
	if (video_phase != VIDEO_IDLE)
		return;
	if (!sleeping_slot && requested_slot != selected_slot && sessions[requested_slot - 1].ready) {
		if (!selected_slot) {
			Session_Activate(requested_slot);
		}
		else if (Session_Control(workers[selected_slot - 1].fd, SESSION_SLEEP, selected_slot)) {
			sleeping_slot = selected_slot;
			switch_started = now;
#ifdef _WIN32
			Session_ResetInput();
#endif
		}
	}
	if (sleeping_slot && now - switch_started > 5) {
		/* Never let an unresponsive renderer retain input ownership. */
		Session_Terminate(workers[sleeping_slot - 1].pid, now - switch_started > 10);
	}
#ifdef _WIN32
	Session_FlushInput();
#endif
	if (now >= next_status) {
		memset(&message, 0, sizeof(message));
		message.type = SESSION_SNAPSHOT;
		message.slot = selected_slot ? selected_slot : requested_slot;
		Session_WindowState(&message);
		memcpy(message.info, sessions, sizeof(sessions));
		for (i = 0; i < CL_MAX_SESSIONS; ++i)
			if (sessions[i].exists)
				Session_Send(workers[i].fd, &message);
		next_status = now + 0.2;
	}
}

void CL_SessionsShutdown(void)
{
	int i;
	double deadline;
	if (shutting_down)
		return;
	shutting_down = true;
	if (CL_SessionIsWorker()) {
		Session_ClosePipe(coordinator_fd);
		coordinator_fd = -1;
		return;
	}
	if (!session_coordinator)
		return;
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid > 0) {
			Session_Control(workers[i].fd, SESSION_SHUTDOWN, i + 1);
		}
		if (sessions[i].exists)
			Session_ClosePipe(workers[i].fd);
	}
	/* Let engines send disconnect and finish recordings before destroying their
	 * parent window. Bound the wait, including workers stuck during startup. */
	deadline = Sys_DoubleTime() + 1.0;
	for (i = 0; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid <= 0)
			continue;
		while (!Session_Reap(&workers[i], false)) {
			if (Sys_DoubleTime() >= deadline) {
				Session_Terminate(workers[i].pid, true);
				Session_Reap(&workers[i], true);
				break;
			}
#ifdef _WIN32
			/* Child HWND destruction can send synchronous parent notifications. */
			SDL_PumpEvents();
#endif
			SDL_Delay(10);
		}
		workers[i].pid = 0;
	}
}

#else
qbool CL_SessionSetCaption(const char *caption) { return false; }
qbool CL_SessionRequestWindowAction(qbool restore) { return false; }
void CL_SessionDropFile(const char *path) { Session_LocalDropFile(path); }
qbool CL_SessionRequestQuit(qbool window_close) { return false; }
qbool CL_SessionRestartVideo(void) { return false; }
void CL_SessionsDetachWindow(void) { session_window = NULL; }
SDL_Window *CL_SessionCreateWindow(void) { return NULL; }
void CL_SessionsEarlyInit(void) { }
qbool CL_SessionsInitCoordinator(void) { return false; }
void CL_SessionsAttachWindow(SDL_Window *window) { session_window = window; }
void CL_SessionsFrame(void) { Session_LocalInfo(); }
void CL_SessionsWait(void) { SDL_Delay(1); }
void CL_SessionsShutdown(void) { }
void CL_SessionSelect(int slot) { Con_Printf("Embedded sessions require Windows or Linux/X11.\n"); }
static void Session_Close(int slot) { Con_Printf("Session 1 cannot be closed. Use quit to close the client.\n"); }
#endif

static void Session_Select_f(void)
{
	if (cbuf_current == &cbuf_svc)
		return;
	if (Cmd_Argc() != 2)
		Con_Printf("Usage: session <1-9>\n");
	else
		CL_SessionSelect(atoi(Cmd_Argv(1)));
}

static void Session_Close_f(void)
{
	if (cbuf_current == &cbuf_svc)
		return;
	if (Cmd_Argc() > 2) {
		Con_Printf("Usage: session_close [number]\n");
		return;
	}
	Session_Close(Cmd_Argc() == 2 ? atoi(Cmd_Argv(1)) : local_slot);
}

static void Session_CloseCurrent_f(void)
{
	if (cbuf_current == &cbuf_svc)
		return;
	if (Cmd_Argc() != 1) {
		Con_Printf("Usage: close\n");
		return;
	}
	Session_Close(local_slot);
}

static void Session_List_f(void)
{
	int i;
	cl_session_info_t info;
	char map[sizeof(info.map)];
	for (i = 1; i <= CL_MAX_SESSIONS; ++i)
		if (CL_SessionInfo(i, &info)) {
			char player_count[32] = "";
			char *digit;
			COM_StripExtension(COM_SkipPath(info.map), map, sizeof(map));
			if (info.state == ca_active) {
				if (info.max_players > 0)
					snprintf(player_count, sizeof(player_count), "%d/%d", info.players, info.max_players);
				else
					snprintf(player_count, sizeof(player_count), "%d", info.players);
				if (info.players >= info.max_players) {
					for (digit = player_count; *digit; ++digit)
						if (*digit >= '0' && *digit <= '9')
							*digit = *digit - '0' + 0x12;
				}
			}
			Con_Printf("%c %d: %s &cbbf%s&r %s\n", info.selected ? '*' : ' ', i,
				info.server[0] ? info.server : "console", map, player_count);
		}
}

void CL_SessionsInit(void)
{
	atexit(CL_SessionsShutdown);
	Cmd_AddCommand("session", Session_Select_f);
	Cmd_AddCommand("close", Session_CloseCurrent_f);
	Cmd_AddCommand("session_close", Session_Close_f);
	Cmd_AddCommand("session_list", Session_List_f);
	Session_LocalInfo();
}
