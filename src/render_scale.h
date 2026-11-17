// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef RENDER_SCALE_H
#define RENDER_SCALE_H

#include <windows.h>

// Lets the game's own world rasterizer draw at twice the HiRes resolution
// (960 lines). See render_scale.cpp.

// Prepares the rasterizer for up to 960 lines. Call once, after the
// executable was recognized. Returns FALSE (and changes nothing) when the
// code differs from the original game.
BOOL RenderScaleInstall(DWORD gameBase);

// Switches the conversion of world coordinates to pixels between the
// original HiRes scale (FALSE) and twice that (TRUE). Only after a
// successful RenderScaleInstall.
void RenderScaleSetDoubled(BOOL doubled);

// Draws the sky panorama at twice its size. The panorama is first (columns
// 0 to half - 1) followed by second, period columns in all, rows lines high,
// and is shown from column (x - scroll2 / 2) at 1x. scroll2 is the scroll
// at twice the precision. left2 and top2 are where the sky starts in dest.
void RenderScaleDrawSky(
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight,
    const BYTE* first, LONG firstPitch,
    const BYTE* second, LONG secondPitch,
    LONG half, LONG period, LONG rows,
    LONG scroll2, LONG left2, LONG top2);

// Before the HUD is drawn into its 480-line surface: flips the top bit of
// every pixel, so each pixel differs from what the HUD drew there last
// frame, and keeps a copy in before (width bytes per row).
void RenderScalePrepareHud(
    BYTE* hud, LONG hudPitch, LONG width, LONG height, BYTE* before);

// After the HUD was drawn: every pixel that changed is copied as a 2x2
// block into dest.
void RenderScaleComposeHud(
    const BYTE* hud, LONG hudPitch, const BYTE* before,
    LONG width, LONG height,
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight);

#endif
