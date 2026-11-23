/**
 * A small pool of worker threads for splitting a frame filter into parts.
 *
 * The calling thread always runs part 0, so a pool of size 1 starts no
 * thread at all (single-processor systems such as Windows 98).
 *
 * @file worker_pool.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_WORKER_POOL_H
#define ROAD_RASH_WORKER_POOL_H

#include <windows.h>

/**
 * One part of a task, run by the pool.
 *
 * @param [in] part The part to work on, 0 to parts - 1.
 * @param [in] parts The number of parts the task is split into.
 * @param [in,out] context The context given to WorkerPoolRun.
 */
typedef void (*WorkerTask)(DWORD part, DWORD parts, void* context);

/**
 * Starts min(processors, maxThreads, 8) - 1 worker threads.
 *
 * A running pool is stopped first. Falls back to size 1 when threads
 * cannot be created; a partly started pool runs with the threads it got.
 *
 * @param [in] maxThreads The largest number of parts, including the calling
 *             thread.
 */
void WorkerPoolStart(DWORD maxThreads);

/** Ends the worker threads; the pool is size 1 afterwards. */
void WorkerPoolStop();

/**
 * Gives the number of parts a task is split into.
 *
 * @return The number of worker threads plus the calling thread.
 */
DWORD WorkerPoolSize();

/**
 * Runs a task in every part and returns when all parts are done.
 *
 * @param [in] task Called as task(part, WorkerPoolSize(), context).
 * @param [in,out] context Passed to every part.
 */
void WorkerPoolRun(WorkerTask task, void* context);

#endif
