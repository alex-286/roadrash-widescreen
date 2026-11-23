/**
 * Screenshots of locked surfaces as BMP files.
 *
 * @file screenshot.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_SCREENSHOT_H
#define ROAD_RASH_SCREENSHOT_H

#include <windows.h>

/**
 * Writes a locked surface as a BMP file in the game folder.
 *
 * @param [in] name With numbered set, a prefix: the first free name from
 *             prefix001.bmp to prefix999.bmp is used. Otherwise the file
 *             name; an existing file is replaced.
 * @param [in] numbered TRUE to number the file name.
 * @param [in] pixels The first pixel of the surface.
 * @param [in] pitch The bytes per row of the surface.
 * @param [in] width The width of the surface in pixels.
 * @param [in] height The height of the surface in pixels.
 * @param [in] bitsPerPixel 8 (palette indices) or 32 (X8R8G8B8, written as
 *             24-bit).
 * @param [in] palette 256 PALETTEENTRY values for 8 bits per pixel;
 *             otherwise ignored.
 * @param [out] savedName Receives the file name that was written (at least
 *              MAX_PATH characters).
 * @return TRUE when the whole file was written.
 */
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
