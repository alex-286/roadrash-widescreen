#ifndef ROAD_RASH_SHARP_BILINEAR_H
#define ROAD_RASH_SHARP_BILINEAR_H

#include <windows.h>

// Sharp bilinear scaling of an 8-bit frame straight to the display size:
// conceptually a nearest-neighbor enlargement by the whole factor closest to
// the scale, followed by bilinear filtering of the remainder. Pixels stay
// crisp; only a narrow band at pixel edges is blended, so non-integer scales
// do not produce uneven pixel widths.

// Builds the lookup tables and per-thread buffers for the worker pool.
BOOL SharpBilinearInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);
void SharpBilinearShutdown();
BOOL SharpBilinearIsReady();

// rgb maps palette indices to X8R8G8B8.
void SharpBilinearScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch);

#endif
