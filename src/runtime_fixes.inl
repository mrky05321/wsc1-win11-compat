// Included after the proxy logger. Only the identified WSC1 image is hooked.
#include <mmsystem.h>
#include <algorithm>
#include "window_behavior.inl"
#include "cd_manifest_validation.h"
#include "memory_hooks.h"

static HWND s_main;
struct PopupAnchor { HWND window; int x, y; WNDPROC previous; bool manual; };
static PopupAnchor s_popups[64] = {};
static bool s_adjusting;
static HHOOK s_cbt, s_return, s_messages;
static decltype(&SetWindowPos) s_position = SetWindowPos;
static decltype(&mciSendCommandA) s_mci = mciSendCommandA;
static bool s_positionFix;
static WCHAR s_audioPath[32768];
static DWORD s_track[100], s_discEnd, s_audioStart;
static unsigned s_tracks;
static MCIDEVICEID s_audioDevice;
static DWORD s_timeFormat = MCI_FORMAT_TMSF;

static BOOL CALLBACK FindMain(HWND w, LPARAM) {
    RECT r = {};
    WCHAR title[128] = {}; GetWindowTextW(w, title, ARRAYSIZE(title));
    if (IsWindowVisible(w) && !(GetWindowLongW(w, GWL_STYLE) & WS_CHILD) &&
        !wcscmp(title, L"鋼鉄の咆哮") &&
        GetClientRect(w, &r) && r.right >= 600 && r.bottom >= 440 &&
        r.right <= 1280 && r.bottom <= 960) {
        if (!s_main || !IsWindow(s_main)) s_main = w;
    }
    return TRUE;
}
static POINT MainOrigin() {
    if (!IsWindow(s_main)) {
        s_main = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), FindMain, 0);
    }
    POINT p = {};
    // dxwrapper can virtualize ClientToScreen/GetWindowRect for the game.
    // WINDOWINFO supplies the actual client origin in desktop coordinates.
    WINDOWINFO info = {}; info.cbSize = sizeof(info);
    if (s_main && GetWindowInfo(s_main, &info)) {
        p.x = info.rcClient.left; p.y = info.rcClient.top;
    }
    return p;
}

