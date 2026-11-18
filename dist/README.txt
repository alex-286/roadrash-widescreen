Road Rash Widescreen Patch
==========================

A widescreen patch for the Windows 95 version of Road Rash. It runs the
game at the native resolution of your monitor with a view that matches its
aspect ratio, so you see more of the road instead of a stretched 4:3
picture. Races are drawn at twice the original resolution (960 lines), and
optional filters smooth the classic graphics on modern displays.

The menus stay at the resolution of the races: they are enlarged to the
screen height in the middle of the screen, and the videos by the largest
whole factor that fits.
NativeMenus=0 in RoadRashWidescreen.ini switches the screen to 640x480 for
the menus, as the original game does.

On Windows XP and newer, the patch changes nothing on disk: delete
ddraw.dll and the game is back to normal. On Windows 95/98/ME, a small tool
changes one name in ROADRASH.EXE so that the game loads the patch; it can
undo the change.


Files
-----

ddraw.dll                 The patch for Windows XP and newer. It is loaded
                          by the game instead of the Windows DirectDraw
                          library and forwards all calls to it, adding the
                          widescreen changes.
RRWS.dll                  The same patch under another name, for Windows
                          95/98/ME.
RRWSPatch.exe             Windows 95/98/ME only: sets up ROADRASH.EXE to
                          load RRWS.dll, and undoes it when run again.
RoadRashWidescreen.ini    Optional settings (resolution, filters, ...).
README.txt                This file.
LICENSE.txt               The license of the patch (GNU GPL v3).


Requirements
------------

- Road Rash for Windows 95, version 1.0A: ROADRASH.EXE of 23 August 1996,
  541,184 bytes. This executable serves all of the game's languages
  (English, German, French, Spanish, Italian). Other executables are not
  patched; the game then runs as usual.
- The game option "High Resolution" (640x480) must be enabled.
- Windows 95 or newer. Tested on Windows 98 SE and Windows 11; Windows 95,
  ME and the versions from XP to 10 should work but are untested.


Installation
------------

Never replace the ddraw.dll in the Windows system folder!

Windows XP and newer:

1. Copy ddraw.dll and RoadRashWidescreen.ini into the game folder, next to
   ROADRASH.EXE.
2. Start the game and make sure "High Resolution" is enabled.
3. Optional: open RoadRashWidescreen.ini in a text editor to change the
   settings. Every setting is explained in the file.

Windows 95, 98 and ME:

Windows 9x ignores a ddraw.dll in the game folder, so the game has to load
the patch under the name RRWS.dll.

1. Copy RRWS.dll, RRWSPatch.exe and RoadRashWidescreen.ini into the game
   folder, next to ROADRASH.EXE. Do not copy ddraw.dll.
2. Run RRWSPatch.exe and click "Yes". It changes the name DDRAW.dll in
   ROADRASH.EXE to RRWS.dll (nothing else) and saves the unchanged program
   as ROADRASH.BAK.
3. Start the game as usual and make sure "High Resolution" is enabled.
4. Optional: change the settings in RoadRashWidescreen.ini.

Keep RRWS.dll in the game folder: the changed ROADRASH.EXE does not start
without it.


Uninstallation
--------------

On Windows 95/98/ME, first run RRWSPatch.exe again and click "Yes" to undo
the change to ROADRASH.EXE (or copy ROADRASH.BAK back to ROADRASH.EXE).

Then delete these files from the game folder:
ddraw.dll or RRWS.dll and RRWSPatch.exe, RoadRashWidescreen.ini,
RoadRashWidescreen.log and, if present, the screenshots
RoadRashScreen001.bmp, ... and RoadRashFrame.bmp.


Game fixes
----------

The patch also repairs some faults of the original game. Each can be
turned off in the [Fixes] section of RoadRashWidescreen.ini.

- The Rat and Super bikes race in their own colors, as in the shop.
- Buildings, riders and cars move smoothly in High Resolution mode instead
  of in steps of 2 pixels.
- On Windows Vista and newer, a music volume of 0 no longer mutes all other
  sound of the game.
- Riders and cars in the distance move smoothly instead of in jumps: the
  game updated them only every few game ticks.
- The rider's shadow stays under the bike in the first frame of a punch.
- The red barn, the brown shop and the white house stand on the ground
  instead of hovering above it.


Hotkeys
-------

F11   During a race: cycle through the display filters (only if a filter
      other than "none" is set in RoadRashWidescreen.ini).
F12   Save a screenshot as RoadRashScreen001.bmp, RoadRashScreen002.bmp,
      ... in the game folder, in races and in the menus. It shows exactly
      what is on screen. In a race, the game's unscaled frame is also
      saved as RoadRashFrame.bmp.


Troubleshooting
---------------

Each time the game starts, the patch writes RoadRashWidescreen.log to the
game folder. It shows whether the game version was recognized and which
display mode and filter are used. Please attach it when reporting a
problem.

- No log file at all: the game did not load the patch. On Windows 95/98/ME,
  use RRWS.dll and RRWSPatch.exe as described above; a ddraw.dll in the
  game folder is ignored there.
- Nothing changes: make sure "High Resolution" is enabled and check the log
  for the recognized game version.
- A filter is set but not used: the 32-bit display mode is not available at
  your resolution; the log says so and the game runs in 8-bit mode.
- Low frame rate with a filter: try Filter=sharp-bilinear,
  WaitForVBlank=0, RenderScale=1 or Filter=none.
- No music in races, but music in the menus: the log lists every MIDI
  output with its number at the first race; set MusicDevice in the [Fixes]
  section to the number of the one you want.
- The game crashes: the log records where. Please attach it.


Credits
-------

Road Rash (Windows 95) (c) 1996 Electronic Arts. Electronic Arts presents a
Buzz Puppet Production in association with Papyrus Design Group, Inc.
Road Rash PC is based on Road Rash 3DO, (c) Electronic Arts 1991-94. All
credit for the game, its design, art, sound and the original program goes
to its creators.

Ideas and findings of these Road Rash projects guided the patch (no code
was taken from them):
- RoadRash30th-HD by OBLPlife, RoadRash30-MultiFix by OBLPlife and magoowa
- RoadRashFFNNOPatch by FFNNO
- Open Rash and Open Rash Next by Replica-
- road-rash by arkits
Their links and what each one contributed are listed in the README on the
project page.


License
-------

Copyright (C) 2026 Alexander Schmidt

This program is free software under the GNU General Public License,
version 3 or later; see LICENSE.txt. It comes WITHOUT ANY WARRANTY.

Road Rash is a trademark of Electronic Arts. This project is not affiliated
with or endorsed by Electronic Arts, contains no game files, and requires an
original copy of the game.
