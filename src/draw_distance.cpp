// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "draw_distance.h"
#include "road_rash_patch.h"

// The road ahead is built as one slice per track segment (FUN_00443D70,
// called with 33 = segments + 1) and drawn from far to near by FUN_00443AF0,
// which starts at slice 31 and steps back 31 times. The segments themselves
// are streamed 80 ahead of the camera (FUN_004553xx: position + 0x5000), so
// more could be drawn without loading more of the track. The sprites beside
// the road, however, are loaded for the original distance only, so beyond
// 33 segments they are not drawn (see HookedSectionSetup).
//
// Riders, cars, pedestrians and other world nodes are still registered only
// up to depth camera + 0x2000 (32 segments, at most 50; FUN_00438450), so
// they keep appearing at the original distance.
//
// Arrays sized for 32 segments move into this DLL at the requested size:
// - the slices (0x224 bytes, segments + 2 of them) and the two row caches
//   (0x90 bytes, segments + 2 each), kept in their original relative layout
//   (cache 1, cache 2, 0x30 bytes, slices), because the slice code also
//   reads the slice before the first one, which lies in cache 2;
// - the crest thresholds (one per slice), computed as FUN_00444E30 does;
// - the depth reciprocal table (0x1100 entries, indexed by depth / 2, with
//   no bounds check), extended by 2^23 / index, which its last entries
//   already follow.
// The fixed references to slice 31 (the farthest drawn) and slice 32 (the
// one after it) move to slices segments - 1 and segments. FUN_00444E30,
// which sets up the arrays at race start, runs as before and then sets up
// the new ones the same way.

static const DWORD kImageBase = 0x00400000;
static const DWORD kSliceSize = 0x224;
static const DWORD kCacheEntrySize = 0x90;
static const DWORD kCacheToSliceGap = 0x30;
static const DWORD kBlockPadding = 0x400;

static const DWORD kSlicesVa = 0x004BC800;
static const DWORD kCache1Va = 0x004BA190;
static const DWORD kCache2Va = 0x004BB4B0;
static const DWORD kThresholdsVa = 0x004C1160;
static const DWORD kThresholdSourceVa = 0x004734A0;
static const DWORD kThresholdSourceCount = 34;
static const DWORD kLastThresholdVa = 0x004C11DC;
static const DWORD kReciprocalVa = 0x0046F098;
static const DWORD kReciprocalCount = 0x1100;
static const DWORD kNewReciprocalCount = 0x4000;
static const DWORD kReciprocalFrontBytes = 0x4000;
static const DWORD kThresholdStart = 0x100;
static const DWORD kThresholdStep = 0x80;

static const DWORD kSetupCallRva = 0x00042A31;
static const DWORD kSetupFunctionRva = 0x00044E30;
static const DWORD kSliceCountPushRva = 0x00043A5A;
static const DWORD kDrawLoopCountRva = 0x00043AFE;

// An instruction whose 32-bit operand is an address to move.
struct AddressSite
{
    DWORD rva;
    BYTE prefix[4];
    DWORD prefixLength;
    DWORD original;
};

enum Target
{
    kTargetSlices,
    kTargetFarthestSlice,
    kTargetFarthestSegment,
    kTargetNextSliceRow,
    kTargetNextSlice,
    kTargetCache1,
    kTargetCache2,
    kTargetThresholds,
    kTargetReciprocal
};

struct TargetedSite
{
    AddressSite site;
    Target target;
};

