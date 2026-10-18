#pragma once
// Validate the CD timeline before it is consumed by MCI track arithmetic.
// The caller owns start[0..capacity-1], with one-based CD track indices.
#include <windows.h>

inline bool ValidCdManifest(unsigned count, DWORD discEnd, DWORD audioStart,
                            const DWORD* start, unsigned capacity) {
    // MSF/TMSF store minutes in a byte. Bound all later additions/conversions.
    if (!start || count < 2 || count > 99 || count >= capacity ||
        discEnd > 256 * 4500 - 1 || audioStart >= discEnd)
        return false;
    if (start[1] > audioStart || start[2] < audioStart)
        return false; // The audio WAV may include the track-2 pregap.
    for (unsigned i = 1; i <= count; ++i) {
        if (start[i] >= discEnd || (i > 1 && start[i] <= start[i - 1]))
            return false;
    }
    return true;
}

inline bool ReadManifestNumber(const WCHAR* section, const WCHAR* key,
                               const WCHAR* path, DWORD& result) {
    WCHAR value[32];
    DWORD n = GetPrivateProfileStringW(section, key, L"", value, ARRAYSIZE(value), path);
    if (!n || n >= ARRAYSIZE(value) - 1) return false;
    ULONGLONG number = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (value[i] < L'0' || value[i] > L'9') return false;
        number = number * 10 + (value[i] - L'0');
        if (number > MAXDWORD) return false;
    }
    result = static_cast<DWORD>(number);
    return true;
}

// Accept the extractor's uncompressed CD PCM only. RIFF chunk sizes are checked
// against both the declared RIFF boundary and the physical file length.
inline bool ValidCdWave(const WCHAR* path, DWORD frames) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    DWORD header[3], read = 0;
    bool valid = GetFileSizeEx(file, &size) && size.QuadPart >= 12 &&
        ReadFile(file, header, sizeof(header), &read, nullptr) && read == sizeof(header) &&
        header[0] == 0x46464952 && header[2] == 0x45564157 &&
        ULONGLONG(header[1]) + 8 == ULONGLONG(size.QuadPart);
    bool format = false, data = false;
    ULONGLONG offset = 12;
    while (valid && offset < ULONGLONG(size.QuadPart)) {
        DWORD chunk[2];
        LARGE_INTEGER pos; pos.QuadPart = offset;
        if (offset + 8 > ULONGLONG(size.QuadPart) ||
            !SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) ||
            !ReadFile(file, chunk, sizeof(chunk), &read, nullptr) || read != sizeof(chunk)) { valid = false; break; }
        ULONGLONG end = offset + 8 + chunk[1];
        if (end + (chunk[1] & 1) > ULONGLONG(size.QuadPart)) { valid = false; break; }
        if (chunk[0] == 0x20746d66) { // fmt
            struct Pcm { WORD tag, channels; DWORD rate, bytes; WORD align, bits; } pcm = {};
            if (format || chunk[1] < sizeof(pcm) ||
                !ReadFile(file, &pcm, sizeof(pcm), &read, nullptr) || read != sizeof(pcm) ||
                pcm.tag != 1 || pcm.channels != 2 || pcm.rate != 44100 ||
                pcm.bytes != 176400 || pcm.align != 4 || pcm.bits != 16) { valid = false; break; }
            format = true;
        } else if (chunk[0] == 0x61746164) {
            if (data || ULONGLONG(chunk[1]) != ULONGLONG(frames) * 2352) { valid = false; break; }
            data = true;
        }
        offset = end + (chunk[1] & 1);
    }
    CloseHandle(file);
    return valid && format && data;
}
