// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "render_scale.h"
#include "road_rash_patch.h"

#include <intrin.h>

// String intrinsics: plain fill and copy loops would compile to memset and
// memcpy, which this DLL does not have (no C runtime).
#pragma intrinsic(__stosb, __movsb)

// The game computes the 3D world in a 320x240 space with 16.16 fixed point
// and converts every quad corner to pixels in FUN_0040FFC0:
// (corner >> shift) & mask. FUN_00410160 sets shift 15 for HiRes (640x480);
// nothing else reads the shift. Shift 14 puts the corners into a 1280x960
// space (twice the virtual width in widescreen) and shift 13 into 2560x1920,
// so the game's own rasterizer draws the world at 2x or 4x. 3x is no power
// of two: FUN_0040FFC0 (called only from 0x40FF5A and 0x40FF87, the quad
// list walk) then jumps to HookedCorners, which multiplies by 3 instead.
//
// The textured quad rasterizer (FUN_004180B0) builds the left and right edge
// of each quad in two fixed tables of 480 rows x 12 bytes (x, u, v) at
// 0x482BF8 and 0x484280, and FUN_004184A0 clamps every edge to row 479. The
// tables move into this DLL with 1920 rows, and the clamp follows the scale
// of each race. The flat-colour rasterizer (FUN_0044D620) has no per-row
// tables and only clips against the clip rectangle.
//
// The scale is set for each race (RenderScaleSetActive), when its display
// mode is known; the install prepares everything up to the largest scale
// the race may use.

static const DWORD kImageBase = 0x00400000;
static const DWORD kOriginalRows = 480;
static const DWORD kMaxRows = kOriginalRows * kMaxRenderScale;
static const DWORD kEdgeRowSize = 12;

static const DWORD kLeftEdgesVa = 0x00482BF8;
static const DWORD kRightEdgesVa = 0x00484280;
static const DWORD kEdgeClampRva = 0x00018584;

static const DWORD kShiftStoreRva = 0x000101BC;
static const DWORD kShiftRva = 0x00081FA8;
static const DWORD kMaskRva = 0x00081FF4;
static const DWORD kHiResShift = 15;

// FUN_0040FFC0 (fastcall: quad list node, quad) and the function it ends
// with, FUN_00410080 (fastcall: quad, node).
static const DWORD kCornersRva = 0x0000FFC0;
static const DWORD kCornersTailRva = 0x00010080;
static const DWORD kQuadCornersOffset = 0x44;
static const DWORD kNodeCornersOffset = 0x18;
static const DWORD kCornerCount = 8;

struct EdgeSite
{
    DWORD rva;
    BYTE prefix[2];
    BYTE prefixLength;
    DWORD target;
};

// Every instruction of the rasterizer that addresses the edge tables.
static const EdgeSite kEdgeSites[] =
{
    { 0x000183B9, { 0xB9, 0x00 }, 1, 0x00484280 },  // mov ecx, right
    { 0x000183D8, { 0xB9, 0x00 }, 1, 0x00482BF8 },  // mov ecx, left
    { 0x00018431, { 0xB9, 0x00 }, 1, 0x00482BF8 },  // mov ecx, left
    { 0x00018458, { 0xB9, 0x00 }, 1, 0x00484280 },  // mov ecx, right
    { 0x00018781, { 0x8B, 0x96 }, 2, 0x00482BF8 },  // mov edx, [esi + left]
    { 0x00018787, { 0x8D, 0x8E }, 2, 0x00482BF8 },  // lea ecx, [esi + left]
    { 0x00018798, { 0x8B, 0xBE }, 2, 0x00484280 },  // mov edi, [esi + right]
    { 0x0001879E, { 0x8D, 0x9E }, 2, 0x00484280 },  // lea ebx, [esi + right]
    { 0x0001883A, { 0x8B, 0x9E }, 2, 0x00484284 },  // mov ebx, [esi + right.u]
    { 0x00018840, { 0x8B, 0x86 }, 2, 0x00484288 },  // mov eax, [esi + right.v]
    { 0x0001884E, { 0x8B, 0x86 }, 2, 0x00482BFC },  // mov eax, [esi + left.u]
    { 0x0001886E, { 0x8B, 0x86 }, 2, 0x00482C00 },  // mov eax, [esi + left.v]
};

