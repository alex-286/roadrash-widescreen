/**
 * Gives Alt+Tab and the other task switching keys back to Windows while
 * the game runs.
 *
 * @file alt_tab.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ALT_TAB_H
#define ALT_TAB_H

#include <windows.h>

/**
 * Lets the player switch away from the game with the keyboard.
 *
 * On Windows NT and newer, Windows blocks Alt+Tab, Alt+Esc, Ctrl+Esc and
 * the Windows keys for ROADRASH.EXE with the IgnoreAltTab compatibility
 * fix; this ends the keyboard hook of that fix. On Windows 9x the game
 * blocks Alt+Tab itself; this removes its keyboard hook and gives Alt+Tab,
 * Alt+Esc and the Windows keys of its windows to Windows.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE 1.0A.
 */
void AllowAltTab(DWORD gameBase);

#endif
