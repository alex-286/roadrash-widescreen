// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include <windows.h>
#include <unknwn.h>
#include <intrin.h>

#include "road_rash_patch.h"
#include "version.h"

// Windows 95's kernel32 has no InterlockedCompareExchange (only Windows 98
// and NT 4 do); the intrinsic is a plain lock cmpxchg and needs no import.
#pragma intrinsic(_InterlockedCompareExchange)

typedef HRESULT (WINAPI *DirectDrawCreateFn)(GUID*, LPVOID*, IUnknown*);
typedef HRESULT (WINAPI *DirectDrawCreateExFn)(GUID*, LPVOID*, REFIID, IUnknown*);
typedef HRESULT (WINAPI *DirectDrawEnumerateAFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumerateWFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumerateExAFn)(LPVOID, LPVOID, DWORD);
typedef HRESULT (WINAPI *DirectDrawEnumerateExWFn)(LPVOID, LPVOID, DWORD);

static HMODULE g_systemDdraw = NULL;
static HMODULE g_proxyModule = NULL;
static volatile LONG g_loadLock = 0;
static CHAR g_systemPath[MAX_PATH];

// Set when another copy of the patch already is the game's DirectDraw
// library: this copy then does nothing at all.
static BOOL g_inactiveCopy = FALSE;

// The game's DirectDrawCreate import, when this copy redirected it (loaded
// as a plugin, for example an .asi of an ASI loader).
static LPVOID* g_redirectedSlot = NULL;
static LPVOID g_redirectedOriginal = NULL;

static const CHAR kVersionExport[] = "RoadRashWidescreenVersion";

// Lets a second copy of the patch, and ChainDll=, recognize this DLL.
extern "C" DWORD WINAPI RoadRashWidescreenVersion()
{
    return (RRWS_VERSION_MAJOR << 16) | (RRWS_VERSION_MINOR << 8) |
        RRWS_VERSION_PATCH;
}

static BOOL IsPatchModule(HMODULE module)
{
    return module == g_proxyModule ||
        GetProcAddress(module, kVersionExport) != NULL;
}

// [Display] ChainDll=: another DirectDraw library (for example a wrapper
// renamed to dd2d9.dll) that this ddraw.dll forwards to instead of the
// system one. A name without a folder is looked up next to the game. Empty
// (the default): the system DirectDraw library.
static HMODULE LoadChainedDdraw()
{
    static CHAR settings[MAX_PATH];
    static CHAR name[MAX_PATH];
    static CHAR path[MAX_PATH];
    DWORD length;
    DWORD folderLength;
    HMODULE module;

    if (!GetPatchSettingsPath(settings))
    {
        return NULL;
    }

    GetPrivateProfileStringA(
        "Display", "ChainDll", "", name, sizeof(name), settings);

    // A comment after the value (; ...) and the spaces before it are not
    // part of the name.
    for (length = 0; name[length] != 0 && name[length] != ';'; ++length)
    {
    }
    while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == '\t'))
    {
        --length;
    }
    name[length] = 0;
    if (length == 0)
    {
        return NULL;
    }

    if (name[0] == '\\' || name[0] == '/' || (length >= 2 && name[1] == ':'))
    {
        lstrcpyA(path, name);
    }
    else
    {
        lstrcpyA(path, settings);
        folderLength = lstrlenA(path);
        while (folderLength > 0 && path[folderLength - 1] != '\\')
        {
            --folderLength;
        }
        if (folderLength + length + 1 > MAX_PATH)
        {
            LogProxyMessage("ChainDll is too long:", name, 0);
            return NULL;
        }
        lstrcpyA(path + folderLength, name);
    }

    module = LoadLibraryA(path);
    if (module == NULL)
    {
        LogProxyMessage(
            "Could not load the DirectDraw library named by ChainDll; using the system one:",
            path, GetLastError());
        return NULL;
    }

    if (IsPatchModule(module))
    {
        FreeLibrary(module);
        LogProxyMessage(
            "ChainDll names a copy of this patch; using the system DirectDraw library:",
            path, 0);
        return NULL;
    }

    if (GetProcAddress(module, "DirectDrawCreate") == NULL)
    {
        FreeLibrary(module);
        LogProxyMessage(
            "ChainDll names a library without DirectDrawCreate; using the system DirectDraw library:",
            path, 0);
        return NULL;
    }

    LogProxyMessage(
        "Loaded the DirectDraw library named by ChainDll:", path, 0);
    return module;
}

