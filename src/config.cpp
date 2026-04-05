#include "config.h"
#include <cstring>

static void GetIniPath(char* out, size_t outLen, HMODULE hModule)
{
    GetModuleFileNameA(hModule, out, (DWORD)outLen);
    // Replace filename with TOTDPatch.ini in the same directory
    char* slash = strrchr(out, '\\');
    if (slash)
        strcpy_s(slash + 1, outLen - (slash - out + 1), "TOTDPatch.ini");
    else
        strcpy_s(out, outLen, "TOTDPatch.ini");
}

static int ReadInt(const char* ini, const char* section, const char* key, int fallback)
{
    return (int)GetPrivateProfileIntA(section, key, fallback, ini);
}

static void ReadStr(const char* ini, const char* section, const char* key,
                    const char* fallback, char* out, DWORD outLen)
{
    GetPrivateProfileStringA(section, key, fallback, out, outLen, ini);
}

void Config_Load(Config& cfg, HMODULE hModule)
{
    char ini[MAX_PATH];
    GetIniPath(ini, sizeof(ini), hModule);

    cfg.width            = ReadInt(ini, "Display", "Width",  1920);
    cfg.height           = ReadInt(ini, "Display", "Height", 1080);
    cfg.stretchToFit     = ReadInt(ini, "Display", "StretchToFit",     0) != 0;
    cfg.keepAspect       = ReadInt(ini, "Display", "KeepAspect",       1) != 0;
    cfg.pauseOnFocusLoss = ReadInt(ini, "Display", "PauseOnFocusLoss", 0) != 0;
    cfg.muteOnFocusLoss  = ReadInt(ini, "Display", "MuteOnFocusLoss",  0) != 0;

    char mode[32];
    ReadStr(ini, "Display", "Mode", "borderless", mode, sizeof(mode));

    if (_stricmp(mode, "windowed") == 0)
        cfg.mode = MODE_WINDOWED;
    else if (_stricmp(mode, "fullscreen") == 0)
        cfg.mode = MODE_FULLSCREEN;
    else
        cfg.mode = MODE_BORDERLESS;  // default
}
