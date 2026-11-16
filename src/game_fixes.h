// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef GAME_FIXES_H
#define GAME_FIXES_H

#include <windows.h>

// Fixes for faults in the original game, independent of the widescreen view.
// Applied once, after the executable has been recognized. gameBase is the
// module base of ROADRASH.EXE, iniPath the settings file (or NULL).
void ApplyGameFixes(DWORD gameBase, const CHAR* iniPath);

#endif
