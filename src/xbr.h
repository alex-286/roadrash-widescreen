#ifndef ROAD_RASH_XBR_H
#define ROAD_RASH_XBR_H

#include <windows.h>

// xBR (Hyllian's "level 2" rules) pixel-art scaling of an 8-bit frame by 2
// or 3. Like hqx it smooths edges between flat colors, with cleaner
// diagonals and shallow slopes.

// Allocates per-frame state for the worker pool. scale must be 2 or 3.
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

#endif
