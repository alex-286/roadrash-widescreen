#include "road_rash_patch.h"
#include "hqx_upscaler.h"
#include "fps_overlay.h"
#include "worker_pool.h"

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
// Sky panorama: the draw call, its two half-surfaces, the destination, and
// the period/half-width/height globals.
static const DWORD kSkyCallRva = 0x00043AD3;
static const DWORD kSkyFunctionRva = 0x0004C800;
static const DWORD kSkyPeriodRva = 0x0009E644;
static const DWORD kSkyHalfWidthRva = 0x0009E650;
static const DWORD kSkyHeightRva = 0x000B7EC8;
static const DWORD kSkySurfaceARva = 0x00075418;
static const DWORD kSkySurfaceBRva = 0x0007541C;
static const DWORD kGameBackBufferRva = 0x00075410;
static const DWORD kLoadingImageFlagRva = 0x00075390;
static const DWORD kRenderingEnabledRva = 0x00066680;
static const DWORD kSurfaceRestoreRva = 0x0001DF60;
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
// The rest of DDSURFACEDESC v1 used for the 32-bit presenter: the lock pitch and
// pointer, and the pixel format at 0x48 (size, flags, bit count, RGB masks).
static const DWORD kDdsurfaceDescPitchOffset = 0x00000010;
static const DWORD kDdsurfaceDescSurfaceOffset = 0x00000024;
static const DWORD kDdsurfaceDescPixelFormatOffset = 0x00000048;
static const DWORD kDdPixelFormatSize = 0x00000020;
static const DWORD kDdPixelFormatFlagsOffset = 0x0000004C;
static const DWORD kDdPixelFormatRedMaskOffset = 0x00000058;
static const DWORD kDdPixelFormatGreenMaskOffset = 0x0000005C;
static const DWORD kDdPixelFormatBlueMaskOffset = 0x00000060;
static const DWORD kDdpfPaletteIndexed8 = 0x00000020;
static const DWORD kDdpfRgb = 0x00000040;
static const DWORD kDdsCapsSystemMemory = 0x00000800;
static const DWORD kDdsCapsVideoMemory = 0x00004000;
static const DWORD kDdlockWait = 0x00000001;
static const DWORD kDdlockWriteOnly = 0x00000020;
static const DWORD kGetDisplayModeVtableIndex = 12;
static const DWORD kSurfaceGetPaletteVtableIndex = 20;
static const DWORD kSurfaceLockVtableIndex = 25;
static const DWORD kSurfaceSetPaletteVtableIndex = 31;
static const DWORD kSurfaceUnlockVtableIndex = 32;
static const DWORD kPaletteGetEntriesVtableIndex = 4;
// The game's IDirectDrawPalette, created right after the gameplay surfaces.
static const DWORD kGamePaletteRva = 0x00075424;

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
// The game renders into a virtual buffer (480 lines high, width matching the
// real display's aspect ratio) that is stretched to the real display mode
// when it flips.
static DWORD kVirtualLogicalWidth = 428;
static DWORD kVirtualDisplayWidth = 856;
static DWORD g_virtualWidthOverride = 0;
static DWORD kTargetDisplayWidth = 1920;
static DWORD kTargetDisplayHeight = 1080;
static const DWORD kSafeDisplayWidth = 1920;
static const DWORD kSafeDisplayHeight = 1080;
static const DWORD kMinVirtualWidth = 640;
static const DWORD kMaxVirtualWidth = 1280;
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
static DWORD g_realDisplayWidth = 0;
static DWORD g_realDisplayHeight = 0;
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
static BOOL g_waitForVerticalBlank = TRUE;
// Frame filters. "none" keeps the game's 8-bit display mode; every other
// filter uses the 32-bit presenter, which converts the 8-bit frame through
// the game palette. F11 cycles through the 32-bit filters during a race.
enum FrameFilter
{
    kFilterNone,
    kFilterNearest,
    kFilterHqx,
    kFilterCount
};

static const CHAR* const kFilterNames[kFilterCount] =
{
    "none",
    "nearest",
    "hqx"
};

// The filter from the ini, the filter in use (kept across races), whether
// the 32-bit display mode is active, and filters that failed to start.
static DWORD g_filterSetting = kFilterNone;
static DWORD g_activeFilter = kFilterNone;
static BOOL g_presenterDisplayActive = FALSE;
static BOOL g_filterUnavailable[kFilterCount];
static DWORD g_hqxScaleSetting = 0;
static DWORD g_hqxScale = 2;
// The game palette as X8R8G8B8, refreshed every presented frame.
static DWORD g_paletteRgb[256];
// Two staging surfaces take turns, so a frame is never written into the
// surface the previous present may still be reading.
static LPVOID g_staging = NULL;
static LPVOID g_stagingSpare = NULL;
// Frame timing diagnostics: the end of the previous present.
static LARGE_INTEGER g_lastPresentEnd;
static BOOL g_lastPresentValid = FALSE;
static LPVOID g_backPalette = NULL;
static BOOL g_showFps = FALSE;
// The first log message of a run replaces the previous log.
static BOOL g_truncateLog = FALSE;

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
        g_truncateLog ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    g_truncateLog = FALSE;
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

// Reads Filter= (a filter name; anything after it, such as a ; comment, is
// ignored). Without it, the older Hqx=1 selects hqx.
static DWORD ReadFilterSetting(const CHAR* path)
{
    CHAR value[64];
    DWORD length = 0;
    DWORD start = 0;
    DWORD filter;

    GetPrivateProfileStringA(
        "Display", "Filter", "", value, sizeof(value), path);
    while (value[start] == ' ' || value[start] == '\t')
    {
        ++start;
    }

    while (value[start + length] != '\0' &&
           value[start + length] != ' ' &&
           value[start + length] != '\t' &&
           value[start + length] != ';')
    {
        ++length;
    }

    value[start + length] = '\0';
    if (length == 0)
    {
        return GetPrivateProfileIntA("Display", "Hqx", 0, path) != 0 ?
            kFilterHqx : kFilterNone;
    }

    for (filter = 0; filter < kFilterCount; ++filter)
    {
        if (lstrcmpiA(value + start, kFilterNames[filter]) == 0)
        {
            return filter;
        }
    }

    LogMessage(
        "Unknown Filter value in the ini; using none.\r\n",
        sizeof("Unknown Filter value in the ini; using none.\r\n") - 1);
    return kFilterNone;
}

