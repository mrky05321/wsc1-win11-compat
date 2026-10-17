// UI-thread-only fixes for the identified game's forced windowed mode.
// The application retains ownership of cleanup after its message loop exits.
#include <commctrl.h>

namespace WindowBehavior {
static HWND window;
static bool enabled, fixMove, fixClose, inMove, attaching;
static const UINT_PTR subclassId = 0x57534331;

static LRESULT Default(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    return IsWindowUnicode(w) ? DefWindowProcW(w, msg, wp, lp) : DefWindowProcA(w, msg, wp, lp);
}

static LRESULT CALLBACK Procedure(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (fixClose && msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE) {
        if (IsWindowEnabled(w)) PostQuitMessage(0);
        return 0;
    }
    if (fixClose && msg == WM_SYSKEYDOWN && wp == VK_F4 && (lp & (1L << 29))) {
        if (!(lp & (1L << 30)) && IsWindowEnabled(w)) PostQuitMessage(0);
        return 0;
    }
    if (fixMove) {
        if (msg == WM_ENTERSIZEMOVE) inMove = true;
        // WSC1's fullscreen placement notifications are not needed by a manual
        // desktop move. Keep the backend's enter/exit and position notifications.
        if (inMove && (msg == WM_MOVING || msg == WM_MOVE))
            return Default(w, msg, wp, lp);
        if (inMove && msg == WM_WINDOWPOSCHANGING && lp && !IsIconic(w) && !IsZoomed(w)) {
            auto position = reinterpret_cast<WINDOWPOS*>(lp);
            if (!(position->flags & SWP_NOMOVE)) {
                const int x = position->x, y = position->y;
                const UINT flags = position->flags;
                LRESULT result = DefSubclassProc(w, msg, wp, lp);
                // dxwrapper's exclusive-surface size correction also rewrites
                // x/y to the display origin. Retain the user's requested move.
                position->x = x; position->y = y;
                position->flags &= ~SWP_NOMOVE;
                position->flags |= flags & SWP_NOSIZE;
                return result;
            }
        }
        if (msg == WM_EXITSIZEMOVE) {
            LRESULT result = DefSubclassProc(w, msg, wp, lp);
            inMove = false;
            return result;
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(w, Procedure, subclassId);
        if (window == w) { window = nullptr; inMove = false; }
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static void Configure(bool windowed, bool move, bool close) {
    enabled = windowed && (move || close);
    fixMove = move; fixClose = close;
}

static bool Attach(HWND w) {
    if (!enabled || !IsWindow(w) || attaching) return false;
    if (window == w) return true;
    if (window && IsWindow(window)) return false;
    attaching = true;
    const BOOL success = SetWindowSubclass(w, Procedure, subclassId, 0);
    if (success) window = w;
    attaching = false;
    return success != FALSE;
}

static void FilterMessage(MSG& msg) {
    // Some game loops consume system keys before DispatchMessage. Transform
    // only Alt+F4 for this enabled main window (or its child control), never an
    // owned modal dialog. Quit the loop before destroying the graphics window:
    // WSC1 otherwise waits for a rendering worker inside WM_DESTROY.
    if (!enabled || !fixClose || !IsWindowEnabled(window) ||
        (msg.hwnd != window && !IsChild(window, msg.hwnd))) return;
    if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_F4 && (msg.lParam & (1L << 29))) {
        if (!(msg.lParam & (1L << 30))) PostQuitMessage(0);
        msg.message = WM_NULL;
        msg.wParam = 0; msg.lParam = 0;
        msg.hwnd = window;
    }
}
} // namespace WindowBehavior
