/**
 * Lets the game's own world rasterizer draw at 2, 3 or 4 times the HiRes
 * resolution (960, 1440 or 1920 lines).
 *
 * See render_scale.cpp.
 *
 * @file render_scale.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RENDER_SCALE_H
#define RENDER_SCALE_H

#include <windows.h>

/** The largest scale the rasterizer can be prepared for. */
static const DWORD kMaxRenderScale = 4;

/**
 * Prepares the rasterizer for up to maxScale x 480 lines.
 *
 * Call once, after the executable was recognized.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @param [in] maxScale The largest scale used later (2 to kMaxRenderScale).
 * @return FALSE (and nothing is changed) when the code differs from the
 *         original game.
 */
BOOL RenderScaleInstall(DWORD gameBase, DWORD maxScale);

/**
 * Sets the scale of the conversion of world coordinates to pixels and of
 * the rasterizer.
 *
 * Only after a successful RenderScaleInstall.
 *
 * @param [in] scale 1 for the original HiRes, up to the installed maximum.
 */
void RenderScaleSetActive(DWORD scale);

/**
 * Converts HiRes fixed point (such as the sky camera) to pixels at a
 * scale.
 *
 * @param [in] value The fixed point value, with 15 fraction bits.
 * @param [in] scale The render scale.
 * @return value * scale / 2^15, rounded down, without overflow.
 */
LONG RenderScaleFixed(LONG value, DWORD scale);

/**
 * Draws the sky panorama enlarged scale times.
 *
 * The panorama is first (columns 0 to half - 1) followed by second, period
 * columns in all, rows lines high, and is shown from column
 * (x - scroll / scale) at 1x.
 *
 * @param [out] dest The first pixel of the frame the sky is drawn into.
 * @param [in] destPitch The bytes per row of dest.
 * @param [in] destWidth The width of dest in pixels.
 * @param [in] destHeight The height of dest in pixels.
 * @param [in] first The first half of the panorama.
 * @param [in] firstPitch The bytes per row of first.
 * @param [in] second The second half of the panorama.
 * @param [in] secondPitch The bytes per row of second.
 * @param [in] half The number of columns of first.
 * @param [in] period The number of columns of the whole panorama.
 * @param [in] rows The number of lines of the panorama.
 * @param [in] scale The render scale.
 * @param [in] scroll The scroll in enlarged pixels.
 * @param [in] left The column in dest where the sky starts.
 * @param [in] top The row in dest where the sky starts.
 */
void RenderScaleDrawSky(
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight,
    const BYTE* first, LONG firstPitch,
    const BYTE* second, LONG secondPitch,
    LONG half, LONG period, LONG rows, DWORD scale,
    LONG scroll, LONG left, LONG top);

/**
 * Prepares the 480-line HUD surface before the HUD is drawn into it.
 *
 * Flips the top bit of every pixel, so each pixel differs from what the
 * HUD drew there last frame, and keeps a copy.
 *
 * @param [in,out] hud The first pixel of the HUD surface.
 * @param [in] hudPitch The bytes per row of hud.
 * @param [in] width The width of the HUD in pixels.
 * @param [in] height The height of the HUD in pixels.
 * @param [out] before Receives the prepared surface, width bytes per row.
 */
void RenderScalePrepareHud(
    BYTE* hud, LONG hudPitch, LONG width, LONG height, BYTE* before);

/**
 * Enlarges the HUD over the world after it was drawn.
 *
 * Every pixel that changed since RenderScalePrepareHud is copied as a block
 * of scale x scale pixels into dest.
 *
 * @param [in] hud The first pixel of the HUD surface.
 * @param [in] hudPitch The bytes per row of hud.
 * @param [in] before The copy made by RenderScalePrepareHud.
 * @param [in] width The width of the HUD in pixels.
 * @param [in] height The height of the HUD in pixels.
 * @param [in] scale The render scale.
 * @param [in,out] dest The first pixel of the enlarged frame.
 * @param [in] destPitch The bytes per row of dest.
 * @param [in] destWidth The width of dest in pixels.
 * @param [in] destHeight The height of dest in pixels.
 * @param [in,out] levels The smooth haze level plane, laid out like dest,
 *                 cleared under the copied blocks; or NULL.
 */
void RenderScaleComposeHud(
    const BYTE* hud, LONG hudPitch, const BYTE* before,
    LONG width, LONG height, DWORD scale,
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight,
    BYTE* levels);

#endif
