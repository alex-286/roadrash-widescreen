// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "resampler.h"
#include "cpu_features.h"
#include "worker_pool.h"

#include <intrin.h>
#include <mmintrin.h>

#pragma intrinsic(__movsd)
// The MMX row functions leave the MMX state to ScaleTask, which clears it
// once with _mm_empty after all rows.
#pragma warning(disable : 4799)

static const DWORD kTaps = kResamplerTaps;
// Weights are fixed point with 10 fractional bits; they sum to kOne.
static const LONG kWeightBits = 10;
static const LONG kOne = 1 << kWeightBits;

struct ScaleJob
{
    Resampler* resampler;
    const BYTE* source;
    LONG sourcePitch;
    const DWORD* rgb;
    // With MMX: the palette with each color's channels already in 16-bit
    // lanes.
    const __m64* palette;
    // The smooth distance haze, or NULL; with MMX, its colors unpacked.
    const HazeFrame* haze;
    const __m64* hazePalette;
    BYTE* destination;
    LONG destinationPitch;
};

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

// Catmull-Rom weights for the taps at -1, 0, +1 and +2 around a sample with
// fraction s/256. In units of 2 * 256^3:
//   w0 = -s^3 + 2*256*s^2 - 256^2*s
//   w1 = 3*s^3 - 5*256*s^2 + 2*256^3
//   w2 = -3*s^3 + 4*256*s^2 + 256^2*s
//   w3 = s^3 - 256*s^2
// All terms fit in 32 bits for s <= 256.
static void CatmullRomWeights(LONG s, SHORT* weights)
{
    static const LONG kUnitShift = 25 - kWeightBits;
    static const LONG kRound = 1 << (kUnitShift - 1);
    LONG s2 = s * s;
    LONG s3 = s2 * s;
    LONG raw[kTaps];
    LONG sum = 0;
    DWORD tap;

    raw[0] = -s3 + 2 * 256 * s2 - 65536 * s;
    raw[1] = 3 * s3 - 5 * 256 * s2 + 2 * 16777216;
    raw[2] = -3 * s3 + 4 * 256 * s2 + 65536 * s;
    raw[3] = s3 - 256 * s2;

    // 2 * 256^3 = 2^25 is the unit; rescale to kOne with rounding (the
    // offset keeps the rounding symmetric for negative weights).
    for (tap = 0; tap < kTaps; ++tap)
    {
        LONG value = raw[tap] >= 0 ?
            (raw[tap] + kRound) >> kUnitShift :
            -((-raw[tap] + kRound) >> kUnitShift);

        weights[tap] = static_cast<SHORT>(value);
        sum += value;
    }

    // Rounding may leave the sum off by one; the largest tap absorbs it.
    weights[s < 128 ? 1 : 2] =
        static_cast<SHORT>(weights[s < 128 ? 1 : 2] + (kOne - sum));
}

// Maps the destination pixel center to a source position in 16.16 fixed
// point and weights the four source pixels around it.
static void BicubicTaps(
    DWORD source,
    DWORD destination,
    DWORD position,
    LONG* first,
    SHORT* weights)
{
    LONG sample = MulDiv(
        static_cast<int>(2 * position + 1),
        static_cast<int>(source * 32768),
        static_cast<int>(destination)) - 32768;

    *first = (sample >> 16) - 1;
    CatmullRomWeights((sample & 0xFFFF) >> 8, weights);
}

// Builds the four source pixels (clamped to the image) and weights of each
// destination pixel. An entry that takes a single source pixel is stored
// with that pixel in the first tap, so it can be copied.
static BOOL BuildAxis(
    DWORD source,
    DWORD destination,
    DWORD** indexTable,
    SHORT** weightTable)
{
    DWORD* index = reinterpret_cast<DWORD*>(
        Allocate(destination * kTaps * sizeof(DWORD)));
    SHORT* weight = reinterpret_cast<SHORT*>(
        Allocate(destination * kTaps * sizeof(SHORT)));
    DWORD position;

    *indexTable = index;
    *weightTable = weight;
    if (index == NULL || weight == NULL)
    {
        return FALSE;
    }

    for (position = 0; position < destination; ++position)
    {
        DWORD* taps = index + position * kTaps;
        SHORT* weights = weight + position * kTaps;
        LONG first;
        DWORD tap;
        DWORD used = 0;
        DWORD single = 0;

        BicubicTaps(source, destination, position, &first, weights);
        for (tap = 0; tap < kTaps; ++tap)
        {
            LONG pixel = first + static_cast<LONG>(tap);

            if (pixel < 0)
            {
                pixel = 0;
            }
            else if (pixel >= static_cast<LONG>(source))
            {
                pixel = static_cast<LONG>(source) - 1;
            }

            taps[tap] = static_cast<DWORD>(pixel);
            if (weights[tap] != 0)
            {
                ++used;
                single = tap;
            }
        }

        if (used == 1 && single != 0)
        {
            taps[0] = taps[single];
            weights[0] = weights[single];
            weights[single] = 0;
        }
    }

    return TRUE;
}

