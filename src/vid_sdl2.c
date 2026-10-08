/*
   ===========================================================================
   Copyright (C) 1999-2005 Id Software, Inc.

   This file is part of Quake III Arena source code.

   Quake III Arena source code is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the License,
   or (at your option) any later version.

   Quake III Arena source code is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Foobar; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
   ===========================================================================

 */

#include "quakedef.h"

#include <SDL.h>
#include <SDL_syswm.h>

#ifdef X11_GAMMA_WORKAROUND
#include <X11/extensions/xf86vmode.h>
#endif

#ifdef _WIN32
#include <windows.h>

#endif

#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <OpenGL/OpenGL.h>
#include "in_osx.h"
#else
#include <GL/gl.h>
#endif

#include "ezquake-icon.c"
#include "keys.h"
#include "cl_session.h"
#include "tr_types.h"
#include "input.h"
#include "rulesets.h"
#include "utils.h"
#include "textencoding.h"

#include "r_local.h"
#include "gl_framebuffer.h"
#include "r_texture.h"
#include "qmb_particles.h"
#include "r_state.h"
#include "r_buffers.h"
#include "r_renderer.h"
#include "r_program.h"
#include "menu.h"
#include "menu_ui_bridge.h"

SDL_GLContext GLM_SDL_CreateContext(SDL_Window* window);
SDL_GLContext GLC_SDL_CreateContext(SDL_Window* window);

#ifdef __linux__
// This is hack to ignore keyboard events we receive between FOCUS_GAINED & TAKE_FOCUS
// Without it the keys you press to switch back to ezQuake will fire, which is probably not desired
// Affects X11 only - might also be needed on FreeBSD/OSX?
static qbool block_keyboard_input = false;
#endif
#ifdef __APPLE__
static int deadkey_modifiers_held_down = 0;
static cvar_t in_ignore_deadkeys = { "in_ignore_deadkeys", "1", CVAR_SILENT };

#define APPLE_RALT_HELD_DOWN 1
#define APPLE_LALT_HELD_DOWN 2
#endif

#define	WINDOW_CLASS_NAME	"ezQuake"

#define VID_RENDERER_MIN 0
#define VID_RENDERER_MAX 1

#define VID_MULTISAMPLED   1
#define VID_ACCELERATED    2
#define VID_DEPTHBUFFER24  4
#define VID_GAMMACORRECTED 8

/* FIXME: This should be in a header file and it probably shouldn't be called TP_
 *        since there are a lot of triggers that has nothing to do with teamplay.
 *        Should probably split them
 */
extern void TP_ExecTrigger(const char *);

static void in_raw_callback(cvar_t *var, char *value, qbool *cancel);
static void in_grab_windowed_mouse_callback(cvar_t *var, char *value, qbool *cancel);
static void conres_changed_callback (cvar_t *var, char *string, qbool *cancel);
static void framebuffer_smooth_changed_callback(cvar_t* var, char* string, qbool* cancel);
static void vid_reload_callback(cvar_t* var, char* string, qbool* cancel);
static void GrabMouse(qbool grab, qbool raw);
static qbool session_alt_tab_enabled = true;
static qbool session_alt_tab_initialized;

#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)
static qbool session_swap_control_checked;
static SDL_SysWMinfo session_swap_wm;
static int64_t (*session_swap_msc)(Display *, unsigned long, int64_t, int64_t, int64_t);
#endif
static void HandleEvents(void);
static void VID_UpdateConRes(void);
void IN_Restart_f(void);

static SDL_Window       *sdl_window;
static SDL_GLContext    sdl_context;

glconfig_t glConfig;
qbool vid_hwgamma_enabled = false;
static qbool mouse_active = false;
qbool mouseinitialized = false; // unfortunately non static, lame...
int mx, my;
static int old_x = 0, old_y = 0;
extern double cursor_x, cursor_y;

qbool ActiveApp = true;
qbool Minimized = false;

double vid_vsync_lag;
double vid_last_swap_time;

static SDL_DisplayMode *modelist;
static int modelist_count;

#ifdef X11_GAMMA_WORKAROUND
static unsigned short sysramps[3*4096];
#endif

qbool vid_initialized = false;

static int last_working_width;
static int last_working_height;
static int last_working_hz;
static int last_working_display;
static qbool last_working_values = false;

// deferred events (Sys_SendDeferredKeyEvents)
static qbool wheelup_deferred = false;
static qbool wheeldown_deferred = false;

// vid_reload
#define CVAR_RELOAD_GFX_COMMAND "vid_reload"
static qbool vid_reload_pending = false;
static cvar_t vid_reload_auto = { "vid_reload_auto", "1", 0, vid_reload_callback };

static void vid_reload_callback(cvar_t* var, char* string, qbool* cancel)
{
	vid_reload_pending = false;

	if (atoi(string) != 0) {
		vid_reload_pending = Cvar_AnyModified(CVAR_RELOAD_GFX);
	}
}

//
// OS dependent cvar defaults
//
#ifdef __APPLE__
	#define CVAR_DEF1 "0"
#else
	#define CVAR_DEF1 "1"
#endif

#if defined(__linux__) || defined(__FreeBSD__)
	#define CVAR_DEF2 "0"
#else
	#define CVAR_DEF2 "1"
#endif

//
// cvars
//
extern cvar_t sys_inactivesleep;

// latched variables that can only change over a restart
cvar_t r_colorbits                = {"vid_colorbits",              "0",       CVAR_LATCH_GFX };
cvar_t r_24bit_depth              = {"vid_24bit_depth",            "1",       CVAR_LATCH_GFX };
cvar_t r_fullscreen               = {"vid_fullscreen",             "1",       CVAR_LATCH_GFX };
cvar_t r_displayRefresh           = {"vid_displayfrequency",       "0",       CVAR_LATCH_GFX | CVAR_AUTO };
cvar_t vid_displayNumber          = {"vid_displaynumber",          "0",       CVAR_LATCH_GFX | CVAR_AUTO };
cvar_t vid_usedesktopres          = {"vid_usedesktopres",          "1",       CVAR_LATCH_GFX | CVAR_AUTO };
cvar_t vid_win_borderless         = {"vid_win_borderless",         "0",       CVAR_LATCH_GFX };
cvar_t vid_width                  = {"vid_width",                  "0",       CVAR_LATCH_GFX | CVAR_AUTO };
cvar_t vid_height                 = {"vid_height",                 "0",       CVAR_LATCH_GFX | CVAR_AUTO };
cvar_t vid_win_width              = {"vid_win_width",              "640",     CVAR_LATCH_GFX };
cvar_t vid_win_height             = {"vid_win_height",             "480",     CVAR_LATCH_GFX };
#ifdef __APPLE__
cvar_t vid_hwgammacontrol         = {"vid_hwgammacontrol",         "2",       CVAR_LATCH_GFX };
#else
cvar_t vid_hwgammacontrol         = {"vid_hwgammacontrol",         "0",       CVAR_LATCH_GFX };
#endif
cvar_t vid_minimize_on_focus_loss = {"vid_minimize_on_focus_loss", CVAR_DEF1, CVAR_LATCH_GFX };
// TODO: Move the in_* cvars
cvar_t in_raw                     = {"in_raw",                     "1",       CVAR_ARCHIVE | CVAR_SILENT, in_raw_callback};
cvar_t in_grab_windowed_mouse     = {"in_grab_windowed_mouse",     "1",       CVAR_ARCHIVE | CVAR_SILENT, in_grab_windowed_mouse_callback};
cvar_t vid_grab_keyboard          = {"vid_grab_keyboard",          "0",       CVAR_LATCH_GFX }; /* Needs vid_restart thus vid_.... */
#ifdef EZ_MULTIPLE_RENDERERS
cvar_t vid_renderer               = {"vid_renderer",               "1",       CVAR_LATCH_GFX };
#endif
cvar_t vid_gl_core_profile        = {"vid_gl_core_profile",        "0",       CVAR_LATCH_GFX };

#ifdef X11_GAMMA_WORKAROUND
cvar_t vid_gamma_workaround       = {"vid_gamma_workaround",       "1",       CVAR_LATCH_GFX };
#endif

cvar_t in_release_mouse_modes     = {"in_release_mouse_modes",     "2",       CVAR_SILENT };
cvar_t in_ignore_touch_events     = {"in_ignore_touch_events",     "1",       CVAR_SILENT };
#ifdef __linux__
cvar_t in_ignore_unfocused_keyb   = {"in_ignore_unfocused_keyb",   "0",       CVAR_SILENT };
#endif
cvar_t vid_vsync_lag_fix          = {"vid_vsync_lag_fix",          "0"                    };
cvar_t vid_vsync_lag_tweak        = {"vid_vsync_lag_tweak",        "1.0"                  };
cvar_t r_swapInterval             = {"vid_vsync",                  "0",       CVAR_SILENT };
cvar_t r_win_save_pos             = {"vid_win_save_pos",           "1",       CVAR_SILENT };
cvar_t r_win_save_size            = {"vid_win_save_size",          "1",       CVAR_SILENT };
cvar_t vid_xpos                   = {"vid_xpos",                   "3",       CVAR_SILENT };
cvar_t vid_ypos                   = {"vid_ypos",                   "39",      CVAR_SILENT };
cvar_t vid_win_displayNumber      = {"vid_win_displaynumber",      "0",       CVAR_SILENT };
cvar_t r_conwidth                 = {"vid_conwidth",               "0",       CVAR_NO_RESET | CVAR_SILENT | CVAR_AUTO, conres_changed_callback };
cvar_t r_conheight                = {"vid_conheight",              "0",       CVAR_NO_RESET | CVAR_SILENT | CVAR_AUTO, conres_changed_callback };
cvar_t r_conscale                 = {"vid_conscale",               "2.0",     CVAR_NO_RESET | CVAR_SILENT, conres_changed_callback };
cvar_t vid_flashonactivity        = {"vid_flashonactivity",        "1",       CVAR_SILENT };
cvar_t r_verbose                  = {"vid_verbose",                "0",       CVAR_SILENT };
cvar_t r_showextensions           = {"vid_showextensions",         "0",       CVAR_SILENT };
cvar_t gl_multisamples            = {"gl_multisamples",            "0",       CVAR_LATCH_GFX | CVAR_AUTO }; // It's here because it needs to be registered before window creation
cvar_t vid_gammacorrection        = {"vid_gammacorrection",        "0",       CVAR_LATCH_GFX };
#ifdef __APPLE__
cvar_t vid_software_palette       = {"vid_software_palette",       "0",       CVAR_NO_RESET | CVAR_LATCH_GFX };
#else
cvar_t vid_software_palette       = {"vid_software_palette",       "1",       CVAR_NO_RESET | CVAR_LATCH_GFX };
#endif

cvar_t vid_framebuffer             = {"vid_framebuffer",               "0",       CVAR_NO_RESET | CVAR_LATCH_GFX, conres_changed_callback };
cvar_t vid_framebuffer_blit        = {"vid_framebuffer_blit",          "0",       CVAR_NO_RESET };
cvar_t vid_framebuffer_width       = {"vid_framebuffer_width",         "0",       CVAR_NO_RESET | CVAR_AUTO, conres_changed_callback };
cvar_t vid_framebuffer_height      = {"vid_framebuffer_height",        "0",       CVAR_NO_RESET | CVAR_AUTO, conres_changed_callback };
cvar_t vid_framebuffer_scale       = {"vid_framebuffer_scale",         "1",       CVAR_NO_RESET, conres_changed_callback };
cvar_t vid_framebuffer_depthformat = {"vid_framebuffer_depthformat",   "0",       CVAR_NO_RESET | CVAR_LATCH_GFX };
cvar_t gl_reverse_z                = {"gl_reverse_z",                  "1",       CVAR_LATCH_GFX };
cvar_t vid_framebuffer_hdr         = {"vid_framebuffer_hdr",           "0",       CVAR_NO_RESET | CVAR_LATCH_GFX };
cvar_t vid_framebuffer_hdr_tonemap = {"vid_framebuffer_hdr_tonemap",   "0" };
cvar_t vid_framebuffer_smooth      = {"vid_framebuffer_smooth",        "1",       CVAR_NO_RESET, framebuffer_smooth_changed_callback };
cvar_t vid_framebuffer_sshotmode   = {"vid_framebuffer_sshotmode",     "0" };
cvar_t vid_framebuffer_multisample = {"vid_framebuffer_multisample",   "0" };
cvar_t vid_framebuffer_fxaa        = {"vid_framebuffer_fxaa",          "0" };

static cvar_t *session_window_vars[SESSION_WINDOW_SETTINGS] = {
	&vid_width, &vid_height, &vid_win_width, &vid_win_height,
	&r_colorbits, &r_24bit_depth, &r_fullscreen, &r_displayRefresh,
	&vid_usedesktopres, &vid_win_borderless, &gl_multisamples,
	&vid_displayNumber, &vid_minimize_on_focus_loss, &vid_gammacorrection,
	&vid_xpos, &vid_ypos, &vid_win_displayNumber
};

