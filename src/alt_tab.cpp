/**
 * Gives Alt+Tab and the other task switching keys back to Windows while
 * the game runs.
 *
 * The Windows compatibility database (sysmain.sdb) applies the IgnoreAltTab
 * fix to ROADRASH.EXE (matched by its name and IMAGES\\STREET.RRI and
 * IMAGES\\SPLASH.RRI). The fix lives in the 32-bit AcGenral.dll. When the
 * process has started, it creates a thread that installs a WH_KEYBOARD_LL
 * hook and then only runs a GetMessage loop. The hook swallows Alt+Tab,
 * Alt+Esc, Ctrl+Esc and the Windows and menu keys.
 *
 * Windows removes a low-level hook when the thread that installed it ends.
 * WM_QUIT ends the loop of that thread, so its hook goes away with it. The
 * thread is recognized by its start address inside AcGenral.dll and by its
 * first instructions, which push WH_KEYBOARD_LL (6A 0D) and call
 * SetWindowsHookEx through the import table (FF 15).
 *
 * The functions newer than Windows 95 are looked up at run time, so the DLL
 * still loads on Windows 9x, where the fix does not exist.
 *
 * Windows NT and newer handle Alt+Tab before the game gets the keys, so the
 * game's own block (see AllowAltTabWin9x) has no effect there.
 *
 * @file alt_tab.cpp
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "alt_tab.h"
#include "road_rash_patch.h"

#include <tlhelp32.h>

/**
 * ThreadQuerySetWin32StartAddress, the information class of
 * NtQueryInformationThread that gives a thread's start address.
 */
static const DWORD kThreadQuerySetWin32StartAddress = 9;
/** The number of bytes at a thread's start searched for the hook call. */
static const DWORD kStartCodeScanLength = 64;
/** The number of tries to post WM_QUIT to the hook thread. */
static const DWORD kPostAttempts = 20;
/** The wait between two tries in milliseconds. */
static const DWORD kPostRetryDelay = 50;
/** The longest wait for the hook thread to end in milliseconds. */
static const DWORD kThreadExitTimeout = 2000;

/** CreateToolhelp32Snapshot, looked up at run time. */
typedef HANDLE (WINAPI *CreateToolhelp32SnapshotFn)(DWORD, DWORD);
/** Thread32First or Thread32Next, looked up at run time. */
typedef BOOL (WINAPI *Thread32Fn)(HANDLE, LPTHREADENTRY32);
/** OpenThread, looked up at run time (not in Windows 95). */
typedef HANDLE (WINAPI *OpenThreadFn)(DWORD, BOOL, DWORD);
/** NtQueryInformationThread from ntdll.dll, looked up at run time. */
typedef LONG (WINAPI *NtQueryInformationThreadFn)(
    HANDLE, DWORD, PVOID, ULONG, PULONG);

/**
 * Writes a line to the log.
 *
 * @param [in] text The line, without line break.
 */
static void LogAltTab(const CHAR* text)
{
    LogProxyMessage(text, NULL, 0);
}

/**
 * Gives the size of a loaded module from its PE header.
 *
 * @param [in] module The module.
 * @return SizeOfImage, or 0 when the header is not valid.
 */
static DWORD ModuleImageSize(HMODULE module)
{
    const BYTE* base = reinterpret_cast<const BYTE*>(module);
    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return 0;
    }

    nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return 0;
    }

    return nt->OptionalHeader.SizeOfImage;
}

/**
 * Tells whether the code at a thread's start installs a low-level keyboard
 * hook first: push 0Dh (WH_KEYBOARD_LL) directly followed by
 * call dword ptr [import].
 *
 * @param [in] start The start of the thread.
 * @param [in] moduleEnd The end of the module that holds start.
 * @return TRUE when the code was found.
 */
