#include "road_rash_patch.h"

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

typedef HRESULT (WINAPI *DirectDrawSetDisplayModeFn)(
    LPVOID,
    DWORD,
    DWORD,
    DWORD);
typedef HRESULT (WINAPI *DirectDrawCreateSurfaceFn)(
    LPVOID,
    LPVOID,
    LPVOID*,
    LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumModesCallback)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumDisplayModesFn)(
    LPVOID,
    DWORD,
    LPVOID,
    LPVOID,
    DirectDrawEnumModesCallback);

struct DirectDrawHookState
{
    LPVOID object;
    LPVOID* vtable;
    DirectDrawCreateSurfaceFn originalCreateSurface;
    DirectDrawSetDisplayModeFn originalSetDisplayMode;
};

static const DWORD kExpectedTimestamp = 0x321DCBE7;
static const DWORD kExpectedImageBase = 0x00400000;
static const DWORD kExpectedImageSize = 0x000D8000;
static const DWORD kGameModeSetDisplayModeReturnRva = 0x00048903;
static const DWORD kMenuSetDisplayModeReturnRva = 0x000485F1;
static const DWORD kMenuSetDisplayMode16ReturnRva = 0x00048609;
static const DWORD kPrimaryCreateReturnRva = 0x00048A58;
// The HUD/cockpit draw call (E8 rel32 to the HUD function) and the JZ that
// shrinks the message rectangle to 640 pixels when the display origin is not 0.
static const DWORD kHudCallRva = 0x0004C7B9;
static const DWORD kHudFunctionRva = 0x00046C30;
static const DWORD kHudMessageJumpRva = 0x00047322;
static const DWORD kHudCockpitWidth = 640;
static const DWORD kCreateSurfaceVtableIndex = 6;
static const DWORD kEnumDisplayModesVtableIndex = 8;
static const DWORD kWaitForVerticalBlankVtableIndex = 22;
static const DWORD kSetDisplayModeVtableIndex = 21;
static const DWORD kSurfaceReleaseVtableIndex = 2;
static const DWORD kSurfaceBltVtableIndex = 5;
static const DWORD kSurfaceFlipVtableIndex = 11;
static const DWORD kSurfaceGetAttachedVtableIndex = 12;
static const DWORD kSurfaceVtableRangeCount = 23;
static const DWORD kSurfaceGetFlipStatusVtableIndex = 18;
static const DWORD kSurfaceIsLostVtableIndex = 24;
static const DWORD kDdsdCaps = 0x00000001;
static const DWORD kDdsCapsBackBuffer = 0x00000004;
static const DWORD kDdsCapsOffscreenPlain = 0x00000040;
static const DWORD kDdsCapsPrimarySurface = 0x00000200;
static const DWORD kDdsCapsPrimaryFlipChain = 0x00000218;
static const DWORD kDdsdPrimaryChainFlags = 0x00000021;
static const DWORD kDdsurfaceDescSize = 0x0000006C;
static const DWORD kDdsurfaceDescFlagsOffset = 0x00000004;
static const DWORD kDdsurfaceDescBackBufferCountOffset = 0x00000014;
static const DWORD kDdsurfaceDescCapsOffset = 0x00000068;
static const DWORD kDdbltWait = 0x01000000;
static const DWORD kDdWaitVbBlockBegin = 0x00000001;
static const HRESULT kDdEnumReturnOk = 1;
// DDSURFACEDESC v1 places dimensions at 0x08/0x0C and RGB bit count at 0x54.
static const DWORD kDdsdHeight = 0x00000002;
static const DWORD kDdsdWidth = 0x00000004;
static const DWORD kDdsdPixelFormat = 0x00001000;
static const DWORD kDdsurfaceDescV1MinimumSize = 0x00000058;
static const DWORD kDdsurfaceDescHeightOffset = 0x00000008;
static const DWORD kDdsurfaceDescWidthOffset = 0x0000000C;
static const DWORD kDdPixelFormatRgbBitCountOffset = 0x00000054;

static const DWORD kHighResolutionFlagRva = 0x00075380;
static const DWORD kLogicalWidthARva = 0x00066674;
static const DWORD kLogicalWidthBRva = 0x0006667C;
static const DWORD kLogicalHeightRva = 0x00066678;
static const DWORD kDisplayOriginXRva = 0x00075394;
static const DWORD kDisplayOriginYRva = 0x00075398;
static const DWORD kDisplayWidthRva = 0x0007539C;
static const DWORD kDisplayHeightRva = 0x000753A0;
static const DWORD kClipLeftRva = 0x000B7EB0;
static const DWORD kClipTopRva = 0x000B7EB4;
static const DWORD kClipRightRva = 0x000B7EB8;
static const DWORD kClipBottomRva = 0x000B7EBC;

static const DWORD kOriginalLogicalWidth = 320;
static const DWORD kOriginalLogicalHeight = 240;
static const DWORD kOriginalDisplayWidth = 640;
static const DWORD kOriginalDisplayHeight = 480;
// The game renders into a 856x480 virtual buffer that is stretched to the
// real 1920x1080 display mode when it flips.
static const DWORD kVirtualLogicalWidth = 428;
static const DWORD kVirtualDisplayWidth = 856;
static const DWORD kTargetDisplayWidth = 1920;
static const DWORD kTargetDisplayHeight = 1080;
static const DWORD kFallbackLogicalWidth = 360;
static const DWORD kFallbackDisplayWidth = 720;

typedef ULONG (WINAPI *SurfaceReleaseFn)(LPVOID);
typedef HRESULT (WINAPI *SurfaceBltFn)(
    LPVOID,
    LPVOID,
    LPVOID,
    LPVOID,
    DWORD,
    LPVOID);
