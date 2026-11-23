/**
 * Detection of the processor features the frame filters can use.
 *
 * @file cpu_features.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_CPU_FEATURES_H
#define ROAD_RASH_CPU_FEATURES_H

#include <windows.h>

/**
 * Tells whether the processor has MMX.
 *
 * Every Pentium MMX, Pentium II and later processor has it, including the
 * Pentium 4. MMX shares the FPU state, so Windows 95 and 98 save it like
 * the FPU; no operating system support is needed. The answer is read once
 * with CPUID and cached; a build with ROAD_RASH_NO_MMX always answers
 * FALSE.
 *
 * @return TRUE when the processor has MMX.
 */
BOOL CpuHasMmx();

#endif