// Battle polls the cursor through 004060D0 rather than relying on WM_MOUSEMOVE.
// In the game's exclusive mode that helper leaves desktop coordinates intact.
// The graphics backend forces a window without changing that game mode flag.
static void (__cdecl* s_battleCursor)(POINT*);
static const BYTE* s_exclusive;
static const BYTE* s_applicationSlot;
static decltype(&GetCursorPos) s_getCursor = GetCursorPos;
static void __cdecl BattleCursor(POINT* point) {
    s_battleCursor(point);
    if (point && *s_exclusive && IsWindow(s_main)) {
        const POINT origin = MainOrigin();
        point->x -= origin.x;
        point->y -= origin.y;
    }
}
static bool IsBattleSprite(const BYTE* sprite) {
    if (!sprite || !s_applicationSlot || !MemoryHooks::Accessible(s_applicationSlot, sizeof(void*))) return false;
    const BYTE* application = nullptr;
    memcpy(&application, s_applicationSlot, sizeof(application));
    if (!application || !MemoryHooks::Accessible(application + 0x8c, sizeof(void*))) return false;
    const BYTE* battle = nullptr;
    memcpy(&battle, application + 0x8c, sizeof(battle));
    if (!battle || !MemoryHooks::Accessible(battle + 0x10aa, sizeof(void*))) return false;
    const BYTE* battleSprite = nullptr;
    memcpy(&battleSprite, battle + 0x10aa, sizeof(battleSprite));
    return sprite == battleSprite;
}
static BOOL WINAPI SpriteCursorForObject(POINT* point, const BYTE* sprite) {
    // The software cursor renderer independently calls GetCursorPos. Its draw
    // surface and clipping rectangle use client coordinates even in exclusive
    // mode, so correcting battle input alone does not position this image.
    if (!s_getCursor(point)) return FALSE;
    // Other renderer objects retain their original desktop-coordinate path.
    // Design placement refresh has a separate, scoped DesignCursor correction.
    if (IsBattleSprite(sprite) && IsWindow(s_main)) {
        const POINT origin = MainOrigin();
        point->x -= origin.x;
        point->y -= origin.y;
    }
    return TRUE;
}
static __declspec(naked) BOOL WINAPI SpriteCursor(POINT*) {
    // At the matched call site ESI is the renderer object. Preserve the native
    // GetCursorPos ABI (one stdcall argument) while passing that object onward.
    __asm {
        push esi
        push dword ptr [esp + 8]
        call SpriteCursorForObject
        ret 4
    }
}
static BOOL WINAPI DesignCursor(POINT* point) {
    if (!s_getCursor(point)) return FALSE;
    const POINT origin = MainOrigin();
    if (IsWindow(s_main)) {
        point->x -= origin.x;
        point->y -= origin.y;
    }
    return TRUE;
}
static bool InstallDesignPreview(BYTE* image) {
    // Only the left/right-key preview refresh uses this EDI binding. Its POINT
    // is packed into command 0x7FA, whose receiver expects client coordinates.
    // Global cursor APIs, mouse messages and stored part coordinates are untouched.
    static const BYTE binding[] = {0x8b,0x3d,0x10,0x12,0x4c,0x00};
    static const BYTE call[] = {0xff,0xd7};
    static const BYTE command[] = {0x68,0xfa,0x07,0x00,0x00};
    struct Match { DWORD rva; const BYTE* bytes; SIZE_T size; };
    const Match matches[] = {{0x4e925,binding,sizeof(binding)},
        {0x4e986,call,sizeof(call)},{0x4ea01,call,sizeof(call)},
        {0x4e995,command,sizeof(command)},{0x4ea10,command,sizeof(command)}};
    for (const auto& m : matches)
        if (!MemoryHooks::Accessible(image + m.rva,m.size,true) || memcmp(image + m.rva,m.bytes,m.size)) return false;
    MemoryHooks::CodePatch patches[1] = {};
    auto& patch = patches[0]; patch.address = image + 0x4e925; patch.size = sizeof(binding);
    memcpy(patch.before,binding,sizeof(binding));
    patch.after[0] = 0xbf; // mov edi, immediate callback; nop
    const DWORD callback = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(DesignCursor));
    memcpy(patch.after + 1,&callback,sizeof(callback)); patch.after[5] = 0x90;
    return MemoryHooks::PatchCode(patches);
}
static bool InstallCursorPair(BYTE* image) {
    static const BYTE instructions[] = {
        0x8d,0x44,0x24,0x1c,0x50, // lea eax,[esp+1c]; push eax
        0xff,0x15,0x4c,0x12,0x4c,0x00, // call the renderer's GetCursorPos
        0x8b,0x4c,0x24,0x1c,0x8b,0x56,0x1c,0x8b,0x46,0x20
    };
    static const BYTE helper[] = {
        0x56,0x8b,0x74,0x24,0x08,0x56,0xff,0x15,0xbc,0x12,0x4c,0x00,
        0xa0,0x64,0xb9,0x6a,0x00,0x84,0xc0,0x75,0x10,0xa1,0x7c,0xb7,
        0x6a,0x00,0x56,0x8b,0x40,0x0c,0x50,0xff,0x15,0x30,0x12,0x4c,
        0x00,0x5e,0xc3
    };
    if (!MemoryHooks::Accessible(image + 0x4715, sizeof(instructions), true) ||
        !MemoryHooks::Accessible(image + 0x60d0, sizeof(helper), true) ||
        !MemoryHooks::Accessible(image + 0x8e1d6, 5, true) ||
        memcmp(image + 0x4715, instructions, sizeof(instructions)) ||
        memcmp(image + 0x60d0, helper, sizeof(helper))) return false;
    BYTE* battle = image + 0x8e1d6;
    if (battle[0] != 0xe8 || battle + 5 + *reinterpret_cast<const LONG*>(battle + 1) != image + 0x60d0)
        return false;
    MemoryHooks::CodePatch patches[2] = {};
    patches[0].address = battle; patches[0].size = 5;
    patches[1].address = image + 0x471a; patches[1].size = 6;
    // Expected bytes come from the identified build, never a fresh snapshot
    // which could silently adopt a competing writer after signature validation.
    patches[0].before[0] = 0xe8;
    const DWORD originalRel = 0x60d0u - (0x8e1d6u + 5);
    memcpy(patches[0].before + 1, &originalRel, 4);
    memcpy(patches[1].before, instructions + 5, 6);
    for (auto& p : patches) p.after[0] = 0xe8;
    const DWORD battleRel = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(BattleCursor) - reinterpret_cast<ULONG_PTR>(battle + 5));
    const DWORD spriteRel = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(SpriteCursor) - reinterpret_cast<ULONG_PTR>(image + 0x471f));
    memcpy(patches[0].after + 1, &battleRel, 4);
    memcpy(patches[1].after + 1, &spriteRel, 4); patches[1].after[5] = 0x90;
    // Publish forwarding state before code can reach the replacement.
    s_battleCursor = reinterpret_cast<decltype(s_battleCursor)>(image + 0x60d0);
    s_exclusive = image + 0x2ab964;
    s_applicationSlot = image + 0x2ab77c;
    return MemoryHooks::PatchPair(patches);
}
static PopupAnchor* Anchor(HWND w) {
    for (auto& a : s_popups) if (a.window == w) return &a;
    return nullptr;
}
static LRESULT CALLBACK PopupProcedure(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    auto a = Anchor(w);
    WNDPROC previous = a ? a->previous : nullptr;
    if (!previous) return DefWindowProcW(w, msg, wp, lp);
    if (msg == WM_ENTERSIZEMOVE) a->manual = true;
    if (msg == WM_EXITSIZEMOVE) a->manual = false;
    if (msg == WM_WINDOWPOSCHANGING && !s_adjusting && !a->manual && IsWindow(s_main)) {
        auto p = reinterpret_cast<WINDOWPOS*>(lp);
        POINT origin = MainOrigin();
        if (!(p->flags & SWP_NOMOVE) &&
            (p->x != origin.x + a->x || p->y != origin.y + a->y) &&
            p->x >= 0 && p->y >= 0 && p->x < 640 && p->y < 480) {
            a->x = p->x; a->y = p->y;
            p->x += origin.x; p->y += origin.y;
            Log("Popup move translated hwnd=%p logical=%d,%d screen=%d,%d", w, a->x, a->y, p->x, p->y);
        }
    }
    return CallWindowProcW(previous, w, msg, wp, lp);
}
static LRESULT CALLBACK WindowCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND && !s_adjusting) {
        auto c = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        HWND w = reinterpret_cast<HWND>(wp);
        POINT p = MainOrigin();
        // Real child controls already use client coordinates. Translate only small
        // top-level game popups created inside the original 640x480 canvas.
        WCHAR cls[128] = {}; GetClassNameW(w, cls, ARRAYSIZE(cls));
        if (s_positionFix && s_main && !(c->style & WS_CHILD) && (c->style & WS_POPUP) &&
            c->x != CW_USEDEFAULT && c->y != CW_USEDEFAULT && c->cx > 0 && c->cy > 0 &&
            c->cx <= 640 && c->cy <= 480 &&
            wcscmp(cls, L"#32768") && wcscmp(cls, L"tooltips_class32") &&
            (!c->hwndParent || c->hwndParent == s_main || c->hwndParent == GetDesktopWindow())) {
            for (auto& a : s_popups) if (!a.window) {
                // Dialog creation initially uses the owner's absolute origin;
                // WSC1 subsequently MoveWindow's it to unadjusted logical xy.
                bool absolute = c->hwndParent == s_main &&
                    c->x >= p.x && c->x < p.x + 640 && c->y >= p.y && c->y < p.y + 480;
                a = {w, absolute ? c->x - p.x : c->x, absolute ? c->y - p.y : c->y, nullptr, false};
                a.previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PopupProcedure)));
                if (!a.previous) {
                    // Do not alter the popup position or owner if subclassing failed.
                    a = {};
                    Log("Popup subclass installation failed hwnd=%p error=%lu", w, GetLastError());
                    break;
                }
                if (!absolute) { c->x += p.x; c->y += p.y; }
                c->hwndParent = s_main; // owner; never turn the popup into a child
                Log("Popup tracked hwnd=%p class=%ls logical=%d,%d screen=%d,%d proc=%p",
                    w, cls, a.x, a.y, c->x, c->y, a.previous);
                break;
            }
        }
    } else if (code == HCBT_DESTROYWND) {
        if (auto a = Anchor(reinterpret_cast<HWND>(wp))) {
            if (a->previous && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(a->window, GWLP_WNDPROC)) == PopupProcedure)
                SetWindowLongPtrW(a->window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(a->previous));
            *a = {};
        }
        if (s_main == reinterpret_cast<HWND>(wp)) s_main = nullptr;
    }
    return CallNextHookEx(s_cbt, code, wp, lp);
}
static LRESULT CALLBACK WindowReturned(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && s_positionFix && !s_adjusting) {
        auto c = reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (c->message == WM_WINDOWPOSCHANGED) {
            if (c->hwnd == s_main && !IsIconic(s_main)) {
                s_adjusting = true;
                POINT p = MainOrigin();
                for (auto& a : s_popups) if (IsWindow(a.window))
                    s_position(a.window, nullptr, p.x + a.x, p.y + a.y, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                s_adjusting = false;
            } else if (auto a = Anchor(c->hwnd)) {
                WINDOWINFO r = {}; r.cbSize = sizeof(r);
                if (GetWindowInfo(c->hwnd, &r)) {
                    POINT p = MainOrigin(); a->x = r.rcWindow.left - p.x; a->y = r.rcWindow.top - p.y;
                }
            }
        }
    }
    return CallNextHookEx(s_return, code, wp, lp);
}