typedef HRESULT (WINAPI *SurfaceFlipFn)(LPVOID, LPVOID, DWORD);
typedef HRESULT (WINAPI *SurfaceGetAttachedFn)(LPVOID, LPVOID, LPVOID*);
typedef HRESULT (WINAPI *WaitForVerticalBlankFn)(LPVOID, DWORD, HANDLE);

static DirectDrawHookState g_hookState;
static DWORD g_gameBase = 0;
static BOOL g_widescreenActive = FALSE;
static BOOL g_virtualDisplayActive = FALSE;
static LPVOID g_virtualPrimary = NULL;
static LPVOID g_virtualBack = NULL;
static LPVOID* g_surfaceVtable = NULL;
static SurfaceReleaseFn g_originalSurfaceRelease = NULL;
static SurfaceBltFn g_originalSurfaceBlt = NULL;
static SurfaceFlipFn g_originalSurfaceFlip = NULL;
typedef HRESULT (WINAPI *SurfaceQueryFn)(LPVOID, DWORD);
typedef HRESULT (WINAPI *SurfaceIsLostFn)(LPVOID);
static SurfaceQueryFn g_originalSurfaceGetFlipStatus = NULL;
static SurfaceIsLostFn g_originalSurfaceIsLost = NULL;
static DWORD g_queryLogCount = 0;
static DWORD g_backLogCount = 0;
static SurfaceGetAttachedFn g_originalSurfaceGetAttached = NULL;
static DWORD g_flipLogCount = 0;
static DWORD g_displayModeLogCount = 0;
static BOOL g_nativeDisplayModesLogged = FALSE;

static void LogMessage(const char* message, DWORD length)
{
    CHAR path[MAX_PATH];
    DWORD pathLength = GetModuleFileNameA(NULL, path, MAX_PATH);
    DWORD directoryLength;
    static const CHAR fileName[] = "RoadRashWidescreen.log";
    HANDLE file;
    DWORD bytesWritten;

    if (pathLength == 0 || pathLength >= MAX_PATH)
    {
        OutputDebugStringA(message);
        return;
    }

    directoryLength = pathLength;
    while (directoryLength > 0 &&
           path[directoryLength - 1] != '\\' &&
           path[directoryLength - 1] != '/')
    {
        --directoryLength;
    }

    if (directoryLength == 0 ||
        directoryLength + sizeof(fileName) > MAX_PATH)
    {
        OutputDebugStringA(message);
        return;
    }

    lstrcpyA(path + directoryLength, fileName);

    file = CreateFileA(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        OutputDebugStringA(message);
        return;
    }

    if (!WriteFile(file, message, length, &bytesWritten, NULL) ||
        bytesWritten != length)
    {
        OutputDebugStringA(message);
    }
    CloseHandle(file);
}

static void AppendHex32(CHAR* message, DWORD* length, DWORD value)
{
    static const CHAR digits[] = "0123456789ABCDEF";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4)
    {
        message[*length] = digits[(value >> shift) & 0x0F];
        ++*length;
    }
}

static void LogDisplayModeRequest(
    DWORD caller,
    DWORD width,
    DWORD height,
    DWORD bitsPerPixel)
{
    CHAR message[128];
    DWORD length;

    lstrcpyA(message, "SetDisplayMode caller=0x");
    length = sizeof("SetDisplayMode caller=0x") - 1;
    AppendHex32(message, &length, caller);
    lstrcpyA(message + length, " mode=0x");
    length += sizeof(" mode=0x") - 1;
    AppendHex32(message, &length, width);
    lstrcpyA(message + length, "x");
    ++length;
    AppendHex32(message, &length, height);
    lstrcpyA(message + length, " bpp=0x");
    length += sizeof(" bpp=0x") - 1;
    AppendHex32(message, &length, bitsPerPixel);
    lstrcpyA(message + length, "\r\n");
    length += sizeof("\r\n") - 1;
    LogMessage(message, length);
}

static void LogHRESULTResult(
    const CHAR* prefix,
    DWORD prefixLength,
    HRESULT result)
{
    CHAR message[64];
    DWORD length = prefixLength;

    lstrcpyA(message, prefix);
    AppendHex32(message, &length, static_cast<DWORD>(result));
    lstrcpyA(message + length, "\r\n");
    length += sizeof("\r\n") - 1;
    LogMessage(message, length);
}

static void LogDisplayModeResult(HRESULT result)
{
    LogHRESULTResult(
        "SetDisplayMode result=0x",
        sizeof("SetDisplayMode result=0x") - 1,
        result);
}

struct NativeDisplayModeLogContext
{
    struct Mode
    {
        DWORD width;
        DWORD height;
        DWORD bitsPerPixel;
    };

    Mode modes[64];
    DWORD loggedModes;
    BOOL hasTarget8BitMode;
    BOOL hasFallback8BitMode;
};

static void LogNativeDisplayMode(
    DWORD width,
    DWORD height,
    DWORD bitsPerPixel)
{
    CHAR message[128];
    DWORD length;

    lstrcpyA(message, "Native DirectDraw mode=0x");
    length = sizeof("Native DirectDraw mode=0x") - 1;
    AppendHex32(message, &length, width);
    lstrcpyA(message + length, "x");
    ++length;
    AppendHex32(message, &length, height);
    lstrcpyA(message + length, " bpp=0x");
    length += sizeof(" bpp=0x") - 1;
    AppendHex32(message, &length, bitsPerPixel);
    lstrcpyA(message + length, "\r\n");
    length += sizeof("\r\n") - 1;
    LogMessage(message, length);
}

