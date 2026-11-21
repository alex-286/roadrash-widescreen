// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#ifndef ROAD_RASH_PATCH_H
#define ROAD_RASH_PATCH_H

#include <windows.h>

BOOL InstallRoadRashWidescreenHook(LPVOID directDraw);
void LogProxyAttach(HMODULE proxy);
void LogProxyMessage(const CHAR* text, const CHAR* path, DWORD error);
void LogProxyDetach();
void RemoveProxyHooks();
void InstallCrashLog();
BOOL GetPatchSettingsPath(CHAR* path);

#endif
