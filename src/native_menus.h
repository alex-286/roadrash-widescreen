// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef NATIVE_MENUS_H
#define NATIVE_MENUS_H

#include <windows.h>

// Menus in the native display mode: the game's 640x480 menu (a Windows
// interface: its main window, button windows, dialogs and the video
// window) is enlarged to the screen height in the middle of the screen;
// the videos by a whole factor.
//
// NativeMenusInstall patches the game's imports once (gameBase = module
// base of ROADRASH.EXE; background: fill the area around the menu with
// its blurred picture instead of black). NativeMenusWatchSurface is given
// every new DirectDraw surface, to hook the drawing into the menu's screen
// surface.
// NativeMenusEnter is called after the menu's display mode (width x height)
// was set, NativeMenusLeave before the race's mode is set.
BOOL NativeMenusInstall(DWORD gameBase, BOOL background);
void NativeMenusWatchSurface(LPVOID surface);
void NativeMenusEnter(DWORD width, DWORD height);
void NativeMenusLeave();

// When the game ends: removes the window hook and, on Windows 95/98/ME
// (sharedSystem), puts the shared DirectDraw vtable slots back.
void NativeMenusShutdown(BOOL sharedSystem);
BOOL NativeMenusActive();

#endif