void VID_SessionWindowSettings(session_video_settings_t *settings, qbool pending_only)
{
	int i;
	memset(settings, 0, sizeof(*settings));
	for (i = 0; i < SESSION_WINDOW_SETTINGS && session_window_vars[i]; ++i) {
		cvar_t *var = session_window_vars[i];
		const char *value = pending_only && (var->flags & CVAR_LATCH) ? var->latchedString : var->string;
		if (value) {
			strlcpy(settings->values[i], value, sizeof(settings->values[i]));
		}
	}
}

static void VID_ApplySessionWindowSettings(const session_video_settings_t *settings, qbool preserve_pending)
{
	int i;
	for (i = 0; i < SESSION_WINDOW_SETTINGS && session_window_vars[i]; ++i) {
		cvar_t *var = session_window_vars[i];
		if (settings->values[i][0] && (!preserve_pending || strcmp(settings->values[i], var->string))) {
			char value[sizeof(settings->values[i])];
			char *pending = preserve_pending ? var->latchedString : NULL;
			if (preserve_pending)
				var->latchedString = NULL;
			strlcpy(value, settings->values[i], sizeof(value));
			if (var->flags & CVAR_LATCH)
				Cvar_LatchedSet(var, value);
			else
				Cvar_SetIgnoreCallback(var, value);
			if (preserve_pending)
				var->latchedString = pending;
		}
	}
}

void VID_SessionSyncWindowSettings(const session_video_settings_t *settings)
{
	VID_ApplySessionWindowSettings(settings, true);
}

//
// function declaration
//

static qbool IN_IsFullscreen(void)
{
	/* Foreign wrappers use the window owner's actual fullscreen state. */
	return CL_SessionIsWorker() ? CL_SessionWindowIsFullscreen() : r_fullscreen.integer != 0;
}

// True if we need to release the mouse and let the OS show cursor again
static qbool IN_OSMouseCursorRequired(void)
{
	// the ImGui menus use the system cursor, even in fullscreen
	if (M_ImGui_IsOpen()) {
		return true;
	}

	// Explicit check here for key_game... really setting all modes is equivalent to "in_grab_windowed_mouse 0"
	qbool in_os_cursor_mode = (key_dest != key_game || cls.demoplayback) && (in_release_mouse_modes.integer & (1 << key_dest));

	// Windowed & (not-grabbing mouse | in OS cursor mode)
	return (!IN_IsFullscreen() && (!in_grab_windowed_mouse.value || in_os_cursor_mode));
}

// True if we're in a mode where we need to keep track of mouse movement
qbool IN_MouseTrackingRequired(void)
{
	return (key_dest == key_menu || key_dest == key_hudeditor || key_dest == key_demo_controls);
}

// True if we need to display the internal Quake cursor to track the mouse
qbool IN_QuakeMouseCursorRequired(void)
{
	return mouse_active && IN_MouseTrackingRequired() && !IN_OSMouseCursorRequired();
}

static int session_cursor_x, session_cursor_y;
static qbool session_cursor_valid, session_cursor_pending;

#ifdef _WIN32
static int session_mouse_x, session_mouse_y;
static unsigned int session_cursor_sequence, coordinator_cursor_sequence;
static int session_cursor_restore_x, session_cursor_restore_y;
static session_input_settings_t coordinator_input, coordinator_applied_input;
static qbool coordinator_input_applied;
#endif

static void IN_SnapMouseBackToCentre(void)
{
#ifdef _WIN32
	if (CL_SessionIsWorker())
		return;
#endif
	SDL_WarpMouseInWindow(sdl_window, glConfig.vidWidth / 2, glConfig.vidHeight / 2);
	old_x = glConfig.vidWidth / 2;
	old_y = glConfig.vidHeight / 2;
}

static void in_raw_callback(cvar_t *var, char *value, qbool *cancel)
{
	if ((var == &in_raw) && (atoi(value) != in_raw.value)) {
		Cvar_SetValue(&in_raw, atoi(value));
		IN_Restart_f();
	}
}

static void in_grab_windowed_mouse_callback(cvar_t *val, char *value, qbool *cancel)
{
	GrabMouse((atoi(value) > 0 ? true : false), in_raw.integer);
}

static void GrabMouse(qbool grab, qbool raw)
{
	if (grab && !CL_SessionIsActive())
		return;
	if ((grab && mouse_active && raw == in_raw.integer) || (!grab && !mouse_active) || !mouseinitialized || !sdl_window) {
		return;
	}

	if (!IN_IsFullscreen() && in_grab_windowed_mouse.integer == 0) {
		if (!mouse_active) {
			return;
		}
		grab = 0;
	}

#ifdef _WIN32
	if (CL_SessionIsWorker()) {
		mouse_active = grab;
		session_mouse_x = session_mouse_y = 0;
		return;
	}
#endif

	// set initial position
	if (!raw && grab) {
		// the first getState() will still return the old values so snapping back doesn't work...
		// ... open problem, people will get a jump if releasing mouse when re-grabbing with in_raw 0
		IN_SnapMouseBackToCentre();
	}

#if SDL_VERSION_ATLEAST(2, 0, 16)
	SDL_SetWindowMouseGrab(sdl_window, grab ? SDL_TRUE : SDL_FALSE);
#else
	SDL_SetWindowGrab(sdl_window, grab ? SDL_TRUE : SDL_FALSE);
#endif
	SDL_SetRelativeMouseMode((raw && grab) ? SDL_TRUE : SDL_FALSE);
	SDL_GetRelativeMouseState(NULL, NULL);

	// never show real cursor in fullscreen (unless the ImGui menus are open)
	if (IN_IsFullscreen() && !(M_ImGui_IsOpen() && !grab)) {
		SDL_ShowCursor(SDL_DISABLE);
	} else {
		SDL_ShowCursor(grab ? SDL_DISABLE : SDL_ENABLE);
	}

	// Force rewrite of it
	SDL_SetCursor(NULL);

	mouse_active = grab;
}

void IN_StartupMouse(void)
{
	Cvar_Register(&in_raw);
	Cvar_Register(&in_grab_windowed_mouse);
	Cvar_Register(&in_release_mouse_modes);
	Cvar_Register(&in_ignore_touch_events);
#ifdef __APPLE__
	Cvar_Register(&in_ignore_deadkeys);

	if (in_raw.integer > 0) {
		if (OSX_Mouse_Init() != 0) {
			Com_Printf("warning: failed to initialize raw input mouse thread...\n");
			Cvar_SetValue(&in_raw, 0);
		}
	}
#endif

	mouseinitialized = true;

	Com_Printf("%s mouse input initialized\n", in_raw.integer > 0 ? "RAW" : "SDL");
	if (in_raw.integer == 0) {
		IN_SnapMouseBackToCentre();
	}
}

static qbool IN_KeyboardGrabRequired(void)
{
	return vid_grab_keyboard.integer && CL_SessionIsActive() && ActiveApp && !Minimized &&
		(mouse_active || M_ImGui_IsOpen());
}

static void IN_SetKeyboardGrab(qbool grab)
{
#if SDL_VERSION_ATLEAST(2, 0, 16)
#ifdef _WIN32
	/* The coordinator owns keyboard capture for Windows workers. */
	if (CL_SessionIsWorker())
		return;
#endif
	if (sdl_window)
		SDL_SetWindowKeyboardGrab(sdl_window, grab ? SDL_TRUE : SDL_FALSE);
#endif
}

void IN_ActivateMouse(void)
{
	GrabMouse(true, in_raw.integer);
	IN_SetKeyboardGrab(IN_KeyboardGrabRequired());
}

void IN_DeactivateMouse(void)
{
	GrabMouse(false, in_raw.integer);
	IN_SetKeyboardGrab(false);
}

static void IN_RestoreSessionCursor(void)
{
	int width, height;
	if (!session_cursor_pending || !CL_SessionIsActive() || !ActiveApp || Minimized || mouse_active ||
		!IN_OSMouseCursorRequired())
		return;
	SDL_GetWindowSize(sdl_window, &width, &height);
	if (width <= 0 || height <= 0)
		return;
	session_cursor_x = bound(0, session_cursor_x, width - 1);
	session_cursor_y = bound(0, session_cursor_y, height - 1);
#ifdef _WIN32
	/* Windows workers render offscreen; the owner must move the real cursor
	 * after applying this session's mouse capture/visibility settings. */
	session_cursor_restore_x = session_cursor_x;
	session_cursor_restore_y = session_cursor_y;
	if (++session_cursor_sequence == 0)
		++session_cursor_sequence;
#else
	SDL_WarpMouseInWindow(sdl_window, session_cursor_x, session_cursor_y);
#endif
	session_cursor_pending = false;
}

static void IN_Frame(void)
{
	if (!sdl_window) {
		return;
	}

	/* Restore before draining motion events from the newly selected session. */
	IN_RestoreSessionCursor();
	HandleEvents();

#ifdef _WIN32
	if (CL_SessionIsWorker()) {
		session_input_settings_t settings;
		if (!CL_SessionIsActive() || !ActiveApp || Minimized || IN_OSMouseCursorRequired())
			IN_DeactivateMouse();
		else
			IN_ActivateMouse();
		memset(&settings, 0, sizeof(settings));
		settings.grab = mouse_active;
		settings.raw = in_raw.integer != 0;
		settings.keyboard_grab = IN_KeyboardGrabRequired();
		settings.show_cursor = (!IN_IsFullscreen() || M_ImGui_IsOpen()) && !mouse_active;
		settings.text_entry = key_dest == key_console || key_dest == key_message || M_ImGui_IsOpen();
		settings.disable_win_keys = (int)Cvar_Value("sys_disableWinKeys");
		settings.cursor_sequence = session_cursor_sequence;
		settings.cursor_x = session_cursor_restore_x;
		settings.cursor_y = session_cursor_restore_y;
		CL_SessionInputSettings(&settings);
		mx = mouse_active ? session_mouse_x : 0;
		my = mouse_active ? session_mouse_y : 0;
		session_mouse_x = session_mouse_y = 0;
		return;
	}
#endif
	if (!CL_SessionIsActive() || !ActiveApp || Minimized || IN_OSMouseCursorRequired()) {
		/* Menus need an unconfined pointer, but must retain the configured
		 * keyboard grab so window-manager shortcuts don't steal their keys. */
		GrabMouse(false, in_raw.integer);
		IN_SetKeyboardGrab(IN_KeyboardGrabRequired());
		if (CL_SessionIsActive() && M_ImGui_IsOpen()) {
			SDL_ShowCursor(SDL_ENABLE);
			/* SDL/ImGui cache cursor state per process, but X11 stores the
			 * cursor on our shared window. Another session can replace it
			 * without invalidating this worker's cache. NULL reapplies the
			 * current cursor even when SDL already considers it visible. */
			if (CL_SessionIsWorker())
				SDL_SetCursor(NULL);
		}
		return;
	}
	else {
		IN_ActivateMouse();
	}

	if (mouse_active && SDL_GetRelativeMouseMode()) {
#ifdef __APPLE__
		OSX_Mouse_GetMouseMovement(&mx, &my);
#else
		SDL_GetRelativeMouseState(&mx, &my);
#endif
	}
	
}

void Sys_SendDeferredKeyEvents(void)
{
	if (wheelup_deferred) {
		Key_Event(K_MWHEELUP, false);
		wheelup_deferred = false;
	}
	if (wheeldown_deferred) {
		Key_Event(K_MWHEELDOWN, false);
		wheeldown_deferred = false;
	}
}

void Sys_SendKeyEvents(void)
{
	IN_Frame();

	if (sys_inactivesleep.integer > 0 && CL_SessionIsActive()) {
		// Yield the CPU a little
		if ((ISPAUSED && (!ActiveApp)) || Minimized || block_drawing) {
			if (!cls.download) {
				SDL_Delay(50);
			}
			scr_skipupdate = 1; // no point to draw anything
		} else if (!ActiveApp) { // Delay a bit less if just not active window
			if (!cls.download) {
				SDL_Delay(20);
			}
		}
	}
}

void IN_Restart_f(void)
{
	qbool old_mouse_active = mouse_active;

	IN_Shutdown();
	IN_Init();

	// if mouse was active before restart, try to re-activate it
	if (old_mouse_active) {
		IN_ActivateMouse();
	}
}

