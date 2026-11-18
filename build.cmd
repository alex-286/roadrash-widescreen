@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (C) 2026 Alexander Schmidt
setlocal

set "ROOT=%~dp0"
for %%I in ("%ROOT%.") do set "ROOT=%%~fI"
set "BUILD=%ROOT%\build"
set "VCVARS="
set "PLATFORM_SDK_ROOT="

if defined VS80_ROOT set "VCVARS=%VS80_ROOT%\VC\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio 8\VC\vcvarsall.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio 8\VC\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio 8\VC\vcvarsall.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio 8\VC\vcvarsall.bat"

if not exist "%VCVARS%" (
    echo Visual Studio 2005 was not found.
    echo Set VS80_ROOT to its installation directory, for example:
    echo   set VS80_ROOT=C:\Program Files\Microsoft Visual Studio 8
    exit /b 1
)

if defined PLATFORM_SDK_ROOT goto sdk_root_configured
if exist "C:\Program Files\Microsoft Platform SDK\Include\Windows.h" set "PLATFORM_SDK_ROOT=C:\Program Files\Microsoft Platform SDK"
if not defined PLATFORM_SDK_ROOT (
    echo Microsoft Platform SDK headers were not found.
    echo Set PLATFORM_SDK_ROOT to the SDK installation directory.
    exit /b 1
)

:sdk_root_configured
if not exist "%PLATFORM_SDK_ROOT%\Include\Windows.h" (
    echo Windows.h was not found under: %PLATFORM_SDK_ROOT%\Include
    exit /b 1
)

if not exist "%BUILD%\." mkdir "%BUILD%"
if errorlevel 1 (
    echo Could not create the build directory: %BUILD%
    exit /b 1
)

if exist "%BUILD%\ddraw.dll" del /q "%BUILD%\ddraw.dll"

call "%VCVARS%" x86
if errorlevel 1 (
    echo Could not initialize the Visual C++ 2005 x86 environment.
    exit /b 1
)

set "INCLUDE=%PLATFORM_SDK_ROOT%\Include;%PLATFORM_SDK_ROOT%\Include\crt;%INCLUDE%"
set "LIB=%PLATFORM_SDK_ROOT%\Lib;%LIB%"

