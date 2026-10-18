#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures, hookFailure, hookCalls, unhooks;
static bool unhookFailure, subclassFailure;
static HHOOK WINAPI TestHook(int type, HOOKPROC, HINSTANCE, DWORD) {
    ++hookCalls;
    return type == hookFailure ? nullptr : reinterpret_cast<HHOOK>(type + 10);
}
static BOOL WINAPI TestUnhook(HHOOK) { ++unhooks; return !unhookFailure; }
static LONG_PTR WINAPI TestSubclass(HWND w, int index, LONG_PTR value) {
    return subclassFailure ? 0 : SetWindowLongPtrW(w, index, value);
}
static HMODULE g_self;
static WCHAR g_folder[32768];
static void Log(const char*, ...) {}
#define SetWindowsHookExW TestHook
#define UnhookWindowsHookEx TestUnhook
#undef SetWindowLongPtrW
#define SetWindowLongPtrW TestSubclass
#include "../src/runtime_fixes.inl"
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef SetWindowLongPtrW

static void Check(bool value, const char* label) {
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
static unsigned protectCalls, flushCalls;
static unsigned failProtectMask, failFlushMask;
static bool queryFailure;
static FARPROC* raceSlot;
static FARPROC raceValue;
static BOOL WINAPI Protect(void* address, SIZE_T size, DWORD mode, DWORD* old) {
    ++protectCalls;
    if (failProtectMask & (1u << protectCalls)) return FALSE;
    BOOL ok = VirtualProtect(address, size, mode, old);
    if (ok && raceSlot && protectCalls == 1) *raceSlot = raceValue;
    return ok;
}
static BOOL WINAPI Flush(HANDLE process, const void* address, SIZE_T size) {
    ++flushCalls;
    return (failFlushMask & (1u << flushCalls)) ? FALSE : FlushInstructionCache(process, address, size);
}
static SIZE_T WINAPI Query(const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size) {
    return queryFailure ? 0 : VirtualQuery(address, info, size);
}
static void ResetFaults() {
    protectCalls = flushCalls = failProtectMask = failFlushMask = 0;
    raceSlot = nullptr; queryFailure = false; MemoryHooks::status = 0;
    MemoryHooks::protect = Protect; MemoryHooks::flush = Flush;
    MemoryHooks::query = Query;
}
static INT_PTR WINAPI Original() { return 1; }
static INT_PTR WINAPI Replacement() { return 2; }
static INT_PTR WINAPI Rival() { return 3; }
static void MemoryTests() {
    auto memory = static_cast<BYTE*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) { Check(false, "allocate memory fixture"); return; }
    auto slot = reinterpret_cast<FARPROC*>(memory);
    FARPROC original = Original;
    *slot = Original;
    DWORD unused;
    VirtualProtect(memory, 4096, PAGE_READONLY, &unused);
    HMODULE module = GetModuleHandleW(nullptr);
    ResetFaults(); queryFailure = true;
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module) && protectCalls == 0,
        "IAT query failure never dereferences an unvalidated slot");
    ResetFaults();
    VirtualProtect(memory,4096,PAGE_READONLY|PAGE_GUARD,&unused);
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module), "IAT guarded slot is rejected");
    VirtualProtect(memory,4096,PAGE_READONLY,&unused);
    ResetFaults();
    Check(MemoryHooks::Replace(slot, Replacement, &original, module) && *slot == Replacement && original == Original,
        "IAT read-only page, callable original retained");
    Check(MemoryHooks::Replace(slot, Replacement, &original, module) && original == Original,
        "IAT duplicate installation does not forward to itself");
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(slot, &info, sizeof(info));
    Check(info.Protect == PAGE_READONLY, "IAT original protection restored");
    VirtualProtect(memory, 4096, PAGE_READWRITE, &unused); *slot = Rival;
    ResetFaults();
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module) && *slot == Rival && protectCalls == 0,
        "IAT rejects a different API even within the expected module");
    *slot = Original; VirtualProtect(memory, 4096, PAGE_READONLY, &unused);
    ResetFaults(); failProtectMask = 1u << 1;
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module) && *slot == Original,
        "IAT initial protection failure makes no change");
    ResetFaults(); raceSlot = slot; raceValue = Rival;
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module) && *slot == Rival && original == Original,
        "IAT concurrent replacement is preserved by compare-exchange");
    VirtualProtect(memory, 4096, PAGE_READWRITE, &unused); *slot = Original;
    VirtualProtect(memory, 4096, PAGE_READONLY, &unused);
    ResetFaults(); failProtectMask = (1u << 2) | (1u << 3);
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module) && *slot == Original &&
        (MemoryHooks::status & MemoryHooks::ProtectionFailure), "IAT restore failure rolls back and reports failure");
    VirtualQuery(slot, &info, sizeof(info));
    Check(info.Protect == PAGE_READONLY, "IAT rollback retries original protection");
    ResetFaults(); failProtectMask = 0xfffffffc; // OS keeps refusing after the initial grant.
    Check(!MemoryHooks::Replace(slot, Replacement, &original, module) &&
        (MemoryHooks::status & MemoryHooks::ProtectionFailure) && original == Original,
        "persistent OS restoration refusal is reported, callable original retained");
    VirtualProtect(memory,4096,PAGE_READWRITE,&unused); *slot = Original;
    VirtualProtect(memory, 8192, PAGE_EXECUTE_READWRITE, &unused);
    for (int scenario = 0; scenario < 6; ++scenario) {
        MemoryHooks::CodePatch pair[2] = {};
        for (int i = 0; i < 2; ++i) {
            pair[i].address = memory + i * 4096; pair[i].size = 6;
            memset(pair[i].before, 0x90, 6); memset(pair[i].after, 0xcc, 6);
            memcpy(pair[i].address, pair[i].before, 6);
        }
        if (scenario == 1) memory[4096] = 0x91;
        VirtualProtect(memory, 8192, PAGE_EXECUTE_READ, &unused);
        ResetFaults();
        if (scenario == 2) failProtectMask = 1u << 1;
        if (scenario == 3) failProtectMask = 1u << 2;
        if (scenario == 4) failFlushMask = 1u << 1;
        if (scenario == 5) failProtectMask = (1u << 3) | (1u << 4);
        const bool ok = MemoryHooks::PatchPair(pair);
        Check(ok == (scenario == 0), "cursor pair reports success only after both patches and cleanup");
        Check(memory[0] == (scenario == 0 ? 0xcc : 0x90) &&
            memory[4096] == (scenario == 0 ? 0xcc : scenario == 1 ? 0x91 : 0x90),
            "cursor mismatch/protect/flush/restore failure leaves original pair");
        VirtualQuery(memory, &info, sizeof(info));
        Check(info.Protect == PAGE_EXECUTE_READ, "cursor first page protection restored");
        VirtualQuery(memory + 4096, &info, sizeof(info));
        Check(info.Protect == PAGE_EXECUTE_READ, "cursor second page protection restored");
        VirtualProtect(memory, 8192, PAGE_EXECUTE_READWRITE, &unused);
    }
    VirtualFree(memory, 0, MEM_RELEASE);
}

