/**
 * The widescreen patch: hooks the game's DirectDraw object, writes the log
 * and the crash log.
 *
 * @file road_rash_patch.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_PATCH_H
#define ROAD_RASH_PATCH_H

#include <windows.h>

/**
 * Installs the widescreen hooks into a new DirectDraw object.
 *
 * The first call checks the executable, reads the settings and installs
 * the game fixes, the draw distance, the distance fade, the smooth road,
 * the native menus and the render scale. Every call hooks CreateSurface
 * and SetDisplayMode of the object's vtable (once).
 *
 * @param [in] directDraw The IDirectDraw object the game created.
 * @return FALSE when the executable is not supported or the vtable could
 *         not be hooked.
 */
BOOL InstallRoadRashWidescreenHook(LPVOID directDraw);

/**
 * Starts a new log with the path of the proxy and the Windows version,
 * which shows that Windows loaded this proxy at all.
 *
 * Called from DllMain on process attach, so it only uses kernel32.
 *
 * @param [in] proxy The proxy DLL.
 */
void LogProxyAttach(HMODULE proxy);

/**
 * Writes a line to the log.
 *
 * @param [in] text The text, without line break (at most 160 characters
 *             are used).
 * @param [in] path A path written after the text, or NULL.
 * @param [in] error A Win32 error code written after them, or 0 for none.
 */
void LogProxyMessage(const CHAR* text, const CHAR* path, DWORD error);

/**
 * Writes the last log line when the game process ends normally
 * (ExitProcess); called from DllMain.
 */
void LogProxyDetach();

/**
 * Removes the DirectDraw hooks when the DLL is unloaded, before
 * LogProxyDetach.
 *
 * On Windows 95/98/ME, DDRAW.DLL and its vtables are shared by every
 * process: slots left pointing into this DLL lead into freed memory after
 * the game (DirectDraw's cleanup of the process, the next DirectDraw
 * program), which ended in a fatal exception 0E in KERNEL32. Every hooked
 * slot gets its original value back, unless something else changed it
 * since. On Windows NT each process has its own copy, which may already be
 * released, so it is left alone.
 */
void RemoveProxyHooks();

/**
 * Sets the crash log as the unhandled exception filter.
 *
 * Called when the DLL is loaded and again when the game creates
 * DirectDraw, in case the game set its own filter in between; the
 * previous filter is called after the crash was logged.
 */
void InstallCrashLog();

/**
 * Gives the path of the settings file RoadRashWidescreen.ini next to
 * ROADRASH.EXE, for the proxy, which reads ChainDll before the game creates
 * DirectDraw.
 *
 * @param [out] path Receives the path (MAX_PATH characters).
 * @return FALSE when the path does not fit; the defaults are used then.
 */
BOOL GetPatchSettingsPath(CHAR* path);

#endif
