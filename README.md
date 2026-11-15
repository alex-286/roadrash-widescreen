# Road Rash Widescreen Patch

A widescreen patch for the Windows 95 version of **Road Rash**. It runs the
game at your monitor's native resolution with a view that matches its aspect
ratio, so you see more of the road instead of a stretched 4:3 picture.
Optional filters smooth the classic 640x480 graphics on modern displays.

The patch is a drop-in `ddraw.dll` placed next to the game. It changes nothing
on disk: delete the file and the game is back to normal.

<!-- Screenshots will be added here. -->

## Features

- **Native resolution, true widescreen:** the game renders a 480-line view as
  wide as your display's aspect ratio (856x480 for 16:9, 1120x480 for 21:9),
  which is scaled to the full screen. Uses the desktop resolution by default.
- **Fixed HUD and sky:** the cockpit, gauges and messages stay centered, and
  the sky panorama fills the whole width.
- **Display filters:** optional `nearest`, `hqx`, `xbr`, `sharp-bilinear` and
  `bicubic` filters, switchable during a race with **F11**.
- **Scanlines:** an optional CRT-style scanline effect on top of any filter.
- **No tearing:** waits for the vertical blank before each frame (can be
  turned off).
- **FPS and filter display:** optional on-screen frame rate and filter name.
- **Screenshots:** **F12** saves exactly what is on screen, at full
  resolution with the filter applied.
- **Multi-core:** filters spread their work across up to 8 CPU cores.
- **Safe:** only the supported game version is patched; anything else is left
  untouched. Menus run exactly as before.
- **Lightweight and retro-friendly:** a small DLL without runtime
  dependencies, built for Windows 98 up to Windows 11.

## Requirements

- **Road Rash for Windows 95, German version 1.0A.** The patch recognizes the
  executable by its build signature; other language versions or releases are
  not patched (the game then runs as usual). Support for more versions may
  follow.
- The game option **High Resolution** (640x480) must be enabled.
- Windows 98 or newer. The patch is tested on Windows 11; Windows 98 is a
  target but not tested yet.
- The display filters need a 32-bit display mode at your resolution and
  benefit from a multi-core CPU. Without filters, the game's original 8-bit
  display mode is used.

## Installation

