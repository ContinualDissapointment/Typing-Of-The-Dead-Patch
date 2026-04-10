#include "config.h"
#include <cstring>
#include <cctype>
#include <cstdlib>

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

// ---------------------------------------------------------------------------
// Key name → virtual key code
// ---------------------------------------------------------------------------

static DWORD ParseKeyName(const char* s)
{
    // Skip leading whitespace
    while (*s == ' ') ++s;

    // Hex literal: 0x00 – 0xFF
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (DWORD)strtoul(s, nullptr, 16);

    // Single letter A–Z or digit 0–9 (VK codes equal their ASCII values)
    if (s[1] == '\0')
    {
        char c = (char)toupper((unsigned char)s[0]);
        if (c >= 'A' && c <= 'Z') return (DWORD)c;
        if (c >= '0' && c <= '9') return (DWORD)c;
    }

    // Named keys (case-insensitive)
    static const struct { const char* name; DWORD vk; } k[] = {
        {"ENTER",     VK_RETURN},  {"RETURN",   VK_RETURN},
        {"ESCAPE",    VK_ESCAPE},  {"ESC",      VK_ESCAPE},
        {"SPACE",     VK_SPACE},   {"TAB",      VK_TAB},
        {"BACK",      VK_BACK},    {"BACKSPACE",VK_BACK},
        {"DELETE",    VK_DELETE},  {"DEL",      VK_DELETE},
        {"INSERT",    VK_INSERT},
        {"HOME",      VK_HOME},    {"END",      VK_END},
        {"PAGEUP",    VK_PRIOR},   {"PGUP",     VK_PRIOR},
        {"PAGEDOWN",  VK_NEXT},    {"PGDN",     VK_NEXT},
        {"PAUSE",     VK_PAUSE},
        {"LEFT",      VK_LEFT},    {"RIGHT",    VK_RIGHT},
        {"UP",        VK_UP},      {"DOWN",     VK_DOWN},
        {"F1",  VK_F1},  {"F2",  VK_F2},  {"F3",  VK_F3},  {"F4",  VK_F4},
        {"F5",  VK_F5},  {"F6",  VK_F6},  {"F7",  VK_F7},  {"F8",  VK_F8},
        {"F9",  VK_F9},  {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1},
        {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
        {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5},
        {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7},
        {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9},
        {nullptr, 0}
    };
    for (int i = 0; k[i].name; ++i)
        if (_stricmp(s, k[i].name) == 0)
            return k[i].vk;

    return 0;
}

static void LoadControls(const char* ini, Config& cfg)
{
    cfg.remapCount = 0;

    char buf[4096];
    if (!GetPrivateProfileSectionA("Controls", buf, sizeof(buf), ini))
        return;

    for (const char* p = buf; *p && cfg.remapCount < 32; p += strlen(p) + 1)
    {
        char line[256];
        strncpy_s(line, p, _TRUNCATE);

        // Strip inline comments
        char* semi = strchr(line, ';');
        if (semi) *semi = '\0';

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        // Trim trailing whitespace from key name
        char* key = line;
        char* val = eq + 1;
        for (char* t = key + strlen(key) - 1; t >= key && *t == ' '; --t) *t = '\0';
        while (*val == ' ') ++val;
        for (char* t = val + strlen(val) - 1; t >= val && *t == ' '; --t) *t = '\0';

        DWORD src = ParseKeyName(key);
        DWORD dst = ParseKeyName(val);
        if (src && dst && src != dst)
        {
            cfg.remaps[cfg.remapCount++] = { src, dst };
        }
    }
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
        cfg.mode = MODE_BORDERLESS;

    LoadControls(ini, cfg);
}
