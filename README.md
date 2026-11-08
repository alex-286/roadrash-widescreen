# Road Rash Widescreen Patch

This repository starts with an x86 `ddraw.dll` proxy built by the Visual C++
2005 compiler. The first milestone is a DLL that exports common DirectDraw
entry points and forwards them to the operating system's DirectDraw DLL.

## Build

Open this folder in VS Code and run **Terminal > Run Build Task** (Ctrl+Shift+B),
or run `build.cmd` from a command prompt.

The Microsoft Platform SDK is also required for Windows headers and import
libraries. By default, the script looks for it at
`C:\Program Files\Microsoft Platform SDK`.

The script searches for Visual Studio 2005 in both `Program Files` locations.
If it is installed elsewhere, set `VS80_ROOT` to the Visual Studio 2005
installation directory before building. Set `PLATFORM_SDK_ROOT` if the Platform
SDK is installed somewhere else. The output is:

```text
build\ddraw.dll
```

The build targets 32-bit x86, omits the C/C++ runtime, and sets the Windows
subsystem version to 4.0. It uses only legacy Windows APIs so the proxy can be
tested on Windows 98 as well as in 32-bit game processes on newer Windows
versions.

## Current scope

This is a forwarding proxy, not yet a widescreen patch. It does not change
display modes, surface sizes, software-renderer pitch, projection, or HUD
layout. Intercepting `SetDisplayMode` alone cannot make a software renderer
produce a correct Hor+ image. The earlier example offset `0x000A1234` was only
a placeholder and must not be used.

The proxy also does not currently chain through DDrawCompat or dgVoodoo2.
Modern-Windows rendering compatibility therefore still needs separate testing.
The same x86 DLL is intended for the game's 32-bit process; it is not a 64-bit
DLL.

Initial inspection of the German PC release confirmed that `ROADRASH.EXE` is a
32-bit x86 executable importing `DDRAW.dll!DirectDrawCreate` by name. Its
initialization requests 640x480 DirectDraw modes at 8 and 16 bits per pixel.
That confirms the proxy exports the game's entry point, but is not enough to
implement Hor+: the render-surface allocation, software-renderer pitch,
projection, and HUD behavior still need to be identified and tested. These
mode requests are observations, not patch offsets. No game executable is
included or modified by this starter project.
