// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "sharp_bilinear.h"
#include "cpu_features.h"
#include "worker_pool.h"

#include <intrin.h>
#include <mmintrin.h>

#pragma intrinsic(__movsd)
// The MMX row functions leave the MMX state to ScaleTask, which clears it
// once with _mm_empty after all rows.
#pragma warning(disable : 4799)

static const DWORD kMaxParts = 8;

// Per output column or row: the first source pixel and the weight (0-255)
// of the next one.
struct AxisTable
{
    DWORD* index;
    BYTE* weight;
};

// Each part keeps the last two horizontally filtered source rows.
struct PartRows
{
    DWORD* rows[2];
    LONG tags[2];
};

struct ScaleJob
{
    const BYTE* source;
    LONG sourcePitch;
    const DWORD* rgb;
    // With MMX: the palette with each color's channels in 16-bit lanes.
    const __m64* palette;
    // The smooth distance haze, or NULL; with MMX, its colors unpacked.
    const HazeFrame* haze;
    const __m64* hazePalette;
    BYTE* destination;
    LONG destinationPitch;
};

static DWORD g_sourceWidth = 0;
static DWORD g_sourceHeight = 0;
static DWORD g_destinationWidth = 0;
static DWORD g_destinationHeight = 0;
static AxisTable g_columns;
static AxisTable g_rows;
static PartRows g_parts[kMaxParts];
static DWORD g_partCount = 0;
// With MMX: each column's weight in all four 16-bit lanes.
static __m64* g_columnWeights = NULL;

