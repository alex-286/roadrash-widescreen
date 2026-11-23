/**
 * Keeps the road from jumping by a few pixels while riding.
 *
 * FUN_00443D70 (called once per frame at 0x443A5C) projects the road ahead
 * segment by segment. The camera stands at frac = position & 0xFF within
 * its segment, so the first segment boundary lies 0x100 - frac ahead. For
 * each road (one, or two where the road splits) it keeps a block of ten
 * values p[0..9] and calls FUN_00444630 once per segment, which turns the
 * block into screen coordinates (16.16 in the 320x240 space) and steps it
 * on:
 *
 * @verbatim
 *   p[0] height step (section + 0xBC)   p[1] height, p[1] += p[0]
 *   p[2] side step                      p[3] side offset, p[3] += p[2]
 *   p[4] depth, += 0x100                p[5], p[6] camera side and height
 *   p[7] out: top of the road           p[8], p[9] the previous segment
 *   screen y = (p[6] - p[1]) * scale + 0x5A0000
 *   screen x = (p[3] - p[5] + edge) * scale + centre, scale = 1 / depth
 * @endverbatim
 *
 * The first block is started in whole units:
 *
 * @verbatim
 *   p[1] = -((frac * slope) >> 8)          drops rem = (frac * slope) & 0xFF
 *   p[2] = (curve * (0x100 - frac)) / 256  drops eps = the rest of the division
 * @endverbatim
 *
 * Every later segment inherits both: the road is rem / 256 units too high,
 * and segment k lies k * eps / 256 units to the side. When frac grows, the
 * whole parts change in steps, so the road and its edges jump by up to a
 * few pixels near the camera (twice as many at 960 lines).
 *
 * After each plain segment (section type + 0x3C = 0) this adds
 * rem * scale / 256 to its screen y values and k * eps * scale / 256 to its
 * screen x values, and the same to the far edge of the previous segment,
 * which the game copies from this one. eps is the same for both roads (the
 * second one is started from the first one's side step, or the other way
 * round); rem is per road, and a block that the game copies from the other
 * road takes that road's rem. Anything unexpected is left as the game
 * computed it.
 *
 * @file smooth_road.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "smooth_road.h"
#include "road_rash_patch.h"

/** The call of the road projection, FUN_00443D70, once per frame. */
static const DWORD kSetupCallRva = 0x00043A5C;
/** The road projection, FUN_00443D70. */
static const DWORD kSetupRva = 0x00043D70;
/** The projection of one segment, FUN_00444630. */
static const DWORD kSectionRva = 0x00044630;
/** The calls of FUN_00444630 in the road projection. */
static const DWORD kSectionCalls[] =
{
    0x00043E99, 0x0004406B, 0x0004407B, 0x00044266, 0x00044276, 0x000444A0
};
/** The number of entries in kSectionCalls. */
static const DWORD kSectionCallCount = sizeof(kSectionCalls) / sizeof(kSectionCalls[0]);

/**
 * The ring of the 128 segments around the camera.
 *
 * A node holds its road state (byte + 0x1C: 1 = one road, 2 = split) and the
 * course sections of both roads (+ 0x0C, + 0x10).
 */
static const DWORD kNodeRingRva = 0x0009E820;

/**
 * The lea edi, [eax * 2 + table] in FUN_00443940 that names the depth
 * reciprocal table, which DrawDistance may have moved.
 */
static const DWORD kReciprocalLeaRva = 0x00043950;
/** The game's own address of the depth reciprocal table. */
static const DWORD kReciprocalVa = 0x0046F098;

/** The offset of the scale (1 / depth) in a segment record. */
static const DWORD kScaleOffset = 0x20;
/**
 * The offsets of the screen y values in a segment record (FUN_00444630,
 * type 0).
 */
static const DWORD kYOffsets[] = { 0x38, 0x4C, 0x54, 0x5C, 0x64, 0x6C };
/**
 * The offsets of the screen x values in a segment record (FUN_00444630,
 * type 0).
 */
static const DWORD kXOffsets[] = { 0x40, 0x44, 0x48, 0x50, 0x58, 0x60, 0x68 };

