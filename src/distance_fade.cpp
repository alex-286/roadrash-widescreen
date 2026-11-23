/**
 * Fades the farthest road segments into a haze, so new segments do not pop
 * in whole at the end of the draw distance.
 *
 * The road ahead is drawn slice by slice from far to near (FUN_00443AF0):
 * FUN_00443CD0 adds the slice's world objects and FUN_00443B40 its road
 * and scenery to the quad list (0xA8-byte quads from 0x49E66C, write
 * pointer 0x49E660). FUN_00450420 rasterizes the list when it is full and
 * at the end of the frame (FUN_0040FF40, then FUN_0044D300), with the
 * current quad's list node (quad + 0x68) at 0x4C5D88. Textured quads are
 * drawn span by span through the function at 0x484278 (destination at
 * 0x4C5D50), flat ones through FUN_0044DB30.
 *
 * Wrappers around the two slice functions tag every quad they add with the
 * haze level of its slice. The span and fill wrappers draw the pixels of
 * hazy quads through a palette remap toward a haze color: for each level
 * and palette entry, the entry closest to the blend of the two. The haze
 * color is the average of the track's sky panorama (sky and distant land).
 * The haze grows from the first of the farthest fadeSegments segments to
 * 3/4 in the farthest one, so new segments appear out of the haze.
 *
 * Smooth haze (a 32-bit filter and RenderScale above 1): the remap can only
 * pick among the 256 palette colors, so the haze shows in steps. Instead,
 * hazy pixels keep their own color and their level goes into a plane laid
 * out like the frame (the rasterizer's destination is base 0x4B7EC0 +
 * y * pitch 0x4B7EC4 + x). Pixels that nearer quads draw over get level 0
 * again; transparent texels, which a span leaves unchanged, keep the level
 * of what lies behind them. The filter looks up each hazy pixel in a table
 * of the palette blended toward the haze color (DistanceFadePrepareFrame),
 * and the levels are cleared after the frame.
 *
 * @file distance_fade.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "distance_fade.h"
#include "draw_distance.h"
#include "road_rash_patch.h"

#include <intrin.h>

#pragma intrinsic(__stosb, __movsb)

/**
 * The preferred load address of ROADRASH.EXE, which its absolute addresses
 * assume.
 */
static const DWORD kImageBase = 0x00400000;
/** The size of a quad in the quad list in bytes. */
static const DWORD kQuadSize = 0xA8;
/** The offset of a quad's list node in the quad. */
static const DWORD kNodeOffset = 0x68;
/** The size of a road slice in bytes. */
static const DWORD kSliceSize = 0x224;
/** The largest number of quads tagged per list. */
static const DWORD kMaxQuads = 8192;
/** The widest frame row: 1280 virtual pixels at RenderScale 4. */
static const DWORD kMaxSpan = 5120;
/** The most frame rows: 480 at RenderScale 4. */
static const DWORD kMaxFrameRows = 1920;
/** The number of remap levels, including the unused level 0. */
static const DWORD kHazeSteps = 16;
/** The haze level of the farthest segment, in sixteenths (3/4). */
static const DWORD kMaxHaze = 12;

/** The start of the quad list (0x49E66C). */
static const DWORD kQuadListBaseVa = 0x0049E66C;
/** The write pointer of the quad list (0x49E660). */
static const DWORD kQuadListWriteVa = 0x0049E660;
/** The list node of the quad being rasterized (0x4C5D88). */
static const DWORD kCurrentNodeVa = 0x004C5D88;
/** The pointer to the span function of textured quads (0x484278). */
static const DWORD kSpanFunctionVa = 0x00484278;
/** The destination of the next span (0x4C5D50). */
static const DWORD kSpanDestinationVa = 0x004C5D50;
/** The palette loaded for the track (0x4B8490), 256 entries of four bytes. */
static const DWORD kLoadedPaletteVa = 0x004B8490;
/** The surface of the first half of the sky panorama. */
static const DWORD kSkySurfaceAVa = 0x00475418;
/** The surface of the second half of the sky panorama. */
static const DWORD kSkySurfaceBVa = 0x0047541C;
/** The first pixel of the frame the rasterizer draws into (0x4B7EC0). */
static const DWORD kFrameBaseVa = 0x004B7EC0;
/** The bytes per row of that frame (0x4B7EC4). */
static const DWORD kFramePitchVa = 0x004B7EC4;