static LRESULT CALLBACK WindowMessage(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE) {
        MainOrigin();
        if (WindowBehavior::Attach(s_main))
            WindowBehavior::FilterMessage(*reinterpret_cast<MSG*>(lp));
    }
    return CallNextHookEx(s_messages, code, wp, lp);
}

static DWORD FramesToMs(DWORD frames) { return static_cast<DWORD>((static_cast<ULONGLONG>(frames) * 1000) / 75); }
static unsigned TrackAt(DWORD frame) {
    unsigned t = 1;
    while (t < s_tracks && frame >= s_track[t + 1]) ++t;
    return t;
}
static DWORD EncodeCdTime(DWORD frame, bool length = false) {
    DWORD ms = FramesToMs(frame);
    if (s_timeFormat == MCI_FORMAT_MILLISECONDS) return ms;
    if (s_timeFormat == MCI_FORMAT_TMSF && !length) {
        unsigned t = TrackAt(frame); frame -= s_track[t];
        return MCI_MAKE_TMSF(t, frame / 4500, (frame / 75) % 60, frame % 75);
    }
    return MCI_MAKE_MSF(frame / 4500, (frame / 75) % 60, frame % 75);
}
static bool DecodeCdTime(DWORD value, DWORD& frame) {
    if (s_timeFormat == MCI_FORMAT_MILLISECONDS) {
        frame = static_cast<DWORD>((static_cast<ULONGLONG>(value) * 75) / 1000); return true;
    }
    if (s_timeFormat == MCI_FORMAT_TMSF) {
        unsigned t = MCI_TMSF_TRACK(value);
        if (t < 1 || t > s_tracks ||
            MCI_TMSF_SECOND(value) >= 60 || MCI_TMSF_FRAME(value) >= 75) return false;
        frame = s_track[t] + MCI_TMSF_MINUTE(value) * 4500 + MCI_TMSF_SECOND(value) * 75 + MCI_TMSF_FRAME(value);
        if (frame > (t == s_tracks ? s_discEnd : s_track[t + 1])) return false;
    } else {
        if (MCI_MSF_SECOND(value) >= 60 || MCI_MSF_FRAME(value) >= 75) return false;
        frame = MCI_MSF_MINUTE(value) * 4500 + MCI_MSF_SECOND(value) * 75 + MCI_MSF_FRAME(value);
    }
    return true;
}
static bool IsCdOpen(DWORD_PTR flags, DWORD_PTR args) {
    if (!args || !(flags & MCI_OPEN_TYPE)) return false;
    auto p = reinterpret_cast<MCI_OPEN_PARMSA*>(args);
    return (flags & MCI_OPEN_TYPE_ID) ? reinterpret_cast<ULONG_PTR>(p->lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO :
        p->lpstrDeviceType && !_stricmp(p->lpstrDeviceType, "cdaudio");
}
static MCIERROR WINAPI FixedMci(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR args) {
    bool cd = msg == MCI_OPEN && IsCdOpen(flags, args);
    if (cd && s_tracks && !s_audioDevice && ValidCdWave(s_audioPath, s_discEnd - s_audioStart)) {
        MCI_OPEN_PARMSW p = {};
        p.lpstrDeviceType = reinterpret_cast<LPCWSTR>(MCI_DEVTYPE_WAVEFORM_AUDIO);
        p.lpstrElementName = s_audioPath;
        MCIERROR err = mciSendCommandW(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_TYPE_ID | MCI_OPEN_ELEMENT | MCI_WAIT,
            reinterpret_cast<DWORD_PTR>(&p));
        if (!err) {
            MCI_SET_PARMS set = {}; set.dwTimeFormat = MCI_FORMAT_MILLISECONDS;
            err = mciSendCommandW(p.wDeviceID, MCI_SET, MCI_SET_TIME_FORMAT | MCI_WAIT, reinterpret_cast<DWORD_PTR>(&set));
            if (!err) {
                s_audioDevice = p.wDeviceID; s_timeFormat = MCI_FORMAT_TMSF;
                reinterpret_cast<MCI_OPEN_PARMSA*>(args)->wDeviceID = s_audioDevice;
                Log("CD audio redirected to PCM device=%u tracks=%u", s_audioDevice, s_tracks);
                return 0;
            }
            mciSendCommandW(p.wDeviceID, MCI_CLOSE, MCI_WAIT, 0);
        }
        Log("PCM open failed error=%lu; preserving native CD path", err);
    }
    if (s_audioDevice && id == s_audioDevice) {
        MCIERROR err;
        switch (msg) {
        case MCI_CLOSE:
            err = s_mci(id, msg, flags, args); if (!err) s_audioDevice = 0; return err;
        case MCI_SET:
            if (flags & MCI_SET_TIME_FORMAT) {
                if (!args) return MCIERR_NULL_PARAMETER_BLOCK;
                DWORD f = reinterpret_cast<MCI_SET_PARMS*>(args)->dwTimeFormat;
                if (f != MCI_FORMAT_TMSF && f != MCI_FORMAT_MSF && f != MCI_FORMAT_MILLISECONDS) return MCIERR_BAD_TIME_FORMAT;
                s_timeFormat = f;
                flags &= ~MCI_SET_TIME_FORMAT;
                if (!flags || flags == MCI_WAIT) return 0;
            }
            return s_mci(id, msg, flags, args);
        case MCI_PLAY: {
            if (!args) return MCIERR_NULL_PARAMETER_BLOCK;
            MCI_PLAY_PARMS p = *reinterpret_cast<MCI_PLAY_PARMS*>(args);
            DWORD from = s_track[2], to = s_discEnd;
            if ((flags & MCI_FROM) && !DecodeCdTime(p.dwFrom, from)) return MCIERR_OUTOFRANGE;
            if ((flags & MCI_TO) && !DecodeCdTime(p.dwTo, to)) return MCIERR_OUTOFRANGE;
            if (from < s_audioStart || from >= s_discEnd || to > s_discEnd || to <= from) return MCIERR_OUTOFRANGE;
            p.dwFrom = FramesToMs(from - s_audioStart); p.dwTo = FramesToMs(to - s_audioStart);
            err = s_mci(id, msg, flags, reinterpret_cast<DWORD_PTR>(&p));
            Log("CD play from=%lu to=%lu ms=%lu..%lu flags=0x%lx error=%lu", from, to, p.dwFrom, p.dwTo, static_cast<DWORD>(flags), err);
            return err;
        }
        case MCI_SEEK: {
            if (!args) return MCIERR_NULL_PARAMETER_BLOCK;
            MCI_SEEK_PARMS p = *reinterpret_cast<MCI_SEEK_PARMS*>(args);
            if (flags & MCI_TO) {
                DWORD frame; if (!DecodeCdTime(p.dwTo, frame) || frame < s_audioStart || frame > s_discEnd) return MCIERR_OUTOFRANGE;
                p.dwTo = FramesToMs(frame - s_audioStart);
            }
            return s_mci(id, msg, flags, reinterpret_cast<DWORD_PTR>(&p));
        }
        case MCI_STATUS: {
            if (!args) return MCIERR_NULL_PARAMETER_BLOCK;
            auto p = reinterpret_cast<MCI_STATUS_PARMS*>(args);
            if (!(flags & MCI_STATUS_ITEM)) return MCIERR_MISSING_PARAMETER;
            unsigned t = static_cast<unsigned>(p->dwTrack);
            if ((flags & MCI_TRACK) && (t < 1 || t > s_tracks)) return MCIERR_OUTOFRANGE;
            switch (p->dwItem) {
            case MCI_STATUS_NUMBER_OF_TRACKS: p->dwReturn = s_tracks; return 0;
            case MCI_STATUS_TIME_FORMAT: p->dwReturn = s_timeFormat; return 0;
            case MCI_STATUS_MEDIA_PRESENT:
            case MCI_STATUS_READY: p->dwReturn = TRUE; return 0;
            case MCI_STATUS_LENGTH:
                p->dwReturn = EncodeCdTime((flags & MCI_TRACK) ? ((t == s_tracks ? s_discEnd : s_track[t+1]) - s_track[t]) : s_discEnd, true); return 0;
            case MCI_CDA_STATUS_TYPE_TRACK: p->dwReturn = (t == 1) ? MCI_CDA_TRACK_OTHER : MCI_CDA_TRACK_AUDIO; return 0;
            case MCI_STATUS_POSITION:
                if (flags & MCI_TRACK) { p->dwReturn = EncodeCdTime(s_track[t]); return 0; }
                if (flags & MCI_STATUS_START) { p->dwReturn = EncodeCdTime(s_track[1]); return 0; }
                // Fall through to the actual PCM position, translated to CD time.
            case MCI_STATUS_CURRENT_TRACK: {
                MCI_STATUS_PARMS q = {}; q.dwItem = MCI_STATUS_POSITION;
                err = s_mci(id, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT, reinterpret_cast<DWORD_PTR>(&q));
                if (err) return err;
                ULONGLONG offset = (static_cast<ULONGLONG>(q.dwReturn) * 75 + 500) / 1000;
                if (offset > s_discEnd - s_audioStart) return MCIERR_OUTOFRANGE;
                DWORD frame = s_audioStart + static_cast<DWORD>(offset);
                p->dwReturn = p->dwItem == MCI_STATUS_CURRENT_TRACK ? TrackAt(frame) : EncodeCdTime(frame); return 0;
            }
            default: return s_mci(id, msg, flags, args);
            }
        }
        case MCI_GETDEVCAPS: {
            if (!args) return MCIERR_NULL_PARAMETER_BLOCK;
            auto p = reinterpret_cast<MCI_GETDEVCAPS_PARMS*>(args);
            if (p->dwItem == MCI_GETDEVCAPS_DEVICE_TYPE) { p->dwReturn = MCI_DEVTYPE_CD_AUDIO; return 0; }
            if (p->dwItem == MCI_GETDEVCAPS_CAN_EJECT || p->dwItem == MCI_GETDEVCAPS_CAN_RECORD || p->dwItem == MCI_GETDEVCAPS_CAN_SAVE) { p->dwReturn = FALSE; return 0; }
            return s_mci(id, msg, flags, args);
        }
        default: return s_mci(id, msg, flags, args);
        }
    }
    MCI_OPEN_PARMSA nativeOpen = {};
    char cdElement[4] = {};
    if (cd && !(flags & MCI_OPEN_ELEMENT)) {
        // A CD-less file installation can still use a real/virtual original CD.
        // Avoid opening the first optical drive when other games are mounted.
        char drives[256] = {};
        GetLogicalDriveStringsA(ARRAYSIZE(drives), drives);
        for (const char* d = drives; *d; d += strlen(d) + 1) {
            char volume[128] = {};
            if (GetDriveTypeA(d) == DRIVE_CDROM && GetVolumeInformationA(d, volume, ARRAYSIZE(volume), nullptr, nullptr, nullptr, nullptr, 0) &&
                !_stricmp(volume, "Kurogane")) {
                nativeOpen = *reinterpret_cast<MCI_OPEN_PARMSA*>(args);
                cdElement[0] = d[0]; cdElement[1] = ':';
                nativeOpen.lpstrElementName = cdElement;
                Log("Native CD audio selecting WSC1 drive=%s", cdElement);
                MCIERROR result = s_mci(id, msg, flags | MCI_OPEN_ELEMENT, reinterpret_cast<DWORD_PTR>(&nativeOpen));
                if (!result) reinterpret_cast<MCI_OPEN_PARMSA*>(args)->wDeviceID = nativeOpen.wDeviceID;
                Log("Native CD open error=%lu device=%u", result, nativeOpen.wDeviceID);
                return result;
            }
        }
    }
    MCIERROR err = s_mci(id, msg, flags, args);
    Log("Native MCI device=%u command=0x%x flags=0x%lx error=%lu%s", id, msg, static_cast<DWORD>(flags), err, cd ? " CD_OPEN" : "");
    return err;
}

static bool ReplaceSlot(BYTE* image, DWORD rva, FARPROC replacement, FARPROC* original, HMODULE expected) {
    const bool success = MemoryHooks::Replace(reinterpret_cast<FARPROC*>(image + rva), replacement, original, expected);
    if (!success) MemoryHooks::status |= MemoryHooks::IatSkipped;
    return success;
}

// Keep any failed-to-remove hook handle so the remaining callback can still
// chain. All popup callbacks are inert when the pair is unavailable.
static void InstallWindowHooks(bool windowed) {
    if (s_positionFix) {
        s_cbt = SetWindowsHookExW(WH_CBT, WindowCreated, g_self, GetCurrentThreadId());
        s_return = SetWindowsHookExW(WH_CALLWNDPROCRET, WindowReturned, g_self, GetCurrentThreadId());
        if (!s_cbt || !s_return) {
            s_positionFix = false;
            MemoryHooks::status |= MemoryHooks::WindowHookFailure;
            if (s_cbt && UnhookWindowsHookEx(s_cbt)) s_cbt = nullptr;
            if (s_return && UnhookWindowsHookEx(s_return)) s_return = nullptr;
        }
    }
    if (windowed) {
        s_messages = SetWindowsHookExW(WH_GETMESSAGE, WindowMessage, g_self, GetCurrentThreadId());
        if (!s_messages) {
            MemoryHooks::status |= MemoryHooks::WindowHookFailure;
            WindowBehavior::Configure(false, false, false);
        }
    }
}
static void InstallRuntimeFixes(const WCHAR* ini) {
    BYTE* image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    WCHAR name[MAX_PATH]; GetModuleFileNameW(nullptr, name, ARRAYSIZE(name));
    WCHAR* basename = wcsrchr(name, L'\\');
    if (!basename || _wcsicmp(basename + 1, L"BCS.ICD") || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != 0x3855e0a2 || nt->OptionalHeader.SizeOfImage != 0x3a9000) {
        Log("Runtime fixes skipped: main image is not the identified WSC1 build"); return;
    }
    s_positionFix = GetPrivateProfileIntW(L"Compatibility", L"FixPopupCoordinates", 1, ini) != 0;
    WCHAR backendIni[32768]; swprintf_s(backendIni, L"%sdxwrapper.ini", g_folder);
    const bool windowed = GetPrivateProfileIntW(L"d3d9", L"EnableWindowMode", 0, backendIni) != 0 &&
        GetPrivateProfileIntW(L"d3d9", L"FullscreenWindowMode", 0, backendIni) == 0;
    WindowBehavior::Configure(windowed,
        GetPrivateProfileIntW(L"Compatibility", L"FixWindowMove", 1, ini) != 0,
        GetPrivateProfileIntW(L"Compatibility", L"FixAltF4", 1, ini) != 0);
    if (windowed && GetPrivateProfileIntW(L"Compatibility", L"FixBattleMouse", 1, ini)) {
        if (!InstallCursorPair(image)) MemoryHooks::status |= MemoryHooks::CursorSkipped;
    }
    if (windowed && GetPrivateProfileIntW(L"Compatibility", L"FixDesignPreview", 1, ini)) {
        if (!InstallDesignPreview(image)) MemoryHooks::status |= MemoryHooks::DesignSkipped;
    }
    InstallWindowHooks(windowed);
    WCHAR manifest[32768]; swprintf_s(manifest, L"%smusic\\cd-audio.ini", g_folder);
    swprintf_s(s_audioPath, L"%smusic\\cd-audio.wav", g_folder);
    const DWORD wavAttributes = GetFileAttributesW(s_audioPath);
    if (GetPrivateProfileIntW(L"Compatibility", L"DigitalCDAudio", 1, ini) &&
        wavAttributes != INVALID_FILE_ATTRIBUTES && !(wavAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        DWORD tracks = 0, discEnd = 0, audioStart = 0;
        bool parsed = ReadManifestNumber(L"Disc", L"Tracks", manifest, tracks) &&
            ReadManifestNumber(L"Disc", L"EndFrame", manifest, discEnd) &&
            ReadManifestNumber(L"Disc", L"AudioStartFrame", manifest, audioStart);
        DWORD positions[ARRAYSIZE(s_track)] = {};
        if (tracks >= 2 && tracks < ARRAYSIZE(positions)) {
            for (unsigned i = 1; i <= tracks; ++i) {
                WCHAR key[16]; swprintf_s(key, L"%u", i);
                if (!ReadManifestNumber(L"Tracks", key, manifest, positions[i])) parsed = false;
            }
        }
        if (parsed && ValidCdManifest(tracks, discEnd, audioStart, positions, ARRAYSIZE(positions)) &&
            ValidCdWave(s_audioPath, discEnd - audioStart)) {
            s_tracks = tracks; s_discEnd = discEnd; s_audioStart = audioStart;
            memcpy(s_track, positions, sizeof(s_track));
        } else {
            Log("Invalid CD audio timeline: ignoring PCM manifest and using native MCI");
        }
    }
    ReplaceSlot(image, 0xc1350, reinterpret_cast<FARPROC>(FixedMci), reinterpret_cast<FARPROC*>(&s_mci), GetModuleHandleW(L"winmm.dll"));
    Log("WSC1 runtime fixes v1.0.4 enabled; audio tracks=%u", s_tracks);
}
