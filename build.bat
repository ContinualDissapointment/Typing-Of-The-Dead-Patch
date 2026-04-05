@echo off
setlocal

:: ---------------------------------------------------------------------------
:: TOTD Patch build script
:: Produces ddraw.dll (x86) using VS2022 Preview MSVC
:: ---------------------------------------------------------------------------

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvarsall.bat"
if not exist %VCVARS% (
    echo ERROR: Could not find vcvarsall.bat at %VCVARS%
    exit /b 1
)

:: Set up the x86 build environment
call %VCVARS% x86

if not exist obj mkdir obj

set SRCS=src\dllmain.cpp src\config.cpp src\ddraw_proxy.cpp src\surface_proxy.cpp

set CFLAGS=/nologo /O2 /MT /W3 /EHsc /DWIN32 /D_WINDOWS /DNDEBUG /DDIRECTDRAW_VERSION=0x0700 /I"src"

set LFLAGS=/nologo /DLL /SUBSYSTEM:WINDOWS /DEF:src\ddraw.def /OUT:ddraw.dll /IMPLIB:obj\ddraw_proxy.lib /MACHINE:X86

echo Compiling...
cl.exe %CFLAGS% /Foobj\ /c %SRCS%
if errorlevel 1 goto :error

echo Linking...
link.exe %LFLAGS% obj\dllmain.obj obj\config.obj obj\ddraw_proxy.obj obj\surface_proxy.obj ^
    ddraw.lib dxguid.lib kernel32.lib user32.lib gdi32.lib ole32.lib
if errorlevel 1 goto :error

echo.
echo Build successful: ddraw.dll
echo Copy ddraw.dll and TOTDPatch.ini to the game folder alongside Tod_e.exe
goto :eof

:error
echo.
echo *** BUILD FAILED ***
exit /b 1
