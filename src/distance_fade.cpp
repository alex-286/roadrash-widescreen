// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "distance_fade.h"
#include "draw_distance.h"
#include "road_rash_patch.h"

#include <intrin.h>

#pragma intrinsic(__stosb, __movsb)

// The road ahead is drawn slice by slice from far to near (FUN_00443AF0):
// FUN_00443CD0 adds the slice's world objects and FUN_00443B40 its road
// and scenery to the quad list (0xA8-byte quads from 0x49E66C, write
// pointer 0x49E660). FUN_00450420 rasterizes the list when it is full and
// at the end of the frame (FUN_0040FF40, then FUN_0044D300), with the
// current quad's list node (quad + 0x68) at 0x4C5D88. Textured quads are
// drawn span by span through the function at 0x484278 (destination at
// 0x4C5D50), flat ones through FUN_0044DB30.
//
// Wrappers around the two slice functions tag every quad they add with the
// haze level of its slice. The span and fill wrappers draw the pixels of
// hazy quads through a palette remap toward a haze color: for each level
// and palette entry, the entry closest to the blend of the two. The haze
// color is the average of the track's sky panorama (sky and distant land).
// The haze grows from the first of the farthest fadeSegments segments to
// 3/4 in the farthest one, so new segments appear out of the haze.

static const DWORD kImageBase = 0x00400000;
static const DWORD kQuadSize = 0xA8;
static const DWORD kNodeOffset = 0x68;
static const DWORD kSliceSize = 0x224;
static const DWORD kMaxQuads = 8192;
static const DWORD kMaxSpan = 4096;
static const DWORD kHazeSteps = 16;
static const DWORD kMaxHaze = 12;

static const DWORD kQuadListBaseVa = 0x0049E66C;
static const DWORD kQuadListWriteVa = 0x0049E660;
static const DWORD kCurrentNodeVa = 0x004C5D88;
static const DWORD kSpanFunctionVa = 0x00484278;
static const DWORD kSpanDestinationVa = 0x004C5D50;
static const DWORD kLoadedPaletteVa = 0x004B8490;
static const DWORD kSkySurfaceAVa = 0x00475418;
static const DWORD kSkySurfaceBVa = 0x0047541C;

static const DWORD kObjectsFunctionRva = 0x00043CD0;
static const DWORD kRoadFunctionRva = 0x00043B40;
static const DWORD kListBuildFunctionRva = 0x0000FF40;
static const DWORD kFillFunctionRva = 0x0004DB30;
static const DWORD kSpanCallRva = 0x000188E4;

// DirectDraw interface slots and surface description fields.
static const DWORD kSurfaceLockIndex = 25;
static const DWORD kSurfaceUnlockIndex = 32;
static const DWORD kDescriptionSize = 0x6C;
static const DWORD kDescriptionHeight = 0x08;
static const DWORD kDescriptionWidth = 0x0C;
static const DWORD kDescriptionPitch = 0x10;
static const DWORD kDescriptionSurface = 0x24;
static const DWORD kLockWaitReadOnly = 0x00000011;

struct CallSite
{
    DWORD rva;
    DWORD target;
};

static const CallSite kObjectsCalls[] = { { 0x00043B03, 0x00043CD0 }, { 0x00043B10, 0x00043CD0 } };
static const CallSite kRoadCalls[] = { { 0x00043B1B, 0x00043B40 }, { 0x00043B2A, 0x00043B40 } };
static const CallSite kListBuildCall = { 0x00050450, 0x0000FF40 };
static const CallSite kFillCalls[] = { { 0x0004D698, 0x0004DB30 }, { 0x0004DAC0, 0x0004DB30 } };

typedef void (__fastcall *SliceFn)(BYTE* slice);
typedef DWORD (__fastcall *ListBuildFn)(DWORD list);
typedef void (__fastcall *SpanFn)(int count, DWORD u, DWORD a3, DWORD a4, DWORD a5);
typedef void (__fastcall *FillFn)(int count, DWORD color, BYTE* destination);
typedef HRESULT (WINAPI *LockFn)(LPVOID, LPVOID, LPVOID, DWORD, HANDLE);
typedef HRESULT (WINAPI *UnlockFn)(LPVOID, LPVOID);

static DWORD g_base = 0;
static BYTE* g_slices = NULL;
static DWORD g_segments = 0;
static DWORD g_fadeSegments = 0;
static BYTE g_tags[kMaxQuads];
static BYTE* g_tagStart = NULL;
static BYTE g_currentLevel = 0;
static BYTE g_row[kMaxSpan];

