// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_NEAREST_H
#define ROAD_RASH_NEAREST_H

#include <windows.h>

#include "haze_frame.h"

// Nearest-neighbor scaling of an 8-bit frame straight to the display size,
// so the present only copies. (Windows 98's DirectDraw stretches 32-bit
// surfaces in software, which took 32 ms per frame at 1920x1080.)

// Builds the lookup tables for the given sizes.
BOOL NearestInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);
void NearestShutdown();
BOOL NearestIsReady();

// rgb maps palette indices to X8R8G8B8; haze (or NULL) adds the smooth
// distance haze.
void NearestScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch);

#endif
