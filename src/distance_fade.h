/**
 * Fades the farthest road segments into a haze, so new segments do not pop
 * in whole at the end of the draw distance.
 *
 * @file distance_fade.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DISTANCE_FADE_H
#define DISTANCE_FADE_H

#include <windows.h>

#include "haze_frame.h"

/** The largest number of road segments that can fade into the haze. */
static const DWORD kMaxFadeSegments = 16;

/** The number of fading segments used when the settings name none. */
static const DWORD kDefaultFadeSegments = 16;

/**
 * Fades the farthest road segments into a haze instead of letting each new
 * segment pop in whole.
 *
 * Call once after DrawDistanceInstall.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @param [in] fadeSegments The number of farthest segments that fade (up to
 *             kMaxFadeSegments); 0 leaves the game unchanged.
 * @return FALSE (and nothing is changed) when the code differs from the
 *         original game.
 */
BOOL DistanceFadeInstall(DWORD gameBase, DWORD fadeSegments);

/**
 * Switches the smooth haze on or off, for the races drawn through a 32-bit
 * filter.
 *
 * Instead of drawing hazy pixels in the nearest palette color, the frame
 * keeps their own color and a level plane holds their haze, which the
 * filter blends in. Set for each race.
 *
 * @param [in] smooth TRUE for the smooth haze; FALSE returns to the palette
 *             remap.
 */
void DistanceFadeSetSmooth(BOOL smooth);

/**
 * Gives the haze of the frame before it is filtered.
 *
 * @param [in] frame The locked 8-bit frame the game draws into.
 * @param [in] pitch The bytes per row of frame.
 * @param [in] height The number of rows of frame.
 * @param [in] rgb The palette the frame is shown with, as X8R8G8B8 per index.
 * @return The haze to blend in, or NULL when there is no smooth haze or
 *         none in this frame.
 */
const HazeFrame* DistanceFadePrepareFrame(
    const BYTE* frame, LONG pitch, DWORD height, const DWORD* rgb);

/** Clears the haze of the frame after it was filtered, for the next frame. */
void DistanceFadeFrameDone();

/**
 * Gives the level plane laid out like a frame.
 *
 * Code that draws over the frame outside the game's rasterizer clears the
 * levels of the pixels it covers.
 *
 * @param [in] frame The frame the plane must match.
 * @param [in] pitch The bytes per row of frame.
 * @return The level plane, with the same pitch as frame, or NULL when
 *         there is none for this frame.
 */
BYTE* DistanceFadeLevelsFor(const BYTE* frame, LONG pitch);

#endif
