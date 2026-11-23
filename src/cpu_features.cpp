/**
 * Detection of the processor features the frame filters can use, with
 * CPUID.
 *
 * @file cpu_features.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cpu_features.h"

#include <intrin.h>

BOOL CpuHasMmx()
{
#ifdef ROAD_RASH_NO_MMX
    return FALSE;
#else
    static LONG cached = -1;

    if (cached < 0)
    {
        int registers[4];

        __cpuid(registers, 0);
        if (registers[0] >= 1)
        {
            __cpuid(registers, 1);
            cached = (registers[3] >> 23) & 1;
        }
        else
        {
            cached = 0;
        }
    }

    return cached != 0;
#endif
}
