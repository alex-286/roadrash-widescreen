/**
 * Lets the game draw more road segments ahead of the camera.
 *
 * The road ahead is built as one slice per track segment (FUN_00443D70,
 * called with 33 = segments + 1) and drawn from far to near by
 * FUN_00443AF0, which starts at slice 31 and steps back 31 times. The
 * segments themselves are streamed 80 ahead of the camera (FUN_004553xx:
 * position + 0x5000), so more could be drawn without loading more of the
 * track. The sprites beside the road, however, are loaded for the original
 * distance only, so beyond 33 segments they are not drawn (see
 * HookedSectionSetup).
 *
 * Riders, cars, pedestrians and other world nodes are drawn from a visible
 * list that FUN_00438450 builds every frame: the nodes from just ahead of
 * the camera up to depth (camera & ~0xFF) + 0x2000 (32 segments), at most
 * 50, in course order. Only the drawing reads it (FUN_00438610, per segment
 * from far to near, and the cursor at 0x4385F0), and the game logic does
 * not. The depth limit follows the draw distance; the list moves into this
 * DLL with room for proportionally more nodes (see HookedFarScenery).
 *
 * Arrays sized for 32 segments move into this DLL at the requested size:
 * - the slices (0x224 bytes, segments + 2 of them) and the two row caches
 *   (0x90 bytes, segments + 2 each), kept in their original relative layout
 *   (cache 1, cache 2, 0x30 bytes, slices), because the slice code also
 *   reads the slice before the first one, which lies in cache 2;
 * - the crest thresholds (one per slice), computed as FUN_00444E30 does;
 * - the depth reciprocal table (0x1100 entries, indexed by depth / 2, with
 *   no bounds check), extended by 2^23 / index, which its last entries
 *   already follow.
 *
 * The fixed references to slice 31 (the farthest drawn) and slice 32 (the
 * one after it) move to slices segments - 1 and segments. FUN_00444E30,
 * which sets up the arrays at race start, runs as before and then sets up
 * the new ones the same way.
 *
 * Scenery beyond the original distance: the pictures beside the road come
 * from four family slots (0x49E550); a picture id selects the slot by its
 * top two bits (FUN_0044AC00 and its siblings), whatever family the slot
 * holds. The course lists load events (position, slot, family); the walker
 * 80 segments ahead (0x49EE30) queues each one when it gets near
 * (FUN_00452310), and the loader thread (FUN_0044ABA0) replaces the slot.
 * Each segment's row cache entry (key = its course position) is filled
 * from the slots by FUN_00444630 when the segment appears at the far end
 * (the walls and rock faces beside the road: FUN_0040B8E0, FUN_0040CCB0,
 * FUN_0040CCF0), and kept while the key matches; FUN_0044B630 fills the
 * objects' pictures (trees, buildings) while drawing. The original's 32
 * segments never reach a slot whose family is not loaded yet. Further out,
 * a slot can still hold the previous family: the entry then shows wrong
 * pictures and points into freed memory once the slot is reloaded (the
 * crash at 0x44B6AD).
 *
 * So beyond 33 segments (depth 0x2100, the limit Open Rash uses for the
 * same reason) an entry is filled only while every slot it uses is idle,
 * fully loaded, has no reload queued and no load event still pending at or
 * before the segment: then the slot holds the family the segment needs.
 * Otherwise the entry is emptied and given a negative key, which
 * FUN_00444630 never fills. A far entry is emptied again as soon as one of
 * its slots is locked or changes, before the next frame is drawn (the main
 * thread locks a slot before the loader thread frees it). The scenery a far
 * segment draws through FUN_00438610 is held to the same rule. Once a
 * segment is within 33 segments, an emptied entry gets a key that matches
 * no position, and the game fills it as in the original.
 *
 * @file draw_distance.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "draw_distance.h"
#include "road_rash_patch.h"

/**
 * The preferred load address of ROADRASH.EXE, which its absolute addresses
 * assume.
 */
static const DWORD kImageBase = 0x00400000;
/** The size of a slice in bytes. */
static const DWORD kSliceSize = 0x224;
/** The size of a row cache entry in bytes. */
static const DWORD kCacheEntrySize = 0x90;
/** The bytes between the second row cache and the slices. */
static const DWORD kCacheToSliceGap = 0x30;
/** Spare bytes before and after the moved block. */
static const DWORD kBlockPadding = 0x400;