static HRESULT WINAPI LogNativeDisplayModeCallback(
    LPVOID surfaceDescription,
    LPVOID context)
{
    BYTE* description = reinterpret_cast<BYTE*>(surfaceDescription);
    NativeDisplayModeLogContext* logContext =
        reinterpret_cast<NativeDisplayModeLogContext*>(context);
    DWORD descriptionSize;
    DWORD descriptionFlags;
    DWORD width;
    DWORD height;
    DWORD bitsPerPixel;

    if (description == NULL || logContext == NULL)
    {
        return kDdEnumReturnOk;
    }

    descriptionSize = *reinterpret_cast<DWORD*>(description);
    if (descriptionSize < kDdsurfaceDescV1MinimumSize)
    {
        return kDdEnumReturnOk;
    }

    descriptionFlags = *reinterpret_cast<DWORD*>(
        description + sizeof(DWORD));
    if ((descriptionFlags &
            (kDdsdHeight | kDdsdWidth | kDdsdPixelFormat)) !=
        (kDdsdHeight | kDdsdWidth | kDdsdPixelFormat))
    {
        return kDdEnumReturnOk;
    }

    height = *reinterpret_cast<DWORD*>(
        description + kDdsurfaceDescHeightOffset);
    width = *reinterpret_cast<DWORD*>(
        description + kDdsurfaceDescWidthOffset);
    bitsPerPixel = *reinterpret_cast<DWORD*>(
        description + kDdPixelFormatRgbBitCountOffset);

    if (bitsPerPixel != 8 &&
        bitsPerPixel != 16 &&
        bitsPerPixel != 24 &&
        bitsPerPixel != 32)
    {
        return kDdEnumReturnOk;
    }

    if (width == kTargetDisplayWidth &&
        height == kTargetDisplayHeight &&
        bitsPerPixel == 8)
    {
        logContext->hasTarget8BitMode = TRUE;
    }

    if (width == kFallbackDisplayWidth &&
        height == kOriginalDisplayHeight &&
        bitsPerPixel == 8)
    {
        logContext->hasFallback8BitMode = TRUE;
    }

    if (logContext->loggedModes < sizeof(logContext->modes) / sizeof(logContext->modes[0]))
    {
        NativeDisplayModeLogContext::Mode* mode =
            &logContext->modes[logContext->loggedModes];
        mode->width = width;
        mode->height = height;
        mode->bitsPerPixel = bitsPerPixel;
        ++logContext->loggedModes;
    }

    return kDdEnumReturnOk;
}

static void LogNativeDisplayModes(LPVOID directDraw)
{
    DirectDrawEnumDisplayModesFn enumerateModes;
    NativeDisplayModeLogContext logContext;
    DWORD modeIndex;
    HRESULT result;

    if (g_nativeDisplayModesLogged)
    {
        return;
    }

    g_nativeDisplayModesLogged = TRUE;
    if (g_hookState.vtable == NULL ||
        g_hookState.vtable[kEnumDisplayModesVtableIndex] == NULL)
    {
        LogMessage(
            "Could not enumerate native DirectDraw modes: missing vtable entry.\r\n",
            sizeof("Could not enumerate native DirectDraw modes: missing vtable entry.\r\n") - 1);
        return;
    }

    enumerateModes = reinterpret_cast<DirectDrawEnumDisplayModesFn>(
        g_hookState.vtable[kEnumDisplayModesVtableIndex]);
    logContext.loggedModes = 0;
    logContext.hasTarget8BitMode = FALSE;
    logContext.hasFallback8BitMode = FALSE;
    result = enumerateModes(
        directDraw,
        0,
        NULL,
        &logContext,
        LogNativeDisplayModeCallback);
    LogHRESULTResult(
        "EnumDisplayModes result=0x",
        sizeof("EnumDisplayModes result=0x") - 1,
        result);

    if (SUCCEEDED(result))
    {
        for (modeIndex = 0; modeIndex < logContext.loggedModes; ++modeIndex)
        {
            LogNativeDisplayMode(
                logContext.modes[modeIndex].width,
                logContext.modes[modeIndex].height,
                logContext.modes[modeIndex].bitsPerPixel);
        }

        if (logContext.hasTarget8BitMode)
        {
            LogMessage(
                "Native DirectDraw mode list includes 1920x1080x8.\r\n",
                sizeof("Native DirectDraw mode list includes 1920x1080x8.\r\n") - 1);
        }
        else
        {
            LogMessage(
                "Native DirectDraw mode list does not include 1920x1080x8.\r\n",
                sizeof("Native DirectDraw mode list does not include 1920x1080x8.\r\n") - 1);
        }

        if (logContext.hasFallback8BitMode)
        {
            LogMessage(
                "Native DirectDraw mode list includes 720x480x8.\r\n",
                sizeof("Native DirectDraw mode list includes 720x480x8.\r\n") - 1);
        }
        else
        {
            LogMessage(
                "Native DirectDraw mode list does not include 720x480x8.\r\n",
                sizeof("Native DirectDraw mode list does not include 720x480x8.\r\n") - 1);
        }
    }
}

static HRESULT CallOriginalSetDisplayMode(
    LPVOID directDraw,
    DWORD width,
    DWORD height,
    DWORD bitsPerPixel,
    DWORD caller)
{
    BOOL logCall = g_displayModeLogCount < 8;
    HRESULT result;

    if (logCall)
    {
        ++g_displayModeLogCount;
        LogDisplayModeRequest(caller, width, height, bitsPerPixel);
    }

    result = g_hookState.originalSetDisplayMode(
        directDraw,
        width,
        height,
        bitsPerPixel);

    if (logCall)
    {
        LogDisplayModeResult(result);
    }

    if (SUCCEEDED(result) && directDraw == g_hookState.object)
    {
        LogNativeDisplayModes(directDraw);
    }

    return result;
}

