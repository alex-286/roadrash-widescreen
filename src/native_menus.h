/**
 * Menus in the native display mode.
 *
 * The game's 640x480 menu (a Windows interface: its main window, button
 * windows, dialogs and the video window) is enlarged to the screen height
 * in the middle of the screen; the videos by a whole factor.
 *
 * @file native_menus.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NATIVE_MENUS_H
#define NATIVE_MENUS_H

#include <windows.h>

/**
 * Patches the game's imports for the native menus.
 *
 * Every import is checked first, so nothing changes on a mismatch. A
 * second call does nothing.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @param [in] background TRUE to fill the area around the menu with its
 *             blurred picture instead of black.
 * @return TRUE when the imports are patched (or already were).
 */
BOOL NativeMenusInstall(DWORD gameBase, BOOL background);

/**
 * Hooks the drawing into a new DirectDraw surface.
 *
 * Given every new DirectDraw surface, so the drawing into the menu's screen
 * surface can be enlarged. Does nothing before NativeMenusInstall.
 *
 * @param [in] surface The new IDirectDrawSurface, or NULL.
 */
void NativeMenusWatchSurface(LPVOID surface);

/**
 * Starts the native menus, after the menu's display mode was set.
 *
 * @param [in] width The width of the display mode in pixels.
 * @param [in] height The height of the display mode in pixels.
 */
void NativeMenusEnter(DWORD width, DWORD height);

/**
 * Ends the native menus, before the race's display mode is set or before
 * the menus fall back to 640x480.
 *
 * @param [in] keepScreenSize TRUE to let the main window keep covering the
 *             screen while the mode changes, so the desktop does not show
 *             around it.
 */
void NativeMenusLeave(BOOL keepScreenSize);

/**
 * Cleans up when the game ends.
 *
 * Removes the window hook. On Windows 95/98/ME, DDRAW.DLL and its surface
 * vtable are shared by every process; left pointing into this DLL, the
 * vtable would lead into freed memory once the game is gone, so the slots
 * get their original values back, unless something else changed them
 * since. On Windows NT every process has its own copy, which is not
 * touched.
 *
 * @param [in] sharedSystem TRUE on Windows 95/98/ME.
 */
void NativeMenusShutdown(BOOL sharedSystem);

/**
 * Tells whether the native menus are shown.
 *
 * @return TRUE between NativeMenusEnter and NativeMenusLeave.
 */
BOOL NativeMenusActive();

#endif
