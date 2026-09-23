/* Independent engines, sharing one native drawable. No game-state pointers
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
void IN_DeactivateMouse(void);

static cl_session_info_t sessions[CL_MAX_SESSIONS];
static int local_slot = 1, selected_slot = 1;
static SDL_atomic_t active = { 1 }; /* Also read by the audio callback. */
static SDL_Window *session_window;
static qbool worker_window_fullscreen;
static qbool worker_window_minimized;

int CL_SessionSelected(void) { return selected_slot; }
int CL_SessionNumber(void) { return local_slot; }
qbool CL_SessionIsWorker(void) { return local_slot != 1; }
qbool CL_SessionIsActive(void) { return SDL_AtomicGet(&active) != 0; }
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
	cl_session_info_t *info = &sessions[local_slot - 1];
	extern double connect_time;
	memset(info, 0, sizeof(*info));
	info->slot = local_slot;
	info->exists = true;
	info->ready = host_everything_loaded;
	info->selected = selected_slot == local_slot;
	info->state = cls.state;
	info->connecting = cls.state == ca_disconnected && connect_time != 0;
	info->spectator = cl.spectator != 0;
	info->demo = cls.demoplayback;
	strlcpy(info->server, cls.servername, sizeof(info->server));
	strlcpy(info->name, Info_ValueForKey(cl.serverinfo, "hostname"), sizeof(info->name));
	strlcpy(info->map, cl.model_name[1], sizeof(info->map));
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

qbool CL_SessionVideoRestartAllowed(void)
{
	return !CL_SessionIsWorker() && CL_SessionCount() <= 1;
}

#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 34)
#define SESSION_HAVE_SPAWN_CLOSEFROM
#endif
#endif

enum { SESSION_READY = 1, SESSION_SELECT, SESSION_SLEEP, SESSION_ASLEEP,
	SESSION_WAKE, SESSION_SNAPSHOT, SESSION_CLOSE, SESSION_MINIMIZE, SESSION_RESTORE };
typedef struct {
	int type, slot;
	qbool fullscreen;
	qbool minimized;
	int x, y, width, height;
	unsigned long window;
	cl_session_info_t info[CL_MAX_SESSIONS];
} session_message_t;
typedef struct {
	int fd;
	pid_t pid;
	Window window;
	double started;
} session_worker_t;

static session_worker_t workers[CL_MAX_SESSIONS];
static int coordinator_fd = -1;
static int requested_slot = 1, sleeping_slot;
static double switch_started, next_status;
static Window parent_window;
static Display *display;
static qbool supported, shutting_down;
static void *xlib;
static int (*session_XSelectInput)(Display *, Window, long);
static Status (*session_XGetWindowAttributes)(Display *, Window, XWindowAttributes *);
static long session_events;
static int (*session_XSync)(Display *, Bool);
static int (*session_XPutBackEvent)(Display *, XEvent *);

static void Session_WindowState(session_message_t *message)
{
	message->fullscreen = CL_SessionWindowIsFullscreen();
	message->minimized = CL_SessionWindowIsMinimized();
	if (session_window) {
		SDL_GetWindowPosition(session_window, &message->x, &message->y);
		SDL_GetWindowSize(session_window, &message->width, &message->height);
	}
}

static void Session_ApplyWindowState(const session_message_t *message)
{
	extern qbool Minimized, scr_skipupdate;
	int x, y, width, height;
	XEvent event;
	worker_window_fullscreen = message->fullscreen;
	worker_window_minimized = message->minimized;
	if (Minimized && !worker_window_minimized)
		scr_skipupdate = false;
	Minimized = worker_window_minimized;
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
}

