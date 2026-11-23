/**
 * Sharp bilinear scaling of an 8-bit frame straight to the display size.
 *
 * Conceptually a nearest-neighbor enlargement by the whole factor closest
 * to the scale, followed by bilinear filtering of the remainder. Pixels
 * stay crisp; only a narrow band at pixel edges is blended, so non-integer
 * scales do not produce uneven pixel widths.
 *
 * @file sharp_bilinear.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_SHARP_BILINEAR_H
#define ROAD_RASH_SHARP_BILINEAR_H

#include <windows.h>

#include "haze_frame.h"

/**
 * Builds the lookup tables and per-thread buffers for the worker pool.
 *
 * @param [in] sourceWidth The width of the 8-bit frame in pixels.
 * @param [in] sourceHeight The height of the 8-bit frame in pixels.
 * @param [in] destinationWidth The width of the scaled frame in pixels.
 * @param [in] destinationHeight The height of the scaled frame in pixels.
 * @return TRUE when the tables and buffers are ready.
 */
BOOL SharpBilinearInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);

/** Frees the lookup tables and buffers. */
void SharpBilinearShutdown();

/**
 * Tells whether SharpBilinearScale can be used.
 *
 * @return TRUE after a successful SharpBilinearInitialize.
 */
BOOL SharpBilinearIsReady();

/**
 * Scales an 8-bit frame to the sizes given to SharpBilinearInitialize.
 *
 * @param [in] source The first pixel of the 8-bit frame.
 * @param [in] sourcePitch The bytes per row of source.
 * @param [in] rgb 256 X8R8G8B8 colors, one per palette index.
 * @param [in] haze The smooth distance haze to add, or NULL.
 * @param [out] destination The first pixel of the X8R8G8B8 frame.
 * @param [in] destinationPitch The bytes per row of destination.
 */
void SharpBilinearScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch);

#endif
