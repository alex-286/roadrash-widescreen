// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "nearest.h"
#include "worker_pool.h"

#include <intrin.h>

#pragma intrinsic(__movsd)

struct ScaleJob
{
    const BYTE* source;
    LONG sourcePitch;
    const DWORD* rgb;
    BYTE* destination;
    LONG destinationPitch;
};

static const DWORD kMaxParts = 8;

static DWORD g_destinationWidth = 0;
static DWORD g_destinationHeight = 0;
static DWORD* g_columns = NULL;
static DWORD* g_rows = NULL;
// One converted row per worker part. Output rows are copied from it, never
// from the destination, which may be video memory (slow to read).
static DWORD* g_rowBuffers[kMaxParts];
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

// The source pixel under each destination pixel center.
static DWORD* BuildAxis(DWORD source, DWORD destination)
{
    DWORD* table = reinterpret_cast<DWORD*>(
        Allocate(destination * sizeof(DWORD)));
    DWORD position;

    if (table == NULL)
    {
        return NULL;
    }

    for (position = 0; position < destination; ++position)
    {
        // Below 2^32 for any display size (2 * 16384 * 2560).
        DWORD pixel = (2 * position + 1) * source / (2 * destination);

        table[position] = pixel < source ? pixel : source - 1;
    }

    return table;
}

static void ScaleTask(DWORD part, DWORD parts, void* context)
{
    const ScaleJob* job = reinterpret_cast<const ScaleJob*>(context);
    DWORD first;
    DWORD last;
    DWORD row;
    DWORD* buffer;
    LONG converted = -1;

    if (part >= g_partCount)
    {
        return;
    }

    if (parts > g_partCount)
    {
        parts = g_partCount;
    }

    first = g_destinationHeight * part / parts;
    last = g_destinationHeight * (part + 1) / parts;
    buffer = g_rowBuffers[part];
    for (row = first; row < last; ++row)
    {
        // Consecutive rows from the same source row reuse the buffer.
        if (static_cast<LONG>(g_rows[row]) != converted)
        {
            const BYTE* line = job->source +
                static_cast<LONG>(g_rows[row]) * job->sourcePitch;
            const DWORD* columns = g_columns;
            const DWORD* rgb = job->rgb;
            DWORD column;

            for (column = 0; column < g_destinationWidth; ++column)
            {
                buffer[column] = rgb[line[columns[column]]];
            }

            converted = static_cast<LONG>(g_rows[row]);
        }

        __movsd(
            reinterpret_cast<unsigned long*>(
                job->destination + static_cast<LONG>(row) * job->destinationPitch),
            reinterpret_cast<const unsigned long*>(buffer),
            g_destinationWidth);
    }
}

BOOL NearestInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight)
{
    DWORD count;
    DWORD index;

    NearestShutdown();
    if (sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0)
    {
        return FALSE;
    }

    g_columns = BuildAxis(sourceWidth, destinationWidth);
    g_rows = BuildAxis(sourceHeight, destinationHeight);
    if (g_columns == NULL || g_rows == NULL)
    {
        NearestShutdown();
        return FALSE;
    }

    count = WorkerPoolSize();
    if (count > kMaxParts)
    {
        count = kMaxParts;
    }

    for (index = 0; index < count; ++index)
    {
        g_rowBuffers[index] = reinterpret_cast<DWORD*>(
            Allocate(destinationWidth * sizeof(DWORD)));
        if (g_rowBuffers[index] == NULL)
        {
            NearestShutdown();
            return FALSE;
        }
    }

    g_partCount = count;

    g_destinationWidth = destinationWidth;
    g_destinationHeight = destinationHeight;
    return TRUE;
}

void NearestShutdown()
{
    DWORD index;

    for (index = 0; index < kMaxParts; ++index)
    {
        Free(g_rowBuffers[index]);
        g_rowBuffers[index] = NULL;
    }

    g_partCount = 0;
    Free(g_columns);
    Free(g_rows);
    g_columns = NULL;
    g_rows = NULL;
    g_destinationWidth = 0;
    g_destinationHeight = 0;
}

BOOL NearestIsReady()
{
    return g_destinationWidth != 0;
}

void NearestScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch)
{
    ScaleJob job;

    if (!NearestIsReady())
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