// Converts co-ordinates for the whole desktop to co-ordinates for a specific display
static void VID_RelativePositionFromAbsolute(int* x, int* y, int* display)
{
	int displays = SDL_GetNumVideoDisplays();
	int i = 0;

	for (i = 0; i < displays; ++i)
	{
		SDL_Rect bounds;

		if (SDL_GetDisplayBounds(i, &bounds) == 0)
		{
			if (*x >= bounds.x && *x < bounds.x + bounds.w && *y >= bounds.y && *y < bounds.y + bounds.h)
			{
				*x = *x - bounds.x;
				*y = *y - bounds.y;
				*display = i;
				return;
			}
		}
	}

	*display = 0;
}

// Converts co-ordinates for a specific display to those for whole desktop
static void VID_AbsolutePositionFromRelative(int* x, int* y, int* display)
{
	SDL_Rect bounds;
	
	// Try and get bounds for the specified display - default back to main display if there's an issue
	if (SDL_GetDisplayBounds(*display, &bounds))
	{
		*display = 0;
		if (SDL_GetDisplayBounds(*display, &bounds))
		{
			// Still an issue - reset back to top-left of screen
			Com_Printf("Error detecting resolution...\n");
			*x = *y = 0;
			return;
		}
	}

	// Adjust co-ordinates, making sure some of the window will always be visible
	*x = bounds.x + min(*x, bounds.w - 30);
	*y = bounds.y + min(*y, bounds.h - 30);
}

void VID_SessionSyncWindowGeometry(int x, int y, int width, int height)
{
	int displayNumber = 0;
	if (r_win_save_pos.integer) {
		VID_RelativePositionFromAbsolute(&x, &y, &displayNumber);
		Cvar_SetValue(&vid_win_displayNumber, displayNumber);
		Cvar_SetValue(&vid_xpos, x);
		Cvar_SetValue(&vid_ypos, y);
	}
	if (r_win_save_size.integer) {
		/* A real resize updates the saved current size, while preserving a
		 * separately requested resolution awaiting vid_restart. */
		char *pending_width = vid_win_width.latchedString;
		char *pending_height = vid_win_height.latchedString;
		vid_win_width.latchedString = vid_win_height.latchedString = NULL;
		Cvar_LatchedSetValue(&vid_win_width, width);
		Cvar_LatchedSetValue(&vid_win_height, height);
		vid_win_width.latchedString = pending_width;
		vid_win_height.latchedString = pending_height;
	}
}

static int VID_SetDeviceGammaRampReal(unsigned short *ramps)
{
#ifdef X11_GAMMA_WORKAROUND
	static short once = 1;
	static short gamma_works = 0;

	if (!vid_gamma_workaround.integer) {
		SDL_SetWindowGammaRamp(sdl_window, ramps, ramps + 4096, ramps + (2 * 4096));
		vid_hwgamma_enabled = true;
		return 0;
	}

	if (once) {
		if (glConfig.gammacrap.size < 0 || glConfig.gammacrap.size > 4096) {
			Com_Printf("error: gamma size is broken, gamma won't work (reported size %d)\n", glConfig.gammacrap.size);
			once = 0;
			return 0;
		}
		if (!XF86VidModeGetGammaRamp(glConfig.gammacrap.display, glConfig.gammacrap.screen, glConfig.gammacrap.size, sysramps, sysramps + 4096, sysramps + (2 * 4096))) {
			Com_Printf("error: cannot get system gamma ramps, gamma won't work\n");
			once = 0;
			return 0;
		}
		once = 0;
		gamma_works = 1;
	}

	if (gamma_works) {
		/* Just double check the gamma size... */
		if (glConfig.gammacrap.size < 0 || glConfig.gammacrap.size > 4096) {
			Com_Printf("error: gamma size broken but worked initially, wtf?! gamma won't work\n");
			gamma_works = 0;
			vid_hwgamma_enabled = false;
		}
		/* It returns true unconditionally ... */
		XF86VidModeSetGammaRamp(glConfig.gammacrap.display, glConfig.gammacrap.screen, glConfig.gammacrap.size, ramps, ramps + 4096, ramps + (2 * 4096));
		vid_hwgamma_enabled = true;
	}
	return 0;
#else
	if (SDL_SetWindowGammaRamp(sdl_window, ramps, ramps + 256, ramps + 512) == 0) {
		vid_hwgamma_enabled = true;
		return 0;
	}
	return -1;
#endif
}

#ifdef X11_GAMMA_WORKAROUND
static void VID_RestoreSystemGamma(void)
{
	if (CL_SessionIsCoordinator() || !sdl_window || COM_CheckParm(cmdline_param_client_nohardwaregamma)) {
		return;
	}
	VID_SetDeviceGammaRampReal(sysramps);
}
#endif

static void window_event(SDL_WindowEvent *event)
{
	extern qbool scr_skipupdate;
	int flags = SDL_GetWindowFlags(sdl_window);
#ifdef _WIN32
	if (CL_SessionIsWorker() && (event->event == SDL_WINDOWEVENT_FOCUS_GAINED ||
		event->event == SDL_WINDOWEVENT_FOCUS_LOST || event->event == SDL_WINDOWEVENT_RESTORED ||
		event->event == SDL_WINDOWEVENT_MINIMIZED))
		return;
#endif

	MenuUI_ProcessEvent(&(SDL_Event) { .window = *event });

	switch (event->event) {
		case SDL_WINDOWEVENT_MINIMIZED:
			Minimized = true;

		case SDL_WINDOWEVENT_FOCUS_LOST:
			ActiveApp = false;
#ifdef __linux__
			block_keyboard_input = in_ignore_unfocused_keyb.integer;
#endif
#ifdef X11_GAMMA_WORKAROUND
			if (vid_gamma_workaround.integer) {
				if (Minimized || vid_hwgammacontrol.integer != 3) {
					VID_RestoreSystemGamma();
				}
			}
#endif
#ifdef _WIN32
			Sys_ActiveAppChanged ();
#endif
			break;

		case SDL_WINDOWEVENT_FOCUS_GAINED:
#ifdef __linux__
			if (CL_SessionIsWorker())
				block_keyboard_input = false;
#endif
			if (!CL_SessionIsCoordinator())
				TP_ExecTrigger("f_focusgained");
			/* Fall through */
		case SDL_WINDOWEVENT_RESTORED:
			Minimized = CL_SessionIsWorker() && CL_SessionWindowIsMinimized();
			ActiveApp = true;
			scr_skipupdate = 0;
#ifdef X11_GAMMA_WORKAROUND
			if (vid_gamma_workaround.integer) {
				v_gamma.modified = true;
			}
#endif
#ifdef _WIN32
			Sys_ActiveAppChanged ();
#endif
			break;

		case SDL_WINDOWEVENT_MOVED:
			if (!CL_SessionIsWorker() && !(flags & SDL_WINDOW_FULLSCREEN) && r_win_save_pos.integer) {
				int displayNumber = 0;
				int x = event->data1;
				int y = event->data2;

				VID_RelativePositionFromAbsolute(&x, &y, &displayNumber);

				Cvar_SetValue(&vid_win_displayNumber, displayNumber);
				Cvar_SetValue(&vid_xpos, x);
				Cvar_SetValue(&vid_ypos, y);
			}
			break;

		case SDL_WINDOWEVENT_RESIZED:
			if (CL_SessionIsWorker() || !(flags & SDL_WINDOW_FULLSCREEN)) {
				glConfig.vidWidth = event->data1;
				glConfig.vidHeight = event->data2;
				if (!CL_SessionIsWorker() && r_win_save_size.integer) {
					Cvar_LatchedSetValue(&vid_win_width, event->data1);
					Cvar_LatchedSetValue(&vid_win_height, event->data2);
				}
				if (!CL_SessionIsCoordinator() && (!r_conwidth.integer || !r_conheight.integer))
					VID_UpdateConRes();
			}
			if (renderer.InvalidateViewport)
				renderer.InvalidateViewport();
			break;

#ifdef __linux__
		case SDL_WINDOWEVENT_TAKE_FOCUS:
			// On X, sequence is FOCUS_GAINED, [Keyboard 'down' events], TAKE_FOCUS
			// On Windows, it's just FOCUS_GAINED then TAKE_FOCUS, so nothing to block really
			block_keyboard_input = false;
			break;
#endif
	}
}

// FIXME: APPLE K_F13-15 etc...

static const byte scantokey[128] = {
//  0               1               2               3               4               5               6                   7
//  8               9               A               B               C               D               E                   F
    0,              0,              0,              0,              'a',            'b',            'c',                'd',            // 0
    'e',            'f',            'g',            'h',            'i',            'j',            'k',                'l',
    'm',            'n',            'o',            'p',            'q',            'r',            's',                't',            // 1
    'u',            'v',            'w',            'x',            'y',            'z',            '1',                '2',
    '3',            '4',            '5',            '6',            '7',            '8',            '9',                '0',            // 2
    K_ENTER,        K_ESCAPE,       K_BACKSPACE,    K_TAB,          K_SPACE,        '-',            '=',                '[',
    ']',            '\\',           0,              ';',            '\'',           '`',            ',',                '.',            // 3
    '/' ,           K_CAPSLOCK,     K_F1,           K_F2,           K_F3,           K_F4,           K_F5,               K_F6,
    K_F7,           K_F8,           K_F9,           K_F10,          K_F11,          K_F12,          K_PRINTSCR,         K_SCRLCK,    // 4
    K_PAUSE,        K_INS,          K_HOME,         K_PGUP,         K_DEL,          K_END,          K_PGDN,             K_RIGHTARROW,
    K_LEFTARROW,    K_DOWNARROW,    K_UPARROW,      KP_NUMLOCK,     KP_SLASH,       KP_STAR,        KP_MINUS,           KP_PLUS,        // 5
    KP_ENTER,       KP_END,         KP_DOWNARROW,   KP_PGDN,        KP_LEFTARROW,   KP_5,           KP_RIGHTARROW,      KP_HOME,
    KP_UPARROW,     KP_PGUP,        KP_INS,         KP_DEL,         K_ISO,          K_MENU,         0,                  0,              // 6
    0,              0,              0,              0,              0,              0,              0,                  0,
    0,              0,              0,              0,              0,              0,              K_MENU,             0,              // 7
#ifdef __APPLE__
    K_LCTRL,        K_LSHIFT,       K_LALT,         K_CMD,          K_RCTRL,        K_RSHIFT,       K_RALT,             K_CMD,         // E
#else
    K_LCTRL,        K_LSHIFT,       K_LALT,         K_LWIN,         K_RCTRL,        K_RSHIFT,       K_RALT,             K_RWIN,         // E
#endif
};

byte Key_ScancodeToQuakeCode(int scancode)
{
	byte quakeCode = 0;
	if (scancode < 120)
		quakeCode = scantokey[scancode];
	else if (scancode >= 224 && scancode < 224 + 8)
		quakeCode = scantokey[scancode - 104];

	if (cl_keypad.integer == 0 || key_dest == key_menu || (cl_keypad.integer == 2 && !(key_dest == key_console || key_dest == key_message))) {
		// compatibility mode without knowledge about keypad-keys:
		switch (quakeCode)
		{
		case KP_NUMLOCK:     quakeCode = K_PAUSE;          break;
		case KP_SLASH:       quakeCode = '/';              break;
		case KP_STAR:        quakeCode = '*';              break;
		case KP_MINUS:       quakeCode = '-';              break;
		case KP_HOME:        quakeCode = K_HOME;           break;
		case KP_UPARROW:     quakeCode = K_UPARROW;        break;
		case KP_PGUP:        quakeCode = K_PGUP;           break;
		case KP_LEFTARROW:   quakeCode = K_LEFTARROW;      break;
		case KP_5:           quakeCode = '5';              break;
		case KP_RIGHTARROW:  quakeCode = K_RIGHTARROW;     break;
		case KP_PLUS:        quakeCode = '+';              break;
		case KP_END:         quakeCode = K_END;            break;
		case KP_DOWNARROW:   quakeCode = K_DOWNARROW;      break;
		case KP_PGDN:        quakeCode = K_PGDN;           break;
		case KP_INS:         quakeCode = K_INS;            break;
		case KP_DEL:         quakeCode = K_DEL;            break;
		case KP_ENTER:       quakeCode = K_ENTER;          break;
		default:                                           break;
		}
	}
	else if (cl_keypad.integer == 1 && key_dest != key_game) {
		// Treat as normal return key
		if (quakeCode == KP_ENTER) {
			quakeCode = K_ENTER;
		}
	}

	return quakeCode;
}

byte Key_CharacterToQuakeCode(char ch)
{
	// Uses fact that SDLK_a == 'a'... is this okay?
	
	// Convert from key-code to scan-code to see what physical button they pressed
	int scancode = SDL_GetScancodeFromKey(ch);

	return Key_ScancodeToQuakeCode(scancode);
}

wchar Key_Event_TextInput(wchar unichar);

