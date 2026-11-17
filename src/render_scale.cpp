// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "render_scale.h"
#include "road_rash_patch.h"

// The game computes the 3D world in a 320x240 space with 16.16 fixed point
// and converts every quad corner to pixels in FUN_0040FFC0:
// (corner >> shift) & mask. FUN_00410160 sets shift 15 for HiRes (640x480).
// Shift 14 puts the corners into a 1280x960 space (twice the virtual width
// in widescreen), so the game's own rasterizer draws the world at twice the
// resolution.
//
// The textured quad rasterizer (FUN_004180B0) builds the left and right edge
// of each quad in two fixed tables of 480 rows x 12 bytes (x, u, v) at
// 0x482BF8 and 0x484280, and FUN_004184A0 clamps every edge to row 479. The
// tables move into this DLL with 960 rows and the clamp moves to row 959.
// The flat-colour rasterizer (FUN_0044D620) has no per-row tables and only
// clips against the clip rectangle.

static const DWORD kImageBase = 0x00400000;
static const DWORD kMaxRows = 960;
static const DWORD kEdgeRowSize = 12;
static const DWORD kOriginalRows = 480;

static const DWORD kLeftEdgesVa = 0x00482BF8;
static const DWORD kRightEdgesVa = 0x00484280;
static const DWORD kEdgeClampRva = 0x00018584;

static const DWORD kShiftStoreRva = 0x000101BC;
static const DWORD kShiftRva = 0x00081FA8;
static const DWORD kHiResShift = 15;
static const DWORD kDoubledShift = 14;

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

static DWORD g_leftEdges[kMaxRows * kEdgeRowSize / sizeof(DWORD)];
static DWORD g_rightEdges[kMaxRows * kEdgeRowSize / sizeof(DWORD)];
static DWORD g_scaleBase = 0;
static BOOL g_scaleInstalled = FALSE;

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

static BOOL WriteCodeDword(DWORD rva, DWORD value)
{
    BYTE* destination = reinterpret_cast<BYTE*>(g_scaleBase + rva);
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (!VirtualProtect(destination, 4, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return FALSE;
    }

    destination[0] = static_cast<BYTE>(value);
    destination[1] = static_cast<BYTE>(value >> 8);
    destination[2] = static_cast<BYTE>(value >> 16);
    destination[3] = static_cast<BYTE>(value >> 24);
    VirtualProtect(destination, 4, oldProtection, &ignoredProtection);
    FlushInstructionCache(GetCurrentProcess(), destination, 4);
    return TRUE;
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

BOOL RenderScaleInstall(DWORD gameBase)
{
    const BYTE* code;
    DWORD index;
    DWORD relocation = gameBase - kImageBase;
    static const BYTE clamp[5] = { 0xBA, 0xDF, 0x01, 0x00, 0x00 };
    static const BYTE shiftStore[2] = { 0xC7, 0x05 };

    if (g_scaleInstalled)
    {
        return TRUE;
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
            LogScale("RenderScale=2 stays off: unexpected code in the world rasterizer.");
            return FALSE;
        }
    }

    code = reinterpret_cast<const BYTE*>(gameBase + kEdgeClampRva);
    for (index = 0; index < sizeof(clamp); ++index)
    {
        if (code[index] != clamp[index])
        {
            LogScale("RenderScale=2 stays off: unexpected edge clamp in the world rasterizer.");
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
        LogScale("RenderScale=2 stays off: unexpected code in the HiRes setup.");
        return FALSE;
    }

    for (index = 0; index < kEdgeSiteCount; ++index)
    {
        const EdgeSite& site = kEdgeSites[index];

        if (!WriteCodeDword(
                site.rva + site.prefixLength,
                RelocatedTarget(site.target)))
        {
            LogScale("RenderScale=2: could not move the edge tables; the game may be unstable.");
            return FALSE;
        }
    }

    if (!WriteCodeDword(kEdgeClampRva + 1, kMaxRows - 1))
    {
        LogScale("RenderScale=2: could not raise the edge clamp; only 480 lines are drawn.");
        return FALSE;
    }

    g_scaleInstalled = TRUE;
    LogScale("RenderScale=2: the world rasterizer accepts 960 lines.");
    return TRUE;
}

void RenderScaleDrawSky(
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight,
    const BYTE* first, LONG firstPitch,
    const BYTE* second, LONG secondPitch,
    LONG half, LONG period, LONG rows,
    LONG scroll2, LONG left2, LONG top2)
{
    LONG period2 = period * 2;
    LONG start;
    LONG count;
    LONG row;

    if (half <= 0 || period2 <= 0 || left2 < 0 || left2 >= destWidth)
    {
        return;
    }

    // Panorama position at twice the resolution of the first column.
    start = -scroll2 % period2;
    if (start < 0)
    {
        start += period2;
    }

    count = destWidth - left2;
    for (row = 0; row < rows; ++row)
    {
        LONG y = top2 + row * 2;
        const BYTE* rowFirst = first + row * firstPitch;
        const BYTE* rowSecond = second + row * secondPitch;
        BYTE* out;
        LONG position = start;
        LONG x;

        if (y < 0)
        {
            continue;
        }

        if (y >= destHeight)
        {
            break;
        }

        out = dest + y * destPitch + left2;
        for (x = 0; x < count; ++x)
        {
            LONG column = position >> 1;

            out[x] = column < half ?
                rowFirst[column] : rowSecond[column - half];
            if (++position == period2)
            {
                position = 0;
            }
        }

        if (y + 1 < destHeight)
        {
            BYTE* next = out + destPitch;

            for (x = 0; x < count; ++x)
            {
                next[x] = out[x];
            }
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
    LONG width, LONG height,
    BYTE* dest, LONG destPitch, LONG destWidth, LONG destHeight)
{
    LONG y;

    for (y = 0; y < height && y * 2 + 1 < destHeight; ++y)
    {
        const BYTE* row = hud + y * hudPitch;
        const BYTE* old = before + y * width;
        BYTE* top = dest + y * 2 * destPitch;
        BYTE* bottom = top + destPitch;
        LONG x = 0;

        while (x < width && x * 2 + 1 < destWidth)
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

                top[x * 2] = value;
                top[x * 2 + 1] = value;
                bottom[x * 2] = value;
                bottom[x * 2 + 1] = value;
            }

            ++x;
        }
    }
}

void RenderScaleSetDoubled(BOOL doubled)
{
    DWORD shift = doubled ? kDoubledShift : kHiResShift;
    DWORD* current;

    if (!g_scaleInstalled)
    {
        return;
    }

    // For later HiRes setups, and for the one that may have run already.
    WriteCodeDword(kShiftStoreRva + 6, shift);
    current = reinterpret_cast<DWORD*>(g_scaleBase + kShiftRva);
    if (*current == kHiResShift || *current == kDoubledShift)
    {
        *current = shift;
    }
}
