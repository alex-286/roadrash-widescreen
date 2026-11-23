/**
 * The bicubic filter: a separable resampler in which every output pixel is
 * a weighted sum of 4x4 source pixels, with Catmull-Rom cubic weights
 * (smooth, photo-like enlargement).
 *
 * Work is split across the worker pool. Integer arithmetic only.
 *
 * @file resampler.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_RESAMPLER_H
#define ROAD_RASH_RESAMPLER_H

#include <windows.h>

#include "haze_frame.h"

/** The largest number of parts a frame is split into. */
static const DWORD kResamplerMaxParts = 8;

/** The number of source pixels weighted per axis for each output pixel. */
static const DWORD kResamplerTaps = 4;

/**
 * The horizontally filtered source rows one part keeps: the last
 * kResamplerTaps rows, row r in slot r % kResamplerTaps.
 */
struct ResamplerRows
{
    /** One X8R8G8B8 row, destinationWidth pixels, per slot. */
    DWORD* rows[kResamplerTaps];
    /** The source row held in each slot, or -1 for none. */
    LONG tags[kResamplerTaps];
};

/** The tables and buffers of the bicubic filter for one pair of sizes. */
struct Resampler
{
    /** The width of the 8-bit frame in pixels. */
    DWORD sourceWidth;
    /** The height of the 8-bit frame in pixels. */
    DWORD sourceHeight;
    /** The width of the scaled frame in pixels. */
    DWORD destinationWidth;
    /** The height of the scaled frame in pixels. */
    DWORD destinationHeight;
    /** kResamplerTaps source columns per destination column. */
    DWORD* columnIndex;
    /** kResamplerTaps weights (10 fraction bits) per destination column. */
    SHORT* columnWeight;
    /** kResamplerTaps source rows per destination row. */
    DWORD* rowIndex;
    /** kResamplerTaps weights (10 fraction bits) per destination row. */
    SHORT* rowWeight;
    /** The filtered rows of each part. */
    ResamplerRows parts[kResamplerMaxParts];
    /** The number of parts with buffers; 0 when not initialized. */
    DWORD partCount;
};

/**
 * Builds the weight tables and per-thread buffers.
 *
 * Call after WorkerPoolStart; the frame is split into WorkerPoolSize()
 * parts (at most kResamplerMaxParts).
 *
 * @param [out] resampler The resampler to set up; whatever it held is freed
 *              first.
 * @param [in] sourceWidth The width of the 8-bit frame in pixels.
 * @param [in] sourceHeight The height of the 8-bit frame in pixels.
 * @param [in] destinationWidth The width of the scaled frame in pixels.
 * @param [in] destinationHeight The height of the scaled frame in pixels.
 * @return TRUE when the tables and buffers are ready.
 */
BOOL ResamplerInitialize(
    Resampler* resampler,
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight);

/**
 * Frees the tables and buffers.
 *
 * @param [in,out] resampler The resampler, left empty.
 */
void ResamplerShutdown(Resampler* resampler);

/**
 * Tells whether ResamplerScaleIndexed can be used.
 *
 * @param [in] resampler The resampler.
 * @return TRUE after a successful ResamplerInitialize.
 */
BOOL ResamplerIsReady(const Resampler* resampler);

/**
 * Scales an 8-bit frame to the sizes given to ResamplerInitialize.
 *
 * @param [in,out] resampler The resampler; its row buffers are used.
 * @param [in] source The first pixel of the 8-bit frame.
 * @param [in] sourcePitch The bytes per row of source.
 * @param [in] rgb 256 X8R8G8B8 colors, one per palette index.
 * @param [in] haze The smooth distance haze to add, or NULL.
 * @param [out] destination The first pixel of the X8R8G8B8 frame.
 * @param [in] destinationPitch The bytes per row of destination.
 */
void ResamplerScaleIndexed(
    Resampler* resampler,
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch);

#endif