static void keyb_textinputevent(char* text)
{
	int i = 0;
	int len = 0;
	wchar unichar = 0;

	// Only process text input messages here
	if (key_dest != key_console && key_dest != key_message)
		return;

	if (!*text)
		return;

#ifdef __APPLE__
	// operating system is sending deadkey-modified input... ignore
	if (deadkey_modifiers_held_down) {
		return;
	}
#endif

#ifdef __linux__
	if (block_keyboard_input) {
		return;
	}
#endif

	len = strlen(text);
	for (i = 0; i < len; ++i)
	{
		unichar = TextEncodingDecodeUTF8(text, &i);

		if (unichar)
			Key_Event_TextInput(unichar);
	}
}

static void keyb_event(SDL_KeyboardEvent *event)
{
	byte result = Key_ScancodeToQuakeCode(event->keysym.scancode);
	static qbool session_keys[CL_MAX_SESSIONS];
	int slot = event->keysym.scancode - SDL_SCANCODE_1;
	if (!CL_SessionIsActive())
		return;
	/* SDL's normal fullscreen/grabbed Alt+Tab fallback minimizes its window.
	 * A worker must request that action from the owner instead of letting SDL
	 * manage fullscreen through a foreign-window wrapper. Keep delivering the
	 * key event normally, just as SDL does before applying its fallback. */
	if (CL_SessionIsWorker() && session_alt_tab_enabled && !event->repeat &&
		event->state == SDL_PRESSED && event->keysym.sym == SDLK_TAB &&
		(event->keysym.mod & KMOD_ALT) && IN_IsFullscreen()) {
#if SDL_VERSION_ATLEAST(2, 0, 16)
		if (SDL_GetWindowKeyboardGrab(sdl_window))
#else
		if (SDL_GetWindowGrab(sdl_window) && SDL_GetHintBoolean(SDL_HINT_GRAB_KEYBOARD, SDL_FALSE))
#endif
			VID_Minimize();
	}
	if (slot >= 0 && slot < CL_MAX_SESSIONS) {
		if (event->state && (event->keysym.mod & KMOD_CTRL) && !(event->keysym.mod & KMOD_ALT)) {
			session_keys[slot] = true;
			if (!event->repeat)
				CL_SessionSelect(slot + 1);
			return;
		}
		if (session_keys[slot] && !event->state) {
			session_keys[slot] = false;
			return;
		}
	}

#ifdef __APPLE__
	if (in_ignore_deadkeys.integer) {
		// Apologies for the guesswork, no Apple keyboard...
		int left_alt = (in_ignore_deadkeys.integer == 2 ? SDLK_LALT : SDLK_LGUI);
		int right_alt = (in_ignore_deadkeys.integer == 2 ? SDLK_RALT : SDLK_RGUI);

		if (event->keysym.sym == left_alt) {
			deadkey_modifiers_held_down ^= APPLE_LALT_HELD_DOWN;
			deadkey_modifiers_held_down |= (event->state ? APPLE_LALT_HELD_DOWN : 0);
		}
		else if (event->keysym.sym == right_alt) {
			deadkey_modifiers_held_down ^= APPLE_RALT_HELD_DOWN;
			deadkey_modifiers_held_down |= (event->state ? APPLE_RALT_HELD_DOWN : 0);
		}
	}
#endif

	if (result == 0) {
		Com_DPrintf("%s: unknown scancode %d\n", __func__, event->keysym.scancode);
		return;
	}

#ifdef __linux__
	if (block_keyboard_input) {
		Com_DPrintf("%s: scan-code %d, qchar %d: suppressed\n", __func__, event->keysym.scancode, result);
		return;
	}
#endif
	Key_Event(result, event->state);
}

static void mouse_button_event(SDL_MouseButtonEvent *event)
{
	unsigned key;
	extern cvar_t cl_delay_input;

	switch (event->button) {
	case SDL_BUTTON_LEFT:
		key = K_MOUSE1;
		break;
	case SDL_BUTTON_RIGHT:
		key = K_MOUSE2;
		break;
	case SDL_BUTTON_MIDDLE:
		key = K_MOUSE3;
		break;
	case 8:
	case SDL_BUTTON_X1:
		key = K_MOUSE4;
		break;
	case 9:
	case SDL_BUTTON_X2:
		key = K_MOUSE5;
		break;
	default:
		Com_DPrintf("%s: unknown button %d\n", __func__, event->button);
		return;
	}

	if (cl_delay_input.value > 0) {
		IN_AddButtonToDelayBuffer(key, event->state);
	} else {
		Key_Event(key, event->state);
	}
}

static void mouse_wheel_event(SDL_MouseWheelEvent *event)
{
    extern cvar_t cl_delay_input;
    
    if (cl_delay_input.value > 0) {
        if (event->y > 0) {
            IN_AddWheelToDelayBuffer(1);
        }
        else if (event->y < 0) {
            IN_AddWheelToDelayBuffer(-1);
        }
    } else {
        if (event->y > 0) {
            if (wheelup_deferred) {
                Key_Event(K_MWHEELUP, false);
            }
			Key_Event(K_MWHEELUP, true);
			wheelup_deferred = true;
		}
		else if (event->y < 0) {
			if (wheeldown_deferred) {
				Key_Event(K_MWHEELDOWN, false);
			}
			Key_Event(K_MWHEELDOWN, true);
			wheeldown_deferred = true;
		}
	}
}

static void HandleInputEvent(SDL_Event event)
{
	qbool track_movement_through_state = mouse_active && !SDL_GetRelativeMouseMode();
	/* Record absolute window coordinates before the menu consumes motion.
	 * Never replace a saved position with relative mouse-look input or with
	 * the cursor inherited from another session while a restore is pending. */
	if ((event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) &&
		CL_SessionIsWorker() && CL_SessionIsActive() &&
		ActiveApp && !Minimized && !mouse_active && IN_OSMouseCursorRequired() &&
		!session_cursor_pending) {
		Uint32 which = event.type == SDL_MOUSEMOTION ? event.motion.which : event.button.which;
		if (which != SDL_TOUCH_MOUSEID || !in_ignore_touch_events.integer) {
			session_cursor_x = event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x;
			session_cursor_y = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
			session_cursor_valid = true;
		}
	}
	/* Both native SDL input and forwarded session input must reach the menu
	 * before the game. The menu lets releases through to clear held keys. */
	if (MenuUI_ProcessEvent(&event))
		return;
#ifdef _WIN32
	if (CL_SessionIsWorker())
		track_movement_through_state = false;
#endif
	switch (event.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP:
#ifdef __APPLE__
			if (developer.integer == 2) {
				Con_Printf("key%s event, scan=%d, sym=%d, mod=%d\n", event.type == SDL_KEYDOWN ? "down" : "up", event.key.keysym.scancode, event.key.keysym.sym, event.key.keysym.mod);
			}
#endif
			keyb_event(&event.key);
			break;
		case SDL_TEXTINPUT:
			keyb_textinputevent(event.text.text);
			break;
		case SDL_MOUSEMOTION:
			if (event.motion.which != SDL_TOUCH_MOUSEID || !in_ignore_touch_events.integer) {
#ifdef _WIN32
				if (CL_SessionIsWorker() && mouse_active) {
					session_mouse_x += event.motion.xrel;
					session_mouse_y += event.motion.yrel;
				}
#endif
#ifdef __APPLE__
				if (developer.integer == 2) {
					Con_Printf("motion event, which=%d\n", event.motion.which);
				}
#endif
				if (!track_movement_through_state) {
					float factor = (IN_MouseTrackingRequired() ? cursor_sensitivity.value : 1);

					cursor_x += event.motion.xrel * factor;
					cursor_y += event.motion.yrel * factor;

					cursor_x = bound(0, cursor_x, VID_RenderWidth2D());
					cursor_y = bound(0, cursor_y, VID_RenderHeight2D());
				}
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
#ifdef __APPLE__
			if (developer.integer == 2) {
				Con_Printf("mouse%s event, which=%d, button=%d\n", event.type == SDL_MOUSEBUTTONDOWN ? "down" : "up", event.button.which, event.button.button);
			}
#endif
			if (event.button.which != SDL_TOUCH_MOUSEID || !in_ignore_touch_events.integer) {
				mouse_button_event(&event.button);
			}
			break;
		case SDL_MOUSEWHEEL:
			if (event.wheel.which != SDL_TOUCH_MOUSEID || !in_ignore_touch_events.integer) {
				mouse_wheel_event(&event.wheel);
			}
			break;
	}
}

#ifdef _WIN32
void VID_SessionInputState(qbool focused, qbool minimized, qbool reset)
{
	qbool gained = !ActiveApp && focused && !minimized;
	if (sdl_window && (reset || ActiveApp != (focused && !minimized))) {
		SDL_Event event;
		memset(&event, 0, sizeof(event));
		event.type = SDL_WINDOWEVENT;
		event.window.windowID = SDL_GetWindowID(sdl_window);
		/* Reset ImGui's held buttons and keys along with the engine's input. */
		event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
		MenuUI_ProcessEvent(&event);
		if (focused && !minimized) {
			event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
			MenuUI_ProcessEvent(&event);
		}
	}
	if (reset || (ActiveApp && (!focused || minimized))) {
		Key_ClearStates();
		CL_ClearSessionInput();
		session_mouse_x = session_mouse_y = mx = my = 0;
	}
	ActiveApp = focused && !minimized;
	Minimized = minimized;
	if (gained) {
		scr_skipupdate = false;
		if (CL_SessionIsActive())
			TP_ExecTrigger("f_focusgained");
	}
}

void VID_SessionInputEvent(const SDL_Event *event)
{
	SDL_Event local_event = *event;
	Uint32 window_id;
	if (!CL_SessionIsActive() || !ActiveApp || Minimized || !sdl_window)
		return;

	/* SDL window IDs belong to a process. ImGui must see the worker's render
	 * window ID, not the coordinator's shared-window ID. */
	window_id = SDL_GetWindowID(sdl_window);
	switch (local_event.type) {
	case SDL_KEYDOWN: case SDL_KEYUP: local_event.key.windowID = window_id; break;
	case SDL_TEXTINPUT: local_event.text.windowID = window_id; break;
	case SDL_MOUSEMOTION: local_event.motion.windowID = window_id; break;
	case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: local_event.button.windowID = window_id; break;
	case SDL_MOUSEWHEEL: local_event.wheel.windowID = window_id; break;
	}
	HandleInputEvent(local_event);
}

void VID_SessionInputSettings(const session_input_settings_t *settings)
{
	session_input_settings_t effective;
	qbool focused;
	if (settings)
		coordinator_input = *settings;
	else {
		memset(&coordinator_input, 0, sizeof(coordinator_input));
		coordinator_input.show_cursor = true;
		coordinator_cursor_sequence = 0;
	}
	if (!sdl_window)
		return;
	focused = (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS) &&
		!(SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MINIMIZED);
	effective = coordinator_input;
	effective.grab = effective.grab && focused;
	effective.keyboard_grab = effective.keyboard_grab && focused;
	effective.raw = effective.raw && effective.grab;
	effective.show_cursor = effective.show_cursor || !focused;
	if (!focused)
		effective.disable_win_keys = 0;
	if (coordinator_input_applied && !memcmp(&effective, &coordinator_applied_input, sizeof(effective)))
		return;
#if SDL_VERSION_ATLEAST(2, 0, 16)
	SDL_SetWindowMouseGrab(sdl_window, effective.grab ? SDL_TRUE : SDL_FALSE);
	SDL_SetWindowKeyboardGrab(sdl_window, effective.keyboard_grab ? SDL_TRUE : SDL_FALSE);
#else
	SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, effective.keyboard_grab ? "1" : "0");
	SDL_SetWindowGrab(sdl_window, effective.grab ? SDL_TRUE : SDL_FALSE);
#endif
	SDL_SetRelativeMouseMode(effective.raw ? SDL_TRUE : SDL_FALSE);
	SDL_GetRelativeMouseState(NULL, NULL);
	if (effective.grab && !effective.raw &&
		(!coordinator_input_applied || !coordinator_applied_input.grab || coordinator_applied_input.raw)) {
		int width, height;
		SDL_GetWindowSize(sdl_window, &width, &height);
		SDL_WarpMouseInWindow(sdl_window, width / 2, height / 2);
	}
	SDL_ShowCursor(effective.show_cursor ? SDL_ENABLE : SDL_DISABLE);
	SDL_SetCursor(NULL);
	if (focused && effective.show_cursor && !effective.grab && effective.cursor_sequence &&
		effective.cursor_sequence != coordinator_cursor_sequence) {
		int width, height;
		SDL_GetWindowSize(sdl_window, &width, &height);
		if (width > 0 && height > 0) {
			SDL_WarpMouseInWindow(sdl_window, bound(0, effective.cursor_x, width - 1),
				bound(0, effective.cursor_y, height - 1));
			coordinator_cursor_sequence = effective.cursor_sequence;
		}
	}
#ifndef WITHOUT_WINKEYHOOK
	Sys_SessionInputPolicy(effective.disable_win_keys, CL_SessionWindowIsFullscreen(), effective.text_entry);
#endif
	coordinator_applied_input = effective;
	coordinator_input_applied = true;
}
#endif

