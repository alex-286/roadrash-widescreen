#include "worker_pool.h"

static const DWORD kMaxThreads = 8;
static const DWORD kWorkerStackSize = 64 * 1024;

struct Worker
{
    HANDLE thread;
    HANDLE start;
};

static Worker g_workers[kMaxThreads];
static HANDLE g_doneEvents[kMaxThreads];
static DWORD g_size = 1;
static volatile LONG g_quit = 0;

// The current task. It is written before the start events are set, which
// orders the writes before the workers read them.
static WorkerTask g_task = NULL;
static void* g_context = NULL;

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

        g_workers[part].thread = CreateThread(
            NULL,
            kWorkerStackSize,
            WorkerMain,
            reinterpret_cast<LPVOID>(static_cast<DWORD_PTR>(part)),
            0,
            NULL);
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
