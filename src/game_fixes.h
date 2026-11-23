/**
 * Fixes for faults in the original game, independent of the widescreen
 * view.
 *
 * @file game_fixes.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GAME_FIXES_H
#define GAME_FIXES_H

#include <windows.h>

/**
 * Applies the fixes for faults in the original game.
 *
 * Applied once, after the executable has been recognized. Each fix is
 * switched by a key in the [Fixes] section of the settings file:
 * - FixBikeColors (on): the Rat and Super bikes race in their own colours.
 * - SmoothSprites (on): objects move in 1-pixel steps in HiRes.
 * - MidiVolumeGuard (on): a music volume of 0 no longer mutes the game,
 *   and a driver that cannot set the volume no longer stops the music.
 * - MusicDevice (-1): the MIDI output for the race music; -1 is the MIDI
 *   mapper.
 * - MidiErrorsToLog (on): race music errors go to the log instead of a
 *   hidden message box.
 * - FullRateObjects (on): riders and cars in view move every game tick.
 * - FixPunchShadow (on): the first punch frame keeps its shadow under the
 *   bike.
 * - GroundBuildings (on): three building pictures no longer float above
 *   the ground.
 * - FixOdometer (on): the HUD distance no longer wraps to 0 after 25.5.
 * - AllowAltTab (off): Alt+Tab and the Windows keys go to Windows.
 *
 * The results of the MIDI calls are always logged.
 *
 * @param [in] gameBase Module base of ROADRASH.EXE.
 * @param [in] iniPath The settings file, or NULL.
 */
void ApplyGameFixes(DWORD gameBase, const CHAR* iniPath);

#endif