static const TargetedSite kSites[] =
{
    // FUN_00443A00: the slice array passed to FUN_00443D70.
    { { 0x00043A42, { 0xBA }, 1, 0x004BC800 }, kTargetSlices },
    // FUN_00443AF0: the draw loop starts at slice 31.
    { { 0x00043AF2, { 0xBE }, 1, 0x004C0A5C }, kTargetFarthestSlice },
    // FUN_0040DAC0: the segment of slice 31 (closing polygon at the end).
    { { 0x0000DAF1, { 0x8B, 0x0D }, 2, 0x004C0A64 }, kTargetFarthestSegment },
    // FUN_00443A80: slice 32 and its first row, for the sky.
    { { 0x00043A81, { 0xA1 }, 1, 0x004C0C94 }, kTargetNextSliceRow },
    { { 0x00043ADC, { 0xB9 }, 1, 0x004C0C80 }, kTargetNextSlice },
    // FUN_00443D70: the row caches (base + slot * 0x90).
    { { 0x00043E8B, { 0x05 }, 1, 0x004BA190 }, kTargetCache1 },
    { { 0x00044038, { 0x05 }, 1, 0x004BA190 }, kTargetCache1 },
    { { 0x00044233, { 0x05 }, 1, 0x004BA190 }, kTargetCache1 },
    { { 0x0004446C, { 0x05 }, 1, 0x004BA190 }, kTargetCache1 },
    { { 0x0004405A, { 0x05 }, 1, 0x004BB4B0 }, kTargetCache2 },
    { { 0x00044255, { 0x05 }, 1, 0x004BB4B0 }, kTargetCache2 },
    { { 0x0004448E, { 0x05 }, 1, 0x004BB4B0 }, kTargetCache2 },
    // FUN_00443D70: the crest thresholds.
    { { 0x0004454A, { 0xBF }, 1, 0x004C1160 }, kTargetThresholds },
    // Every read of the depth reciprocal table ([index * 2 + table]).
    { { 0x0000BA48, { 0x8B, 0x1C, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0000BA4F, { 0x8B, 0x3C, 0x7D }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0000BC11, { 0x8B, 0x3C, 0x4D }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0000BC1F, { 0x8B, 0x1C, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0000C4C7, { 0x8B, 0x04, 0x4D }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x00043950, { 0x8D, 0x3C, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x00044364, { 0x0F, 0xAF, 0x34, 0x45 }, 4, 0x0046F098 }, kTargetReciprocal },
    { { 0x00044396, { 0x0F, 0xAF, 0x14, 0x45 }, 4, 0x0046F098 }, kTargetReciprocal },
    { { 0x000446BF, { 0x8B, 0x3C, 0x4D }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x00044884, { 0x8B, 0x04, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x000448D1, { 0x8B, 0x04, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x000448D8, { 0x8B, 0x14, 0x55 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x00044916, { 0x8B, 0x04, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0004496D, { 0x8B, 0x14, 0x55 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0004498D, { 0x8B, 0x04, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0004B645, { 0x8B, 0x0C, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0004BB45, { 0x8B, 0x14, 0x55 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0004BED7, { 0x8B, 0x14, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
    { { 0x0004C267, { 0x8B, 0x14, 0x45 }, 3, 0x0046F098 }, kTargetReciprocal },
};

static const DWORD kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

// Scenery beyond the original distance. The sprites beside the road come
// from four family slots (0x49E550, looked up by FUN_0044AC00 and its
// siblings), which the loader thread (FUN_0044ABA0) refills with the next
// family when the course needs it. Each segment's row cache entry (key =
// its course piece) is filled from those slots by FUN_00444630 when the
// segment first appears at the far end, and is kept while the key matches;
// FUN_0044B630 also fills it while drawing. The original's 32 segments
// never reach a slot that is about to change. Further out, an entry can be
// filled from a slot that still holds the previous family, shows the wrong
// sprites, and points into freed memory once the slot is reloaded (the
// crash at 0x44B6AD). So beyond 33 segments (depth 0x2100, the limit Open
// Rash uses for the same reason) an entry is emptied and given a negative
// key, which FUN_00444630 never fills, and the segment's scenery and
// objects (FUN_00438610) are not drawn; what comes from the course itself
// still is. Once the segment is within 33 segments, its entry gets a key
// that matches no piece, and the game fills it as in the original.
static const DWORD kSectionSetupRva = 0x00044630;
static const DWORD kSectionSetupCalls[] =
{
    0x00043E99, 0x0004406B, 0x0004407B, 0x00044266, 0x00044276, 0x000444A0
};
static const DWORD kSliceObjectsRva = 0x00038610;
static const DWORD kSliceObjectsCalls[] =
{
    0x00043B6D, 0x00043BC2, 0x00043BF2, 0x00043CA2
};
static const DWORD kEmptyEntryVa = 0x004C10D0;
static const DWORD kObjectCursorVa = 0x004977BC;
static const DWORD kObjectCursorNextVa = 0x004977C0;
static const LONG kNearDepth = 0x2100;
static const LONG kFarKey = static_cast<LONG>(0x80000002);
static const LONG kRefillKey = 0x7FFFFFFE;
static const DWORD kEntryDwords = kCacheEntrySize / sizeof(DWORD);

typedef void (__fastcall *SectionSetupFn)(BYTE* section, BYTE* record, LONG* projection);
typedef void (__fastcall *SliceObjectsFn)(LONG* record, int from, int to);

typedef void (__cdecl *SetupFn)(void);

static DWORD g_base = 0;
static DWORD g_segments = kOriginalDrawSegments;
static BYTE* g_cache1 = NULL;
static BYTE* g_cache2 = NULL;
static BYTE* g_slices = NULL;
static LONG* g_thresholds = NULL;
static LONG* g_reciprocal = NULL;

static void LogDistance(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

static DWORD GameVa(DWORD va)
{
    return g_base - kImageBase + va;
}

static DWORD ReadDword(const BYTE* bytes)
{
    return static_cast<DWORD>(bytes[0]) |
        (static_cast<DWORD>(bytes[1]) << 8) |
        (static_cast<DWORD>(bytes[2]) << 16) |
        (static_cast<DWORD>(bytes[3]) << 24);
}

static BOOL WriteCode(DWORD rva, const void* bytes, DWORD length)
{
    BYTE* destination = reinterpret_cast<BYTE*>(g_base + rva);
    const BYTE* source = reinterpret_cast<const BYTE*>(bytes);
    DWORD oldProtection;
    DWORD ignoredProtection;
    DWORD index;

    if (!VirtualProtect(destination, length, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return FALSE;
    }

    for (index = 0; index < length; ++index)
    {
        destination[index] = source[index];
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

static BOOL MatchesSite(const AddressSite& site)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + site.rva);
    DWORD index;

    for (index = 0; index < site.prefixLength; ++index)
    {
        if (code[index] != site.prefix[index])
        {
            return FALSE;
        }
    }

    return ReadDword(code + site.prefixLength) == GameVa(site.original);
}

static BYTE* Slice(DWORD index)
{
    return g_slices + index * kSliceSize;
}

static DWORD NewAddress(Target target)
{
    switch (target)
    {
    case kTargetSlices:
        return reinterpret_cast<DWORD>(Slice(0));
    case kTargetFarthestSlice:
        return reinterpret_cast<DWORD>(Slice(g_segments - 1));
    case kTargetFarthestSegment:
        return reinterpret_cast<DWORD>(Slice(g_segments - 1)) + 8;
    case kTargetNextSliceRow:
        return reinterpret_cast<DWORD>(Slice(g_segments)) + 0x14;
    case kTargetNextSlice:
        return reinterpret_cast<DWORD>(Slice(g_segments));
    case kTargetCache1:
        return reinterpret_cast<DWORD>(g_cache1);
    case kTargetCache2:
        return reinterpret_cast<DWORD>(g_cache2);
    case kTargetThresholds:
        return reinterpret_cast<DWORD>(g_thresholds);
    default:
        return reinterpret_cast<DWORD>(g_reciprocal);
    }
}

// FUN_00444E30 sets up the original arrays at race start; the new ones get
// the same setup afterwards.
static void __cdecl HookedSetup()
{
    const LONG* source = reinterpret_cast<const LONG*>(GameVa(kThresholdSourceVa));
    DWORD slices = g_segments + 2;
    DWORD index;

    reinterpret_cast<SetupFn>(g_base + kSetupFunctionRva)();

    // Both caches: the first dword of each entry is its key.
    for (index = 0; index < 2 * slices; ++index)
    {
        *reinterpret_cast<DWORD*>(g_cache1 + index * kCacheEntrySize) = 0;
    }

    // Each slice points at its two inline rows, and twice at itself.
    for (index = 0; index < slices; ++index)
    {
        BYTE* slice = Slice(index);

        *reinterpret_cast<BYTE**>(slice + 0x14) = slice + 0x1C;
        *reinterpret_cast<BYTE**>(slice + 0x18) = slice + 0x120;
        *reinterpret_cast<BYTE**>(slice + 0x128) = slice;
        *reinterpret_cast<BYTE**>(slice + 0x24) = slice;
    }

    // Crest thresholds: source height << 16 over the depth reciprocal of
    // each segment; past its last entry the source stays at that value.
    for (index = 0; index < g_segments; ++index)
    {
        DWORD sourceIndex = index < kThresholdSourceCount ?
            index : kThresholdSourceCount - 1;

        g_thresholds[index] = (source[sourceIndex] << 16) /
            g_reciprocal[kThresholdStart + index * kThresholdStep];
    }

    g_thresholds[g_segments] = *reinterpret_cast<const LONG*>(GameVa(kLastThresholdVa));
    g_thresholds[g_segments + 1] = g_thresholds[g_segments];
}

// projection[4]: the segment's depth; record + 0xFC: its row cache entry.
static void __fastcall HookedSectionSetup(
    BYTE* section,
    BYTE* record,
    LONG* projection)
{
    LONG* entry = *reinterpret_cast<LONG**>(record + 0xFC);

    if (projection[4] > kNearDepth)
    {
        if (entry[0] != kFarKey)
        {
            const LONG* empty = reinterpret_cast<const LONG*>(GameVa(kEmptyEntryVa));
            DWORD index;

            for (index = 0; index < kEntryDwords; ++index)
            {
                entry[index] = empty[index];
            }

            entry[0] = kFarKey;
        }
    }
    else if (entry[0] == kFarKey)
    {
        entry[0] = kRefillKey;
    }

    reinterpret_cast<SectionSetupFn>(g_base + kSectionSetupRva)(
        section, record, projection);
}

// record[6]: the segment's depth. A segment that is not drawn hands the
// object cursor on unchanged, as one without objects does.
static void __fastcall HookedSliceObjects(LONG* record, int from, int to)
{
    if (record[6] > kNearDepth)
    {
        *reinterpret_cast<LONG*>(GameVa(kObjectCursorNextVa)) =
            *reinterpret_cast<LONG*>(GameVa(kObjectCursorVa));
        return;
    }

    reinterpret_cast<SliceObjectsFn>(g_base + kSliceObjectsRva)(record, from, to);
}

static BOOL IsCallTo(DWORD rva, DWORD targetRva)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + rva);

    return code[0] == 0xE8 && ReadDword(code + 1) == targetRva - (rva + 5);
}

static void RedirectCall(DWORD rva, const void* hook)
{
    BYTE call[5];

    call[0] = 0xE8;
    *reinterpret_cast<DWORD*>(call + 1) =
        reinterpret_cast<DWORD>(hook) - (g_base + rva + 5);
    WriteCode(rva, call, sizeof(call));
}

static void* Allocate(DWORD bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

BOOL DrawDistanceInstall(DWORD gameBase, DWORD segments)
{
    BYTE call[5];
    const BYTE* code;
    DWORD slices;
    DWORD cacheBytes;
    BYTE* block;
    BYTE* table;
    const BYTE* original;
    DWORD index;

    if (segments <= kOriginalDrawSegments)
    {
        return TRUE;
    }

    if (segments > kMaxDrawSegments)
    {
        segments = kMaxDrawSegments;
    }

    g_base = gameBase;

    // Check everything first, so nothing is changed on a mismatch.
    for (index = 0; index < kSiteCount; ++index)
    {
        if (!MatchesSite(kSites[index].site))
        {
            LogDistance("DrawDistance stays at 32: unexpected code in the road drawing.");
            return FALSE;
        }
    }

    code = reinterpret_cast<const BYTE*>(gameBase + kSliceCountPushRva);
    if (code[0] != 0x6A || code[1] != kOriginalDrawSegments + 1)
    {
        LogDistance("DrawDistance stays at 32: unexpected slice count.");
        return FALSE;
    }

    code = reinterpret_cast<const BYTE*>(gameBase + kDrawLoopCountRva);
    if (code[0] != 0xBF || ReadDword(code + 1) != kOriginalDrawSegments - 1)
    {
        LogDistance("DrawDistance stays at 32: unexpected draw loop.");
        return FALSE;
    }

    code = reinterpret_cast<const BYTE*>(gameBase + kSetupCallRva);
    if (code[0] != 0xE8 ||
        ReadDword(code + 1) != kSetupFunctionRva - (kSetupCallRva + 5))
    {
        LogDistance("DrawDistance stays at 32: unexpected race setup.");
        return FALSE;
    }

    for (index = 0; index < sizeof(kSectionSetupCalls) / sizeof(DWORD); ++index)
    {
        if (!IsCallTo(kSectionSetupCalls[index], kSectionSetupRva))
        {
            LogDistance("DrawDistance stays at 32: unexpected segment setup.");
            return FALSE;
        }
    }

    for (index = 0; index < sizeof(kSliceObjectsCalls) / sizeof(DWORD); ++index)
    {
        if (!IsCallTo(kSliceObjectsCalls[index], kSliceObjectsRva))
        {
            LogDistance("DrawDistance stays at 32: unexpected scenery drawing.");
            return FALSE;
        }
    }

    // One block in the original layout: cache 1, cache 2, gap, slices.
    slices = segments + 2;
    cacheBytes = slices * kCacheEntrySize;
    block = reinterpret_cast<BYTE*>(Allocate(
        kBlockPadding + 2 * cacheBytes + kCacheToSliceGap +
        slices * kSliceSize + kBlockPadding));
    g_thresholds = reinterpret_cast<LONG*>(Allocate((slices + 2) * sizeof(LONG)));
    table = reinterpret_cast<BYTE*>(Allocate(
        kReciprocalFrontBytes + kNewReciprocalCount * sizeof(LONG)));
    if (block == NULL || g_thresholds == NULL || table == NULL)
    {
        LogDistance("DrawDistance stays at 32: not enough memory.");
        return FALSE;
    }

    g_segments = segments;
    g_cache1 = block + kBlockPadding;
    g_cache2 = g_cache1 + cacheBytes;
    g_slices = g_cache2 + cacheBytes + kCacheToSliceGap;

    // The depth reciprocals: the bytes before the table (read for negative
    // depths, as before), the original entries, then 2^23 / index.
    original = reinterpret_cast<const BYTE*>(GameVa(kReciprocalVa)) -
        kReciprocalFrontBytes;
    for (index = 0; index < kReciprocalFrontBytes + kReciprocalCount * sizeof(LONG); ++index)
    {
        table[index] = original[index];
    }

    g_reciprocal = reinterpret_cast<LONG*>(table + kReciprocalFrontBytes);
    for (index = kReciprocalCount; index < kNewReciprocalCount; ++index)
    {
        g_reciprocal[index] = static_cast<LONG>((1UL << 23) / index);
    }

    for (index = 0; index < kSiteCount; ++index)
    {
        const AddressSite& site = kSites[index].site;

        if (!WriteCodeDword(
                site.rva + site.prefixLength,
                NewAddress(kSites[index].target)))
        {
            LogDistance("DrawDistance: a code change failed; the game may be unstable.");
            return FALSE;
        }
    }

    call[0] = static_cast<BYTE>(segments + 1);
    WriteCode(kSliceCountPushRva + 1, call, 1);
    WriteCodeDword(kDrawLoopCountRva + 1, segments - 1);
    call[0] = 0xE8;
    *reinterpret_cast<DWORD*>(call + 1) = reinterpret_cast<DWORD>(HookedSetup) -
        (gameBase + kSetupCallRva + 5);
    WriteCode(kSetupCallRva, call, sizeof(call));

    for (index = 0; index < sizeof(kSectionSetupCalls) / sizeof(DWORD); ++index)
    {
        RedirectCall(kSectionSetupCalls[index], HookedSectionSetup);
    }

    for (index = 0; index < sizeof(kSliceObjectsCalls) / sizeof(DWORD); ++index)
    {
        RedirectCall(kSliceObjectsCalls[index], HookedSliceObjects);
    }

    {
        CHAR message[96];

        wsprintfA(message, "DrawDistance: the road is drawn %lu segments ahead.", segments);
        LogDistance(message);
    }

    return TRUE;
}

BYTE* DrawDistanceSlices(DWORD gameBase)
{
    if (g_slices != NULL)
    {
        return g_slices;
    }

    return reinterpret_cast<BYTE*>(gameBase - kImageBase + kSlicesVa);
}

DWORD DrawDistanceSegments()
{
    return g_segments;
}