/** The game's slice array (0x4BC800). */
static const DWORD kSlicesVa = 0x004BC800;
/** The game's first row cache (0x4BA190). */
static const DWORD kCache1Va = 0x004BA190;
/** The game's second row cache (0x4BB4B0). */
static const DWORD kCache2Va = 0x004BB4B0;
/** The game's crest thresholds (0x4C1160). */
static const DWORD kThresholdsVa = 0x004C1160;
/** The heights the crest thresholds are computed from (0x4734A0). */
static const DWORD kThresholdSourceVa = 0x004734A0;
/** The number of entries at kThresholdSourceVa. */
static const DWORD kThresholdSourceCount = 34;
/** The game's threshold after the last drawn slice (0x4C11DC). */
static const DWORD kLastThresholdVa = 0x004C11DC;
/** The game's depth reciprocal table (0x46F098). */
static const DWORD kReciprocalVa = 0x0046F098;
/** The number of entries of the game's reciprocal table. */
static const DWORD kReciprocalCount = 0x1100;
/** The number of entries of the moved reciprocal table. */
static const DWORD kNewReciprocalCount = 0x4000;
/**
 * The bytes before the reciprocal table that are copied too (read for
 * negative depths).
 */
static const DWORD kReciprocalFrontBytes = 0x4000;
/** The reciprocal index of the first crest threshold. */
static const DWORD kThresholdStart = 0x100;
/** The reciprocal index step from one crest threshold to the next. */
static const DWORD kThresholdStep = 0x80;

/** The call of the race setup FUN_00444E30. */
static const DWORD kSetupCallRva = 0x00042A31;
/** FUN_00444E30, which sets up the arrays at race start. */
static const DWORD kSetupFunctionRva = 0x00044E30;
/** The push 33 (segments + 1) before the call of FUN_00443D70. */
static const DWORD kSliceCountPushRva = 0x00043A5A;
/** The mov edi, 31 (segments - 1) of the draw loop in FUN_00443AF0. */
static const DWORD kDrawLoopCountRva = 0x00043AFE;

/** An instruction whose 32-bit operand is an address to move. */
struct AddressSite
{
    /** The relative address of the instruction. */
    DWORD rva;
    /** The bytes of the instruction before the address. */
    BYTE prefix[4];
    /** The number of bytes used in prefix. */
    DWORD prefixLength;
    /** The address the instruction holds in the original game. */
    DWORD original;
};

/** The arrays and places an AddressSite moves to. */
enum Target
{
    /** The first slice. */
    kTargetSlices,
    /** The farthest drawn slice, segments - 1. */
    kTargetFarthestSlice,
    /** The segment of the farthest drawn slice (+ 8). */
    kTargetFarthestSegment,
    /** The first row of the slice after the farthest drawn one (+ 0x14). */
    kTargetNextSliceRow,
    /** The slice after the farthest drawn one. */
    kTargetNextSlice,
    /** The first row cache. */
    kTargetCache1,
    /** The second row cache. */
    kTargetCache2,
    /** The crest thresholds. */
    kTargetThresholds,
    /** The depth reciprocal table. */
    kTargetReciprocal
};

/** An address to move and where it moves to. */
struct TargetedSite
{
    /** The instruction. */
    AddressSite site;
    /** The new place of its address. */
    Target target;
};

/** Every instruction that addresses the moved arrays. */
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

/** The number of entries in kSites. */
static const DWORD kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

/**
 * The segment projection FUN_00444630, which fills a segment's row cache
 * entry.
 */
static const DWORD kSectionSetupRva = 0x00044630;
/** The calls of FUN_00444630 in the road projection. */
static const DWORD kSectionSetupCalls[] =
{
    0x00043E99, 0x0004406B, 0x0004407B, 0x00044266, 0x00044276, 0x000444A0
};
/** FUN_00438610, which draws a segment's objects and scenery. */
static const DWORD kSliceObjectsRva = 0x00038610;
/** The calls of FUN_00438610 in the slice drawing. */
static const DWORD kSliceObjectsCalls[] =
{
    0x00043B6D, 0x00043BC2, 0x00043BF2, 0x00043CA2
};
/** An empty row cache entry the game keeps (0x4C10D0). */
static const DWORD kEmptyEntryVa = 0x004C10D0;
/** The object cursor of the visible list (0x4977BC). */
static const DWORD kObjectCursorVa = 0x004977BC;
/** The object cursor handed to the next segment (0x4977C0). */
static const DWORD kObjectCursorNextVa = 0x004977C0;
/**
 * The depth up to which segments are filled as in the original (33
 * segments).
 */
static const LONG kNearDepth = 0x2100;
/**
 * The negative key of an emptied far entry, which FUN_00444630 never
 * fills.
 */
static const LONG kFarKey = static_cast<LONG>(0x80000002);
/** The key that matches no position, so the game fills the entry again. */
static const LONG kRefillKey = 0x7FFFFFFE;
/** The DWORDs of a row cache entry. */
static const DWORD kEntryDwords = kCacheEntrySize / sizeof(DWORD);

