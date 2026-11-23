/**
 * Screenshots of locked surfaces as BMP files in the game folder.
 *
 * @file screenshot.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "screenshot.h"

#include <intrin.h>

#pragma intrinsic(__movsb)

/** The size of BITMAPFILEHEADER in bytes. */
static const DWORD kFileHeaderSize = 14;
/** The size of BITMAPINFOHEADER in bytes. */
static const DWORD kInfoHeaderSize = 40;
/** The size of the 256-color table of an 8-bit BMP in bytes. */
static const DWORD kPaletteSize = 256 * 4;
/** The highest number of a numbered screenshot. */
static const DWORD kMaxNumber = 999;

/**
 * Stores a DWORD in little-endian byte order.
 *
 * @param [out] destination The four bytes to write.
 * @param [in] value The value to store.
 */
static void PutDword(BYTE* destination, DWORD value)
{
    destination[0] = static_cast<BYTE>(value);
    destination[1] = static_cast<BYTE>(value >> 8);
    destination[2] = static_cast<BYTE>(value >> 16);
    destination[3] = static_cast<BYTE>(value >> 24);
}

/**
 * Gives the folder of the game executable, with a trailing backslash.
 *
 * @param [out] path Receives the folder (MAX_PATH characters).
 * @return The length of the folder, or 0 when it is not known.
 */
static DWORD GetGameFolder(CHAR* path)
{
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);

    if (length == 0 || length >= MAX_PATH)
    {
        return 0;
    }

    while (length > 0 && path[length - 1] != '\\' && path[length - 1] != '/')
    {
        --length;
    }

    path[length] = '\0';
    return length;
}

/**
 * Creates the BMP file in the game folder.
 *
 * @param [in] name The file name, or with numbered the prefix.
 * @param [in] numbered TRUE to use the first free name from prefix001.bmp to
 *             prefix999.bmp; FALSE to replace the file name.
 * @param [out] savedName Receives the file name that is created.
 * @return The file, open for writing, or INVALID_HANDLE_VALUE.
 */
static HANDLE CreateBmpFile(const CHAR* name, BOOL numbered, CHAR* savedName)
{
    CHAR path[MAX_PATH];
    DWORD folderLength = GetGameFolder(path);
    DWORD number;

    // A prefix, three digits and ".bmp" must fit behind the folder.
    if (lstrlenA(name) + folderLength + 8 >= MAX_PATH)
    {
        return INVALID_HANDLE_VALUE;
    }

    if (!numbered)
    {
        lstrcpyA(savedName, name);
        lstrcpyA(path + folderLength, name);
        return CreateFileA(
            path, GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    for (number = 1; number <= kMaxNumber; ++number)
    {
        HANDLE file;

        wsprintfA(savedName, "%s%03u.bmp", name, number);
        lstrcpyA(path + folderLength, savedName);
        file = CreateFileA(
            path, GENERIC_WRITE, 0, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS)
        {
            return file;
        }
    }

    return INVALID_HANDLE_VALUE;
}

BOOL ScreenshotWriteBmp(
    const CHAR* name,
    BOOL numbered,
    const BYTE* pixels,
    LONG pitch,
    DWORD width,
    DWORD height,
    DWORD bitsPerPixel,
    const BYTE* palette,
    CHAR* savedName)
{
    BYTE header[kFileHeaderSize + kInfoHeaderSize + kPaletteSize];
    DWORD headerSize;
    DWORD stride;
    BYTE* row;
    HANDLE file;
    DWORD written;
    DWORD index;
    LONG line;
    BOOL ok = TRUE;

    savedName[0] = '\0';
    if (pixels == NULL || width == 0 || height == 0 ||
        (bitsPerPixel != 8 && bitsPerPixel != 32) ||
        (bitsPerPixel == 8 && palette == NULL))
    {
        return FALSE;
    }

    // 8-bit stays palettized; 32-bit is stored as 24-bit. Rows are padded
    // to four bytes and stored bottom-up.
    headerSize = kFileHeaderSize + kInfoHeaderSize +
        (bitsPerPixel == 8 ? kPaletteSize : 0);
    stride = bitsPerPixel == 8 ? (width + 3) & ~3UL : (width * 3 + 3) & ~3UL;
    for (index = 0; index < sizeof(header); ++index)
    {
        reinterpret_cast<volatile BYTE*>(header)[index] = 0;
    }

    header[0] = 'B';
    header[1] = 'M';
    PutDword(header + 2, headerSize + stride * height);
    PutDword(header + 10, headerSize);
    PutDword(header + 14, kInfoHeaderSize);
    PutDword(header + 18, width);
    PutDword(header + 22, height);
    header[26] = 1;
    header[28] = static_cast<BYTE>(bitsPerPixel == 8 ? 8 : 24);
    PutDword(header + 34, stride * height);
    if (bitsPerPixel == 8)
    {
        PutDword(header + 46, 256);
        for (index = 0; index < 256; ++index)
        {
            BYTE* entry = header + kFileHeaderSize + kInfoHeaderSize + index * 4;

            entry[0] = palette[index * 4 + 2];
            entry[1] = palette[index * 4 + 1];
            entry[2] = palette[index * 4];
        }
    }

    row = reinterpret_cast<BYTE*>(VirtualAlloc(
        NULL, stride, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (row == NULL)
    {
        return FALSE;
    }

    file = CreateBmpFile(name, numbered, savedName);
    if (file == INVALID_HANDLE_VALUE)
    {
        VirtualFree(row, 0, MEM_RELEASE);
        return FALSE;
    }

    ok = WriteFile(file, header, headerSize, &written, NULL) &&
        written == headerSize;
    for (line = static_cast<LONG>(height) - 1; ok && line >= 0; --line)
    {
        const BYTE* source = pixels + line * pitch;
        DWORD column;

        // The padding bytes stay zero: VirtualAlloc returns zeroed memory
        // and only the first width pixels are written.
        if (bitsPerPixel == 8)
        {
            // A plain copy loop would compile to memcpy, which this
            // CRT-free DLL does not have.
            __movsb(row, source, width);
        }
        else
        {
            const DWORD* color = reinterpret_cast<const DWORD*>(source);

            for (column = 0; column < width; ++column)
            {
                row[column * 3] = static_cast<BYTE>(color[column]);
                row[column * 3 + 1] = static_cast<BYTE>(color[column] >> 8);
                row[column * 3 + 2] = static_cast<BYTE>(color[column] >> 16);
            }
        }

        ok = WriteFile(file, row, stride, &written, NULL) && written == stride;
    }

    CloseHandle(file);
    VirtualFree(row, 0, MEM_RELEASE);
    return ok;
}
