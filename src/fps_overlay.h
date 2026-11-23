/**
 * A small frame rate and label display in the top-left corner of the
 * screen.
 *
 * @file fps_overlay.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_FPS_OVERLAY_H
#define ROAD_RASH_FPS_OVERLAY_H

#include <windows.h>

/**
 * Counts one presented frame; the displayed rate is refreshed twice a
 * second.
 */
void FpsOverlayCountFrame();

/**
 * Draws the frame rate and an optional label into the top-left corner of a
 * locked surface.
 *
 * Letters without a glyph are left blank.
 *
 * @param [in,out] pixels The first pixel of the locked surface.
 * @param [in] pitch The bytes per row of the surface.
 * @param [in] width The width of the surface in pixels.
 * @param [in] height The height of the surface in pixels.
 * @param [in] bytesPerPixel 1 (palette indices) or 4 (X8R8G8B8).
 * @param [in] scale The factor that multiplies the glyph size.
 * @param [in] foreground The color of the glyphs, in the surface's format.
 * @param [in] background The color behind the glyphs, in the surface's format.
 * @param [in] showRate TRUE to draw the frame rate.
 * @param [in] label Text drawn after the rate, such as the filter name,
 *             or NULL.
 */
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

/**
 * Finds the palette entries that stand in for white and black.
 *
 * The brightest and darkest entries by channel sum are used, which keeps
 * the text readable during palette fades.
 *
 * @param [in] entries 256 PALETTEENTRY values (red, green, blue, flags).
 * @param [out] white Receives the index of the entry closest to white.
 * @param [out] black Receives the index of the entry closest to black.
 */
void FpsOverlayPickColors(const BYTE* entries, DWORD* white, DWORD* black);

#endif
