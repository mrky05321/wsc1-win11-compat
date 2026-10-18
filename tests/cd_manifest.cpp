#include <windows.h>
#include <cstdio>
#include "../src/cd_manifest_validation.h"

static int failures;
static void Check(bool value, const char* label) {
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}

int main() {
    DWORD tracks[100] = {};
    tracks[1] = 150;
    tracks[2] = 1000;
    tracks[3] = 2000;
    Check(ValidCdManifest(3, 3000, 900, tracks, 100), "pregap inside WAV accepted");
    Check(ValidCdManifest(3, 3000, 1000, tracks, 100), "audio starts at track 2");
    Check(!ValidCdManifest(1, 3000, 900, tracks, 100), "reject missing audio tracks");
    Check(!ValidCdManifest(100, 3000, 900, tracks, 100), "reject oversized track list");
    Check(!ValidCdManifest(3, 2000, 900, tracks, 100), "reject last track at disc end");
    Check(!ValidCdManifest(3, 1999, 900, tracks, 100), "reject last track beyond disc end");
    Check(!ValidCdManifest(3, 3000, 3000, tracks, 100), "reject empty audio range");
    Check(!ValidCdManifest(3, 3000, 100, tracks, 100), "reject audio start before first track");
    Check(!ValidCdManifest(3, 3000, 1100, tracks, 100), "reject audio start after second track");
    tracks[3] = 1000;
    Check(!ValidCdManifest(3, 3000, 900, tracks, 100), "reject non-increasing tracks");
    tracks[3] = 2000;
    tracks[2] = 0;
    Check(!ValidCdManifest(3, 3000, 900, tracks, 100), "reject missing second track");
    std::printf("CD manifest validation failures: %d\n", failures);
    return failures ? 1 : 0;
}