/** FUN_00443CD0, which adds a slice's world objects to the quad list. */
static const DWORD kObjectsFunctionRva = 0x00043CD0;
/** FUN_00443B40, which adds a slice's road and scenery to the quad list. */
static const DWORD kRoadFunctionRva = 0x00043B40;
/** FUN_0040FF40, called before the quad list is rasterized. */
static const DWORD kListBuildFunctionRva = 0x0000FF40;
/** FUN_0044DB30, which fills a span of a flat quad. */
static const DWORD kFillFunctionRva = 0x0004DB30;
/** The call dword ptr [0x484278] that draws a span of a textured quad. */
static const DWORD kSpanCallRva = 0x000188E4;

/** The vtable slot of IDirectDrawSurface::Lock. */
static const DWORD kSurfaceLockIndex = 25;
/** The vtable slot of IDirectDrawSurface::Unlock. */
static const DWORD kSurfaceUnlockIndex = 32;
/** The size of DDSURFACEDESC in bytes. */
static const DWORD kDescriptionSize = 0x6C;
/** The offset of dwHeight in DDSURFACEDESC. */
static const DWORD kDescriptionHeight = 0x08;
/** The offset of dwWidth in DDSURFACEDESC. */
static const DWORD kDescriptionWidth = 0x0C;
/** The offset of lPitch in DDSURFACEDESC. */
static const DWORD kDescriptionPitch = 0x10;
/** The offset of lpSurface in DDSURFACEDESC. */
static const DWORD kDescriptionSurface = 0x24;
/** DDLOCK_WAIT | DDLOCK_READONLY. */
static const DWORD kLockWaitReadOnly = 0x00000011;

/** A call in the game's code and the function it calls. */
struct CallSite
{
    /** The relative address of the E8 call. */
    DWORD rva;
    /** The relative address of the function it calls. */
    DWORD target;
};

/** The calls of FUN_00443CD0 in the slice loop. */
static const CallSite kObjectsCalls[] = { { 0x00043B03, 0x00043CD0 }, { 0x00043B10, 0x00043CD0 } };
/** The calls of FUN_00443B40 in the slice loop. */
static const CallSite kRoadCalls[] = { { 0x00043B1B, 0x00043B40 }, { 0x00043B2A, 0x00043B40 } };
/** The call of FUN_0040FF40 in FUN_00450420. */
static const CallSite kListBuildCall = { 0x00050450, 0x0000FF40 };
/** The calls of FUN_0044DB30 in the flat rasterizer. */
static const CallSite kFillCalls[] = { { 0x0004D698, 0x0004DB30 }, { 0x0004DAC0, 0x0004DB30 } };

/** A slice function: FUN_00443CD0 or FUN_00443B40. */
typedef void (__fastcall *SliceFn)(BYTE* slice);
/** FUN_0040FF40. */
typedef DWORD (__fastcall *ListBuildFn)(DWORD list);
/** The span function of textured quads. */
typedef void (__fastcall *SpanFn)(int count, DWORD u, DWORD a3, DWORD a4, DWORD a5);
/** The span fill of flat quads, FUN_0044DB30. */
typedef void (__fastcall *FillFn)(int count, DWORD color, BYTE* destination);
/** IDirectDrawSurface::Lock. */
typedef HRESULT (WINAPI *LockFn)(LPVOID, LPVOID, LPVOID, DWORD, HANDLE);
/** IDirectDrawSurface::Unlock. */
typedef HRESULT (WINAPI *UnlockFn)(LPVOID, LPVOID);