static BOOL InstallsKeyboardHook(const BYTE* start, const BYTE* moduleEnd)
{
    DWORD index;

    for (index = 0; index + 4 <= kStartCodeScanLength; ++index)
    {
        if (start + index + 4 > moduleEnd)
        {
            return FALSE;
        }

        if (start[index] == 0x6A && start[index + 1] == 0x0D &&
            start[index + 2] == 0xFF && start[index + 3] == 0x15)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * Ends the thread of the IgnoreAltTab fix with WM_QUIT.
 *
 * Posting fails while the thread has no message queue yet, so it is tried
 * kPostAttempts times.
 *
 * @param [in] thread The thread, opened with SYNCHRONIZE.
 * @param [in] threadId The id of the thread.
 * @return TRUE when the thread has ended.
 */
static BOOL EndHookThread(HANDLE thread, DWORD threadId)
{
    DWORD attempt;

    for (attempt = 0; attempt < kPostAttempts; ++attempt)
    {
        // Fails while the thread has no message queue yet.
        if (PostThreadMessageA(threadId, WM_QUIT, 0, 0))
        {
            return WaitForSingleObject(thread, kThreadExitTimeout) ==
                WAIT_OBJECT_0;
        }

        Sleep(kPostRetryDelay);
    }

    return FALSE;
}

/** The game's WH_KEYBOARD hook procedure (0x423260). */
static const DWORD kGameKeyboardHookRva = 0x00023260;
/** The game's handle of that hook (0x49612C). */
static const DWORD kGameKeyboardHookHandleRva = 0x0009612C;
/** The handle of the game's main window (0x4753DC). */
static const DWORD kGameWindowRva = 0x000753DC;
/** The window procedure of the main window, class RashRoot (0x4232A0). */
static const DWORD kGameWindowProcRva = 0x000232A0;
/** The context code bit of a key message: Alt is held. */
static const DWORD kKeyAltDown = 0x20000000;
/** The largest number of switching keys and commands written to the log. */
static const DWORD kMaxSwitchLogs = 8;

/** The start of the game's hook up to its test for Tab with Alt held. */
static const BYTE kGameKeyboardHookCode[] =
{
    0x8B, 0x44, 0x24, 0x04, 0x53, 0x8B, 0x4C, 0x24, 0x0C, 0x85, 0xC0, 0x7C,
    0x1A, 0x8B, 0x54, 0x24, 0x10, 0x83, 0xF9, 0x09, 0x75, 0x15, 0xF7, 0xC2,
    0x00, 0x00, 0x00, 0x20
};

/** The game's main window, on Windows 9x. */
static HWND g_gameWindow = NULL;
/** The game's window procedure that SwitchWindowProc passes on to. */
static WNDPROC g_gameWindowProc = NULL;
/** The WH_GETMESSAGE hook of SwitchMessageHook. */
static HHOOK g_messageHook = NULL;
/** The number of switching keys and commands written to the log. */
static DWORD g_switchLogCount = 0;

/**
 * Tells whether a key message is Alt+Tab, Alt+Shift+Tab, Alt+Esc or a
 * Windows key.
 *
 * @param [in] message The message.
 * @param [in] key The virtual key (wParam).
 * @param [in] data The key data (lParam).
 * @return TRUE for a switching key.
 */
static BOOL IsSwitchKey(UINT message, WPARAM key, LPARAM data)
{
    if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
    {
        return (key == VK_TAB || key == VK_ESCAPE) &&
            (static_cast<DWORD>(data) & kKeyAltDown) != 0;
    }

    if (message == WM_KEYDOWN || message == WM_KEYUP)
    {
        return key == VK_LWIN || key == VK_RWIN;
    }

    return FALSE;
}

/**
 * Tells whether a message is a WM_SYSCOMMAND that switches windows.
 *
 * @param [in] message The message.
 * @param [in] command The command (wParam).
 * @return TRUE for SC_NEXTWINDOW, SC_PREVWINDOW and SC_TASKLIST.
 */
static BOOL IsSwitchCommand(UINT message, WPARAM command)
{
    if (message != WM_SYSCOMMAND)
    {
        return FALSE;
    }

    command &= 0xFFF0;
    return command == SC_NEXTWINDOW || command == SC_PREVWINDOW ||
        command == SC_TASKLIST;
}

/**
 * Writes a line to the log, up to kMaxSwitchLogs times.
 *
 * @param [in] text The line, without line break.
 */
static void LogSwitch(const CHAR* text)
{
    if (g_switchLogCount < kMaxSwitchLogs)
    {
        ++g_switchLogCount;
        LogAltTab(text);
    }
}

/**
 * WH_GETMESSAGE hook: moves the switching keys of every window of the game
 * to the main window.
 *
 * @param [in] code The hook code.
 * @param [in] removal PM_REMOVE when the message is taken from the queue.
 * @param [in,out] data The MSG.
 * @return The result of the next hook.
 */
static LRESULT CALLBACK SwitchMessageHook(int code, WPARAM removal, LPARAM data)
{
    if (code == HC_ACTION && removal == PM_REMOVE)
    {
        MSG* message = reinterpret_cast<MSG*>(data);

        if (message->hwnd != NULL && message->hwnd != g_gameWindow &&
            IsSwitchKey(message->message, message->wParam, message->lParam))
        {
            message->hwnd = g_gameWindow;
        }
    }

    return CallNextHookEx(g_messageHook, code, removal, data);
}

/**
 * Subclass of the main window: gives the switching keys and commands to
 * DefWindowProc and everything else to the game.
 *
 * @param [in] window The main window.
 * @param [in] message The message.
 * @param [in] wParam The first message parameter.
 * @param [in] lParam The second message parameter.
 * @return The result of DefWindowProc or of the game's procedure.
 */
static LRESULT CALLBACK SwitchWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (IsSwitchKey(message, wParam, lParam))
    {
        if (message == WM_SYSKEYDOWN || message == WM_KEYDOWN)
        {
            LogSwitch("AllowAltTab: gave a switching key to Windows.");
        }

        return DefWindowProcA(window, message, wParam, lParam);
    }

    if (IsSwitchCommand(message, wParam))
    {
        LogSwitch("AllowAltTab: gave a switching command to Windows.");
        return DefWindowProcA(window, message, wParam, lParam);
    }

    if (message == WM_ACTIVATEAPP)
    {
        LogSwitch(wParam != 0 ?
            "AllowAltTab: the game has the focus again." :
            "AllowAltTab: the game lost the focus.");
    }

    return CallWindowProcA(g_gameWindowProc, window, message, wParam, lParam);
}

/**
 * Removes the game's own Alt+Tab block on Windows 95/98/ME.
 *
 * These have no IgnoreAltTab fix: there the game blocks the keys itself.
 * On Windows 9x, DefWindowProc starts the switch when it gets Alt+Tab or
 * Alt+Esc and then sends the main window a WM_SYSCOMMAND. ROADRASH.EXE 1.0A
 * keeps both from it in three places:
 * - Its main window (class RashRoot, procedure 0x4232A0, handle at
 *   0x4753DC) installs a WH_KEYBOARD hook (0x423260, handle at 0x49612C)
 *   in WM_CREATE that discards Tab while Alt is held.
 * - During a race the main window answers every WM_SYSKEYDOWN itself. In
 *   the menus the focus is on the buttons (class RashButton, 0x4227D0),
 *   which answer every WM_KEYDOWN and WM_SYSKEYDOWN.
 * - In the menus the main window swallows every WM_SYSCOMMAND except
 *   SC_RESTORE.
 *
 * This removes the hook. A message hook moves the switching keys of every
 * window of the game to the main window, and a subclass of the main window
 * gives these keys and the switching commands to DefWindowProc. The game
 * itself restores its surfaces when it gets the focus back (0x41DF60).
 *
 * @param [in] gameBase Module base of ROADRASH.EXE 1.0A.
 */
static void AllowAltTabWin9x(DWORD gameBase)
{
    const BYTE* hookCode =
        reinterpret_cast<const BYTE*>(gameBase + kGameKeyboardHookRva);
    HHOOK* gameHook =
        reinterpret_cast<HHOOK*>(gameBase + kGameKeyboardHookHandleRva);
    HWND window = *reinterpret_cast<HWND*>(gameBase + kGameWindowRva);
    DWORD threadId;
    DWORD index;

    for (index = 0; index < sizeof(kGameKeyboardHookCode); ++index)
    {
        if (hookCode[index] != kGameKeyboardHookCode[index])
        {
            LogAltTab("AllowAltTab stays off: the game's keyboard hook differs.");
            return;
        }
    }

    if (window == NULL || !IsWindow(window) ||
        GetWindowLongA(window, GWL_WNDPROC) !=
            static_cast<LONG>(gameBase + kGameWindowProcRva))
    {
        LogAltTab("AllowAltTab stays off: the game's main window was not found.");
        return;
    }

    threadId = GetWindowThreadProcessId(window, NULL);
    g_gameWindow = window;
    g_messageHook = SetWindowsHookExA(
        WH_GETMESSAGE, SwitchMessageHook, NULL, threadId);
    if (g_messageHook == NULL)
    {
        LogProxyMessage(
            "AllowAltTab stays off: the message hook failed.",
            NULL,
            GetLastError());
        return;
    }

    g_gameWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongA(
        window,
        GWL_WNDPROC,
        reinterpret_cast<LONG>(SwitchWindowProc)));
    if (g_gameWindowProc == NULL)
    {
        LogProxyMessage(
            "AllowAltTab stays off: the main window cannot be subclassed.",
            NULL,
            GetLastError());
        UnhookWindowsHookEx(g_messageHook);
        g_messageHook = NULL;
        return;
    }

    // The game unhooks the handle again in WM_DESTROY; NULL makes that a
    // harmless no-op.
    if (*gameHook != NULL)
    {
        UnhookWindowsHookEx(*gameHook);
        *gameHook = NULL;
        LogAltTab("AllowAltTab: removed the game's Alt+Tab block; Alt+Tab, Alt+Esc and the Windows key go to Windows.");
    }
    else
    {
        LogAltTab("AllowAltTab: the game's keyboard hook is not installed; Alt+Tab, Alt+Esc and the Windows key go to Windows.");
    }
}