#if defined(_WIN32) && !defined(WITHOUT_WINKEYHOOK)
static void HandleWindowsKeyboardEvents(unsigned int flags, qbool down)
{
	static const struct { unsigned int mask; int key; SDL_Scancode scancode; } keys[] = {
		{ WINDOWS_LWINDOWSKEY, K_LWIN, SDL_SCANCODE_LGUI },
		{ WINDOWS_RWINDOWSKEY, K_RWIN, SDL_SCANCODE_RGUI },
		{ WINDOWS_MENU, K_MENU, SDL_SCANCODE_APPLICATION },
		{ WINDOWS_PRINTSCREEN, K_PRINTSCR, SDL_SCANCODE_PRINTSCREEN },
		{ WINDOWS_CAPSLOCK, K_CAPSLOCK, SDL_SCANCODE_CAPSLOCK }
	};
	int i;
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		if (!(flags & keys[i].mask))
			continue;
		if (CL_SessionIsCoordinator()) {
			SDL_Event event;
			memset(&event, 0, sizeof(event));
			event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
			event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
			event.key.keysym.scancode = keys[i].scancode;
			event.key.keysym.mod = SDL_GetModState();
			CL_SessionsForwardInput(&event);
		}
		else if (!CL_SessionIsWorker())
			Key_Event(keys[i].key, down);
	}
}
#endif

static void HandleEvents(void)
{
	SDL_Event event;
	qbool track_movement_through_state = (mouse_active && !SDL_GetRelativeMouseMode());

	while (SDL_PollEvent(&event)) {
		/* Discard events for windows retired by a video restart. */
		if (event.type == SDL_WINDOWEVENT &&
			event.window.windowID != SDL_GetWindowID(sdl_window))
			continue;
#ifdef _WIN32
		if (CL_SessionIsCoordinator()) {
			/* Non-raw capture is sampled and recentered once per pump below. */
			if (event.type == SDL_MOUSEMOTION && coordinator_applied_input.grab && !coordinator_applied_input.raw)
				continue;
			if (CL_SessionsForwardInput(&event))
				continue;
		}
		if (CL_SessionIsWorker() && event.type != SDL_WINDOWEVENT && event.type != SDL_QUIT && event.type != SDL_DROPFILE)
			continue;
#endif
		if (!CL_SessionIsActive() && event.type != SDL_WINDOWEVENT && event.type != SDL_QUIT &&
			!(CL_SessionIsCoordinator() && event.type == SDL_DROPFILE)) {
			if (event.type == SDL_DROPFILE)
				SDL_free(event.drop.file);
			continue;
		}
		switch (event.type) {
		case SDL_QUIT:
			if (!CL_SessionRequestQuit(true))
				Host_Quit();
			break;
		case SDL_WINDOWEVENT:
			window_event(&event.window);
			break;
		case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
		case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
			HandleInputEvent(event);
			break;
		case SDL_DROPFILE:
			CL_SessionDropFile(event.drop.file);
			SDL_free(event.drop.file);
			break;
		}
	}

/* Pumping SDL can dispatch low-level keyboard hooks. Drain them before the
	 * coordinator waits, even if they suppressed the native key message. */
#if defined(_WIN32) && !defined(WITHOUT_WINKEYHOOK)
	HandleWindowsKeyboardEvents(windows_keys_down, true);
	HandleWindowsKeyboardEvents(windows_keys_up, false);

	windows_keys_down = windows_keys_up = 0;
#endif

#ifdef _WIN32
	if (CL_SessionIsWorker())
		return;
#endif
	if (track_movement_through_state) {
		float factor = (IN_MouseTrackingRequired() ? cursor_sensitivity.value : 1);
		int pos_x, pos_y;

		SDL_GetMouseState(&pos_x, &pos_y);

		mx = pos_x - old_x;
		my = pos_y - old_y;

		cursor_x = min(max(0, cursor_x + (pos_x - glConfig.vidWidth / 2) * factor), VID_RenderWidth2D());
		cursor_y = min(max(0, cursor_y + (pos_y - glConfig.vidHeight / 2) * factor), VID_RenderHeight2D());

		IN_SnapMouseBackToCentre();
	}
}

/*****************************************************************************/

void VID_SoftRestart(void)
{
	R_Shutdown(r_shutdown_reload);
	QMB_ShutdownParticles();
}

void VID_Shutdown(qbool restart)
{
	// the menus own GL objects and textures, release them while the context still exists
	M_ImGui_VidReady(false);
	MenuUI_VidShutdown();

	IN_DeactivateMouse();

#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)
	session_swap_control_checked = false;
	session_swap_msc = NULL;
#endif

	SDL_StopTextInput();

#ifdef X11_GAMMA_WORKAROUND
	if (vid_gamma_workaround.integer) {
		VID_RestoreSystemGamma();
	}
#endif

	/* A detached worker can quit before it receives its replacement window. */
	if (sdl_context)
		R_Shutdown(restart ? r_shutdown_restart : r_shutdown_full);

	if (sdl_context) {
		SDL_GL_DeleteContext(sdl_context);
		sdl_context = NULL;
	}

	if (sdl_window) {
		CL_SessionsDetachWindow();
		SDL_DestroyWindow(sdl_window);
		sdl_window = NULL;
	}

	SDL_GL_ResetAttributes();

	if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}

	memset(&glConfig, 0, sizeof(glConfig));

	Q_free(modelist);
	modelist_count = 0;
	vid_hwgamma_enabled = false;
	vid_initialized = false;

	if (!restart && !CL_SessionIsCoordinator()) {
		QMB_ShutdownParticles();
	}
}

static int VID_SDL_InitSubSystem(void)
{
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
		if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
			Sys_Error("Couldn't initialize SDL video: %s\n", SDL_GetError());
			return -1;
		}
	}

	SDL_StartTextInput();

	return 0;
}

// This is called during video initialisation & vid_restart, but not vid_reload
// Do not include any cvars here that should take effect without full restart
static void VID_RegisterLatchCvars(void)
{
	int i;
	Cvar_SetCurrentGroup(CVAR_GROUP_VIDEO);

	for (i = 0; i < SESSION_WINDOW_SETTINGS && session_window_vars[i]; ++i) {
		if (session_window_vars[i]->flags & CVAR_LATCH)
			Cvar_Register(session_window_vars[i]);
	}
	Cvar_Register(&vid_hwgammacontrol);
#ifdef X11_GAMMA_WORKAROUND
	Cvar_Register(&vid_gamma_workaround);
#endif
	Cvar_Register(&vid_grab_keyboard);
#ifdef EZ_MULTIPLE_RENDERERS
	Cvar_Register(&vid_renderer);
#endif
	Cvar_Register(&vid_gl_core_profile);
	Cvar_Register(&vid_framebuffer);
	Cvar_Register(&vid_software_palette);
	Cvar_Register(&vid_framebuffer_depthformat);
	Cvar_Register(&gl_reverse_z);
	Cvar_Register(&vid_framebuffer_hdr);

	Cvar_ResetCurrentGroup();
}

void VID_RegisterCvars(void)
{
	Cvar_SetCurrentGroup(CVAR_GROUP_VIDEO);

	Cvar_Register(&vid_vsync_lag_fix);
	Cvar_Register(&vid_vsync_lag_tweak);
	Cvar_Register(&r_win_save_pos);
	Cvar_Register(&r_win_save_size);
	Cvar_Register(&r_swapInterval);
	Cvar_Register(&r_verbose);
	Cvar_Register(&vid_xpos);
	Cvar_Register(&vid_ypos);
	Cvar_Register(&r_conwidth);
	Cvar_Register(&r_conheight);
	Cvar_Register(&r_conscale);
	Cvar_Register(&vid_flashonactivity);
	Cvar_Register(&r_showextensions);
	Cvar_Register(&vid_win_displayNumber);
#ifdef __linux__
	Cvar_Register(&in_ignore_unfocused_keyb);
#endif

	Cvar_Register(&vid_framebuffer_blit);
	Cvar_Register(&vid_framebuffer_width);
	Cvar_Register(&vid_framebuffer_height);
	Cvar_Register(&vid_framebuffer_scale);
	Cvar_Register(&vid_framebuffer_hdr_tonemap);
	Cvar_Register(&vid_framebuffer_smooth);
	Cvar_Register(&vid_framebuffer_sshotmode);
	Cvar_Register(&vid_framebuffer_multisample);
	Cvar_Register(&vid_framebuffer_fxaa);

	Cvar_Register(&vid_reload_auto);

	Cvar_ResetCurrentGroup();
}

// Returns valid display number
int VID_DisplayNumber(qbool fullscreen)
{
	int displayNumber = (fullscreen ? vid_displayNumber.value : vid_win_displayNumber.value);
	int displays = SDL_GetNumVideoDisplays();

	return max(0, min(displays - 1, displayNumber));
}

static void VID_SetupModeList(void)
{
	int i;

	modelist_count = SDL_GetNumDisplayModes(VID_DisplayNumber(r_fullscreen.integer == 1));

	if (modelist_count <= 0) {
		Com_Printf("error getting display modes: %s\n", SDL_GetError());
		modelist_count = 0;
	}

	modelist = Q_calloc(modelist_count, sizeof(*modelist));

	for (i = 0; i < modelist_count; i++) {
		SDL_GetDisplayMode(0, i, &modelist[i]);
	}
}

static void VID_SetupResolution(void)
{
	SDL_DisplayMode display_mode;
	int display_nbr;

	if (r_fullscreen.integer) {
		display_nbr = VID_DisplayNumber(true);
		if (vid_usedesktopres.integer == 1) {
			if (SDL_GetDesktopDisplayMode(display_nbr, &display_mode) == 0) {
				glConfig.vidWidth = last_working_width = display_mode.w;
				glConfig.vidHeight = last_working_height = display_mode.h;
				glConfig.displayFrequency = last_working_hz = display_mode.refresh_rate;
				last_working_display = display_nbr;
				last_working_values = true;
				Cvar_AutoSetInt(&vid_width, display_mode.w);
				Cvar_AutoSetInt(&vid_height, display_mode.h);
				Cvar_AutoSetInt(&r_displayRefresh, display_mode.refresh_rate);
				return;
			} else {
				Com_Printf("warning: failed to get desktop resolution\n");
			}
		}
		/* Note the fall through in case the above fails */

		if (vid_width.integer == 0 || vid_height.integer == 0) {
			/* Try some default if nothing is set and we failed to get desktop res */
			glConfig.vidWidth = 1024;
			glConfig.vidHeight = 768;
			glConfig.displayFrequency = 0;

			Cvar_LatchedSetValue(&vid_width, 1024);
			Cvar_LatchedSetValue(&vid_height, 768);
			Cvar_LatchedSetValue(&r_displayRefresh, 0);

			return;
		}

		/* USER specified resolution */
		glConfig.vidWidth = bound(320, vid_width.integer, vid_width.integer);
		glConfig.vidHeight = bound(200, vid_height.integer, vid_height.integer);
		glConfig.displayFrequency = r_displayRefresh.integer;

	} else { /* Windowed mode */
		if (vid_win_width.integer == 0 || vid_win_height.integer == 0) {
			Cvar_LatchedSetValue(&vid_win_width, 640);
			Cvar_LatchedSetValue(&vid_win_height, 480);
			Cvar_LatchedSetValue(&r_displayRefresh, 0);
		}

		glConfig.vidWidth = bound(320, vid_win_width.integer, vid_win_width.integer);
		glConfig.vidHeight = bound(240, vid_win_height.integer, vid_win_height.integer);
		glConfig.displayFrequency = 0;
	}
}

int VID_GetCurrentModeIndex(void)
{
	int i;

	int best_freq = 0;
	int best_idx = -1;

	for (i = 0; i < modelist_count; i++) {
		if (modelist[i].w == vid_width.integer && modelist[i].h == vid_height.integer) {
			if (modelist[i].refresh_rate == r_displayRefresh.integer) {
				Com_DPrintf("MATCHED: %dx%d hz:%d\n", modelist[i].w, modelist[i].h, modelist[i].refresh_rate);
				return i;
			}

			if (modelist[i].refresh_rate > best_freq) {
				best_freq = modelist[i].refresh_rate;
				best_idx = i;
			}
		}
	}

	/* width/height matched but not hz, using the best available */
	if (best_idx >= 0) {
		Cvar_AutoSetInt(&r_displayRefresh, modelist[best_idx].refresh_rate);
	}

	return best_idx;
}

int VID_GetModeIndexCount(void) {
	return modelist_count;
}