// True when the weights select the first tap alone. Small opposite weights
// can cancel out, so all of them are checked.
static __forceinline BOOL IsCopy(const SHORT* weights)
{
    return weights[1] == 0 && weights[2] == 0 && weights[3] == 0;
}

static __forceinline DWORD ClampChannel(LONG value)
{
    value = (value + kOne / 2) >> kWeightBits;
    if (value < 0)
    {
        return 0;
    }

    return value > 255 ? 255 : static_cast<DWORD>(value);
}

// Weighted sum of four X8R8G8B8 colors, clamped per channel.
static __forceinline DWORD Combine(
    DWORD c0, DWORD c1, DWORD c2, DWORD c3, const SHORT* weights)
{
    LONG w0 = weights[0];
    LONG w1 = weights[1];
    LONG w2 = weights[2];
    LONG w3 = weights[3];
    LONG red =
        static_cast<LONG>((c0 >> 16) & 0xFF) * w0 +
        static_cast<LONG>((c1 >> 16) & 0xFF) * w1 +
        static_cast<LONG>((c2 >> 16) & 0xFF) * w2 +
        static_cast<LONG>((c3 >> 16) & 0xFF) * w3;
    LONG green =
        static_cast<LONG>((c0 >> 8) & 0xFF) * w0 +
        static_cast<LONG>((c1 >> 8) & 0xFF) * w1 +
        static_cast<LONG>((c2 >> 8) & 0xFF) * w2 +
        static_cast<LONG>((c3 >> 8) & 0xFF) * w3;
    LONG blue =
        static_cast<LONG>(c0 & 0xFF) * w0 +
        static_cast<LONG>(c1 & 0xFF) * w1 +
        static_cast<LONG>(c2 & 0xFF) * w2 +
        static_cast<LONG>(c3 & 0xFF) * w3;

    return (ClampChannel(red) << 16) |
        (ClampChannel(green) << 8) |
        ClampChannel(blue);
}

// MMX is used when the processor has it; the results are identical.
static BOOL g_useMmx = FALSE;

// MMX: Combine for the four channels at once. pmaddwd forms the sums of
// two taps per channel in 32 bits, which are added, rounded and shifted as
// in ClampChannel; the signed and unsigned saturating packs then clamp to
// 0-255 exactly like ClampChannel. Combine leaves the top byte 0.
// The colors come with their channels in 16-bit lanes.
static __forceinline DWORD CombineMmxLanes(
    const __m64* c0, const __m64* c1, const __m64* c2, const __m64* c3,
    const __m64* weights)
{
    __m64 p0 = *c0;
    __m64 p1 = *c1;
    __m64 p2 = *c2;
    __m64 p3 = *c3;
    __m64 zero = _mm_setzero_si64();
    __m64 round = _mm_set1_pi32(kOne / 2);
    __m64 weights01 = weights[0];
    __m64 weights23 = weights[1];
    __m64 blueGreen = _mm_add_pi32(
        _mm_madd_pi16(_mm_unpacklo_pi16(p0, p1), weights01),
        _mm_madd_pi16(_mm_unpacklo_pi16(p2, p3), weights23));
    __m64 redTop = _mm_add_pi32(
        _mm_madd_pi16(_mm_unpackhi_pi16(p0, p1), weights01),
        _mm_madd_pi16(_mm_unpackhi_pi16(p2, p3), weights23));

    blueGreen = _mm_srai_pi32(_mm_add_pi32(blueGreen, round), kWeightBits);
    redTop = _mm_srai_pi32(_mm_add_pi32(redTop, round), kWeightBits);
    return static_cast<DWORD>(_mm_cvtsi64_si32(_mm_packs_pu16(
        _mm_packs_pi32(blueGreen, redTop), zero))) & 0x00FFFFFF;
}

static __forceinline __m64 Unpack(DWORD color)
{
    return _mm_unpacklo_pi8(
        _mm_cvtsi32_si64(static_cast<int>(color)), _mm_setzero_si64());
}

static __forceinline DWORD CombineMmx(
    DWORD c0, DWORD c1, DWORD c2, DWORD c3, const __m64* weights)
{
    __m64 lanes[4];

    lanes[0] = Unpack(c0);
    lanes[1] = Unpack(c1);
    lanes[2] = Unpack(c2);
    lanes[3] = Unpack(c3);
    return CombineMmxLanes(
        &lanes[0], &lanes[1], &lanes[2], &lanes[3], weights);
}

