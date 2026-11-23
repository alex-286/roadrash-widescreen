/**
 * CRT-like scanlines for the filtered 32-bit frames.
 *
 * @file scanlines.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_SCANLINES_H
#define ROAD_RASH_SCANLINES_H

#include <windows.h>

/**
 * Darkens rows between the source lines of a filtered X8R8G8B8 frame, like
 * the gaps between the lines of a CRT.
 *
 * The frame must have at least two rows per source line; otherwise nothing
 * is drawn.
 *
 * @param [in,out] pixels The first pixel of the frame.
 * @param [in] pitch The bytes per row of the frame.
 * @param [in] width The width of the frame in pixels.
 * @param [in] height The height of the frame in pixels.
 * @param [in] sourceHeight The number of lines of the frame before it was
 *             filtered.
 * @param [in] strength The darkening in percent (1-100); 0 draws nothing.
 */
void ScanlinesApply(
    BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD sourceHeight,
    DWORD strength);

#endif
