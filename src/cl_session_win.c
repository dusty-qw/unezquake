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
	arguments[argc++] = "-nohwgamma";
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

SDL_Window *Session_WinCreateWindow(uintptr_t parent)
{
	RECT rect;
	HWND child;
	SDL_Window *window;
	if (!IsWindow((HWND)parent) || !GetClientRect((HWND)parent, &rect)) {
		SDL_SetError("The session's application window no longer exists");
		return NULL;
	}
	/* Cross-process SDL_CreateWindowFrom cannot subclass the owner's HWND.
	 * Give SDL a window it owns, embedded before it can become visible. The
	 * executable applies the same DPI awareness in coordinator and workers.
	 * WS_DISABLED leaves keyboard/mouse focus with the main window. */
	window = SDL_CreateWindow("", 0, 0, rect.right, rect.bottom,
		SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN);
	if (!window)
		return NULL;
	child = (HWND)Session_WinWindowHandle(window);
	SetWindowLongPtr(child, GWL_STYLE, WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
	SetWindowLongPtr(child, GWL_EXSTYLE, GetWindowLongPtr(child, GWL_EXSTYLE) & ~WS_EX_APPWINDOW);
	SetLastError(ERROR_SUCCESS);
	if (!SetParent(child, (HWND)parent) && GetLastError() != ERROR_SUCCESS) {
		DWORD error = GetLastError();
		SDL_DestroyWindow(window);
		SetLastError(error);
		Session_WinError("Cannot embed session render window");
		return NULL;
	}
	SetWindowPos(child, NULL, 0, 0, rect.right, rect.bottom,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	return window;
}

void Session_WinGeometry(SDL_Window *window, int width, int height)
{
	HWND child = (HWND)Session_WinWindowHandle(window);
	RECT rect;
	if (width > 0 && height > 0 && GetClientRect(child, &rect) &&
		(rect.right != width || rect.bottom != height))
		SetWindowPos(child, NULL, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
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

void Session_WinSetActive(SDL_Window *window, qbool active)
{
	HWND child = (HWND)Session_WinWindowHandle(window);
	if (!child)
		return;
	/* Use native show-without-activation: SDL_ShowWindow raises a top-level
	 * window, while this is a child in another process's application window. */
	ShowWindow(child, active ? SW_SHOWNOACTIVATE : SW_HIDE);
}
#endif
