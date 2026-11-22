// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ALT_TAB_H
#define ALT_TAB_H

#include <windows.h>

// On Windows NT and newer, Windows blocks Alt+Tab, Alt+Esc, Ctrl+Esc and
// the Windows keys for ROADRASH.EXE with the IgnoreAltTab compatibility
// fix; this ends the keyboard hook of that fix. On Windows 9x the game
// blocks Alt+Tab itself; this removes its keyboard hook and gives Alt+Tab,
// Alt+Esc and the Windows keys of its windows to Windows. gameBase is the
// module base of ROADRASH.EXE 1.0A.
void AllowAltTab(DWORD gameBase);

#endif
