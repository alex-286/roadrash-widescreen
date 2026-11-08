# Road Rash Widescreen Patch

This repository builds a 32-bit x86 `ddraw.dll` proxy with Visual C++ 2005. It
forwards common DirectDraw entry points to the operating system's DirectDraw
DLL and includes an experimental, version-checked Road Rash widescreen hook.

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

## Experimental widescreen prototype

The proxy now has a version-checked runtime patch for the German Road Rash
Windows 95 executable (1.0A). It hooks the game's `IDirectDraw::SetDisplayMode`
call used when entering the high-resolution game renderer. On that call only,
it sets the real display mode to 1920x1080 at 8 bits per pixel, but tells the
game it is running at 856x480 (logical width 428). The gameplay flip-chain
primary surface is replaced by a plain 1920x1080 primary plus an 856x480
offscreen "virtual" back buffer, and each `Flip` is turned into a stretched
`Blt` of that buffer onto the screen. The patch does not request modes above
1920x1080. If 1920x1080 is unavailable, it tries the previously tested 720x480
(3:2) mode, then retains the original 640x480 mode. It restores the original
game state when returning to the menu. The executable on disk is not changed.

The prototype requires the game's **High Resolution** option (640x480). Other
resolutions, executable builds, and display-mode calls are passed through
unchanged. Native mode support can vary by system; the current test system
enumerated 1920x1080x8, but Windows 98 still needs to be tested. The supported
executable is identified by its PE timestamp, image size, and instruction
signatures; unknown versions are left untouched.

This is the first rendering patch, not yet a visually verified release. It
expands the renderer's logical width and clipping region, but the game still
needs runtime testing to confirm that every road-projection and HUD path
responds correctly. The DLL writes diagnostic messages to
`RoadRashWidescreen.log` beside the game executable. It changes only the
existing `CreateSurface` and `SetDisplayMode` vtable slots and the surface
`Release`, `Flip`, and `GetAttachedSurface` slots; the first eight
`SetDisplayMode` calls log the caller address, requested mode, and HRESULT. The
surface replacement happens only at the verified gameplay primary-surface
`CreateSurface` callsite.
Remove an old log before a diagnostic run. After the first successful
display-mode change, it enumerates native DirectDraw modes and logs their
dimensions and bit depths, up to 64 entries. This probe does not change the
active display mode.

The proxy loads the operating system's DirectDraw DLL and does not currently
chain through DDrawCompat or dgVoodoo2. Modern-Windows translation-layer
compatibility and actual Windows 98 runtime behavior remain to be tested.
The same x86 DLL is intended for the game's 32-bit process; it is not a
64-bit DLL.

## Test installation

For a local test, copy `build\ddraw.dll` beside the supported `ROADRASH.EXE`.
Do not replace the Windows system copy of `ddraw.dll`. Keep the game's
**High Resolution** option enabled, then check `RoadRashWidescreen.log` beside
the executable for version-recognition, hook, and display-mode results.
Removing the local proxy restores the game's normal DirectDraw loading.