// The tap pairs (w0, w1, w0, w1) and (w2, w3, w2, w3) for pmaddwd.
static __forceinline void SplitWeights(const SHORT* weights, __m64* split)
{
    __m64 all = *reinterpret_cast<const __m64*>(weights);

    split[0] = _mm_unpacklo_pi32(all, all);
    split[1] = _mm_unpackhi_pi32(all, all);
}

// The color table index of a source pixel: the palette index, or with
// haze the haze level and palette index.
template <bool Hazy>
static __forceinline DWORD PixelKey(const BYTE* line, const BYTE* levels, DWORD column)
{
    return Hazy ? HazeKey(line, levels, column) : line[column];
}

// Filters source row `sourceRow` horizontally into output (X8R8G8B8).
template <bool Hazy>
static void FilterRowWith(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    const Resampler* resampler = job->resampler;
    const BYTE* line =
        job->source + static_cast<LONG>(sourceRow) * job->sourcePitch;
    const BYTE* levels = Hazy ?
        job->haze->levels + static_cast<LONG>(sourceRow) * job->haze->levelPitch :
        NULL;
    const DWORD* index = resampler->columnIndex;
    const SHORT* weight = resampler->columnWeight;
    const DWORD* rgb = Hazy ? job->haze->rgb : job->rgb;
    DWORD width = resampler->destinationWidth;
    DWORD column;

    if (g_useMmx)
    {
        const __m64* palette = Hazy ? job->hazePalette : job->palette;

        for (column = 0; column < width; ++column)
        {
            if (IsCopy(weight))
            {
                output[column] = rgb[PixelKey<Hazy>(line, levels, index[0])];
            }
            else
            {
                __m64 split[2];

                SplitWeights(weight, split);
                output[column] = CombineMmxLanes(
                    &palette[PixelKey<Hazy>(line, levels, index[0])],
                    &palette[PixelKey<Hazy>(line, levels, index[1])],
                    &palette[PixelKey<Hazy>(line, levels, index[2])],
                    &palette[PixelKey<Hazy>(line, levels, index[3])],
                    split);
            }

            index += kTaps;
            weight += kTaps;
        }

        return;
    }

    for (column = 0; column < width; ++column)
    {
        output[column] = IsCopy(weight) ?
            rgb[PixelKey<Hazy>(line, levels, index[0])] :
            Combine(
                rgb[PixelKey<Hazy>(line, levels, index[0])],
                rgb[PixelKey<Hazy>(line, levels, index[1])],
                rgb[PixelKey<Hazy>(line, levels, index[2])],
                rgb[PixelKey<Hazy>(line, levels, index[3])],
                weight);
        index += kTaps;
        weight += kTaps;
    }
}

static void FilterRow(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    if (job->haze != NULL && job->haze->rows[sourceRow] != 0)
    {
        FilterRowWith<true>(job, sourceRow, output);
    }
    else
    {
        FilterRowWith<false>(job, sourceRow, output);
    }
}

// Each part keeps the last four horizontally filtered source rows, in slot
// row % 4. The rows a destination row needs are consecutive (or repeat the
// edge row), so they never share a slot.
static const DWORD* GetRow(const ScaleJob* job, ResamplerRows* part, DWORD row)
{
    DWORD slot = row & (kTaps - 1);

    if (part->tags[slot] != static_cast<LONG>(row))
    {
        FilterRow(job, row, part->rows[slot]);
        part->tags[slot] = static_cast<LONG>(row);
    }

    return part->rows[slot];
}

