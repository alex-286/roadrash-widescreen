@echo off
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
    echo C++ compilation failed.
    exit /b 1
)

link.exe /nologo /NODEFAULTLIB /DLL /MACHINE:I386 /SUBSYSTEM:WINDOWS,4.0 /ENTRY:DllMain@12 /DEF:"%ROOT%\src\ddraw.def" /OUT:"%BUILD%\ddraw.dll" "%BUILD%\ddraw_proxy.obj" kernel32.lib
if errorlevel 1 (
    echo DLL linking failed.
    exit /b 1
)

echo Created: %BUILD%\ddraw.dll
endlocal & exit /b 0
