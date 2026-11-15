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

set "HQX=%ROOT%\vendor\hqx"
if not exist "%HQX%\source\HQ2x.cc" (
    echo The hqx submodule is missing. Run:
    echo   git submodule update --init
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

cl.exe /nologo /Zl /GS- /O2 /W3 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /Fo"%BUILD%\xbr.obj" /c "%ROOT%\src\xbr.cpp"
if errorlevel 1 (
    echo xBR compilation failed.
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

rem The hqx sources are compiled unchanged. src\hqx_compat supplies the
rem <stdint.h> that Visual C++ 2005 lacks and an empty <iostream>, and
rem src\hqx_upscaler.cpp replaces vendor\hqx\source\HQx.cc.
set "HQX_FLAGS=/nologo /Zl /GS- /GR- /O2 /W3 /wd4146 /DWIN32 /D_WINDOWS /D_USRDLL /D_MBCS /I"%ROOT%\src\hqx_compat" /I"%HQX%\include""

cl.exe %HQX_FLAGS% /Fo"%BUILD%\hqx_upscaler.obj" /c "%ROOT%\src\hqx_upscaler.cpp"
if errorlevel 1 (
    echo hqx upscaler compilation failed.
    exit /b 1
)

cl.exe %HQX_FLAGS% /Fo"%BUILD%\HQ2x.obj" /c /Tp"%HQX%\source\HQ2x.cc"
if errorlevel 1 (
    echo hq2x compilation failed.
    exit /b 1
)

cl.exe %HQX_FLAGS% /Fo"%BUILD%\HQ3x.obj" /c /Tp"%HQX%\source\HQ3x.cc"
if errorlevel 1 (
    echo hq3x compilation failed.
    exit /b 1
)

link.exe /nologo /NODEFAULTLIB /DLL /MACHINE:I386 /SUBSYSTEM:WINDOWS,4.0 /ENTRY:DllMain@12 /DEF:"%ROOT%\src\ddraw.def" /OUT:"%BUILD%\ddraw.dll" "%BUILD%\ddraw_proxy.obj" "%BUILD%\road_rash_patch.obj" "%BUILD%\fps_overlay.obj" "%BUILD%\worker_pool.obj" "%BUILD%\sharp_bilinear.obj" "%BUILD%\resampler.obj" "%BUILD%\xbr.obj" "%BUILD%\scanlines.obj" "%BUILD%\screenshot.obj" "%BUILD%\hqx_upscaler.obj" "%BUILD%\HQ2x.obj" "%BUILD%\HQ3x.obj" kernel32.lib user32.lib
if errorlevel 1 (
    echo DLL linking failed.
    exit /b 1
)

echo Created: %BUILD%\ddraw.dll

rem Assemble the release package: the DLL plus the files from dist\ and the
rem license texts, renamed to .txt so they open in Notepad.
set "RELEASE=%BUILD%\release"
if exist "%RELEASE%\." rmdir /s /q "%RELEASE%"
mkdir "%RELEASE%"
if errorlevel 1 (
    echo Could not create the release directory: %RELEASE%
    exit /b 1
)

copy /y "%BUILD%\ddraw.dll" "%RELEASE%\ddraw.dll" >nul && ^
copy /y "%ROOT%\dist\RoadRashWidescreen.ini" "%RELEASE%\RoadRashWidescreen.ini" >nul && ^
copy /y "%ROOT%\dist\README.txt" "%RELEASE%\README.txt" >nul && ^
copy /y "%ROOT%\LICENSE" "%RELEASE%\LICENSE.txt" >nul && ^
copy /y "%ROOT%\THIRD_PARTY_NOTICES.md" "%RELEASE%\THIRD_PARTY_NOTICES.txt" >nul
if errorlevel 1 (
    echo Could not assemble the release package.
    exit /b 1
)

echo Created: %RELEASE%
endlocal & exit /b 0
