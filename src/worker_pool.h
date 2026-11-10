#ifndef ROAD_RASH_WORKER_POOL_H
#define ROAD_RASH_WORKER_POOL_H

#include <windows.h>

// A small pool of worker threads for splitting a frame filter into parts.
// The calling thread always runs part 0, so a pool of size 1 starts no
// thread at all (single-processor systems such as Windows 98).
typedef void (*WorkerTask)(DWORD part, DWORD parts, void* context);

// Starts min(processors, maxThreads, 8) - 1 worker threads. Falls back to
// size 1 when threads cannot be created.
void WorkerPoolStart(DWORD maxThreads);
void WorkerPoolStop();

// The number of parts a task is split into, including the calling thread.
DWORD WorkerPoolSize();

// Runs task(part, WorkerPoolSize(), context) for every part and returns when
// all parts are done.
void WorkerPoolRun(WorkerTask task, void* context);

#endif
