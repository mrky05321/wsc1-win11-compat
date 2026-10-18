#include <windows.h>
#include <cstdio>
#include <vector>
#include <cstring>
#include "../src/cd_manifest_validation.h"

static int failures;
static void Check(bool value, const char* label) {
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}

int wmain(int argc, WCHAR** argv) {
    if (argc == 3) {
        DWORD count = 0, end = 0, audio = 0, positions[100] = {};
        bool parsed = ReadManifestNumber(L"Disc",L"Tracks",argv[1],count) &&
            ReadManifestNumber(L"Disc",L"EndFrame",argv[1],end) &&
            ReadManifestNumber(L"Disc",L"AudioStartFrame",argv[1],audio);
        if (count < 2 || count >= ARRAYSIZE(positions)) parsed = false;
        if (parsed) for (unsigned i=1; i<=count; ++i) {
            WCHAR key[16]; swprintf_s(key,L"%u",i);
            if (!ReadManifestNumber(L"Tracks",key,argv[1],positions[i])) parsed = false;
        }
        Check(parsed && ValidCdManifest(count,end,audio,positions,ARRAYSIZE(positions)) &&
            ValidCdWave(argv[2],end-audio), "read-only validation of owner's extracted INI and WAV");
        return failures ? 1 : 0;
    }
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
    tracks[2] = 1000;
    Check(!ValidCdManifest(3, MAXDWORD, 900, tracks, 100), "reject unrepresentable MSF disc length");
    WCHAR temp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(ARRAYSIZE(temp), temp); GetTempFileNameW(temp,L"wsc",0,file);
    const WCHAR* bad[] = {L"-1", L"4294967296", L"99999999999999999999999999999999999999", L"12x", L""};
    DWORD value = 123;
    Check(!ReadManifestNumber(L"Disc",L"Missing",file,value), "reject missing number");
    for (auto text : bad) {
        WritePrivateProfileStringW(L"Disc",L"Tracks",text,file);
        Check(!ReadManifestNumber(L"Disc",L"Tracks",file,value), "reject malformed or overflowing INI number");
    }
    WritePrivateProfileStringW(L"Disc",L"Tracks",L"16",file);
    Check(ReadManifestNumber(L"Disc",L"Tracks",file,value) && value == 16, "accept extractor decimal format");
    DeleteFileW(file);
    // Exact extractor-style PCM, two CD frames (1176 stereo samples).
    std::vector<BYTE> wav(44 + 2 * 2352, 0);
    DWORD header[] = {0x46464952, DWORD(wav.size()-8), 0x45564157,
        0x20746d66, 16, 0x00020001, 44100, 176400, 0x00100004, 0x61746164, 2*2352};
    memcpy(wav.data(), header, sizeof(header));
    auto save = [&]() {
        HANDLE f = CreateFileW(file,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        DWORD written; WriteFile(f,wav.data(),DWORD(wav.size()),&written,nullptr); CloseHandle(f);
    };
    save();
    Check(ValidCdWave(file,2), "accept exact CD PCM sample count");
    Check(!ValidCdWave(file,3), "reject WAV length that differs from manifest");
    wav[24] = 0; save();
    Check(!ValidCdWave(file,2), "reject different sample rate");
    memcpy(wav.data(),header,sizeof(header)); wav.pop_back(); save();
    Check(!ValidCdWave(file,2), "reject truncated RIFF data");
    wav.resize(44+2*2352); memcpy(wav.data(),header,sizeof(header));
    DWORD oversized = MAXDWORD; memcpy(wav.data()+40,&oversized,4); save();
    Check(!ValidCdWave(file,2), "reject overflowing chunk boundary");
    DeleteFileW(file);
    std::printf("CD manifest validation failures: %d\n", failures);
    return failures ? 1 : 0;
}