static BOOL MatchesBytes(
    const BYTE* actual,
    const BYTE* expected,
    DWORD length)
{
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

static DWORD FindSupportedGameBase()
{
    HMODULE module = GetModuleHandleA(NULL);
    BYTE* image;
    PIMAGE_DOS_HEADER dosHeader;
    PIMAGE_NT_HEADERS32 ntHeaders;
    static const BYTE modeSetupSignature[] = {
        0xB8, 0x80, 0x02, 0x00, 0x00, 0x8A, 0xCB, 0xD3, 0xF8
    };
    static const BYTE setDisplayModeCall[] = {
        0xFF, 0x55, 0x54
    };
    static const BYTE primaryCreateCall[] = {
        0xFF, 0x53, 0x18
    };
    static const BYTE getAttachedSurfaceCall[] = {
        0xFF, 0x53, 0x30
    };
    static const BYTE hudCall[] = {
        0xE8, 0x72, 0xA4, 0xFF, 0xFF
    };
    static const BYTE hudMessageJump[] = {
        0x74, 0x0B, 0xC7, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x80, 0x02
    };

    if (module == NULL)
    {
        return 0;
    }

    image = reinterpret_cast<BYTE*>(module);
    dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(image);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
        dosHeader->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
        dosHeader->e_lfanew > 0x10000)
    {
        return 0;
    }

    ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS32>(
        image + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        ntHeaders->FileHeader.TimeDateStamp != kExpectedTimestamp ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        ntHeaders->OptionalHeader.ImageBase != kExpectedImageBase ||
        ntHeaders->OptionalHeader.SizeOfImage != kExpectedImageSize)
    {
        return 0;
    }

    if (!MatchesBytes(
            image + 0x00048888,
            modeSetupSignature,
            sizeof(modeSetupSignature)) ||
        !MatchesBytes(
            image + 0x00048900,
            setDisplayModeCall,
            sizeof(setDisplayModeCall)) ||
        !MatchesBytes(
            image + 0x00048A55,
            primaryCreateCall,
            sizeof(primaryCreateCall)) ||
        !MatchesBytes(
            image + 0x00048A70,
            getAttachedSurfaceCall,
            sizeof(getAttachedSurfaceCall)) ||
        !MatchesBytes(
            image + kHudCallRva,
            hudCall,
            sizeof(hudCall)) ||
        !MatchesBytes(
            image + kHudMessageJumpRva,
            hudMessageJump,
            sizeof(hudMessageJump)))
    {
        return 0;
    }

    return reinterpret_cast<DWORD>(module);
}

static DWORD ReadGameDword(DWORD rva)
{
    return *reinterpret_cast<DWORD*>(g_gameBase + rva);
}

static void WriteGameDword(DWORD rva, DWORD value)
{
    *reinterpret_cast<DWORD*>(g_gameBase + rva) = value;
}

typedef DWORD (__cdecl *HudDrawFn)(void);

// Draws the cockpit, gauges, and text with the display origin shifted so the
// 640-pixel-wide HUD is centered in the wider virtual buffer.
static DWORD __cdecl HookedHudDraw(void)
{
    DWORD result;

    WriteGameDword(
        kDisplayOriginXRva,
        (kVirtualDisplayWidth - kHudCockpitWidth) / 2);
    result = reinterpret_cast<HudDrawFn>(g_gameBase + kHudFunctionRva)();
    WriteGameDword(kDisplayOriginXRva, 0);
    return result;
}

static void WriteGameCode(DWORD rva, const void* bytes, DWORD length)
{
    BYTE* destination = reinterpret_cast<BYTE*>(g_gameBase + rva);
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
        return;
    }

    for (index = 0; index < length; ++index)
    {
        destination[index] = source[index];
    }

    VirtualProtect(destination, length, oldProtection, &ignoredProtection);
    FlushInstructionCache(GetCurrentProcess(), destination, length);
}

static void SetHudCentering(BOOL enable)
{
    BYTE call[5];
    BYTE jump = enable ? 0xEB : 0x74;
    DWORD target = enable ?
        reinterpret_cast<DWORD>(HookedHudDraw) :
        g_gameBase + kHudFunctionRva;
    DWORD relative = target - (g_gameBase + kHudCallRva + 5);

    call[0] = 0xE8;
    *reinterpret_cast<DWORD*>(call + 1) = relative;
    WriteGameCode(kHudCallRva, call, sizeof(call));
    WriteGameCode(kHudMessageJumpRva, &jump, 1);
}

static void SetWidescreenGameState(
    DWORD logicalWidth,
    DWORD logicalHeight,
    DWORD displayWidth,
    DWORD displayHeight)
{
    WriteGameDword(kLogicalWidthARva, logicalWidth);
    WriteGameDword(kLogicalWidthBRva, logicalWidth);
    WriteGameDword(kLogicalHeightRva, logicalHeight);
    WriteGameDword(kDisplayOriginXRva, 0);
    WriteGameDword(kDisplayOriginYRva, 0);
    WriteGameDword(kDisplayWidthRva, displayWidth);
    WriteGameDword(kDisplayHeightRva, displayHeight);
    WriteGameDword(kClipLeftRva, 0);
    WriteGameDword(kClipTopRva, 0);
    WriteGameDword(kClipRightRva, displayWidth);
    WriteGameDword(kClipBottomRva, displayHeight - 1);
}

