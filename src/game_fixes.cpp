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

// Punch shadow: FUN_00443610 (called once per HiRes setup, from 0x4101AA)
// loads the rider's shadow frames (DATA\BIKERS\HISHADOW.DAT, LOSHADOW.DAT
// in low resolution) into a table of 326 entries at 0x49BFE0: pixel
// pointer, width byte, height byte. Every frame is drawn centred on its
// box. Frame 176, the first frame of the punch, has two stray pixels in the
// top corner of a 118x168 box (59x84 in low resolution), while the shadow
// itself is the band of 13 (6) rows at the bottom, so the shadow jumps far
// away from the bike. After loading, the entry points at a copy of the
// shadow alone.
static const DWORD kShadowLoaderCallRva = 0x000101AA;
static const DWORD kShadowLoaderRva = 0x00043610;
static const DWORD kShadowTableRva = 0x0009BFE0;
static const DWORD kPunchShadowFrame = 176;
static const DWORD kMaxStrayPixels = 8;
static const DWORD kMaxShadowBytes = 2048;

typedef int (__cdecl *ShadowLoaderFn)();

static BYTE g_punchShadow[kMaxShadowBytes];

// Floating buildings: the pictures of the red barn (BARN12), the brown shop
// with the sign (RUSTICA2) and the white house (HOUSEB2) in FAMILIES.RSC end
// in empty rows (2, 5 and 10), and the game places them as if their
// picture reached the bottom of the texture, so they hover above the
// ground. Every quad is turned into pixels by FUN_0040FFC0 (called at
// 0x40FF5A and 0x40FF87); a quad whose texture (quad + 8, a texture header
// with the name at + 0x10) is one of these is drawn that many rows lower,
// scaled to its height on screen. Only the drawing changes.
static const DWORD kQuadToPixelsRva = 0x0000FFC0;
static const DWORD kQuadToPixelsCalls[] = { 0x0000FF5A, 0x0000FF87 };
static const DWORD kTextureMagic = 0x00BC614E;

struct FloatingBuilding
{
    CHAR name[8];
    LONG rows;
};

// The shop is lowered by 3 of its 5 empty rows, which looks best (it has a
// porch at its bottom edge).
static const FloatingBuilding kFloatingBuildings[] =
{
    { { 'B', 'A', 'R', 'N', '1', '2', 0, 0 }, 2 },
    { { 'R', 'U', 'S', 'T', 'I', 'C', 'A', '2' }, 3 },
    { { 'H', 'O', 'U', 'S', 'E', 'B', '2', 0 }, 10 }
};

static const DWORD kFloatingBuildingCount =
    sizeof(kFloatingBuildings) / sizeof(kFloatingBuildings[0]);

typedef void (__fastcall *QuadToPixelsFn)(BYTE* node, BYTE* quad);

static BOOL g_buildingLogged[kFloatingBuildingCount];

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

// Race music diagnostics. FUN_00433BB0 plays Audio\Music\GRUNGE<n>.MID
// (SBKGRNG<n> with an AWE card, flag 0x46D900), RIFF "MIDS" files of
// ready-made stream buffers, on the MIDI mapper: midiStreamOpen(device -1),
// midiStreamProperty, midiOutPrepareHeader and midiStreamOut per buffer,
// midiStreamRestart (FUN_00433E30), then midiOutSetVolume on the mapper.
// Any failure leaves the race silent, with an error box that is hidden in
// fullscreen. The results of these calls are logged (failures always,
// within a limit), and so is each music start.
static const DWORD kAweMusicFlagRva = 0x0006D900;
static const DWORD kMaxMidiLogLines = 24;

struct MidiImport
{
    DWORD iatVa;
    const CHAR* name;
};

static const MidiImport kMidiImports[] =
{
    { 0x004CA978, "midiStreamOpen" },
    { 0x004CA974, "midiStreamProperty" },
    { 0x004CA970, "midiOutPrepareHeader" },
    { 0x004CA96C, "midiStreamOut" },
    { 0x004CA960, "midiStreamRestart" },
    { 0x004CA968, "midiOutSetVolume" }
};

typedef MMRESULT (WINAPI *MidiStreamOpenFn)(
    LPHMIDISTRM, LPUINT, DWORD, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *MidiStreamPropertyFn)(HMIDISTRM, LPBYTE, DWORD);
typedef MMRESULT (WINAPI *MidiHeaderFn)(HMIDIOUT, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI *MidiStreamRestartFn)(HMIDISTRM);
typedef MMRESULT (WINAPI *MidiOutSetVolumeFn)(HMIDIOUT, DWORD);