1. Download a release (or [build](#building) the patch yourself).
2. Copy `ddraw.dll` and `RoadRashWidescreen.ini` into the game folder, next
   to `ROADRASH.EXE`. Never replace the `ddraw.dll` in the Windows system
   folder.
3. Optional: adjust `RoadRashWidescreen.ini` (see
   [Configuration](#configuration)).
4. Start the game and make sure **High Resolution** is enabled.

To uninstall, delete `ddraw.dll` (and the ini, log and screenshot files the
patch created) from the game folder.

## Configuration

All settings are optional. `RoadRashWidescreen.ini` next to `ROADRASH.EXE`
holds them; the release includes it with every setting explained (source:
[`dist/RoadRashWidescreen.ini`](dist/RoadRashWidescreen.ini)). Missing or
invalid values fall back to their defaults.

```ini
[Display]
DisplayWidth=0   ; 0 (default) = desktop width, or a width in pixels
DisplayHeight=0  ; 0 (default) = desktop height, or a height in pixels
VirtualWidth=0   ; 0 (default) = fit the display's aspect ratio, or 640-1280
WaitForVBlank=1  ; 1 (default) = wait for vertical blank, 0 = no wait
Filter=none      ; none (default), nearest, hqx, xbr, sharp-bilinear, bicubic
HqxScale=0       ; hqx/xbr factor: 0 (default) = auto (2 up to 960 lines, else 3), 2, or 3
Scanlines=0      ; 0 (default) = off, or 1-100 = scanline darkening in percent
ShowFps=0        ; 0 (default) = off, 1 = show the frame rate during races
ShowFilter=0     ; 0 (default) = off, 1 = show the active filter's name
```

| Setting | Description |
|---|---|
| `DisplayWidth`, `DisplayHeight` | The screen resolution used during races. If it is not available, 1920x1080 is tried, then 720x480, then the game's original 640x480. |
| `VirtualWidth` | The width the game renders at (multiple of 8). By default it matches your display's aspect ratio; other values change the aspect ratio. |
| `WaitForVBlank` | Prevents tearing, but limits the frame rate to the monitor's refresh rate. |
| `Filter` | The display filter, see [Filters](#filters). `none` keeps the game's original 8-bit display mode. |
| `HqxScale` | Scale factor of the pixel-art filters `hqx` and `xbr`; their result is then scaled smoothly to the screen. The automatic choice is the smallest factor that reaches the screen height, so at 1080p the 3x result is reduced (supersampling). A lower factor is faster. |
| `Scanlines` | Strength of the scanline effect; 30 to 50 is a good start. |
| `ShowFps`, `ShowFilter` | On-screen display in the top-left corner, for example `60 FPS  BICUBIC`. |

## Filters

The game draws at 480 lines, so the picture is always enlarged. A filter
decides how:

| Filter | Look | Best for |
|---|---|---|
| `none` | Original look, the game's 8-bit display mode. | Maximum compatibility, slow PCs. |
| `nearest` | Sharp, blocky pixels. | The authentic pixel look. |
| `hqx` | Smooths edges between flat colors (hq2x/hq3x). | Pixel-art style. |
| `xbr` | Like hqx, with rounder outlines and cleaner diagonals. | Pixel-art style. |
| `sharp-bilinear` | Crisp pixels of even size, also at non-integer scales such as 1080p. | A clean pixel look on any resolution. |
| `bicubic` | Smooth, photo-like enlargement; also softens the game's blocky near sprites. | A soft, modern look. |

With any filter except `none`, press **F11** during a race to cycle through
`nearest`, `hqx`, `xbr`, `sharp-bilinear` and `bicubic`; the choice is kept
for later races. Switching to or from `none` requires restarting the game.
`Scanlines` apply to every filter except `nearest`.

`hqx` and `xbr` enlarge by a whole factor (2x or 3x); their result is then
scaled to the exact screen size with area averaging. That avoids doubled
rows and columns at non-integer sizes such as 1080p or 4K, and at 1080p the
3x result is reduced, which gives a supersampled, particularly clean picture.

On a modern CPU every filter takes only a few milliseconds per frame. On a
single-core CPU such as a Pentium 4, `sharp-bilinear` is the fastest choice;
`bicubic` and `xbr` are only practical at low resolutions there.

## Hotkeys

| Key | Action |
|---|---|
| **F11** | Cycle through the display filters (during a race, with a filter enabled). |
| **F12** | Save a screenshot of the screen as `RoadRashScreen001.bmp`, `002`, ... in the game folder. It shows exactly what is displayed, including filter, scanlines and on-screen display. The game's unscaled 856x480 frame is also saved as `RoadRashFrame.bmp` (replaced each time). |

## Troubleshooting

The patch writes `RoadRashWidescreen.log` to the game folder each time the
game starts. It shows whether the game version was recognized, which display
mode and filter are used, and, per filter, how the time of a frame is spent.
Please attach it when reporting a problem.

- **Nothing changes:** make sure **High Resolution** is enabled and check the
  log for the recognized game version.
- **No filter, although one is set:** the 32-bit display mode is not available
  at your resolution; the log says so and the game runs in 8-bit mode.
- **Low frame rate with a filter:** try `sharp-bilinear`, a lower `HqxScale`,
  or `Filter=none`.

## Building

Requirements:

- Microsoft Visual Studio 2005 (Visual C++ 8)
- Microsoft Platform SDK (expected at `C:\Program Files\Microsoft Platform SDK`)
- Git, to fetch the [hqx](https://github.com/brunexgeek/hqx) submodule

```bat
git clone --recursive <repository-url>
cd roadrash-widescreen
build.cmd
```

In an existing clone, run `git submodule update --init` first. In VS Code,
**Terminal > Run Build Task** (Ctrl+Shift+B) runs the same script. If Visual
Studio 2005 or the Platform SDK is installed elsewhere, set `VS80_ROOT` or
`PLATFORM_SDK_ROOT` to its folder before building.

The result is `build\ddraw.dll`: a 32-bit x86 DLL without C runtime
dependencies, linked for Windows subsystem 4.0 so that it also loads on
Windows 98.

The script also assembles the release package in `build\release\`: the DLL,
the player documentation and default settings from [`dist/`](dist/)
(`README.txt`, `RoadRashWidescreen.ini`), and the license texts as
`LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`. Zip that folder for a release.

## How it works

- The proxy forwards all DirectDraw calls to the system's `ddraw.dll` and
  patches only the supported Road Rash executable, in memory.
- When a race starts, the display is set to the native resolution while the
  game renders into a wider 480-line buffer, which is scaled to the screen on
  every frame. Two game routines are redirected while racing: the HUD is
  drawn centered, and the sky panorama is tiled across the whole width. Both
  are restored when returning to the menu.
- With a filter, the display runs at 32 bits per pixel. The game keeps
  rendering 8-bit frames, which are converted through the game palette,
  filtered and then presented.

## Known limitations

- Only the German version 1.0A is supported so far.
- Windows 98 is not tested yet.
- With `WaitForVBlank=0`, tearing is possible.
- The 720x480 fallback mode uses no filter and shows no on-screen display.
- The patch does not chain through other DirectDraw wrappers such as
  DDrawCompat or dgVoodoo2.

## License

Copyright (C) 2026 Alexander Schmidt

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See `LICENSE` for the full text.

The [hqx](https://github.com/brunexgeek/hqx) library by Bruno Ribeiro is
licensed under the Apache License 2.0 and compiled into `ddraw.dll`; the xBR
filter implements an algorithm by Hyllian. See `THIRD_PARTY_NOTICES.md`,
which must accompany any distribution of `ddraw.dll`.

Road Rash is a trademark of Electronic Arts. This project is not affiliated
with or endorsed by Electronic Arts, contains no game files, and requires an
original copy of the game.