static void RestoreOriginalGameState()
{
    WriteGameDword(kLogicalWidthARva, kOriginalLogicalWidth);
    WriteGameDword(kLogicalWidthBRva, kOriginalLogicalWidth);
    WriteGameDword(kLogicalHeightRva, kOriginalLogicalHeight);
    WriteGameDword(kDisplayOriginXRva, 0);
    WriteGameDword(kDisplayOriginYRva, 0);
    WriteGameDword(kDisplayWidthRva, kOriginalDisplayWidth);
    WriteGameDword(kDisplayHeightRva, kOriginalDisplayHeight);
    WriteGameDword(kClipLeftRva, 0);
    WriteGameDword(kClipTopRva, 0);
    WriteGameDword(kClipRightRva, kOriginalDisplayWidth);
    WriteGameDword(kClipBottomRva, kOriginalDisplayHeight - 1);
    if (g_virtualDisplayActive)
    {
        SetHudCentering(FALSE);
    }

    g_virtualDisplayActive = FALSE;
}

static void ZeroDescription(BYTE* description)
{
    volatile DWORD* words = reinterpret_cast<volatile DWORD*>(description);
    DWORD index;

    for (index = 0; index < kDdsurfaceDescSize / sizeof(DWORD); ++index)
    {
        words[index] = 0;
    }
}

static void InstallSurfaceHooks(LPVOID surface);

static HRESULT CreateVirtualPrimary(
    LPVOID directDraw,
    LPVOID surfaceDescription,
    LPVOID* surface,
    LPVOID outer)
{
    BYTE* description = reinterpret_cast<BYTE*>(surfaceDescription);
    BYTE backDescription[kDdsurfaceDescSize];
    DWORD* flags;
    DWORD* backBufferCount;
    DWORD* caps;
    DWORD originalFlags;
    DWORD originalBackBufferCount;
    DWORD originalCaps;
    LPVOID backBuffer = NULL;
    HRESULT result;

    if (description == NULL ||
        surface == NULL ||
        *reinterpret_cast<DWORD*>(description) < kDdsurfaceDescSize)
    {
        return E_FAIL;
    }

    flags = reinterpret_cast<DWORD*>(
        description + kDdsurfaceDescFlagsOffset);
    backBufferCount = reinterpret_cast<DWORD*>(
        description + kDdsurfaceDescBackBufferCountOffset);
    caps = reinterpret_cast<DWORD*>(
        description + kDdsurfaceDescCapsOffset);
    if (*flags != kDdsdPrimaryChainFlags || *caps != kDdsCapsPrimaryFlipChain)
    {
        LogMessage(
            "Virtual display skipped; the primary surface descriptor was unexpected.\r\n",
            sizeof("Virtual display skipped; the primary surface descriptor was unexpected.\r\n") - 1);
        return g_hookState.originalCreateSurface(
            directDraw,
            surfaceDescription,
            surface,
            outer);
    }

    originalFlags = *flags;
    originalBackBufferCount = *backBufferCount;
    originalCaps = *caps;
    *flags = kDdsdCaps;
    *backBufferCount = 0;
    *caps = kDdsCapsPrimarySurface;
    result = g_hookState.originalCreateSurface(
        directDraw,
        surfaceDescription,
        surface,
        outer);
    *flags = originalFlags;
    *backBufferCount = originalBackBufferCount;
    *caps = originalCaps;
    if (FAILED(result))
    {
        LogHRESULTResult(
            "Real primary surface creation failed=0x",
            sizeof("Real primary surface creation failed=0x") - 1,
            result);
        return result;
    }

    InstallSurfaceHooks(*surface);
    ZeroDescription(backDescription);
    *reinterpret_cast<DWORD*>(backDescription) = kDdsurfaceDescSize;
    *reinterpret_cast<DWORD*>(backDescription + kDdsurfaceDescFlagsOffset) =
        kDdsdCaps | kDdsdHeight | kDdsdWidth;
    *reinterpret_cast<DWORD*>(backDescription + kDdsurfaceDescHeightOffset) =
        kOriginalDisplayHeight;
    *reinterpret_cast<DWORD*>(backDescription + kDdsurfaceDescWidthOffset) =
        kVirtualDisplayWidth;
    *reinterpret_cast<DWORD*>(backDescription + kDdsurfaceDescCapsOffset) =
        kDdsCapsOffscreenPlain;
    result = g_hookState.originalCreateSurface(
        directDraw,
        backDescription,
        &backBuffer,
        NULL);
    if (FAILED(result))
    {
        LogHRESULTResult(
            "Virtual back buffer creation failed=0x",
            sizeof("Virtual back buffer creation failed=0x") - 1,
            result);
        g_originalSurfaceRelease(*surface);
        *surface = NULL;
        return result;
    }

    g_virtualPrimary = *surface;
    g_virtualBack = backBuffer;
    LogMessage(
        "Created the real 1920x1080 primary surface and a 856x480 virtual back buffer.\r\n",
        sizeof("Created the real 1920x1080 primary surface and a 856x480 virtual back buffer.\r\n") - 1);
    return result;
}

static ULONG WINAPI HookedSurfaceRelease(LPVOID surface)
{
    ULONG count = g_originalSurfaceRelease(surface);

    if (surface == g_virtualPrimary && count == 0)
    {
        LPVOID back = g_virtualBack;

        g_virtualPrimary = NULL;
        g_virtualBack = NULL;
        if (back != NULL)
        {
            g_originalSurfaceRelease(back);
        }
    }

    return count;
}