cl.exe /nologo /Zl /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\ddraw_proxy.obj" /c "%ROOT%\src\ddraw_proxy.cpp"
if errorlevel 1 (
    echo DirectDraw proxy compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\road_rash_patch.obj" /c "%ROOT%\src\road_rash_patch.cpp"
if errorlevel 1 (
    echo Road Rash patch compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\game_fixes.obj" /c "%ROOT%\src\game_fixes.cpp"
if errorlevel 1 (
    echo Game fixes compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\distance_fade.obj" /c "%ROOT%\src\distance_fade.cpp"
if errorlevel 1 (
    echo Distance fade compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\native_menus.obj" /c "%ROOT%\src\native_menus.cpp"
if errorlevel 1 (
    echo Native menus compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\draw_distance.obj" /c "%ROOT%\src\draw_distance.cpp"
if errorlevel 1 (
    echo Draw distance compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\nearest.obj" /c "%ROOT%\src\nearest.cpp"
if errorlevel 1 (
    echo Nearest filter compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\cpu_features.obj" /c "%ROOT%\src\cpu_features.cpp"
if errorlevel 1 (
    echo CPU feature detection compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\render_scale.obj" /c "%ROOT%\src\render_scale.cpp"
if errorlevel 1 (
    echo Render scale compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\worker_pool.obj" /c "%ROOT%\src\worker_pool.cpp"
if errorlevel 1 (
    echo Worker pool compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\screenshot.obj" /c "%ROOT%\src\screenshot.cpp"
if errorlevel 1 (
    echo Screenshot compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\scanlines.obj" /c "%ROOT%\src\scanlines.cpp"
if errorlevel 1 (
    echo Scanlines compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\resampler.obj" /c "%ROOT%\src\resampler.cpp"
if errorlevel 1 (
    echo Resampler compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\sharp_bilinear.obj" /c "%ROOT%\src\sharp_bilinear.cpp"
if errorlevel 1 (
    echo Sharp bilinear compilation failed.
    exit /b 1
)

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\fps_overlay.obj" /c "%ROOT%\src\fps_overlay.cpp"
if errorlevel 1 (
    echo FPS overlay compilation failed.
    exit /b 1
)

rc.exe /fo"%BUILD%\ddraw.res" "%ROOT%\src\ddraw.rc"
if errorlevel 1 (
    echo Version resource compilation failed.
    exit /b 1
)

link.exe /nologo /NODEFAULTLIB /DLL /MACHINE:I386 /SUBSYSTEM:WINDOWS,4.0 /ENTRY:DllMain@12 /DEF:"%ROOT%\src\ddraw.def" /OUT:"%BUILD%\ddraw.dll" "%BUILD%\ddraw_proxy.obj" "%BUILD%\road_rash_patch.obj" "%BUILD%\game_fixes.obj" "%BUILD%\render_scale.obj" "%BUILD%\cpu_features.obj" "%BUILD%\nearest.obj" "%BUILD%\draw_distance.obj" "%BUILD%\distance_fade.obj" "%BUILD%\native_menus.obj" "%BUILD%\fps_overlay.obj" "%BUILD%\worker_pool.obj" "%BUILD%\sharp_bilinear.obj" "%BUILD%\resampler.obj" "%BUILD%\scanlines.obj" "%BUILD%\screenshot.obj" "%BUILD%\ddraw.res" kernel32.lib user32.lib gdi32.lib
if errorlevel 1 (
    echo DLL linking failed.
    exit /b 1
)

echo Created: %BUILD%\ddraw.dll

rem RRWSPatch.exe points ROADRASH.EXE at RRWS.dll (the same DLL under another
rem name), because Windows 9x keeps using the system ddraw.dll otherwise.
if exist "%BUILD%\RRWSPatch.exe" del /q "%BUILD%\RRWSPatch.exe"
cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_MBCS /Fo"%BUILD%\win9x_patcher.obj" /c "%ROOT%\src\win9x_patcher.cpp"
if errorlevel 1 (
    echo Windows 9x patcher compilation failed.
    exit /b 1
)

rc.exe /fo"%BUILD%\win9x_patcher.res" "%ROOT%\src\win9x_patcher.rc"
if errorlevel 1 (
    echo Windows 9x patcher resource compilation failed.
    exit /b 1
)

link.exe /nologo /NODEFAULTLIB /MACHINE:I386 /SUBSYSTEM:WINDOWS,4.0 /ENTRY:PatcherMain@0 /OUT:"%BUILD%\RRWSPatch.exe" "%BUILD%\win9x_patcher.obj" "%BUILD%\win9x_patcher.res" kernel32.lib user32.lib
if errorlevel 1 (
    echo Windows 9x patcher linking failed.
    exit /b 1
)

echo Created: %BUILD%\RRWSPatch.exe

rem Assemble the release package: the DLL plus the files from dist\ and the
rem license text, renamed to .txt so it opens in Notepad.
set "RELEASE=%BUILD%\release"
if exist "%RELEASE%\." rmdir /s /q "%RELEASE%"
mkdir "%RELEASE%"
if errorlevel 1 (
    echo Could not create the release directory: %RELEASE%
    exit /b 1
)

copy /y "%BUILD%\ddraw.dll" "%RELEASE%\ddraw.dll" >nul && ^
copy /y "%BUILD%\ddraw.dll" "%RELEASE%\RRWS.dll" >nul && ^
copy /y "%BUILD%\RRWSPatch.exe" "%RELEASE%\RRWSPatch.exe" >nul && ^
copy /y "%ROOT%\dist\RoadRashWidescreen.ini" "%RELEASE%\RoadRashWidescreen.ini" >nul && ^
copy /y "%ROOT%\dist\README.txt" "%RELEASE%\README.txt" >nul && ^
copy /y "%ROOT%\LICENSE" "%RELEASE%\LICENSE.txt" >nul
if errorlevel 1 (
    echo Could not assemble the release package.
    exit /b 1
)

echo Created: %RELEASE%
endlocal & exit /b 0