const SDL_DisplayMode *VID_GetDisplayMode(int index)
{
	if (index < 0 || index >= modelist_count) {
		return NULL;
	}

	return &modelist[index];
}

static void VID_SDL_GL_SetupWindowAttributes(int options)
{
	SDL_GL_ResetAttributes();
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, options & VID_MULTISAMPLED ? 1 : 0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, options & VID_MULTISAMPLED ? bound(2, gl_multisamples.integer, 16) : 0);
	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, options & VID_ACCELERATED ? 1 : 0);
	/* A context-free owner must provide a depth buffer for any engine that
	 * renders directly to the window, regardless of another engine's FBO use. */
	if (vid_framebuffer.integer && !CL_SessionIsCoordinator()) {
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	}
	else {
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, options & VID_DEPTHBUFFER24 ? 24 : 16);
	}
	SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, options & VID_GAMMACORRECTED ? 1 : 0);
}

static SDL_GLContext VID_SDL_GL_SetupContextAttributes(void)
{
#ifdef EZ_MULTIPLE_RENDERERS
	if (vid_renderer.integer < VID_RENDERER_MIN || vid_renderer.integer > VID_RENDERER_MAX) {
#ifdef RENDERER_OPTION_CLASSIC_OPENGL
		Con_Printf("Invalid vid_renderer value detected, falling back to default.\n");
		Cvar_LatchedSetValue(&vid_renderer, VID_RENDERER_MIN);
#else
		Sys_Error("Invalid vid_renderer value detected");
#endif
	}
#endif

#ifdef RENDERER_OPTION_MODERN_OPENGL
	if (R_UseModernOpenGL()) {
		return GLM_SDL_CreateContext(sdl_window);
	}
#endif
#ifdef RENDERER_OPTION_CLASSIC_OPENGL
	if (R_UseImmediateOpenGL()) {
		return GLC_SDL_CreateContext(sdl_window);
	}
#endif
#ifdef RENDERER_OPTION_VULKAN
	if (R_UseVulkan()) {
		//return VK_SDL_CreateContext(sdl_window);
	}
#endif

	return NULL;
}

static int VID_SetWindowIcon(SDL_Window *sdl_window)
{
#ifdef __APPLE__
	// on OS X the icon is handled by the app bundle
	// it actually is higher resolution than the one here.
	return 0;
#else
	SDL_Surface *icon_surface;
        icon_surface = SDL_CreateRGBSurfaceFrom((void *)ezquake_icon.pixel_data, ezquake_icon.width, ezquake_icon.height, ezquake_icon.bytes_per_pixel * 8,
                ezquake_icon.width * ezquake_icon.bytes_per_pixel,
                0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);

	if (icon_surface) {
		SDL_SetWindowIcon(sdl_window, icon_surface);
		SDL_FreeSurface(icon_surface);
		return 0;
	}

	return -1;
#endif
}

static SDL_Window *VID_SDL_CreateWindow(int flags)
{
	if (CL_SessionIsWorker())
		return CL_SessionCreateWindow();
	if (r_fullscreen.integer == 0) {
		int displayNumber = VID_DisplayNumber(false);
		int xpos = vid_xpos.integer;
		int ypos = vid_ypos.integer;

		VID_AbsolutePositionFromRelative(&xpos, &ypos, &displayNumber);

		return SDL_CreateWindow(WINDOW_CLASS_NAME, xpos, ypos, glConfig.vidWidth, glConfig.vidHeight, flags);
	}
	else {
		int windowWidth = glConfig.vidWidth;
		int windowHeight = glConfig.vidHeight;
		int windowX = SDL_WINDOWPOS_CENTERED;
		int windowY = SDL_WINDOWPOS_CENTERED;
		int displayNumber = VID_DisplayNumber(true);
		SDL_Rect bounds;

		if (SDL_GetDisplayBounds(displayNumber, &bounds) == 0) {
			windowX = bounds.x;
			windowY = bounds.y;
			windowWidth = bounds.w;
			windowHeight = bounds.h;
		}
		else {
			Com_Printf("Couldn't determine bounds of display #%d, defaulting to main display\n", displayNumber);
		}

		return SDL_CreateWindow(WINDOW_CLASS_NAME, windowX, windowY, windowWidth, windowHeight, flags);
	}
}

#ifdef X11_GAMMA_WORKAROUND
static void VID_X11_GetGammaRampSize(void)
{
	glConfig.gammacrap.size = -1;

	SDL_VERSION(&glConfig.gammacrap.info.version);
	glConfig.gammacrap.screen = SDL_GetWindowDisplayIndex(sdl_window);

	if (glConfig.gammacrap.screen < 0) {
		Com_Printf("error: couldn't get screen number to set gamma\n");
		return;
	}

	if (SDL_GetWindowWMInfo(sdl_window, &glConfig.gammacrap.info) != SDL_TRUE) {
		Com_Printf("error: can not get display pointer, gamma won't work: %s\n", SDL_GetError());
		return;
	}

	if (glConfig.gammacrap.info.subsystem != SDL_SYSWM_X11) {
		Com_Printf("error: not x11, gamma won't work\n");
		return;
	}

	glConfig.gammacrap.display = glConfig.gammacrap.info.info.x11.display;
	XF86VidModeGetGammaRampSize(glConfig.gammacrap.display, glConfig.gammacrap.screen, &glConfig.gammacrap.size);

	if (glConfig.gammacrap.size <= 0 || glConfig.gammacrap.size > 4096) {
		Com_Printf("error: gamma size '%d' seems weird, refusing to use it\n", glConfig.gammacrap.size);
		glConfig.gammacrap.size = -1;
		return;
	}
}
#endif

static void VID_SetWindowResolution(void)
{
	if (CL_SessionIsWorker())
		return;
	if (r_fullscreen.integer > 0 && vid_usedesktopres.integer != 1) {
		int index = VID_GetCurrentModeIndex();

		if (index < 0) {
			Com_Printf("Couldn't find a matching video mode for the selected values, check video settings!\n");
			if (last_working_values == true) {
				Com_Printf("Using last known working settings: %dx%d@%dHz\n", last_working_width, last_working_height, last_working_hz);
				Cvar_LatchedSetValue(&vid_width, (float)last_working_width);
				Cvar_LatchedSetValue(&vid_height, (float)last_working_height);
				Cvar_LatchedSetValue(&r_displayRefresh, (float)last_working_hz);
				Cvar_LatchedSetValue(&vid_displayNumber, (float)last_working_display);
				Cvar_AutoSetInt(&vid_width, last_working_width);
				Cvar_AutoSetInt(&vid_height, last_working_height);
				Cvar_AutoSetInt(&r_displayRefresh, last_working_hz);
				Cvar_AutoSetInt(&vid_displayNumber, last_working_display);
			}
			else {
				Com_Printf("Using desktop resolution as fallback\n");
				Cvar_LatchedSet(&vid_usedesktopres, "1");
				Cvar_AutoSet(&vid_usedesktopres, "1");
			}
			VID_SetupResolution();
		}
		else {
			if (SDL_SetWindowDisplayMode(sdl_window, &modelist[index]) != 0) {
				Com_Printf("sdl error: %s\n", SDL_GetError());
			}
			else {
				last_working_width = (&modelist[index])->w;
				last_working_height = (&modelist[index])->h;
				last_working_hz = (&modelist[index])->refresh_rate;
				last_working_display = vid_displayNumber.integer;
				last_working_values = true;
			}
		}

		if (SDL_SetWindowFullscreen(sdl_window, SDL_WINDOW_FULLSCREEN) < 0) {
			Com_Printf("Failed to change to fullscreen mode\n");
		}
	}

	SDL_SetWindowMinimumSize(sdl_window, 320, 240);
}

static void VID_SDL_Init(void)
{
	int flags;
	
	if (glConfig.initialized == true) {
		return;
	}

	VID_SDL_InitSubSystem();

	flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_SHOWN;
	// MEAG: deliberately not specifying SDL_WINDOW_ALLOW_HIGHDPI as in our current workflow, it
	//          breaks retina devices (we ask for display resolution and get told lower value)
	//       Understand this is meant to be helped by NSHighResolutionCapable in Info.plist, but
	//          BLooD_DoG tried that and it didn't help.  No OSX device to test on, so removed
	//          for the moment.
	//       Flag has no effect on Windows (see SetProcessDpiAwarenessFunc in sys_win.c)
	if (r_fullscreen.integer > 0) {
		flags |= (vid_usedesktopres.integer == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	}
	else {
		flags |= (vid_win_borderless.integer > 0 ? SDL_WINDOW_BORDERLESS : 0);
	}

	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
		CL_SessionIsWorker() ||
		(CL_SessionIsCoordinator() && strcmp(SDL_GetCurrentVideoDriver(), "windows")) ||
		vid_minimize_on_focus_loss.integer == 0 ? "0" : "1");
#ifdef __APPLE__
	SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
#endif
	SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, vid_grab_keyboard.integer == 0 ? "0" : "1");
	if (CL_SessionIsWorker() && !session_alt_tab_initialized &&
		!strcmp(SDL_GetCurrentVideoDriver(), "x11")) {
		/* Preserve the user's Alt+Tab policy, but perform SDL's usual action
		 * through the coordinator. The keyboard grab itself is unchanged. */
		session_alt_tab_enabled = SDL_GetHintBoolean("SDL_ALLOW_ALT_TAB_WHILE_GRABBED", SDL_TRUE);
		session_alt_tab_initialized = true;
		SDL_SetHintWithPriority("SDL_ALLOW_ALT_TAB_WHILE_GRABBED", "0", SDL_HINT_OVERRIDE);
	}
	SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0", SDL_HINT_OVERRIDE);
#ifdef __APPLE__
#ifdef SDL_HINT_TOUCH_MOUSE_EVENTS
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
#endif

	{
		int i;
		int vid_options[] = {
			// Try to get everything they ask for...
			VID_MULTISAMPLED | VID_ACCELERATED | VID_DEPTHBUFFER24 | VID_GAMMACORRECTED,
			// ... multisampled off
			VID_ACCELERATED | VID_DEPTHBUFFER24 | VID_GAMMACORRECTED,
			// ... give up on gamma corrected
			VID_MULTISAMPLED | VID_ACCELERATED | VID_DEPTHBUFFER24,
			VID_ACCELERATED | VID_DEPTHBUFFER24,
			// 
			VID_ACCELERATED | VID_DEPTHBUFFER24 | VID_GAMMACORRECTED,
			VID_ACCELERATED | VID_GAMMACORRECTED,
			VID_ACCELERATED,
			// Un-accelerated and at this point we're desperate
			VID_DEPTHBUFFER24,
			VID_GAMMACORRECTED,
			0
		};

		while (true) {
			for (i = 0, sdl_window = NULL; sdl_window == NULL && i < sizeof(vid_options) / sizeof(vid_options[0]); ++i) {
				if ((vid_options[i] & VID_MULTISAMPLED) && gl_multisamples.integer <= 0) {
					continue;
				}
				if ((vid_options[i] & VID_DEPTHBUFFER24) && !r_24bit_depth.integer) {
					continue;
				}
				if ((vid_options[i] & VID_ACCELERATED) && COM_CheckParm(cmdline_param_client_unaccelerated_visuals)) {
					continue;
				}
				if ((vid_options[i] & VID_GAMMACORRECTED) && vid_gammacorrection.integer == 0) {
					continue;
				}
				if (!(vid_options[i] & VID_GAMMACORRECTED) && vid_gammacorrection.integer == 2) {
					continue;
				}

				sdl_window = NULL;
				VID_SDL_GL_SetupWindowAttributes(vid_options[i]);
				VID_SetupModeList();
				VID_SetupResolution();
				sdl_window = VID_SDL_CreateWindow(flags);

				if (sdl_window) {
					VID_SetWindowResolution();
					/* The coordinator owns a GL-capable drawable, but no context
					 * or renderer. Every playable engine creates its own context. */
					if (CL_SessionIsCoordinator())
						break;

					// Try to create context and see what we get
					sdl_context = VID_SDL_GL_SetupContextAttributes();

					if (!sdl_context) {
						SDL_DestroyWindow(sdl_window);
						sdl_window = NULL;
					}
					else {
						break;
					}
				}
			}

#if defined(RENDERER_OPTION_CLASSIC_OPENGL) && defined(EZ_MULTIPLE_RENDERERS)
			// FIXME: Implement falling back from Vulkan too
			if (!sdl_window && !R_UseImmediateOpenGL()) {
				Con_Printf("&cf00Error&r: failed to create rendering context, trying classic OpenGL...\n");

				Cvar_LatchedSetValue(&vid_renderer, 0);
				continue;
			}
#endif

			break;
		}

		if (!sdl_window) {
			Sys_Error("Failed to create SDL window/context: %s\n", SDL_GetError());
		}

		// Alert user if our mode doesn't match what they requested
		if (!(vid_options[i] & VID_MULTISAMPLED) && gl_multisamples.integer > 0) {
			Cvar_AutoSetInt(&gl_multisamples, 0);
			Com_Printf("WARNING: MSAA request failed - disabled\n");
		}

		if (!(vid_options[i] & VID_DEPTHBUFFER24) && r_24bit_depth.integer) {
			Cvar_AutoSetInt(&r_24bit_depth, 0);
			Com_Printf("WARNING: 24-bit depth buffer request failed\n");
		}

		if (!(vid_options[i] & VID_ACCELERATED) && !COM_CheckParm(cmdline_param_client_unaccelerated_visuals)) {
			Com_Printf("WARNING: Using unaccelerated graphics\n");
		}

		if (vid_gammacorrection.integer && !(vid_options[i] & VID_GAMMACORRECTED)) {
			Com_Printf("WARNING: Not able to apply gamma-correction\n");
			Cvar_LatchedSetValue(&vid_gammacorrection, 0);
		}
	}

	if (!CL_SessionIsWorker() && VID_SetWindowIcon(sdl_window) < 0) {
		Com_Printf("Failed to set window icon");
	}

	v_gamma.modified = true;
	r_swapInterval.modified = true;