static void ReadVirtualWidthSetting()
{
    CHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    static const CHAR fileName[] = "RoadRashWidescreen.ini";
    typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
    SetProcessDPIAwareFn setDpiAware;
    DWORD width;
    DWORD displayWidth;
    DWORD displayHeight;

    // Without this, the desktop size is reported DPI-virtualized on newer Windows.
    setDpiAware = reinterpret_cast<SetProcessDPIAwareFn>(GetProcAddress(
        GetModuleHandleA("user32.dll"), "SetProcessDPIAware"));
    if (setDpiAware != NULL)
    {
        setDpiAware();
    }

    displayWidth = GetSystemMetrics(SM_CXSCREEN);
    displayHeight = GetSystemMetrics(SM_CYSCREEN);

    while (length > 0 && path[length - 1] != '\\')
    {
        --length;
    }

    if (length != 0 && length + sizeof(fileName) <= MAX_PATH)
    {
        lstrcpyA(path + length, fileName);
        g_waitForVerticalBlank = GetPrivateProfileIntA(
            "Display", "WaitForVBlank", 1, path) != 0;
        g_filterSetting = ReadFilterSetting(path);
        g_activeFilter = g_filterSetting;
        g_showFps = GetPrivateProfileIntA(
            "Display", "ShowFps", 0, path) != 0;
        width = GetPrivateProfileIntA("Display", "HqxScale", 0, path);
        if (width == 2 || width == 3)
        {
            g_hqxScaleSetting = width;
        }

        width = GetPrivateProfileIntA("Display", "DisplayWidth", 0, path);
        if (width != 0)
        {
            displayWidth = width;
        }

        width = GetPrivateProfileIntA("Display", "DisplayHeight", 0, path);
        if (width != 0)
        {
            displayHeight = width;
        }

        width = GetPrivateProfileIntA("Display", "VirtualWidth", 0, path);
        width &= ~7UL;
        if (width >= kMinVirtualWidth && width <= kMaxVirtualWidth)
        {
            g_virtualWidthOverride = width;
        }
    }

    if (displayWidth < kOriginalDisplayWidth ||
        displayHeight < kOriginalDisplayHeight ||
        displayWidth > 16384 ||
        displayHeight > 16384)
    {
        displayWidth = kSafeDisplayWidth;
        displayHeight = kSafeDisplayHeight;
    }

    kTargetDisplayWidth = displayWidth;
    kTargetDisplayHeight = displayHeight;
}