// The palette the remap was built for, the sky it took the haze from, and
// the remap per haze level (level 0 unused).
static BYTE g_palette[1024];
static LPVOID g_hazeSkyA = NULL;
static LPVOID g_hazeSkyB = NULL;
static BOOL g_remapReady = FALSE;
static BYTE g_remap[kHazeSteps][256];

static DWORD GameVa(DWORD va)
{
    return g_base - kImageBase + va;
}

template <typename T>
static T& GameVariable(DWORD va)
{
    return *reinterpret_cast<T*>(GameVa(va));
}

static LPVOID* Vtable(LPVOID object)
{
    return *reinterpret_cast<LPVOID**>(object);
}

// The haze level (0 = none, up to kMaxHaze of 16) of a slice.
static BYTE LevelForSlice(const BYTE* slice)
{
    LONG index = static_cast<LONG>(slice - g_slices) / static_cast<LONG>(kSliceSize);
    LONG fromEnd = static_cast<LONG>(g_segments) - index;
    LONG fade = static_cast<LONG>(g_fadeSegments);

    if (index < 0 || fromEnd < 1 || fromEnd > fade)
    {
        return 0;
    }

    return static_cast<BYTE>(
        (static_cast<LONG>(kMaxHaze) * (fade + 1 - fromEnd) + fade / 2) / fade);
}

// Tags the quads added since the last call with the current level.
static void TagPending()
{
    BYTE* base = GameVariable<BYTE*>(kQuadListBaseVa);
    BYTE* end = GameVariable<BYTE*>(kQuadListWriteVa);
    BYTE* quad;

    if (g_tagStart < base || g_tagStart > end)
    {
        g_tagStart = base;
    }

    for (quad = g_tagStart; quad < end; quad += kQuadSize)
    {
        DWORD index = static_cast<DWORD>(quad - base) / kQuadSize;

        if (index < kMaxQuads)
        {
            g_tags[index] = g_currentLevel;
        }
    }

    g_tagStart = end;
}

static BYTE CurrentQuadLevel()
{
    BYTE* node = GameVariable<BYTE*>(kCurrentNodeVa);
    BYTE* base = GameVariable<BYTE*>(kQuadListBaseVa);
    DWORD index;

    if (!g_remapReady || node == NULL || node - kNodeOffset < base)
    {
        return 0;
    }

    index = static_cast<DWORD>(node - kNodeOffset - base) / kQuadSize;
    return index < kMaxQuads ? g_tags[index] : 0;
}

// Adds the colors of one sky panorama half to the sums.
static void AddSkyColors(LPVOID surface, const BYTE* palette, DWORD* sums, DWORD* count)
{
    BYTE description[kDescriptionSize];
    volatile DWORD* words = reinterpret_cast<volatile DWORD*>(description);
    const BYTE* pixels;
    LONG pitch;
    LONG width;
    LONG height;
    LONG x;
    LONG y;
    DWORD index;

    if (surface == NULL)
    {
        return;
    }

    for (index = 0; index < kDescriptionSize / sizeof(DWORD); ++index)
    {
        words[index] = 0;
    }

    words[0] = kDescriptionSize;
    if (FAILED(reinterpret_cast<LockFn>(Vtable(surface)[kSurfaceLockIndex])(
            surface, NULL, description, kLockWaitReadOnly, NULL)))
    {
        return;
    }

    pixels = *reinterpret_cast<BYTE**>(description + kDescriptionSurface);
    pitch = *reinterpret_cast<LONG*>(description + kDescriptionPitch);
    width = *reinterpret_cast<LONG*>(description + kDescriptionWidth);
    height = *reinterpret_cast<LONG*>(description + kDescriptionHeight);
    if (pixels != NULL)
    {
        for (y = 0; y < height; y += 2)
        {
            const BYTE* row = pixels + y * pitch;

            for (x = 0; x < width; x += 2)
            {
                const BYTE* entry = palette + row[x] * 4;

                sums[0] += entry[0];
                sums[1] += entry[1];
                sums[2] += entry[2];
                ++*count;
            }
        }
    }

    reinterpret_cast<UnlockFn>(Vtable(surface)[kSurfaceUnlockIndex])(surface, NULL);
}

