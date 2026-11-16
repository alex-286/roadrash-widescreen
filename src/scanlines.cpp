// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "scanlines.h"
#include "cpu_features.h"
#include "worker_pool.h"

#include <mmintrin.h>

// ScanlineTask clears the MMX state once with _mm_empty after all rows.
#pragma warning(disable : 4799)

struct ScanlineJob
{
    BYTE* pixels;
    LONG pitch;
    DWORD width;
    DWORD height;
    DWORD sourceHeight;
    DWORD strength;
    BOOL mmx;
};

// The brightness factor (0-256) of an output row. Its center lies at
// fraction phase / period within its source line, where
// phase = (2 * row + 1) * sourceHeight mod period and period = 2 * height.
//
// With a whole number of rows per source line, the rows in the last third
// of each line are darkened by the full strength. Otherwise rows fall at
// varying phases, which would leave some lines without a dark row; rows are
// then darkened in proportion to their closeness to a line boundary (full
// strength on it, none from a third of a line away), which spreads the
// darkening evenly.
static DWORD RowFactor(const ScanlineJob* job, DWORD row)
{
    DWORD period = 2 * job->height;
    DWORD phase = (2 * row + 1) * job->sourceHeight % period;
    DWORD full = job->strength * 256 / 100;
    DWORD distance;

    if (job->height % job->sourceHeight == 0)
    {
        return 3 * phase >= 2 * period ? 256 - full : 256;
    }

    distance = phase < period - phase ? phase : period - phase;
    if (3 * distance >= period)
    {
        return 256;
    }

    return 256 - static_cast<DWORD>(MulDiv(
        static_cast<int>(full),
        static_cast<int>(period - 3 * distance),
        static_cast<int>(period)));
}

static void ScanlineTask(DWORD part, DWORD parts, void* context)
{
    const ScanlineJob* job = reinterpret_cast<const ScanlineJob*>(context);
    DWORD first = job->height * part / parts;
    DWORD last = job->height * (part + 1) / parts;
    DWORD row;

    for (row = first; row < last; ++row)
    {
        DWORD factor = RowFactor(job, row);
        DWORD* pixel;
        DWORD column;

        if (factor >= 256)
        {
            continue;
        }

        pixel = reinterpret_cast<DWORD*>(
            job->pixels + static_cast<LONG>(row) * job->pitch);
        column = 0;
        if (job->mmx)
        {
            // Two pixels at a time; channel * factor stays below 65536,
            // so the result equals the loop below (top byte cleared).
            __m64 zero = _mm_setzero_si64();
            __m64 scale = _mm_set1_pi16(static_cast<short>(factor));
            __m64 mask = _mm_set_pi32(0x00FFFFFF, 0x00FFFFFF);

            for (; column + 2 <= job->width; column += 2)
            {
                __m64 colors = *reinterpret_cast<__m64*>(pixel + column);

                *reinterpret_cast<__m64*>(pixel + column) = _mm_and_si64(
                    _mm_packs_pu16(
                        _mm_srli_pi16(
                            _mm_mullo_pi16(_mm_unpacklo_pi8(colors, zero), scale),
                            8),
                        _mm_srli_pi16(
                            _mm_mullo_pi16(_mm_unpackhi_pi8(colors, zero), scale),
                            8)),
                    mask);
            }
        }

        for (; column < job->width; ++column)
        {
            DWORD color = pixel[column];

            pixel[column] =
                ((((color & 0x00FF00FF) * factor) >> 8) & 0x00FF00FF) |
                ((((color & 0x0000FF00) * factor) >> 8) & 0x0000FF00);
        }
    }

    // The FPU (shared with MMX) must be usable again by the caller.
    if (job->mmx)
    {
        _mm_empty();
    }
}

void ScanlinesApply(
    BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD sourceHeight,
    DWORD strength)
{
    ScanlineJob job;

    if (strength == 0 || sourceHeight == 0 || height < 2 * sourceHeight)
    {
        return;
    }

    if (strength > 100)
    {
        strength = 100;
    }

    job.pixels = pixels;
    job.pitch = pitch;
    job.width = width;
    job.height = height;
    job.sourceHeight = sourceHeight;
    job.strength = strength;
    job.mmx = CpuHasMmx();
    WorkerPoolRun(ScanlineTask, &job);
}