/** Module base of ROADRASH.EXE. */
static DWORD g_base = 0;
/** The slice array the game draws from. */
static BYTE* g_slices = NULL;
/** The number of segments the game draws ahead. */
static DWORD g_segments = 0;
/** The number of farthest segments that fade; 0 when off. */
static DWORD g_fadeSegments = 0;
/** The haze level of every quad in the list, by its index. */
static BYTE g_tags[kMaxQuads];
/** The first quad not yet tagged. */
static BYTE* g_tagStart = NULL;
/** The haze level of the slice being added to the list. */
static BYTE g_currentLevel = 0;
/** The row buffer a hazy span is drawn into. */
static BYTE g_row[kMaxSpan];

/** The palette the remap was built for. */
static BYTE g_palette[1024];
/** The first sky half the haze color was taken from. */
static LPVOID g_hazeSkyA = NULL;
/** The second sky half the haze color was taken from. */
static LPVOID g_hazeSkyB = NULL;
/** TRUE once the remap was built. */
static BOOL g_remapReady = FALSE;
/**
 * The remap per haze level (level 0 unused): the palette entry closest to
 * each entry blended toward the haze color.
 */
static BYTE g_remap[kHazeSteps][256];
/**
 * How often each palette index occurs in the sky panorama, for the haze
 * color in the displayed palette.
 */
static DWORD g_skyCounts[256];
/** The number of sky pixels counted in g_skyCounts. */
static DWORD g_skyTotal = 0;

/** TRUE when the smooth haze is wanted for this race. */
static BOOL g_smoothWanted = FALSE;
/** The level plane, laid out like the frame at g_frameBase, or NULL. */
static BYTE* g_levels = NULL;
/** The size of the level plane in bytes. */
static DWORD g_levelBytes = 0;
/** The frame the level plane belongs to, once it is known. */
static const BYTE* g_frameBase = NULL;
/** The bytes per row of the frame and the level plane. */
static LONG g_framePitch = 0;
/** The number of rows of the frame and the level plane. */
static DWORD g_frameRows = 0;
/** Nonzero for the rows that hold haze. */
static BYTE g_hazeRows[kMaxFrameRows];
/** The first row marked in g_hazeRows. */
static LONG g_firstHazeRow = 0x7FFFFFFF;
/** The last row marked in g_hazeRows, or -1 for none. */
static LONG g_lastHazeRow = -1;
/** The colors of each haze level and palette index for the filters. */
static DWORD g_hazeRgb[kHazeTableSize];
/** The haze handed to the filter for this frame. */
static HazeFrame g_hazeFrame;

/**
 * Converts an address of the game's image to the loaded module.
 *
 * @param [in] va The address for the preferred image base.
 * @return The address in the loaded module.
 */
static DWORD GameVa(DWORD va)
{
    return g_base - kImageBase + va;
}

/**
 * Gives a variable of the game.
 *
 * @tparam T The type of the variable.
 * @param [in] va The address for the preferred image base.
 * @return The variable in the loaded module.
 */
template <typename T>
static T& GameVariable(DWORD va)
{
    return *reinterpret_cast<T*>(GameVa(va));
}

/**
 * Gives the vtable of a COM object.
 *
 * @param [in] object The object.
 * @return Its vtable.
 */
static LPVOID* Vtable(LPVOID object)
{
    return *reinterpret_cast<LPVOID**>(object);
}

/**
 * Gives the haze level of a slice.
 *
 * @param [in] slice The slice in the game's slice array.
 * @return 0 for none, up to kMaxHaze (in sixteenths).
 */
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

/** Tags the quads added since the last call with the current level. */
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

/**
 * Gives the haze level of the quad being rasterized.
 *
 * @return The level, or 0 for none or when the remap is not ready.
 */
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