static void* Allocate(DWORD bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void Free(void* pointer)
{
    if (pointer != NULL)
    {
        VirtualFree(pointer, 0, MEM_RELEASE);
    }
}

// Maps each destination pixel center to a source position in 16.16 fixed
// point (source pixel centers at k + 0.5). Within a source pixel, positions
// closer than half a prescaled pixel to its center snap to the center; the
// rest ramps to the neighbor, as the prescaled image would under bilinear
// filtering.
static BOOL BuildAxis(DWORD source, DWORD destination, AxisTable* table)
{
    DWORD prescale = (destination + source / 2) / source;
    LONG range;
    DWORD position;

    table->index = reinterpret_cast<DWORD*>(
        Allocate(destination * sizeof(DWORD)));
    table->weight = reinterpret_cast<BYTE*>(Allocate(destination));
    if (table->index == NULL || table->weight == NULL)
    {
        return FALSE;
    }

    if (prescale < 1)
    {
        prescale = 1;
    }

    range = 32768 - 32768 / static_cast<LONG>(prescale);
    for (position = 0; position < destination; ++position)
    {
        LONG texel = MulDiv(
            static_cast<int>(2 * position + 1),
            static_cast<int>(source * 32768),
            static_cast<int>(destination));
        LONG floor = texel & ~0xFFFF;
        LONG center = (texel & 0xFFFF) - 32768;
        LONG clamped = center < -range ? -range : (center > range ? range : center);
        LONG sample = floor +
            (center - clamped) * static_cast<LONG>(prescale);
        LONG first = sample >> 16;
        LONG weight = (sample & 0xFFFF) >> 8;

        if (first < 0)
        {
            first = 0;
            weight = 0;
        }

        if (first >= static_cast<LONG>(source) - 1)
        {
            first = static_cast<LONG>(source) - 1;
            weight = 0;
        }

        table->index[position] = static_cast<DWORD>(first);
        table->weight[position] = static_cast<BYTE>(weight);
    }

    return TRUE;
}

static void FreeAxis(AxisTable* table)
{
    Free(table->index);
    Free(table->weight);
    table->index = NULL;
    table->weight = NULL;
}

// Blends two X8R8G8B8 colors; weight is the share of second (0-256).
static __forceinline DWORD Blend(DWORD first, DWORD second, DWORD weight)
{
    DWORD inverse = 256 - weight;
    DWORD redBlue =
        ((first & 0x00FF00FF) * inverse + (second & 0x00FF00FF) * weight) >> 8;
    DWORD green =
        ((first & 0x0000FF00) * inverse + (second & 0x0000FF00) * weight) >> 8;

    return (redBlue & 0x00FF00FF) | (green & 0x0000FF00);
}

// The color table index of a source pixel: the palette index, or with
// haze the haze level and palette index.
template <bool Hazy>
static __forceinline DWORD PixelKey(const BYTE* line, const BYTE* levels, DWORD column)
{
    return Hazy ? HazeKey(line, levels, column) : line[column];
}

template <bool Hazy>
static const BYTE* LevelRow(const ScaleJob* job, DWORD sourceRow)
{
    return Hazy ?
        job->haze->levels + static_cast<LONG>(sourceRow) * job->haze->levelPitch :
        NULL;
}

template <bool Hazy>
static void FilterRowWith(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    const BYTE* source = job->source +
        static_cast<LONG>(sourceRow) * job->sourcePitch;
    const BYTE* levels = LevelRow<Hazy>(job, sourceRow);
    const DWORD* rgb = Hazy ? job->haze->rgb : job->rgb;
    DWORD column;

    for (column = 0; column < g_destinationWidth; ++column)
    {
        DWORD index = g_columns.index[column];
        DWORD weight = g_columns.weight[column];
        DWORD color = rgb[PixelKey<Hazy>(source, levels, index)];

        output[column] = weight == 0 ?
            color : Blend(color, rgb[PixelKey<Hazy>(source, levels, index + 1)], weight);
    }
}

// MMX: the blend of Blend for the four channels at once, as
// (a * 256 + (b - a) * weight) >> 8 = (a * (256 - weight) + b * weight) >> 8.
// The intermediate (b - a) * weight may leave the 16-bit range, but the
// final sum lies in 0-65280, so the wrapping 16-bit arithmetic is exact and
// the result equals Blend's.
static __forceinline __m64 BlendMmx(__m64 first, __m64 second, __m64 weight)
{
    return _mm_srli_pi16(
        _mm_add_pi16(
            _mm_slli_pi16(first, 8),
            _mm_mullo_pi16(_mm_sub_pi16(second, first), weight)),
        8);
}

template <bool Hazy>
static void FilterRowMmxWith(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    const BYTE* source = job->source +
        static_cast<LONG>(sourceRow) * job->sourcePitch;
    const BYTE* levels = LevelRow<Hazy>(job, sourceRow);
    const DWORD* rgb = Hazy ? job->haze->rgb : job->rgb;
    const __m64* palette = Hazy ? job->hazePalette : job->palette;
    const DWORD* index = g_columns.index;
    const BYTE* weight = g_columns.weight;
    const __m64* weights = g_columnWeights;
    __m64 zero = _mm_setzero_si64();
    DWORD column;

    for (column = 0; column < g_destinationWidth; ++column)
    {
        DWORD first = index[column];

        if (weight[column] == 0)
        {
            output[column] = rgb[PixelKey<Hazy>(source, levels, first)];
            continue;
        }

        output[column] = static_cast<DWORD>(_mm_cvtsi64_si32(_mm_packs_pu16(
            BlendMmx(
                palette[PixelKey<Hazy>(source, levels, first)],
                palette[PixelKey<Hazy>(source, levels, first + 1)],
                weights[column]),
            zero)));
    }
}

static BOOL RowHasHaze(const ScaleJob* job, DWORD sourceRow)
{
    return job->haze != NULL && job->haze->rows[sourceRow] != 0;
}

static void FilterRow(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    if (RowHasHaze(job, sourceRow))
    {
        FilterRowWith<true>(job, sourceRow, output);
    }
    else
    {
        FilterRowWith<false>(job, sourceRow, output);
    }
}

static void FilterRowMmx(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    if (RowHasHaze(job, sourceRow))
    {
        FilterRowMmxWith<true>(job, sourceRow, output);
    }
    else
    {
        FilterRowMmxWith<false>(job, sourceRow, output);
    }
}

static void BlendRowsMmx(
    const DWORD* upper, const DWORD* lower, DWORD weight, DWORD* output)
{
    __m64 zero = _mm_setzero_si64();
    __m64 factor = _mm_set1_pi16(static_cast<short>(weight));
    DWORD column = 0;

    for (; column + 2 <= g_destinationWidth; column += 2)
    {
        __m64 top = *reinterpret_cast<const __m64*>(upper + column);
        __m64 bottom = *reinterpret_cast<const __m64*>(lower + column);

        *reinterpret_cast<__m64*>(output + column) = _mm_packs_pu16(
            BlendMmx(
                _mm_unpacklo_pi8(top, zero),
                _mm_unpacklo_pi8(bottom, zero),
                factor),
            BlendMmx(
                _mm_unpackhi_pi8(top, zero),
                _mm_unpackhi_pi8(bottom, zero),
                factor));
    }

    for (; column < g_destinationWidth; ++column)
    {
        output[column] = Blend(upper[column], lower[column], weight);
    }
}

// Returns the filtered source row, computing it into a slot that does not
// hold keep.
static DWORD* GetRow(const ScaleJob* job, PartRows* part, LONG row, LONG keep)
{
    DWORD slot;

    if (part->tags[0] == row)
    {
        return part->rows[0];
    }

    if (part->tags[1] == row)
    {
        return part->rows[1];
    }

    slot = part->tags[0] == keep ? 1 : 0;
    if (g_columnWeights != NULL)
    {
        FilterRowMmx(job, static_cast<DWORD>(row), part->rows[slot]);
    }
    else
    {
        FilterRow(job, static_cast<DWORD>(row), part->rows[slot]);
    }
    part->tags[slot] = row;
    return part->rows[slot];
}

static void ScaleTask(DWORD part, DWORD, void* context)
{
    const ScaleJob* job = reinterpret_cast<const ScaleJob*>(context);
    PartRows* rows = &g_parts[part];
    DWORD first;
    DWORD last;
    DWORD row;

    if (part >= g_partCount)
    {
        return;
    }

    first = g_destinationHeight * part / g_partCount;
    last = g_destinationHeight * (part + 1) / g_partCount;
    rows->tags[0] = -1;
    rows->tags[1] = -1;
    for (row = first; row < last; ++row)
    {
        LONG sourceRow = static_cast<LONG>(g_rows.index[row]);
        DWORD weight = g_rows.weight[row];
        DWORD* output = reinterpret_cast<DWORD*>(
            job->destination + static_cast<LONG>(row) * job->destinationPitch);
        const DWORD* upper = GetRow(job, rows, sourceRow, sourceRow + 1);
        const DWORD* lower;
        DWORD column;

        if (weight == 0)
        {
            __movsd(
                reinterpret_cast<unsigned long*>(output),
                reinterpret_cast<const unsigned long*>(upper),
                g_destinationWidth);
            continue;
        }

        lower = GetRow(job, rows, sourceRow + 1, sourceRow);
        if (g_columnWeights != NULL)
        {
            BlendRowsMmx(upper, lower, weight, output);
            continue;
        }

        for (column = 0; column < g_destinationWidth; ++column)
        {
            output[column] = Blend(upper[column], lower[column], weight);
        }
    }

    // The FPU (shared with MMX) must be usable again by the caller.
    if (g_columnWeights != NULL)
    {
        _mm_empty();
    }
}

BOOL SharpBilinearInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight)
{
    DWORD count = WorkerPoolSize();
    DWORD index;

    SharpBilinearShutdown();
    if (sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0)
    {
        return FALSE;
    }

    if (count > kMaxParts)
    {
        count = kMaxParts;
    }

    g_sourceWidth = sourceWidth;
    g_sourceHeight = sourceHeight;
    g_destinationWidth = destinationWidth;
    g_destinationHeight = destinationHeight;
    if (!BuildAxis(sourceWidth, destinationWidth, &g_columns) ||
        !BuildAxis(sourceHeight, destinationHeight, &g_rows))
    {
        SharpBilinearShutdown();
        return FALSE;
    }

    if (CpuHasMmx())
    {
        g_columnWeights = reinterpret_cast<__m64*>(
            Allocate(destinationWidth * sizeof(__m64)));
        if (g_columnWeights != NULL)
        {
            for (index = 0; index < destinationWidth; ++index)
            {
                WORD weight = g_columns.weight[index];
                WORD* lanes = reinterpret_cast<WORD*>(&g_columnWeights[index]);

                lanes[0] = weight;
                lanes[1] = weight;
                lanes[2] = weight;
                lanes[3] = weight;
            }
        }
    }

    for (index = 0; index < count; ++index)
    {
        g_parts[index].rows[0] = reinterpret_cast<DWORD*>(
            Allocate(destinationWidth * sizeof(DWORD)));
        g_parts[index].rows[1] = reinterpret_cast<DWORD*>(
            Allocate(destinationWidth * sizeof(DWORD)));
        if (g_parts[index].rows[0] == NULL || g_parts[index].rows[1] == NULL)
        {
            SharpBilinearShutdown();
            return FALSE;
        }
    }

    g_partCount = count;
    return TRUE;
}

