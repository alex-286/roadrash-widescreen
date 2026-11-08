#include "road_rash_patch.h"

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

typedef HRESULT (WINAPI *DirectDrawSetDisplayModeFn)(
    LPVOID,
    DWORD,
    DWORD,
    DWORD);
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
    DirectDrawSetDisplayModeFn originalSetDisplayMode;
};

static const DWORD kExpectedTimestamp = 0x321DCBE7;
static const DWORD kExpectedImageBase = 0x00400000;
static const DWORD kExpectedImageSize = 0x000D8000;
static const DWORD kGameModeSetDisplayModeReturnRva = 0x00048903;
static const DWORD kMenuSetDisplayModeReturnRva = 0x000485F1;
static const DWORD kMenuSetDisplayMode16ReturnRva = 0x00048609;
static const DWORD kEnumDisplayModesVtableIndex = 8;
static const DWORD kSetDisplayModeVtableIndex = 21;
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
static const DWORD kWidescreenLogicalWidth = 360;
static const DWORD kWidescreenDisplayWidth = 720;

static DirectDrawHookState g_hookState;
static DWORD g_gameBase = 0;
static BOOL g_widescreenActive = FALSE;
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
    BOOL hasWidescreen8BitMode;
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

    if (width == kWidescreenDisplayWidth &&
        height == kOriginalDisplayHeight &&
        bitsPerPixel == 8)
    {
        logContext->hasWidescreen8BitMode = TRUE;
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
    logContext.hasWidescreen8BitMode = FALSE;
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

        if (logContext.hasWidescreen8BitMode)
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
            sizeof(setDisplayModeCall)))
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

static void SetWidescreenGameState()
{
    WriteGameDword(kLogicalWidthARva, kWidescreenLogicalWidth);
    WriteGameDword(kLogicalWidthBRva, kWidescreenLogicalWidth);
    WriteGameDword(kDisplayOriginXRva, 0);
    WriteGameDword(kDisplayOriginYRva, 0);
    WriteGameDword(kDisplayWidthRva, kWidescreenDisplayWidth);
    WriteGameDword(kDisplayHeightRva, kOriginalDisplayHeight);
    WriteGameDword(kClipLeftRva, 0);
    WriteGameDword(kClipTopRva, 0);
    WriteGameDword(kClipRightRva, kWidescreenDisplayWidth);
    WriteGameDword(kClipBottomRva, kOriginalDisplayHeight - 1);
}

static void RestoreOriginalGameState()
{
    WriteGameDword(kLogicalWidthARva, kOriginalLogicalWidth);
    WriteGameDword(kLogicalWidthBRva, kOriginalLogicalWidth);
    WriteGameDword(kDisplayOriginXRva, 0);
    WriteGameDword(kDisplayOriginYRva, 0);
    WriteGameDword(kDisplayWidthRva, kOriginalDisplayWidth);
    WriteGameDword(kDisplayHeightRva, kOriginalDisplayHeight);
    WriteGameDword(kClipLeftRva, 0);
    WriteGameDword(kClipTopRva, 0);
    WriteGameDword(kClipRightRva, kOriginalDisplayWidth);
    WriteGameDword(kClipBottomRva, kOriginalDisplayHeight - 1);
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
            kWidescreenDisplayWidth,
            kOriginalDisplayHeight,
            bitsPerPixel,
            caller);
        if (FAILED(result))
        {
            HRESULT fallback = CallOriginalSetDisplayMode(
                directDraw,
                width,
                height,
                bitsPerPixel,
                caller);

            if (FAILED(fallback))
            {
                LogMessage(
                    "720x480 and the original 640x480 display modes both failed.\r\n",
                    sizeof("720x480 and the original 640x480 display modes both failed.\r\n") - 1);
            }
            else
            {
                LogMessage(
                    "720x480 was unavailable; retained the original 640x480 display mode.\r\n",
                    sizeof("720x480 was unavailable; retained the original 640x480 display mode.\r\n") - 1);
            }

            return fallback;
        }

        SetWidescreenGameState();
        g_widescreenActive = TRUE;
        LogMessage(
            "Enabled the experimental 720x480 (3:2) wider render viewport.\r\n",
            sizeof("Enabled the experimental 720x480 (3:2) wider render viewport.\r\n") - 1);
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
    LPVOID* displayModeSlot;
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

    displayModeSlot = &originalVtable[kSetDisplayModeVtableIndex];
    g_hookState.object = directDraw;
    g_hookState.vtable = originalVtable;
    g_hookState.originalSetDisplayMode =
        reinterpret_cast<DirectDrawSetDisplayModeFn>(
            originalVtable[kSetDisplayModeVtableIndex]);

    if (!VirtualProtect(
            displayModeSlot,
            sizeof(*displayModeSlot),
            PAGE_READWRITE,
            &oldProtection))
    {
        g_hookState.object = NULL;
        g_hookState.vtable = NULL;
        g_hookState.originalSetDisplayMode = NULL;
        LogMessage(
            "DirectDraw hook installation failed: could not update the SetDisplayMode vtable slot.\r\n",
            sizeof("DirectDraw hook installation failed: could not update the SetDisplayMode vtable slot.\r\n") - 1);
        return FALSE;
    }

    *displayModeSlot = reinterpret_cast<LPVOID>(HookedSetDisplayMode);
    if (!VirtualProtect(
            displayModeSlot,
            sizeof(*displayModeSlot),
            oldProtection,
            &ignoredProtection))
    {
        LogMessage(
            "SetDisplayMode was hooked, but its original memory protection could not be restored.\r\n",
            sizeof("SetDisplayMode was hooked, but its original memory protection could not be restored.\r\n") - 1);
    }

    LogMessage(
        "Installed the in-place DirectDraw display-mode hook.\r\n",
        sizeof("Installed the in-place DirectDraw display-mode hook.\r\n") - 1);
    return TRUE;
}