/** The road projection, FUN_00443D70. */
typedef void (__fastcall *SetupFn)(DWORD, LONG*, int, LONG*, int);
/** The projection of one segment, FUN_00444630: section, record, block. */
typedef void (__fastcall *SectionFn)(BYTE*, BYTE*, LONG*);

/** What is known about the block of values the game keeps for one road. */
struct RoadBlock
{
    /** The game's block p[0..9]. */
    LONG* block;
    /** TRUE once a segment of the block was projected this frame. */
    BOOL seen;
    /**
     * TRUE when the block was started as expected, so the correction applies.
     */
    BOOL valid;
    /** The height fraction the game dropped, in 1/256 units. */
    LONG rem;
    /** p[1] before the last projected segment. */
    LONG entryHeight;
    /** p[4] before the last projected segment. */
    LONG entryDepth;
    /** p[1] after the last projected segment, expected for the next one. */
    LONG nextHeight;
    /** p[4] after the last projected segment, expected for the next one. */
    LONG nextDepth;
    /** The segment record projected last. */
    BYTE* lastRecord;
    /** The side correction applied to lastRecord, in 1/256 units. */
    LONG lastX;
    /** The height correction applied to lastRecord, in 1/256 units. */
    LONG lastY;
};

/** Module base of ROADRASH.EXE. */
static DWORD g_base = 0;
/** The road projection that HookedSetup calls. */
static SetupFn g_setupNext = NULL;
/**
 * The segment projection that HookedSection calls: the game's, or
 * DrawDistance's hook in front of it.
 */
static SectionFn g_sectionNext = NULL;
/** TRUE while the game projects the road of a frame. */
static BOOL g_active = FALSE;
/** The camera's position within its segment, 0-255. */
static LONG g_frac = 0;
/** The depth of the first segment boundary, 0x100 - g_frac. */
static LONG g_firstDepth = 0;
/** The rest the game dropped when it divided the first side step. */
static LONG g_eps = 0;
/** The first side step as the game computes it. */
static LONG g_firstSideStep = 0;
/** TRUE when g_eps applies to this frame. */
static BOOL g_epsValid = FALSE;
/** The road (0 or 1) whose curve the game used for the first side step. */
static int g_epsBlock = 0;
/** The blocks of the one or two roads of this frame. */
static RoadBlock g_blocks[2];
/** The number of entries used in g_blocks. */
static DWORD g_blockCount = 0;

/**
 * Writes a line to the log.
 *
 * @param [in] text The line, without line break.
 */
static void LogSmooth(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

/**
 * Reads an unaligned little-endian DWORD.
 *
 * @param [in] bytes The four bytes.
 * @return The value.
 */
static DWORD ReadDword(const BYTE* bytes)
{
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
        (static_cast<DWORD>(bytes[3]) << 24);
}

/**
 * Gives the target of a call in the game's code.
 *
 * @param [in] rva The relative address of the instruction.
 * @return The absolute target of the E8 call at rva, or 0 when it is no
 *         call.
 */
static DWORD CallTarget(DWORD rva)
{
    const BYTE* code = reinterpret_cast<const BYTE*>(g_base + rva);

    if (code[0] != 0xE8)
    {
        return 0;
    }

    return g_base + rva + 5 + ReadDword(code + 1);
}

/**
 * Points a call in the game's code to a hook.
 *
 * @param [in] rva The relative address of the E8 call.
 * @param [in] hook The new target.
 * @return FALSE when the code could not be made writable.
 */
static BOOL RedirectCall(DWORD rva, const void* hook)
{
    BYTE* code = reinterpret_cast<BYTE*>(g_base + rva);
    DWORD relative = reinterpret_cast<DWORD>(hook) - (g_base + rva + 5);
    DWORD oldProtection;
    DWORD ignoredProtection;
    DWORD index;

    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        return FALSE;
    }

    code[0] = 0xE8;
    for (index = 0; index < 4; ++index)
    {
        code[1 + index] = static_cast<BYTE>(relative >> (index * 8));
    }

    VirtualProtect(code, 5, oldProtection, &ignoredProtection);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    return TRUE;
}

