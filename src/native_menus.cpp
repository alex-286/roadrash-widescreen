// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "native_menus.h"
#include "road_rash_patch.h"
#include "screenshot.h"

#include <intrin.h>

#pragma intrinsic(__stosb)

// The menus are a Windows interface drawn with DirectDraw: FUN_004485D0
// sets 640x480x16, creates the primary surface (stored at 0x475404) and
// 640x480 offscreen surfaces. The main window RashRoot (0x4753DC, a topmost
// popup at 0,0, 640x480, with a null background brush, so it never paints
// itself) holds the menu buttons as invisible child windows (RashButton,
// RashInfo, placed with MoveWindow), so Windows itself finds the button
// under the mouse. Everything visible is drawn by copying rectangles of the
// offscreen picture to the same place on the primary (BltFast,
// FUN_0041FC10), with the rectangles taken from the update region of
// RashRoot or from GetWindowRect: the game assumes a 640x480 screen.
// Dialogs, the save dialog (centred by MoveWindow) and the video window
// ("AVI Window", 0,0, 640x480, played into by MCI) are top-level windows
// placed in screen coordinates.
//
// In the native display mode the menu is enlarged to the screen height
// (2.25x at 1920x1080, 3x at 2560x1440; the scale is a fraction, and
// screen pixels are mapped back to the game pixel they show) and centred.
// RashRoot covers the whole
// screen, as in the original: on
// Windows 8 and newer, what the game copies into the primary surface is
// shown only where its window is. Then:
// - its copies into the primary (Blt, BltFast) are enlarged and moved, and
//   a Lock of the primary (screenshots) sees a 640x480 copy of the menu;
// - the child windows of RashRoot and the video window are enlarged and
//   moved, both when the menus start and whenever the game places them
//   (MoveWindow, and a CBT hook for new windows); the video window then
//   becomes the largest whole multiple of the video's own size (MCI_PUT),
//   centred on black (or the Matte picture). Dialogs keep their size and
//   are centred where they would be in the enlarged menu;
// - positions the game reads come back in its 640x480 coordinates:
//   GetWindowRect, GetCursorPos, and the paint rectangle and update region
//   of RashRoot (as their bounding box); rectangles it invalidates in
//   RashRoot go the other way.
// The rest of the screen is cleared to black once per primary surface and
// then shows the menu picture blurred and darkened (see DrawBackground).

static const DWORD kImageBase = 0x00400000;
static const DWORD kRootWindowVa = 0x004753DC;
static const DWORD kMenuPrimaryVa = 0x00475404;
static const DWORD kVideoWindowVa = 0x0047E9B8;
static const DWORD kMattePictureVa = 0x004753F8;
// The video's own size, which FUN_00402650 reads with MCI_WHERE.
static const DWORD kVideoWidthVa = 0x0047E9D4;
static const DWORD kVideoHeightVa = 0x0047E9D8;

static const DWORD kGetWindowRectIatVa = 0x004CA8C4;
static const DWORD kGetCursorPosIatVa = 0x004CA858;
static const DWORD kMoveWindowIatVa = 0x004CA834;
static const DWORD kInvalidateRectIatVa = 0x004CA8CC;
static const DWORD kBeginPaintIatVa = 0x004CA840;
static const DWORD kGetUpdateRgnIatVa = 0x004CA844;
static const DWORD kMciSendCommandIatVa = 0x004CA920;

static const LONG kMenuWidth = 640;
static const LONG kMenuHeight = 480;
static const CHAR kVideoWindowClass[] = "AVI Window";

// IDirectDrawSurface vtable slots, DDSURFACEDESC and DDBLTFX fields, flags.
static const DWORD kSurfaceBltIndex = 5;
static const DWORD kSurfaceBltFastIndex = 7;
static const DWORD kSurfaceGetSurfaceDescIndex = 22;
static const DWORD kSurfaceLockIndex = 25;
static const DWORD kSurfaceUnlockIndex = 32;
static const DWORD kDescriptionSize = 0x6C;
static const DWORD kDescriptionHeight = 0x08;
static const DWORD kDescriptionWidth = 0x0C;
static const DWORD kDescriptionPitch = 0x10;
static const DWORD kDescriptionSurface = 0x24;
static const DWORD kDescriptionBitCount = 0x54;
static const DWORD kBltFxSize = 100;
static const DWORD kBltFxFillColor = 0x50;
static const DWORD kBltColorFill = 0x00000400;
static const DWORD kBltKeyDest = 0x00002000;
static const DWORD kBltKeySource = 0x00008000;
static const DWORD kBltWait = 0x01000000;
static const DWORD kBltFastSourceKey = 0x00000001;
static const DWORD kBltFastDestKey = 0x00000002;
static const DWORD kBltFastWait = 0x00000010;

// MCI_PUT with MCI_DGV_PUT_DESTINATION | MCI_DGV_RECT: the video's place in
// its window, as x, y, width, height after the callback dword.
static const UINT kMciPut = 0x0842;
static const DWORD kMciPutDestination = 0x00040000;
static const DWORD kMciRect = 0x00010000;
static const DWORD kMciPutParamsSize = 20;

typedef BOOL (WINAPI *GetWindowRectFn)(HWND, LPRECT);
typedef BOOL (WINAPI *GetCursorPosFn)(LPPOINT);
typedef BOOL (WINAPI *MoveWindowFn)(HWND, int, int, int, int, BOOL);
typedef BOOL (WINAPI *InvalidateRectFn)(HWND, const RECT*, BOOL);
typedef HDC (WINAPI *BeginPaintFn)(HWND, LPPAINTSTRUCT);
typedef int (WINAPI *GetUpdateRgnFn)(HWND, HRGN, BOOL);
typedef DWORD (WINAPI *MciSendCommandFn)(UINT, UINT, DWORD, DWORD);
typedef HRESULT (WINAPI *SurfaceBltFn)(
    LPVOID, LPRECT, LPVOID, LPRECT, DWORD, LPVOID);
typedef HRESULT (WINAPI *SurfaceBltFastFn)(
    LPVOID, DWORD, DWORD, LPVOID, LPRECT, DWORD);
typedef HRESULT (WINAPI *SurfaceGetSurfaceDescFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *SurfaceLockFn)(
    LPVOID, LPRECT, LPVOID, DWORD, HANDLE);
typedef HRESULT (WINAPI *SurfaceUnlockFn)(LPVOID, LPVOID);

static DWORD g_menusBase = 0;
static BOOL g_menusActive = FALSE;
// Menu scale = g_scaleNumerator / g_scaleDenominator.
static LONG g_scaleNumerator = 1;
static LONG g_scaleDenominator = 1;
static LONG g_offsetX = 0;
static LONG g_offsetY = 0;
static LONG g_screenWidth = 0;
static LONG g_screenHeight = 0;
static LPVOID g_clearedPrimary = NULL;
static HHOOK g_cbtHook = NULL;
static HHOOK g_keyboardHook = NULL;
static BYTE* g_lockCopy = NULL;
static BOOL g_lockCopyActive = FALSE;

static GetWindowRectFn g_originalGetWindowRect = NULL;
static GetCursorPosFn g_originalGetCursorPos = NULL;
static MoveWindowFn g_originalMoveWindow = NULL;
static InvalidateRectFn g_originalInvalidateRect = NULL;
static BeginPaintFn g_originalBeginPaint = NULL;
static GetUpdateRgnFn g_originalGetUpdateRgn = NULL;
static MciSendCommandFn g_originalMciSendCommand = NULL;