static void ScaleTask(DWORD part, DWORD, void* context)
{
    const ScaleJob* job = reinterpret_cast<const ScaleJob*>(context);
    Resampler* resampler = job->resampler;
    ResamplerRows* rows = &resampler->parts[part];
    DWORD width = resampler->destinationWidth;
    DWORD first;
    DWORD last;
    DWORD row;
    DWORD slot;

    if (part >= resampler->partCount)
    {
        return;
    }

    first = resampler->destinationHeight * part / resampler->partCount;
    last = resampler->destinationHeight * (part + 1) / resampler->partCount;
    for (slot = 0; slot < kTaps; ++slot)
    {
        rows->tags[slot] = -1;
    }

    for (row = first; row < last; ++row)
    {
        const DWORD* index = resampler->rowIndex + row * kTaps;
        const SHORT* weight = resampler->rowWeight + row * kTaps;
        DWORD* output = reinterpret_cast<DWORD*>(
            job->destination + static_cast<LONG>(row) * job->destinationPitch);
        const DWORD* row0;
        const DWORD* row1;
        const DWORD* row2;
        const DWORD* row3;
        DWORD column;

        // A row that takes a single source row is a copy of it.
        if (IsCopy(weight))
        {
            __movsd(
                reinterpret_cast<unsigned long*>(output),
                reinterpret_cast<const unsigned long*>(
                    GetRow(job, rows, index[0])),
                width);
            continue;
        }

        row0 = GetRow(job, rows, index[0]);
        row1 = GetRow(job, rows, index[1]);
        row2 = GetRow(job, rows, index[2]);
        if (g_useMmx)
        {
            __m64 split[2];

            // With a zero fourth weight, the fourth row does not matter.
            row3 = weight[3] == 0 ? row2 : GetRow(job, rows, index[3]);
            SplitWeights(weight, split);
            for (column = 0; column < width; ++column)
            {
                output[column] = CombineMmx(
                    row0[column], row1[column], row2[column], row3[column],
                    split);
            }

            continue;
        }

        row3 = GetRow(job, rows, index[3]);
        for (column = 0; column < width; ++column)
        {
            output[column] = Combine(
                row0[column], row1[column], row2[column], row3[column],
                weight);
        }
    }

    // The FPU (shared with MMX) must be usable again by the caller.
    if (g_useMmx)
    {
        _mm_empty();
    }
}

BOOL ResamplerInitialize(
    Resampler* resampler,
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight)
{
    DWORD count = WorkerPoolSize();
    DWORD index;
    DWORD slot;

    ResamplerShutdown(resampler);
    if (sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0)
    {
        return FALSE;
    }

    if (count > kResamplerMaxParts)
    {
        count = kResamplerMaxParts;
    }

    g_useMmx = CpuHasMmx();
    resampler->sourceWidth = sourceWidth;
    resampler->sourceHeight = sourceHeight;
    resampler->destinationWidth = destinationWidth;
    resampler->destinationHeight = destinationHeight;
    if (!BuildAxis(
            sourceWidth, destinationWidth,
            &resampler->columnIndex, &resampler->columnWeight) ||
        !BuildAxis(
            sourceHeight, destinationHeight,
            &resampler->rowIndex, &resampler->rowWeight))
    {
        ResamplerShutdown(resampler);
        return FALSE;
    }

    for (index = 0; index < count; ++index)
    {
        for (slot = 0; slot < kTaps; ++slot)
        {
            resampler->parts[index].rows[slot] = reinterpret_cast<DWORD*>(
                Allocate(destinationWidth * sizeof(DWORD)));
            if (resampler->parts[index].rows[slot] == NULL)
            {
                ResamplerShutdown(resampler);
                return FALSE;
            }
        }
    }

    resampler->partCount = count;
    return TRUE;
}

void ResamplerShutdown(Resampler* resampler)
{
    DWORD index;
    DWORD slot;

    for (index = 0; index < kResamplerMaxParts; ++index)
    {
        for (slot = 0; slot < kTaps; ++slot)
        {
            Free(resampler->parts[index].rows[slot]);
            resampler->parts[index].rows[slot] = NULL;
        }
    }

    Free(resampler->columnIndex);
    Free(resampler->columnWeight);
    Free(resampler->rowIndex);
    Free(resampler->rowWeight);
    resampler->columnIndex = NULL;
    resampler->columnWeight = NULL;
    resampler->rowIndex = NULL;
    resampler->rowWeight = NULL;
    resampler->partCount = 0;
    resampler->sourceWidth = 0;
    resampler->sourceHeight = 0;
    resampler->destinationWidth = 0;
    resampler->destinationHeight = 0;
}

BOOL ResamplerIsReady(const Resampler* resampler)
{
    return resampler->partCount != 0;
}

void ResamplerScaleIndexed(
    Resampler* resampler,
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    const HazeFrame* haze,
    BYTE* destination,
    LONG destinationPitch)
{
    ScaleJob job;

    if (resampler->partCount == 0)
    {
        return;
    }

    job.resampler = resampler;
    job.source = source;
    job.sourcePitch = sourcePitch;
    job.rgb = rgb;
    job.palette = NULL;
    job.haze = haze;
    job.hazePalette = NULL;
    if (g_useMmx)
    {
        static __m64 unpacked[256];
        static __m64 unpackedHaze[kHazeTableSize];
        DWORD index;

        for (index = 0; index < 256; ++index)
        {
            unpacked[index] = Unpack(rgb[index]);
        }

        if (haze != NULL)
        {
            for (index = 0; index < kHazeTableSize; ++index)
            {
                unpackedHaze[index] = Unpack(haze->rgb[index]);
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