static LPVOID g_midiOriginal[sizeof(kMidiImports) / sizeof(kMidiImports[0])];
static DWORD g_midiLogLines = 0;
static BOOL g_midiVolumeTolerant = FALSE;

// [Fixes] MusicDevice: -1 = the MIDI mapper (as the game does), otherwise
// the number of a MIDI output from the list in the log.
static LONG g_musicDevice = -1;

typedef UINT (WINAPI *MidiOutGetNumDevsFn)(void);
typedef MMRESULT (WINAPI *MidiOutGetDevCapsFn)(UINT_PTR, LPMIDIOUTCAPSA, UINT);

static const CHAR* MidiTechnologyName(WORD technology)
{
    switch (technology)
    {
    case MOD_MIDIPORT:
        return "external MIDI port";
    case MOD_SYNTH:
        return "synthesizer";
    case MOD_SQSYNTH:
        return "square wave synthesizer";
    case MOD_FMSYNTH:
        return "FM synthesizer";
    case MOD_MAPPER:
        return "MIDI mapper";
    case 6:
        return "wavetable synthesizer";
    case 7:
        return "software synthesizer";
    default:
        return "other";
    }
}

// The MIDI outputs Windows offers, once (the mapper plays on one of them,
// as set in the Control Panel).
static void LogMidiDevices()
{
    static BOOL logged = FALSE;
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    MidiOutGetNumDevsFn getCount;
    MidiOutGetDevCapsFn getCaps;
    UINT count;
    UINT index;

    if (logged || winmm == NULL)
    {
        return;
    }

    logged = TRUE;
    getCount = reinterpret_cast<MidiOutGetNumDevsFn>(
        GetProcAddress(winmm, "midiOutGetNumDevs"));
    getCaps = reinterpret_cast<MidiOutGetDevCapsFn>(
        GetProcAddress(winmm, "midiOutGetDevCapsA"));
    if (getCount == NULL || getCaps == NULL)
    {
        return;
    }

    count = getCount();
    {
        CHAR message[96];

        wsprintfA(
            message,
            "Race music: %lu MIDI outputs; MusicDevice=%ld plays on %s.",
            count,
            g_musicDevice,
            g_musicDevice < 0 ? "the MIDI mapper" : "that output");
        LogFix(message);
    }

    for (index = 0; index < count && index < 16; ++index)
    {
        MIDIOUTCAPSA caps;
        CHAR message[128];

        if (getCaps(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        {
            continue;
        }

        caps.szPname[MAXPNAMELEN - 1] = 0;
        wsprintfA(
            message,
            "  MIDI output %lu: %s (%s)",
            index,
            caps.szPname,
            MidiTechnologyName(caps.wTechnology));
        LogFix(message);
    }
}

static void LogMidi(const CHAR* call, MMRESULT result)
{
    CHAR message[96];

    if (g_midiLogLines >= kMaxMidiLogLines)
    {
        return;
    }

    ++g_midiLogLines;
    wsprintfA(message, "Race music: %s returned %lu.", call, result);
    LogFix(message);
}

static MMRESULT WINAPI HookedMidiStreamOpen(
    LPHMIDISTRM stream,
    LPUINT device,
    DWORD count,
    DWORD_PTR callback,
    DWORD_PTR instance,
    DWORD flags)
{
    UINT chosen;
    MMRESULT result;

    LogMidiDevices();
    if (g_musicDevice >= 0 && device != NULL)
    {
        chosen = static_cast<UINT>(g_musicDevice);
        result = reinterpret_cast<MidiStreamOpenFn>(g_midiOriginal[0])(
            stream, &chosen, count, callback, instance, flags);
        LogMidi("midiStreamOpen (the MusicDevice output)", result);
        return result;
    }

    result = reinterpret_cast<MidiStreamOpenFn>(g_midiOriginal[0])(
        stream, device, count, callback, instance, flags);
    LogMidi("midiStreamOpen (MIDI mapper)", result);
    return result;
}

static MMRESULT WINAPI HookedMidiStreamProperty(
    HMIDISTRM stream,
    LPBYTE data,
    DWORD property)
{
    MMRESULT result = reinterpret_cast<MidiStreamPropertyFn>(g_midiOriginal[1])(
        stream, data, property);

    if (result != MMSYSERR_NOERROR)
    {
        LogMidi("midiStreamProperty", result);
    }

    return result;
}

static MMRESULT WINAPI HookedMidiOutPrepareHeader(
    HMIDIOUT out,
    LPMIDIHDR header,
    UINT size)
{
    MMRESULT result = reinterpret_cast<MidiHeaderFn>(g_midiOriginal[2])(
        out, header, size);

    if (result != MMSYSERR_NOERROR)
    {
        LogMidi("midiOutPrepareHeader", result);
    }

    return result;
}

static MMRESULT WINAPI HookedMidiStreamOut(
    HMIDISTRM stream,
    LPMIDIHDR header,
    UINT size)
{
    MMRESULT result = reinterpret_cast<MidiHeaderFn>(g_midiOriginal[3])(
        reinterpret_cast<HMIDIOUT>(stream), header, size);

    if (result != MMSYSERR_NOERROR)
    {
        LogMidi("midiStreamOut", result);
    }

    return result;
}

static MMRESULT WINAPI HookedMidiStreamRestart(HMIDISTRM stream)
{
    MMRESULT result = reinterpret_cast<MidiStreamRestartFn>(g_midiOriginal[4])(
        stream);

    if (result != MMSYSERR_NOERROR)
    {
        LogMidi("midiStreamRestart", result);
    }

    return result;
}

// With MidiVolumeGuard on, a MIDI driver that cannot set the volume
// through the mapper no longer stops the music: the song keeps playing at
// the driver's own volume.
static MMRESULT WINAPI HookedMidiOutSetVolume(HMIDIOUT out, DWORD volume)
{
    MMRESULT result;

    // The game sets the volume of the mapper (device -1); with MusicDevice
    // it belongs to the chosen output.
    if (g_musicDevice >= 0 && out == reinterpret_cast<HMIDIOUT>(-1))
    {
        out = reinterpret_cast<HMIDIOUT>(static_cast<UINT_PTR>(g_musicDevice));
    }

    result = reinterpret_cast<MidiOutSetVolumeFn>(g_midiOriginal[5])(
        out, volume);

    if (result != MMSYSERR_NOERROR)
    {
        LogMidi("midiOutSetVolume (MIDI mapper)", result);
        if (g_midiVolumeTolerant)
        {
            LogMidi("the volume is left as it is and the music plays on", 0);
            return MMSYSERR_NOERROR;
        }
    }

    return result;
}

static void InstallMidiLogging()
{
    static const LPVOID hooks[] =
    {
        reinterpret_cast<LPVOID>(HookedMidiStreamOpen),
        reinterpret_cast<LPVOID>(HookedMidiStreamProperty),
        reinterpret_cast<LPVOID>(HookedMidiOutPrepareHeader),
        reinterpret_cast<LPVOID>(HookedMidiStreamOut),
        reinterpret_cast<LPVOID>(HookedMidiStreamRestart),
        reinterpret_cast<LPVOID>(HookedMidiOutSetVolume)
    };
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    DWORD index;

    // Check every import first, so nothing changes on a mismatch.
    for (index = 0; index < sizeof(kMidiImports) / sizeof(kMidiImports[0]); ++index)
    {
        LPVOID* slot = reinterpret_cast<LPVOID*>(
            g_fixesBase - 0x00400000 + kMidiImports[index].iatVa);
        FARPROC expected = winmm != NULL ?
            GetProcAddress(winmm, kMidiImports[index].name) : NULL;

        if (expected == NULL || *slot != reinterpret_cast<LPVOID>(expected))
        {
            LogFix("Race music diagnostics stay off: the game's MIDI imports are not as expected.");
            return;
        }
    }

    for (index = 0; index < sizeof(kMidiImports) / sizeof(kMidiImports[0]); ++index)
    {
        LPVOID* slot = reinterpret_cast<LPVOID*>(
            g_fixesBase - 0x00400000 + kMidiImports[index].iatVa);
        DWORD oldProtection;
        DWORD ignoredProtection;

        if (VirtualProtect(slot, sizeof(LPVOID), PAGE_READWRITE, &oldProtection))
        {
            g_midiOriginal[index] = *slot;
            *slot = hooks[index];
            VirtualProtect(slot, sizeof(LPVOID), oldProtection, &ignoredProtection);
        }
    }
}

static DWORD __fastcall HookedMusicStart(DWORD song, DWORD track)
{
    DWORD master = *reinterpret_cast<DWORD*>(g_fixesBase + kMasterVolumeRva);
    DWORD music = *reinterpret_cast<DWORD*>(g_fixesBase + kMusicVolumeRva);
    DWORD result;

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

    result = reinterpret_cast<MusicStartFn>(
        g_fixesBase + kMusicStartFunctionRva)(song, track);

    if (g_midiLogLines < kMaxMidiLogLines)
    {
        CHAR message[128];

        ++g_midiLogLines;
        wsprintfA(
            message,
            "Race music: song %lu (%s) %s.",
            track,
            *reinterpret_cast<BYTE*>(g_fixesBase + kAweMusicFlagRva) != 0 ?
                "AWE SoundFont version" : "General MIDI version",
            result == 0 ? "started" : "failed to start");
        LogFix(message);
    }

    return result;
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

static BOOL RowHasPixels(const BYTE* row, LONG width)
{
    LONG x;

    for (x = 0; x < width; ++x)
    {
        if (row[x] != 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static void CutPunchShadow()
{
    BYTE* entry = reinterpret_cast<BYTE*>(
        g_fixesBase + kShadowTableRva) + kPunchShadowFrame * 8;
    const BYTE* pixels = *reinterpret_cast<BYTE**>(entry);
    LONG width = entry[4];
    LONG height = entry[5];
    LONG top = -1;
    LONG bottom = -1;
    LONG left = width;
    LONG right = -1;
    LONG newWidth;
    LONG newHeight;
    DWORD stray = 0;
    LONG x;
    LONG y;
    CHAR message[128];

    if (pixels == NULL || pixels == g_punchShadow ||
        !((width == 118 && height == 168) || (width == 59 && height == 84)))
    {
        return;
    }

    // The shadow: the lowest band of rows in use, without a gap.
    for (y = height - 1; y >= 0; --y)
    {
        if (RowHasPixels(pixels + y * width, width))
        {
            if (bottom < 0)
            {
                bottom = y;
            }

            top = y;
        }
        else if (bottom >= 0)
        {
            break;
        }
    }

    if (bottom < 0)
    {
        return;
    }

    for (y = 0; y < height; ++y)
    {
        for (x = 0; x < width; ++x)
        {
            if (pixels[y * width + x] == 0)
            {
                continue;
            }

            if (y < top)
            {
                ++stray;
            }
            else
            {
                if (x < left)
                {
                    left = x;
                }

                if (x > right)
                {
                    right = x;
                }
            }
        }
    }

    newWidth = right - left + 1;
    newHeight = bottom - top + 1;
    if (stray == 0 || stray > kMaxStrayPixels || newHeight < 4 ||
        static_cast<DWORD>(newWidth * newHeight) > kMaxShadowBytes)
    {
        LogFix("Punch shadow stays as it is: frame 176 is not as expected.");
        return;
    }

    for (y = 0; y < newHeight; ++y)
    {
        for (x = 0; x < newWidth; ++x)
        {
            g_punchShadow[y * newWidth + x] =
                pixels[(top + y) * width + left + x];
        }
    }

    *reinterpret_cast<BYTE**>(entry) = g_punchShadow;
    entry[4] = static_cast<BYTE>(newWidth);
    entry[5] = static_cast<BYTE>(newHeight);

    wsprintfA(
        message,
        "Punch shadow: frame 176 cut from %ldx%ld to %ldx%ld (%lu stray pixels).",
        width,
        height,
        newWidth,
        newHeight,
        stray);
    LogFix(message);
}

static int __cdecl HookedShadowLoader()
{
    int result = reinterpret_cast<ShadowLoaderFn>(
        g_fixesBase + kShadowLoaderRva)();

    if (result != 0)
    {
        CutPunchShadow();
    }

    return result;
}

static void FixPunchShadow()
{
    BYTE call[5] = { 0xE8 };
    DWORD callEnd = g_fixesBase + kShadowLoaderCallRva + sizeof(call);

    StoreDword(call + 1, g_fixesBase + kShadowLoaderRva - callEnd);
    if (!MatchesGameBytes(kShadowLoaderCallRva, call, sizeof(call)))
    {
        LogFix("Punch shadow fix stays off: unexpected code in the HiRes setup.");
        return;
    }

    StoreDword(
        call + 1,
        reinterpret_cast<DWORD>(HookedShadowLoader) - callEnd);
    if (!WriteGameBytes(kShadowLoaderCallRva, call, sizeof(call)))
    {
        LogFix("Punch shadow fix stays off: the HiRes setup could not be changed.");
        return;
    }

    // In case the shadows were already loaded.
    CutPunchShadow();
    LogFix("Punch shadow fix: the first punch frame keeps its shadow under the bike.");
}

static const FloatingBuilding* FindFloatingBuilding(const BYTE* texture)
{
    const DWORD* name;
    DWORD index;

    if (texture == NULL ||
        *reinterpret_cast<const DWORD*>(texture) != kTextureMagic)
    {
        return NULL;
    }

    name = reinterpret_cast<const DWORD*>(texture + 0x10);
    for (index = 0; index < kFloatingBuildingCount; ++index)
    {
        const DWORD* wanted = reinterpret_cast<const DWORD*>(
            kFloatingBuildings[index].name);

        if (name[0] != wanted[0] || name[1] != wanted[1])
        {
            continue;
        }

        if (!g_buildingLogged[index])
        {
            CHAR shown[9];
            CHAR message[96];
            DWORD c;

            g_buildingLogged[index] = TRUE;
            for (c = 0; c < 8; ++c)
            {
                shown[c] = kFloatingBuildings[index].name[c];
            }

            shown[8] = 0;
            wsprintfA(
                message,
                "Floating buildings: %s is drawn %ld rows lower.",
                shown,
                kFloatingBuildings[index].rows);
            LogFix(message);
        }

        return &kFloatingBuildings[index];
    }

    return NULL;
}

// quad + 0x44..0x60: four corners x, y (16.16, y grows downward); quad + 8:
// its texture. The texture's rows y0 + 1 to y1 - 1 (shorts at + 6 and + 10)
// span the quad from its top to its bottom (FUN_0044D300).
static void __fastcall HookedQuadToPixels(BYTE* node, BYTE* quad)
{
    QuadToPixelsFn original = reinterpret_cast<QuadToPixelsFn>(
        g_fixesBase + kQuadToPixelsRva);
    const BYTE* texture = *reinterpret_cast<BYTE**>(quad + 8);
    const FloatingBuilding* building = FindFloatingBuilding(texture);
    LONG* corners = reinterpret_cast<LONG*>(quad + 0x44);
    LONG saved[4];
    LONG top;
    LONG bottom;
    LONG span;
    LONG shift;
    DWORD index;

    if (building == NULL)
    {
        original(node, quad);
        return;
    }

    span = *reinterpret_cast<const SHORT*>(texture + 10) -
        *reinterpret_cast<const SHORT*>(texture + 6) - 2;
    top = corners[1];
    bottom = corners[1];
    for (index = 0; index < 4; ++index)
    {
        LONG y = corners[index * 2 + 1];

        saved[index] = y;
        if (y < top)
        {
            top = y;
        }

        if (y > bottom)
        {
            bottom = y;
        }
    }

    if (span <= 0 || bottom <= top)
    {
        original(node, quad);
        return;
    }

    shift = MulDiv(bottom - top, building->rows, span);
    for (index = 0; index < 4; ++index)
    {
        corners[index * 2 + 1] += shift;
    }

    original(node, quad);

    for (index = 0; index < 4; ++index)
    {
        corners[index * 2 + 1] = saved[index];
    }
}

static void LowerFloatingBuildings()
{
    BYTE calls[2][5];
    DWORD site;

    for (site = 0; site < 2; ++site)
    {
        DWORD callEnd = g_fixesBase + kQuadToPixelsCalls[site] + 5;

        calls[site][0] = 0xE8;
        StoreDword(calls[site] + 1, g_fixesBase + kQuadToPixelsRva - callEnd);
        if (!MatchesGameBytes(kQuadToPixelsCalls[site], calls[site], 5))
        {
            LogFix("Floating buildings stay as they are: unexpected code in the quad list.");
            return;
        }

        StoreDword(
            calls[site] + 1,
            reinterpret_cast<DWORD>(HookedQuadToPixels) - callEnd);
    }

    for (site = 0; site < 2; ++site)
    {
        if (!WriteGameBytes(kQuadToPixelsCalls[site], calls[site], 5))
        {
            LogFix("Floating buildings stay as they are: the quad list could not be changed.");
            return;
        }
    }

    LogFix("Floating buildings: the red barn, the brown shop and the white house stand on the ground.");
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
        g_midiVolumeTolerant = TRUE;
        GuardMidiVolume();
    }

    if (iniPath != NULL)
    {
        g_musicDevice = static_cast<LONG>(
            GetPrivateProfileIntA("Fixes", "MusicDevice", -1, iniPath));
    }

    InstallMidiLogging();

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "FullRateObjects", 1, iniPath) != 0)
    {
        FullRateObjects();
    }

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "FixPunchShadow", 1, iniPath) != 0)
    {
        FixPunchShadow();
    }

    if (iniPath == NULL ||
        GetPrivateProfileIntA("Fixes", "GroundBuildings", 1, iniPath) != 0)
    {
        LowerFloatingBuildings();
    }
}
