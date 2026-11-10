#include "fps_overlay.h"

#include <intrin.h>

#pragma intrinsic(__stosb, __stosd)

static const DWORD kGlyphWidth = 5;
static const DWORD kGlyphHeight = 7;
static const DWORD kGlyphAdvance = kGlyphWidth + 1;
static const DWORD kBoxPadding = 2;
static const DWORD kMargin = 4;
static const DWORD kUpdateIntervalMs = 500;
static const DWORD kMaxTextLength = 8;
static const DWORD kMaxLineLength = 40;

struct Glyph
{
    CHAR character;
    BYTE rows[7];
};

// 5x7 glyphs; bit 4 is the leftmost column. Digits and the letters of "FPS"
// and of the filter names.
static const Glyph kGlyphs[] =
{
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
};

static CHAR g_text[kMaxTextLength + 1] = "-- FPS";
static DWORD g_frameCount = 0;
static DWORD g_intervalStart = 0;
static BOOL g_started = FALSE;

void FpsOverlayCountFrame()
{
    DWORD now = GetTickCount();
    DWORD elapsed;
    DWORD rate;
    CHAR digits[4];
    DWORD count = 0;
    DWORD length = 0;

    if (!g_started)
    {
        g_started = TRUE;
        g_intervalStart = now;
        g_frameCount = 0;
        return;
    }

    ++g_frameCount;
    elapsed = now - g_intervalStart;
    if (elapsed < kUpdateIntervalMs)
    {
        return;
    }

    rate = (g_frameCount * 1000 + elapsed / 2) / elapsed;
    if (rate > 999)
    {
        rate = 999;
    }

    do
    {
        digits[count++] = static_cast<CHAR>('0' + rate % 10);
        rate /= 10;
    }
    while (rate != 0);

    while (count > 0)
    {
        g_text[length++] = digits[--count];
    }

    g_text[length++] = ' ';
    g_text[length++] = 'F';
    g_text[length++] = 'P';
    g_text[length++] = 'S';
    g_text[length] = '\0';
    g_frameCount = 0;
    g_intervalStart = now;
}

static const BYTE* FindGlyph(CHAR character)
{
    DWORD index;

    if (character >= 'a' && character <= 'z')
    {
        character = static_cast<CHAR>(character - 'a' + 'A');
    }

    for (index = 0; index < sizeof(kGlyphs) / sizeof(kGlyphs[0]); ++index)
    {
        if (kGlyphs[index].character == character)
        {
            return kGlyphs[index].rows;
        }
    }

    return NULL;
}

static void FillPixels(
    BYTE* pixels,
    LONG pitch,
    DWORD bytesPerPixel,
    DWORD left,
    DWORD top,
    DWORD right,
    DWORD bottom,
    DWORD color)
{
    DWORD y;

    // String intrinsics: plain fill loops would compile to memset, which
    // this CRT-free DLL does not have.
    for (y = top; y < bottom; ++y)
    {
        BYTE* row = pixels + static_cast<LONG>(y) * pitch;

        if (bytesPerPixel == 4)
        {
            __stosd(
                reinterpret_cast<unsigned long*>(row) + left,
                color,
                right - left);
        }
        else
        {
            __stosb(row + left, static_cast<BYTE>(color), right - left);
        }
    }
}

void FpsOverlayDraw(
    BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD bytesPerPixel,
    DWORD scale,
    DWORD foreground,
    DWORD background,
    BOOL showRate,
    const CHAR* label)
{
    CHAR text[kMaxLineLength + 1];
    DWORD length = 0;
    DWORD boxLeft = kMargin * scale;
    DWORD boxTop = kMargin * scale;
    DWORD boxRight;
    DWORD boxBottom;
    DWORD index;

    // "60 FPS", the label, or both separated by two spaces.
    if (showRate)
    {
        for (index = 0; g_text[index] != '\0'; ++index)
        {
            text[length++] = g_text[index];
        }
    }

    if (label != NULL && label[0] != '\0')
    {
        if (length != 0)
        {
            text[length++] = ' ';
            text[length++] = ' ';
        }

        for (index = 0; label[index] != '\0' && length < kMaxLineLength; ++index)
        {
            text[length++] = label[index];
        }
    }

    text[length] = '\0';
    if (pixels == NULL || length == 0 || scale == 0 ||
        (bytesPerPixel != 1 && bytesPerPixel != 4))
    {
        return;
    }

    boxRight = boxLeft +
        (kBoxPadding * 2 + length * kGlyphAdvance - 1) * scale;
    boxBottom = boxTop + (kBoxPadding * 2 + kGlyphHeight) * scale;
    if (boxRight > width || boxBottom > height)
    {
        return;
    }

    FillPixels(
        pixels, pitch, bytesPerPixel,
        boxLeft, boxTop, boxRight, boxBottom, background);

    for (index = 0; index < length; ++index)
    {
        const BYTE* rows = FindGlyph(text[index]);
        DWORD glyphLeft = boxLeft + (kBoxPadding + index * kGlyphAdvance) * scale;
        DWORD glyphTop = boxTop + kBoxPadding * scale;
        DWORD row;
        DWORD column;

        if (rows == NULL)
        {
            continue;
        }

        for (row = 0; row < kGlyphHeight; ++row)
        {
            for (column = 0; column < kGlyphWidth; ++column)
            {
                if ((rows[row] & (0x10 >> column)) != 0)
                {
                    DWORD x = glyphLeft + column * scale;
                    DWORD y = glyphTop + row * scale;

                    FillPixels(
                        pixels, pitch, bytesPerPixel,
                        x, y, x + scale, y + scale, foreground);
                }
            }
        }
    }
}

void FpsOverlayPickColors(const BYTE* entries, DWORD* white, DWORD* black)
{
    DWORD brightest = 0;
    DWORD darkest = 0;
    DWORD maximum = 0;
    DWORD minimum = 0xFFFFFFFF;
    DWORD index;

    // The brightest and darkest entries by channel sum stand in for white
    // and black, which keeps the text readable during palette fades.
    for (index = 0; index < 256; ++index)
    {
        const BYTE* entry = entries + index * 4;
        DWORD sum = static_cast<DWORD>(entry[0]) + entry[1] + entry[2];

        if (sum > maximum)
        {
            maximum = sum;
            brightest = index;
        }

        if (sum < minimum)
        {
            minimum = sum;
            darkest = index;
        }
    }

    *white = brightest;
    *black = darkest;
}
