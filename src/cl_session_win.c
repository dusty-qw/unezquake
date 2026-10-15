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

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "quakedef.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include "cl_session_win.h"
#include "cl_session.h"

static HANDLE session_job, coordinator_wake;

/* These handles are local to a worker. The hidden window has neither a parent
 * nor an owner: attaching it to the coordinator would join their input queues.
 * SDL owns only a wrapper for this local window, never the coordinator's HWND. */
static HWND render_window, presentation_window;
static HDC render_dc, presentation_dc;
static qbool presenting;
static BOOL (WINAPI *session_make_current)(HDC, HGLRC);

static void Session_WinError(const char *operation)
{
	SDL_SetError("%s (Windows error %lu)", operation, (unsigned long)GetLastError());
}

qbool Session_WinInit(void)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	coordinator_wake = CreateEvent(&security, FALSE, FALSE, NULL);
	if (!coordinator_wake) {
		Session_WinError("Cannot create coordinator wake event");
		return false;
	}
	session_job = CreateJobObject(NULL, NULL);
	if (!session_job ||
		!SetInformationJobObject(session_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
		!AssignProcessToJobObject(session_job, GetCurrentProcess())) {
		Session_WinError("Cannot establish session process lifetime");
		if (session_job)
			CloseHandle(session_job);
		session_job = NULL;
		CloseHandle(coordinator_wake);
		coordinator_wake = NULL;
		return false;
	}
	/* The coordinator is a member too, so workers inherit membership at birth.
	 * There is no CreateProcess/AssignProcess crash gap. This non-inheritable
	 * handle stays open until coordinator process exit, killing any stragglers. */
	return true;
}