// Rebuilds the remap when the track's palette or sky changed. The palette
// is the one loaded for the track (0x4B8490), not the displayed one, which
// the game fades in and out: a remap for the loaded palette also fits its
// faded versions, which scale every entry alike.
static void UpdateRemap()
{
    const BYTE* loaded = reinterpret_cast<const BYTE*>(GameVa(kLoadedPaletteVa));
    LPVOID skyA = GameVariable<LPVOID>(kSkySurfaceAVa);
    LPVOID skyB = GameVariable<LPVOID>(kSkySurfaceBVa);
    BYTE palette[1024];
    DWORD sums[3] = { 0, 0, 0 };
    DWORD count = 0;
    LONG haze[3];
    DWORD index;
    DWORD level;
    BOOL changed = !g_remapReady || skyA != g_hazeSkyA || skyB != g_hazeSkyB;

    __movsb(palette, loaded, sizeof(palette));
    for (index = 0; index < sizeof(palette) && !changed; ++index)
    {
        changed = palette[index] != g_palette[index];
    }

    if (!changed)
    {
        return;
    }

    AddSkyColors(skyA, palette, sums, &count);
    AddSkyColors(skyB, palette, sums, &count);
    if (count == 0)
    {
        haze[0] = 176;
        haze[1] = 192;
        haze[2] = 216;
    }
    else
    {
        haze[0] = static_cast<LONG>(sums[0] / count);
        haze[1] = static_cast<LONG>(sums[1] / count);
        haze[2] = static_cast<LONG>(sums[2] / count);
    }

    // For each level and entry: the entry (other than 0) closest to the
    // blend of the entry and the haze.
    for (level = 1; level < kHazeSteps; ++level)
    {
        for (index = 0; index < 256; ++index)
        {
            const BYTE* entry = palette + index * 4;
            LONG red = (entry[0] * (16 - static_cast<LONG>(level)) + haze[0] * static_cast<LONG>(level)) >> 4;
            LONG green = (entry[1] * (16 - static_cast<LONG>(level)) + haze[1] * static_cast<LONG>(level)) >> 4;
            LONG blue = (entry[2] * (16 - static_cast<LONG>(level)) + haze[2] * static_cast<LONG>(level)) >> 4;
            LONG best = 0x7FFFFFFF;
            DWORD candidate;
            BYTE choice = static_cast<BYTE>(index);

            for (candidate = 1; candidate < 256; ++candidate)
            {
                const BYTE* other = palette + candidate * 4;
                LONG dr = other[0] - red;
                LONG dg = other[1] - green;
                LONG db = other[2] - blue;
                LONG distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;

                if (distance < best)
                {
                    best = distance;
                    choice = static_cast<BYTE>(candidate);
                }
            }

            g_remap[level][index] = choice;
        }
    }

    // String intrinsic: a plain copy loop would compile to memcpy, which
    // this CRT-free DLL does not have.
    __movsb(g_palette, palette, sizeof(palette));
    g_hazeSkyA = skyA;
    g_hazeSkyB = skyB;
    g_remapReady = TRUE;

    {
        CHAR message[96];

        wsprintfA(message, "DistanceFade: haze color %ld,%ld,%ld.", haze[0], haze[1], haze[2]);
        LogProxyMessage(message, NULL, 0);
    }
}

static void CallSlice(DWORD rva, BYTE* slice)
{
    TagPending();
    g_currentLevel = LevelForSlice(slice);
    reinterpret_cast<SliceFn>(g_base + rva)(slice);
    TagPending();
    g_currentLevel = 0;
}

static void __fastcall HookedObjects(BYTE* slice)
{
    CallSlice(kObjectsFunctionRva, slice);
}

static void __fastcall HookedRoad(BYTE* slice)
{
    CallSlice(kRoadFunctionRva, slice);
}

// The list is about to be drawn: tag what is still untagged and make sure
// the remap fits the palette. Afterwards the game starts the list over at
// its base.
static DWORD __fastcall HookedListBuild(DWORD list)
{
    DWORD result;

    TagPending();
    UpdateRemap();
    result = reinterpret_cast<ListBuildFn>(g_base + kListBuildFunctionRva)(list);
    g_tagStart = GameVariable<BYTE*>(kQuadListBaseVa);
    return result;
}

static void __fastcall HookedSpan(int count, DWORD u, DWORD a3, DWORD a4, DWORD a5)
{
    SpanFn span = GameVariable<SpanFn>(kSpanFunctionVa);
    BYTE level = CurrentQuadLevel();
    BYTE** destinationVariable;
    BYTE* destination;
    const BYTE* remap;
    int index;

    if (level == 0 || count <= 0 || count > static_cast<int>(kMaxSpan))
    {
        span(count, u, a3, a4, a5);
        return;
    }

    // The span draws into the row buffer, which starts as a copy of the
    // frame; the pixels it changed go into the frame through the remap.
    // (Pixels it leaves out, transparent texels, stay as they are.)
    destinationVariable = &GameVariable<BYTE*>(kSpanDestinationVa);
    destination = *destinationVariable;
    for (index = 0; index < count; ++index)
    {
        g_row[index] = destination[index];
    }

    *destinationVariable = g_row;
    span(count, u, a3, a4, a5);
    *destinationVariable = destination;

    remap = g_remap[level];
    for (index = 0; index < count; ++index)
    {
        if (g_row[index] != destination[index])
        {
            destination[index] = remap[g_row[index]];
        }
    }
}

