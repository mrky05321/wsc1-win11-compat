#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return 2;
    typedef DWORD (WINAPI* Query)();
    Query version = reinterpret_cast<Query>(GetProcAddress(dll, "WSC1CompatGetVersion"));
    Query status = reinterpret_cast<Query>(GetProcAddress(dll, "WSC1CompatGetStatus"));
    bool valid = version && status && version() == 0x00010004 && status() == 0;
    valid = valid && GetProcAddress(dll, MAKEINTRESOURCEA(100)) == reinterpret_cast<FARPROC>(version);
    valid = valid && GetProcAddress(dll, MAKEINTRESOURCEA(101)) == reinterpret_cast<FARPROC>(status);
    // Loading and querying metadata must not require dxwrapper or install game hooks.
    for (WORD ordinal = 1; ordinal <= 22; ++ordinal)
        valid = valid && GetProcAddress(dll, MAKEINTRESOURCEA(ordinal));
    FreeLibrary(dll);
    std::puts(valid ? "PASS: x86 proxy loads, exports and version match; hooks remain idle" : "FAIL: proxy metadata");
    return valid ? 0 : 3;
}
