"""Compile the actual focus cases and deferred minimize block with MSVC mocks."""
import ctypes
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "src/vid_sdl2.c"
source = source_path.read_text(encoding="utf-8")
start = source.index("static void window_event(SDL_WindowEvent *event)\n{")
end = source.index("\n\t\tcase SDL_WINDOWEVENT_MOVED:", start)
focus = source[start:end] + "\n default: break;\n }\n}\n"
marker = "\tif (session_minimize_pending) {"
flush = ""
if marker in source:
    start = source.index(marker, source.index("static void HandleEvents(void)\n{"))
    pos, depth = source.index("{", start), 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    flush = source[start:end]
fixture = r'''
#define true 1
#define false 0
#define SDL_WINDOW_FULLSCREEN 1
#define SDL_WINDOW_INPUT_FOCUS 2
#define SDL_WINDOW_MINIMIZED 4
#define SDL_WINDOW_FULLSCREEN_DESKTOP 9
#define SDL_WINDOWEVENT_MINIMIZED 1
#define SDL_WINDOWEVENT_FOCUS_LOST 2
#define SDL_WINDOWEVENT_FOCUS_GAINED 3
#define SDL_WINDOWEVENT_RESTORED 4
typedef int qbool;
typedef struct { int event; } SDL_WindowEvent;
typedef union { SDL_WindowEvent window; } SDL_Event;
static int coordinator=1, worker, flags=SDL_WINDOW_FULLSCREEN, sdl_window=1, minimized_calls;
static qbool ActiveApp, Minimized, scr_skipupdate, session_minimize_pending;
static int session_minimize_on_focus_loss=1;
static struct { int integer; } vid_minimize_on_focus_loss={1};
static int SDL_GetWindowFlags(int w) { return flags; }
static int CL_SessionIsCoordinator(void) { return coordinator; }
static int CL_SessionIsWorker(void) { return worker; }
static int CL_SessionWindowIsMinimized(void) { return flags & SDL_WINDOW_MINIMIZED; }
static void MenuUI_ProcessEvent(SDL_Event *e) { }
static void VID_SuspendSessionGamma(void) { }
static void Sys_ActiveAppChanged(void) { }
static void TP_ExecTrigger(const char *s) { }
static void VID_Minimize(void) { ++minimized_calls; }
'''
checks = r'''
static void event(int type) { SDL_WindowEvent e={type}; window_event(&e); }
__declspec(dllexport) int check(void) {
    event(SDL_WINDOWEVENT_FOCUS_LOST);
    if (!session_minimize_pending || minimized_calls) return 1;
    flush(); flush();
    if (session_minimize_pending || minimized_calls != 1) return 2;
    event(SDL_WINDOWEVENT_FOCUS_LOST);
    flags |= SDL_WINDOW_INPUT_FOCUS;
    event(SDL_WINDOWEVENT_FOCUS_GAINED); flush();
    if (session_minimize_pending || minimized_calls != 1) return 3;
    flags=SDL_WINDOW_FULLSCREEN; vid_minimize_on_focus_loss.integer=0; session_minimize_on_focus_loss=0;
    event(SDL_WINDOWEVENT_FOCUS_LOST);
    if (session_minimize_pending) return 4;
    vid_minimize_on_focus_loss.integer=1; session_minimize_on_focus_loss=1; flags=0;
    event(SDL_WINDOWEVENT_FOCUS_LOST);
    if (session_minimize_pending) return 5;
    flags=SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MINIMIZED;
    event(SDL_WINDOWEVENT_MINIMIZED); flush();
    if (minimized_calls != 1) return 6;
    flags=SDL_WINDOW_FULLSCREEN; coordinator=0;
    event(SDL_WINDOWEVENT_FOCUS_LOST);
    if (session_minimize_pending) return 7;
    worker=1; event(SDL_WINDOWEVENT_FOCUS_LOST);
    if (session_minimize_pending) return 8;
    /* Grabbed Alt+Tab may request minimization while focus is still held. */
    worker=0; coordinator=1; flags |= SDL_WINDOW_INPUT_FOCUS;
    session_minimize_pending=1; flush();
    if (minimized_calls != 2) return 9;
    /* Environment policy can disable minimization independently of the cvar. */
    flags=SDL_WINDOW_FULLSCREEN; session_minimize_on_focus_loss=0;
    event(SDL_WINDOWEVENT_FOCUS_LOST); flush();
    if (minimized_calls != 2) return 10;
    /* SDL's auto policy minimizes exclusive fullscreen, but not desktop fullscreen. */
    session_minimize_on_focus_loss=2; flags=SDL_WINDOW_FULLSCREEN_DESKTOP;
    event(SDL_WINDOWEVENT_FOCUS_LOST); flush();
    if (minimized_calls != 2) return 11;
    flags=SDL_WINDOW_FULLSCREEN;
    event(SDL_WINDOWEVENT_FOCUS_LOST); flush();
    if (minimized_calls != 3) return 12;
    return 0;
}
'''
compiler = sys.argv[2] if len(sys.argv) > 2 else shutil.which("cl")
if not compiler:
    raise SystemExit("Run from an x64 MSVC developer shell, or pass source path and cl.exe path.")
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / "check.c").write_text(fixture + focus + "\nstatic void flush(void) {\n" + flush + "\n}\n" + checks, encoding="utf-8")
    subprocess.run([str(compiler), "/nologo", "/LD", "/GS-", "check.c", "/link", "/NOENTRY", "/NODEFAULTLIB"], cwd=path, check=True)
    dll = ctypes.CDLL(str(path / "check.dll"))
    result = dll.check()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    kernel.FreeLibrary(dll._handle)
    assert result == 0, f"focus/minimize regression: case {result}"
print("PASS: deferred focus-loss minimization, cancellation, policy, workers and grabbed Alt+Tab")