static HRESULT WINAPI HookedSurfaceGetAttached(
    LPVOID surface,
    LPVOID capabilities,
    LPVOID* attached)
{
    if (surface == g_virtualPrimary &&
        g_virtualBack != NULL &&
        capabilities != NULL &&
        attached != NULL &&
        (*reinterpret_cast<DWORD*>(capabilities) & kDdsCapsBackBuffer) != 0)
    {
        typedef ULONG (WINAPI *AddRefFn)(LPVOID);
        LPVOID* backVtable = *reinterpret_cast<LPVOID**>(g_virtualBack);

        reinterpret_cast<AddRefFn>(backVtable[1])(g_virtualBack);
        *attached = g_virtualBack;
        LogMessage(
            "Returned the virtual back buffer to GetAttachedSurface.\r\n",
            sizeof("Returned the virtual back buffer to GetAttachedSurface.\r\n") - 1);
        return S_OK;
    }

    return g_originalSurfaceGetAttached(surface, capabilities, attached);
}

static HRESULT WINAPI HookedSurfaceFlip(
    LPVOID surface,
    LPVOID target,
    DWORD flags)
{
    HRESULT result;

    if (surface != g_virtualPrimary || g_virtualBack == NULL)
    {
        if (g_virtualDisplayActive && g_flipLogCount < 8)
        {
            ++g_flipLogCount;
            LogMessage(
                "Flip called on a non-virtual surface.\r\n",
                sizeof("Flip called on a non-virtual surface.\r\n") - 1);
        }
        return g_originalSurfaceFlip(surface, target, flags);
    }

    if (g_flipLogCount < 4)
    {
        LogMessage(
            "Virtual Flip entered.\r\n",
            sizeof("Virtual Flip entered.\r\n") - 1);
    }

    result = g_originalSurfaceBlt(
        surface,
        NULL,
        g_virtualBack,
        NULL,
        kDdbltWait,
        NULL);
    if (g_flipLogCount < 4 || (FAILED(result) && g_flipLogCount < 8))
    {
        ++g_flipLogCount;
        LogHRESULTResult(
            "Virtual present result=0x",
            sizeof("Virtual present result=0x") - 1,
            result);
    }

    return result;
}

static HRESULT WINAPI HookedSurfaceGetFlipStatus(LPVOID surface, DWORD flags)
{
    HRESULT result = g_originalSurfaceGetFlipStatus(surface, flags);

    if (surface == g_virtualPrimary && g_queryLogCount < 6)
    {
        ++g_queryLogCount;
        LogHRESULTResult(
            "GetFlipStatus on virtual primary=0x",
            sizeof("GetFlipStatus on virtual primary=0x") - 1,
            result);
    }

    return result;
}

static void EnsureFlipHook(LPVOID surface)
{
    LPVOID* vtable = *reinterpret_cast<LPVOID**>(surface);
    LPVOID current = vtable[kSurfaceFlipVtableIndex];
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (current == reinterpret_cast<LPVOID>(HookedSurfaceFlip))
    {
        return;
    }

    if (!VirtualProtect(
            &vtable[kSurfaceFlipVtableIndex],
            sizeof(LPVOID),
            PAGE_READWRITE,
            &oldProtection))
    {
        return;
    }

    g_originalSurfaceFlip = reinterpret_cast<SurfaceFlipFn>(current);
    vtable[kSurfaceFlipVtableIndex] =
        reinterpret_cast<LPVOID>(HookedSurfaceFlip);
    VirtualProtect(
        &vtable[kSurfaceFlipVtableIndex],
        sizeof(LPVOID),
        oldProtection,
        &ignoredProtection);
    LogMessage(
        "The surface Flip slot had been replaced; re-installed the Flip hook.\r\n",
        sizeof("The surface Flip slot had been replaced; re-installed the Flip hook.\r\n") - 1);
}

static HRESULT WINAPI HookedSurfaceIsLost(LPVOID surface)
{
    HRESULT result = g_originalSurfaceIsLost(surface);

    if (surface == g_virtualPrimary)
    {
        EnsureFlipHook(surface);
    }

    if (surface == g_virtualPrimary && g_queryLogCount < 6)
    {
        ++g_queryLogCount;
        LogHRESULTResult(
            "IsLost on virtual PRIMARY=0x",
            sizeof("IsLost on virtual PRIMARY=0x") - 1,
            result);
    }
    else if (surface == g_virtualBack && g_backLogCount < 3)
    {
        ++g_backLogCount;
        LogHRESULTResult(
            "IsLost on virtual back buffer=0x",
            sizeof("IsLost on virtual back buffer=0x") - 1,
            result);
    }

    return result;
}

