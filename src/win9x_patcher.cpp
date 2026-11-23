/**
 * RRWSPatch.exe: makes ROADRASH.EXE load the widescreen patch on Windows
 * 95/98/ME.
 *
 * Windows 9x keeps using the system ddraw.dll even when a ddraw.dll is next
 * to the game (its dsound.dll depends on the system copy, and the 9x loader
 * shares modules by name). This tool renames the game's DDRAW.dll import to
 * RRWS.dll, which is the patch DLL under another name. Running it again
 * restores the original import. Only the 9 bytes of the import name change.
 *
 * @file win9x_patcher.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>

#include "version.h"

/** The PE timestamp of ROADRASH.EXE 1.0A. */
static const DWORD kExpectedTimestamp = 0x321DCBE7;
/** The title of the tool's message boxes. */
static const CHAR kTitle[] =
    "Road Rash Widescreen " RRWS_VERSION_STRING " - Windows 9x setup";
/** The game's original DirectDraw import name. */
static const CHAR kOriginalName[] = "DDRAW.dll";
/** The import name after the change. */
static const CHAR kPatchedName[] = "RRWS.dll";
/** The game executable, next to the tool. */
static const CHAR kGameFile[] = "ROADRASH.EXE";
/** The copy of the unchanged game executable. */
static const CHAR kBackupFile[] = "ROADRASH.BAK";
/** The patch DLL under the name the changed game imports. */
static const CHAR kDllFile[] = "RRWS.dll";

/**
 * Shows a message box with the tool's title.
 *
 * @param [in] text The message.
 * @param [in] type The MB_ button and icon flags.
 */
static void ShowMessage(const CHAR* text, UINT type)
{
    MessageBoxA(NULL, text, kTitle, type | MB_SETFOREGROUND);
}

/**
 * Shows an error message with a Windows error code.
 *
 * @param [in] text The message.
 * @param [in] error The Windows error code.
 */
static void ShowError(const CHAR* text, DWORD error)
{
    CHAR message[512];

    wsprintfA(message, "%s\n\n(Windows error %lu)", text, error);
    ShowMessage(message, MB_OK | MB_ICONERROR);
}

/**
 * Builds the path of a file in the folder of this program.
 *
 * @param [out] path Receives the folder, with a trailing backslash, and the
 *              file name (MAX_PATH characters).
 * @param [in] fileName The file name.
 * @return FALSE if it does not fit.
 */
static BOOL BuildPath(CHAR* path, const CHAR* fileName)
{
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);

    if (length == 0 || length >= MAX_PATH)
    {
        return FALSE;
    }

    while (length > 0 && path[length - 1] != '\\' && path[length - 1] != '/')
    {
        --length;
    }

    if (length == 0 || length + lstrlenA(fileName) + 1 > MAX_PATH)
    {
        return FALSE;
    }

    lstrcpyA(path + length, fileName);
    return TRUE;
}

/**
 * Maps an RVA to a file offset through the section table.
 *
 * @param [in] image The file.
 * @param [in] fileSize The size of the file.
 * @param [in] nt The PE header in the file.
 * @param [in] rva The RVA.
 * @param [in] size The number of bytes that must follow.
 * @param [out] offset Receives the file offset.
 * @return FALSE if the RVA plus size is not inside one section's file
 *         data.
 */
