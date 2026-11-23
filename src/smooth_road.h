/**
 * Keeps the road from jumping by a few pixels while riding.
 *
 * @file smooth_road.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMOOTH_ROAD_H
#define SMOOTH_ROAD_H

#include <windows.h>

/**
 * Adds back the parts of a unit that the game drops when it projects the
 * road, so the road and its edges no longer jump by a few pixels while
 * riding.
 *
 * Call once after DrawDistanceInstall, whose section hook it chains to.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @return FALSE (and nothing is changed) when the code differs from the
 *         original game.
 */
BOOL SmoothRoadInstall(DWORD gameBase);

#endif
