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
it sets the real display mode (the desktop resolution by default, see the ini
settings) at 8 bits per pixel, but tells the
game it is running at a 480-line virtual view whose width fits the display's
aspect ratio (856x480 for 16:9, logical width 428). The gameplay flip-chain
primary surface is replaced by a plain primary of the real size plus an
offscreen "virtual" back buffer, and each `Flip` is turned into a stretched
`Blt` of that buffer onto the screen. If the chosen mode is unavailable, it
tries 1920x1080 and then the previously tested 720x480
(3:2) mode, then retains the original 640x480 mode. It restores the original
game state when returning to the menu. The executable on disk is not changed.

The prototype requires the game's **High Resolution** option (640x480). Other
resolutions, executable builds, and display-mode calls are passed through
unchanged. Native mode support can vary by system; the current test system
enumerated 1920x1080x8, but Windows 98 still needs to be tested. The supported
executable is identified by its PE timestamp, image size, and instruction
signatures; unknown versions are left untouched.

Gameplay is verified at 1920x1080 on a modern Windows system. Two game routines
had to be patched for the wider view, both only while the virtual view is
active and both undone when returning to the menu:

- **HUD centering:** the cockpit, gauges, needles and text are positioned
  relative to a horizontal origin value. The call to the HUD routine is
  redirected so the origin is 108 px while it runs, which centers the
  640-pixel-wide HUD. A conditional jump that narrows the message rectangle to
  640 px is made unconditional.
- **Sky panorama:** the game draws the sky from two 360-pixel halves in three
  pieces, which leaves gaps (stale or black pixels) beyond 720 px. The call is
  redirected to a routine that tiles the panorama across the whole view.

Both patch sites are byte-checked before the executable is accepted.

On modern Windows something replaces the surface vtable's `Flip` slot after the
hooks are installed. The `IsLost` hook, which the game calls right before every
flip, therefore re-installs the `Flip` hook when needed.

### Diagnostics and settings

- `RoadRashWidescreen.log` beside the game executable records hook and
  display-mode results. It is replaced each time the game starts. After the
  first successful display-mode change it enumerates native DirectDraw modes
  (up to 64 entries) without changing the active mode.
- **F12** during gameplay saves the 8-bit virtual frame, before it is stretched,
  as `RoadRashFrame.bmp` beside the executable. It uses only legacy APIs.
- `RoadRashWidescreen.ini` beside the executable is optional:

  ```ini
  [Display]
  DisplayWidth=0
  DisplayHeight=0
  VirtualWidth=0
  WaitForVBlank=1
  ```

  `DisplayWidth`/`DisplayHeight` select the real 8-bit display mode. 0 (the
  default) uses the desktop resolution, e.g. 2560x1440. If that mode is
  rejected, 1920x1080 is tried, then the 720x480 fallback. `VirtualWidth` is
  the game's render width (multiple of 8, 640-1280); 0 (the default) picks the
  width that best fits the display's aspect ratio at 480 lines (856 for 16:9,
  1120 for 21:9). Setting it manually changes the aspect ratio. Invalid values
  are ignored. `WaitForVBlank` (default
  1) makes each frame wait for the vertical blank before it is presented, which
  prevents tearing but caps the frame rate at the monitor's refresh rate. Set it
  to 0 to disable the wait. An unsupported wait is ignored rather than blocking.

Known limitations: with `WaitForVBlank=0`, tearing is possible. Widths above
856 and displays above 1920x1080 are untested, and so is Windows 98.

The proxy loads the operating system's DirectDraw DLL and does not currently
chain through DDrawCompat or dgVoodoo2. Modern-Windows translation-layer
compatibility and actual Windows 98 runtime behavior remain to be tested.
The same x86 DLL is intended for the game's 32-bit process; it is not a
64-bit DLL.

## Test installation

For a local test, copy `build\ddraw.dll` beside the supported `ROADRASH.EXE`.
Do not replace the Windows system copy of `ddraw.dll`. Keep the game's
**High Resolution** option enabled, then check `RoadRashWidescreen.log` beside
the executable for version-recognition, hook, and display-mode results. Press
F12 during a race to save `RoadRashFrame.bmp`.
Removing the local proxy restores the game's normal DirectDraw loading.