/** The number of family slots. */
static const DWORD kSlotCount = 4;
/** The family each slot holds, negative while it loads (0x4B7F10). */
static const DWORD kSlotFamilyVa = 0x004B7F10;
/**
 * The lock of each slot, set during a lookup and during a whole reload
 * (0x49E630).
 */
static const DWORD kSlotLockVa = 0x0049E630;
/** The number of slots in use (0x4755F8). */
static const DWORD kSlotsInUseVa = 0x004755F8;
/** The queued loads (FUN_0044B120): entries of family, slot, mode. */
static const DWORD kLoadQueueVa = 0x0049E5D0;
/** The write index of the load queue. */
static const DWORD kLoadQueueWriteVa = 0x004755F0;
/** The read index of the load queue. */
static const DWORD kLoadQueueReadVa = 0x004755F4;
/** The number of entries of the load queue. */
static const DWORD kLoadQueueLength = 8;
/** The load mode that replaces the slot's family. */
static const DWORD kLoadModeReplace = 3;
/**
 * The walker 80 segments ahead (0x49EE30); its first DWORD is its course
 * position.
 */
static const DWORD kFrontWalkerVa = 0x0049EE30;
/** The DWORD index of the walker's active branch. */
static const DWORD kWalkerActiveBranch = 1;
/** The DWORD index of the walker's branch mode. */
static const DWORD kWalkerBranchMode = 7;
/** The branch mode in which both branches of a split are walked. */
static const DWORD kWalkerBothBranches = 2;
/** The offset of the first branch in the walker. */
static const DWORD kBranchOffset = 0x28;
/** The size of a branch in the walker. */
static const DWORD kBranchSize = 0x1F4;
/**
 * The offset of a branch's load events: table, position in the section,
 * index of the next event, forward flag at + 0x10.
 */
static const DWORD kBranchEventsOffset = 0x70;
/** The size of a load event: position in segments, state, slot, family. */
static const DWORD kEventSize = 0xC;
/** The offset of the first event in an event table (+ 8 holds their count). */
static const DWORD kEventsFirstOffset = 0x18;
/** The most events an event table is trusted to hold. */
static const LONG kMaxEvents = 0x1000;
/** The course length of a segment. */
static const DWORD kSegmentUnit = 0x100;

/**
 * A far entry filled from the slots: its key, the slots it used and the
 * families they held.
 */
struct FarFill
{
    /** TRUE when the entry was filled from ready slots. */
    BOOL valid;
    /** The key (course position) the entry was filled for. */
    LONG key;
    /** One bit per slot the entry's pictures come from. */
    DWORD slots;
    /** The family each slot held when the entry was filled. */
    LONG family[kSlotCount];
};

/** The bookkeeping of every entry of both moved row caches. */
static FarFill g_farFills[2 * (kMaxDrawSegments + 2)];

/** The segment projection FUN_00444630. */
typedef void (__fastcall *SectionSetupFn)(BYTE* section, BYTE* record, LONG* projection);
/** FUN_00438610, which draws a segment's objects and scenery. */
typedef void (__fastcall *SliceObjectsFn)(LONG* record, int from, int to);

/**
 * FUN_0044B980, which draws the + 0x3D objects of a segment whose layer
 * bit is in layers (FUN_00438610 draws kSceneryFirstLayers before the
 * riders and kSceneryOtherLayers after them).
 */
typedef void (__fastcall *SceneryFn)(LONG* record, DWORD layers, int from, int to);
/** FUN_0044B5F0, which draws the object at + 0x60 of a segment. */
typedef void (__fastcall *SingleSceneryFn)(LONG* record, int from, int to);
/** FUN_0044B980. */
static const DWORD kSceneryRva = 0x0004B980;
/** FUN_0044B5F0. */
static const DWORD kSingleSceneryRva = 0x0004B5F0;
/** The layers drawn before the riders. */
static const DWORD kSceneryFirstLayers = 0x00000FF8;
/** The layers drawn after the riders. */
static const DWORD kSceneryOtherLayers = 0xFFFFF007;
/** Where FUN_00438610 calls FUN_0044B980, checked at install. */
static const DWORD kSceneryCalls[] = { 0x0003864C, 0x000386D5, 0x00038764 };
/** Where FUN_00438610 calls FUN_0044B5F0, checked at install. */
static const DWORD kSingleSceneryCalls[] = { 0x000386EA, 0x00038779 };

