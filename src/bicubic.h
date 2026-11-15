// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_BICUBIC_H
#define ROAD_RASH_BICUBIC_H

#include <windows.h>

// Bicubic (Catmull-Rom) scaling of an 8-bit frame straight to the display
// size. Smooth like a photo enlargement: it also softens blocks the game
// itself enlarged with nearest-neighbor scaling.

// Builds the lookup tables and per-thread buffers for the worker pool.
BOOL BicubicInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);
void BicubicShutdown();
BOOL BicubicIsReady();

// rgb maps palette indices to X8R8G8B8.
void BicubicScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch);

#endif