static DWORD lastFrom, lastTo, backendPosition;
static unsigned mciCalls;
static MCIERROR WINAPI Mci(MCIDEVICEID, UINT command, DWORD_PTR, DWORD_PTR args) {
    ++mciCalls;
    if (command == MCI_PLAY) {
        auto p = reinterpret_cast<MCI_PLAY_PARMS*>(args); lastFrom = p->dwFrom; lastTo = p->dwTo;
    }
    if (command == MCI_SEEK) lastTo = reinterpret_cast<MCI_SEEK_PARMS*>(args)->dwTo;
    if (command == MCI_STATUS) reinterpret_cast<MCI_STATUS_PARMS*>(args)->dwReturn = backendPosition;
    return 0;
}
static void MciTests() {
    s_mci = Mci; s_audioDevice = 7; s_tracks = 3;
    s_track[1] = 150; s_track[2] = 1000; s_track[3] = 2000;
    s_audioStart = 900; s_discEnd = 3000; s_timeFormat = MCI_FORMAT_TMSF;
    MCI_PLAY_PARMS play = {}; play.dwFrom = MCI_MAKE_TMSF(2,0,0,0); play.dwTo = MCI_MAKE_TMSF(3,0,0,0);
    Check(!FixedMci(7,MCI_PLAY,MCI_FROM|MCI_TO,reinterpret_cast<DWORD_PTR>(&play)) &&
        lastFrom == 1333 && lastTo == 14666, "PLAY converts track boundaries to WAV milliseconds");
    unsigned calls = mciCalls;
    play.dwFrom = MCI_MAKE_TMSF(1,0,0,0);
    Check(FixedMci(7,MCI_PLAY,MCI_FROM|MCI_TO,reinterpret_cast<DWORD_PTR>(&play)) == MCIERR_OUTOFRANGE &&
        calls == mciCalls, "PLAY rejects data track before WAV without calling backend");
    play.dwFrom = MCI_MAKE_TMSF(2,0,60,0);
    Check(FixedMci(7,MCI_PLAY,MCI_FROM|MCI_TO,reinterpret_cast<DWORD_PTR>(&play)) == MCIERR_OUTOFRANGE,
        "PLAY rejects invalid seconds");
    play.dwFrom = MCI_MAKE_TMSF(2,0,0,75);
    Check(FixedMci(7,MCI_PLAY,MCI_FROM|MCI_TO,reinterpret_cast<DWORD_PTR>(&play)) == MCIERR_OUTOFRANGE,
        "PLAY rejects invalid frames");
    play.dwFrom = MCI_MAKE_TMSF(2,1,0,0);
    Check(FixedMci(7,MCI_PLAY,MCI_FROM|MCI_TO,reinterpret_cast<DWORD_PTR>(&play)) == MCIERR_OUTOFRANGE,
        "PLAY rejects TMSF offset beyond selected track");
    Check(FixedMci(7,MCI_SET,MCI_SET_TIME_FORMAT,0) == MCIERR_NULL_PARAMETER_BLOCK, "SET null block rejected");
    s_timeFormat = MCI_FORMAT_MILLISECONDS;
    play.dwFrom = MAXDWORD;
    Check(FixedMci(7,MCI_PLAY,MCI_FROM,reinterpret_cast<DWORD_PTR>(&play)) == MCIERR_OUTOFRANGE,
        "PLAY extreme milliseconds rejected");
    MCI_SEEK_PARMS seek = {}; seek.dwTo = 40000;
    Check(!FixedMci(7,MCI_SEEK,MCI_TO,reinterpret_cast<DWORD_PTR>(&seek)) && lastTo == 28000,
        "SEEK permits exact disc end");
    seek.dwTo = 40014;
    Check(FixedMci(7,MCI_SEEK,MCI_TO,reinterpret_cast<DWORD_PTR>(&seek)) == MCIERR_OUTOFRANGE,
        "SEEK rejects position beyond disc end");
    MCI_STATUS_PARMS status = {}; status.dwItem = MCI_STATUS_LENGTH; status.dwTrack = 3;
    Check(!FixedMci(7,MCI_STATUS,MCI_STATUS_ITEM|MCI_TRACK,reinterpret_cast<DWORD_PTR>(&status)) && status.dwReturn == 13333,
        "STATUS final track length uses disc boundary");
    status.dwTrack = 4;
    Check(FixedMci(7,MCI_STATUS,MCI_STATUS_ITEM|MCI_TRACK,reinterpret_cast<DWORD_PTR>(&status)) == MCIERR_OUTOFRANGE,
        "STATUS out-of-range track rejected");
    status.dwItem = MCI_STATUS_POSITION; backendPosition = MAXDWORD;
    Check(FixedMci(7,MCI_STATUS,MCI_STATUS_ITEM,reinterpret_cast<DWORD_PTR>(&status)) == MCIERR_OUTOFRANGE,
        "STATUS extreme backend position cannot wrap");
    backendPosition = 28000;
    Check(!FixedMci(7,MCI_STATUS,MCI_STATUS_ITEM,reinterpret_cast<DWORD_PTR>(&status)) && status.dwReturn == 40000,
        "STATUS exact WAV end maps to disc end");
    s_audioDevice = 0; s_tracks = 0;
    MCI_OPEN_PARMSA open = {}; open.lpstrDeviceType = "cdaudio"; open.lpstrElementName = "Z:";
    calls = mciCalls;
    Check(!FixedMci(0,MCI_OPEN,MCI_OPEN_TYPE|MCI_OPEN_ELEMENT,reinterpret_cast<DWORD_PTR>(&open)) && mciCalls == calls + 1,
        "invalid manifest preserves native CD open");
}