void AllowAltTab(DWORD gameBase)
{
    HMODULE shims;
    HMODULE kernel;
    HMODULE ntdll;
    CreateToolhelp32SnapshotFn createSnapshot;
    Thread32Fn threadFirst;
    Thread32Fn threadNext;
    OpenThreadFn openThread;
    NtQueryInformationThreadFn queryThread;
    const BYTE* moduleStart;
    const BYTE* moduleEnd;
    DWORD imageSize;
    DWORD processId;
    DWORD currentThreadId;
    HANDLE snapshot;
    THREADENTRY32 entry;
    DWORD found = 0;
    DWORD ended = 0;

    if ((GetVersion() & 0x80000000) != 0)
    {
        AllowAltTabWin9x(gameBase);
        return;
    }

    shims = GetModuleHandleA("AcGenral.dll");
    if (shims == NULL)
    {
        LogAltTab("AllowAltTab: no Windows compatibility fix blocks Alt+Tab.");
        return;
    }

    kernel = GetModuleHandleA("kernel32.dll");
    ntdll = GetModuleHandleA("ntdll.dll");
    if (kernel == NULL || ntdll == NULL)
    {
        LogAltTab("AllowAltTab stays off: a system library is missing.");
        return;
    }

    createSnapshot = reinterpret_cast<CreateToolhelp32SnapshotFn>(
        GetProcAddress(kernel, "CreateToolhelp32Snapshot"));
    threadFirst = reinterpret_cast<Thread32Fn>(
        GetProcAddress(kernel, "Thread32First"));
    threadNext = reinterpret_cast<Thread32Fn>(
        GetProcAddress(kernel, "Thread32Next"));
    openThread = reinterpret_cast<OpenThreadFn>(
        GetProcAddress(kernel, "OpenThread"));
    queryThread = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(ntdll, "NtQueryInformationThread"));
    if (createSnapshot == NULL || threadFirst == NULL ||
        threadNext == NULL || openThread == NULL || queryThread == NULL)
    {
        LogAltTab("AllowAltTab stays off: Windows lacks the thread functions.");
        return;
    }

    imageSize = ModuleImageSize(shims);
    if (imageSize == 0)
    {
        LogAltTab("AllowAltTab stays off: AcGenral.dll has no valid header.");
        return;
    }

    moduleStart = reinterpret_cast<const BYTE*>(shims);
    moduleEnd = moduleStart + imageSize;
    processId = GetCurrentProcessId();
    currentThreadId = GetCurrentThreadId();

    snapshot = createSnapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        LogAltTab("AllowAltTab stays off: the thread list is unavailable.");
        return;
    }

    entry.dwSize = sizeof(entry);
    if (threadFirst(snapshot, &entry))
    {
        do
        {
            HANDLE thread;
            const BYTE* start = NULL;

            if (entry.th32OwnerProcessID != processId ||
                entry.th32ThreadID == currentThreadId)
            {
                continue;
            }

            thread = openThread(
                THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE,
                entry.th32ThreadID);
            if (thread == NULL)
            {
                continue;
            }

            if (queryThread(
                    thread, kThreadQuerySetWin32StartAddress,
                    &start, sizeof(start), NULL) >= 0 &&
                start >= moduleStart && start < moduleEnd &&
                InstallsKeyboardHook(start, moduleEnd))
            {
                ++found;
                if (EndHookThread(thread, entry.th32ThreadID))
                {
                    ++ended;
                }
            }

            CloseHandle(thread);
            entry.dwSize = sizeof(entry);
        }
        while (threadNext(snapshot, &entry));
    }

    CloseHandle(snapshot);

    if (found == 0)
    {
        LogAltTab("AllowAltTab: the Alt+Tab block of Windows was not found; Alt+Tab stays as Windows sets it.");
    }
    else if (ended == found)
    {
        LogAltTab("AllowAltTab: ended the Alt+Tab block of Windows; Alt+Tab and the Windows key work.");
    }
    else
    {
        LogAltTab("AllowAltTab: the Alt+Tab block of Windows did not end; Alt+Tab stays blocked.");
    }
}
