// WSC1-specific loader/diagnostics. Rendering is provided by unmodified dxwrapper.
// Game files, clocks and saves are untouched. Validated runtime compatibility
// hooks are confined to this process (see runtime_fixes.inl).
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <wchar.h>

static HMODULE g_self;
static HMODULE g_backend;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static WCHAR g_folder[32768];
static DWORD g_status;
extern "C" { FARPROC g_slots[22] = {}; }
static const char* const g_names[22] = {
    "AcquireDDThreadLock", "CompleteCreateSysmemSurface", "D3DParseUnknownCommand",
    "DDGetAttachedSurfaceLcl", "DDInternalLock", "DDInternalUnlock", "DSoundHelp",
    "DirectDrawCreate", "DirectDrawCreateClipper", "DirectDrawCreateEx",
    "DirectDrawEnumerateA", "DirectDrawEnumerateExA", "DirectDrawEnumerateExW",
    "DirectDrawEnumerateW", "DllCanUnloadNow", "DllGetClassObject",
    "GetDDSurfaceLocal", "GetOLEThunkData", "GetSurfaceFromDC", "RegisterSpecialCase",
    "ReleaseDDThreadLock", "SetAppCompatData"
};

// This release never creates log files, even if an older INI enables them.
// Keep no-op call sites so runtime diagnostics cannot introduce file I/O.
static void Log(const char*, ...) {}
static void LogPath(const char*, const WCHAR*) {}

#include "runtime_fixes.inl"

static BOOL CALLBACK Init(PINIT_ONCE, PVOID, PVOID*) {
    DWORD count = GetModuleFileNameW(g_self, g_folder, ARRAYSIZE(g_folder));
    if (!count || count >= ARRAYSIZE(g_folder)) return FALSE;
    WCHAR* slash = wcsrchr(g_folder, L'\\');
    if (!slash) return FALSE;
    slash[1] = 0;
    WCHAR ini[32768], backend[32768], selected[32];
    if (swprintf_s(ini, L"%swsc1-compat.ini", g_folder) < 0) return FALSE;
    GetPrivateProfileStringW(L"Compatibility", L"Backend", L"dxwrapper", selected, ARRAYSIZE(selected), ini);
    Log("WSC1 DirectDraw proxy v1.0.3 x86");
    WCHAR image[32768];
    if (GetModuleFileNameW(nullptr, image, ARRAYSIZE(image))) LogPath("process", image);
    if (_wcsicmp(selected, L"system") == 0) {
        if (!GetSystemDirectoryW(backend, ARRAYSIZE(backend)) ||
            wcscat_s(backend, L"\\ddraw.dll") != 0) return FALSE;
        g_status = 2;
    } else if (_wcsicmp(selected, L"dxwrapper") == 0) {
        if (swprintf_s(backend, L"%sdxwrapper.dll", g_folder) < 0) return FALSE;
    } else {
        Log("Invalid Backend setting. Expected dxwrapper or system.");
        SetLastError(ERROR_BAD_CONFIGURATION);
        return FALSE;
    }
    LogPath("backend", backend);
    // Absolute path; dependencies can come only from that DLL directory or Windows.
    g_backend = LoadLibraryExW(backend, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_backend) {
        Log("Backend load failed winerror=%lu", GetLastError());
        return FALSE;
    }
    for (unsigned i = 0; i < ARRAYSIZE(g_slots); ++i) {
        g_slots[i] = GetProcAddress(g_backend, g_names[i]);
        if (!g_slots[i]) {
            Log("Backend missing export=%s", g_names[i]);
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
    }
    InstallRuntimeFixes(ini);
    g_status |= 1;
    Log("Backend ready; all 22 DirectDraw export names resolved");
    // Retain the LoadLibrary reference for process lifetime: backend COM objects
    // can survive this proxy's callers. Never unload it from DllMain.
    return TRUE;
}

extern "C" void __stdcall Resolve(unsigned index) {
    const DWORD previousError = GetLastError();
    if (index >= ARRAYSIZE(g_slots) || !InitOnceExecuteOnce(&g_once, Init, nullptr, nullptr)) {
        Log("Cannot resolve DirectDraw entrypoint index=%u", index);
        // Unknown legacy exports have varying ABIs; returning a guessed HRESULT
        // would corrupt the stack. Fail explicitly instead of forging success.
        RaiseFailFastException(nullptr, nullptr, 0);
        ExitProcess(ERROR_DLL_INIT_FAILED);
    }
    SetLastError(previousError);
}

#define FORWARD(name, index) \
    extern "C" __declspec(naked) void F_##name() { \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { push index } \
        __asm { call Resolve } \
        __asm { popad } \
        __asm { popfd } \
        __asm { jmp dword ptr [g_slots + index * 4] } \
    }
FORWARD(AcquireDDThreadLock, 0)
FORWARD(CompleteCreateSysmemSurface, 1)
FORWARD(D3DParseUnknownCommand, 2)
FORWARD(DDGetAttachedSurfaceLcl, 3)
FORWARD(DDInternalLock, 4)
FORWARD(DDInternalUnlock, 5)
FORWARD(DSoundHelp, 6)
FORWARD(DirectDrawCreateClipper, 8)
FORWARD(DirectDrawEnumerateA, 10)
FORWARD(DirectDrawEnumerateExA, 11)
FORWARD(DirectDrawEnumerateExW, 12)
FORWARD(DirectDrawEnumerateW, 13)
FORWARD(DllCanUnloadNow, 14)
FORWARD(DllGetClassObject, 15)
FORWARD(GetDDSurfaceLocal, 16)
FORWARD(GetOLEThunkData, 17)
FORWARD(GetSurfaceFromDC, 18)
FORWARD(RegisterSpecialCase, 19)
FORWARD(ReleaseDDThreadLock, 20)
FORWARD(SetAppCompatData, 21)

extern "C" HRESULT WINAPI ProxyDirectDrawCreate(GUID* guid, LPDIRECTDRAW* output, IUnknown* outer) {
    Resolve(7);
    typedef HRESULT (WINAPI *Fn)(GUID*, LPDIRECTDRAW*, IUnknown*);
    HRESULT hr = reinterpret_cast<Fn>(g_slots[7])(guid, output, outer);
    Log("DirectDrawCreate guid=%p outer=%p hr=0x%08lx object=%p", guid, outer,
        static_cast<unsigned long>(hr), SUCCEEDED(hr) && output ? *output : nullptr);
    return hr;
}

extern "C" HRESULT WINAPI ProxyDirectDrawCreateEx(GUID* guid, LPVOID* output, REFIID iid, IUnknown* outer) {
    Resolve(9);
    typedef HRESULT (WINAPI *Fn)(GUID*, LPVOID*, REFIID, IUnknown*);
    HRESULT hr = reinterpret_cast<Fn>(g_slots[9])(guid, output, iid, outer);
    Log("DirectDrawCreateEx iid.Data1=0x%08lx hr=0x%08lx object=%p",
        iid.Data1, static_cast<unsigned long>(hr), SUCCEEDED(hr) && output ? *output : nullptr);
    return hr;
}

extern "C" DWORD WINAPI CompatVersion() { return 0x00010003; }
extern "C" DWORD WINAPI CompatStatus() { return g_status | MemoryHooks::status; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_self = module;
    return TRUE;
}