static HMODULE LoadSystemDdraw()
{
    while (_InterlockedCompareExchange(&g_loadLock, 1, 0) != 0)
    {
        Sleep(0);
    }

    if (g_systemDdraw == NULL)
    {
        g_systemDdraw = LoadChainedDdraw();
    }

    if (g_systemDdraw == NULL)
    {
        UINT length = GetSystemDirectoryA(g_systemPath, MAX_PATH);

        if (length == 0)
        {
            InterlockedExchange(&g_loadLock, 0);
            return NULL;
        }

        if (length >= MAX_PATH)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            InterlockedExchange(&g_loadLock, 0);
            return NULL;
        }

        if (g_systemPath[length - 1] == '\\' || g_systemPath[length - 1] == '/')
        {
            if (length + sizeof("ddraw.dll") > MAX_PATH)
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                InterlockedExchange(&g_loadLock, 0);
                return NULL;
            }
            lstrcatA(g_systemPath, "ddraw.dll");
        }
        else
        {
            if (length + sizeof("\\ddraw.dll") > MAX_PATH)
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                InterlockedExchange(&g_loadLock, 0);
                return NULL;
            }
            lstrcatA(g_systemPath, "\\ddraw.dll");
        }

        g_systemDdraw = LoadLibraryA(g_systemPath);
        if (g_systemDdraw == NULL)
        {
            DWORD error = GetLastError();
            LogProxyMessage(
                "Could not load the system DirectDraw library", g_systemPath,
                error);
            SetLastError(error);
        }
        else if (g_systemDdraw == g_proxyModule)
        {
            // Windows 9x matches loaded modules by name, so a full path
            // to the system ddraw.dll can return this proxy. Forwarding
            // to it would call these exports again forever.
            FreeLibrary(g_systemDdraw);
            g_systemDdraw = NULL;
            LogProxyMessage(
                "Windows returned this proxy instead of the system DirectDraw library",
                g_systemPath, 0);
            SetLastError(ERROR_DLL_INIT_FAILED);
        }
        else
        {
            LogProxyMessage(
                "Loaded the system DirectDraw library", g_systemPath, 0);
        }
    }

    HMODULE module = g_systemDdraw;
    InterlockedExchange(&g_loadLock, 0);
    return module;
}

static FARPROC ResolveSystemExport(LPCSTR name)
{
    HMODULE module = LoadSystemDdraw();
    if (module == NULL)
    {
        return NULL;
    }

    FARPROC procedure = GetProcAddress(module, name);
    if (procedure == NULL)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
    }
    return procedure;
}