static void InstallSurfaceHooks(LPVOID surface)
{
    LPVOID* vtable;
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (surface == NULL || g_surfaceVtable != NULL)
    {
        return;
    }

    vtable = *reinterpret_cast<LPVOID**>(surface);
    if (vtable == NULL ||
        vtable[kSurfaceReleaseVtableIndex] == NULL ||
        vtable[kSurfaceBltVtableIndex] == NULL ||
        vtable[kSurfaceFlipVtableIndex] == NULL ||
        vtable[kSurfaceGetAttachedVtableIndex] == NULL ||
        vtable[kSurfaceGetFlipStatusVtableIndex] == NULL ||
        vtable[kSurfaceIsLostVtableIndex] == NULL)
    {
        return;
    }

    if (!VirtualProtect(
            &vtable[kSurfaceReleaseVtableIndex],
            kSurfaceVtableRangeCount * sizeof(LPVOID),
            PAGE_READWRITE,
            &oldProtection))
    {
        LogMessage(
            "Could not update the DirectDraw surface vtable.\r\n",
            sizeof("Could not update the DirectDraw surface vtable.\r\n") - 1);
        return;
    }

    g_originalSurfaceRelease = reinterpret_cast<SurfaceReleaseFn>(
        vtable[kSurfaceReleaseVtableIndex]);
    g_originalSurfaceBlt = reinterpret_cast<SurfaceBltFn>(
        vtable[kSurfaceBltVtableIndex]);
    g_originalSurfaceFlip = reinterpret_cast<SurfaceFlipFn>(
        vtable[kSurfaceFlipVtableIndex]);
    g_originalSurfaceGetAttached = reinterpret_cast<SurfaceGetAttachedFn>(
        vtable[kSurfaceGetAttachedVtableIndex]);
    vtable[kSurfaceReleaseVtableIndex] =
        reinterpret_cast<LPVOID>(HookedSurfaceRelease);
    vtable[kSurfaceFlipVtableIndex] =
        reinterpret_cast<LPVOID>(HookedSurfaceFlip);
    vtable[kSurfaceGetAttachedVtableIndex] =
        reinterpret_cast<LPVOID>(HookedSurfaceGetAttached);
    g_originalSurfaceGetFlipStatus = reinterpret_cast<SurfaceQueryFn>(
        vtable[kSurfaceGetFlipStatusVtableIndex]);
    g_originalSurfaceIsLost = reinterpret_cast<SurfaceIsLostFn>(
        vtable[kSurfaceIsLostVtableIndex]);
    vtable[kSurfaceGetFlipStatusVtableIndex] =
        reinterpret_cast<LPVOID>(HookedSurfaceGetFlipStatus);
    vtable[kSurfaceIsLostVtableIndex] =
        reinterpret_cast<LPVOID>(HookedSurfaceIsLost);
    g_surfaceVtable = vtable;
    VirtualProtect(
        &vtable[kSurfaceReleaseVtableIndex],
        kSurfaceVtableRangeCount * sizeof(LPVOID),
        oldProtection,
        &ignoredProtection);
    LogMessage(
        "Installed the in-place DirectDraw surface Release/Flip/GetAttachedSurface hooks.\r\n",
        sizeof("Installed the in-place DirectDraw surface Release/Flip/GetAttachedSurface hooks.\r\n") - 1);
}

static HRESULT WINAPI HookedCreateSurface(
    LPVOID directDraw,
    LPVOID surfaceDescription,
    LPVOID* surface,
    LPVOID outer)
{
    DWORD caller = reinterpret_cast<DWORD>(_ReturnAddress());
    HRESULT result;

    if (g_hookState.originalCreateSurface == NULL)
    {
        return E_FAIL;
    }

    if (directDraw == g_hookState.object &&
        g_virtualDisplayActive &&
        caller == g_gameBase + kPrimaryCreateReturnRva)
    {
        return CreateVirtualPrimary(
            directDraw,
            surfaceDescription,
            surface,
            outer);
    }

    result = g_hookState.originalCreateSurface(
        directDraw,
        surfaceDescription,
        surface,
        outer);
    if (SUCCEEDED(result) && surface != NULL)
    {
        InstallSurfaceHooks(*surface);
    }

    return result;
}


static HRESULT WINAPI HookedSetDisplayMode(
    LPVOID directDraw,
    DWORD width,
    DWORD height,
    DWORD bitsPerPixel)
{
    DWORD caller = reinterpret_cast<DWORD>(_ReturnAddress());

    if (g_hookState.originalSetDisplayMode == NULL)
    {
        return E_FAIL;
    }

    if (directDraw != g_hookState.object)
    {
        return CallOriginalSetDisplayMode(
            directDraw,
            width,
            height,
            bitsPerPixel,
            caller);
    }

    if (caller == g_gameBase + kMenuSetDisplayModeReturnRva ||
        caller == g_gameBase + kMenuSetDisplayMode16ReturnRva)
    {
        if (g_widescreenActive)
        {
            RestoreOriginalGameState();
            g_widescreenActive = FALSE;
            LogMessage(
                "Widescreen mode ended; restored the 640x480 game state.\r\n",
                sizeof("Widescreen mode ended; restored the 640x480 game state.\r\n") - 1);
        }

        return CallOriginalSetDisplayMode(
            directDraw,
            width,
            height,
            bitsPerPixel,
            caller);
    }

    if (caller == g_gameBase + kGameModeSetDisplayModeReturnRva &&
        width == kOriginalDisplayWidth &&
        height == kOriginalDisplayHeight &&
        bitsPerPixel == 8)
    {
        HRESULT result;

        if (g_widescreenActive)
        {
            RestoreOriginalGameState();
            g_widescreenActive = FALSE;
        }

        if (*reinterpret_cast<BYTE*>(g_gameBase + kHighResolutionFlagRva) == 0 ||
            ReadGameDword(kLogicalWidthARva) != kOriginalLogicalWidth ||
            ReadGameDword(kLogicalWidthBRva) != kOriginalLogicalWidth ||
            ReadGameDword(kLogicalHeightRva) != kOriginalLogicalHeight ||
            ReadGameDword(kDisplayWidthRva) != kOriginalDisplayWidth ||
            ReadGameDword(kDisplayHeightRva) != kOriginalDisplayHeight)
        {
            LogMessage(
                "Widescreen skipped; the game is not in its expected 640x480 high-resolution mode.\r\n",
                sizeof("Widescreen skipped; the game is not in its expected 640x480 high-resolution mode.\r\n") - 1);
            return CallOriginalSetDisplayMode(
                directDraw,
                width,
                height,
                bitsPerPixel,
                caller);
        }

        result = CallOriginalSetDisplayMode(
            directDraw,
            kTargetDisplayWidth,
            kTargetDisplayHeight,
            bitsPerPixel,
            caller);
        if (SUCCEEDED(result))
        {
            SetWidescreenGameState(
                kVirtualLogicalWidth,
                kOriginalLogicalHeight,
                kVirtualDisplayWidth,
                kOriginalDisplayHeight);
            g_virtualDisplayActive = TRUE;
            SetHudCentering(TRUE);
            g_widescreenActive = TRUE;
            LogMessage(
                "Enabled the 856x480 virtual viewport stretched to 1920x1080.\r\n",
                sizeof("Enabled the 856x480 virtual viewport stretched to 1920x1080.\r\n") - 1);
            return result;
        }

        LogMessage(
            "1920x1080 was unavailable; trying the 720x480 fallback.\r\n",
            sizeof("1920x1080 was unavailable; trying the 720x480 fallback.\r\n") - 1);
        result = CallOriginalSetDisplayMode(
            directDraw,
            kFallbackDisplayWidth,
            kOriginalDisplayHeight,
            bitsPerPixel,
            caller);
        if (SUCCEEDED(result))
        {
            SetWidescreenGameState(
                kFallbackLogicalWidth,
                kOriginalLogicalHeight,
                kFallbackDisplayWidth,
                kOriginalDisplayHeight);
            g_widescreenActive = TRUE;
            LogMessage(
                "Enabled the experimental 720x480 (3:2) fallback viewport.\r\n",
                sizeof("Enabled the experimental 720x480 (3:2) fallback viewport.\r\n") - 1);
            return result;
        }

        result = CallOriginalSetDisplayMode(
            directDraw,
            width,
            height,
            bitsPerPixel,
            caller);
        if (FAILED(result))
        {
            LogMessage(
                "1920x1080, 720x480, and the original 640x480 display modes all failed.\r\n",
                sizeof("1920x1080, 720x480, and the original 640x480 display modes all failed.\r\n") - 1);
        }
        else
        {
            LogMessage(
                "1920x1080 and 720x480 were unavailable; retained the original 640x480 mode.\r\n",
                sizeof("1920x1080 and 720x480 were unavailable; retained the original 640x480 mode.\r\n") - 1);
        }

        return result;
    }

    return CallOriginalSetDisplayMode(
        directDraw,
        width,
        height,
        bitsPerPixel,
        caller);
}

