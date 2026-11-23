/**
 * Lets the game draw more road segments ahead of the camera.
 *
 * @file draw_distance.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DRAW_DISTANCE_H
#define DRAW_DISTANCE_H

#include <windows.h>

/** The number of road segments the original game draws ahead of the camera. */
static const DWORD kOriginalDrawSegments = 32;

/**
 * The largest number of road segments that can be drawn ahead.
 *
 * 48 is tested (and is what the 30th Anniversary Edition draws with the
 * same engine); with 64, buildings beside the road were broken at medium
 * distance in the city, for a reason not yet found.
 */
static const DWORD kMaxDrawSegments = 48;

/**
 * Lets the game draw more road segments ahead.
 *
 * Call once, after the executable was recognized and before the first
 * race.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @param [in] segments The number of segments to draw ahead (33 to
 *             kMaxDrawSegments); kOriginalDrawSegments leaves the game
 *             unchanged.
 * @return FALSE (and nothing is changed) when the code differs from the
 *         original game.
 */
BOOL DrawDistanceInstall(DWORD gameBase, DWORD segments);

/**
 * Gives the slice array the game draws the road from.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @return The moved array after a successful install, the game's own
 *         otherwise.
 */
BYTE* DrawDistanceSlices(DWORD gameBase);

/**
 * Gives the number of road segments the game draws ahead.
 *
 * @return The installed number after a successful install,
 *         kOriginalDrawSegments otherwise.
 */
DWORD DrawDistanceSegments();

#endif
