// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "game_fixes.h"
#include "road_rash_patch.h"

// Every fix checks the original bytes before it changes anything and logs
// why it stays off when they differ. Addresses are RVAs in ROADRASH.EXE,
// German 1.0A. Absolute addresses inside instructions are compared after
// adding the actual module base, in case the executable was relocated.

// Bike colours: in a race every bike is drawn from one template and
// recoloured through a 256-byte palette remap, one per bike model
// (15 tables, read only at 0x44384C as [model * 256 + 0x46E158]). The
// tables of models 0-4 and 5-9 are swapped in the original data, so the
// Rat and Super bikes race in each other's colours while the shop shows
// them correctly. Swapping the two groups back fixes it.
static const DWORD kBikeRemapTablesRva = 0x0006E158;
static const DWORD kBikeRemapTableSize = 256;
static const DWORD kBikeRemapTableCount = 15;
static const DWORD kBikeRemapGroupSize = 5;
static const DWORD kBikeRemapTablesChecksum = 0x6C2053F7;

// Sprite snapping: FUN_00410160 sets how FUN_0040FFC0 converts the 16.16
// corners of every world quad into pixels: (corner >> shift) & mask. In
// HiRes it writes shift 15 and mask 0xFFFFFFFE, which keeps every corner on
// an even pixel, so objects move in steps of 2 pixels. The mask 0xFFFFFFFF
// (as in low resolution) keeps the full 640x480 precision.
static const DWORD kHiResMaskStoreRva = 0x000101C6;
static const DWORD kCornerMaskRva = 0x00081FF4;
static const DWORD kSnappingCornerMask = 0xFFFFFFFE;
static const DWORD kFullCornerMask = 0xFFFFFFFF;

// Music volume: when a song starts, FUN_00433BB0 sets the MIDI volume to
// master x music from the Sound Mix screen with midiOutSetVolume. Since
// Windows Vista that call changes the volume of the whole game in the
// Windows volume mixer, and Windows remembers it: a music volume of 0 left
// the game without any sound, even after a restart. A song that would
// play silently now takes the game's own path for muted music instead, so
// midiOutSetVolume(0) is never called.
static const DWORD kMusicStartCallRva = 0x00035E3F;
static const DWORD kMusicStartFunctionRva = 0x00033BB0;
static const DWORD kMasterVolumeRva = 0x00063048;
static const DWORD kMusicVolumeRva = 0x000630A8;
static const DWORD kMusicPlayingFlagRva = 0x000C4174;

typedef DWORD (__fastcall *MusicStartFn)(DWORD, DWORD);

// Object update rate: FUN_00438170 walks the world nodes (riders, cars,
// pedestrians, cops) every game tick and updates each one through
// FUN_00437E60 once its next-update tick [node + 0x0C] has come. That
// function runs the node's think and move handlers with the time since its
// last update ([node + 0x18]), then picks a new interval [node + 0x14] from
// the distance to the camera: 1 tick close by, up to 8 for riders 30
// segments ahead (0x1E for nodes off the road). Far riders and cars
// therefore move in visible jumps. At 0x438108 it computes
// next = [node + 0x14] + tick. Within the range where nodes are drawn
// (FUN_00438450: up to camera + 0x2000), next becomes tick + 1, so they move
// every tick; their movement still follows the elapsed time as before.
// Nodes further away, behind the camera or off the road keep their
// interval, and [node + 0x14] stays as the game computed it.
static const DWORD kNextUpdateRva = 0x00038108;
static const DWORD kCameraPositionRva = 0x000B6E5C;
static const LONG kFullRateBehind = -0x400;
static const LONG kFullRateAhead = 0x2100;

static DWORD g_cameraPositionVa = 0;

static DWORD g_fixesBase = 0;

