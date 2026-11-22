// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef DISTANCE_FADE_H
#define DISTANCE_FADE_H

#include <windows.h>

#include "haze_frame.h"

static const DWORD kMaxFadeSegments = 16;
static const DWORD kDefaultFadeSegments = 16;

// Fades the farthest `fadeSegments` road segments into a haze instead of
// letting each new segment pop in whole; 0 leaves the game unchanged. Call
// once after DrawDistanceInstall. Returns FALSE (and changes nothing) when
// the code differs from the original game.
BOOL DistanceFadeInstall(DWORD gameBase, DWORD fadeSegments);

// Smooth haze, for the races drawn through a 32-bit filter: instead of
// drawing hazy pixels in the nearest palette color, the frame keeps their
// own color and a level plane holds their haze, which the filter blends in.
// Set for each race; FALSE returns to the palette remap.
void DistanceFadeSetSmooth(BOOL smooth);

// Before the frame is filtered: the haze to blend in, or NULL (no smooth
// haze, or none in this frame). frame and pitch are the locked 8-bit frame
// the game draws into, rgb the palette it is shown with.
const HazeFrame* DistanceFadePrepareFrame(
    const BYTE* frame, LONG pitch, DWORD height, const DWORD* rgb);

// After the frame was filtered: clears its haze for the next frame.
void DistanceFadeFrameDone();

// The level plane laid out like frame (with pitch), or NULL; code that
// draws over the frame outside the game's rasterizer clears the levels of
// the pixels it covers.
BYTE* DistanceFadeLevelsFor(const BYTE* frame, LONG pitch);

#endif