/**
 * Adds the colors of one sky panorama half to the sums.
 *
 * Every second pixel of every second row is counted.
 *
 * @param [in] surface The sky surface, or NULL.
 * @param [in] palette 256 palette entries of four bytes.
 * @param [in,out] sums The sums of red, green and blue.
 * @param [in,out] count The number of pixels counted.
 * @param [in,out] counts The number of pixels counted per palette index.
 */
static void AddSkyColors(
    LPVOID surface, const BYTE* palette, DWORD* sums, DWORD* count, DWORD* counts)
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
                ++counts[row[x]];
            }
        }
    }

    reinterpret_cast<UnlockFn>(Vtable(surface)[kSurfaceUnlockIndex])(surface, NULL);
}

/**
 * Rebuilds the remap when the track's palette or sky changed.
 *
 * The palette is the one loaded for the track (0x4B8490), not the displayed
 * one, which the game fades in and out: a remap for the loaded palette also
 * fits its faded versions, which scale every entry alike.
 */
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

    __stosb(reinterpret_cast<BYTE*>(g_skyCounts), 0, sizeof(g_skyCounts));
    AddSkyColors(skyA, palette, sums, &count, g_skyCounts);
    AddSkyColors(skyB, palette, sums, &count, g_skyCounts);
    g_skyTotal = count;
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

/**
 * Calls a slice function and tags the quads it adds with the slice's
 * level.
 *
 * @param [in] rva The slice function.
 * @param [in] slice The slice.
 */
static void CallSlice(DWORD rva, BYTE* slice)
{
    TagPending();
    g_currentLevel = LevelForSlice(slice);
    reinterpret_cast<SliceFn>(g_base + rva)(slice);
    TagPending();
    g_currentLevel = 0;
}

/**
 * Replaces the calls of FUN_00443CD0: adds the slice's objects, tagged.
 *
 * @param [in] slice The slice.
 */
static void __fastcall HookedObjects(BYTE* slice)
{
    CallSlice(kObjectsFunctionRva, slice);
}

/**
 * Replaces the calls of FUN_00443B40: adds the slice's road, tagged.
 *
 * @param [in] slice The slice.
 */
static void __fastcall HookedRoad(BYTE* slice)
{
    CallSlice(kRoadFunctionRva, slice);
}

/**
 * Replaces the call of FUN_0040FF40, before the list is drawn.
 *
 * Tags what is still untagged and makes sure the remap fits the palette.
 * Afterwards the game starts the list over at its base.
 *
 * @param [in] list Passed on to the game.
 * @return The game's result.
 */
static DWORD __fastcall HookedListBuild(DWORD list)
{
    DWORD result;

    TagPending();
    UpdateRemap();
    result = reinterpret_cast<ListBuildFn>(g_base + kListBuildFunctionRva)(list);
    g_tagStart = GameVariable<BYTE*>(kQuadListBaseVa);
    return result;
}

/**
 * Gives the levels of the pixels the game draws into.
 *
 * @param [in] destination The first pixel drawn.
 * @param [in] count The number of pixels drawn.
 * @param [out] row Receives the frame row of destination.
 * @return The levels of the pixels, when the game draws into the frame of
 *         the level plane and they lie within one of its rows; otherwise
 *         NULL.
 */
static BYTE* LevelsAt(const BYTE* destination, int count, LONG* row)
{
    const BYTE* base;
    LONG pitch;
    DWORD offset;
    LONG y;

    if (g_levels == NULL || g_frameBase == NULL)
    {
        return NULL;
    }

    base = GameVariable<const BYTE*>(kFrameBaseVa);
    pitch = GameVariable<LONG>(kFramePitchVa);
    if (base != g_frameBase || pitch != g_framePitch || destination < base)
    {
        return NULL;
    }

    offset = static_cast<DWORD>(destination - base);
    y = static_cast<LONG>(offset / static_cast<DWORD>(pitch));
    if (y >= static_cast<LONG>(g_frameRows) ||
        static_cast<LONG>(offset - static_cast<DWORD>(y * pitch)) + count > pitch)
    {
        return NULL;
    }

    *row = y;
    return g_levels + offset;
}