static const DWORD kEdgeSiteCount = sizeof(kEdgeSites) / sizeof(kEdgeSites[0]);

typedef void (__fastcall *CornersTailFn)(BYTE* quad, BYTE* node);

static DWORD g_leftEdges[kMaxRows * kEdgeRowSize / sizeof(DWORD)];
static DWORD g_rightEdges[kMaxRows * kEdgeRowSize / sizeof(DWORD)];
static DWORD g_scaleBase = 0;
static DWORD g_maxScale = 1;
static BOOL g_scaleInstalled = FALSE;
// 3x: HookedCorners multiplies while the HiRes shift is set.
static BOOL g_tripled = FALSE;

static void LogScale(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

static DWORD ReadDword(const BYTE* bytes)
{
    return static_cast<DWORD>(bytes[0]) |
        (static_cast<DWORD>(bytes[1]) << 8) |
        (static_cast<DWORD>(bytes[2]) << 16) |
        (static_cast<DWORD>(bytes[3]) << 24);
}

static BOOL WriteCode(DWORD rva, const BYTE* bytes, DWORD length)
{
    BYTE* destination = reinterpret_cast<BYTE*>(g_scaleBase + rva);
    DWORD oldProtection;
    DWORD ignoredProtection;
    DWORD index;

    if (!VirtualProtect(destination, length, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return FALSE;
    }

    for (index = 0; index < length; ++index)
    {
        destination[index] = bytes[index];
    }

    VirtualProtect(destination, length, oldProtection, &ignoredProtection);
    FlushInstructionCache(GetCurrentProcess(), destination, length);
    return TRUE;
}

static BOOL WriteCodeDword(DWORD rva, DWORD value)
{
    BYTE bytes[4];

    bytes[0] = static_cast<BYTE>(value);
    bytes[1] = static_cast<BYTE>(value >> 8);
    bytes[2] = static_cast<BYTE>(value >> 16);
    bytes[3] = static_cast<BYTE>(value >> 24);
    return WriteCode(rva, bytes, sizeof(bytes));
}

// The address an edge site should use instead of its original target.
static DWORD RelocatedTarget(DWORD target)
{
    DWORD tableBytes = kOriginalRows * kEdgeRowSize;

    if (target >= kLeftEdgesVa && target < kLeftEdgesVa + tableBytes)
    {
        return reinterpret_cast<DWORD>(g_leftEdges) + (target - kLeftEdgesVa);
    }

    return reinterpret_cast<DWORD>(g_rightEdges) + (target - kRightEdgesVa);
}

// The shift that converts HiRes corners at a power-of-two scale (3x keeps
// the HiRes shift and multiplies in HookedCorners).
static DWORD ShiftForScale(DWORD scale)
{
    switch (scale)
    {
    case 2:
        return kHiResShift - 1;
    case 4:
        return kHiResShift - 2;
    default:
        return kHiResShift;
    }
}

LONG RenderScaleFixed(LONG value, DWORD scale)
{
    // value = whole * 2^15 + part with 0 <= part < 2^15 (the shift of a
    // negative value rounds down), so value * scale / 2^15 rounded down is
    // whole * scale + part * scale / 2^15.
    return (value >> kHiResShift) * static_cast<LONG>(scale) +
        static_cast<LONG>(((static_cast<DWORD>(value) & 0x7FFF) * scale) >> kHiResShift);
}

// Replaces FUN_0040FFC0 (whose first instruction jumps here) for 3x: the
// eight corner coordinates of the quad become pixels, then FUN_00410080
// runs as at the end of the original.
static void __fastcall HookedCorners(BYTE* node, BYTE* quad)
{
    const LONG* corners = reinterpret_cast<const LONG*>(quad + kQuadCornersOffset);
    LONG* pixels = reinterpret_cast<LONG*>(node + kNodeCornersOffset);
    DWORD shift = *reinterpret_cast<const DWORD*>(g_scaleBase + kShiftRva) & 0x1F;
    LONG mask = *reinterpret_cast<const LONG*>(g_scaleBase + kMaskRva);
    DWORD index;

    if (g_tripled && shift == kHiResShift)
    {
        for (index = 0; index < kCornerCount; ++index)
        {
            pixels[index] = RenderScaleFixed(corners[index], 3) & mask;
        }
    }
    else
    {
        for (index = 0; index < kCornerCount; ++index)
        {
            pixels[index] = (corners[index] >> shift) & mask;
        }
    }

    reinterpret_cast<CornersTailFn>(g_scaleBase + kCornersTailRva)(quad, node);
}

BOOL RenderScaleInstall(DWORD gameBase, DWORD maxScale)
{
    const BYTE* code;
    DWORD index;
    DWORD relocation = gameBase - kImageBase;
    static const BYTE clamp[5] = { 0xBA, 0xDF, 0x01, 0x00, 0x00 };
    static const BYTE shiftStore[2] = { 0xC7, 0x05 };
    // push ebx / push esi / mov eax, [edx + 0x44]
    static const BYTE cornersStart[5] = { 0x53, 0x56, 0x8B, 0x42, 0x44 };

    if (g_scaleInstalled)
    {
        return TRUE;
    }

    if (maxScale < 2 || maxScale > kMaxRenderScale)
    {
        return FALSE;
    }

    g_scaleBase = gameBase;

    // Check every site first, so nothing is changed on a mismatch.
    for (index = 0; index < kEdgeSiteCount; ++index)
    {
        const EdgeSite& site = kEdgeSites[index];

        code = reinterpret_cast<const BYTE*>(gameBase + site.rva);
        if (code[0] != site.prefix[0] ||
            (site.prefixLength == 2 && code[1] != site.prefix[1]) ||
            ReadDword(code + site.prefixLength) != site.target + relocation)
        {
            LogScale("RenderScale stays at 1: unexpected code in the world rasterizer.");
            return FALSE;
        }
    }

    code = reinterpret_cast<const BYTE*>(gameBase + kEdgeClampRva);
    for (index = 0; index < sizeof(clamp); ++index)
    {
        if (code[index] != clamp[index])
        {
            LogScale("RenderScale stays at 1: unexpected edge clamp in the world rasterizer.");
            return FALSE;
        }
    }

    // mov dword ptr [0x481FA8], 15
    code = reinterpret_cast<const BYTE*>(gameBase + kShiftStoreRva);
    if (code[0] != shiftStore[0] ||
        code[1] != shiftStore[1] ||
        ReadDword(code + 2) != gameBase + kShiftRva ||
        ReadDword(code + 6) != kHiResShift)
    {
        LogScale("RenderScale stays at 1: unexpected code in the HiRes setup.");
        return FALSE;
    }

    if (maxScale >= 3)
    {
        code = reinterpret_cast<const BYTE*>(gameBase + kCornersRva);
        for (index = 0; index < sizeof(cornersStart); ++index)
        {
            if (code[index] != cornersStart[index])
            {
                LogScale("RenderScale stays at 1: unexpected code in the corner conversion.");
                return FALSE;
            }
        }
    }

    for (index = 0; index < kEdgeSiteCount; ++index)
    {
        const EdgeSite& site = kEdgeSites[index];

        if (!WriteCodeDword(
                site.rva + site.prefixLength,
                RelocatedTarget(site.target)))
        {
            LogScale("RenderScale: could not move the edge tables; the game may be unstable.");
            return FALSE;
        }
    }

    // 3x needs the corner conversion of this DLL (it also serves the other
    // scales, so a race may use any of them).
    if (maxScale >= 3)
    {
        BYTE jump[5];

        jump[0] = 0xE9;
        *reinterpret_cast<DWORD*>(jump + 1) =
            reinterpret_cast<DWORD>(HookedCorners) - (gameBase + kCornersRva + 5);
        if (!WriteCode(kCornersRva, jump, sizeof(jump)))
        {
            LogScale("RenderScale: could not change the corner conversion; the game may be unstable.");
            return FALSE;
        }
    }

    g_maxScale = maxScale;
    g_scaleInstalled = TRUE;

    {
        CHAR message[80];

        wsprintfA(
            message,
            "RenderScale: the world rasterizer accepts up to %lu lines.",
            kOriginalRows * maxScale);
        LogScale(message);
    }

    return TRUE;
}

void RenderScaleDrawSky(
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight,
    const BYTE* first, LONG firstPitch,
    const BYTE* second, LONG secondPitch,
    LONG half, LONG period, LONG rows, DWORD scale,
    LONG scroll, LONG left, LONG top)
{
    LONG factor = static_cast<LONG>(scale);
    LONG scaledPeriod = period * factor;
    LONG start;
    LONG count;
    LONG row;

    if (half <= 0 || factor <= 0 || scaledPeriod <= 0 || left < 0 || left >= destWidth)
    {
        return;
    }

    // Panorama position, in enlarged pixels, of the first column.
    start = -scroll % scaledPeriod;
    if (start < 0)
    {
        start += scaledPeriod;
    }

    count = destWidth - left;
    for (row = 0; row < rows; ++row)
    {
        LONG y = top + row * factor;
        const BYTE* rowFirst = first + row * firstPitch;
        const BYTE* rowSecond = second + row * secondPitch;
        BYTE* out;
        LONG column = start / factor;
        LONG step = start % factor;
        LONG copy;
        LONG x;

        if (y + factor <= 0)
        {
            continue;
        }

        if (y >= destHeight)
        {
            break;
        }

        if (y < 0)
        {
            continue;
        }

        out = dest + y * destPitch + left;
        for (x = 0; x < count; ++x)
        {
            out[x] = column < half ?
                rowFirst[column] : rowSecond[column - half];
            if (++step == factor)
            {
                step = 0;
                if (++column == period)
                {
                    column = 0;
                }
            }
        }

        for (copy = 1; copy < factor && y + copy < destHeight; ++copy)
        {
            __movsb(out + copy * destPitch, out, static_cast<size_t>(count));
        }
    }
}

void RenderScalePrepareHud(
    BYTE* hud, LONG hudPitch, LONG width, LONG height, BYTE* before)
{
    LONG y;

    for (y = 0; y < height; ++y)
    {
        BYTE* row = hud + y * hudPitch;
        BYTE* copy = before + y * width;
        LONG x = 0;

        for (; x + 4 <= width; x += 4)
        {
            DWORD value = *reinterpret_cast<DWORD*>(row + x) ^ 0x80808080;

            *reinterpret_cast<DWORD*>(row + x) = value;
            *reinterpret_cast<DWORD*>(copy + x) = value;
        }

        for (; x < width; ++x)
        {
            row[x] ^= 0x80;
            copy[x] = row[x];
        }
    }
}

void RenderScaleComposeHud(
    const BYTE* hud, LONG hudPitch, const BYTE* before,
    LONG width, LONG height, DWORD scale,
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight)
{
    LONG factor = static_cast<LONG>(scale);
    LONG y;

    for (y = 0; y < height && (y + 1) * factor <= destHeight; ++y)
    {
        const BYTE* row = hud + y * hudPitch;
        const BYTE* old = before + y * width;
        BYTE* top = dest + y * factor * destPitch;
        LONG x = 0;

        while (x < width && (x + 1) * factor <= destWidth)
        {
            if (x + 4 <= width &&
                *reinterpret_cast<const DWORD*>(row + x) ==
                    *reinterpret_cast<const DWORD*>(old + x))
            {
                x += 4;
                continue;
            }

            if (row[x] != old[x])
            {
                BYTE value = row[x];
                BYTE* block = top + x * factor;
                LONG blockY;

                for (blockY = 0; blockY < factor; ++blockY)
                {
                    __stosb(block, value, static_cast<size_t>(factor));
                    block += destPitch;
                }
            }

            ++x;
        }
    }
}

void RenderScaleSetActive(DWORD scale)
{
    DWORD shift;
    DWORD* current;

    if (!g_scaleInstalled)
    {
        return;
    }

    if (scale < 1)
    {
        scale = 1;
    }

    if (scale > g_maxScale)
    {
        scale = g_maxScale;
    }

    shift = ShiftForScale(scale);
    g_tripled = scale == 3;

    // The last row the textured rasterizer may write.
    WriteCodeDword(kEdgeClampRva + 1, kOriginalRows * scale - 1);

    // For later HiRes setups, and for the one that may have run already.
    WriteCodeDword(kShiftStoreRva + 6, shift);
    current = reinterpret_cast<DWORD*>(g_scaleBase + kShiftRva);
    if (*current <= kHiResShift && *current >= ShiftForScale(kMaxRenderScale))
    {
        *current = shift;
    }
}
