#ifndef ROAD_RASH_HQX_UPSCALER_H
#define ROAD_RASH_HQX_UPSCALER_H

#include <windows.h>

// Allocates the work buffers for an 8-bit source frame of the given size,
// split into one stripe per worker pool part. scale must be 2 or 3.
BOOL HqxUpscalerInitialize(DWORD width, DWORD height, DWORD scale);
void HqxUpscalerShutdown();
BOOL HqxUpscalerIsReady();

// Takes 256 PALETTEENTRY values (red, green, blue, flags). The similarity
// table is only rebuilt when the colors changed.
void HqxUpscalerSetPalette(const BYTE* entries);

// Writes the hqx-scaled frame (width * scale by height * scale, X8R8G8B8).
void HqxUpscalerScale(
    const BYTE* source,
    LONG sourcePitch,
    BYTE* destination,
    LONG destinationPitch);

#endif