static void __fastcall HookedFill(int count, DWORD color, BYTE* destination)
{
    BYTE level = CurrentQuadLevel();

    if (level == 0 || count <= 0)
    {
        reinterpret_cast<FillFn>(g_base + kFillFunctionRva)(count, color, destination);
        return;
    }

    // String intrinsic: a plain loop would compile to memset, which this
    // CRT-free DLL does not have.
    __stosb(destination, g_remap[level][color & 0xFF], static_cast<size_t>(count));
}

static DWORD ReadDword(const BYTE* bytes)
{
    return static_cast<DWORD>(bytes[0]) |
        (static_cast<DWORD>(bytes[1]) << 8) |
        (static_cast<DWORD>(bytes[2]) << 16) |
        (static_cast<DWORD>(bytes[3]) << 24);
}

static BOOL IsCallTo(const CallSite& site)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + site.rva);

    return code[0] == 0xE8 && ReadDword(code + 1) == site.target - (site.rva + 5);
}

static BOOL WriteCode(DWORD rva, const BYTE* bytes, DWORD length)
{
    BYTE* destination = reinterpret_cast<BYTE*>(g_base + rva);
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

// Writes call <function> at rva, padded with NOPs to length bytes.
static BOOL WriteCall(DWORD rva, const void* function, DWORD length)
{
    BYTE code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };

    *reinterpret_cast<DWORD*>(code + 1) =
        reinterpret_cast<DWORD>(function) - (g_base + rva + 5);
    return WriteCode(rva, code, length);
}

BOOL DistanceFadeInstall(DWORD gameBase, DWORD fadeSegments)
{
    const BYTE* spanCall;
    DWORD index;
    BOOL ok = TRUE;
    CHAR message[96];

    if (fadeSegments == 0)
    {
        return TRUE;
    }

    g_base = gameBase;
    g_slices = DrawDistanceSlices(gameBase);
    g_segments = DrawDistanceSegments();
    if (fadeSegments > kMaxFadeSegments)
    {
        fadeSegments = kMaxFadeSegments;
    }

    if (fadeSegments >= g_segments)
    {
        fadeSegments = g_segments / 2;
    }

    // call dword ptr [0x484278]
    spanCall = reinterpret_cast<const BYTE*>(gameBase + kSpanCallRva);
    if (!IsCallTo(kObjectsCalls[0]) || !IsCallTo(kObjectsCalls[1]) ||
        !IsCallTo(kRoadCalls[0]) || !IsCallTo(kRoadCalls[1]) ||
        !IsCallTo(kListBuildCall) ||
        !IsCallTo(kFillCalls[0]) || !IsCallTo(kFillCalls[1]) ||
        spanCall[0] != 0xFF || spanCall[1] != 0x15 ||
        ReadDword(spanCall + 2) != GameVa(kSpanFunctionVa))
    {
        LogProxyMessage(
            "DistanceFade stays off: unexpected code in the road drawing.",
            NULL,
            0);
        return FALSE;
    }

    g_fadeSegments = fadeSegments;
    __stosb(g_tags, 0, kMaxQuads);

    for (index = 0; index < 2; ++index)
    {
        ok = WriteCall(kObjectsCalls[index].rva, HookedObjects, 5) && ok;
        ok = WriteCall(kRoadCalls[index].rva, HookedRoad, 5) && ok;
        ok = WriteCall(kFillCalls[index].rva, HookedFill, 5) && ok;
    }

    ok = WriteCall(kListBuildCall.rva, HookedListBuild, 5) && ok;
    ok = WriteCall(kSpanCallRva, HookedSpan, 6) && ok;
    if (!ok)
    {
        LogProxyMessage(
            "DistanceFade: a code change failed; the game may be unstable.",
            NULL,
            0);
        return FALSE;
    }

    wsprintfA(
        message,
        "DistanceFade: the farthest %lu of %lu road segments fade into a haze.",
        fadeSegments,
        g_segments);
    LogProxyMessage(message, NULL, 0);
    return TRUE;
}