/**
 * Converts a world correction to screen units.
 *
 * @param [in] fraction The correction, in 1/256 units.
 * @param [in] scale The scale of the segment (1 / depth).
 * @return fraction * scale / 256.
 */
static LONG Scaled(LONG fraction, LONG scale)
{
    return fraction != 0 ? MulDiv(fraction, scale, 256) : 0;
}

/**
 * Reads the depth reciprocal table, wherever DrawDistance put it.
 *
 * @param [in] depth The depth.
 * @return The scale (1 / depth).
 */
static LONG Reciprocal(LONG depth)
{
    const BYTE* table = reinterpret_cast<const BYTE*>(
        ReadDword(reinterpret_cast<const BYTE*>(g_base + kReciprocalLeaRva + 3)));

    return *reinterpret_cast<const LONG*>(table + (depth & ~1) * 2);
}

/**
 * Finds the entry of a block of the game's values.
 *
 * @param [in] block The game's block p[0..9].
 * @param [in] add TRUE to add an empty entry when there is none.
 * @return The entry, or NULL when there is none and none can be added.
 */
static RoadBlock* FindBlock(LONG* block, BOOL add)
{
    DWORD index;

    for (index = 0; index < g_blockCount; ++index)
    {
        if (g_blocks[index].block == block)
        {
            return &g_blocks[index];
        }
    }

    if (!add || g_blockCount >= 2)
    {
        return NULL;
    }

    {
        // No ZeroMemory: it needs memset from the C runtime.
        static const RoadBlock empty = { NULL };

        g_blocks[g_blockCount] = empty;
    }

    g_blocks[g_blockCount].block = block;
    return &g_blocks[g_blockCount++];
}

/**
 * Gives the world corrections applied to a record, if it is the last one a
 * block handled.
 *
 * @param [in] record The segment record, or NULL.
 * @param [out] x Receives the side correction, in 1/256 units, or 0.
 * @param [out] y Receives the height correction, in 1/256 units, or 0.
 */
static void LastCorrection(const BYTE* record, LONG* x, LONG* y)
{
    DWORD index;

    *x = 0;
    *y = 0;
    for (index = 0; index < g_blockCount; ++index)
    {
        if (record != NULL && g_blocks[index].lastRecord == record)
        {
            *x = g_blocks[index].lastX;
            *y = g_blocks[index].lastY;
            return;
        }
    }
}

/**
 * Corrects the extra point projected where the road width changes.
 *
 * Where the road width changes within the previous segment (its section is
 * plain, flags + 0x37 & 0x18), FUN_00444630 projects one more point on its
 * left (+ 0xA0, at angle + 0x138) or right edge (+ 0xA8, angle + 0x13C),
 * between that segment and this one at t / 256 of the way (0x100 - t with
 * flag 8), from their uncorrected world values.
 *
 * @param [in,out] previous The record of the previous segment, or NULL.
 * @param [in] angleOffset The offset of t in the section.
 * @param [in] pointOffset The offset of the point in previous.
 * @param [in] x The side correction of this segment, in 1/256 units.
 * @param [in] y The height correction of this segment, in 1/256 units.
 */
static void CorrectWidthPoint(
    BYTE* previous,
    DWORD angleOffset,
    DWORD pointOffset,
    LONG x,
    LONG y)
{
    const BYTE* section;
    LONG previousX;
    LONG previousY;
    LONG t;
    LONG pointX;
    LONG pointY;
    LONG scale;

    if (previous == NULL)
    {
        return;
    }

    section = *reinterpret_cast<BYTE**>(previous + 0x0C);
    if (section == NULL || section[0x3C] != 0 || (section[0x37] & 0x18) == 0)
    {
        return;
    }

    t = *reinterpret_cast<const LONG*>(section + angleOffset);
    if ((section[0x37] & 8) != 0)
    {
        t = 0x100 - t;
    }

    LastCorrection(previous, &previousX, &previousY);
    pointX = previousX + (((x - previousX) * t) >> 8);
    pointY = previousY + (((y - previousY) * t) >> 8);
    if (pointX == 0 && pointY == 0)
    {
        return;
    }

    scale = Reciprocal(*reinterpret_cast<const LONG*>(previous + 0x18) + t);
    *reinterpret_cast<LONG*>(previous + pointOffset) += Scaled(pointX, scale);
    *reinterpret_cast<LONG*>(previous + pointOffset + 4) += Scaled(pointY, scale);
}

