// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef HAZE_FRAME_H
#define HAZE_FRAME_H

#include <windows.h>

// The smooth distance haze of a frame for the 32-bit filters: a haze level
// for every pixel of the 8-bit frame, and the colors to show for each level
// and palette index. See distance_fade.cpp.
static const DWORD kHazeLevels = 16;
static const DWORD kHazeTableSize = kHazeLevels * 256;

struct HazeFrame
{
    // One level (0 = no haze) per frame pixel, levelPitch bytes per row.
    const BYTE* levels;
    LONG levelPitch;
    // Nonzero for the frame rows that hold any haze; other rows need no
    // look at levels.
    const BYTE* rows;
    // X8R8G8B8 per (level << 8) | index; level 0 is the plain palette.
    const DWORD* rgb;
};

// The table index of the pixel at column of a frame row and its level row.
static __forceinline DWORD HazeKey(const BYTE* line, const BYTE* levels, DWORD column)
{
    return (static_cast<DWORD>(levels[column]) << 8) | line[column];
}

#endif
