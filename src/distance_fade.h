// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef DISTANCE_FADE_H
#define DISTANCE_FADE_H

#include <windows.h>

static const DWORD kMaxFadeSegments = 16;
static const DWORD kDefaultFadeSegments = 16;

// Fades the farthest `fadeSegments` road segments into the sky behind them
// with an ordered dither instead of letting each new segment pop in whole;
// 0 leaves the game unchanged. Call once after DrawDistanceInstall. Returns
// FALSE (and changes nothing) when the code differs from the original game.
BOOL DistanceFadeInstall(DWORD gameBase, DWORD fadeSegments);

#endif
