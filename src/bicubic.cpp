#include "bicubic.h"
#include "worker_pool.h"

#include <intrin.h>

#pragma intrinsic(__movsd)

static const DWORD kMaxParts = 8;
static const DWORD kTaps = 4;
// Weights are fixed point with 10 fractional bits; they sum to kOne.
static const LONG kWeightBits = 10;
static const LONG kOne = 1 << kWeightBits;

// Per output column or row: four source pixels and their weights.
struct AxisTable
{
    DWORD* index;
    SHORT* weight;
};

// Each part keeps the last four horizontally filtered source rows, in slot
// row % 4. The four rows an output row needs are consecutive (or repeat the
// edge row), so they never share a slot.
struct PartRows
{
    DWORD* rows[kTaps];
    LONG tags[kTaps];
};

struct ScaleJob
{
    const BYTE* source;
    LONG sourcePitch;
    const DWORD* rgb;
    BYTE* destination;
    LONG destinationPitch;
};

static DWORD g_destinationWidth = 0;
static DWORD g_destinationHeight = 0;
static AxisTable g_columns;
static AxisTable g_rows;
static PartRows g_parts[kMaxParts];
static DWORD g_partCount = 0;

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
static void ComputeWeights(LONG s, SHORT* weights)
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

// Maps each destination pixel center to a source position in 16.16 fixed
// point and stores the four clamped source pixels around it.
static BOOL BuildAxis(DWORD source, DWORD destination, AxisTable* table)
{
    DWORD position;

    table->index = reinterpret_cast<DWORD*>(
        Allocate(destination * kTaps * sizeof(DWORD)));
    table->weight = reinterpret_cast<SHORT*>(
        Allocate(destination * kTaps * sizeof(SHORT)));
    if (table->index == NULL || table->weight == NULL)
    {
        return FALSE;
    }

    for (position = 0; position < destination; ++position)
    {
        LONG sample = MulDiv(
            static_cast<int>(2 * position + 1),
            static_cast<int>(source * 32768),
            static_cast<int>(destination)) - 32768;
        LONG first = (sample >> 16) - 1;
        DWORD tap;

        ComputeWeights((sample & 0xFFFF) >> 8, table->weight + position * kTaps);
        for (tap = 0; tap < kTaps; ++tap)
        {
            LONG index = first + static_cast<LONG>(tap);

            if (index < 0)
            {
                index = 0;
            }
            else if (index >= static_cast<LONG>(source))
            {
                index = static_cast<LONG>(source) - 1;
            }

            table->index[position * kTaps + tap] = static_cast<DWORD>(index);
        }
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

// True when the weights select the second tap alone. Small opposite weights
// on the outer taps can cancel out, so all of them are checked.
static __forceinline BOOL IsCopy(const SHORT* weights)
{
    return weights[0] == 0 && weights[2] == 0 && weights[3] == 0;
}

static void FilterRow(const ScaleJob* job, DWORD sourceRow, DWORD* output)
{
    const BYTE* source = job->source +
        static_cast<LONG>(sourceRow) * job->sourcePitch;
    const DWORD* rgb = job->rgb;
    const DWORD* index = g_columns.index;
    const SHORT* weight = g_columns.weight;
    DWORD column;

    for (column = 0; column < g_destinationWidth; ++column)
    {
        // A sample on a source pixel center needs no blending.
        output[column] = IsCopy(weight) ?
            rgb[source[index[1]]] :
            Combine(
                rgb[source[index[0]]],
                rgb[source[index[1]]],
                rgb[source[index[2]]],
                rgb[source[index[3]]],
                weight);
        index += kTaps;
        weight += kTaps;
    }
}

static const DWORD* GetRow(const ScaleJob* job, PartRows* part, DWORD row)
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
    PartRows* rows = &g_parts[part];
    DWORD first;
    DWORD last;
    DWORD row;
    DWORD slot;

    if (part >= g_partCount)
    {
        return;
    }

    first = g_destinationHeight * part / g_partCount;
    last = g_destinationHeight * (part + 1) / g_partCount;
    for (slot = 0; slot < kTaps; ++slot)
    {
        rows->tags[slot] = -1;
    }

    for (row = first; row < last; ++row)
    {
        const DWORD* index = g_rows.index + row * kTaps;
        const SHORT* weight = g_rows.weight + row * kTaps;
        DWORD* output = reinterpret_cast<DWORD*>(
            job->destination + static_cast<LONG>(row) * job->destinationPitch);
        const DWORD* row0;
        const DWORD* row1;
        const DWORD* row2;
        const DWORD* row3;
        DWORD column;

        // A row on a source row center is a copy of that filtered row.
        if (IsCopy(weight))
        {
            __movsd(
                reinterpret_cast<unsigned long*>(output),
                reinterpret_cast<const unsigned long*>(
                    GetRow(job, rows, index[1])),
                g_destinationWidth);
            continue;
        }

        row0 = GetRow(job, rows, index[0]);
        row1 = GetRow(job, rows, index[1]);
        row2 = GetRow(job, rows, index[2]);
        row3 = GetRow(job, rows, index[3]);
        for (column = 0; column < g_destinationWidth; ++column)
        {
            output[column] = Combine(
                row0[column], row1[column], row2[column], row3[column],
                weight);
        }
    }
}

BOOL BicubicInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight)
{
    DWORD count = WorkerPoolSize();
    DWORD index;
    DWORD slot;

    BicubicShutdown();
    if (sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0)
    {
        return FALSE;
    }

    if (count > kMaxParts)
    {
        count = kMaxParts;
    }

    g_destinationWidth = destinationWidth;
    g_destinationHeight = destinationHeight;
    if (!BuildAxis(sourceWidth, destinationWidth, &g_columns) ||
        !BuildAxis(sourceHeight, destinationHeight, &g_rows))
    {
        BicubicShutdown();
        return FALSE;
    }

    for (index = 0; index < count; ++index)
    {
        for (slot = 0; slot < kTaps; ++slot)
        {
            g_parts[index].rows[slot] = reinterpret_cast<DWORD*>(
                Allocate(destinationWidth * sizeof(DWORD)));
            if (g_parts[index].rows[slot] == NULL)
            {
                BicubicShutdown();
                return FALSE;
            }
        }
    }

    g_partCount = count;
    return TRUE;
}

void BicubicShutdown()
{
    DWORD index;
    DWORD slot;

    for (index = 0; index < kMaxParts; ++index)
    {
        for (slot = 0; slot < kTaps; ++slot)
        {
            Free(g_parts[index].rows[slot]);
            g_parts[index].rows[slot] = NULL;
        }
    }

    FreeAxis(&g_columns);
    FreeAxis(&g_rows);
    g_partCount = 0;
    g_destinationWidth = 0;
    g_destinationHeight = 0;
}

BOOL BicubicIsReady()
{
    return g_partCount != 0;
}

void BicubicScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
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
    job.destination = destination;
    job.destinationPitch = destinationPitch;
    WorkerPoolRun(ScaleTask, &job);
}
