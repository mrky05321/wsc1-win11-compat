#pragma once
#include <windows.h>
#include <cstring>

// Injectable OS boundary for failure tests. Status is available through the
// existing WSC1CompatGetStatus export; no disk diagnostics are produced.
namespace MemoryHooks {
static DWORD status;
enum : DWORD { IatSkipped = 0x100, CursorSkipped = 0x200,
    ProtectionFailure = 0x400, CacheFailure = 0x800, WindowHookFailure = 0x1000 };
static decltype(&VirtualProtect) protect = VirtualProtect;
static decltype(&FlushInstructionCache) flush = FlushInstructionCache;
static decltype(&VirtualQuery) query = VirtualQuery;
inline PVOID exchange(PVOID volatile* slot, PVOID value, PVOID expected) {
    return InterlockedCompareExchangePointer(slot, value, expected);
}

inline bool Accessible(const void* address, SIZE_T size, bool executable = false) {
    MEMORY_BASIC_INFORMATION info = {};
    if (!query(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readable) || (executable && !(info.Protect &
        (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))) return false;
    const ULONG_PTR offset = reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(info.BaseAddress);
    return offset <= info.RegionSize && size <= info.RegionSize - offset;
}
inline bool Restore(void* address, SIZE_T size, DWORD old) {
    DWORD unused;
    if (protect(address, size, old, &unused) || protect(address, size, old, &unused)) return true;
    status |= ProtectionFailure;
    return false;
}

inline bool Replace(FARPROC* slot, FARPROC replacement, FARPROC* original, HMODULE expected) {
    if (!slot || !original || !*original || !replacement || !expected ||
        reinterpret_cast<ULONG_PTR>(slot) % sizeof(void*) || !Accessible(slot, sizeof(*slot))) return false;
    const FARPROC current = *reinterpret_cast<FARPROC volatile*>(slot);
    // Idempotent only when a previous successful installation published a real
    // original. Never turn the trampoline into its own forwarding destination.
    if (current == replacement) return *original != replacement;
    MEMORY_BASIC_INFORMATION target = {};
    if (current != *original || !Accessible(reinterpret_cast<void*>(current), 1, true) ||
        !query(reinterpret_cast<void*>(current), &target, sizeof(target)) || target.AllocationBase != expected) return false;
    DWORD old;
    if (!protect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return false;
    // The original is already callable before publishing the hook. A competing
    // writer is never adopted as the original and never overwritten by us.
    const bool installed = exchange(reinterpret_cast<PVOID volatile*>(slot),
        reinterpret_cast<PVOID>(replacement), reinterpret_cast<PVOID>(current)) == reinterpret_cast<PVOID>(current);
    if (!Restore(slot, sizeof(*slot), old)) {
        DWORD unused;
        if (installed && protect(slot, sizeof(*slot), PAGE_READWRITE, &unused))
            exchange(reinterpret_cast<PVOID volatile*>(slot),
                reinterpret_cast<PVOID>(current), reinterpret_cast<PVOID>(replacement));
        Restore(slot, sizeof(*slot), old);
        return false;
    }
    return installed;
}

struct CodePatch { BYTE* address; SIZE_T size; BYTE before[6], after[6]; DWORD old; bool writable; };
inline bool PatchPair(CodePatch (&patches)[2]) {
    // Validate the entire pair before changing either site. Installation runs
    // once at first DirectDraw initialization, not at every cursor poll.
    for (auto& p : patches)
        if (!Accessible(p.address, p.size, true) || memcmp(p.address, p.before, p.size)) return false;
    for (auto& p : patches) {
        if (!protect(p.address, p.size, PAGE_EXECUTE_READWRITE, &p.old)) {
            for (auto& q : patches) if (q.writable) Restore(q.address, q.size, q.old);
            return false;
        }
        p.writable = true;
    }
    bool match = true;
    for (auto& p : patches) if (memcmp(p.address, p.before, p.size)) match = false;
    if (!match) {
        for (auto& p : patches) Restore(p.address, p.size, p.old);
        return false;
    }
    for (auto& p : patches) memcpy(p.address, p.after, p.size);
    bool success = true;
    for (auto& p : patches) if (!flush(GetCurrentProcess(), p.address, p.size)) {
        status |= CacheFailure; success = false;
    }
    for (auto& p : patches) if (!Restore(p.address, p.size, p.old)) success = false;
    if (success) return true;
    // Best-effort rollback. OS refusal cannot be made infallible; expose it to
    // callers instead of falsely reporting success. Do not overwrite a rival.
    for (auto& p : patches) {
        DWORD unused;
        if (protect(p.address, p.size, PAGE_EXECUTE_READWRITE, &unused)) {
            if (!memcmp(p.address, p.after, p.size)) memcpy(p.address, p.before, p.size);
            if (!flush(GetCurrentProcess(), p.address, p.size)) status |= CacheFailure;
            Restore(p.address, p.size, p.old);
        } else status |= ProtectionFailure;
    }
    return false;
}
} // namespace MemoryHooks