/**
 * The visible list of FUN_00438450: 50 node pointers at 0x4C3D30, filled
 * 1-based through 0x4C3D2C.
 *
 * FUN_00438610 also reads the entry before the first one (when the cursor
 * is -1), so the moved list keeps a spare entry in front.
 */
static const DWORD kVisibleListVa = 0x004C3D30;
/** The length of the game's visible list. */
static const DWORD kOriginalVisibleCount = 50;
/** The length of the moved visible list at the largest draw distance. */
static const DWORD kMaxVisibleCount =
    (kOriginalVisibleCount * kMaxDrawSegments + kOriginalDrawSegments - 1) /
    kOriginalDrawSegments;
/** Every instruction that addresses the visible list. */
static const AddressSite kVisibleListSites[] =
{
    // FUN_00438450: mov [esi * 4 + list - 4], ebx
    { 0x0003852E, { 0x89, 0x1C, 0xB5 }, 3, 0x004C3D2C },
    // FUN_00438610: mov ebx, [eax * 4 + list] / lea esi, [eax * 4 + list]
    { 0x00038669, { 0x8B, 0x1C, 0x85 }, 3, 0x004C3D30 },
    { 0x00038673, { 0x8D, 0x34, 0x85 }, 3, 0x004C3D30 },
    // FUN_00438610: cmp esi, list (end of the walk)
    { 0x00038727, { 0x81, 0xFE }, 2, 0x004C3D30 },
};
/** The add eax, 0x2000 (the depth limit) in FUN_00438450. */
static const DWORD kVisibleDepthRva = 0x000384C0;
/** The mov edx, 50 (the list length) in FUN_00438450. */
static const DWORD kVisibleCountRva = 0x000384C9;

/** The race setup FUN_00444E30. */
typedef void (__cdecl *SetupFn)(void);

/** Module base of ROADRASH.EXE. */
static DWORD g_base = 0;
/** The number of segments drawn ahead. */
static DWORD g_segments = kOriginalDrawSegments;
/** The moved first row cache, or NULL. */
static BYTE* g_cache1 = NULL;
/** The moved second row cache, or NULL. */
static BYTE* g_cache2 = NULL;
/** The moved slices, or NULL. */
static BYTE* g_slices = NULL;
/** The moved crest thresholds, or NULL. */
static LONG* g_thresholds = NULL;
/** The moved depth reciprocal table, or NULL. */
static LONG* g_reciprocal = NULL;
/**
 * TRUE when the visible list was moved, so riders, cars and pedestrians
 * are drawn as far as the road.
 */
static BOOL g_farObjects = FALSE;
/** The moved visible list, with the spare entry in front. */
static DWORD g_visibleList[1 + kMaxVisibleCount];

/**
 * Writes a line to the log.
 *
 * @param [in] text The line, without line break.
 */