/**
 * Projects one segment with the game's function and corrects its screen
 * values; replaces the calls of FUN_00444630.
 *
 * @param [in] section The course section of the segment.
 * @param [in,out] record The segment record that receives the screen
 *                 values.
 * @param [in,out] p The block p[0..9] of the road.
 */
static void __fastcall HookedSection(BYTE* section, BYTE* record, LONG* p)
{
    BYTE* previousLeft = reinterpret_cast<BYTE*>(p[8]);
    BYTE* previousRight = reinterpret_cast<BYTE*>(p[9]);
    LONG depth = p[4];
    LONG height = p[1];
    RoadBlock* block = g_active ? FindBlock(p, TRUE) : NULL;
    LONG x = 0;
    LONG y = 0;
    DWORD index;

    if (block != NULL)
    {
        if (block->seen && block->nextDepth == depth && block->nextHeight == height)
        {
            // The same road, one segment further.
        }
        else if (depth == g_firstDepth)
        {
            LONG product = static_cast<LONG>(
                static_cast<DWORD>(g_frac) * static_cast<DWORD>(p[0]));

            block->valid = height == -(product >> 8);
            block->rem = product & 0xFF;

            // The road whose curve the game used for the first side step.
            if (block == &g_blocks[g_epsBlock] &&
                (p[3] != 0 || p[2] != g_firstSideStep))
            {
                g_epsValid = FALSE;
            }
        }
        else
        {
            const RoadBlock* other = &g_blocks[block == &g_blocks[0] ? 1 : 0];

            // Copied from the other road, before or after the other road
            // handled this segment (the game calls the first road first).
            block->valid = g_blockCount == 2 && other->seen && other->valid &&
                ((other->nextDepth == depth && other->nextHeight == height) ||
                 (other->entryDepth == depth && other->entryHeight == height));
            block->rem = other->rem;
        }
    }

    g_sectionNext(section, record, p);

    if (block == NULL)
    {
        return;
    }

    block->seen = TRUE;
    block->entryDepth = depth;
    block->entryHeight = height;
    block->nextDepth = p[4];
    block->nextHeight = p[1];

    if (block->valid && section[0x3C] == 0 && depth >= g_firstDepth)
    {
        LONG scale = *reinterpret_cast<const LONG*>(record + kScaleOffset);
        LONG dx;
        LONG dy;

        y = block->rem;
        x = g_epsValid ? ((depth - g_firstDepth) >> 8) * g_eps : 0;
        dx = Scaled(x, scale);
        dy = Scaled(y, scale);

        for (index = 0; index < sizeof(kYOffsets) / sizeof(kYOffsets[0]); ++index)
        {
            *reinterpret_cast<LONG*>(record + kYOffsets[index]) += dy;
        }

        for (index = 0; index < sizeof(kXOffsets) / sizeof(kXOffsets[0]); ++index)
        {
            *reinterpret_cast<LONG*>(record + kXOffsets[index]) += dx;
        }

        p[7] += dy;

        // The previous segment's far edge, copied from this one.
        if (previousLeft != NULL)
        {
            *reinterpret_cast<LONG*>(previousLeft + 0x28) += dy;
            *reinterpret_cast<LONG*>(previousLeft + 0x30) += dx;
        }

        if (previousRight != NULL)
        {
            if (previousRight != previousLeft)
            {
                *reinterpret_cast<LONG*>(previousRight + 0x28) += dy;
            }

            *reinterpret_cast<LONG*>(previousRight + 0x34) += dx;
        }
    }

    CorrectWidthPoint(previousLeft, 0x138, 0xA0, x, y);
    CorrectWidthPoint(previousRight, 0x13C, 0xA8, x, y);

    block->lastRecord = record;
    block->lastX = x;
    block->lastY = y;
}

