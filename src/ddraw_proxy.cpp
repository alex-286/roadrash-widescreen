#include <windows.h>
#include <unknwn.h>

typedef HRESULT (WINAPI *DirectDrawCreateFn)(GUID*, LPVOID*, IUnknown*);
typedef HRESULT (WINAPI *DirectDrawCreateExFn)(GUID*, LPVOID*, REFIID, IUnknown*);
typedef HRESULT (WINAPI *DirectDrawEnumerateAFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumerateWFn)(LPVOID, LPVOID);
typedef HRESULT (WINAPI *DirectDrawEnumerateExAFn)(LPVOID, LPVOID, DWORD);
typedef HRESULT (WINAPI *DirectDrawEnumerateExWFn)(LPVOID, LPVOID, DWORD);

static HMODULE g_systemDdraw = NULL;
static volatile LONG g_loadLock = 0;
static CHAR g_systemPath[MAX_PATH];

static HMODULE LoadSystemDdraw()
{
    while (InterlockedCompareExchange(&g_loadLock, 1, 0) != 0)
    {
        Sleep(0);
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
    DirectDrawCreateFn procedure =
        reinterpret_cast<DirectDrawCreateFn>(
            ResolveSystemExport("DirectDrawCreate"));
    if (procedure == NULL)
    {
        return ProxyFailure();
    }
    return procedure(lpGuid, lplpDD, pUnkOuter);
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

extern "C" BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID reserved)
{
    instance;
    reason;
    reserved;
    return TRUE;
}