static void WindowTests() {
    const int types[] = {WH_CBT, WH_CALLWNDPROCRET, WH_GETMESSAGE, -1};
    for (int type : types) {
        hookFailure = type; hookCalls = unhooks = 0; unhookFailure = false;
        s_cbt = s_return = s_messages = nullptr; s_positionFix = true;
        WindowBehavior::Configure(true, true, true);
        InstallWindowHooks(true);
        Check(s_positionFix == (type != WH_CBT && type != WH_CALLWNDPROCRET), "popup pair enabled only when both hooks exist");
        Check(WindowBehavior::enabled == (type != WH_GETMESSAGE), "message-hook failure disables move/close behavior only");
        Check(unhooks == ((type == WH_CBT || type == WH_CALLWNDPROCRET) ? 1 : 0), "partial popup pair is rolled back");
    }
    hookFailure = WH_CALLWNDPROCRET; unhookFailure = true; s_positionFix = true;
    InstallWindowHooks(false);
    Check(!s_positionFix && s_cbt, "failed unhook retains handle with popup translation disabled");
    s_main = CreateWindowExW(0,L"STATIC",L"test main",WS_OVERLAPPEDWINDOW,200,150,640,480,nullptr,nullptr,nullptr,nullptr);
    HWND popup = CreateWindowExW(0,L"STATIC",L"test popup",WS_POPUP,0,0,100,100,nullptr,nullptr,nullptr,nullptr);
    CREATESTRUCTW create = {}; create.style = WS_POPUP; create.x = 20; create.y = 30; create.cx = create.cy = 100;
    CBT_CREATEWNDW cbt = {}; cbt.lpcs = &create;
    s_positionFix = true; subclassFailure = true; s_cbt = nullptr;
    WindowCreated(HCBT_CREATEWND,reinterpret_cast<WPARAM>(popup),reinterpret_cast<LPARAM>(&cbt));
    Check(!Anchor(popup) && create.x == 20 && create.y == 30 && !create.hwndParent,
        "subclass failure leaves popup coordinates and owner untouched");
    subclassFailure = false;
    WindowCreated(HCBT_CREATEWND,reinterpret_cast<WPARAM>(popup),reinterpret_cast<LPARAM>(&cbt));
    POINT origin = MainOrigin();
    Check(Anchor(popup) && create.x == origin.x + 20 && create.y == origin.y + 30 && create.hwndParent == s_main,
        "successful popup subclass translates origin and owner");
    WindowCreated(HCBT_DESTROYWND,reinterpret_cast<WPARAM>(popup),0);
    Check(!Anchor(popup), "popup destruction clears tracking and restores window procedure");
    DestroyWindow(popup); DestroyWindow(s_main); s_main = nullptr;
}
int main() {
    // Retain the full initializer in this translation unit for compile coverage.
    (void)&InstallRuntimeFixes;
    MemoryTests(); MciTests(); WindowTests();
    std::printf("Runtime safety failures: %d\n", failures);
    return failures ? 1 : 0;
}