qbool Session_WinValidateWorker(intptr_t pipe, uintptr_t parent, uintptr_t wake)
{
	ULONG server_pid;
	DWORD window_pid, mode, handle_flags;
	BOOL in_job = FALSE;
	if (pipe <= 0 || !wake || !GetHandleInformation((HANDLE)wake, &handle_flags) ||
		!IsWindow((HWND)parent) ||
		GetFileType((HANDLE)pipe) != FILE_TYPE_PIPE ||
		!GetNamedPipeServerProcessId((HANDLE)pipe, &server_pid) ||
		!GetNamedPipeHandleState((HANDLE)pipe, &mode, NULL, NULL, NULL, NULL, 0) ||
		(mode & (PIPE_READMODE_MESSAGE | PIPE_NOWAIT)) != (PIPE_READMODE_MESSAGE | PIPE_NOWAIT) ||
		!IsProcessInJob(GetCurrentProcess(), NULL, &in_job) || !in_job)
		return false;
	GetWindowThreadProcessId((HWND)parent, &window_pid);
	if (window_pid != server_pid)
		return false;
	coordinator_wake = (HANDLE)wake;
	SetHandleInformation(coordinator_wake, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation((HANDLE)pipe, HANDLE_FLAG_INHERIT, 0);
	/* A desktop override must not turn a worker into another SDL backend. */
	SDL_setenv("SDL_VIDEODRIVER", "windows", 1);
	return true;
}

qbool Session_WinSend(intptr_t pipe, const void *message, size_t size)
{
	DWORD written = 0;
	/* Nonblocking message pipes either accept the entire message or zero bytes.
	 * A full queue is retried by the common session state machine. */
	if (pipe <= 0 || !WriteFile((HANDLE)pipe, message, (DWORD)size, &written, NULL) || written != size)
		return false;
	if (CL_SessionIsWorker())
		SetEvent(coordinator_wake);
	return true;
}

int Session_WinReceive(intptr_t pipe, void *message, size_t size)
{
	DWORD read = 0, error;
	if (ReadFile((HANDLE)pipe, message, (DWORD)size, &read, NULL))
		return (int)read;
	error = GetLastError();
	return error == ERROR_NO_DATA ? -1 : 0;
}

void Session_WinClose(intptr_t pipe)
{
	if (pipe > 0)
		CloseHandle((HANDLE)pipe);
}

void Session_WinTerminate(intptr_t process)
{
	if (process > 0)
		TerminateProcess((HANDLE)process, EXIT_FAILURE);
}

qbool Session_WinReap(intptr_t process, qbool wait)
{
	HANDLE handle = (HANDLE)process;
	if (wait) {
		/* Keep servicing messages from children tearing down their HWNDs. */
		while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
			SDL_PumpEvents();
	}
	if (WaitForSingleObject(handle, 0) != WAIT_OBJECT_0)
		return false;
	CloseHandle(handle);
	return true;
}

/* Match the Windows argument parser: double backslashes before a closing
 * quote, and escape embedded quotes. Paths ending in a slash remain intact. */
static qbool Session_WinArgument(char *command, size_t capacity, const char *argument)
{
	size_t used = strlen(command), slashes;
	const char *p = argument;
	if (used + strlen(argument) * 2 + 4 >= capacity)
		return false;
	command[used++] = ' ';
	command[used++] = '"';
	for (;;) {
		slashes = 0;
		while (*p == '\\') {
			++slashes;
			++p;
		}
		if (!*p || *p == '"')
			slashes *= 2;
		while (slashes--)
			command[used++] = '\\';
		if (!*p)
			break;
		if (*p == '"')
			command[used++] = '\\';
		command[used++] = *p++;
	}
	command[used++] = '"';
	command[used] = 0;
	return true;
}

qbool Session_WinStart(int slot, uintptr_t parent, SDL_Window *window, intptr_t *pipe, intptr_t *process)
{
	static unsigned int serial;
	char pipe_name[128], executable[MAX_OSPATH], number[24], parent_arg[32], pipe_arg[32], w[24], h[24];
	char *command = NULL;
	char wake_arg[32];
	HANDLE inherited[2];
	const char *arguments[32];
	int argc = 0, i, width, height;
	DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT, executable_length;
	DWORD error = ERROR_SUCCESS;
	HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
	SECURITY_ATTRIBUTES security = { sizeof(security), NULL, TRUE };
	STARTUPINFOEXA startup;
	PROCESS_INFORMATION child;
	SIZE_T attribute_size = 0;
	qbool attributes_initialized = false, success = false;
	memset(&startup, 0, sizeof(startup));
	memset(&child, 0, sizeof(child));
	startup.StartupInfo.cb = sizeof(startup);

	snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\unezquake-session-%lu-%u",
		(unsigned long)GetCurrentProcessId(), ++serial);
	server = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, 256 * 1024, 256 * 1024, 0, NULL);
	if (server == INVALID_HANDLE_VALUE)
		goto done;
	client = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, &security, OPEN_EXISTING, 0, NULL);
	if (client == INVALID_HANDLE_VALUE || !SetNamedPipeHandleState(client, &mode, NULL, NULL))
		goto done;
	if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
		goto done;
	executable_length = GetModuleFileNameA(NULL, executable, sizeof(executable));
	if (!executable_length || executable_length >= sizeof(executable))
		goto done;

	InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
	startup.lpAttributeList = Q_malloc(attribute_size);
	if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size))
		goto done;
	attributes_initialized = true;
	/* Inherit only this pipe and the coordinator's notification event. */
	inherited[0] = client;
	inherited[1] = coordinator_wake;
	if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		inherited, sizeof(inherited), NULL, NULL))
		goto done;

	SDL_GetWindowSize(window, &width, &height);
	snprintf(wake_arg, sizeof(wake_arg), "%llu", (unsigned long long)(uintptr_t)coordinator_wake);
	snprintf(number, sizeof(number), "%d", slot);
	snprintf(parent_arg, sizeof(parent_arg), "%llu", (unsigned long long)parent);
	snprintf(pipe_arg, sizeof(pipe_arg), "%llu", (unsigned long long)(uintptr_t)client);
	snprintf(w, sizeof(w), "%d", width);
	snprintf(h, sizeof(h), "%d", height);
	command = Q_calloc(32768, 1);
	if (!Session_WinArgument(command, 32768, executable))
		goto arguments_too_long;
	/* CreateProcess expects the executable at the start, without leading space. */
	memmove(command, command + 1, strlen(command));
	if (slot == 1) {
		for (i = 1; i < COM_Argc(); ++i)
			if (!Session_WinArgument(command, 32768, COM_Argv(i)))
				goto arguments_too_long;
	}
	else {
		arguments[argc++] = "-basedir"; arguments[argc++] = com_basedir;
		arguments[argc++] = "-window";
		arguments[argc++] = "-width"; arguments[argc++] = w;
		arguments[argc++] = "-height"; arguments[argc++] = h;
		arguments[argc++] = "+set"; arguments[argc++] = "cl_net_clientport"; arguments[argc++] = "0";
		arguments[argc++] = "+set"; arguments[argc++] = "vid_renderer"; arguments[argc++] = Cvar_String("vid_renderer");
	}
	arguments[argc++] = "-session-worker";
	arguments[argc++] = pipe_arg; arguments[argc++] = number; arguments[argc++] = parent_arg;
	arguments[argc++] = "-session-wake"; arguments[argc++] = wake_arg;
	arguments[argc++] = "+set"; arguments[argc++] = "sys_inactivesleep"; arguments[argc++] = "0";
	for (i = 0; i < argc; ++i)
		if (!Session_WinArgument(command, 32768, arguments[i]))
			goto arguments_too_long;
	if (!CreateProcessA(executable, command, NULL, NULL, TRUE,
		EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &child))
		goto done;
	CloseHandle(child.hThread);
	*pipe = (intptr_t)server;
	*process = (intptr_t)child.hProcess;
	success = true;
	goto done;