void SharpBilinearShutdown()
{
    DWORD index;

    for (index = 0; index < kMaxParts; ++index)
    {
        Free(g_parts[index].rows[0]);
        Free(g_parts[index].rows[1]);
        g_parts[index].rows[0] = NULL;
        g_parts[index].rows[1] = NULL;
    }

    FreeAxis(&g_columns);
    FreeAxis(&g_rows);
    Free(g_columnWeights);
    g_columnWeights = NULL;
    g_partCount = 0;
    g_sourceWidth = 0;
    g_sourceHeight = 0;
    g_destinationWidth = 0;
    g_destinationHeight = 0;
}

BOOL SharpBilinearIsReady()
{
    return g_partCount != 0;
}

void SharpBilinearScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch)
{
    ScaleJob job;

    if (g_partCount == 0)
    {
        return;
    }

    job.source = source;
    job.sourcePitch = sourcePitch;
    job.rgb = rgb;
    job.palette = NULL;
    job.haze = haze;
    job.hazePalette = NULL;
    if (g_columnWeights != NULL)
    {
        static __m64 unpacked[256];
        static __m64 unpackedHaze[kHazeTableSize];
        __m64 zero = _mm_setzero_si64();
        DWORD index;

        for (index = 0; index < 256; ++index)
        {
            unpacked[index] = _mm_unpacklo_pi8(
                _mm_cvtsi32_si64(static_cast<int>(rgb[index])), zero);
        }

        if (haze != NULL)
        {
            for (index = 0; index < kHazeTableSize; ++index)
            {
                unpackedHaze[index] = _mm_unpacklo_pi8(
                    _mm_cvtsi32_si64(static_cast<int>(haze->rgb[index])), zero);
            }

            job.hazePalette = unpackedHaze;
        }

        _mm_empty();
        job.palette = unpacked;
    }

    job.destination = destination;
    job.destinationPitch = destinationPitch;
    WorkerPoolRun(ScaleTask, &job);
}
