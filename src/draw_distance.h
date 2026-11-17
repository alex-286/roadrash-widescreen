// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef DRAW_DISTANCE_H
#define DRAW_DISTANCE_H

#include <windows.h>

// The game draws 32 road segments ahead of the camera. 48 is tested (and is
// what the 30th Anniversary Edition draws with the same engine); with 64,
// buildings beside the road were broken at medium distance in the city,
// for a reason not yet found.
static const DWORD kOriginalDrawSegments = 32;
static const DWORD kMaxDrawSegments = 48;

// Lets the game draw `segments` road segments ahead (33 to 48); 32 leaves
// the game unchanged. Call once, after the executable was recognized and
// before the first race. Returns FALSE (and changes nothing) when the code
// differs from the original game.
BOOL DrawDistanceInstall(DWORD gameBase, DWORD segments);

#endif