BOOL InstallRoadRashWidescreenHook(LPVOID directDraw)
{
    DWORD base;
    LPVOID* originalVtable;
    LPVOID* firstHookSlot;
    LPVOID* displayModeSlot;
    DWORD hookRangeSize;
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (directDraw == NULL)
    {
        return FALSE;
    }

    if (g_gameBase == 0)
    {
        base = FindSupportedGameBase();
        if (base == 0)
        {
            return FALSE;
        }

        g_gameBase = base;
        LogMessage(
            "Recognized the supported Road Rash German 1.0A executable.\r\n",
            sizeof("Recognized the supported Road Rash German 1.0A executable.\r\n") - 1);
    }

    if (g_hookState.object == directDraw)
    {
        return TRUE;
    }

    originalVtable = *reinterpret_cast<LPVOID**>(directDraw);
    if (originalVtable == NULL ||
        originalVtable[kCreateSurfaceVtableIndex] == NULL ||
        originalVtable[kSetDisplayModeVtableIndex] == NULL)
    {
        LogMessage(
            "DirectDraw hook installation failed: invalid interface vtable.\r\n",
            sizeof("DirectDraw hook installation failed: invalid interface vtable.\r\n") - 1);
        return FALSE;
    }

    if (g_hookState.vtable != NULL)
    {
        if (g_hookState.vtable == originalVtable)
        {
            return TRUE;
        }

        LogMessage(
            "A second DirectDraw vtable was left unchanged.\r\n",
            sizeof("A second DirectDraw vtable was left unchanged.\r\n") - 1);
        return FALSE;
    }

    firstHookSlot = &originalVtable[kCreateSurfaceVtableIndex];
    displayModeSlot = &originalVtable[kSetDisplayModeVtableIndex];
    hookRangeSize =
        (kSetDisplayModeVtableIndex - kCreateSurfaceVtableIndex + 1) *
        sizeof(*firstHookSlot);
    g_hookState.object = directDraw;
    g_hookState.vtable = originalVtable;
    g_hookState.originalCreateSurface =
        reinterpret_cast<DirectDrawCreateSurfaceFn>(
            originalVtable[kCreateSurfaceVtableIndex]);
    g_hookState.originalSetDisplayMode =
        reinterpret_cast<DirectDrawSetDisplayModeFn>(
            originalVtable[kSetDisplayModeVtableIndex]);

    if (!VirtualProtect(
            firstHookSlot,
            hookRangeSize,
            PAGE_READWRITE,
            &oldProtection))
    {
        g_hookState.object = NULL;
        g_hookState.vtable = NULL;
        g_hookState.originalCreateSurface = NULL;
        g_hookState.originalSetDisplayMode = NULL;
        LogMessage(
            "DirectDraw hook installation failed: could not update the vtable slots.\r\n",
            sizeof("DirectDraw hook installation failed: could not update the vtable slots.\r\n") - 1);
        return FALSE;
    }

    *firstHookSlot = reinterpret_cast<LPVOID>(HookedCreateSurface);
    *displayModeSlot = reinterpret_cast<LPVOID>(HookedSetDisplayMode);
    if (!VirtualProtect(
            firstHookSlot,
            hookRangeSize,
            oldProtection,
            &ignoredProtection))
    {
        LogMessage(
            "DirectDraw hooks were installed, but their original memory protection could not be restored.\r\n",
            sizeof("DirectDraw hooks were installed, but their original memory protection could not be restored.\r\n") - 1);
    }

    LogMessage(
        "Installed the in-place DirectDraw surface and display-mode hooks.\r\n",
        sizeof("Installed the in-place DirectDraw surface and display-mode hooks.\r\n") - 1);
    return TRUE;
}