static SurfaceBltFn g_originalBlt = NULL;
static SurfaceBltFastFn g_originalBltFast = NULL;
static SurfaceLockFn g_originalLock = NULL;
static SurfaceUnlockFn g_originalUnlock = NULL;

static BOOL GetSurfaceSize(LPVOID surface, LONG* width, LONG* height);
static void InvalidateBackground();
static void ReassertImportHooks();

static void LogMenus(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

static DWORD GameVa(DWORD va)
{
    return g_menusBase - kImageBase + va;
}

static HWND RootWindow()
{
    return *reinterpret_cast<HWND*>(GameVa(kRootWindowVa));
}

static LONG FloorDivide(LONG value, LONG divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

static LONG CeilDivide(LONG value, LONG divisor)
{
    return -FloorDivide(-value, divisor);
}

// Game coordinates (640x480) and screen coordinates. A length is the
// difference of its two ends, so neighbouring rectangles stay seamless
// when the scale is not a whole number.
static LONG Scaled(LONG value)
{
    return FloorDivide(value * g_scaleNumerator, g_scaleDenominator);
}

static LONG ScreenX(LONG x) { return g_offsetX + Scaled(x); }
static LONG ScreenY(LONG y) { return g_offsetY + Scaled(y); }

// The game pixel that a screen pixel belongs to (the inverse of ScreenX
// and ScreenY, also for scales that are not whole numbers).
static LONG GamePixel(LONG screen, LONG offset)
{
    return CeilDivide(
        (screen - offset + 1) * g_scaleDenominator, g_scaleNumerator) - 1;
}

static void RectToScreen(RECT* rect)
{
    rect->left = ScreenX(rect->left);
    rect->top = ScreenY(rect->top);
    rect->right = ScreenX(rect->right);
    rect->bottom = ScreenY(rect->bottom);
}

static void RectFromScreen(RECT* rect)
{
    // Every game pixel the rectangle touches; exact for rectangles made by
    // RectToScreen.
    rect->left = GamePixel(rect->left, g_offsetX);
    rect->top = GamePixel(rect->top, g_offsetY);
    rect->right = GamePixel(rect->right - 1, g_offsetX) + 1;
    rect->bottom = GamePixel(rect->bottom - 1, g_offsetY) + 1;
}

static BOOL IsChildWindow(HWND window)
{
    return (GetWindowLongA(window, GWL_STYLE) & WS_CHILD) != 0;
}

static HWND TopLevelOf(HWND window)
{
    while (window != NULL && IsChildWindow(window))
    {
        window = GetParent(window);
    }

    return window;
}

static BOOL IsVideoClass(LPCSTR name)
{
    return name != NULL && HIWORD(reinterpret_cast<DWORD>(name)) != 0 &&
        lstrcmpiA(name, kVideoWindowClass) == 0;
}

static BOOL IsVideoWindow(HWND window)
{
    CHAR name[32];

    return window != NULL &&
        GetClassNameA(window, name, sizeof(name)) != 0 &&
        IsVideoClass(name);
}

// RashRoot's children and the video window are enlarged with the menu;
// other top-level windows (dialogs) keep their size and are centred where
// they would be in the enlarged menu.
static BOOL IsScaledTopLevel(HWND top)
{
    return top != NULL && (top == RootWindow() || IsVideoWindow(top));
}

static LONG CentredScreen(LONG position, LONG size, LONG offset)
{
    return offset + Scaled(position + size / 2) - size / 2;
}

static LONG CentredGame(LONG position, LONG size, LONG offset)
{
    return GamePixel(position + size / 2, offset) - size / 2;
}

// Clears the whole screen surface once, so the area around the menu is
// black (RashRoot paints nothing there).
static void ClearPrimaryOnce(LPVOID surface)
{
    BYTE effects[kBltFxSize];

    if (surface == g_clearedPrimary)
    {
        return;
    }

    g_clearedPrimary = surface;
    InvalidateBackground();
    __stosb(effects, 0, kBltFxSize);
    *reinterpret_cast<DWORD*>(effects) = kBltFxSize;
    *reinterpret_cast<DWORD*>(effects + kBltFxFillColor) = 0;
    g_originalBlt(
        surface, NULL, NULL, NULL, kBltColorFill | kBltWait, effects);
}

static BOOL IsMenuPrimary(LPVOID surface)
{
    if (!g_menusActive || surface == NULL ||
        surface != *reinterpret_cast<LPVOID*>(GameVa(kMenuPrimaryVa)))
    {
        return FALSE;
    }

    ClearPrimaryOnce(surface);
    return TRUE;
}

static BOOL WINAPI HookedGetWindowRect(HWND window, LPRECT rect)
{
    BOOL result;
    HWND top;

    if (g_menusActive && rect != NULL && window != NULL &&
        window == RootWindow())
    {
        SetRect(rect, 0, 0, kMenuWidth, kMenuHeight);
        return TRUE;
    }

    result = g_originalGetWindowRect(window, rect);
    if (!result || !g_menusActive || rect == NULL)
    {
        return result;
    }

    top = TopLevelOf(window);
    if (IsScaledTopLevel(top))
    {
        RectFromScreen(rect);
    }
    else if (top != NULL)
    {
        // A dialog or one of its controls: moved like the dialog.
        RECT dialog;

        if (g_originalGetWindowRect(top, &dialog))
        {
            LONG width = dialog.right - dialog.left;
            LONG height = dialog.bottom - dialog.top;
            LONG dx = dialog.left - CentredGame(dialog.left, width, g_offsetX);
            LONG dy = dialog.top - CentredGame(dialog.top, height, g_offsetY);

            OffsetRect(rect, -dx, -dy);
        }
    }

    return result;
}

static BOOL WINAPI HookedGetCursorPos(LPPOINT point)
{
    BOOL result = g_originalGetCursorPos(point);

    if (result && g_menusActive && point != NULL)
    {
        point->x = GamePixel(point->x, g_offsetX);
        point->y = GamePixel(point->y, g_offsetY);
    }

    return result;
}

static BOOL WINAPI HookedMoveWindow(
    HWND window,
    int x,
    int y,
    int width,
    int height,
    BOOL repaint)
{
    if (g_menusActive && window != NULL && window != RootWindow())
    {
        if ((IsChildWindow(window) && GetParent(window) == RootWindow()) ||
            (!IsChildWindow(window) && IsVideoWindow(window)))
        {
            RECT rect;

            SetRect(&rect, x, y, x + width, y + height);
            RectToScreen(&rect);
            x = rect.left;
            y = rect.top;
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        else if (IsChildWindow(window))
        {
            // A control in a dialog: placed relative to the dialog.
        }
        else
        {
            x = CentredScreen(x, width, g_offsetX);
            y = CentredScreen(y, height, g_offsetY);
        }
    }

    {
        BOOL result = g_originalMoveWindow(window, x, y, width, height, repaint);

        ReassertImportHooks();
        return result;
    }
}

static BOOL WINAPI HookedInvalidateRect(HWND window, const RECT* rect, BOOL erase)
{
    RECT shifted;

    if (g_menusActive && rect != NULL && window != NULL &&
        window == RootWindow())
    {
        shifted = *rect;
        RectToScreen(&shifted);
        return g_originalInvalidateRect(window, &shifted, erase);
    }

    return g_originalInvalidateRect(window, rect, erase);
}

static void ClipToMenu(RECT* rect)
{
    RECT menu;
    RECT clipped;

    SetRect(&menu, 0, 0, kMenuWidth, kMenuHeight);
    if (IntersectRect(&clipped, rect, &menu))
    {
        *rect = clipped;
    }
    else
    {
        SetRectEmpty(rect);
    }
}

static HDC WINAPI HookedBeginPaint(HWND window, LPPAINTSTRUCT paint)
{
    HDC result = g_originalBeginPaint(window, paint);

    if (result != NULL && g_menusActive && paint != NULL &&
        window == RootWindow())
    {
        RectFromScreen(&paint->rcPaint);
        ClipToMenu(&paint->rcPaint);
    }

    return result;
}

// The update region becomes its bounding box in game coordinates, so the
// game copies that rectangle (as it does for a simple region).
static int WINAPI HookedGetUpdateRgn(HWND window, HRGN region, BOOL erase)
{
    int result = g_originalGetUpdateRgn(window, region, erase);
    RECT box;

    if (result == ERROR || !g_menusActive || region == NULL ||
        window != RootWindow())
    {
        return result;
    }

    if (result == NULLREGION || GetRgnBox(region, &box) == NULLREGION)
    {
        return result;
    }

    RectFromScreen(&box);
    ClipToMenu(&box);
    if (IsRectEmpty(&box))
    {
        SetRectRgn(region, 0, 0, 0, 0);
        return NULLREGION;
    }

    SetRectRgn(region, box.left, box.top, box.right, box.bottom);
    return SIMPLEREGION;
}

// Before a video, the game fills its offscreen picture black (a full-size
// video) or loads the Matte picture into the surface at 0x4753F8 (a small
// one, at x = 160), but leaves the screen as it is, so the last menu
// showed around the video. The screen is now cleared to black (also the
// menu background around it), with the Matte picture in the menu area for
// a small video.
static void PaintVideoBackground(BOOL matte)
{
    LPVOID primary = *reinterpret_cast<LPVOID*>(GameVa(kMenuPrimaryVa));
    LPVOID picture = *reinterpret_cast<LPVOID*>(GameVa(kMattePictureVa));
    BYTE effects[kBltFxSize];
    RECT area;

    if (primary == NULL || g_originalBlt == NULL)
    {
        return;
    }

    __stosb(effects, 0, kBltFxSize);
    *reinterpret_cast<DWORD*>(effects) = kBltFxSize;
    g_originalBlt(
        primary, NULL, NULL, NULL, kBltColorFill | kBltWait, effects);
    InvalidateBackground();

    if (matte && picture != NULL)
    {
        SetRect(&area, 0, 0, kMenuWidth, kMenuHeight);
        RectToScreen(&area);
        g_originalBlt(primary, &area, picture, NULL, kBltWait, NULL);
    }
}

// The menu background: the area around the enlarged menu shows the
// current menu picture, enlarged to cover the screen, heavily blurred and
// darkened. Whenever the game copies a whole new menu screen (640x480) to
// the screen, its picture is averaged into a grid of kGridColumns x
// kGridRows colours, which is then stretched smoothly (bilinear, in
// integers) over the area around the menu. Smaller copies (a button lit
// up) leave it as it is.
static const LONG kGridColumns = 32;
static const LONG kGridRows = 24;
static const LONG kCellSize = kMenuWidth / kGridColumns;
static const LONG kBackgroundBrightness = 152;  // of 256
static const LONG kBackgroundSaturation = 333;  // of 256
static const LONG kMaxScreenWidth = 8192;
static const DWORD kLockWait = 0x00000001;
static const DWORD kDescriptionRedMask = 0x58;

struct PixelLayout
{
    DWORD bytes;
    DWORD mask[3];
    LONG shift[3];
    LONG maximum[3];
};

static BOOL g_backgroundEnabled = TRUE;
static BOOL g_backgroundValid = FALSE;
static LONG g_grid[kGridRows][kGridColumns][3];
static LONG g_drawnGrid[kGridRows][kGridColumns][3];
static LONG g_columnIndex[kMaxScreenWidth];
static LONG g_columnFraction[kMaxScreenWidth];
static DWORD g_pack[3][256];

static BOOL ReadPixelLayout(const BYTE* description, PixelLayout* layout)
{
    DWORD channel;

    layout->bytes = *reinterpret_cast<const DWORD*>(
        description + kDescriptionBitCount) / 8;
    if (layout->bytes != 2 && layout->bytes != 4)
    {
        return FALSE;
    }

    for (channel = 0; channel < 3; ++channel)
    {
        DWORD mask = *reinterpret_cast<const DWORD*>(
            description + kDescriptionRedMask + channel * 4);
        LONG shift = 0;

        if (mask == 0)
        {
            return FALSE;
        }

        while ((mask & 1) == 0)
        {
            mask >>= 1;
            ++shift;
        }

        layout->mask[channel] = mask;
        layout->shift[channel] = shift;
        layout->maximum[channel] = static_cast<LONG>(mask);
    }

    return TRUE;
}

static DWORD ReadPixel(const BYTE* pixel, DWORD bytes)
{
    return bytes == 2 ?
        *reinterpret_cast<const WORD*>(pixel) :
        *reinterpret_cast<const DWORD*>(pixel);
}

// The source picture (640x480 from sourceLeft, sourceTop) into the grid,
// already darkened, as 0..255 per channel.
static BOOL ReadBackgroundGrid(LPVOID source, LONG sourceLeft, LONG sourceTop)
{
    BYTE description[kDescriptionSize];
    PixelLayout layout;
    const BYTE* pixels;
    LONG pitch;
    LONG row;
    LONG column;

    __stosb(description, 0, kDescriptionSize);
    *reinterpret_cast<DWORD*>(description) = kDescriptionSize;
    if (FAILED(g_originalLock(source, NULL, description, kLockWait, NULL)))
    {
        return FALSE;
    }

    if (!ReadPixelLayout(description, &layout) ||
        *reinterpret_cast<LONG*>(description + kDescriptionWidth) <
            sourceLeft + kMenuWidth ||
        *reinterpret_cast<LONG*>(description + kDescriptionHeight) <
            sourceTop + kMenuHeight)
    {
        g_originalUnlock(source, NULL);
        return FALSE;
    }

    pitch = *reinterpret_cast<LONG*>(description + kDescriptionPitch);
    pixels = *reinterpret_cast<BYTE**>(description + kDescriptionSurface) +
        sourceTop * pitch + sourceLeft * static_cast<LONG>(layout.bytes);

    // Every second pixel of every second row of each cell.
    for (row = 0; row < kGridRows; ++row)
    {
        for (column = 0; column < kGridColumns; ++column)
        {
            LONG sum[3] = { 0, 0, 0 };
            LONG count = 0;
            LONG y;

            for (y = 0; y < kCellSize; y += 2)
            {
                const BYTE* line = pixels + (row * kCellSize + y) * pitch +
                    column * kCellSize * static_cast<LONG>(layout.bytes);
                LONG x;

                for (x = 0; x < kCellSize; x += 2)
                {
                    DWORD value = ReadPixel(line + x * layout.bytes, layout.bytes);
                    DWORD channel;

                    for (channel = 0; channel < 3; ++channel)
                    {
                        sum[channel] += static_cast<LONG>(
                            (value >> layout.shift[channel]) & layout.mask[channel]) *
                            255 / layout.maximum[channel];
                    }

                    ++count;
                }
            }

            {
                LONG average[3];
                LONG gray;
                DWORD channel;

                for (channel = 0; channel < 3; ++channel)
                {
                    average[channel] = sum[channel] / count;
                }

                // Stronger colors (away from the cell's gray), then darker.
                gray = (average[0] * 77 + average[1] * 150 + average[2] * 29) >> 8;
                for (channel = 0; channel < 3; ++channel)
                {
                    LONG level = gray +
                        (average[channel] - gray) * kBackgroundSaturation / 256;

                    if (level < 0)
                    {
                        level = 0;
                    }

                    if (level > 255)
                    {
                        level = 255;
                    }

                    g_grid[row][column][channel] =
                        level * kBackgroundBrightness / 256;
                }
            }
        }
    }

    g_originalUnlock(source, NULL);
    return TRUE;
}

// Grid position (index, 0..255 fraction) of a screen coordinate: the
// picture covers the screen (scale numerator / denominator), centred.
static void GridPosition(
    LONG screen,
    LONG padding,
    LONG numerator,
    LONG denominator,
    LONG cells,
    LONG* index,
    LONG* fraction)
{
    LONG position = MulDiv(screen - padding, denominator * 256, numerator) /
        kCellSize - 128;

    if (position < 0)
    {
        position = 0;
    }

    if (position >= (cells - 1) * 256)
    {
        position = (cells - 1) * 256 - 1;
    }

    *index = position >> 8;
    *fraction = position & 255;
}

static void DrawBackground(LPVOID primary)
{
    BYTE description[kDescriptionSize];
    PixelLayout layout;
    BYTE* pixels;
    LONG pitch;
    LONG width;
    LONG height;
    LONG numerator;
    LONG denominator;
    LONG paddingX;
    LONG paddingY;
    LONG menuLeft = g_offsetX;
    LONG menuRight = ScreenX(kMenuWidth);
    LONG menuTop = g_offsetY;
    LONG menuBottom = ScreenY(kMenuHeight);
    LONG y;

    __stosb(description, 0, kDescriptionSize);
    *reinterpret_cast<DWORD*>(description) = kDescriptionSize;
    if (FAILED(g_originalLock(primary, NULL, description, kLockWait, NULL)))
    {
        return;
    }

    width = *reinterpret_cast<LONG*>(description + kDescriptionWidth);
    height = *reinterpret_cast<LONG*>(description + kDescriptionHeight);
    if (!ReadPixelLayout(description, &layout) || width > kMaxScreenWidth ||
        width < menuRight || height < menuBottom)
    {
        g_originalUnlock(primary, NULL);
        return;
    }

    pitch = *reinterpret_cast<LONG*>(description + kDescriptionPitch);
    pixels = *reinterpret_cast<BYTE**>(description + kDescriptionSurface);

    {
        DWORD channel;
        LONG level;

        for (channel = 0; channel < 3; ++channel)
        {
            for (level = 0; level < 256; ++level)
            {
                g_pack[channel][level] = static_cast<DWORD>(
                    level * layout.maximum[channel] / 255) << layout.shift[channel];
            }
        }
    }

    // Cover the screen: the larger of the two scales.
    if (width * kMenuHeight >= height * kMenuWidth)
    {
        numerator = width;
        denominator = kMenuWidth;
    }
    else
    {
        numerator = height;
        denominator = kMenuHeight;
    }

    paddingX = (width - MulDiv(kMenuWidth, numerator, denominator)) / 2;
    paddingY = (height - MulDiv(kMenuHeight, numerator, denominator)) / 2;

    {
        LONG x;

        for (x = 0; x < width; ++x)
        {
            GridPosition(
                x, paddingX, numerator, denominator, kGridColumns,
                &g_columnIndex[x], &g_columnFraction[x]);
        }
    }

    for (y = 0; y < height; ++y)
    {
        BYTE* line = pixels + y * pitch;
        BOOL menuRow = y >= menuTop && y < menuBottom;
        LONG rowIndex;
        LONG fy;
        LONG x;

        GridPosition(
            y, paddingY, numerator, denominator, kGridRows, &rowIndex, &fy);

        for (x = 0; x < width; ++x)
        {
            const LONG* c00;
            const LONG* c01;
            const LONG* c10;
            const LONG* c11;
            LONG fx;
            DWORD value = 0;
            DWORD channel;

            if (menuRow && x == menuLeft)
            {
                x = menuRight - 1;
                continue;
            }

            fx = g_columnFraction[x];
            c00 = g_grid[rowIndex][g_columnIndex[x]];
            c01 = g_grid[rowIndex][g_columnIndex[x] + 1];
            c10 = g_grid[rowIndex + 1][g_columnIndex[x]];
            c11 = g_grid[rowIndex + 1][g_columnIndex[x] + 1];

            for (channel = 0; channel < 3; ++channel)
            {
                LONG top = c00[channel] * (256 - fx) + c01[channel] * fx;
                LONG bottom = c10[channel] * (256 - fx) + c11[channel] * fx;
                LONG level = (top * (256 - fy) + bottom * fy) >> 16;

                value |= g_pack[channel][level];
            }

            if (layout.bytes == 2)
            {
                *reinterpret_cast<WORD*>(line + x * 2) = static_cast<WORD>(value);
            }
            else
            {
                *reinterpret_cast<DWORD*>(line + x * 4) = value;
            }
        }
    }

    g_originalUnlock(primary, NULL);
}

// Called before the game copies to the menu's screen surface: a copy of a
// whole menu screen renews the background.
static void UpdateBackground(LPVOID primary, LPVOID source, const RECT* sourceRect)
{
    LONG left = 0;
    LONG top = 0;

    if (!g_backgroundEnabled || source == NULL)
    {
        return;
    }

    if (sourceRect != NULL)
    {
        if (sourceRect->right - sourceRect->left != kMenuWidth ||
            sourceRect->bottom - sourceRect->top != kMenuHeight)
        {
            return;
        }

        left = sourceRect->left;
        top = sourceRect->top;
    }
    else
    {
        LONG width;
        LONG height;

        if (!GetSurfaceSize(source, &width, &height) ||
            width != kMenuWidth || height != kMenuHeight)
        {
            return;
        }
    }

    if (ReadBackgroundGrid(source, left, top))
    {
        LONG* now = &g_grid[0][0][0];
        LONG* drawn = &g_drawnGrid[0][0][0];
        LONG count = kGridRows * kGridColumns * 3;
        BOOL same = g_backgroundValid;
        LONG index;

        for (index = 0; index < count && same; ++index)
        {
            same = now[index] == drawn[index];
        }

        if (same)
        {
            return;
        }

        for (index = 0; index < count; ++index)
        {
            drawn[index] = now[index];
        }

        g_backgroundValid = TRUE;
        DrawBackground(primary);
    }
}

// The screen was cleared: the next whole menu screen draws it again.
static void InvalidateBackground()
{
    g_backgroundValid = FALSE;
}

// The game plays a video into its 640x480 video window at x, y, width,
// height (FUN_00402650: 320x192 doubled to 640x384 at 0,20, or 320x192 on
// the Matte picture). The window has no background, so the rest of it is
// never painted: on Windows 98 it shows the game's picture underneath,
// on Windows 8 and newer an old video frame. The window therefore becomes
// the largest whole multiple of the video's own size that fits the screen
// (5x of 320x192 at 1920x1080, 7x at 2560x1440), centred on black, and the
// video fills it.
static DWORD WINAPI HookedMciSendCommand(
    UINT device,
    UINT message,
    DWORD flags,
    DWORD parameters)
{
    DWORD result;

    if (g_menusActive && message == kMciPut &&
        (flags & (kMciPutDestination | kMciRect)) ==
            (kMciPutDestination | kMciRect) &&
        parameters != 0)
    {
        DWORD copy[kMciPutParamsSize / sizeof(DWORD)];
        const DWORD* source = reinterpret_cast<const DWORD*>(parameters);
        HWND video = *reinterpret_cast<HWND*>(GameVa(kVideoWindowVa));
        LONG x = static_cast<LONG>(source[1]);
        LONG y = static_cast<LONG>(source[2]);
        RECT place;
        LONG width;
        LONG height;

        SetRect(
            &place, x, y,
            x + static_cast<LONG>(source[3]),
            y + static_cast<LONG>(source[4]));
        RectToScreen(&place);
        width = place.right - place.left;
        height = place.bottom - place.top;

        copy[0] = source[0];
        PaintVideoBackground(x > 0);
        if (video != NULL && IsWindow(video) &&
            source[3] != 0 && source[4] != 0)
        {
            // The largest whole multiple of the video's own size (read from
            // MCI by the game) that fits the screen.
            LONG videoWidth = *reinterpret_cast<LONG*>(GameVa(kVideoWidthVa));
            LONG videoHeight = *reinterpret_cast<LONG*>(GameVa(kVideoHeightVa));
            LONG factor;

            if (videoWidth <= 0 || videoHeight <= 0)
            {
                videoWidth = static_cast<LONG>(source[3]);
                videoHeight = static_cast<LONG>(source[4]);
            }

            factor = g_screenWidth / videoWidth;
            if (g_screenHeight / videoHeight < factor)
            {
                factor = g_screenHeight / videoHeight;
            }

            if (factor < 1)
            {
                factor = 1;
            }

            width = videoWidth * factor;
            height = videoHeight * factor;
            g_originalMoveWindow(
                video,
                (g_screenWidth - width) / 2,
                (g_screenHeight - height) / 2,
                width,
                height,
                TRUE);
            copy[1] = 0;
            copy[2] = 0;
        }
        else
        {
            copy[1] = static_cast<DWORD>(place.left - g_offsetX);
            copy[2] = static_cast<DWORD>(place.top - g_offsetY);
        }

        copy[3] = static_cast<DWORD>(width);
        copy[4] = static_cast<DWORD>(height);
        result = g_originalMciSendCommand(
            device, message, flags, reinterpret_cast<DWORD>(copy));
    }
    else
    {
        // MCI_OPEN loads the video DLLs, after which a wrapper may hook
        // the game's imports again.
        result = g_originalMciSendCommand(device, message, flags, parameters);
    }

    ReassertImportHooks();
    return result;
}

// F12 in the menus: the screen as shown (without the mouse pointer, which
// Windows draws on top) as RoadRashScreen001.bmp, 002, ... like F12 in a
// race. The screen is 16-bit, so its pixels are widened to 8 bits per
// channel first.
static void SaveMenuScreenshot()
{
    LPVOID primary = *reinterpret_cast<LPVOID*>(GameVa(kMenuPrimaryVa));
    BYTE description[kDescriptionSize];
    PixelLayout layout;
    CHAR savedName[MAX_PATH];
    CHAR message[MAX_PATH + 48];
    DWORD* frame;
    const BYTE* pixels;
    LONG pitch;
    LONG width;
    LONG height;
    LONG row;
    BOOL saved = FALSE;

    if (primary == NULL || g_originalLock == NULL || g_originalUnlock == NULL)
    {
        return;
    }

    __stosb(description, 0, kDescriptionSize);
    *reinterpret_cast<DWORD*>(description) = kDescriptionSize;
    if (FAILED(g_originalLock(primary, NULL, description, kLockWait, NULL)))
    {
        LogMenus("Menu screenshot: the screen could not be locked.");
        return;
    }

    width = *reinterpret_cast<LONG*>(description + kDescriptionWidth);
    height = *reinterpret_cast<LONG*>(description + kDescriptionHeight);
    pitch = *reinterpret_cast<LONG*>(description + kDescriptionPitch);
    pixels = *reinterpret_cast<BYTE**>(description + kDescriptionSurface);
    frame = NULL;
    if (ReadPixelLayout(description, &layout) && width > 0 && height > 0 &&
        width <= kMaxScreenWidth)
    {
        frame = reinterpret_cast<DWORD*>(VirtualAlloc(
            NULL, static_cast<DWORD>(width * height) * sizeof(DWORD),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }

    if (frame != NULL)
    {
        for (row = 0; row < height; ++row)
        {
            const BYTE* source = pixels + row * pitch;
            DWORD* target = frame + row * width;
            LONG column;

            for (column = 0; column < width; ++column)
            {
                DWORD value = ReadPixel(source + column * layout.bytes, layout.bytes);
                DWORD color = 0;
                DWORD channel;

                for (channel = 0; channel < 3; ++channel)
                {
                    DWORD level = ((value >> layout.shift[channel]) &
                        layout.mask[channel]) * 255 /
                        static_cast<DWORD>(layout.maximum[channel]);

                    color |= level << (16 - channel * 8);
                }

                target[column] = color;
            }
        }
    }

    g_originalUnlock(primary, NULL);
    if (frame == NULL)
    {
        LogMenus("Menu screenshot: unsupported screen format or not enough memory.");
        return;
    }

    saved = ScreenshotWriteBmp(
        "RoadRashScreen",
        TRUE,
        reinterpret_cast<const BYTE*>(frame),
        width * static_cast<LONG>(sizeof(DWORD)),
        static_cast<DWORD>(width),
        static_cast<DWORD>(height),
        32,
        NULL,
        savedName);
    VirtualFree(frame, 0, MEM_RELEASE);
    if (saved)
    {
        wsprintfA(message, "Menu screenshot: saved %s (%ldx%ld).", savedName, width, height);
        LogMenus(message);
    }
    else
    {
        LogMenus("Menu screenshot: the file could not be written.");
    }
}

// Bit 30 of lParam: the key was already down (auto-repeat); bit 31: it is
// being released.
static LRESULT CALLBACK MenuKeyboardHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_menusActive && wParam == VK_F12 &&
        (lParam & 0xC0000000) == 0)
    {
        SaveMenuScreenshot();
    }

    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

// New windows: RashRoot's children and the video window are enlarged with
// the menu; other top-level windows that fit into the 640x480 screen the
// game was made for (its dialogs; not message boxes, which Windows centres
// on the real screen) are centred in it.
static LRESULT CALLBACK MenuCbtHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_CREATEWND && g_menusActive)
    {
        CBT_CREATEWNDA* create = reinterpret_cast<CBT_CREATEWNDA*>(lParam);
        CREATESTRUCTA* window = create != NULL ? create->lpcs : NULL;

        if (window != NULL &&
            window->x != static_cast<int>(CW_USEDEFAULT) &&
            window->y != static_cast<int>(CW_USEDEFAULT))
        {
            BOOL child = (window->style & WS_CHILD) != 0;
            BOOL fits = window->x >= 0 && window->y >= 0 &&
                window->x + window->cx <= kMenuWidth &&
                window->y + window->cy <= kMenuHeight;

            if ((child && window->hwndParent != NULL &&
                    window->hwndParent == RootWindow()) ||
                (!child && fits && IsVideoClass(window->lpszClass)))
            {
                RECT rect;

                SetRect(
                    &rect, window->x, window->y,
                    window->x + window->cx, window->y + window->cy);
                RectToScreen(&rect);
                window->x = rect.left;
                window->y = rect.top;
                window->cx = rect.right - rect.left;
                window->cy = rect.bottom - rect.top;
            }
            else if (!child && fits)
            {
                window->x = CentredScreen(window->x, window->cx, g_offsetX);
                window->y = CentredScreen(window->y, window->cy, g_offsetY);
            }
        }
    }

    return CallNextHookEx(g_cbtHook, code, wParam, lParam);
}

static HRESULT WINAPI HookedBlt(
    LPVOID surface,
    LPRECT destination,
    LPVOID source,
    LPRECT sourceRect,
    DWORD flags,
    LPVOID effects)
{
    RECT shifted;

    if (!IsMenuPrimary(surface))
    {
        return g_originalBlt(
            surface, destination, source, sourceRect, flags, effects);
    }

    if (destination == NULL)
    {
        SetRect(&shifted, 0, 0, kMenuWidth, kMenuHeight);
    }
    else
    {
        shifted = *destination;
    }

    UpdateBackground(surface, source, sourceRect);
    RectToScreen(&shifted);
    return g_originalBlt(surface, &shifted, source, sourceRect, flags, effects);
}

static BOOL GetSurfaceSize(LPVOID surface, LONG* width, LONG* height)
{
    BYTE description[kDescriptionSize];
    LPVOID* vtable = *reinterpret_cast<LPVOID**>(surface);

    __stosb(description, 0, kDescriptionSize);
    *reinterpret_cast<DWORD*>(description) = kDescriptionSize;
    if (FAILED(reinterpret_cast<SurfaceGetSurfaceDescFn>(
            vtable[kSurfaceGetSurfaceDescIndex])(surface, description)))
    {
        return FALSE;
    }

    *width = *reinterpret_cast<LONG*>(description + kDescriptionWidth);
    *height = *reinterpret_cast<LONG*>(description + kDescriptionHeight);
    return TRUE;
}

static HRESULT WINAPI HookedBltFast(
    LPVOID surface,
    DWORD x,
    DWORD y,
    LPVOID source,
    LPRECT sourceRect,
    DWORD flags)
{
    RECT from;
    RECT to;
    DWORD bltFlags = 0;

    ReassertImportHooks();
    if (!IsMenuPrimary(surface))
    {
        return g_originalBltFast(surface, x, y, source, sourceRect, flags);
    }

    UpdateBackground(surface, source, sourceRect);
    if (g_scaleNumerator == g_scaleDenominator)
    {
        return g_originalBltFast(
            surface, ScreenX(x), ScreenY(y), source, sourceRect, flags);
    }

    // Enlarged: a stretching Blt of the same rectangle.
    if (sourceRect != NULL)
    {
        from = *sourceRect;
    }
    else
    {
        LONG width;
        LONG height;

        if (source == NULL || !GetSurfaceSize(source, &width, &height))
        {
            return E_FAIL;
        }

        SetRect(&from, 0, 0, width, height);
    }

    SetRect(
        &to,
        static_cast<LONG>(x),
        static_cast<LONG>(y),
        static_cast<LONG>(x) + from.right - from.left,
        static_cast<LONG>(y) + from.bottom - from.top);
    RectToScreen(&to);

    if (flags & kBltFastSourceKey)
    {
        bltFlags |= kBltKeySource;
    }

    if (flags & kBltFastDestKey)
    {
        bltFlags |= kBltKeyDest;
    }

    if (flags & kBltFastWait)
    {
        bltFlags |= kBltWait;
    }

    return g_originalBlt(surface, &to, source, &from, bltFlags, NULL);
}

// A Lock of the whole primary (the game's screenshot) gets the menu as the
// game drew it: the 640x480 area, one screen pixel per game pixel.
static HRESULT WINAPI HookedLock(
    LPVOID surface,
    LPRECT rect,
    LPVOID description,
    DWORD flags,
    HANDLE event)
{
    RECT shifted;
    HRESULT result;
    BYTE* fields;
    LONG pitch;
    DWORD bytesPerPixel;
    BYTE* pixels;

    if (!IsMenuPrimary(surface))
    {
        return g_originalLock(surface, rect, description, flags, event);
    }

    if (rect != NULL)
    {
        shifted = *rect;
        RectToScreen(&shifted);
        return g_originalLock(surface, &shifted, description, flags, event);
    }

    result = g_originalLock(surface, NULL, description, flags, event);
    if (FAILED(result) || description == NULL)
    {
        return result;
    }

    fields = reinterpret_cast<BYTE*>(description);
    pitch = *reinterpret_cast<LONG*>(fields + kDescriptionPitch);
    bytesPerPixel = *reinterpret_cast<DWORD*>(fields + kDescriptionBitCount) / 8;
    pixels = *reinterpret_cast<BYTE**>(fields + kDescriptionSurface) +
        g_offsetY * pitch + g_offsetX * bytesPerPixel;

    if (g_scaleNumerator != g_scaleDenominator && g_lockCopy != NULL &&
        bytesPerPixel >= 1 &&
        bytesPerPixel <= 4)
    {
        LONG rowBytes = kMenuWidth * static_cast<LONG>(bytesPerPixel);
        LONG row;

        for (row = 0; row < kMenuHeight; ++row)
        {
            const BYTE* from = pixels + Scaled(row) * pitch;
            BYTE* to = g_lockCopy + row * rowBytes;
            LONG column;

            for (column = 0; column < kMenuWidth; ++column)
            {
                DWORD byteIndex;

                for (byteIndex = 0; byteIndex < bytesPerPixel; ++byteIndex)
                {
                    to[column * bytesPerPixel + byteIndex] =
                        from[Scaled(column) * bytesPerPixel + byteIndex];
                }
            }
        }

        g_originalUnlock(surface, NULL);
        g_lockCopyActive = TRUE;
        pixels = g_lockCopy;
        pitch = rowBytes;
    }

    *reinterpret_cast<BYTE**>(fields + kDescriptionSurface) = pixels;
    *reinterpret_cast<LONG*>(fields + kDescriptionPitch) = pitch;
    *reinterpret_cast<DWORD*>(fields + kDescriptionWidth) = kMenuWidth;
    *reinterpret_cast<DWORD*>(fields + kDescriptionHeight) = kMenuHeight;
    return result;
}

static HRESULT WINAPI HookedUnlock(LPVOID surface, LPVOID data)
{
    if (g_lockCopyActive && IsMenuPrimary(surface))
    {
        g_lockCopyActive = FALSE;
        return S_OK;
    }

    return g_originalUnlock(surface, data);
}

static BOOL PatchPointer(LPVOID* slot, LPVOID value, LPVOID* original)
{
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (!VirtualProtect(slot, sizeof(LPVOID), PAGE_READWRITE, &oldProtection))
    {
        return FALSE;
    }

    if (original != NULL)
    {
        *original = *slot;
    }

    *slot = value;
    VirtualProtect(slot, sizeof(LPVOID), oldProtection, &ignoredProtection);
    return TRUE;
}

struct ImportHook
{
    DWORD iatVa;
    const CHAR* module;
    const CHAR* name;
    LPVOID hook;
    LPVOID* original;
};

static const ImportHook g_importHooks[] =
{
    { kGetWindowRectIatVa, "user32.dll", "GetWindowRect",
      reinterpret_cast<LPVOID>(HookedGetWindowRect),
      reinterpret_cast<LPVOID*>(&g_originalGetWindowRect) },
    { kGetCursorPosIatVa, "user32.dll", "GetCursorPos",
      reinterpret_cast<LPVOID>(HookedGetCursorPos),
      reinterpret_cast<LPVOID*>(&g_originalGetCursorPos) },
    { kMoveWindowIatVa, "user32.dll", "MoveWindow",
      reinterpret_cast<LPVOID>(HookedMoveWindow),
      reinterpret_cast<LPVOID*>(&g_originalMoveWindow) },
    { kInvalidateRectIatVa, "user32.dll", "InvalidateRect",
      reinterpret_cast<LPVOID>(HookedInvalidateRect),
      reinterpret_cast<LPVOID*>(&g_originalInvalidateRect) },
    { kBeginPaintIatVa, "user32.dll", "BeginPaint",
      reinterpret_cast<LPVOID>(HookedBeginPaint),
      reinterpret_cast<LPVOID*>(&g_originalBeginPaint) },
    { kGetUpdateRgnIatVa, "user32.dll", "GetUpdateRgn",
      reinterpret_cast<LPVOID>(HookedGetUpdateRgn),
      reinterpret_cast<LPVOID*>(&g_originalGetUpdateRgn) },
    { kMciSendCommandIatVa, "winmm.dll", "mciSendCommandA",
      reinterpret_cast<LPVOID>(HookedMciSendCommand),
      reinterpret_cast<LPVOID*>(&g_originalMciSendCommand) }
};

static const DWORD kImportHookCount =
    sizeof(g_importHooks) / sizeof(g_importHooks[0]);
static BOOL g_importHooksInstalled = FALSE;

// Whether the game's import table names `name` from `module` for the import
// slot at `iatVa`, read from the name list the linker kept beside the slots
// (the executable has one for every DLL). A wrapper loaded by ChainDll may
// have written its own function into the slot already (dd2d9 does as it
// loads); the name still says which function the slot is for.
static BOOL ImportSlotNames(DWORD iatVa, const CHAR* module, const CHAR* name)
{
    const BYTE* base = reinterpret_cast<const BYTE*>(GameVa(kImageBase));
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt;
    const IMAGE_DATA_DIRECTORY* directory;
    const IMAGE_IMPORT_DESCRIPTOR* descriptor;
    DWORD slotRva = iatVa - kImageBase;
    DWORD descriptorIndex;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return FALSE;
    }

    nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return FALSE;
    }

    directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory->VirtualAddress == 0)
    {
        return FALSE;
    }

    descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory->VirtualAddress);

    // (The game imports from 13 DLLs; the limit only ends a broken table.)
    for (descriptorIndex = 0;
         descriptorIndex < 64 && descriptor->Name != 0;
         ++descriptorIndex, ++descriptor)
    {
        const IMAGE_THUNK_DATA* names;
        DWORD slotIndex;
        DWORD index;

        if (descriptor->OriginalFirstThunk == 0 ||
            slotRva < descriptor->FirstThunk ||
            (slotRva - descriptor->FirstThunk) % sizeof(DWORD) != 0 ||
            lstrcmpiA(reinterpret_cast<const CHAR*>(base + descriptor->Name),
                module) != 0)
        {
            continue;
        }

        slotIndex = (slotRva - descriptor->FirstThunk) / sizeof(DWORD);
        names = reinterpret_cast<const IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);

        // The list ends with a zero entry; the slot must come before it.
        for (index = 0; index < slotIndex; ++index)
        {
            if (names[index].u1.AddressOfData == 0)
            {
                return FALSE;
            }
        }

        if (names[slotIndex].u1.AddressOfData == 0 ||
            IMAGE_SNAP_BY_ORDINAL(names[slotIndex].u1.Ordinal))
        {
            return FALSE;
        }

        return lstrcmpA(reinterpret_cast<const CHAR*>(
            reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + names[slotIndex].u1.AddressOfData)->Name),
            name) == 0;
    }

    return FALSE;
}

