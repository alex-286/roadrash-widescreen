// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_FPS_OVERLAY_H
#define ROAD_RASH_FPS_OVERLAY_H

#include <windows.h>

// Counts one presented frame; the displayed rate is refreshed twice a second.
void FpsOverlayCountFrame();

// Draws the frame rate (when showRate is set) and an optional label, such as
// the filter name, into the top-left corner of a locked surface.
// bytesPerPixel is 1 (palette indices) or 4 (X8R8G8B8); scale multiplies the
// glyph size. Letters without a glyph are left blank.
void FpsOverlayDraw(
    BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD bytesPerPixel,
    DWORD scale,
    DWORD foreground,
    DWORD background,
    BOOL showRate,
    const CHAR* label);

// Finds the palette entries closest to white and black in 256 PALETTEENTRY
// values (red, green, blue, flags).
void FpsOverlayPickColors(const BYTE* entries, DWORD* white, DWORD* black);

#endif