/**
 * Finds frac and eps for the frame and projects the road with the game's
 * function; replaces the call of FUN_00443D70.
 *
 * @param [in] position The camera position on the course.
 * @param [in,out] slices Passed on to the game.
 * @param [in] count Passed on to the game.
 * @param [in,out] camera Passed on to the game.
 * @param [in] mode The road the camera is on where the road splits (0 or 1).
 */
static void __fastcall HookedSetup(
    DWORD position,
    LONG* slices,
    int count,
    LONG* camera,
    int mode)
{
    const BYTE* node = *reinterpret_cast<const BYTE* const*>(
        g_base + kNodeRingRva + ((position & 0x7F00) >> 6));

    g_frac = static_cast<LONG>(position & 0xFF);
    g_firstDepth = 0x100 - g_frac;
    g_blockCount = 0;
    g_epsValid = FALSE;

    if (node != NULL)
    {
        CHAR roads = static_cast<CHAR>(node[0x1C]);
        const BYTE* section = NULL;

        if (roads == 1)
        {
            section = *reinterpret_cast<const BYTE* const*>(node + 0x0C);
            g_epsBlock = 0;
        }
        else if (mode == 0 || mode == 1)
        {
            section = *reinterpret_cast<const BYTE* const*>(node + 0x0C + mode * 4);
            g_epsBlock = mode;
        }

        if (section != NULL)
        {
            LONG product = *reinterpret_cast<const LONG*>(section + 0xB8) * g_firstDepth;

            // The game's division by 256, towards zero.
            g_firstSideStep = (product + ((product >> 31) & 0xFF)) >> 8;
            g_eps = product - g_firstSideStep * 256;
            g_epsValid = TRUE;
        }
    }

    g_active = TRUE;
    g_setupNext(position, slices, count, camera, mode);
    g_active = FALSE;
}

BOOL SmoothRoadInstall(DWORD gameBase)
{
    static const BYTE reciprocalLea[3] = { 0x8D, 0x3C, 0x45 };
    const BYTE* lea;
    DWORD sectionTarget;
    DWORD index;

    g_base = gameBase;

    // Check everything first, so nothing is changed on a mismatch. The
    // section calls may already lead to DrawDistance's hook; that is
    // called in turn.
    if (CallTarget(kSetupCallRva) != gameBase + kSetupRva)
    {
        LogSmooth("Smooth road stays off: unexpected code where the road is projected.");
        return FALSE;
    }

    sectionTarget = CallTarget(kSectionCalls[0]);
    for (index = 0; index < kSectionCallCount; ++index)
    {
        if (sectionTarget == 0 || CallTarget(kSectionCalls[index]) != sectionTarget)
        {
            LogSmooth("Smooth road stays off: unexpected code in the road projection.");
            return FALSE;
        }
    }

    if (sectionTarget != gameBase + kSectionRva &&
        (sectionTarget >= gameBase && sectionTarget < gameBase + 0x00100000))
    {
        LogSmooth("Smooth road stays off: the road projection leads somewhere unexpected.");
        return FALSE;
    }

    lea = reinterpret_cast<const BYTE*>(gameBase + kReciprocalLeaRva);
    if (lea[0] != reciprocalLea[0] || lea[1] != reciprocalLea[1] ||
        lea[2] != reciprocalLea[2] || ReadDword(lea + 3) == 0)
    {
        LogSmooth("Smooth road stays off: unexpected code in the road edge projection.");
        return FALSE;
    }

    g_setupNext = reinterpret_cast<SetupFn>(gameBase + kSetupRva);
    g_sectionNext = reinterpret_cast<SectionFn>(sectionTarget);

    for (index = 0; index < kSectionCallCount; ++index)
    {
        if (!RedirectCall(kSectionCalls[index], HookedSection))
        {
            LogSmooth("Smooth road: a code change failed; the road may be unstable.");
            return FALSE;
        }
    }

    if (!RedirectCall(kSetupCallRva, HookedSetup))
    {
        LogSmooth("Smooth road: a code change failed; the road may be unstable.");
        return FALSE;
    }

    LogSmooth("Smooth road: the road and its edges move without jumps.");
    return TRUE;
}
