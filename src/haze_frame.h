/**
 * The smooth distance haze of a frame, as the 32-bit filters read it.
 *
 * A haze level for every pixel of the 8-bit frame, and the colors to show
 * for each level and palette index. See distance_fade.cpp.
 *
 * @file haze_frame.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HAZE_FRAME_H
#define HAZE_FRAME_H

#include <windows.h>

/** The number of haze levels, including level 0 (no haze). */
static const DWORD kHazeLevels = 16;

/** The number of colors in HazeFrame::rgb: one per level and palette index. */
static const DWORD kHazeTableSize = kHazeLevels * 256;

/** The smooth distance haze of one frame. */
struct HazeFrame
{
    /** One level (0 = no haze) per frame pixel, levelPitch bytes per row. */
    const BYTE* levels;
    /** The bytes per row of levels. */
    LONG levelPitch;
    /**
     * One flag per frame row: nonzero for the rows that hold any haze;
     * other rows need no look at levels.
     */
    const BYTE* rows;
    /**
     * X8R8G8B8 per table index (level << 8) | palette index; level 0 is
     * the plain palette.
     */
    const DWORD* rgb;
};

/**
 * Gives the table index of a pixel in HazeFrame::rgb.
 *
 * @param [in] line The frame row of the pixel (palette indices).
 * @param [in] levels The level row that belongs to line.
 * @param [in] column The column of the pixel.
 * @return (level << 8) | palette index.
 */
static __forceinline DWORD HazeKey(const BYTE* line, const BYTE* levels, DWORD column)
{
    return (static_cast<DWORD>(levels[column]) << 8) | line[column];
}

#endif