static qbool Session_Send(int fd, const session_message_t *message)
{
	return send(fd, message, sizeof(*message), MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(*message);
}

static qbool Session_Control(int fd, int type, int slot)
{
	session_message_t message;
	memset(&message, 0, sizeof(message));
	message.type = type;
	message.slot = slot;
	Session_WindowState(&message);
	return Session_Send(fd, &message);
}

qbool CL_SessionRequestWindowAction(qbool restore)
{
	if (!CL_SessionIsWorker())
		return false;
	if (!Session_Control(coordinator_fd, restore ? SESSION_RESTORE : SESSION_MINIMIZE, local_slot))
		Con_Printf("Window request could not be queued.\n");
	return true;
}

static void Session_ReleaseInput(void)
{
	VID_SessionRelease();
	SDL_AtomicSet(&active, 0);
	Key_ClearStates();
	CL_ClearSessionInput();
	IN_DeactivateMouse();
	S_StopAllSounds();
	if (display) {
		session_XSelectInput(display, parent_window, session_events & ~ButtonPressMask);
		session_XSync(display, False);
	}
}

void CL_SessionsEarlyInit(void)
{
	int i = COM_FindParm("-session-worker");
	int type;
	socklen_t size = sizeof(type);
	if (!i)
		return;
	if (i + 3 >= COM_Argc())
		exit(EXIT_FAILURE);
	coordinator_fd = atoi(COM_Argv(i + 1));
	local_slot = atoi(COM_Argv(i + 2));
	parent_window = strtoul(COM_Argv(i + 3), NULL, 10);
	if (coordinator_fd < 3 || local_slot < 2 || local_slot > CL_MAX_SESSIONS || !parent_window ||
		getsockopt(coordinator_fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET)
		exit(EXIT_FAILURE);
	fcntl(coordinator_fd, F_SETFD, FD_CLOEXEC);
	SDL_AtomicSet(&active, 0);
}

SDL_Window *CL_SessionCreateWindow(void)
{
	/* Every engine presents to the original top-level drawable. Child windows
	 * can be composited at a different cadence even when their swap FPS is high.
	 * SDL's foreign-window wrapper never destroys the owner's native window. */
	SDL_SetHint(SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL, "1");
	return SDL_CreateWindowFrom((void *)(uintptr_t)parent_window);
}

void CL_SessionsAttachWindow(SDL_Window *window)
{
	SDL_SysWMinfo wm;
	XWindowAttributes attributes;
	session_window = window;
	SDL_VERSION(&wm.version);
	supported = SDL_GetWindowWMInfo(window, &wm) && wm.subsystem == SDL_SYSWM_X11;
	if (!supported) {
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
	}
	session_XSync(display, False);
}

static qbool Session_Start(int slot)
{
	int sockets[2], width, height, result;
	char fd[24], number[24], parent[32], w[24], h[24];
	char *argv[32];
	char **child_env;
	int argc = 0;
	int env_count;
	posix_spawn_file_actions_t actions;
	session_worker_t *worker = &workers[slot - 1];
	if (worker->pid > 0) {
		Con_Printf("Session %d is still closing.\n", slot);
		return false;
	}
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) {
		Con_Printf("Cannot create session: %s\n", strerror(errno));
		return false;
	}
	SDL_GetWindowSize(session_window, &width, &height);
	strlcpy(fd, "3", sizeof(fd));
	snprintf(number, sizeof(number), "%d", slot);
	snprintf(parent, sizeof(parent), "%lu", parent_window);
	snprintf(w, sizeof(w), "%d", width);
	snprintf(h, sizeof(h), "%d", height);
	argv[argc++] = "/proc/self/exe";
	argv[argc++] = "-session-worker";
	argv[argc++] = fd; argv[argc++] = number; argv[argc++] = parent;
	argv[argc++] = "-basedir"; argv[argc++] = com_basedir;
	argv[argc++] = "-window";
	argv[argc++] = "-width"; argv[argc++] = w;
	argv[argc++] = "-height"; argv[argc++] = h;
	argv[argc++] = "-nohwgamma";
	argv[argc++] = "-noconinput";
	argv[argc++] = "-nostdout";
	argv[argc++] = "+set"; argv[argc++] = "cl_net_clientport"; argv[argc++] = "0";
	argv[argc++] = "+set"; argv[argc++] = "vid_renderer"; argv[argc++] = Cvar_String("vid_renderer");
	argv[argc++] = "+set"; argv[argc++] = "sys_inactivesleep"; argv[argc++] = "0";
	argv[argc] = NULL;
	/* Do not inherit the owner's network sockets, downloads, or GPU handles. */
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
#ifdef SESSION_HAVE_SPAWN_CLOSEFROM
	posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#else
	{
		DIR *dir = opendir("/proc/self/fd");
		struct dirent *entry;
		if (!dir) {
			posix_spawn_file_actions_destroy(&actions);
			close(sockets[0]); close(sockets[1]);
			Con_Printf("Cannot enumerate file descriptors for session.\n");
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
	for (result = 0; result < env_count; ++result)
		child_env[result] = !strncmp(environ[result], "SDL_VIDEODRIVER=", 16) ? "SDL_VIDEODRIVER=x11" : environ[result];
	child_env[env_count] = "SDL_VIDEODRIVER=x11";
	child_env[env_count + 1] = NULL;
	result = posix_spawn(&worker->pid, "/proc/self/exe", &actions, NULL, argv, child_env);
	Q_free(child_env);
	posix_spawn_file_actions_destroy(&actions);
	close(sockets[1]);
	if (result) {
		close(sockets[0]);
		Con_Printf("Cannot start session: %s\n", strerror(result));
		return false;
	}
	worker->fd = sockets[0];
	worker->started = Sys_DoubleTime();
	worker->window = 0;
	memset(&sessions[slot - 1], 0, sizeof(sessions[slot - 1]));
	sessions[slot - 1].slot = slot;
	sessions[slot - 1].exists = true;
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	Con_Printf("Starting session %d...\n", slot);
	return true;
}

static void Session_Activate(int slot)
{
	selected_slot = slot;
	sleeping_slot = 0;
	if (slot == 1) {
		VID_SessionActivate();
		session_XSelectInput(display, parent_window, session_events);
		session_XSync(display, False);
		SDL_AtomicSet(&active, 1);
		CL_ClearSessionInput();
		S_StopAllSounds();
		CL_UpdateCaption(true);
	}
	else {
		Session_Control(workers[slot - 1].fd, SESSION_WAKE, slot);
	}
	next_status = 0;
}

void CL_SessionSelect(int slot)
{
	if (slot < 1 || slot > CL_MAX_SESSIONS) {
		Con_Printf("Session must be between 1 and %d.\n", CL_MAX_SESSIONS);
		return;
	}
	if (CL_SessionIsWorker()) {
		if (!Session_Control(coordinator_fd, SESSION_SELECT, slot))
			Con_Printf("Session switch could not be queued.\n");
		return;
	}
	if (!supported) {
		Con_Printf("Sessions require Linux with SDL's X11 driver. Launch with SDL_VIDEODRIVER=x11 (also works under XWayland).\n");
		return;
	}
	if (sleeping_slot)
		return;
	if (!sessions[slot - 1].exists && !Session_Start(slot))
		return;
	requested_slot = slot;
}

static void Session_Remove(int slot)
{
	session_worker_t *worker = &workers[slot - 1];
	if (worker->fd >= 0)
		close(worker->fd);
	worker->fd = -1;
	worker->window = 0;
	memset(&sessions[slot - 1], 0, sizeof(sessions[slot - 1]));
	if (requested_slot == slot)
		requested_slot = 1;
	if (selected_slot == slot || sleeping_slot == slot)
		Session_Activate(1);
	next_status = 0;
}

static void Session_Close(int slot)
{
	if (slot <= 1 || slot > CL_MAX_SESSIONS) {
		Con_Printf("Use disconnect for session 1; quit closes the whole client.\n");
		return;
	}
	if (CL_SessionIsWorker())
		Session_Control(coordinator_fd, SESSION_CLOSE, slot);
	else if (sessions[slot - 1].exists)
		Session_Control(workers[slot - 1].fd, SESSION_CLOSE, slot);
}

void CL_SessionsFrame(void)
{
	session_message_t message;
	double now = Sys_DoubleTime();
	ssize_t bytes;
	int i;
	if (shutting_down)
		return;
	Session_LocalInfo();
	if (CL_SessionIsWorker()) {
		while ((bytes = recv(coordinator_fd, &message, sizeof(message), MSG_DONTWAIT)) > 0) {
			if (bytes != sizeof(message))
				continue;
			switch (message.type) {
			case SESSION_SLEEP:
				Session_ReleaseInput();
				Session_Control(coordinator_fd, SESSION_ASLEEP, local_slot);
				break;
			case SESSION_WAKE:
				Session_ApplyWindowState(&message);
				VID_SessionActivate();
				session_XSelectInput(display, parent_window, session_events);
				session_XSync(display, False);
				selected_slot = local_slot;
				Key_ClearStates();
				CL_ClearSessionInput();
				S_StopAllSounds();
				SDL_AtomicSet(&active, 1);
				break;
			case SESSION_SNAPSHOT:
				Session_ApplyWindowState(&message);
				selected_slot = message.slot;
				memcpy(sessions, message.info, sizeof(sessions));
				break;
			case SESSION_CLOSE:
				Host_Quit();
				return;
			}
		}
		if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
			Host_Quit();
			return;
		}
		if (now >= next_status) {
			SDL_SysWMinfo wm;
			memset(&message, 0, sizeof(message));
			message.type = SESSION_READY;
			SDL_VERSION(&wm.version);
			if (SDL_GetWindowWMInfo(session_window, &wm))
				message.window = wm.info.x11.window;
			Session_LocalInfo();
			message.info[local_slot - 1] = sessions[local_slot - 1];
			Session_Send(coordinator_fd, &message);
			next_status = now + 0.2;
		}
		return;
	}
	for (i = 1; i < CL_MAX_SESSIONS; ++i) {
		session_worker_t *worker = &workers[i];
		if (worker->pid > 0 && waitpid(worker->pid, NULL, WNOHANG) == worker->pid) {
			worker->pid = 0;
			if (sessions[i].exists) {
				Con_Printf("Session %d closed.\n", i + 1);
				Session_Remove(i + 1);
			}
		}
		if (!sessions[i].exists || worker->fd < 0)
			continue;
		while ((bytes = recv(worker->fd, &message, sizeof(message), MSG_DONTWAIT)) > 0) {
			if (bytes != sizeof(message))
				continue;
			switch (message.type) {
			case SESSION_READY:
				worker->window = message.window;
				sessions[i] = message.info[i];
				break;
			case SESSION_SELECT:
				CL_SessionSelect(message.slot);
				break;
			case SESSION_ASLEEP:
				if (sleeping_slot == i + 1)
					Session_Activate(requested_slot);
				break;
			case SESSION_CLOSE:
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
			close(worker->fd);
			worker->fd = -1;
			continue;
		}
		if (!sessions[i].ready && now - worker->started > 30) {
			Con_Printf("Session %d failed to start.\n", i + 1);
			kill(worker->pid, SIGTERM);
			Session_Remove(i + 1);
		}
	}
	if (!sleeping_slot && requested_slot != selected_slot && sessions[requested_slot - 1].ready) {
		if (selected_slot == 1) {
			Session_ReleaseInput();
			Session_Activate(requested_slot);
		}
		else if (Session_Control(workers[selected_slot - 1].fd, SESSION_SLEEP, selected_slot)) {
			sleeping_slot = selected_slot;
			switch_started = now;
		}
	}
	if (sleeping_slot && now - switch_started > 5) {
		/* Never let an unresponsive renderer retain input ownership. */
		kill(workers[sleeping_slot - 1].pid, SIGTERM);
	}
	if (now >= next_status) {
		memset(&message, 0, sizeof(message));
		message.type = SESSION_SNAPSHOT;
		message.slot = selected_slot;
		Session_WindowState(&message);
		memcpy(message.info, sessions, sizeof(sessions));
		for (i = 1; i < CL_MAX_SESSIONS; ++i)
			if (sessions[i].exists)
				Session_Send(workers[i].fd, &message);
		if (selected_slot != 1 && workers[selected_slot - 1].window && workers[selected_slot - 1].fd >= 0) {
			char title[384];
			cl_session_info_t *info = &sessions[selected_slot - 1];
			snprintf(title, sizeof(title), "unezQuake - Session %d - %s", selected_slot,
				info->server[0] ? info->server : "Console");
			SDL_SetWindowTitle(session_window, title);
		}
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
		close(coordinator_fd);
		coordinator_fd = -1;
		return;
	}
	for (i = 1; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid > 0) {
			Session_Control(workers[i].fd, SESSION_CLOSE, i + 1);
		}
		if (sessions[i].exists)
			close(workers[i].fd);
	}
	/* Let engines send disconnect and finish recordings before destroying their
	 * parent window. Bound the wait, including workers stuck during startup. */
	deadline = Sys_DoubleTime() + 1.0;
	for (i = 1; i < CL_MAX_SESSIONS; ++i) {
		if (workers[i].pid <= 0)
			continue;
		while (waitpid(workers[i].pid, NULL, WNOHANG) == 0) {
			if (Sys_DoubleTime() >= deadline) {
				kill(workers[i].pid, SIGKILL);
				waitpid(workers[i].pid, NULL, 0);
				break;
			}
			SDL_Delay(10);
		}
		workers[i].pid = 0;
	}
}

#else
qbool CL_SessionRequestWindowAction(qbool restore) { return false; }
SDL_Window *CL_SessionCreateWindow(void) { return NULL; }
void CL_SessionsEarlyInit(void) { }
void CL_SessionsAttachWindow(SDL_Window *window) { session_window = window; }
void CL_SessionsFrame(void) { Session_LocalInfo(); }
void CL_SessionsShutdown(void) { }
void CL_SessionSelect(int slot) { Con_Printf("Embedded sessions currently require Linux/X11.\n"); }
static void Session_Close(int slot) { Con_Printf("No additional sessions.\n"); }
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
	Session_Close(Cmd_Argc() == 2 ? atoi(Cmd_Argv(1)) : local_slot);
}

static void Session_List_f(void)
{
	int i;
	cl_session_info_t info;
	for (i = 1; i <= CL_MAX_SESSIONS; ++i)
		if (CL_SessionInfo(i, &info))
			Con_Printf("%c %d: %s %s %s\n", info.selected ? '*' : ' ', i,
				!info.ready ? "starting" : info.state == ca_active ? "active" :
				info.state >= ca_connected ? "connected" : info.connecting ? "connecting" : "disconnected",
				info.server[0] ? info.server : "Console", info.map);
}

void CL_SessionsInit(void)
{
	atexit(CL_SessionsShutdown);
	Cmd_AddCommand("session", Session_Select_f);
	Cmd_AddCommand("session_close", Session_Close_f);
	Cmd_AddCommand("session_list", Session_List_f);
	Session_LocalInfo();
}