static void LogFix(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

static BOOL MatchesGameBytes(DWORD rva, const BYTE* expected, DWORD length)
{
    const BYTE* actual = reinterpret_cast<const BYTE*>(g_fixesBase + rva);
    DWORD index;

    for (index = 0; index < length; ++index)
    {
        if (actual[index] != expected[index])
        {
            return FALSE;
        }
    }

    return TRUE;
}

static BOOL WriteGameBytes(DWORD rva, const void* bytes, DWORD length)
{
    BYTE* destination = reinterpret_cast<BYTE*>(g_fixesBase + rva);
    const BYTE* source = reinterpret_cast<const BYTE*>(bytes);
    DWORD oldProtection;
    DWORD ignoredProtection;
    DWORD index;

    if (!VirtualProtect(
            destination,
            length,
            PAGE_EXECUTE_READWRITE,
            &oldProtection))
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

static void StoreDword(BYTE* bytes, DWORD value)
{
    bytes[0] = static_cast<BYTE>(value);
    bytes[1] = static_cast<BYTE>(value >> 8);
    bytes[2] = static_cast<BYTE>(value >> 16);
    bytes[3] = static_cast<BYTE>(value >> 24);
}

// FNV-1a, 32 bits (64-bit arithmetic would need the C runtime).
static DWORD Checksum(const BYTE* bytes, DWORD length)
{
    DWORD hash = 0x811C9DC5;
    DWORD index;

    for (index = 0; index < length; ++index)
    {
        hash = (hash ^ bytes[index]) * 0x01000193;
    }

    return hash;
}

static void FixBikeColors()
{
    BYTE* tables = reinterpret_cast<BYTE*>(
        g_fixesBase + kBikeRemapTablesRva);
    DWORD groupBytes = kBikeRemapGroupSize * kBikeRemapTableSize;
    DWORD oldProtection;
    DWORD ignoredProtection;
    DWORD index;

    if (Checksum(tables, kBikeRemapTableCount * kBikeRemapTableSize) !=
        kBikeRemapTablesChecksum)
    {
        LogFix("Bike colour fix stays off: the colour tables differ from the original game.");
        return;
    }

    if (!VirtualProtect(tables, 2 * groupBytes, PAGE_READWRITE, &oldProtection))
    {
        LogFix("Bike colour fix stays off: the colour tables could not be changed.");
        return;
    }

    for (index = 0; index < groupBytes; ++index)
    {
        BYTE value = tables[index];

        tables[index] = tables[groupBytes + index];
        tables[groupBytes + index] = value;
    }

    VirtualProtect(tables, 2 * groupBytes, oldProtection, &ignoredProtection);
    LogFix("Bike colour fix: the Rat and Super bikes race in their own colours.");
}

static void SmoothSprites()
{
    BYTE expected[10] = { 0xC7, 0x05 };
    BYTE mask[4];
    DWORD* currentMask = reinterpret_cast<DWORD*>(
        g_fixesBase + kCornerMaskRva);

    // mov dword ptr [0x481FF4], 0xFFFFFFFE
    StoreDword(expected + 2, g_fixesBase + kCornerMaskRva);
    StoreDword(expected + 6, kSnappingCornerMask);
    if (!MatchesGameBytes(kHiResMaskStoreRva, expected, sizeof(expected)))
    {
        LogFix("Smooth sprites stay off: unexpected code in the HiRes setup.");
        return;
    }

    StoreDword(mask, kFullCornerMask);
    if (!WriteGameBytes(kHiResMaskStoreRva + 6, mask, sizeof(mask)))
    {
        LogFix("Smooth sprites stay off: the HiRes setup could not be changed.");
        return;
    }

    // In case HiRes was already set up before the patch was installed.
    if (*currentMask == kSnappingCornerMask)
    {
        *currentMask = kFullCornerMask;
    }

    LogFix("Smooth sprites: objects move in 1-pixel steps in HiRes.");
}

static DWORD __fastcall HookedMusicStart(DWORD song, DWORD track)
{
    DWORD master = *reinterpret_cast<DWORD*>(g_fixesBase + kMasterVolumeRva);
    DWORD music = *reinterpret_cast<DWORD*>(g_fixesBase + kMusicVolumeRva);

    // FUN_00433CB0: ((master * music / 100) * 255) / 100 per channel,
    // which is 0 exactly when master * music < 100.
    if (master * music < 100)
    {
        static BOOL logged = FALSE;

        if (!logged)
        {
            logged = TRUE;
            LogFix("MIDI volume guard: the music volume is 0, so the music stays off instead of muting the game in the Windows mixer.");
        }

        // The game's own path for muted music.
        *reinterpret_cast<BYTE*>(g_fixesBase + kMusicPlayingFlagRva) = 0;
        return 0;
    }

    return reinterpret_cast<MusicStartFn>(
        g_fixesBase + kMusicStartFunctionRva)(song, track);
}

static void GuardMidiVolume()
{
    BYTE call[5] = { 0xE8 };
    DWORD callEnd = g_fixesBase + kMusicStartCallRva + sizeof(call);

    StoreDword(call + 1, g_fixesBase + kMusicStartFunctionRva - callEnd);
    if (!MatchesGameBytes(kMusicStartCallRva, call, sizeof(call)))
    {
        LogFix("MIDI volume guard stays off: unexpected code at the music start.");
        return;
    }

    StoreDword(
        call + 1,
        reinterpret_cast<DWORD>(HookedMusicStart) - callEnd);
    if (!WriteGameBytes(kMusicStartCallRva, call, sizeof(call)))
    {
        LogFix("MIDI volume guard stays off: the music start could not be changed.");
        return;
    }

    LogFix("MIDI volume guard: a music volume of 0 no longer mutes the game.");
}

// Replaces mov eax, [esi + 0x14] / mov cl, [esp + 0x10] / add eax, edi
// with call + mov cl, [esp + 0x10]: esi is the node, edi the tick, and eax
// returns the next-update tick. Flags are set again by the test that
// follows; ecx and edx are kept.
static __declspec(naked) void HookedNextUpdate()
{
    __asm
    {
        push ecx
        push edx
        mov eax, [esi + 0x14]
        cmp eax, 1
        jle done
        cmp dword ptr [esi + 0x2C], 0
        je done
        mov edx, g_cameraPositionVa
        mov ecx, [esi + 0x1C]
        sub ecx, [edx]
        cmp ecx, kFullRateBehind
        jl done
        cmp ecx, kFullRateAhead
        jg done
        mov eax, 1
    done:
        add eax, edi
        pop edx
        pop ecx
        ret
    }
}

static void FullRateObjects()
{
    static const BYTE expected[9] =
    {
        0x8B, 0x46, 0x14,       // mov eax, [esi + 0x14]
        0x8A, 0x4C, 0x24, 0x10, // mov cl, [esp + 0x10]
        0x03, 0xC7              // add eax, edi
    };
    BYTE patch[9] = { 0xE8, 0, 0, 0, 0, 0x8A, 0x4C, 0x24, 0x10 };
    DWORD callEnd = g_fixesBase + kNextUpdateRva + 5;

    if (!MatchesGameBytes(kNextUpdateRva, expected, sizeof(expected)))
    {
        LogFix("Full-rate objects stay off: unexpected code in the object update.");
        return;
    }

    g_cameraPositionVa = g_fixesBase + kCameraPositionRva;
    StoreDword(
        patch + 1,
        reinterpret_cast<DWORD>(HookedNextUpdate) - callEnd);
    if (!WriteGameBytes(kNextUpdateRva, patch, sizeof(patch)))
    {
        LogFix("Full-rate objects stay off: the object update could not be changed.");
        return;
    }

    LogFix("Full-rate objects: riders and cars in view move every game tick.");
}

void ApplyGameFixes(DWORD gameBase, const CHAR* iniPath)
{
    static BOOL applied = FALSE;

    if (applied || gameBase == 0)
    {
        return;
    }

    applied = TRUE;
    g_fixesBase = gameBase;

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "FixBikeColors", 1, iniPath) != 0)
    {
        FixBikeColors();
    }

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "SmoothSprites", 1, iniPath) != 0)
    {
        SmoothSprites();
    }

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "MidiVolumeGuard", 1, iniPath) != 0)
    {
        GuardMidiVolume();
    }

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "FullRateObjects", 1, iniPath) != 0)
    {
        FullRateObjects();
    }
}
