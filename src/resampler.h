// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_RESAMPLER_H
#define ROAD_RASH_RESAMPLER_H

#include <windows.h>

#include "haze_frame.h"

// The bicubic filter: a separable resampler in which every output pixel is
// a weighted sum of 4x4 source pixels, with Catmull-Rom cubic weights
// (smooth, photo-like enlargement).
//
// Work is split across the worker pool. Integer arithmetic only.
static const DWORD kResamplerMaxParts = 8;
static const DWORD kResamplerTaps = 4;

struct ResamplerRows
{
    DWORD* rows[kResamplerTaps];
    LONG tags[kResamplerTaps];
};

struct Resampler
{
    DWORD sourceWidth;
    DWORD sourceHeight;
    DWORD destinationWidth;
    DWORD destinationHeight;
    DWORD* columnIndex;
    SHORT* columnWeight;
    DWORD* rowIndex;
    SHORT* rowWeight;
    ResamplerRows parts[kResamplerMaxParts];
    DWORD partCount;
};

// Builds the weight tables and per-thread buffers.
BOOL ResamplerInitialize(
    Resampler* resampler,
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);
void ResamplerShutdown(Resampler* resampler);
BOOL ResamplerIsReady(const Resampler* resampler);

// Scales an 8-bit frame; rgb maps palette indices to X8R8G8B8, and haze
// (or NULL) adds the smooth distance haze.
void ResamplerScaleIndexed(
    Resampler* resampler,
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch);

#endif
