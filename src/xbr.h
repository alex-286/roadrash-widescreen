// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_XBR_H
#define ROAD_RASH_XBR_H

#include <windows.h>

// xBR (Hyllian's "level 2" rules) pixel-art scaling of an 8-bit frame by 2,
// 3 or 4. Like hqx it smooths edges between flat colors, with cleaner
// diagonals and shallow slopes.

// Allocates per-frame state for the worker pool. scale must be 2, 3 or 4.
BOOL XbrInitialize(DWORD width, DWORD height, DWORD scale);
void XbrShutdown();
BOOL XbrIsReady();

// rgb maps palette indices to X8R8G8B8. The color-distance tables are
// rebuilt when it changes.
void XbrScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch);

// Row-by-row use, without the whole scaled frame in memory: XbrPrepareFrame
// takes the frame, then XbrRow returns scaled row `row` (width * scale
// pixels) for worker part `part` (0-7), computing the scale rows of one
// source row at a time. It matches ResamplerRowSource. Asked in increasing
// order, each source row is computed once per part; a returned row stays
// valid until that part asks for a row of another source row.
BOOL XbrPrepareFrame(const BYTE* source, LONG sourcePitch, const DWORD* rgb);
const DWORD* XbrRow(void* context, DWORD part, DWORD row);

#endif
