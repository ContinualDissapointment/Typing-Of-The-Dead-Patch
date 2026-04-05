#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

// Writes a timestamped line to TOTDPatch.log next to the DLL.
// Call Log_Init once (in DllMain), then use Log() freely.

static FILE* g_logFile = nullptr;

inline void Log_Init(HMODULE hModule)
{
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, sizeof(path));
    char* dot = strrchr(path, '.');
    if (dot) strcpy_s(dot, MAX_PATH - (dot - path), ".log");
    fopen_s(&g_logFile, path, "w");
    if (g_logFile) { setvbuf(g_logFile, nullptr, _IONBF, 0); } // unbuffered
}

inline void Log(const char* fmt, ...)
{
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fputc('\n', g_logFile);
}