#ifdef X11_GAMMA_WORKAROUND
	/* PLEASE REMOVE ME AS SOON AS SDL2 AND XORG ARE TALKING NICELY TO EACHOTHER AGAIN IN TERMS OF GAMMA */
	if (!CL_SessionIsCoordinator() && vid_gamma_workaround.integer != 0) {
		VID_X11_GetGammaRampSize();
	} else {
		glConfig.gammacrap.size = 256;
	}
#endif

	if (!CL_SessionIsCoordinator())
		R_Initialise();

	if (!M_ClassicMenus()) {
		MenuUI_VidInit(sdl_window, sdl_context, glConfig.majorVersion);
		M_ImGui_VidReady(true);
	}

	//always get/set refresh rate
	SDL_DisplayMode display_mode;
	int display_nbr;

	display_nbr = VID_DisplayNumber(true);
	if (SDL_GetDesktopDisplayMode(display_nbr, &display_mode) == 0) {
		Cvar_AutoSetInt(&r_displayRefresh, display_mode.refresh_rate);
	}

	glConfig.initialized = true;
	if (CL_SessionIsWorker())
		SDL_GetWindowSize(sdl_window, &glConfig.vidWidth, &glConfig.vidHeight);
	CL_SessionsAttachWindow(sdl_window);
#ifdef _WIN32
	if (CL_SessionIsCoordinator()) {
		coordinator_input_applied = false;
		VID_SessionInputSettings(NULL);
		SDL_StartTextInput();
	}
#endif
}

static void VID_SwapBuffers (void)
{
#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)
	if (r_swapInterval.integer == 1 && CL_SessionIsWorker()) {
		if (!session_swap_control_checked) {
			const char *(*query_extensions)(Display *, int);
			void *(*current_context)(void);
			const char *extensions, *extension;
			const char *name = "GLX_OML_sync_control";
			session_swap_control_checked = true;
			SDL_VERSION(&session_swap_wm.version);
			query_extensions = SDL_GL_GetProcAddress("glXQueryExtensionsString");
			current_context = SDL_GL_GetProcAddress("glXGetCurrentContext");
			if (query_extensions && current_context && current_context() &&
				SDL_GetWindowWMInfo(sdl_window, &session_swap_wm) && session_swap_wm.subsystem == SDL_SYSWM_X11) {
				extensions = query_extensions(session_swap_wm.info.x11.display, DefaultScreen(session_swap_wm.info.x11.display));
				extension = extensions ? strstr(extensions, name) : NULL;
				if (extension && (extension == extensions || extension[-1] == ' ') &&
					(extension[strlen(name)] == ' ' || extension[strlen(name)] == '\0'))
					session_swap_msc = SDL_GL_GetProcAddress("glXSwapBuffersMscOML");
			}
		}
		if (session_swap_msc) {
			/* Mesa's implicit VSync target uses sent-minus-completed swap counts.
			 * Present completion events on this shared X window also come from
			 * other engines, whose serials can rewind that per-process counter.
			 * A resumed engine can then wait thousands of refreshes for a frame.
			 * Explicit MSC scheduling (divisor 1) targets the next refresh without
			 * deriving a deadline from those independent serial numbers. OML does
			 * not implicitly flush rendering commands, so flush them ourselves. */
			glFlush();
			if (session_swap_msc(session_swap_wm.info.x11.display, session_swap_wm.info.x11.window, 0, 1, 0) >= 0)
				return;
			Con_Printf("Session refresh scheduling failed; using SDL buffer swaps.\n");
			session_swap_msc = NULL;
		}
	}
#endif
	SDL_GL_SwapWindow(sdl_window);
}

void VID_SessionRelease(void)
{
#ifndef _WIN32
	/* Capture even a stationary cursor; Windows uses forwarded motion above. */
	if (CL_SessionIsActive() && ActiveApp && !Minimized && !mouse_active &&
		!session_cursor_pending && IN_OSMouseCursorRequired()) {
		SDL_GetMouseState(&session_cursor_x, &session_cursor_y);
		session_cursor_valid = true;
	}
#endif
	/* Finish the old context's writes before another process uses the drawable. */
	glFinish();
}

