// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_SCREENSHOT_H
#define ROAD_RASH_SCREENSHOT_H

#include <windows.h>

// Writes a locked surface as a BMP file in the game folder.
// bitsPerPixel is 8 (palette: 256 PALETTEENTRY values) or 32 (X8R8G8B8,
// written as 24-bit). With numbered set, name is a prefix and the first free
// name from prefix001.bmp to prefix999.bmp is used; otherwise name is the
// file name and an existing file is replaced. savedName receives the file
// name that was written (at least MAX_PATH characters).
BOOL ScreenshotWriteBmp(
    const CHAR* name,
    BOOL numbered,
    const BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD bitsPerPixel,
    const BYTE* palette,
    CHAR* savedName);

#endif
