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
  DisplayWidth=0   ; 0 (default) = desktop width, or a width in pixels
  DisplayHeight=0  ; 0 (default) = desktop height, or a height in pixels
  VirtualWidth=0   ; 0 (default) = fit the display's aspect ratio, or 640-1280
  WaitForVBlank=1  ; 1 (default) = wait for vertical blank, 0 = no wait
  Filter=none      ; none (default) = old 8-bit path, or nearest, hqx, xbr, sharp-bilinear, bicubic
  HqxScale=0       ; hqx/xbr factor: 0 (default) = auto (2 below 1440 lines), 2, or 3
  ShowFps=0        ; 0 (default) = off, 1 = show the frame rate during races
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
  `Filter` selects a frame filter (see below). `HqxScale` selects the factor
  of the pixel-art filters hqx and xbr, 2 or 3; 0 (the default) picks the
  largest factor whose height fits the display: 2 below 1440 lines, 3 from
  1440 lines.
  `ShowFps=1` shows the number of presented frames per second, refreshed twice
  a second, as white digits on a black box in the top-left corner. In the
  8-bit path it is drawn into the virtual frame with the palette's brightest
  and darkest colors; with a filter it is drawn after filtering, so the filter
  does not blur it. F12 screenshots do not contain it. The 720x480 fallback
  does not show it.

### Frame filters

With any `Filter` other than `none`, the gameplay display mode is set to 32
bits per pixel instead of 8. The game still renders 8-bit palettized frames:
its back buffer and the other gameplay surfaces (sky halves, HUD bitmaps) are
created as 8-bit system-memory surfaces, and the game palette is attached to
the back buffer so the GDI-drawn HUD text keeps its colors. On every flip the
frame is converted through the game palette, filtered into a 32-bit staging
surface, and stretched to the display. Two staging surfaces take turns, so a
frame is never written into the surface the previous present may still be
reading.

| Filter           | Result                                                                |
|------------------|-----------------------------------------------------------------------|
| `none`           | The 8-bit display mode; the presenter below is not used.              |
| `nearest`        | Converted only, then stretched by DirectDraw without smoothing.       |
| `hqx`            | hq2x or hq3x (856x480 becomes 1712x960 or 2568x1440), then stretched. |
| `xbr`            | xBR 2x or 3x pixel-art scaling with cleaner diagonals, then stretched. |
| `sharp-bilinear` | Scaled to the display size with crisp, evenly sized pixels.           |
| `bicubic`        | Scaled to the display size smoothly (Catmull-Rom), like a photo.      |

The older `Hqx=1` setting still selects `hqx` when `Filter` is missing.

**F11** cycles through the 32-bit filters during a race (nearest, hqx, xbr,
sharp-bilinear, bicubic, back to nearest) and logs the new one. The selection
is kept for later races. The cycle stays within the 32-bit display mode: switching between the 8-bit and
32-bit modes in the middle of a race would invalidate all of the game's
surfaces, so with `Filter=none` F11 does nothing. A filter that cannot
allocate its buffers is skipped and the frame falls back to `nearest`.

Filters split their work into horizontal stripes, one per processor (up to 8
threads, `src/worker_pool.cpp`). On a single processor, which includes every
Windows 98 system, no thread is started and the frame is filtered on the game
thread.

#### hqx

The vendored
[hqx](https://github.com/brunexgeek/hqx) library (Apache License 2.0, git
submodule in `vendor/hqx`; run `git submodule update --init` after cloning)
does the scaling.

Its `HQ2x.cc` and `HQ3x.cc` are compiled unchanged, but `HQx.cc` is replaced by
`src/hqx_upscaler.cpp`. The library converts both colors to YUV in floating
point for every neighbor comparison, about 115 ms per 856x480 frame. The
replacement carries each pixel's palette index in the alpha byte and looks up
a 256x256 similarity table that is rebuilt only when the palette changes. That
takes about 6 ms (hq2x) or 7 ms (hq3x) per frame on one core and produces
output byte-identical to the original library. Each stripe is scaled with one
extra source row above and below, so the seams match the single-pass result.
With 8 threads a frame takes about 1.1 ms (hq2x) or 1.5 ms (hq3x).
`src/hqx_compat` supplies the `<stdint.h>` that Visual C++ 2005 lacks and an
empty `<iostream>`, which the hqx sources include without using. On a
1920x1080 display the hq2x output (1712x960) is stretched by a factor of
1.125 without smoothing, which duplicates about every eighth row and column.

#### xBR

`src/xbr.cpp` implements Hyllian's xBR "level 2" rules for 2x and 3x. For
each corner of a source pixel, two weighted color-distance sums over a 4x4
neighborhood decide whether an edge runs diagonally through that corner; if
so, the output subpixels at the corner are blended toward the closer
neighbor. A further test detects shallow slopes, which cover more subpixels.
The rule is written once and applied to all four corners by rotating the
neighborhood. Distances (YUV weighted 48:7:6) and similarity (the hqx
thresholds) come from 256x256 palette tables that are rebuilt only when the
palette changes. Pixels equal to their four direct neighbors are copied.
Compared with hqx, outlines are rounder and diagonals cleaner. One thread
takes about 10 ms (2x) or 13 ms (3x) per frame on a modern CPU, 8 threads
under 2 ms.

#### Sharp bilinear

`src/sharp_bilinear.cpp` scales the 856x480 frame straight to the display
size, so DirectDraw copies it 1:1 instead of stretching it. It behaves like
a nearest-neighbor enlargement by the whole factor closest to the scale,
followed by bilinear filtering of the remainder: each output pixel takes the
color of its source pixel, and only a narrow band at source pixel edges is
blended with the neighbor. Non-integer scales such as 2.25 (1920x1080) thus
give evenly sized pixels instead of every eighth row and column doubled. Per
column and row, lookup tables hold the source pixel and blend weight; rows are
filtered horizontally once and then blended vertically. It uses integer
arithmetic only. One thread takes about 2 ms (2560x1440) or 4 ms (1920x1080)
per frame on a modern CPU, 8 threads well under 1 ms.

#### Bicubic

`src/bicubic.cpp` scales the frame straight to the display size with the
Catmull-Rom cubic, the sharpest bicubic that adds no blur of its own. Unlike
the pixel-art filters it also softens the blocks the game itself creates when
it enlarges near sprites, and the noise of the digitized textures; hard edges
may show a faint overshoot. Each output pixel combines a 4x4 neighborhood with
10-bit integer weights from per-column and per-row tables; each source row is
filtered horizontally once, then rows are combined vertically. Samples that
fall exactly on a source pixel are copied. One thread takes about 19 ms
(2560x1440) or 15 ms (1920x1080) per frame on a modern CPU, 8 threads about
2.5 ms. On a single-core Pentium 4 it is only practical at low display
resolutions.

#### Diagnostics

For each filter (and `none` for the 8-bit path), the log records once where
the time of a frame goes, averaged over 300 frames: `game` (the game's own
work between presents), `convert` (palette conversion, filter, FPS overlay),
`vblank-wait`, `present` (the stretch blit) and the resulting frame time and
rate. Frames after a pause of more than 250 ms, such as loading, are left out.

If the 32-bit mode is not X8R8G8B8, or 8-bit surfaces cannot be created
alongside it, the 8-bit display mode is used as before. The 720x480 fallback
never uses a filter.

Known limitations: with `WaitForVBlank=0`, tearing is possible. Widths above
856 are untested, and so is Windows 98.

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