static HRESULT ProxyFailure()
{
    DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

extern "C" HRESULT WINAPI DirectDrawCreate(
    GUID* lpGuid,
    LPVOID* lplpDD,
    IUnknown* pUnkOuter)
{
    // Loaded as a plugin: the function the game's import held before.
    DirectDrawCreateFn procedure =
        g_redirectedOriginal != NULL ?
            reinterpret_cast<DirectDrawCreateFn>(g_redirectedOriginal) :
            reinterpret_cast<DirectDrawCreateFn>(
                ResolveSystemExport("DirectDrawCreate"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    HRESULT result = procedure(lpGuid, lplpDD, pUnkOuter);
    if (SUCCEEDED(result) && lplpDD != NULL && *lplpDD != NULL)
    {
        InstallRoadRashWidescreenHook(*lplpDD);
    }
    else if (FAILED(result))
    {
        LogProxyMessage(
            "The system DirectDrawCreate failed with HRESULT", NULL,
            static_cast<DWORD>(result));
    }
    return result;
}

extern "C" HRESULT WINAPI DirectDrawCreateEx(
    GUID* lpGuid,
    LPVOID* lplpDD,
    REFIID iid,
    IUnknown* pUnkOuter)
{
    DirectDrawCreateExFn procedure =
        reinterpret_cast<DirectDrawCreateExFn>(
            ResolveSystemExport("DirectDrawCreateEx"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    return procedure(lpGuid, lplpDD, iid, pUnkOuter);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateA(
    LPVOID callback,
    LPVOID context)
{
    DirectDrawEnumerateAFn procedure =
        reinterpret_cast<DirectDrawEnumerateAFn>(
            ResolveSystemExport("DirectDrawEnumerateA"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    return procedure(callback, context);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateW(
    LPVOID callback,
    LPVOID context)
{
    DirectDrawEnumerateWFn procedure =
        reinterpret_cast<DirectDrawEnumerateWFn>(
            ResolveSystemExport("DirectDrawEnumerateW"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    return procedure(callback, context);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExA(
    LPVOID callback,
    LPVOID context,
    DWORD flags)
{
    DirectDrawEnumerateExAFn procedure =
        reinterpret_cast<DirectDrawEnumerateExAFn>(
            ResolveSystemExport("DirectDrawEnumerateExA"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    return procedure(callback, context, flags);
}

extern "C" HRESULT WINAPI DirectDrawEnumerateExW(
    LPVOID callback,
    LPVOID context,
    DWORD flags)
{
    DirectDrawEnumerateExWFn procedure =
        reinterpret_cast<DirectDrawEnumerateExWFn>(
            ResolveSystemExport("DirectDrawEnumerateExW"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    return procedure(callback, context, flags);
}

// TRUE when this DLL is ddraw.dll or RRWS.dll, the names under which the
// game imports it directly; it then works as a plain proxy.
static BOOL LoadedUnderProxyName(HMODULE module)
{
    static CHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    DWORD start = length;

    if (length == 0 || length >= MAX_PATH)
    {
        return TRUE;
    }

    while (start > 0 && path[start - 1] != '\\' && path[start - 1] != '/')
    {
        --start;
    }

    return lstrcmpiA(path + start, "ddraw.dll") == 0 ||
        lstrcmpiA(path + start, "RRWS.dll") == 0;
}

// The loaded module that holds an address, or NULL.
static HMODULE ModuleFromAddress(LPCVOID address)
{
    MEMORY_BASIC_INFORMATION memory;
    static CHAR path[MAX_PATH];

    if (VirtualQuery(address, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT ||
        memory.AllocationBase == NULL ||
        GetModuleFileNameA(
            reinterpret_cast<HMODULE>(memory.AllocationBase),
            path, MAX_PATH) == 0)
    {
        return NULL;
    }

    return reinterpret_cast<HMODULE>(memory.AllocationBase);
}

// Finds the game's import slot of DDRAW.dll!DirectDrawCreate, by its name
// in the import table. The slot counts only once Windows has resolved it:
// it then points into a loaded module, which is ddraw.dll itself or a
// Windows compatibility shim (apphelp.dll) in front of it.
static LPVOID* FindGameDirectDrawSlot()
{
    BYTE* image = reinterpret_cast<BYTE*>(GetModuleHandleA(NULL));
    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(image);
    PIMAGE_NT_HEADERS32 ntHeaders;
    PIMAGE_DATA_DIRECTORY directory;
    PIMAGE_IMPORT_DESCRIPTOR descriptor;
    DWORD imageSize;

    if (image == NULL || dosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
        dosHeader->e_lfanew <= 0)
    {
        return NULL;
    }

    ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS32>(
        image + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_IMPORT)
    {
        return NULL;
    }

    imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    directory = &ntHeaders->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory->VirtualAddress == 0 ||
        directory->VirtualAddress >= imageSize)
    {
        return NULL;
    }

    for (descriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
             image + directory->VirtualAddress);
         reinterpret_cast<BYTE*>(descriptor + 1) <= image + imageSize &&
             descriptor->Name != 0;
         ++descriptor)
    {
        LPVOID* slot;
        const DWORD* names;

        if (descriptor->Name >= imageSize ||
            descriptor->FirstThunk == 0 ||
            descriptor->FirstThunk >= imageSize ||
            descriptor->OriginalFirstThunk == 0 ||
            descriptor->OriginalFirstThunk >= imageSize ||
            lstrcmpiA(
                reinterpret_cast<const CHAR*>(image + descriptor->Name),
                "DDRAW.dll") != 0)
        {
            continue;
        }

        slot = reinterpret_cast<LPVOID*>(image + descriptor->FirstThunk);
        names = reinterpret_cast<const DWORD*>(
            image + descriptor->OriginalFirstThunk);
        for (;
             reinterpret_cast<const BYTE*>(names + 1) <= image + imageSize &&
                 reinterpret_cast<BYTE*>(slot + 1) <= image + imageSize &&
                 *names != 0;
             ++names, ++slot)
        {
            // By name (not by ordinal): IMAGE_IMPORT_BY_NAME, its name
            // after the 2-byte hint.
            if ((*names & IMAGE_ORDINAL_FLAG32) != 0 ||
                *names + 2 + sizeof("DirectDrawCreate") > imageSize ||
                lstrcmpA(
                    reinterpret_cast<const CHAR*>(image + *names + 2),
                    "DirectDrawCreate") != 0)
            {
                continue;
            }

            if (*slot == reinterpret_cast<LPVOID>(*names) ||
                ModuleFromAddress(*slot) == NULL)
            {
                return NULL;
            }

            return slot;
        }
    }

    return NULL;
}

// TRUE when another copy of the patch is loaded as the game's DirectDraw
// library (ddraw.dll, RRWS.dll) or receives the game's import.
static BOOL OtherPatchCopyLoaded(HMODULE self, LPVOID* slot)
{
    HMODULE module = GetModuleHandleA("ddraw.dll");

    if (module != NULL && module != self && IsPatchModule(module))
    {
        return TRUE;
    }

    module = GetModuleHandleA("RRWS.dll");
    if (module != NULL && module != self && IsPatchModule(module))
    {
        return TRUE;
    }

    module = slot != NULL ? ModuleFromAddress(*slot) : NULL;
    return module != NULL && module != self && IsPatchModule(module);
}

// Points the game's DirectDrawCreate import at this DLL, which calls the
// function the import held before (the other ddraw.dll, the system one or a
// compatibility shim in front of them).
static void RedirectGameDirectDraw(LPVOID* slot)
{
    static CHAR path[MAX_PATH];
    HMODULE ddraw = GetModuleHandleA("ddraw.dll");
    HMODULE previous = ModuleFromAddress(*slot);
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (!VirtualProtect(
            slot, sizeof(LPVOID), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        LogProxyMessage(
            "Loaded as a plugin, but the game's DirectDraw import could not be changed; the patch stays inactive",
            NULL, GetLastError());
        return;
    }

    // For the other DirectDraw exports, which the game does not import.
    if (ddraw != NULL && !IsPatchModule(ddraw) &&
        GetProcAddress(ddraw, "DirectDrawCreate") != NULL)
    {
        g_systemDdraw = ddraw;
    }

    g_redirectedOriginal = *slot;
    *slot = reinterpret_cast<LPVOID>(DirectDrawCreate);
    g_redirectedSlot = slot;
    VirtualProtect(slot, sizeof(LPVOID), oldProtection, &ignoredProtection);

    if (GetModuleFileNameA(
            g_systemDdraw != NULL ? g_systemDdraw : previous,
            path, MAX_PATH) == 0)
    {
        lstrcpyA(path, "(unknown path)");
    }
    LogProxyMessage(
        "Loaded as a plugin; the game's DirectDraw calls go through the patch to",
        path, 0);

    if (g_systemDdraw != NULL && previous != g_systemDdraw &&
        GetModuleFileNameA(previous, path, MAX_PATH) != 0)
    {
        LogProxyMessage(
            "The game's DirectDraw import passes through", path, 0);
    }
}

static void RestoreGameDirectDraw()
{
    DWORD oldProtection;
    DWORD ignoredProtection;

    if (g_redirectedSlot == NULL ||
        *g_redirectedSlot != reinterpret_cast<LPVOID>(DirectDrawCreate) ||
        !VirtualProtect(
            g_redirectedSlot, sizeof(LPVOID), PAGE_EXECUTE_READWRITE,
            &oldProtection))
    {
        return;
    }

    *g_redirectedSlot = g_redirectedOriginal;
    VirtualProtect(
        g_redirectedSlot, sizeof(LPVOID), oldProtection, &ignoredProtection);
    g_redirectedSlot = NULL;
}

extern "C" BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID reserved)
{
    reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        static CHAR path[MAX_PATH];
        LPVOID* slot = NULL;
        BOOL plugin;

        g_proxyModule = instance;

        // Under its own names the game imports this DLL directly. Under any
        // other name (an .asi, for example) it was loaded as a plugin and
        // has to take over the game's DirectDraw import itself.
        plugin = !LoadedUnderProxyName(instance);
        if (plugin)
        {
            slot = FindGameDirectDrawSlot();
            if (slot != NULL && ModuleFromAddress(*slot) == instance)
            {
                // The game imports this DLL under another name.
                slot = NULL;
                plugin = FALSE;
            }
            else if (OtherPatchCopyLoaded(instance, slot))
            {
                g_inactiveCopy = TRUE;
                if (GetModuleFileNameA(instance, path, MAX_PATH) == 0)
                {
                    lstrcpyA(path, "(unknown path)");
                }
                LogProxyMessage(
                    "The game already uses the patch as its DirectDraw library; this second copy stays inactive:",
                    path, 0);
                return TRUE;
            }
        }

        LogProxyAttach(instance);
        InstallCrashLog();

        if (slot != NULL)
        {
            RedirectGameDirectDraw(slot);
        }
        else if (plugin)
        {
            LogProxyMessage(
                "Loaded as a plugin, but the game's DirectDraw import was not found or is not resolved yet; the patch stays inactive",
                NULL, 0);
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_inactiveCopy)
        {
            return TRUE;
        }

        // reserved == NULL: the DLL is unloaded while the game keeps running.
        if (reserved == NULL)
        {
            RestoreGameDirectDraw();
        }

        // Shared DirectDraw vtables must not keep pointing into this DLL.
        RemoveProxyHooks();

        // reserved != NULL: the process is ending, not the DLL unloaded.
        if (reserved != NULL)
        {
            LogProxyDetach();
        }
    }
    return TRUE;
}
