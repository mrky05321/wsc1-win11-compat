#pragma once
// Validate the CD timeline before it is consumed by MCI track arithmetic.
// The caller owns start[0..capacity-1], with one-based CD track indices.
#include <windows.h>

inline bool ValidCdManifest(unsigned count, DWORD discEnd, DWORD audioStart,
                            const DWORD* start, unsigned capacity) {
    if (!start || count < 2 || count >= capacity || audioStart >= discEnd)
        return false;
    if (start[1] > audioStart || start[2] < audioStart)
        return false; // The audio WAV may include the track-2 pregap.
    for (unsigned i = 1; i <= count; ++i) {
        if (start[i] >= discEnd || (i > 1 && start[i] <= start[i - 1]))
            return false;
    }
    return true;
}
