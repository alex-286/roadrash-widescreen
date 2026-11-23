/**
 * Nearest-neighbor scaling of an 8-bit frame straight to the display size,
 * so the present only copies.
 *
 * Windows 98's DirectDraw stretches 32-bit surfaces in software, which took
 * 32 ms per frame at 1920x1080.
 *
 * @file nearest.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_NEAREST_H
#define ROAD_RASH_NEAREST_H

#include <windows.h>

#include "haze_frame.h"

/**
 * Builds the lookup tables for the given sizes.
 *
 * @param [in] sourceWidth The width of the 8-bit frame in pixels.
 * @param [in] sourceHeight The height of the 8-bit frame in pixels.
 * @param [in] destinationWidth The width of the scaled frame in pixels.
 * @param [in] destinationHeight The height of the scaled frame in pixels.
 * @return TRUE when the tables are ready.
 */
BOOL NearestInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);

/** Frees the lookup tables. */
void NearestShutdown();

/**
 * Tells whether NearestScale can be used.
 *
 * @return TRUE after a successful NearestInitialize.
 */
BOOL NearestIsReady();

/**
 * Scales an 8-bit frame to the sizes given to NearestInitialize.
 *
 * @param [in] source The first pixel of the 8-bit frame.
 * @param [in] sourcePitch The bytes per row of source.
 * @param [in] rgb 256 X8R8G8B8 colors, one per palette index.
 * @param [in] haze The smooth distance haze to add, or NULL.
 * @param [out] destination The first pixel of the X8R8G8B8 frame.
 * @param [in] destinationPitch The bytes per row of destination.
 */
void NearestScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch);

#endif