// Picks the 480-line virtual width that best fits the display's aspect ratio.
static void SelectVirtualWidth(DWORD displayWidth, DWORD displayHeight)
{
    DWORD width = g_virtualWidthOverride;

    if (width == 0)
    {
        width = (kOriginalDisplayHeight * displayWidth +
            displayHeight / 2) / displayHeight;
        width = (width + 4) & ~7UL;
        if (width < kMinVirtualWidth)
        {
            width = kMinVirtualWidth;
        }
        else if (width > kMaxVirtualWidth)
        {
            width = kMaxVirtualWidth;
        }
    }

    kVirtualDisplayWidth = width;
    kVirtualLogicalWidth = width / 2;
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
    static const BYTE skyCall[] = {
        0xE8, 0x28, 0x8D, 0x00, 0x00
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
            image + kSkyCallRva,
            skyCall,
            sizeof(skyCall)) ||
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

typedef void (__fastcall *SkyDrawFn)(int, int);
typedef void (__fastcall *SurfaceRestoreFn)(LPVOID);
typedef HRESULT (WINAPI *SurfaceBltFastFn)(
    LPVOID, DWORD, DWORD, LPVOID, RECT*, DWORD);

// The game draws the sky panorama in three pieces, which leaves a gap on
// the right when the view is wider than 720 pixels. Draw the remaining
// pieces after the original routine.
static void __fastcall HookedSkyDraw(int scroll, int top)
{
    DWORD period = ReadGameDword(kSkyPeriodRva);
    DWORD half = ReadGameDword(kSkyHalfWidthRva);
    LONG height = static_cast<LONG>(ReadGameDword(kSkyHeightRva));
    LONG viewWidth = static_cast<LONG>(ReadGameDword(kDisplayWidthRva));
    LPVOID first = reinterpret_cast<LPVOID>(ReadGameDword(kSkySurfaceARva));
    LPVOID second = reinterpret_cast<LPVOID>(ReadGameDword(kSkySurfaceBRva));
    LPVOID back = reinterpret_cast<LPVOID>(ReadGameDword(kGameBackBufferRva));
    LONG x;
    LONG phase = scroll;
    DWORD index = 0;

    if (ReadGameDword(kLoadingImageFlagRva) != 0)
    {
        reinterpret_cast<SkyDrawFn>(g_gameBase + kSkyFunctionRva)(scroll, top);
        return;
    }

    if (ReadGameDword(kRenderingEnabledRva) == 0 || period == 0 ||
        half == 0 || back == NULL || first == NULL || second == NULL)
    {
        return;
    }

    reinterpret_cast<SurfaceRestoreFn>(g_gameBase + kSurfaceRestoreRva)(back);
    reinterpret_cast<SurfaceRestoreFn>(g_gameBase + kSurfaceRestoreRva)(first);
    reinterpret_cast<SurfaceRestoreFn>(g_gameBase + kSurfaceRestoreRva)(second);

    while (phase >= static_cast<LONG>(period))
    {
        phase -= period;
    }

    while (phase < 0)
    {
        phase += period;
    }

    if (phase >= static_cast<LONG>(half))
    {
        LPVOID swap = first;

        first = second;
        second = swap;
        phase -= half;
    }

    // Tile the panorama across the whole view: first the tail of the
    // previous half, then alternating halves.
    x = 0;
    while (x < viewWidth)
    {
        LONG width;
        LONG left = 0;
        LPVOID surface;
        RECT source;
        LPVOID* vtable = *reinterpret_cast<LPVOID**>(back);

        if (index == 0 && phase != 0)
        {
            width = phase;
            left = half - phase;
            surface = second;
        }
        else
        {
            width = half;
            surface = (((phase != 0 ? index : index + 1) & 1) != 0) ?
                first : second;
        }

        if (width > viewWidth - x)
        {
            width = viewWidth - x;
        }

        source.left = left;
        source.top = 0;
        source.right = left + width;
        source.bottom = height;
        reinterpret_cast<SurfaceBltFastFn>(vtable[7])(
            back,
            ReadGameDword(kDisplayOriginXRva) + x,
            ReadGameDword(kDisplayOriginYRva) + top,
            surface,
            &source,
            0x10);
        x += width;
        ++index;
    }
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

    target = enable ?
        reinterpret_cast<DWORD>(HookedSkyDraw) :
        g_gameBase + kSkyFunctionRva;
    relative = target - (g_gameBase + kSkyCallRva + 5);
    *reinterpret_cast<DWORD*>(call + 1) = relative;
    WriteGameCode(kSkyCallRva, call, sizeof(call));
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
    g_presenterDisplayActive = FALSE;
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

typedef HRESULT (WINAPI *SurfaceLockFn)(LPVOID, LPVOID, LPVOID, DWORD, HANDLE);
typedef HRESULT (WINAPI *SurfaceUnlockFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *SurfaceGetPaletteFn)(LPVOID, LPVOID*);
typedef HRESULT (WINAPI *SurfaceSetPaletteFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *PaletteGetEntriesFn)(
    LPVOID, DWORD, DWORD, DWORD, BYTE*);
typedef HRESULT (WINAPI *DirectDrawGetDisplayModeFn)(LPVOID, LPVOID);
typedef ULONG (WINAPI *ComReleaseFn)(LPVOID);

static LPVOID* GetVtable(LPVOID object)
{
    return *reinterpret_cast<LPVOID**>(object);
}

static void ReleaseObject(LPVOID object)
{
    reinterpret_cast<ComReleaseFn>(GetVtable(object)[2])(object);
}

static DWORD* DescriptionDword(BYTE* description, DWORD offset)
{
    return reinterpret_cast<DWORD*>(description + offset);
}

static void BuildOffscreenDescription(
    BYTE* description,
    DWORD width,
    DWORD height)
{
    ZeroDescription(description);
    *DescriptionDword(description, 0) = kDdsurfaceDescSize;
    *DescriptionDword(description, kDdsurfaceDescFlagsOffset) =
        kDdsdCaps | kDdsdHeight | kDdsdWidth;
    *DescriptionDword(description, kDdsurfaceDescHeightOffset) = height;
    *DescriptionDword(description, kDdsurfaceDescWidthOffset) = width;
    *DescriptionDword(description, kDdsurfaceDescCapsOffset) =
        kDdsCapsOffscreenPlain;
}

static void SetSystemMemoryCaps(BYTE* description)
{
    DWORD* caps = DescriptionDword(description, kDdsurfaceDescCapsOffset);

    *caps = (*caps | kDdsCapsSystemMemory) & ~kDdsCapsVideoMemory;
}

// While the display runs at 32 bits per pixel, every surface the game
// renders into must stay 8-bit palettized: it writes palette indices into
// them through Lock. System memory keeps them independent of the display.
static void SetIndexedSystemMemoryFormat(BYTE* description)
{
    *DescriptionDword(description, kDdsurfaceDescFlagsOffset) |=
        kDdsdPixelFormat;
    SetSystemMemoryCaps(description);
    *DescriptionDword(description, kDdsurfaceDescPixelFormatOffset) =
        kDdPixelFormatSize;
    *DescriptionDword(description, kDdPixelFormatFlagsOffset) =
        kDdpfRgb | kDdpfPaletteIndexed8;
    *DescriptionDword(description, kDdPixelFormatRgbBitCountOffset) = 8;
}

// Fills 256 PALETTEENTRY values from the game's palette object, or from the
// palette attached to the primary surface when the game has none yet.
static BOOL ReadGamePalette(BYTE* entries)
{
    LPVOID palette = reinterpret_cast<LPVOID>(ReadGameDword(kGamePaletteRva));
    LPVOID attached = NULL;
    HRESULT result;

    if (palette == NULL &&
        g_virtualPrimary != NULL &&
        SUCCEEDED(reinterpret_cast<SurfaceGetPaletteFn>(
            GetVtable(g_virtualPrimary)[kSurfaceGetPaletteVtableIndex])(
                g_virtualPrimary, &attached)))
    {
        palette = attached;
    }

    if (palette == NULL)
    {
        return FALSE;
    }

    result = reinterpret_cast<PaletteGetEntriesFn>(
        GetVtable(palette)[kPaletteGetEntriesVtableIndex])(
            palette, 0, 0, 256, entries);
    if (attached != NULL)
    {
        ReleaseObject(attached);
    }

    return SUCCEEDED(result);
}

// GDI draws the HUD text into the back buffer; on an 8-bit surface it maps
// colors through the attached palette. The game attaches its palette to the
// primary, which in 32-bit mode cannot take one, so attach it here instead.
static void AttachGamePaletteToBackBuffer()
{
    LPVOID palette = reinterpret_cast<LPVOID>(ReadGameDword(kGamePaletteRva));
    HRESULT result;

    if (palette == NULL || palette == g_backPalette || g_virtualBack == NULL)
    {
        return;
    }

    result = reinterpret_cast<SurfaceSetPaletteFn>(
        GetVtable(g_virtualBack)[kSurfaceSetPaletteVtableIndex])(
            g_virtualBack, palette);
    g_backPalette = palette;
    LogHRESULTResult(
        "Attached the game palette to the 8-bit back buffer=0x",
        sizeof("Attached the game palette to the 8-bit back buffer=0x") - 1,
        result);
}

// The 32-bit presenter needs an X8R8G8B8 display and 8-bit system-memory
// surfaces alongside it.
static BOOL ProbePresenterDisplay(LPVOID directDraw)
{
    BYTE description[kDdsurfaceDescSize];
    LPVOID surface = NULL;
    HRESULT result;

    ZeroDescription(description);
    *DescriptionDword(description, 0) = kDdsurfaceDescSize;
    result = reinterpret_cast<DirectDrawGetDisplayModeFn>(
        GetVtable(directDraw)[kGetDisplayModeVtableIndex])(
            directDraw, description);
    if (FAILED(result) ||
        *DescriptionDword(description, kDdPixelFormatRgbBitCountOffset) != 32 ||
        *DescriptionDword(description, kDdPixelFormatRedMaskOffset) != 0x00FF0000 ||
        *DescriptionDword(description, kDdPixelFormatGreenMaskOffset) != 0x0000FF00 ||
        *DescriptionDword(description, kDdPixelFormatBlueMaskOffset) != 0x000000FF)
    {
        LogMessage(
            "The 32-bit display mode is not X8R8G8B8.\r\n",
            sizeof("The 32-bit display mode is not X8R8G8B8.\r\n") - 1);
        return FALSE;
    }

    BuildOffscreenDescription(description, 8, 8);
    SetIndexedSystemMemoryFormat(description);
    result = g_hookState.originalCreateSurface(
        directDraw,
        description,
        &surface,
        NULL);
    if (FAILED(result))
    {
        LogHRESULTResult(
            "8-bit surfaces are unavailable in 32-bit mode=0x",
            sizeof("8-bit surfaces are unavailable in 32-bit mode=0x") - 1,
            result);
        return FALSE;
    }

    ReleaseObject(surface);
    return TRUE;
}

// Sets the real gameplay mode: 32 bits per pixel for the 32-bit presenter
// when a filter is requested and the mode is usable, otherwise the game's
// 8 bits per pixel.
static HRESULT SetVirtualDisplayMode(
    LPVOID directDraw,
    DWORD width,
    DWORD height,
    DWORD caller)
{
    HRESULT result;

    g_presenterDisplayActive = FALSE;
    if (g_filterSetting != kFilterNone)
    {
        result = CallOriginalSetDisplayMode(
            directDraw,
            width,
            height,
            32,
            caller);
        if (SUCCEEDED(result) && ProbePresenterDisplay(directDraw))
        {
            g_hqxScale = g_hqxScaleSetting;
            if (g_hqxScale == 0)
            {
                g_hqxScale = height >= kOriginalDisplayHeight * 3 ? 3 : 2;
            }

            g_presenterDisplayActive = TRUE;
            LogHRESULTResult(
                "32-bit presenter enabled; hqx scale=0x",
                sizeof("32-bit presenter enabled; hqx scale=0x") - 1,
                g_hqxScale);
            return result;
        }

        LogMessage(
            "32-bit presenter unavailable; using the 8-bit display mode.\r\n",
            sizeof("32-bit presenter unavailable; using the 8-bit display mode.\r\n") - 1);
    }

    return CallOriginalSetDisplayMode(directDraw, width, height, 8, caller);
}

// The size of a filter's output in the staging surface.
static void GetFilterOutputSize(DWORD filter, DWORD* width, DWORD* height)
{
    if (filter == kFilterHqx)
    {
        *width = kVirtualDisplayWidth * g_hqxScale;
        *height = kOriginalDisplayHeight * g_hqxScale;
        return;
    }

    *width = kVirtualDisplayWidth;
    *height = kOriginalDisplayHeight;
}

// The glyph scale of the FPS overlay, so it appears at the same size
// whatever the filter's output size is.
static DWORD GetFilterOverlayScale(DWORD filter)
{
    return filter == kFilterHqx ? g_hqxScale : 1;
}

static void LogFilter(const CHAR* prefix)
{
    CHAR message[96];

    lstrcpyA(message, prefix);
    lstrcatA(message, kFilterNames[g_activeFilter]);
    lstrcatA(message, "\r\n");
    LogMessage(message, lstrlenA(message));
}

// The next filter for F11: cycles through the 32-bit filters, skipping
// filters that failed to start.
static DWORD NextFilter(DWORD filter)
{
    DWORD step;

    for (step = 0; step < kFilterCount; ++step)
    {
        ++filter;
        if (filter >= kFilterCount)
        {
            filter = kFilterNearest;
        }

        if (!g_filterUnavailable[filter])
        {
            break;
        }
    }

    return filter;
}

// 32-bit system-memory surfaces that receive the filtered frame and are
// stretched onto the primary. They are large enough for every filter's
// output. The second one is optional.
static HRESULT CreatePresenterResources(LPVOID directDraw)
{
    BYTE description[kDdsurfaceDescSize];
    LPVOID staging = NULL;
    LPVOID spare = NULL;
    DWORD stagingWidth = 0;
    DWORD stagingHeight = 0;
    DWORD filter;
    HRESULT result;

    for (filter = kFilterNearest; filter < kFilterCount; ++filter)
    {
        DWORD width;
        DWORD height;

        GetFilterOutputSize(filter, &width, &height);
        if (width > stagingWidth)
        {
            stagingWidth = width;
        }

        if (height > stagingHeight)
        {
            stagingHeight = height;
        }
    }

    BuildOffscreenDescription(description, stagingWidth, stagingHeight);
    SetSystemMemoryCaps(description);
    result = g_hookState.originalCreateSurface(
        directDraw,
        description,
        &staging,
        NULL);
    if (FAILED(result))
    {
        LogHRESULTResult(
            "Staging surface creation failed=0x",
            sizeof("Staging surface creation failed=0x") - 1,
            result);
        return result;
    }

    result = g_hookState.originalCreateSurface(
        directDraw,
        description,
        &spare,
        NULL);
    if (FAILED(result))
    {
        spare = NULL;
        LogHRESULTResult(
            "Second staging surface unavailable; using one=0x",
            sizeof("Second staging surface unavailable; using one=0x") - 1,
            result);
    }

    // Filters allocate their own buffers when they are first used.
    WorkerPoolStart(8);
    LogHRESULTResult(
        "Filter threads=0x",
        sizeof("Filter threads=0x") - 1,
        WorkerPoolSize());
    if (g_activeFilter == kFilterNone || g_filterUnavailable[g_activeFilter])
    {
        g_activeFilter = kFilterNearest;
    }

    LogFilter("Filter: ");
    g_staging = staging;
    g_stagingSpare = spare;
    return S_OK;
}

static void ReleasePresenterResources()
{
    HqxUpscalerShutdown();
    WorkerPoolStop();
}

// Prepares the filter's buffers on first use. A filter that cannot start is
// skipped from then on, and the frame falls back to nearest.
static BOOL PrepareFilter(DWORD filter)
{
    if (filter == kFilterHqx && !HqxUpscalerIsReady())
    {
        if (!HqxUpscalerInitialize(
                kVirtualDisplayWidth,
                kOriginalDisplayHeight,
                g_hqxScale))
        {
            LogMessage(
                "hqx buffer allocation failed; filter disabled.\r\n",
                sizeof("hqx buffer allocation failed; filter disabled.\r\n") - 1);
            g_filterUnavailable[kFilterHqx] = TRUE;
            return FALSE;
        }
    }

    return TRUE;
}

// The nearest filter only converts the frame; the present stretches it.
static void ConvertFrame(
    const BYTE* source,
    LONG sourcePitch,
    BYTE* destination,
    LONG destinationPitch)
{
    DWORD row;
    DWORD column;

    for (row = 0; row < kOriginalDisplayHeight; ++row)
    {
        DWORD* output = reinterpret_cast<DWORD*>(destination);

        for (column = 0; column < kVirtualDisplayWidth; ++column)
        {
            output[column] = g_paletteRgb[source[column]];
        }

        source += sourcePitch;
        destination += destinationPitch;
    }
}

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
    BuildOffscreenDescription(
        backDescription,
        kVirtualDisplayWidth,
        kOriginalDisplayHeight);
    if (g_presenterDisplayActive)
    {
        SetIndexedSystemMemoryFormat(backDescription);
    }

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

    if (g_presenterDisplayActive)
    {
        result = CreatePresenterResources(directDraw);
        if (FAILED(result))
        {
            g_originalSurfaceRelease(backBuffer);
            g_originalSurfaceRelease(*surface);
            *surface = NULL;
            return result;
        }
    }

    g_backPalette = NULL;
    g_lastPresentValid = FALSE;
    g_virtualPrimary = *surface;
    g_virtualBack = backBuffer;
    LogMessage(
        "Created the real primary surface and the virtual back buffer.\r\n",
        sizeof("Created the real primary surface and the virtual back buffer.\r\n") - 1);
    return result;
}

static ULONG WINAPI HookedSurfaceRelease(LPVOID surface)
{
    ULONG count = g_originalSurfaceRelease(surface);

    if (surface == g_virtualPrimary && count == 0)
    {
        LPVOID back = g_virtualBack;
        LPVOID staging = g_staging;
        LPVOID spare = g_stagingSpare;

        g_virtualPrimary = NULL;
        g_virtualBack = NULL;
        g_staging = NULL;
        g_stagingSpare = NULL;
        g_backPalette = NULL;
        if (back != NULL)
        {
            g_originalSurfaceRelease(back);
        }

        if (spare != NULL)
        {
            g_originalSurfaceRelease(spare);
        }

        if (staging != NULL)
        {
            g_originalSurfaceRelease(staging);
            ReleasePresenterResources();
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

static BYTE g_dumpHeader[54 + 1024];

static void WriteDumpDword(BYTE* destination, DWORD value)
{
    destination[0] = static_cast<BYTE>(value);
    destination[1] = static_cast<BYTE>(value >> 8);
    destination[2] = static_cast<BYTE>(value >> 16);
    destination[3] = static_cast<BYTE>(value >> 24);
}

// Diagnostic: F12 saves the 856x480 virtual back buffer as RoadRashFrame.bmp.
static void DumpVirtualBackBuffer()
{
    LPVOID* backVtable = *reinterpret_cast<LPVOID**>(g_virtualBack);
    BYTE description[kDdsurfaceDescSize];
    BYTE palette[1024];
    HANDLE file;
    DWORD written;
    DWORD index;
    LONG row;
    BYTE* pixels;
    DWORD pitch;

    ZeroDescription(description);
    *reinterpret_cast<DWORD*>(description) = kDdsurfaceDescSize;
    {
        HRESULT lockResult = reinterpret_cast<SurfaceLockFn>(backVtable[25])(
            g_virtualBack, NULL, description, 1, NULL);

        if (FAILED(lockResult))
        {
            LogHRESULTResult(
                "Frame dump: Lock failed=0x",
                sizeof("Frame dump: Lock failed=0x") - 1,
                lockResult);
            return;
        }
    }

    pitch = *reinterpret_cast<DWORD*>(description + 16);
    pixels = *reinterpret_cast<BYTE**>(description + 0x24);
    for (index = 0; index < sizeof(palette) / sizeof(DWORD); ++index)
    {
        reinterpret_cast<volatile DWORD*>(palette)[index] = 0;
    }

    ReadGamePalette(palette);

    for (index = 0; index < sizeof(g_dumpHeader); ++index)
    {
        reinterpret_cast<volatile BYTE*>(g_dumpHeader)[index] = 0;
    }

    g_dumpHeader[0] = 'B';
    g_dumpHeader[1] = 'M';
    WriteDumpDword(g_dumpHeader + 2, sizeof(g_dumpHeader) + kVirtualDisplayWidth * 480);
    WriteDumpDword(g_dumpHeader + 10, sizeof(g_dumpHeader));
    WriteDumpDword(g_dumpHeader + 14, 40);
    WriteDumpDword(g_dumpHeader + 18, kVirtualDisplayWidth);
    WriteDumpDword(g_dumpHeader + 22, 480);
    g_dumpHeader[26] = 1;
    g_dumpHeader[28] = 8;
    WriteDumpDword(g_dumpHeader + 46, 256);
    for (index = 0; index < 256; ++index)
    {
        g_dumpHeader[54 + index * 4] = palette[index * 4 + 2];
        g_dumpHeader[54 + index * 4 + 1] = palette[index * 4 + 1];
        g_dumpHeader[54 + index * 4 + 2] = palette[index * 4];
    }

    {
        CHAR dumpPath[MAX_PATH];
        DWORD pathLength = GetModuleFileNameA(NULL, dumpPath, MAX_PATH);
        static const CHAR dumpName[] = "RoadRashFrame.bmp";

        while (pathLength > 0 && dumpPath[pathLength - 1] != '\\')
        {
            --pathLength;
        }

        if (pathLength + sizeof(dumpName) > MAX_PATH)
        {
            pathLength = 0;
        }

        lstrcpyA(dumpPath + pathLength, dumpName);
        file = CreateFileA(
            dumpPath,
            GENERIC_WRITE,
            0,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        LogMessage(
            "Frame dump: could not create RoadRashFrame.bmp.\r\n",
            sizeof("Frame dump: could not create RoadRashFrame.bmp.\r\n") - 1);
    }
    else
    {
        WriteFile(file, g_dumpHeader, sizeof(g_dumpHeader), &written, NULL);
        for (row = 479; row >= 0; --row)
        {
            WriteFile(file, pixels + row * pitch, kVirtualDisplayWidth, &written, NULL);
        }

        CloseHandle(file);
        LogMessage(
            "Saved RoadRashFrame.bmp.\r\n",
            sizeof("Saved RoadRashFrame.bmp.\r\n") - 1);
    }

    reinterpret_cast<SurfaceUnlockFn>(backVtable[32])(g_virtualBack, NULL);
}

static void WaitBeforePresent()
{
    if (g_waitForVerticalBlank && g_hookState.object != NULL)
    {
        LPVOID* directDrawVtable =
            *reinterpret_cast<LPVOID**>(g_hookState.object);

        // The result is ignored so an unsupported wait never blocks the present.
        reinterpret_cast<WaitForVerticalBlankFn>(
            directDrawVtable[kWaitForVerticalBlankVtableIndex])(
                g_hookState.object, kDdWaitVbBlockBegin, NULL);
    }
}

static void AppendDecimal(CHAR* message, DWORD* length, DWORD value)
{
    CHAR digits[10];
    DWORD count = 0;

    do
    {
        digits[count++] = static_cast<CHAR>('0' + value % 10);
        value /= 10;
    }
    while (value != 0);

    while (count > 0)
    {
        message[(*length)++] = digits[--count];
    }
}

// Points in time of one virtual present.
struct FrameTiming
{
    LARGE_INTEGER start;
    LARGE_INTEGER converted;
    LARGE_INTEGER waited;
    LARGE_INTEGER presented;
};

// 32-bit arithmetic only: 64-bit division would need the C runtime.
static DWORD ElapsedMicroseconds(LARGE_INTEGER from, LARGE_INTEGER to)
{
    static LARGE_INTEGER frequency;
    DWORD ticks;
    int microseconds;

    if (frequency.LowPart == 0 && !QueryPerformanceFrequency(&frequency))
    {
        return 0;
    }

    if (frequency.HighPart != 0 ||
        frequency.LowPart == 0 ||
        frequency.LowPart > 0x7FFFFFFF)
    {
        return 0;
    }

    ticks = to.LowPart - from.LowPart;
    microseconds = MulDiv(
        static_cast<int>(ticks & 0x7FFFFFFF),
        1000000,
        static_cast<int>(frequency.LowPart));
    return microseconds > 0 ? static_cast<DWORD>(microseconds) : 0;
}

static void AppendTimingField(
    CHAR* message,
    DWORD* length,
    const CHAR* name,
    DWORD value)
{
    lstrcpyA(message + *length, name);
    *length += lstrlenA(name);
    AppendDecimal(message, length, value);
}

// Logs, once per filter (kFilterNone is the 8-bit path), where the time of a
// frame goes, averaged over 300 frames: the game's own work between
// presents, the conversion (palette, filter, overlay), the vertical-blank
// wait, and the present blit.
static void RecordFrameTiming(DWORD slot, const FrameTiming* timing)
{
    static const DWORD kFrames = 300;
    static const DWORD kMaxGameMicroseconds = 250000;
    static DWORD frames[kFilterCount];
    static DWORD game[kFilterCount];
    static DWORD convert[kFilterCount];
    static DWORD wait[kFilterCount];
    static DWORD present[kFilterCount];
    static BOOL logged[kFilterCount];
    BOOL previousValid = g_lastPresentValid;
    LARGE_INTEGER previousEnd = g_lastPresentEnd;
    DWORD gameMicroseconds;
    CHAR message[192];
    DWORD length;
    DWORD total;

    g_lastPresentEnd = timing->presented;
    g_lastPresentValid = TRUE;
    if (!previousValid || logged[slot])
    {
        return;
    }

    // Loading screens and pauses would distort the average.
    gameMicroseconds = ElapsedMicroseconds(previousEnd, timing->start);
    if (gameMicroseconds > kMaxGameMicroseconds)
    {
        return;
    }

    game[slot] += gameMicroseconds;
    convert[slot] += ElapsedMicroseconds(timing->start, timing->converted);
    wait[slot] += ElapsedMicroseconds(timing->converted, timing->waited);
    present[slot] += ElapsedMicroseconds(timing->waited, timing->presented);
    if (++frames[slot] < kFrames)
    {
        return;
    }

    logged[slot] = TRUE;
    total = (game[slot] + convert[slot] + wait[slot] + present[slot]) / kFrames;
    lstrcpyA(message, "Frame timing, filter=");
    lstrcatA(message, slot == kFilterNone ? "none (8-bit)" : kFilterNames[slot]);
    lstrcatA(message, " (average us):");
    length = lstrlenA(message);
    AppendTimingField(message, &length, " game=", game[slot] / kFrames);
    AppendTimingField(message, &length, " convert=", convert[slot] / kFrames);
    AppendTimingField(message, &length, " vblank-wait=", wait[slot] / kFrames);
    AppendTimingField(message, &length, " present=", present[slot] / kFrames);
    AppendTimingField(message, &length, " frame=", total);
    AppendTimingField(
        message, &length, " fps=", total != 0 ? 1000000 / total : 0);
    message[length++] = '\r';
    message[length++] = '\n';
    LogMessage(message, length);
}

// Converts the 8-bit virtual frame to 32 bits through the game palette,
// applies the active filter, and stretches the result onto the primary.
static HRESULT PresentFilteredFrame(LPVOID primary, FrameTiming* timing)
{
    BYTE palette[1024];
    BYTE backDescription[kDdsurfaceDescSize];
    BYTE stagingDescription[kDdsurfaceDescSize];
    LPVOID* backVtable = GetVtable(g_virtualBack);
    LPVOID* stagingVtable = GetVtable(g_staging);
    DWORD filter = g_activeFilter;
    DWORD outputWidth;
    DWORD outputHeight;
    RECT source;
    HRESULT result;

    if (!PrepareFilter(filter))
    {
        filter = kFilterNearest;
        g_activeFilter = kFilterNearest;
    }

    if (ReadGamePalette(palette))
    {
        DWORD index;

        for (index = 0; index < 256; ++index)
        {
            g_paletteRgb[index] =
                (static_cast<DWORD>(palette[index * 4]) << 16) |
                (static_cast<DWORD>(palette[index * 4 + 1]) << 8) |
                static_cast<DWORD>(palette[index * 4 + 2]);
        }

        if (filter == kFilterHqx)
        {
            HqxUpscalerSetPalette(palette);
        }
    }

    GetFilterOutputSize(filter, &outputWidth, &outputHeight);

    AttachGamePaletteToBackBuffer();
    ZeroDescription(backDescription);
    *DescriptionDword(backDescription, 0) = kDdsurfaceDescSize;
    result = reinterpret_cast<SurfaceLockFn>(
        backVtable[kSurfaceLockVtableIndex])(
            g_virtualBack, NULL, backDescription, kDdlockWait, NULL);
    if (SUCCEEDED(result))
    {
        ZeroDescription(stagingDescription);
        *DescriptionDword(stagingDescription, 0) = kDdsurfaceDescSize;
        result = reinterpret_cast<SurfaceLockFn>(
            stagingVtable[kSurfaceLockVtableIndex])(
                g_staging,
                NULL,
                stagingDescription,
                kDdlockWait | kDdlockWriteOnly,
                NULL);
        if (SUCCEEDED(result))
        {
            const BYTE* sourcePixels = *reinterpret_cast<BYTE**>(
                backDescription + kDdsurfaceDescSurfaceOffset);
            LONG sourcePitch = *reinterpret_cast<LONG*>(
                backDescription + kDdsurfaceDescPitchOffset);
            BYTE* stagingPixels = *reinterpret_cast<BYTE**>(
                stagingDescription + kDdsurfaceDescSurfaceOffset);
            LONG stagingPitch = *reinterpret_cast<LONG*>(
                stagingDescription + kDdsurfaceDescPitchOffset);

            switch (filter)
            {
            case kFilterHqx:
                HqxUpscalerScale(
                    sourcePixels, sourcePitch, stagingPixels, stagingPitch);
                break;
            default:
                ConvertFrame(
                    sourcePixels, sourcePitch, stagingPixels, stagingPitch);
                break;
            }

            if (g_showFps)
            {
                FpsOverlayDraw(
                    stagingPixels,
                    stagingPitch,
                    outputWidth,
                    outputHeight,
                    4,
                    GetFilterOverlayScale(filter),
                    0x00FFFFFF,
                    0x00000000);
            }

            reinterpret_cast<SurfaceUnlockFn>(
                stagingVtable[kSurfaceUnlockVtableIndex])(g_staging, NULL);
        }

        reinterpret_cast<SurfaceUnlockFn>(
            backVtable[kSurfaceUnlockVtableIndex])(g_virtualBack, NULL);
    }

    if (FAILED(result) && g_flipLogCount < 8)
    {
        ++g_flipLogCount;
        LogHRESULTResult(
            "Frame conversion skipped; Lock failed=0x",
            sizeof("Frame conversion skipped; Lock failed=0x") - 1,
            result);
    }

    source.left = 0;
    source.top = 0;
    source.right = static_cast<LONG>(outputWidth);
    source.bottom = static_cast<LONG>(outputHeight);
    QueryPerformanceCounter(&timing->converted);
    WaitBeforePresent();
    QueryPerformanceCounter(&timing->waited);
    result = g_originalSurfaceBlt(
        primary,
        NULL,
        g_staging,
        &source,
        kDdbltWait,
        NULL);
    QueryPerformanceCounter(&timing->presented);
    if (g_stagingSpare != NULL)
    {
        LPVOID presented = g_staging;

        g_staging = g_stagingSpare;
        g_stagingSpare = presented;
    }

    return result;
}

// 8-bit path: draws the frame rate into the virtual back buffer, which the
// game redraws completely every frame, right before it is presented.
static void DrawFpsOnBackBuffer()
{
    BYTE palette[1024];
    BYTE description[kDdsurfaceDescSize];
    LPVOID* backVtable = GetVtable(g_virtualBack);
    DWORD white = 255;
    DWORD black = 0;

    if (ReadGamePalette(palette))
    {
        FpsOverlayPickColors(palette, &white, &black);
    }

    ZeroDescription(description);
    *DescriptionDword(description, 0) = kDdsurfaceDescSize;
    if (FAILED(reinterpret_cast<SurfaceLockFn>(
            backVtable[kSurfaceLockVtableIndex])(
                g_virtualBack, NULL, description, kDdlockWait, NULL)))
    {
        return;
    }

    FpsOverlayDraw(
        *reinterpret_cast<BYTE**>(description + kDdsurfaceDescSurfaceOffset),
        *reinterpret_cast<LONG*>(description + kDdsurfaceDescPitchOffset),
        kVirtualDisplayWidth,
        kOriginalDisplayHeight,
        1,
        1,
        white,
        black);
    reinterpret_cast<SurfaceUnlockFn>(
        backVtable[kSurfaceUnlockVtableIndex])(g_virtualBack, NULL);
}

static HRESULT WINAPI HookedSurfaceFlip(
    LPVOID surface,
    LPVOID target,
    DWORD flags)
{
    HRESULT result;
    FrameTiming timing;

    QueryPerformanceCounter(&timing.start);
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

    {
        static BOOL keyWasDown = FALSE;
        BOOL keyDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;

        if (keyDown && !keyWasDown)
        {
            LogMessage(
                "F12 pressed; dumping the virtual frame.\r\n",
                sizeof("F12 pressed; dumping the virtual frame.\r\n") - 1);
            DumpVirtualBackBuffer();
        }

        keyWasDown = keyDown;
    }

    {
        static BOOL toggleWasDown = FALSE;
        static BOOL toggleIgnoredLogged = FALSE;
        BOOL toggleDown = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

        if (toggleDown && !toggleWasDown)
        {
            if (g_staging != NULL)
            {
                g_activeFilter = NextFilter(g_activeFilter);
                LogFilter("F11 pressed; filter: ");
            }
            else if (!toggleIgnoredLogged)
            {
                toggleIgnoredLogged = TRUE;
                LogMessage(
                    "F11 ignored; the 32-bit presenter is not active (Filter=none or 32-bit mode unavailable).\r\n",
                    sizeof("F11 ignored; the 32-bit presenter is not active (Filter=none or 32-bit mode unavailable).\r\n") - 1);
            }
        }

        toggleWasDown = toggleDown;
    }

    if (g_showFps)
    {
        FpsOverlayCountFrame();
    }

    if (g_staging != NULL)
    {
        // The present may fall back to nearest, so read the filter after it.
        result = PresentFilteredFrame(surface, &timing);
        RecordFrameTiming(g_activeFilter, &timing);
        if (g_flipLogCount < 4 || (FAILED(result) && g_flipLogCount < 8))
        {
            ++g_flipLogCount;
            LogHRESULTResult(
                "32-bit present result=0x",
                sizeof("32-bit present result=0x") - 1,
                result);
        }

        return result;
    }

    if (g_showFps)
    {
        DrawFpsOnBackBuffer();
    }

    QueryPerformanceCounter(&timing.converted);
    WaitBeforePresent();
    QueryPerformanceCounter(&timing.waited);
    result = g_originalSurfaceBlt(
        surface,
        NULL,
        g_virtualBack,
        NULL,
        kDdbltWait,
        NULL);
    QueryPerformanceCounter(&timing.presented);
    RecordFrameTiming(kFilterNone, &timing);
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

    if (directDraw == g_hookState.object &&
        g_presenterDisplayActive &&
        caller - g_gameBase < kExpectedImageSize &&
        surfaceDescription != NULL)
    {
        BYTE* description = reinterpret_cast<BYTE*>(surfaceDescription);

        if (*DescriptionDword(description, 0) == kDdsurfaceDescSize &&
            (*DescriptionDword(description, kDdsurfaceDescFlagsOffset) &
                (kDdsdCaps | kDdsdPixelFormat)) == kDdsdCaps &&
            (*DescriptionDword(description, kDdsurfaceDescCapsOffset) &
                (kDdsCapsOffscreenPlain | kDdsCapsPrimarySurface)) ==
                kDdsCapsOffscreenPlain)
        {
            static DWORD indexedLogCount = 0;
            BYTE indexedDescription[kDdsurfaceDescSize];
            DWORD index;

            for (index = 0; index < kDdsurfaceDescSize; index += sizeof(DWORD))
            {
                *reinterpret_cast<volatile DWORD*>(indexedDescription + index) =
                    *DescriptionDword(description, index);
            }

            SetIndexedSystemMemoryFormat(indexedDescription);
            result = g_hookState.originalCreateSurface(
                directDraw,
                indexedDescription,
                surface,
                outer);
            if (indexedLogCount < 8)
            {
                ++indexedLogCount;
                LogHRESULTResult(
                    "Created a game surface as 8-bit system memory=0x",
                    sizeof("Created a game surface as 8-bit system memory=0x") - 1,
                    result);
            }

            if (SUCCEEDED(result) && surface != NULL)
            {
                InstallSurfaceHooks(*surface);
            }

            return result;
        }
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
        DWORD attempt;

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

        for (attempt = 0; attempt < 2; ++attempt)
        {
            DWORD targetWidth = attempt == 0 ? kTargetDisplayWidth : kSafeDisplayWidth;
            DWORD targetHeight = attempt == 0 ? kTargetDisplayHeight : kSafeDisplayHeight;

            if (attempt == 1 &&
                targetWidth == kTargetDisplayWidth &&
                targetHeight == kTargetDisplayHeight)
            {
                break;
            }

            SelectVirtualWidth(targetWidth, targetHeight);
            result = SetVirtualDisplayMode(
                directDraw,
                targetWidth,
                targetHeight,
                caller);
            if (SUCCEEDED(result))
            {
                g_realDisplayWidth = targetWidth;
                g_realDisplayHeight = targetHeight;
                SetWidescreenGameState(
                    kVirtualLogicalWidth,
                    kOriginalLogicalHeight,
                    kVirtualDisplayWidth,
                    kOriginalDisplayHeight);
                g_virtualDisplayActive = TRUE;
                SetHudCentering(TRUE);
                g_widescreenActive = TRUE;
                LogHRESULTResult(
                    "Enabled the virtual viewport; real width=0x",
                    sizeof("Enabled the virtual viewport; real width=0x") - 1,
                    targetWidth);
                LogHRESULTResult(
                    "  real height=0x",
                    sizeof("  real height=0x") - 1,
                    targetHeight);
                LogHRESULTResult(
                    "  virtual width=0x",
                    sizeof("  virtual width=0x") - 1,
                    kVirtualDisplayWidth);
                return result;
            }

            LogMessage(
                "The requested display mode was unavailable.\r\n",
                sizeof("The requested display mode was unavailable.\r\n") - 1);
        }

        LogMessage(
            "Trying the 720x480 fallback.\r\n",
            sizeof("Trying the 720x480 fallback.\r\n") - 1);
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
        g_truncateLog = TRUE;
        ReadVirtualWidthSetting();
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