arguments_too_long:
	SetLastError(ERROR_BAD_LENGTH);
done:
	error = GetLastError();
	if (attributes_initialized)
		DeleteProcThreadAttributeList(startup.lpAttributeList);
	Q_free(startup.lpAttributeList);
	Q_free(command);
	if (client != INVALID_HANDLE_VALUE)
		CloseHandle(client);
	if (!success) {
		if (server != INVALID_HANDLE_VALUE)
			CloseHandle(server);
		SetLastError(error);
		Session_WinError("Cannot launch session worker");
	}
	return success;
}

uintptr_t Session_WinWindowHandle(SDL_Window *window)
{
	SDL_SysWMinfo wm;
	SDL_VERSION(&wm.version);
	if (!window || !SDL_GetWindowWMInfo(window, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS)
		return 0;
	return (uintptr_t)wm.info.win.window;
}

void Session_WinDestroyWindow(SDL_Window *window)
{
	/* The caller deletes its GL context before releasing either drawable. */
	if (window)
		SDL_DestroyWindow(window);
	if (render_dc)
		ReleaseDC(render_window, render_dc);
	if (render_window)
		DestroyWindow(render_window);
	if (presentation_dc)
		ReleaseDC(presentation_window, presentation_dc);
	render_window = presentation_window = NULL;
	render_dc = presentation_dc = NULL;
	presenting = false;
	session_make_current = NULL;
}

SDL_Window *Session_WinCreateWindow(uintptr_t parent)
{
	static const char class_name[] = "unezQuakeSessionDrawable";
	WNDCLASSA wc;
	RECT rect;
	POINT origin = { 0, 0 };
	PIXELFORMATDESCRIPTOR format;
	int pixel_format;
	SDL_Window *window;

	if (render_window || presentation_dc) {
		SDL_SetError("Session drawable was not released before recreation");
		return NULL;
	}
	presentation_window = (HWND)parent;
	if (!IsWindow(presentation_window) || !GetClientRect(presentation_window, &rect) ||
		!ClientToScreen(presentation_window, &origin)) {
		SDL_SetError("The session's application window no longer exists");
		presentation_window = NULL;
		return NULL;
	}
	presentation_dc = GetDC(presentation_window);
	pixel_format = presentation_dc ? GetPixelFormat(presentation_dc) : 0;
	if (!pixel_format || !DescribePixelFormat(presentation_dc, pixel_format, sizeof(format), &format)) {
		Session_WinError("Cannot inspect the shared window's pixel format");
		goto failed;
	}

	memset(&wc, 0, sizeof(wc));
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = class_name;
	if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		Session_WinError("Cannot register the private session drawable");
		goto failed;
	}
	/* A hidden, unowned top-level window on the same display supplies SDL's
	 * context creation and size bookkeeping. Inactive engines render here.
	 * Matching the owner's exact pixel format permits WGL to bind the same
	 * context to the visible drawable without copies or driver extensions. */
	render_window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, "",
		WS_POPUP | WS_DISABLED | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		origin.x, origin.y, rect.right, rect.bottom, NULL, NULL, wc.hInstance, NULL);
	if (!render_window || !(render_dc = GetDC(render_window)) ||
		!SetPixelFormat(render_dc, pixel_format, &format)) {
		Session_WinError("Cannot create the private session drawable");
		goto failed;
	}
	SDL_SetHintWithPriority(SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL, "1", SDL_HINT_OVERRIDE);
	window = SDL_CreateWindowFrom(render_window);
	if (!window)
		goto failed;
	/* A window's pixel format is immutable. SDL may attempt its own format
	 * selection for a foreign window; it must not replace our exact match. */
	if (GetPixelFormat(render_dc) != pixel_format) {
		SDL_SetError("Session drawable does not match the shared pixel format");
		SDL_DestroyWindow(window);
		goto failed;
	}
	SDL_ClearError();
	return window;