// A DirectDraw wrapper behind ChainDll can write its own functions into the
// same import slots after the hooks are in, again whenever a DLL is loaded
// (cnc-ddraw does, also when the video DLLs load). The hook then goes back
// in front, and the wrapper's function becomes the one it calls, so both
// keep working. Without such a wrapper the slots never change. Called after
// the hooks pass a call on, for every new surface and for the menu copies.
static void ReassertImportHooks()
{
    static BOOL logged = FALSE;
    DWORD index;

    if (!g_importHooksInstalled)
    {
        return;
    }

    for (index = 0; index < kImportHookCount; ++index)
    {
        const ImportHook* hook = &g_importHooks[index];
        LPVOID* slot = reinterpret_cast<LPVOID*>(GameVa(hook->iatVa));
        LPVOID current = *slot;

        if (current == hook->hook || current == NULL)
        {
            continue;
        }

        if (PatchPointer(slot, hook->hook, hook->original) && !logged)
        {
            logged = TRUE;
            LogMenus("Native menus: another DLL replaced the game's window imports; the menu hooks now run in front of it.");
        }
    }
}

BOOL NativeMenusInstall(DWORD gameBase, BOOL background)
{
    const ImportHook* hooks = g_importHooks;
    const DWORD hookCount = kImportHookCount;
    BOOL replaced = FALSE;
    DWORD index;

    if (g_importHooksInstalled)
    {
        return TRUE;
    }

    g_menusBase = gameBase;
    g_backgroundEnabled = background;

    // Check every import first, so nothing changes on a mismatch. A slot
    // that holds another DLL's function counts when the import table names
    // the expected one for it: the hook then goes in front of that function
    // and passes its calls on to it, as ReassertImportHooks does later.
    for (index = 0; index < hookCount; ++index)
    {
        LPVOID* slot = reinterpret_cast<LPVOID*>(GameVa(hooks[index].iatVa));
        HMODULE module = GetModuleHandleA(hooks[index].module);
        FARPROC expected = module != NULL ?
            GetProcAddress(module, hooks[index].name) : NULL;

        if (expected != NULL && *slot == reinterpret_cast<LPVOID>(expected))
        {
            continue;
        }

        if (expected == NULL || *slot == NULL ||
            !ImportSlotNames(hooks[index].iatVa, hooks[index].module,
                hooks[index].name))
        {
            LogMenus("Native menus stay off: the game's window imports are not as expected.");
            return FALSE;
        }

        replaced = TRUE;
    }

    g_lockCopy = reinterpret_cast<BYTE*>(VirtualAlloc(
        NULL, kMenuWidth * kMenuHeight * 4, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));

    for (index = 0; index < hookCount; ++index)
    {
        PatchPointer(
            reinterpret_cast<LPVOID*>(GameVa(hooks[index].iatVa)),
            hooks[index].hook,
            hooks[index].original);
    }

    g_importHooksInstalled = TRUE;
    if (replaced)
    {
        LogMenus("Native menus: another DLL had replaced the game's window imports; the menu hooks run in front of it.");
    }
    LogMenus("Native menus: the menus are shown enlarged in the middle of the native display mode.");
    return TRUE;
}

