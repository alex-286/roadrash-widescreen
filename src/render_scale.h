// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef RENDER_SCALE_H
#define RENDER_SCALE_H

#include <windows.h>

// Lets the game's own world rasterizer draw at 2, 3 or 4 times the HiRes
// resolution (960, 1440 or 1920 lines). See render_scale.cpp.
static const DWORD kMaxRenderScale = 4;

// Prepares the rasterizer for up to maxScale x 480 lines (maxScale 2 to 4).
// Call once, after the executable was recognized. Returns FALSE (and
// changes nothing) when the code differs from the original game.
BOOL RenderScaleInstall(DWORD gameBase, DWORD maxScale);

// Sets the scale of the conversion of world coordinates to pixels and of
// the rasterizer: 1 for the original HiRes, up to the installed maximum.
// Only after a successful RenderScaleInstall.
void RenderScaleSetActive(DWORD scale);

// value * scale / 2^15, rounded down, without overflow: the conversion of
// HiRes fixed point (such as the sky camera) to pixels at the scale.
LONG RenderScaleFixed(LONG value, DWORD scale);

// Draws the sky panorama enlarged scale times. The panorama is first
// (columns 0 to half - 1) followed by second, period columns in all, rows
// lines high, and is shown from column (x - scroll / scale) at 1x. scroll is
// the scroll in enlarged pixels; left and top are where the sky starts in
// dest.
void RenderScaleDrawSky(
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight,
    const BYTE* first, LONG firstPitch,
    const BYTE* second, LONG secondPitch,
    LONG half, LONG period, LONG rows, DWORD scale,
    LONG scroll, LONG left, LONG top);

// Before the HUD is drawn into its 480-line surface: flips the top bit of
// every pixel, so each pixel differs from what the HUD drew there last
// frame, and keeps a copy in before (width bytes per row).
void RenderScalePrepareHud(
    BYTE* hud, LONG hudPitch, LONG width, LONG height, BYTE* before);

// After the HUD was drawn: every pixel that changed is copied as a block of
// scale x scale pixels into dest.
void RenderScaleComposeHud(
    const BYTE* hud, LONG hudPitch, const BYTE* before,
    LONG width, LONG height, DWORD scale,
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight);

#endif
