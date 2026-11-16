// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_CPU_FEATURES_H
#define ROAD_RASH_CPU_FEATURES_H

#include <windows.h>

// TRUE when the processor has MMX (every Pentium MMX, Pentium II and later,
// including the Pentium 4). MMX shares the FPU state, so Windows 95 and 98
// save it like the FPU; no operating system support is needed.
BOOL CpuHasMmx();

#endif
