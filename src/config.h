#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum WindowMode {
    MODE_WINDOWED   = 0,
    MODE_BORDERLESS = 1,
    MODE_FULLSCREEN = 2
};

struct KeyRemap { DWORD src; DWORD dst; };

struct Config {
    int        width;
    int        height;
    WindowMode mode;
    bool       stretchToFit;      // true = stretch (ignore aspect), false = letterbox
    bool       keepAspect;        // only used when stretchToFit = false
    bool       pauseOnFocusLoss;  // true = suspend game loop when alt-tabbed
    bool       muteOnFocusLoss;   // true = mute audio when alt-tabbed
    KeyRemap   remaps[32];
    int        remapCount;
};

// Fills cfg from TOTDPatch.ini next to the DLL.
// Falls back to sensible defaults if the file is missing or a key is absent.
void Config_Load(Config& cfg, HMODULE hModule);
