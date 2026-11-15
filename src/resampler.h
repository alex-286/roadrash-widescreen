// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_RESAMPLER_H
#define ROAD_RASH_RESAMPLER_H

#include <windows.h>

// A separable resampler: every output pixel is a weighted sum of up to 4x4
// source pixels. The weights come from a kernel:
//
//   bicubic  Catmull-Rom cubic: smooth, photo-like enlargement.
//   area     Each output pixel averages the source pixels it covers,
//            weighted by coverage. Enlarging gives crisp pixels with soft
//            edges; reducing gives proper averaging (supersampling). Scale
//            ratios down to 1/3 are supported.
//
// Work is split across the worker pool. Integer arithmetic only.
enum ResamplerKernel
{
    kResamplerBicubic,
    kResamplerArea
};

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
    // Set for kernels without negative weights: pixels that blend only two
    // source pixels then use a cheaper packed blend.
    BOOL blendPairs;
};

// Builds the weight tables and per-thread buffers. Fails when the kernel
// does not support the scale (area: reducing by more than 3).
BOOL ResamplerInitialize(
    Resampler* resampler,
    ResamplerKernel kernel,
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);
void ResamplerShutdown(Resampler* resampler);
BOOL ResamplerIsReady(const Resampler* resampler);

// Scales an 8-bit frame; rgb maps palette indices to X8R8G8B8.
void ResamplerScaleIndexed(
    Resampler* resampler,
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch);

// Scales an X8R8G8B8 frame.
void ResamplerScaleRgb(
    Resampler* resampler,
    const BYTE* source,
    LONG sourcePitch,
    BYTE* destination,
    LONG destinationPitch);

// Returns source row `row` (X8R8G8B8, sourceWidth pixels) for worker part
// `part`. Within one scaling, each part asks for rows in increasing order
// and reads a row completely before asking for the next, so a source can
// produce rows on demand into a small per-part buffer.
typedef const DWORD* (*ResamplerRowSource)(void* context, DWORD part, DWORD row);

// Scales an X8R8G8B8 frame whose rows come from rowSource, so the whole
// source image never has to exist in memory.
void ResamplerScaleRgbRows(
    Resampler* resampler,
    ResamplerRowSource rowSource,
    void* context,
    BYTE* destination,
    LONG destinationPitch);

#endif
