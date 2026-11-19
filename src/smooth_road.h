// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef SMOOTH_ROAD_H
#define SMOOTH_ROAD_H

#include <windows.h>

// Adds back the parts of a unit that the game drops when it projects the
// road, so the road and its edges no longer jump by a few pixels while
// riding. Call once after DrawDistanceInstall, whose section hook it
// chains to. Returns FALSE (and changes nothing) when the code differs
// from the original game.
BOOL SmoothRoadInstall(DWORD gameBase);

#endif
