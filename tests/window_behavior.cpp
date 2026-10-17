#include <windows.h>
#include <cstdio>
#include "../src/window_behavior.inl"

static bool cancelClose, emulateBackend = true;
static unsigned backendPositions, gameMoves, closeRequests, destroys, enters, exits;
static int failures;

static void Check(bool condition, const char* description) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) ++failures;
}
static LRESULT CALLBACK LegacyProcedure(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING && emulateBackend && lp) {
        auto pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (!(pos->flags & SWP_NOSIZE)) {
            ++backendPositions;
            pos->x = pos->y = 0;
            pos->cx = 642; pos->cy = 507;
        }
    }
    if (msg == WM_MOVE) ++gameMoves;
    if (msg == WM_MOVING) { SetRect(reinterpret_cast<RECT*>(lp), 0, 0, 642, 507); return TRUE; }
    if (msg == WM_ENTERSIZEMOVE) ++enters;
    if (msg == WM_EXITSIZEMOVE) ++exits;
    // Match WSC1's handled-but-ignored close system command.
    if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE) return 0;
    if (msg == WM_CLOSE) {
        ++closeRequests;
        if (!cancelClose) DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) { ++destroys; return 0; }
    return DefWindowProcW(w, msg, wp, lp);
}
static HWND MakeWindow() {
    return CreateWindowExW(0, L"WSC1WindowBehaviorRegression", L"Hidden regression window",
        WS_OVERLAPPEDWINDOW, 200, 150, 642, 507, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
static bool Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return true;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return false;
}
int main() {
    WNDCLASSW cls = {}; cls.lpfnWndProc = LegacyProcedure;
    cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"WSC1WindowBehaviorRegression";
    if (!RegisterClassW(&cls)) return 2;
    HWND w = MakeWindow();
    if (!w) return 2;
    WindowBehavior::Configure(false, true, true);
    Check(!WindowBehavior::Attach(w), "fullscreen/native mode is not subclassed");
    WINDOWPOS position = {w, nullptr, 347, 225, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE};
    SendMessageW(w, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&position));
    Check(position.x == 0 && position.y == 0, "baseline reproduces backend origin rewrite");
    WindowBehavior::Configure(true, true, true);
    Check(WindowBehavior::Attach(w), "windowed main window is subclassed");
    Check(WindowBehavior::Attach(w), "repeated attachment retains one subclass");
    SendMessageW(w, WM_ENTERSIZEMOVE, 0, 0);
    position.x = 347; position.y = 225; position.cx = 640; position.cy = 480;
    const unsigned oldPositions = backendPositions;
    SendMessageW(w, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&position));
    Check(position.x == 347 && position.y == 225, "manual move retains requested desktop position");
    Check(backendPositions == oldPositions + 1 && position.cx == 642 && position.cy == 507,
        "backend size correction still runs");
    position.x = -350; position.y = -20;
    SendMessageW(w, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&position));
    Check(position.x == -350 && position.y == -20, "negative coordinates survive for other monitors");
    RECT moving = {300, 200, 942, 707};
    SendMessageW(w, WM_MOVING, WMSZ_TOP, reinterpret_cast<LPARAM>(&moving));
    Check(moving.left == 300 && moving.top == 200, "legacy moving rectangle rewrite is skipped");
    const unsigned oldMoves = gameMoves;
    SendMessageW(w, WM_MOVE, 0, MAKELPARAM(300, 200));
    Check(gameMoves == oldMoves, "manual movement does not invoke fullscreen game move handling");
    SendMessageW(w, WM_EXITSIZEMOVE, 0, 0);
    Check(enters == 1 && exits == 1, "backend enter/exit notifications remain paired");
    position.x = 347; position.y = 225;
    SendMessageW(w, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&position));
    Check(position.x == 0 && position.y == 0, "programmatic positions outside manual move are unchanged");

    HWND child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 20, 20, w, nullptr, cls.hInstance, nullptr);
    HWND owned = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 20, 20, w, nullptr, cls.hInstance, nullptr);
    MSG key = {}; key.hwnd = child; key.message = WM_SYSKEYDOWN; key.wParam = VK_F4; key.lParam = 1L << 29;
    WindowBehavior::FilterMessage(key);
    Check(key.hwnd == w && key.message == WM_NULL && Pump(), "queued Alt+F4 exits the UI message loop");
    key.hwnd = owned; key.message = WM_SYSKEYDOWN; key.wParam = VK_F4; key.lParam = 1L << 29;
    WindowBehavior::FilterMessage(key);
    Check(key.message == WM_SYSKEYDOWN && !Pump(), "owned dialog Alt+F4 is not redirected to the game");
    EnableWindow(w, FALSE);
    key.hwnd = w;
    WindowBehavior::FilterMessage(key);
    Check(key.message == WM_SYSKEYDOWN && !Pump(), "disabled main window is not closed behind a modal dialog");
    EnableWindow(w, TRUE);
    key.message = WM_SYSKEYDOWN; key.wParam = VK_F4; key.lParam = (1L << 29) | (1L << 30);
    WindowBehavior::FilterMessage(key);
    Check(key.message == WM_NULL && !Pump(), "held Alt+F4 does not repeat quit requests");
    key.message = WM_KEYDOWN; key.wParam = VK_F4; key.lParam = 0;
    WindowBehavior::FilterMessage(key);
    Check(key.message == WM_KEYDOWN, "F4 without Alt is preserved");
    DestroyWindow(owned);
    cancelClose = true;
    SendMessageW(w, WM_CLOSE, 0, 0); Pump();
    Check(closeRequests == 1 && IsWindow(w), "native WM_CLOSE still reaches the game and may be cancelled");
    SendMessageW(w, WM_SYSCOMMAND, SC_CLOSE, 0);
    Check(Pump() && IsWindow(w) && destroys == 0, "system close quits the loop before graphics-window destruction");
    DestroyWindow(w); // Model the application's cleanup after leaving its loop.
    Check(destroys == 1, "application-owned window cleanup still receives WM_DESTROY");

    w = MakeWindow();
    Check(WindowBehavior::Attach(w), "subclass can attach to a recreated window");
    SendMessageW(w, WM_SYSKEYDOWN, VK_F4, 1L << 29);
    Check(Pump() && IsWindow(w), "direct Alt+F4 also quits before window destruction");
    DestroyWindow(w);

    w = MakeWindow();
    WindowBehavior::Configure(true, true, false);
    Check(WindowBehavior::Attach(w), "move-only configuration attaches");
    const unsigned previousCloses = closeRequests;
    SendMessageW(w, WM_SYSCOMMAND, SC_CLOSE, 0);
    Check(!Pump() && closeRequests == previousCloses && IsWindow(w), "FixAltF4=0 preserves legacy system-close behavior");
    DestroyWindow(w);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    std::printf("Window behavior regression failures: %d\n", failures);
    return failures ? 1 : 0;
}
