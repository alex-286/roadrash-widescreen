Road Rash Widescreen Patch
==========================

A widescreen patch for the Windows 95 version of Road Rash. It runs the
game at the native resolution of your monitor with a view that matches its
aspect ratio, so you see more of the road instead of a stretched 4:3
picture. Optional filters smooth the classic graphics on modern displays.

The patch changes nothing on disk. Delete ddraw.dll and the game is back to
normal.


Files
-----

ddraw.dll                 The patch. It is loaded by the game instead of
                          the Windows DirectDraw library and forwards all
                          calls to it, adding the widescreen changes.
RoadRashWidescreen.ini    Optional settings (resolution, filters, ...).
README.txt                This file.
LICENSE.txt               The license of the patch (GNU GPL v3).
THIRD_PARTY_NOTICES.txt   Licenses of the third-party code used.


Requirements
------------

- Road Rash for Windows 95, German version 1.0A. Other versions are not
  patched; the game then runs as usual.
- The game option "High Resolution" (640x480) must be enabled.
- Windows 98 or newer.


Installation
------------

1. Copy ddraw.dll and RoadRashWidescreen.ini into the game folder, next to
   ROADRASH.EXE.
   Never replace the ddraw.dll in the Windows system folder!
2. Start the game and make sure "High Resolution" is enabled.
3. Optional: open RoadRashWidescreen.ini in a text editor to change the
   settings. Every setting is explained in the file.


Uninstallation
--------------

Delete these files from the game folder:
ddraw.dll, RoadRashWidescreen.ini, RoadRashWidescreen.log and, if present,
RoadRashFrame.bmp.


Hotkeys during a race
---------------------

F11   Cycle through the display filters (only if a filter other than
      "none" is set in RoadRashWidescreen.ini).
F12   Save the current frame as RoadRashFrame.bmp in the game folder.


Troubleshooting
---------------

Each time the game starts, the patch writes RoadRashWidescreen.log to the
game folder. It shows whether the game version was recognized and which
display mode and filter are used. Please attach it when reporting a
problem.

- Nothing changes: make sure "High Resolution" is enabled and check the log
  for the recognized game version.
- A filter is set but not used: the 32-bit display mode is not available at
  your resolution; the log says so and the game runs in 8-bit mode.
- Low frame rate with a filter: try Filter=sharp-bilinear, HqxScale=2, or
  Filter=none.


License
-------

Copyright (C) 2026 Alexander Schmidt

This program is free software under the GNU General Public License,
version 3 or later; see LICENSE.txt. It comes WITHOUT ANY WARRANTY.
Third-party code: see THIRD_PARTY_NOTICES.txt.

Road Rash is a trademark of Electronic Arts. This project is not affiliated
with or endorsed by Electronic Arts, contains no game files, and requires an
original copy of the game.
