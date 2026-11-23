/**
 * The patch version, shown in the log, the setup tool and the file
 * properties.
 *
 * Also read by the resource compiler. The assembly version in
 * win9x_patcher.manifest and the title of dist/README.txt follow it by
 * hand.
 *
 * @file version.h
 * @author Alexander Schmidt
 * @copyright Copyright (C) 2026 Alexander Schmidt
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROAD_RASH_VERSION_H
#define ROAD_RASH_VERSION_H

/** The major version: changes that break existing installations. */
#define RRWS_VERSION_MAJOR 1
/** The minor version: new features. */
#define RRWS_VERSION_MINOR 2
/** The patch version: fixes. */
#define RRWS_VERSION_PATCH 0
/** The whole version as text, "major.minor.patch". */
#define RRWS_VERSION_STRING "1.2.0"

#endif