static void LogDistance(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

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
 * Overwrites bytes of the game's code.
 *
 * @param [in] rva The relative address of the first byte.
 * @param [in] bytes The new bytes.
 * @param [in] length The number of bytes.
 * @return FALSE when the code could not be made writable.
 */
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

/**
 * Overwrites a DWORD of the game's code.
 *
 * @param [in] rva The relative address of the DWORD.
 * @param [in] value The new value.
 * @return FALSE when the code could not be made writable.
 */
static BOOL WriteCodeDword(DWORD rva, DWORD value)
{
    BYTE bytes[4];

    bytes[0] = static_cast<BYTE>(value);
    bytes[1] = static_cast<BYTE>(value >> 8);
    bytes[2] = static_cast<BYTE>(value >> 16);
    bytes[3] = static_cast<BYTE>(value >> 24);
    return WriteCode(rva, bytes, sizeof(bytes));
}

/**
 * Tells whether an instruction is as in the original game.
 *
 * @param [in] site The instruction.
 * @return TRUE when its bytes and address match.
 */
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

/**
 * Tells whether code is as in the original game.
 *
 * @param [in] rva The relative address of the code.
 * @param [in] bytes The expected bytes.
 * @param [in] length The number of bytes.
 * @return TRUE when all bytes match.
 */
static BOOL MatchesBytes(DWORD rva, const BYTE* bytes, DWORD length)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + rva);
    DWORD index;

    for (index = 0; index < length; ++index)
    {
        if (code[index] != bytes[index])
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * Gives a moved slice.
 *
 * @param [in] index The slice, 0 for the nearest.
 * @return The slice.
 */
static BYTE* Slice(DWORD index)
{
    return g_slices + index * kSliceSize;
}

/**
 * Gives the new address for a moved site.
 *
 * @param [in] target The place the site moves to.
 * @return The address in the moved arrays.
 */
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

/**
 * Replaces the call of FUN_00444E30 at race start: sets up the original
 * arrays, then the moved ones the same way.
 */
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
        g_farFills[index].valid = FALSE;
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

/**
 * Reads a variable of the game.
 *
 * @tparam T The type of the variable.
 * @param [in] va The address for the preferred image base.
 * @return The value.
 */
template <typename T>
static T GameValue(DWORD va)
{
    return *reinterpret_cast<const T*>(GameVa(va));
}

/**
 * Gives the slots whose pictures go into a segment's entry.
 *
 * Those FUN_00444630 fills by the segment's type (+ 0x3C: picture ids at
 * + 0x148 and + 0x149, type 0 skipping 0xFF, and for type 3 also at
 * + 0x39), and those of its scenery, which FUN_0044B630 caches there while
 * drawing: + 0x3D objects of 16 bytes from + 0x40 (id in their first byte),
 * and, when + 0x60 is set, the object at + 0x60 (id at + 0x63).
 *
 * @param [in] section The course section of the segment.
 * @return One bit per slot.
 */
static DWORD SectionSlots(const BYTE* section)
{
    BYTE type = section[0x3C];
    DWORD slots = 0;
    DWORD index;

    if (type == 0 || type == 1 || type == 3)
    {
        for (index = 0; index < 2; ++index)
        {
            BYTE id = section[0x148 + index];

            if (type != 0 || id != 0xFF)
            {
                slots |= 1UL << (id >> 6);
            }
        }

        if (type == 3)
        {
            slots |= 1UL << (section[0x39] >> 6);
        }
    }

    for (index = 0; index < section[0x3D]; ++index)
    {
        slots |= 1UL << (section[0x40 + index * 0x10] >> 6);
    }

    if (section[0x60] != 0)
    {
        slots |= 1UL << (section[0x63] >> 6);
    }

    return slots;
}

/**
 * Tells whether the game uses a slot.
 *
 * @param [in] slot The slot.
 * @return TRUE when the slot is below the number of slots in use.
 */
static BOOL SlotInUse(DWORD slot)
{
    return static_cast<LONG>(slot) < GameValue<LONG>(kSlotsInUseVa);
}

/**
 * Gives the family a slot holds.
 *
 * @param [in] slot The slot.
 * @return The family, negative while it loads.
 */
static LONG SlotFamily(DWORD slot)
{
    return GameValue<LONG>(kSlotFamilyVa + slot * sizeof(LONG));
}

/**
 * Tells whether a slot is locked.
 *
 * @param [in] slot The slot.
 * @return TRUE during a lookup and during a whole reload.
 */
static BOOL SlotLocked(DWORD slot)
{
    return GameValue<DWORD>(kSlotLockVa + slot * sizeof(DWORD)) != 0;
}

/**
 * Tells whether a load that replaces a slot's family is queued.
 *
 * @param [in] slot The slot.
 * @return TRUE when such a load is queued.
 */
static BOOL ReplaceQueued(DWORD slot)
{
    DWORD read = GameValue<DWORD>(kLoadQueueReadVa) % kLoadQueueLength;
    DWORD write = GameValue<DWORD>(kLoadQueueWriteVa) % kLoadQueueLength;

    while (read != write)
    {
        const DWORD* load = reinterpret_cast<const DWORD*>(
            GameVa(kLoadQueueVa) + read * 3 * sizeof(DWORD));

        if (load[1] == slot && load[2] == kLoadModeReplace)
        {
            return TRUE;
        }

        read = (read + 1) % kLoadQueueLength;
    }

    return FALSE;
}

/**
 * Tells whether the walker ahead still has a load event for a slot at or
 * before a course position.
 *
 * The slot then does not hold that segment's family yet. Conservative by
 * one segment.
 *
 * @param [in] slot The slot.
 * @param [in] key The course position of the segment.
 * @return TRUE when a load event is pending (or the events are not
 *         trusted).
 */
static BOOL LoadPending(DWORD slot, LONG key)
{
    const BYTE* walker = reinterpret_cast<const BYTE*>(GameVa(kFrontWalkerVa));
    const LONG* walkerDwords = reinterpret_cast<const LONG*>(walker);
    LONG position = walkerDwords[0];
    DWORD branch;

    for (branch = 0; branch < 2; ++branch)
    {
        const BYTE* events;
        const BYTE* table;
        LONG sectionPosition;
        LONG next;
        LONG count;

        if (static_cast<DWORD>(walkerDwords[kWalkerBranchMode]) != kWalkerBothBranches &&
            static_cast<DWORD>(walkerDwords[kWalkerActiveBranch]) != branch)
        {
            continue;
        }

        events = walker + kBranchOffset + branch * kBranchSize + kBranchEventsOffset;
        table = *reinterpret_cast<BYTE* const*>(events);
        if (table == NULL || events[0x10] == 0)
        {
            continue;
        }

        sectionPosition = *reinterpret_cast<const LONG*>(events + 4);
        next = *reinterpret_cast<const LONG*>(events + 8);
        count = *reinterpret_cast<const LONG*>(table + 8);
        if (next < 0 || count < 0 || count > kMaxEvents)
        {
            return TRUE;
        }

        for (; next < count; ++next)
        {
            const BYTE* event = table + kEventsFirstOffset + next * kEventSize;
            LONG eventPosition = position - sectionPosition +
                *reinterpret_cast<const LONG*>(event) * static_cast<LONG>(kSegmentUnit);

            if (eventPosition - static_cast<LONG>(kSegmentUnit) > key)
            {
                break;
            }

            if (event[5] == slot)
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * Tells whether a far segment's pictures can come from the slots now.
 *
 * @param [in] slots One bit per slot the pictures come from.
 * @param [in] key The course position of the segment.
 * @return TRUE when every slot is idle, loaded and holds the family the
 *         segment needs.
 */
static BOOL SlotsReady(DWORD slots, LONG key)
{
    DWORD slot;

    for (slot = 0; slot < kSlotCount; ++slot)
    {
        if ((slots & (1UL << slot)) == 0 || !SlotInUse(slot))
        {
            continue;
        }

        if (SlotLocked(slot) || SlotFamily(slot) <= 0 ||
            ReplaceQueued(slot) || LoadPending(slot, key))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * Tells whether a far entry filled earlier still points at the families it
 * was filled from.
 *
 * @param [in] fill The bookkeeping of the entry.
 * @return TRUE when no slot it used is locked or changed.
 */
static BOOL FillStillValid(const FarFill* fill)
{
    DWORD slot;

    for (slot = 0; slot < kSlotCount; ++slot)
    {
        if ((fill->slots & (1UL << slot)) == 0 || !SlotInUse(slot))
        {
            continue;
        }

        if (SlotLocked(slot) || SlotFamily(slot) != fill->family[slot] ||
            ReplaceQueued(slot))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * Records that a far entry was filled from the slots.
 *
 * @param [out] fill The bookkeeping of the entry.
 * @param [in] key The course position the entry was filled for.
 * @param [in] slots One bit per slot its pictures come from.
 */
static void RecordFill(FarFill* fill, LONG key, DWORD slots)
{
    DWORD slot;

    fill->valid = TRUE;
    fill->key = key;
    fill->slots = slots;
    for (slot = 0; slot < kSlotCount; ++slot)
    {
        fill->family[slot] = SlotFamily(slot);
    }
}

/**
 * Empties a row cache entry and gives it kFarKey.
 *
 * @param [in,out] entry The entry.
 */
static void EmptyEntry(LONG* entry)
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

/**
 * Gives the bookkeeping of an entry in the moved caches.
 *
 * @param [in] entry The entry.
 * @return The bookkeeping, or NULL for an entry outside the moved caches.
 */
static FarFill* FillOf(const LONG* entry)
{
    DWORD offset = reinterpret_cast<DWORD>(entry) - reinterpret_cast<DWORD>(g_cache1);
    DWORD index = offset / kCacheEntrySize;

    if (reinterpret_cast<const BYTE*>(entry) < g_cache1 ||
        offset % kCacheEntrySize != 0 ||
        index >= 2 * (g_segments + 2))
    {
        return NULL;
    }

    return &g_farFills[index];
}

/**
 * Replaces the calls of FUN_00444630: fills a far segment's row cache
 * entry only from ready slots.
 *
 * @param [in] section The course section of the segment.
 * @param [in,out] record The segment record: + 0xFC its row cache entry,
 *                 + 8 the segment, whose position (+ 8) is the entry's key.
 * @param [in,out] projection The game's block p[0..9]; projection[4] is the
 *                 segment's depth.
 */
static void __fastcall HookedSectionSetup(
    BYTE* section,
    BYTE* record,
    LONG* projection)
{
    LONG* entry = *reinterpret_cast<LONG**>(record + 0xFC);
    LONG key = *reinterpret_cast<const LONG*>(*reinterpret_cast<BYTE**>(record + 8) + 8);
    FarFill* fill = FillOf(entry);
    BOOL recordFill = FALSE;
    DWORD slots = 0;

    if (projection[4] > kNearDepth)
    {
        slots = SectionSlots(section);
        if (fill == NULL)
        {
            EmptyEntry(entry);
        }
        else if (entry[0] == key)
        {
            // Filled before: still from the same families? (An entry
            // filled within 33 segments is checked like a new one.)
            if (fill->valid && fill->key == key ?
                    !FillStillValid(fill) :
                    !SlotsReady(slots, key))
            {
                fill->valid = FALSE;
                EmptyEntry(entry);
            }
            else if (!fill->valid || fill->key != key)
            {
                RecordFill(fill, key, slots);
            }
        }
        else if (SlotsReady(slots, key))
        {
            // The game fills an entry whose key does not match; a negative
            // key it leaves alone.
            if (entry[0] == kFarKey)
            {
                entry[0] = kRefillKey;
            }

            recordFill = TRUE;
        }
        else
        {
            fill->valid = FALSE;
            EmptyEntry(entry);
        }
    }
    else
    {
        if (fill != NULL)
        {
            fill->valid = FALSE;
        }

        if (entry[0] == kFarKey)
        {
            entry[0] = kRefillKey;
        }
    }

    reinterpret_cast<SectionSetupFn>(g_base + kSectionSetupRva)(
        section, record, projection);

    if (recordFill && entry[0] == key)
    {
        RecordFill(fill, key, slots);
    }
}

/**
 * Tells whether a segment's scenery (trees, buildings, signs) may be
 * drawn.
 *
 * Always within 33 segments; beyond, only while its entry was filled from
 * loaded slots that still hold the same families (see HookedSectionSetup).
 *
 * @param [in] record The segment record: record[6] its depth, record[2] the
 *             segment (its position at + 8), record[3] its course section,
 *             record[0x3F] its row cache entry.
 * @return TRUE when the scenery may be drawn.
 */
static BOOL SceneryReady(const LONG* record)
{
    const BYTE* section;
    const LONG* entry;
    const FarFill* fill;
    LONG key;

    if (record[6] <= kNearDepth)
    {
        return TRUE;
    }

    section = reinterpret_cast<const BYTE*>(record[3]);
    entry = reinterpret_cast<const LONG*>(record[0x3F]);
    key = *reinterpret_cast<const LONG*>(record[2] + 8);
    fill = FillOf(entry);
    return fill != NULL && fill->valid && fill->key == key && entry[0] == key &&
        (SectionSlots(section) & ~fill->slots) == 0 && FillStillValid(fill);
}

/**
 * Replaces the calls of FUN_0044B980 in FUN_00438610.
 *
 * With the visible list reaching as far as the road, FUN_00438610 draws
 * every segment, interleaving riders, cars and pedestrians with the scenery
 * as in the original; its scenery calls come here and are skipped for a far
 * segment whose pictures are not ready.
 *
 * @param [in] record The segment record.
 * @param [in] layers The layers to draw.
 * @param [in] from Passed on to the game.
 * @param [in] to Passed on to the game.
 */
static void __fastcall HookedFarScenery(LONG* record, DWORD layers, int from, int to)
{
    if (SceneryReady(record))
    {
        reinterpret_cast<SceneryFn>(g_base + kSceneryRva)(record, layers, from, to);
    }
}

/**
 * Replaces the calls of FUN_0044B5F0 in FUN_00438610, skipped like
 * HookedFarScenery.
 *
 * @param [in] record The segment record.
 * @param [in] from Passed on to the game.
 * @param [in] to Passed on to the game.
 */
static void __fastcall HookedFarSingleScenery(LONG* record, int from, int to)
{
    if (SceneryReady(record))
    {
        reinterpret_cast<SingleSceneryFn>(g_base + kSingleSceneryRva)(record, from, to);
    }
}

/**
 * Replaces the calls of FUN_00438610 in the slice drawing.
 *
 * Without the longer visible list (its code differed from the original),
 * FUN_00438610 is not called beyond 33 segments: the riders, cars and
 * pedestrians it draws exist only up to 32 segments there, and the list's
 * order does not hold further out. The object cursor is handed on
 * unchanged, as by a segment without objects. The segment's scenery is drawn
 * as FUN_00438610 draws it when no object is in the way.
 *
 * @param [in] record The segment record.
 * @param [in] from Passed on to the game.
 * @param [in] to Passed on to the game.
 */
static void __fastcall HookedSliceObjects(LONG* record, int from, int to)
{
    const BYTE* section;

    if (record[6] <= kNearDepth || g_farObjects)
    {
        reinterpret_cast<SliceObjectsFn>(g_base + kSliceObjectsRva)(record, from, to);
        return;
    }

    *reinterpret_cast<LONG*>(GameVa(kObjectCursorNextVa)) =
        *reinterpret_cast<LONG*>(GameVa(kObjectCursorVa));

    if (!SceneryReady(record))
    {
        return;
    }

    section = reinterpret_cast<const BYTE*>(record[3]);
    if (section[0x3D] != 0)
    {
        reinterpret_cast<SceneryFn>(g_base + kSceneryRva)(
            record, kSceneryFirstLayers, from, to);
        reinterpret_cast<SceneryFn>(g_base + kSceneryRva)(
            record, kSceneryOtherLayers, from, to);
    }

    if (section[0x60] != 0)
    {
        reinterpret_cast<SingleSceneryFn>(g_base + kSingleSceneryRva)(
            record, from, to);
    }
}

/**
 * Tells whether the game's code holds a call of a function.
 *
 * @param [in] rva The relative address of the instruction.
 * @param [in] targetRva The relative address of the function.
 * @return TRUE when the code is an E8 call to it.
 */
static BOOL IsCallTo(DWORD rva, DWORD targetRva)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + rva);

    return code[0] == 0xE8 && ReadDword(code + 1) == targetRva - (rva + 5);
}

/**
 * Points a call in the game's code to a hook.
 *
 * @param [in] rva The relative address of the E8 call.
 * @param [in] hook The new target.
 */
static void RedirectCall(DWORD rva, const void* hook)
{
    BYTE call[5];

    call[0] = 0xE8;
    *reinterpret_cast<DWORD*>(call + 1) =
        reinterpret_cast<DWORD>(hook) - (g_base + rva + 5);
    WriteCode(rva, call, sizeof(call));
}

/**
 * Allocates zeroed memory with VirtualAlloc.
 *
 * @param [in] bytes The size in bytes.
 * @return The memory, or NULL.
 */
static void* Allocate(DWORD bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

BOOL DrawDistanceInstall(DWORD gameBase, DWORD segments)
{
    BYTE call[5];
    BOOL farObjects;
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

    for (index = 0; index < sizeof(kSceneryCalls) / sizeof(DWORD); ++index)
    {
        if (!IsCallTo(kSceneryCalls[index], kSceneryRva))
        {
            LogDistance("DrawDistance stays at 32: unexpected scenery drawing.");
            return FALSE;
        }
    }

    for (index = 0; index < sizeof(kSingleSceneryCalls) / sizeof(DWORD); ++index)
    {
        if (!IsCallTo(kSingleSceneryCalls[index], kSingleSceneryRva))
        {
            LogDistance("DrawDistance stays at 32: unexpected scenery drawing.");
            return FALSE;
        }
    }

    // The visible list: optional, the road works without it.
    {
        static const BYTE depth[5] = { 0x05, 0x00, 0x20, 0x00, 0x00 };
        static const BYTE count[5] =
        {
            0xBA, static_cast<BYTE>(kOriginalVisibleCount), 0x00, 0x00, 0x00
        };

        farObjects =
            MatchesBytes(kVisibleDepthRva, depth, sizeof(depth)) &&
            MatchesBytes(kVisibleCountRva, count, sizeof(count));
        for (index = 0;
             farObjects &&
             index < sizeof(kVisibleListSites) / sizeof(kVisibleListSites[0]);
             ++index)
        {
            farObjects = MatchesSite(kVisibleListSites[index]);
        }

        if (!farObjects)
        {
            LogDistance("DrawDistance: unexpected code in the object list; riders, cars and pedestrians stay at 32 segments.");
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

    if (farObjects)
    {
        DWORD listBase = reinterpret_cast<DWORD>(&g_visibleList[1]);
        DWORD visibleCount =
            (kOriginalVisibleCount * segments + kOriginalDrawSegments - 1) /
            kOriginalDrawSegments;
        CHAR message[112];

        for (index = 0;
             index < sizeof(kVisibleListSites) / sizeof(kVisibleListSites[0]);
             ++index)
        {
            const AddressSite& site = kVisibleListSites[index];

            WriteCodeDword(
                site.rva + site.prefixLength,
                listBase + (site.original - kVisibleListVa));
        }

        WriteCodeDword(kVisibleDepthRva + 1, segments * kSegmentUnit);
        WriteCodeDword(kVisibleCountRva + 1, visibleCount);

        for (index = 0; index < sizeof(kSceneryCalls) / sizeof(DWORD); ++index)
        {
            RedirectCall(kSceneryCalls[index], HookedFarScenery);
        }

        for (index = 0; index < sizeof(kSingleSceneryCalls) / sizeof(DWORD); ++index)
        {
            RedirectCall(kSingleSceneryCalls[index], HookedFarSingleScenery);
        }

        g_farObjects = TRUE;
        wsprintfA(
            message,
            "DrawDistance: riders, cars and pedestrians appear %lu segments ahead (up to %lu).",
            segments,
            visibleCount);
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
