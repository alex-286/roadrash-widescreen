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

static DWORD g_destinationWidth = 0;
static DWORD g_destinationHeight = 0;
static DWORD* g_columns = NULL;
static DWORD* g_rows = NULL;

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
    DWORD first = g_destinationHeight * part / parts;
    DWORD last = g_destinationHeight * (part + 1) / parts;
    DWORD row;

    for (row = first; row < last; ++row)
    {
        DWORD* output = reinterpret_cast<DWORD*>(
            job->destination + static_cast<LONG>(row) * job->destinationPitch);
        const BYTE* line;
        const DWORD* columns;
        const DWORD* rgb;
        DWORD column;

        // A row from the same source row as the previous one is a copy.
        if (row > first && g_rows[row] == g_rows[row - 1])
        {
            __movsd(
                reinterpret_cast<unsigned long*>(output),
                reinterpret_cast<const unsigned long*>(
                    job->destination +
                        static_cast<LONG>(row - 1) * job->destinationPitch),
                g_destinationWidth);
            continue;
        }

        line = job->source + static_cast<LONG>(g_rows[row]) * job->sourcePitch;
        columns = g_columns;
        rgb = job->rgb;
        for (column = 0; column < g_destinationWidth; ++column)
        {
            output[column] = rgb[line[columns[column]]];
        }
    }
}

BOOL NearestInitialize(
    DWORD sourceWidth,
    DWORD sourceHeight,
    DWORD destinationWidth,
    DWORD destinationHeight)
{
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

    g_destinationWidth = destinationWidth;
    g_destinationHeight = destinationHeight;
    return TRUE;
}

void NearestShutdown()
{
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