static BOOL RvaToOffset(
    const BYTE* image,
    DWORD fileSize,
    const IMAGE_NT_HEADERS32* nt,
    DWORD rva,
    DWORD size,
    DWORD* offset)
{
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    DWORD count = nt->FileHeader.NumberOfSections;
    DWORD index;

    if (reinterpret_cast<const BYTE*>(section + count) > image + fileSize)
    {
        return FALSE;
    }

    for (index = 0; index < count; ++index)
    {
        DWORD start = section[index].VirtualAddress;
        DWORD rawSize = section[index].SizeOfRawData;

        if (rva >= start && rva - start < rawSize && size <= rawSize - (rva - start))
        {
            DWORD fileOffset = section[index].PointerToRawData + (rva - start);

            if (fileOffset > fileSize || size > fileSize - fileOffset)
            {
                return FALSE;
            }

            *offset = fileOffset;
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * Compares a string in the file with a name, case-insensitively, and
 * requires the terminating zero.
 *
 * @param [in] text The string in the file.
 * @param [in] maxLength The bytes that may be read.
 * @param [in] name The name.
 * @return TRUE when they are equal.
 */
static BOOL NameEquals(const BYTE* text, DWORD maxLength, const CHAR* name)
{
    DWORD index;

    for (index = 0; index < maxLength; ++index)
    {
        CHAR a = static_cast<CHAR>(text[index]);
        CHAR b = name[index];

        if (a >= 'a' && a <= 'z')
        {
            a = static_cast<CHAR>(a - 'a' + 'A');
        }

        if (b >= 'a' && b <= 'z')
        {
            b = static_cast<CHAR>(b - 'a' + 'A');
        }

        if (a != b)
        {
            return FALSE;
        }

        if (a == '\0')
        {
            return TRUE;
        }
    }

    return FALSE;
}

/** The state of the game's DirectDraw import. */
enum ImportState
{
    /** Not found, or not the supported executable. */
    kImportUnknown,
    /** The original DDRAW.dll. */
    kImportOriginal,
    /** Changed to RRWS.dll. */
    kImportPatched
};

/**
 * Finds the DirectDraw import of the game.
 *
 * The name area must hold "DDRAW.dll" and its zero (10 bytes).
 *
 * @param [in] image The file.
 * @param [in] fileSize The size of the file.
 * @param [out] nameOffset Receives the file offset of the import name.
 * @param [out] problem Receives the message for kImportUnknown.
 * @return The state of the import.
 */
static ImportState FindDirectDrawImport(
    const BYTE* image,
    DWORD fileSize,
    DWORD* nameOffset,
    const CHAR** problem)
{
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const IMAGE_NT_HEADERS32* nt;
    DWORD directoryRva;
    DWORD directoryOffset;
    DWORD index;

    *problem = "ROADRASH.EXE is not a valid Windows program.";
    if (fileSize < sizeof(IMAGE_DOS_HEADER) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
        static_cast<DWORD>(dos->e_lfanew) > fileSize - sizeof(IMAGE_NT_HEADERS32))
    {
        return kImportUnknown;
    }

    nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
    {
        return kImportUnknown;
    }

    if (nt->FileHeader.TimeDateStamp != kExpectedTimestamp)
    {
        *problem =
            "This ROADRASH.EXE is not the supported Road Rash "
            "version 1.0A. It was not changed.";
        return kImportUnknown;
    }

    *problem = "The DirectDraw import was not found in ROADRASH.EXE.";
    directoryRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for (index = 0; index < 64; ++index)
    {
        const IMAGE_IMPORT_DESCRIPTOR* descriptor;
        DWORD offset;

        if (!RvaToOffset(
                image, fileSize, nt,
                directoryRva + index * sizeof(IMAGE_IMPORT_DESCRIPTOR),
                sizeof(IMAGE_IMPORT_DESCRIPTOR),
                &directoryOffset))
        {
            return kImportUnknown;
        }

        descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            image + directoryOffset);
        if (descriptor->Name == 0 && descriptor->FirstThunk == 0)
        {
            return kImportUnknown;
        }

        if (!RvaToOffset(
                image, fileSize, nt, descriptor->Name,
                sizeof(kOriginalName), &offset))
        {
            continue;
        }

        if (NameEquals(image + offset, sizeof(kOriginalName), kOriginalName))
        {
            *nameOffset = offset;
            return kImportOriginal;
        }

        // A patched name keeps the zero bytes up to the original length.
        if (NameEquals(image + offset, sizeof(kPatchedName), kPatchedName) &&
            image[offset + sizeof(kOriginalName) - 1] == 0)
        {
            *nameOffset = offset;
            return kImportPatched;
        }
    }

    return kImportUnknown;
}

/**
 * Reads a whole file (up to 64 MB) into memory.
 *
 * @param [in] path The file.
 * @param [out] data Receives the contents, to be freed with VirtualFree.
 * @param [out] size Receives the size of the file.
 * @return FALSE (with the last error set) when it could not be read.
 */
static BOOL ReadWholeFile(const CHAR* path, BYTE** data, DWORD* size)
{
    HANDLE file = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD fileSize;
    DWORD bytesRead;
    BYTE* buffer;

    if (file == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    fileSize = GetFileSize(file, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > 64 * 1024 * 1024)
    {
        CloseHandle(file);
        SetLastError(ERROR_BAD_FORMAT);
        return FALSE;
    }

    buffer = reinterpret_cast<BYTE*>(VirtualAlloc(
        NULL, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer == NULL)
    {
        CloseHandle(file);
        return FALSE;
    }

    if (!ReadFile(file, buffer, fileSize, &bytesRead, NULL) || bytesRead != fileSize)
    {
        DWORD error = GetLastError();
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(file);
        SetLastError(error == ERROR_SUCCESS ? ERROR_READ_FAULT : error);
        return FALSE;
    }

    CloseHandle(file);
    *data = buffer;
    *size = fileSize;
    return TRUE;
}

/**
 * Writes the new import name over the old one.
 *
 * A copy of the game from a CD can be read-only, so that attribute is
 * lifted for the write and restored.
 *
 * @param [in] path The game executable.
 * @param [in] offset The file offset of the import name.
 * @param [in] name The new name.
 * @return FALSE (with the last error set) when it could not be written.
 */
static BOOL WriteImportName(const CHAR* path, DWORD offset, const CHAR* name)
{
    DWORD attributes = GetFileAttributesA(path);
    BOOL clearedReadOnly = FALSE;
    HANDLE file;
    DWORD written = 0;
    BOOL ok;
    DWORD error;

    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0)
    {
        if (!SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY))
        {
            return FALSE;
        }
        clearedReadOnly = TRUE;
    }

    file = CreateFileA(
        path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        if (clearedReadOnly)
        {
            SetFileAttributesA(path, attributes);
        }
        SetLastError(error);
        return FALSE;
    }

    ok = SetFilePointer(file, static_cast<LONG>(offset), NULL, FILE_BEGIN) == offset &&
        WriteFile(file, name, lstrlenA(name) + 1, &written, NULL) &&
        written == static_cast<DWORD>(lstrlenA(name) + 1);
    error = ok ? ERROR_SUCCESS : GetLastError();
    if (!CloseHandle(file) && ok)
    {
        ok = FALSE;
        error = GetLastError();
    }

    if (clearedReadOnly)
    {
        SetFileAttributesA(path, attributes);
    }

    SetLastError(error == ERROR_SUCCESS && !ok ? ERROR_WRITE_FAULT : error);
    return ok;
}

/** Asks the player and changes or restores the game's DirectDraw import. */
static void Run()
{
    CHAR gamePath[MAX_PATH];
    CHAR backupPath[MAX_PATH];
    CHAR dllPath[MAX_PATH];
    BYTE* image = NULL;
    DWORD imageSize = 0;
    DWORD nameOffset = 0;
    const CHAR* problem = NULL;
    ImportState state;

    if (!BuildPath(gamePath, kGameFile) ||
        !BuildPath(backupPath, kBackupFile) ||
        !BuildPath(dllPath, kDllFile))
    {
        ShowMessage("The folder path is too long.", MB_OK | MB_ICONERROR);
        return;
    }

    if (GetFileAttributesA(gamePath) == INVALID_FILE_ATTRIBUTES)
    {
        ShowMessage(
            "ROADRASH.EXE was not found.\n\n"
            "Copy RRWSPatch.exe and RRWS.dll into the Road Rash folder, "
            "next to ROADRASH.EXE, and run RRWSPatch.exe there.",
            MB_OK | MB_ICONERROR);
        return;
    }

    if (!ReadWholeFile(gamePath, &image, &imageSize))
    {
        ShowError("ROADRASH.EXE could not be read.", GetLastError());
        return;
    }

    state = FindDirectDrawImport(image, imageSize, &nameOffset, &problem);
    VirtualFree(image, 0, MEM_RELEASE);

    if (state == kImportUnknown)
    {
        ShowMessage(problem, MB_OK | MB_ICONERROR);
        return;
    }

    if (state == kImportOriginal)
    {
        if (MessageBoxA(
                NULL,
                "Set up ROADRASH.EXE for the widescreen patch on "
                "Windows 95/98/ME?\n\n"
                "The game will then load RRWS.dll instead of the Windows "
                "DirectDraw library. Only this one name in the program "
                "changes. A copy of the unchanged program is saved as "
                "ROADRASH.BAK.\n\n"
                "Run RRWSPatch.exe again to undo the change.",
                kTitle,
                MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES)
        {
            return;
        }

        // An existing backup is kept: it may be the only unchanged copy.
        if (!CopyFileA(gamePath, backupPath, TRUE))
        {
            DWORD error = GetLastError();

            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            {
                ShowError(
                    "The backup ROADRASH.BAK could not be created. "
                    "ROADRASH.EXE was not changed.",
                    error);
                return;
            }
        }

        if (!WriteImportName(gamePath, nameOffset, kPatchedName))
        {
            ShowError(
                "ROADRASH.EXE could not be changed. Make sure the game is "
                "not running.",
                GetLastError());
            return;
        }

        if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES)
        {
            ShowMessage(
                "ROADRASH.EXE is set up, but RRWS.dll is missing.\n\n"
                "Copy RRWS.dll into the game folder, next to ROADRASH.EXE. "
                "Without it the game does not start.",
                MB_OK | MB_ICONWARNING);
            return;
        }

        ShowMessage(
            "ROADRASH.EXE is set up. Start the game as usual.\n\n"
            "Keep RRWS.dll next to ROADRASH.EXE: without it the game does "
            "not start.",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (MessageBoxA(
            NULL,
            "ROADRASH.EXE is already set up for the widescreen patch.\n\n"
            "Undo the change? The game then uses the Windows DirectDraw "
            "library again and runs without the patch on Windows 95/98/ME.",
            kTitle,
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
    {
        return;
    }

    if (!WriteImportName(gamePath, nameOffset, kOriginalName))
    {
        ShowError(
            "ROADRASH.EXE could not be changed. Make sure the game is not "
            "running.",
            GetLastError());
        return;
    }

    ShowMessage(
        "ROADRASH.EXE uses the Windows DirectDraw library again.",
        MB_OK | MB_ICONINFORMATION);
}

/** The entry point of RRWSPatch.exe (no C runtime). */
extern "C" void WINAPI PatcherMain()
{
    Run();
    ExitProcess(0);
}
