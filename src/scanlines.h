// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_SCANLINES_H
#define ROAD_RASH_SCANLINES_H

#include <windows.h>

// Darkens rows between the source lines of a filtered X8R8G8B8 frame, like
// the gaps between the lines of a CRT. strength is the darkening in percent
// (1-100). The frame must have at least two rows per source line; otherwise
// nothing is drawn.
void ScanlinesApply(
    BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD sourceHeight,
    DWORD strength);

#endif