void VID_SessionActivate(void)
{
	int flags;
	/* Discard queued input from the previous owner while we are still inactive. */
	HandleEvents();
	session_cursor_pending = session_cursor_valid;
	flags = SDL_GetWindowFlags(sdl_window);
#ifdef _WIN32
	ActiveApp = CL_SessionIsWorker() ? CL_SessionWindowIsFocused() : (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
	session_mouse_x = session_mouse_y = 0;
	session_cursor_sequence = 0;
#else
	ActiveApp = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
	Minimized = CL_SessionWindowIsMinimized();
	scr_skipupdate = 0;
	/* GLX swap interval belongs to the shared drawable, not the engine. */
	r_swapInterval.modified = true;
}

static void VID_SwapBuffersWithVsyncFix(void)
{
	double time_before_swap;

	time_before_swap = Sys_DoubleTime();

	VID_SwapBuffers();

	vid_last_swap_time = Sys_DoubleTime();
	vid_vsync_lag = vid_last_swap_time - time_before_swap;
}

void R_BeginRendering(int *x, int *y, int *width, int *height)
{

	*x = *y = 0;
	*width = glConfig.vidWidth;
	*height = glConfig.vidHeight;

	if (renderer.IsFramebufferEnabled3D()) {
		int scaled_width = VID_ScaledWidth3D();
		int scaled_height = VID_ScaledHeight3D();

		if (scaled_width && scaled_height) {
			*width = scaled_width;
			*height = scaled_height;
		}
	}

	if (cls.state != ca_active) {
		renderer.ClearRenderingSurface(true);
	}
}

void R_EndRendering(void)
{
	if (CL_SessionIsActive() && r_swapInterval.modified) {
		if (r_swapInterval.integer == 0) {
			if (SDL_GL_SetSwapInterval(0)) {
				Con_Printf("vsync: Failed to disable vsync...\n");
			}
            // MacOS vsync fix
            #ifdef __APPLE__
            GLint                       sync = 0;
            CGLContextObj               ctx = CGLGetCurrentContext();
            CGLSetParameter(ctx, kCGLCPSwapInterval, &sync);
            #endif
		} else if (r_swapInterval.integer == -1) {
			if (SDL_GL_SetSwapInterval(-1)) {
				Con_Printf("vsync: Failed to enable late swap tearing (vid_vsync -1), setting vid_vsync 1 instead...\n");
				Cvar_SetValueByName("vid_vsync", 1);
			}
		}

		if (r_swapInterval.integer == 1) {
			if (SDL_GL_SetSwapInterval(1)) {
				Con_Printf("vsync: Failed to enable vsync...\n");
			}
		}

		r_swapInterval.modified = false;
    }

	if (CL_SessionIsActive() && (!scr_skipupdate || block_drawing)) {
		if (vid_vsync_lag_fix.integer > 0) {
			VID_SwapBuffersWithVsyncFix();
		}
		else {
			VID_SwapBuffers(); 
		}
	}

	buffers.EndFrame();
}

void VID_SetCaption (char *text)
{
	if (CL_SessionSetCaption(text))
		return;
	if (!sdl_window) {
		return;
	}

	SDL_SetWindowTitle(sdl_window, text);
}

void VID_NotifyActivity(void)
{
#ifdef _WIN32
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);

	if (ActiveApp || !vid_flashonactivity.value) {
		return;
	}

	if (SDL_GetWindowWMInfo(sdl_window, &info) == SDL_TRUE) {
		if (info.subsystem == SDL_SYSWM_WINDOWS) {
			FlashWindow(GetAncestor(info.info.win.window, GA_ROOT), TRUE);
		}
	}
	else {
		Com_DPrintf("Sys_NotifyActivity: SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
	}
#endif
}

int VID_SetDeviceGammaRamp(unsigned short *ramps)
{
	if (CL_SessionIsWorker())
		return 0;
	if (!sdl_window || (COM_CheckParm(cmdline_param_client_nohardwaregamma) && Ruleset_AllowNoHardwareGamma())) {
		return 0;
	}

	if (r_fullscreen.integer > 0) {
		if (vid_hwgammacontrol.integer > 0) {
			return VID_SetDeviceGammaRampReal(ramps);
		}
	}
	else {
		if (vid_hwgammacontrol.integer >= 2) {
			return VID_SetDeviceGammaRampReal(ramps);
		}
	}

	return 0;
}

void VID_Minimize (void) 
{
	if (CL_SessionRequestWindowAction(false))
		return;
	if (!sdl_window) {
		return;
	}

	SDL_MinimizeWindow(sdl_window);
}

void VID_Restore (void)
{
	if (CL_SessionRequestWindowAction(true))
		return;
	if (!sdl_window || (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS)) {
		return;
	}

	SDL_RestoreWindow(sdl_window);
	SDL_RaiseWindow(sdl_window);
}

static void VID_ParseCmdLine(void)
{
	int i, w = 0, h = 0, display = 0;

	if (COM_CheckParm(cmdline_param_client_windowedmode) || COM_CheckParm(cmdline_param_client_startwindowed)) {
		Cvar_LatchedSetValue(&r_fullscreen, 0);
	}

	if ((i = COM_CheckParm(cmdline_param_client_video_frequency)) && i + 1 < COM_Argc()) {
		Cvar_LatchedSetValue(&r_displayRefresh, Q_atoi(COM_Argv(i + 1)));
	}

	if ((i = COM_CheckParm(cmdline_param_client_video_bpp)) && i + 1 < COM_Argc()) {
		Cvar_LatchedSetValue(&r_colorbits, Q_atoi(COM_Argv(i + 1)));
	}

	if (COM_CheckParm(cmdline_param_client_video_nodesktopres)) {
		Cvar_LatchedSetValue(&vid_usedesktopres, 0);
	}

	w = ((i = COM_CheckParm(cmdline_param_client_video_width))  && i + 1 < COM_Argc()) ? Q_atoi(COM_Argv(i + 1)) : 0;
	h = ((i = COM_CheckParm(cmdline_param_client_video_height)) && i + 1 < COM_Argc()) ? Q_atoi(COM_Argv(i + 1)) : 0;

	display = ((i = COM_CheckParm(cmdline_param_client_video_displaynumber)) && i + 1 < COM_Argc()) ? Q_atoi(COM_Argv(i + 1)) : 0;
	if (i) {
		if (COM_CheckParm(cmdline_param_client_windowedmode)) {
			Cvar_LatchedSetValue(&vid_win_displayNumber, display);
		} else {
			Cvar_LatchedSetValue(&vid_displayNumber, display);
		}
	}

	if (w && h) {
		if (COM_CheckParm(cmdline_param_client_windowedmode)) {
			Cvar_LatchedSetValue(&vid_win_width,  w);
			Cvar_LatchedSetValue(&vid_win_height, h);
		}
		else {
			Cvar_LatchedSetValue(&vid_width, w);
			Cvar_LatchedSetValue(&vid_height, h);
		}
	} // else if (w || h) { Sys_Error("Must specify both -width and -height\n"); }

	if ((i = COM_CheckParm(cmdline_param_client_video_conwidth)) && i + 1 < COM_Argc()) {
		Cvar_SetIgnoreCallback(&r_conwidth, COM_Argv(i + 1));
	}

	if ((i = COM_CheckParm(cmdline_param_client_video_conheight)) && i + 1 < COM_Argc()) {
		Cvar_SetIgnoreCallback(&r_conheight, COM_Argv(i + 1));
	}

#if defined(EZ_MULTIPLE_RENDERERS) && defined(RENDERER_OPTION_MODERN_OPENGL)
	if (COM_CheckParm(cmdline_param_client_video_glsl_renderer)) {
		Cvar_LatchedSetValue(&vid_renderer, 1);
	}
#endif
}

void GFX_Init(void);
void ReloadPaletteAndColormap(void);

static void VID_Startup(void)
{
	qbool old_con_suppress = con_suppress;

	// force models to reload (just flush, no actual loading code here)
	Cache_Flush();

	// shut up warnings during GFX_Init();
	con_suppress = !developer.integer;
	// reload 2D textures, particles textures, some other textures and gfx.wad
	GFX_Init();

	// reload skins
	Skin_Skins_f();

	con_suppress = old_con_suppress;

	// we need done something like for map reloading, for example reload textures for brush models
	R_NewMap(true);
	CL_SpraysRefreshRendererTextures();

	// window may be re-created, so caption need to be forced to update
	CL_UpdateCaption(true);

	// last chance
	CachePics_AtlasFrame();
	// compile all programs
	R_ProgramCompileAll();

	Cvar_ClearAllModifiedFlags(CVAR_RELOAD_GFX);
}

void VID_ReloadCvarChanged(cvar_t* var)
{
	if (!vid_reload_auto.integer) {
		Con_Printf("%s needs %s to immediately take effect.\n", var->name, CVAR_RELOAD_GFX_COMMAND);
	}
	else {
		vid_reload_pending = true;
	}
}

static void VID_Reload_f(void)
{
	if (!host_initialized) { // sanity
		Com_Printf("Can't do %s yet\n", Cmd_Argv(0));
		return;
	}

	VID_SoftRestart();
	ReloadPaletteAndColormap();
	VID_Startup();
	vid_reload_pending = false;
}

void VID_ReloadCheck(void)
{
	if (vid_reload_pending && host_initialized) {
		VID_Reload_f();
	}
}

void VID_SessionRestart(const session_video_settings_t *settings, qbool apply_pending)
{
	struct saved_latch { cvar_t *var; char *value; } *saved = NULL;
	cvar_t *var;
	int i, count = 0;
	if (CL_SessionIsCoordinator()) {
		VID_Shutdown(true);
		VID_SessionSyncWindowSettings(settings);
		VID_SDL_Init();
		vid_initialized = true;
		return;
	}
	/* A shared-window rebuild is not permission to apply another session's
	 * pending renderer changes. Keep those latches through reinitialization. */
	if (!apply_pending) {
		for (var = Cvar_Next(NULL); var; var = Cvar_Next(var))
			if ((var->flags & CVAR_LATCH_GFX) && var->latchedString)
				++count;
		if (count)
			saved = Q_malloc(count * sizeof(*saved));
		for (var = Cvar_Next(NULL), i = 0; var; var = Cvar_Next(var)) {
			if ((var->flags & CVAR_LATCH_GFX) && var->latchedString) {
				saved[i].var = var;
				saved[i++].value = var->latchedString;
				var->latchedString = NULL;
			}
		}
	}
	/* Workers have already torn down graphics before acknowledging DETACHED. */
	if (vid_initialized)
		VID_Shutdown(true);
	VID_ApplySessionWindowSettings(settings, false);
	ReloadPaletteAndColormap();
	Key_ClearStates();
	CL_ClearSessionInput();
	VID_Init(host_basepal);
	VID_Startup();
	for (i = 0; i < count; ++i) {
		Q_free(saved[i].var->latchedString);
		saved[i].var->latchedString = saved[i].value;
	}
	Q_free(saved);
}

static void VID_Restart_f(void)
{
	if (CL_SessionIsWorker() && host_initialized && !host_everything_loaded) {
		CL_SessionRestartVideo();
		return;
	}
	if (!CL_SessionIsActive()) {
		Con_Printf("Select this session before restarting its renderer.\n");
		return;
	}
	if (!host_initialized) { // sanity
		Com_Printf("Can't do %s yet\n", Cmd_Argv(0));
		return;
	}

	if (CL_SessionRestartVideo())
		return;
	VID_Shutdown(true);

	ReloadPaletteAndColormap();

	// keys can get stuck because SDL2 doesn't send keyup event when the video system is down
	Key_ClearStates();
	CL_ClearSessionInput();

	VID_Init(host_basepal);

	VID_Startup();
}

static void VID_DisplayList_f(void)
{
	int displays = SDL_GetNumVideoDisplays();
	int i;

	for (i = 0; i < displays; i++) {
		const char *displayname = SDL_GetDisplayName(i);
		if (displayname == NULL) {
			displayname = "Unknown";
		}
		Com_Printf("%d: %s\n", i, displayname);
	}
}

static void VID_ModeList_f(void)
{
	int i = 0;

	if (modelist == NULL || modelist_count <= 0) {
		Com_Printf("error: no modes available\n");
		return;
	}
	
	for (; i < modelist_count; i++) {
		Com_Printf("%dx%d@%dHz\n", (&modelist[i])->w, (&modelist[i])->h, (&modelist[i])->refresh_rate);
	}
}

void VID_RegisterCommands(void) 
{
	if (!host_initialized) {
		Cmd_AddCommand("vid_gfxinfo", VID_GfxInfo_f);
		Cmd_AddCommand("vid_restart", VID_Restart_f);
		Cmd_AddCommand(CVAR_RELOAD_GFX_COMMAND, VID_Reload_f);
		Cmd_AddCommand("vid_displaylist", VID_DisplayList_f);
		Cmd_AddCommand("vid_modelist", VID_ModeList_f);
		Cmd_AddLegacyCommand("vid_framebuffer_palette", vid_software_palette.name);
	}
}

static void VID_UpdateConRes(void)
{
	qbool set_width = false;
	qbool set_height = false;

	int vidWidth = glConfig.vidWidth;
	int vidHeight = glConfig.vidHeight;

	int effective_width = vid_framebuffer.integer == 1 ? VID_ScaledWidth3D() : 0;
	int effective_height = vid_framebuffer.integer == 1 ? VID_ScaledHeight3D() : 0;

	if (effective_width && effective_height) {
		vidWidth = effective_width;
		vidHeight = effective_height;

		Cvar_AutoSetInt(&vid_framebuffer_width, effective_width);
		Cvar_AutoSetInt(&vid_framebuffer_height, effective_height);
	}
	else {
		Cvar_AutoReset(&vid_framebuffer_width);
		Cvar_AutoReset(&vid_framebuffer_height);
	}

	// Default
	if (r_conwidth.integer == 0 && r_conheight.integer == 0) {
		vid.width = vid.conwidth = bound(320, (int)(vidWidth  / r_conscale.value), vidWidth);
		vid.height = vid.conheight = bound(200, (int)(vidHeight / r_conscale.value), vidHeight);

		set_width = set_height = true;
	}
	else if (r_conwidth.integer == 0) {
		double ar_w = (double)vidWidth/(double)vidHeight;

		vid.height = vid.conheight = bound(200, r_conheight.integer, vidHeight);
		vid.width = vid.conwidth = bound(320, (int)(r_conheight.integer*ar_w + 0.5), vidWidth);

		set_width = true;
	}
	else if (r_conheight.integer == 0) {
		double ar_h = (double)vidHeight/(double)vidWidth;

		vid.height = vid.conheight = bound(200, (int)(r_conwidth.integer*ar_h + 0.5), vidHeight);
		vid.width = vid.conwidth = bound(320, r_conwidth.integer, vidWidth);

		set_height = true;
	}
	else {
		// User specified, use that but check boundaries
		vid.width = vid.conwidth = bound(320, r_conwidth.integer,  vidWidth);
		vid.height = vid.conheight = bound(200, r_conheight.integer, vidHeight);

		Cvar_SetValue(&r_conwidth, vid.conwidth);
		Cvar_SetValue(&r_conheight, vid.conheight);
	}

	if (set_width) {
		Cvar_AutoSetInt(&r_conwidth, vid.conwidth);
	}
	if (set_height) {
		Cvar_AutoSetInt(&r_conheight, vid.conheight);
	}

	vid.aspect = (double) vidWidth / (double) vidHeight;

	vid.numpages = 2; // ??
	Draw_AdjustConback();
	vid.recalc_refdef = 1;
	Con_CheckResize();
}

void GL_FramebufferSetFiltering(qbool linear);

static void framebuffer_smooth_changed_callback(cvar_t* var, char* string, qbool* cancel)
{
	if (string[0] == '\0' || string[1] != '\0' || !(string[0] == '0' || string[0] == '1')) {
		Com_Printf("Value of %s must be 0 or 1\n", var->name);
		*cancel = true;
		return;
	}

	GL_FramebufferSetFiltering(string[0] == '1');
}

static void conres_changed_callback(cvar_t *var, char *string, qbool *cancel)
{
	/* Cvar_SetValue won't trigger a recursive callback since we're in the callback,
	 * but it's required to set the values here first to make them apply, and then cancel
	 * set to true will force the caller to return immediatly when this callback returns...
	 */
	if (var == &r_conwidth) {
		Cvar_SetValue(&r_conwidth, Q_atoi(string));
	}
	else if (var == &r_conheight) {
		Cvar_SetValue(&r_conheight, Q_atoi(string));
	}
	else if (var == &r_conscale) {
		Cvar_SetValue(&r_conscale, Q_atof(string));
	}
	else if (var == &vid_framebuffer) {
		Cvar_SetValue(&vid_framebuffer, Q_atof(string));
	}
	else if (var == &vid_framebuffer_width) {
		Cvar_SetValue(&vid_framebuffer_width, Q_atof(string));
	}
	else if (var == &vid_framebuffer_height) {
		Cvar_SetValue(&vid_framebuffer_height, Q_atof(string));
	}
	else if (var == &vid_framebuffer_scale) {
		Cvar_SetValue(&vid_framebuffer_scale, Q_atof(string));
	}
	else {
		Com_Printf("Called with unknown variable: %s\n", var->name ? var->name : "unknown");
	}

	VID_UpdateConRes();
	*cancel = true;
}

void VID_Init(unsigned char *palette)
{
	vid.colormap = host_colormap;

	Check_Gamma(palette);
	VID_SetPalette(palette);

	VID_RegisterLatchCvars();

	if (!host_initialized) {
		VID_RegisterCvars();
		VID_RegisterCommands();
		VID_ParseCmdLine();
	}

	VID_SDL_Init();

	// print info
	if (!host_initialized || r_verbose.integer) {
		VID_GfxInfo_f();
	}

	VID_UpdateConRes();

	vid_initialized = true;
}

void VID_CoordinatorInit(void)
{
	VID_RegisterLatchCvars();
	VID_RegisterCvars();
	VID_ParseCmdLine();
	SDL_SetHint(SDL_HINT_APP_NAME, "ezQuake");
	VID_SDL_Init();
	vid_initialized = true;
}

void VID_CoordinatorFrame(void)
{
	/* The Windows owner captures input; only workers execute bindings/gameplay. */
#ifdef _WIN32
	VID_SessionInputSettings(&coordinator_input);
#endif
	HandleEvents();
#ifdef _WIN32
	VID_SessionInputSettings(&coordinator_input);
	if (coordinator_applied_input.grab && !coordinator_applied_input.raw) {
		SDL_Event event;
		int x, y, width, height;
		SDL_GetWindowSize(sdl_window, &width, &height);
		SDL_GetMouseState(&x, &y);
		if (x != width / 2 || y != height / 2) {
			memset(&event, 0, sizeof(event));
			event.type = SDL_MOUSEMOTION;
			event.motion.xrel = x - width / 2;
			event.motion.yrel = y - height / 2;
			CL_SessionsForwardInput(&event);
			SDL_WarpMouseInWindow(sdl_window, width / 2, height / 2);
		}
	}
	SDL_GetRelativeMouseState(NULL, NULL);
#endif
}

int VID_ScaledWidth3D(void)
{
	if (vid_framebuffer_scale.value > 0) {
		return glConfig.vidWidth / max(vid_framebuffer_scale.value, 0.25);
	}
	else if (vid_framebuffer_width.integer > 0) {
		return max(vid_framebuffer_width.integer, 320);
	}
	return glConfig.vidWidth;
}

int VID_ScaledHeight3D(void)
{
	if (vid_framebuffer_scale.value > 0) {
		return glConfig.vidHeight / max(vid_framebuffer_scale.value, 0.25);
	}
	else if (vid_framebuffer_height.integer > 0) {
		return max(vid_framebuffer_height.integer, 200);
	}
	return glConfig.vidHeight;
}

int VID_RenderWidth2D(void)
{
	if (vid_framebuffer.integer == USE_FRAMEBUFFER_3DONLY) {
		return glConfig.vidWidth;
	}
	return glwidth;
}

int VID_RenderHeight2D(void)
{
	if (vid_framebuffer.integer == USE_FRAMEBUFFER_3DONLY) {
		return glConfig.vidHeight;
	}
	return glheight;
}