failed:
	{
		char error[512];
		strlcpy(error, SDL_GetError(), sizeof(error));
		Session_WinDestroyWindow(NULL);
		SDL_SetError("%s", error);
	}
	return NULL;
}

void Session_WinGeometry(SDL_Window *window, int width, int height)
{
	RECT rect;
	POINT origin = { 0, 0 };
	if (window && render_window && !IsIconic(presentation_window) && width > 0 && height > 0 &&
		GetWindowRect(render_window, &rect) && ClientToScreen(presentation_window, &origin) &&
		(rect.left != origin.x || rect.top != origin.y ||
		 rect.right - rect.left != width || rect.bottom - rect.top != height)) {
		SetWindowPos(render_window, NULL, origin.x, origin.y, width, height,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

void Session_WinSetActive(SDL_Window *window, qbool active)
{
	SDL_GLContext context = SDL_GL_GetCurrentContext();
	if (!window || !context)
		return;
	if (!session_make_current)
		session_make_current = (BOOL (WINAPI *)(HDC, HGLRC))SDL_GL_GetProcAddress("wglMakeCurrent");
	/* SDL continues to track the local wrapper for sizing and context lifetime.
	 * Only the WGL drawing target changes; no HWND is shown, reparented, or
	 * activated. The coordinator waits for glFinish + ASLEEP before WAKE. */
	if (!session_make_current || !session_make_current(active ? presentation_dc : render_dc, (HGLRC)context))
		Sys_Error("Cannot bind session %s drawable (Windows error %lu)",
			active ? "shared" : "private", (unsigned long)GetLastError());
	presenting = active;
}

void Session_WinSwapBuffers(void)
{
	if (!presenting || !presentation_dc)
		return;
	if (!SwapBuffers(presentation_dc))
		Sys_Error("Cannot present session frame (Windows error %lu)", (unsigned long)GetLastError());
}

void Session_WinWait(const intptr_t *processes, int count, unsigned int timeout)
{
	HANDLE handles[CL_MAX_SESSIONS + 1];
	int i;
	handles[0] = coordinator_wake;
	for (i = 0; i < count; ++i)
		handles[i + 1] = (HANDLE)processes[i];
	/* Wake for native input, worker messages, or process exits. The timeout is
	 * only for maintenance deadlines; it never delays arriving input. */
	MsgWaitForMultipleObjectsEx(count + 1, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

#endif