/**
 * Marks a frame row as holding haze.
 *
 * @param [in] row The row.
 */
static void MarkHazeRow(LONG row)
{
    g_hazeRows[row] = 1;
    if (row < g_firstHazeRow)
    {
        g_firstHazeRow = row;
    }

    if (row > g_lastHazeRow)
    {
        g_lastHazeRow = row;
    }
}

/**
 * Tells whether any of some pixels has haze.
 *
 * @param [in] levels The levels of the pixels.
 * @param [in] count The number of pixels.
 * @return TRUE when a level is not 0.
 */
static BOOL AnyLevel(const BYTE* levels, int count)
{
    int index;

    for (index = 0; index < count; ++index)
    {
        if (levels[index] != 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * Lets a span draw into the row buffer instead of the frame.
 *
 * The buffer starts as a copy of the frame, so the pixels the span changed
 * can be told from those it left out (transparent texels).
 *
 * @param [in] span The game's span function.
 * @param [in] count The number of pixels.
 * @param [in] u Passed on to the span function.
 * @param [in] a3 Passed on to the span function.
 * @param [in] a4 Passed on to the span function.
 * @param [in] a5 Passed on to the span function.
 * @param [in,out] destinationVariable The game's span destination, pointed
 *                 to the buffer during the call.
 * @param [in] destination The frame pixels the span draws over.
 */
static void DrawSpanToRow(
    SpanFn span, int count, DWORD u, DWORD a3, DWORD a4, DWORD a5,
    BYTE** destinationVariable, const BYTE* destination)
{
    __movsb(g_row, destination, static_cast<size_t>(count));
    *destinationVariable = g_row;
    span(count, u, a3, a4, a5);
    *destinationVariable = const_cast<BYTE*>(destination);
}

/**
 * Replaces the span call of textured quads: draws hazy pixels through the
 * remap, or with the smooth haze records their level.
 *
 * @param [in] count The number of pixels.
 * @param [in] u Passed on to the span function.
 * @param [in] a3 Passed on to the span function.
 * @param [in] a4 Passed on to the span function.
 * @param [in] a5 Passed on to the span function.
 */
static void __fastcall HookedSpan(int count, DWORD u, DWORD a3, DWORD a4, DWORD a5)
{
    SpanFn span = GameVariable<SpanFn>(kSpanFunctionVa);
    BYTE level = CurrentQuadLevel();
    BYTE** destinationVariable;
    BYTE* destination;
    BYTE* levels;
    const BYTE* remap;
    LONG row;
    int index;

    if (count <= 0 || count > static_cast<int>(kMaxSpan))
    {
        span(count, u, a3, a4, a5);
        return;
    }

    destinationVariable = &GameVariable<BYTE*>(kSpanDestinationVa);
    destination = *destinationVariable;
    levels = LevelsAt(destination, count, &row);
    if (levels != NULL)
    {
        // Smooth haze: the changed pixels keep their color and get the
        // quad's level (0 for a quad without haze).
        if (level == 0 && (g_hazeRows[row] == 0 || !AnyLevel(levels, count)))
        {
            span(count, u, a3, a4, a5);
            return;
        }

        DrawSpanToRow(span, count, u, a3, a4, a5, destinationVariable, destination);
        for (index = 0; index < count; ++index)
        {
            if (g_row[index] != destination[index])
            {
                destination[index] = g_row[index];
                levels[index] = level;
            }
        }

        if (level != 0)
        {
            MarkHazeRow(row);
        }

        return;
    }

    if (level == 0)
    {
        span(count, u, a3, a4, a5);
        return;
    }

    // The pixels the span changed go into the frame through the remap.
    DrawSpanToRow(span, count, u, a3, a4, a5, destinationVariable, destination);
    remap = g_remap[level];
    for (index = 0; index < count; ++index)
    {
        if (g_row[index] != destination[index])
        {
            destination[index] = remap[g_row[index]];
        }
    }
}

/**
 * Replaces the calls of FUN_0044DB30: fills hazy spans through the remap,
 * or with the smooth haze records their level.
 *
 * @param [in] count The number of pixels.
 * @param [in] color The palette index to fill with.
 * @param [out] destination The first pixel.
 */
static void __fastcall HookedFill(int count, DWORD color, BYTE* destination)
{
    BYTE level = CurrentQuadLevel();
    BYTE* levels;
    LONG row;

    if (count <= 0)
    {
        reinterpret_cast<FillFn>(g_base + kFillFunctionRva)(count, color, destination);
        return;
    }

    levels = LevelsAt(destination, count, &row);
    if (levels != NULL)
    {
        reinterpret_cast<FillFn>(g_base + kFillFunctionRva)(count, color, destination);
        if (level != 0)
        {
            __stosb(levels, level, static_cast<size_t>(count));
            MarkHazeRow(row);
        }
        else if (g_hazeRows[row] != 0)
        {
            __stosb(levels, 0, static_cast<size_t>(count));
        }

        return;
    }

    if (level == 0)
    {
        reinterpret_cast<FillFn>(g_base + kFillFunctionRva)(count, color, destination);
        return;
    }

    // String intrinsic: a plain loop would compile to memset, which this
    // CRT-free DLL does not have.
    __stosb(destination, g_remap[level][color & 0xFF], static_cast<size_t>(count));
}

void DistanceFadeSetSmooth(BOOL smooth)
{
    g_smoothWanted = smooth;
    g_frameBase = NULL;
    g_framePitch = 0;
    g_frameRows = 0;
    __stosb(g_hazeRows, 0, sizeof(g_hazeRows));
    g_firstHazeRow = 0x7FFFFFFF;
    g_lastHazeRow = -1;
}

/**
 * Gives the haze color in the displayed palette (which the game fades in
 * and out): the sky panorama's average.
 *
 * @param [in] rgb 256 X8R8G8B8 colors, one per palette index.
 * @return The haze color as X8R8G8B8.
 */
static DWORD HazeColor(const DWORD* rgb)
{
    DWORD red = 0;
    DWORD green = 0;
    DWORD blue = 0;
    DWORD index;

    if (g_skyTotal == 0)
    {
        return 0x00B0C0D8;
    }

    for (index = 0; index < 256; ++index)
    {
        DWORD count = g_skyCounts[index];

        if (count != 0)
        {
            red += ((rgb[index] >> 16) & 0xFF) * count;
            green += ((rgb[index] >> 8) & 0xFF) * count;
            blue += (rgb[index] & 0xFF) * count;
        }
    }

    return ((red / g_skyTotal) << 16) | ((green / g_skyTotal) << 8) | (blue / g_skyTotal);
}

const HazeFrame* DistanceFadePrepareFrame(
    const BYTE* frame, LONG pitch, DWORD height, const DWORD* rgb)
{
    DWORD haze;
    DWORD hazeRed;
    DWORD hazeGreen;
    DWORD hazeBlue;
    DWORD level;
    DWORD index;

    if (!g_smoothWanted || g_fadeSegments == 0 || frame == NULL || pitch <= 0 ||
        height == 0 || height > kMaxFrameRows)
    {
        return NULL;
    }

    // A new frame layout: haze levels start with the next frame.
    if (frame != g_frameBase || pitch != g_framePitch || height != g_frameRows)
    {
        DWORD bytes = static_cast<DWORD>(pitch) * height;

        if (bytes > g_levelBytes)
        {
            if (g_levels != NULL)
            {
                VirtualFree(g_levels, 0, MEM_RELEASE);
            }

            // Committed pages start zeroed.
            g_levels = reinterpret_cast<BYTE*>(
                VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            g_levelBytes = g_levels != NULL ? bytes : 0;
        }
        else if (g_levels != NULL)
        {
            __stosb(g_levels, 0, bytes);
        }

        DistanceFadeSetSmooth(TRUE);
        if (g_levels != NULL)
        {
            g_frameBase = frame;
            g_framePitch = pitch;
            g_frameRows = height;
        }

        return NULL;
    }

    if (g_lastHazeRow < 0)
    {
        return NULL;
    }

    haze = HazeColor(rgb);
    hazeRed = (haze >> 16) & 0xFF;
    hazeGreen = (haze >> 8) & 0xFF;
    hazeBlue = haze & 0xFF;
    for (level = 0; level < kHazeLevels; ++level)
    {
        DWORD keep = kHazeLevels - level;
        DWORD* table = g_hazeRgb + level * 256;

        for (index = 0; index < 256; ++index)
        {
            DWORD color = rgb[index];
            DWORD red = (((color >> 16) & 0xFF) * keep + hazeRed * level) >> 4;
            DWORD green = (((color >> 8) & 0xFF) * keep + hazeGreen * level) >> 4;
            DWORD blue = ((color & 0xFF) * keep + hazeBlue * level) >> 4;

            table[index] = (red << 16) | (green << 8) | blue;
        }
    }

    g_hazeFrame.levels = g_levels;
    g_hazeFrame.levelPitch = g_framePitch;
    g_hazeFrame.rows = g_hazeRows;
    g_hazeFrame.rgb = g_hazeRgb;
    return &g_hazeFrame;
}

void DistanceFadeFrameDone()
{
    LONG row;

    if (g_levels == NULL || g_frameBase == NULL)
    {
        return;
    }

    for (row = g_firstHazeRow; row <= g_lastHazeRow; ++row)
    {
        if (g_hazeRows[row] != 0)
        {
            __stosb(g_levels + row * g_framePitch, 0, static_cast<size_t>(g_framePitch));
            g_hazeRows[row] = 0;
        }
    }

    g_firstHazeRow = 0x7FFFFFFF;
    g_lastHazeRow = -1;
}

BYTE* DistanceFadeLevelsFor(const BYTE* frame, LONG pitch)
{
    if (g_levels == NULL || frame != g_frameBase || pitch != g_framePitch)
    {
        return NULL;
    }

    return g_levels;
}

/**
 * Reads an unaligned little-endian DWORD.
 *
 * @param [in] bytes The four bytes.
 * @return The value.
 */
static DWORD ReadDword(const BYTE* bytes)
{
    return static_cast<DWORD>(bytes[0]) |
        (static_cast<DWORD>(bytes[1]) << 8) |
        (static_cast<DWORD>(bytes[2]) << 16) |
        (static_cast<DWORD>(bytes[3]) << 24);
}

/**
 * Tells whether a call site holds its original call.
 *
 * @param [in] site The call and its expected target.
 * @return TRUE when the code is an E8 call to the target.
 */
static BOOL IsCallTo(const CallSite& site)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + site.rva);

    return code[0] == 0xE8 && ReadDword(code + 1) == site.target - (site.rva + 5);
}

/**
 * Overwrites bytes of the game's code.
 *
 * @param [in] rva The relative address of the first byte.
 * @param [in] bytes The new bytes.
 * @param [in] length The number of bytes.
 * @return FALSE when the code could not be made writable.
 */
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

/**
 * Writes a call of a function into the game's code, padded with NOPs.
 *
 * @param [in] rva The relative address of the instruction to replace.
 * @param [in] function The function to call.
 * @param [in] length The number of bytes to replace, 5 or 6.
 * @return FALSE when the code could not be made writable.
 */
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