// Puts the hook into a vtable slot unless it is there already. Windows 8
// and newer rebuild the surface vtable when the display mode changes (as
// the Flip hook in road_rash_patch.cpp notices too), so every new surface
// is checked again.
// Every vtable slot changed, with the value it had, for NativeMenusShutdown.
struct PatchedSlot
{
    LPVOID* slot;
    LPVOID hook;
    LPVOID original;
};

static const DWORD kMaxPatchedSlots = 32;
static PatchedSlot g_patchedSlots[kMaxPatchedSlots];
static DWORD g_patchedSlotCount = 0;

static void EnsureSlot(LPVOID* vtable, DWORD index, LPVOID hook, LPVOID* original)
{
    LPVOID current = vtable[index];

    if (current == hook || current == NULL)
    {
        return;
    }

    if (PatchPointer(&vtable[index], hook, original) &&
        g_patchedSlotCount < kMaxPatchedSlots)
    {
        g_patchedSlots[g_patchedSlotCount].slot = &vtable[index];
        g_patchedSlots[g_patchedSlotCount].hook = hook;
        g_patchedSlots[g_patchedSlotCount].original = current;
        ++g_patchedSlotCount;
    }
}

// When the game ends. On Windows 95/98/ME, DDRAW.DLL is shared by every
// process, and so is its surface vtable: left pointing into this DLL, it
// would lead into freed memory once the game is gone (DirectDraw's own
// cleanup after the process, or the next DirectDraw program). The slots get
// their original values back, unless something else changed them since.
// On Windows NT every process has its own copy, which may already be gone,
// so nothing is touched there. The CBT hook is removed in both cases.
void NativeMenusShutdown(BOOL sharedSystem)
{
    DWORD index;

    if (g_cbtHook != NULL)
    {
        UnhookWindowsHookEx(g_cbtHook);
        g_cbtHook = NULL;
    }

    if (g_keyboardHook != NULL)
    {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = NULL;
    }

    g_menusActive = FALSE;
    if (!sharedSystem)
    {
        return;
    }

    for (index = 0; index < g_patchedSlotCount; ++index)
    {
        PatchedSlot* patched = &g_patchedSlots[index];

        if (*patched->slot == patched->hook)
        {
            PatchPointer(patched->slot, patched->original, NULL);
        }
    }

    g_patchedSlotCount = 0;
}

