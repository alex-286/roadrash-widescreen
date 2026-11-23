/**
 * A small pool of worker threads for splitting a frame filter into parts.
 *
 * Each worker waits for its start event, runs its part of the current task
 * and sets its done event; the calling thread runs part 0 and then waits
 * for all done events.
 *
 * @file worker_pool.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "worker_pool.h"

/** The largest pool size, including the calling thread. */
static const DWORD kMaxThreads = 8;
/** The stack size of a worker thread in bytes. */
static const DWORD kWorkerStackSize = 64 * 1024;

/** One worker thread. */
struct Worker
{
    /** The thread, or NULL. */
    HANDLE thread;
    /**
     * Auto-reset event that starts the thread on the current task (or ends it
     * when g_quit is set).
     */
    HANDLE start;
};

/** The worker threads; entry 0 (the calling thread) is unused. */
static Worker g_workers[kMaxThreads];
/**
 * Auto-reset events, set by each worker when its part is done; entry 0 is
 * unused.
 */
static HANDLE g_doneEvents[kMaxThreads];
/** The number of parts, including the calling thread. */
static DWORD g_size = 1;
/** Nonzero while WorkerPoolStop ends the threads. */
static volatile LONG g_quit = 0;

/**
 * The current task.
 *
 * It is written before the start events are set, which orders the writes
 * before the workers read them.
 */
static WorkerTask g_task = NULL;
/** The context of the current task; written like g_task. */
static void* g_context = NULL;

/**
 * The thread function of a worker: runs its part of every task until
 * g_quit is set.
 *
 * @param [in] parameter The part the worker runs, as a pointer-sized value.
 * @return 0.
 */
static DWORD WINAPI WorkerMain(LPVOID parameter)
{
    DWORD part = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(parameter));

    for (;;)
    {
        WaitForSingleObject(g_workers[part].start, INFINITE);
        if (g_quit != 0)
        {
            break;
        }

        g_task(part, g_size, g_context);
        SetEvent(g_doneEvents[part]);
    }

    return 0;
}

void WorkerPoolStart(DWORD maxThreads)
{
    SYSTEM_INFO system;
    DWORD count;
    DWORD part;
    DWORD threadId;

    WorkerPoolStop();
    GetSystemInfo(&system);
    count = system.dwNumberOfProcessors;
    if (count > maxThreads)
    {
        count = maxThreads;
    }

    if (count > kMaxThreads)
    {
        count = kMaxThreads;
    }

    if (count < 2)
    {
        return;
    }

    for (part = 1; part < count; ++part)
    {
        g_workers[part].start = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_doneEvents[part] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (g_workers[part].start == NULL || g_doneEvents[part] == NULL)
        {
            break;
        }

        // Windows 9x fails CreateThread without a thread id pointer.
        g_workers[part].thread = CreateThread(
            NULL,
            kWorkerStackSize,
            WorkerMain,
            reinterpret_cast<LPVOID>(static_cast<DWORD_PTR>(part)),
            0,
            &threadId);
        if (g_workers[part].thread == NULL)
        {
            break;
        }

        g_size = part + 1;
    }

    // A partly started pool runs with the threads it got.
    if (part < count)
    {
        if (g_workers[part].start != NULL)
        {
            CloseHandle(g_workers[part].start);
        }

        if (g_doneEvents[part] != NULL)
        {
            CloseHandle(g_doneEvents[part]);
        }

        g_workers[part].start = NULL;
        g_doneEvents[part] = NULL;
    }
}

void WorkerPoolStop()
{
    DWORD part;

    g_quit = 1;
    for (part = 1; part < g_size; ++part)
    {
        SetEvent(g_workers[part].start);
        WaitForSingleObject(g_workers[part].thread, INFINITE);
        CloseHandle(g_workers[part].thread);
        CloseHandle(g_workers[part].start);
        CloseHandle(g_doneEvents[part]);
        g_workers[part].thread = NULL;
        g_workers[part].start = NULL;
        g_doneEvents[part] = NULL;
    }

    g_size = 1;
    g_quit = 0;
}

DWORD WorkerPoolSize()
{
    return g_size;
}

void WorkerPoolRun(WorkerTask task, void* context)
{
    DWORD part;

    g_task = task;
    g_context = context;
    for (part = 1; part < g_size; ++part)
    {
        SetEvent(g_workers[part].start);
    }

    task(0, g_size, context);
    if (g_size > 1)
    {
        WaitForMultipleObjects(g_size - 1, g_doneEvents + 1, TRUE, INFINITE);
    }
}