void NativeMenusWatchSurface(LPVOID surface)
{
    LPVOID* vtable;

    if (g_menusBase == 0 || surface == NULL)
    {
        return;
    }

    ReassertImportHooks();
    vtable = *reinterpret_cast<LPVOID**>(surface);
    if (vtable == NULL)
    {
        return;
    }

    EnsureSlot(
        vtable, kSurfaceBltIndex,
        reinterpret_cast<LPVOID>(HookedBlt),
        reinterpret_cast<LPVOID*>(&g_originalBlt));
    EnsureSlot(
        vtable, kSurfaceBltFastIndex,
        reinterpret_cast<LPVOID>(HookedBltFast),
        reinterpret_cast<LPVOID*>(&g_originalBltFast));
    EnsureSlot(
        vtable, kSurfaceLockIndex,
        reinterpret_cast<LPVOID>(HookedLock),
        reinterpret_cast<LPVOID*>(&g_originalLock));
    EnsureSlot(
        vtable, kSurfaceUnlockIndex,
        reinterpret_cast<LPVOID>(HookedUnlock),
        reinterpret_cast<LPVOID*>(&g_originalUnlock));
}

// direction 1: from the 640x480 layout into the enlarged menu; -1: back.
static BOOL CALLBACK PlaceChild(HWND window, LPARAM direction)
{
    RECT rect;
    LONG x;
    LONG y;
    LONG width;
    LONG height;

    if (!IsChildWindow(window) || GetParent(window) != RootWindow() ||
        !g_originalGetWindowRect(window, &rect))
    {
        return TRUE;
    }

    // RashRoot is at 0,0, so screen and client coordinates are the same.
    if (direction > 0)
    {
        RectToScreen(&rect);
    }
    else
    {
        RectFromScreen(&rect);
    }

    x = rect.left;
    y = rect.top;
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    SetWindowPos(
        window, NULL, x, y, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    return TRUE;
}

void NativeMenusEnter(DWORD width, DWORD height)
{
    HWND root;

    if (g_originalMoveWindow == NULL ||
        width < static_cast<DWORD>(kMenuWidth) ||
        height < static_cast<DWORD>(kMenuHeight))
    {
        NativeMenusLeave();
        return;
    }

    root = RootWindow();
    if (root == NULL)
    {
        return;
    }

    NativeMenusLeave();

    // As large as the screen allows: the full height on wide screens.
    if (static_cast<LONG>(width) * kMenuHeight >=
        static_cast<LONG>(height) * kMenuWidth)
    {
        g_scaleNumerator = static_cast<LONG>(height);
        g_scaleDenominator = kMenuHeight;
    }
    else
    {
        g_scaleNumerator = static_cast<LONG>(width);
        g_scaleDenominator = kMenuWidth;
    }

    g_screenWidth = static_cast<LONG>(width);
    g_screenHeight = static_cast<LONG>(height);
    g_offsetX = (static_cast<LONG>(width) -
        FloorDivide(kMenuWidth * g_scaleNumerator, g_scaleDenominator)) / 2;
    g_offsetY = (static_cast<LONG>(height) -
        FloorDivide(kMenuHeight * g_scaleNumerator, g_scaleDenominator)) / 2;
    g_clearedPrimary = NULL;
    g_lockCopyActive = FALSE;
    InvalidateBackground();

    if (g_cbtHook == NULL)
    {
        g_cbtHook = SetWindowsHookExA(
            WH_CBT, MenuCbtHook, NULL,
            GetWindowThreadProcessId(root, NULL));
    }

    if (g_keyboardHook == NULL)
    {
        g_keyboardHook = SetWindowsHookExA(
            WH_KEYBOARD, MenuKeyboardHook, NULL,
            GetWindowThreadProcessId(root, NULL));
    }

    SetWindowPos(
        root, NULL, 0, 0,
        static_cast<int>(width), static_cast<int>(height),
        SWP_NOZORDER | SWP_NOACTIVATE);
    EnumChildWindows(root, PlaceChild, 1);
    g_menusActive = TRUE;
    g_originalInvalidateRect(root, NULL, FALSE);

    {
        CHAR message[112];

        wsprintfA(
            message,
            "Native menus: %lux%lu, the menu enlarged to %ldx%ld at %ld,%ld.",
            width,
            height,
            Scaled(kMenuWidth),
            Scaled(kMenuHeight),
            g_offsetX,
            g_offsetY);
        LogMenus(message);
    }
}

void NativeMenusLeave()
{
    HWND root;

    if (!g_menusActive)
    {
        return;
    }

    g_menusActive = FALSE;
    g_lockCopyActive = FALSE;
    root = RootWindow();
    if (root != NULL)
    {
        EnumChildWindows(root, PlaceChild, -1);
        SetWindowPos(
            root, NULL, 0, 0, kMenuWidth, kMenuHeight,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

BOOL NativeMenusActive()
{
    return g_menusActive;
}
