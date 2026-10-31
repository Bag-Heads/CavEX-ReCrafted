/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef IMPORT_CONVERT_NOTICE_H
#define IMPORT_CONVERT_NOTICE_H

#include <stdbool.h>

#include "../../network/level_archive.h"

/* convert-notice-1024 (maintainer Option B, 2026-10-23), reworked by
 * ui-build-1026/T09: which worlds get the "Must be converted" tag and the
 * backup question before a Minecraft Beta world is loaded and stamped.
 *
 * WHICH WORLDS: the same two facts the import path reads.
 *   - no CaveWiiGenerator tag (level_archive_was_created_by_cavewii false):
 *     the world was not made by this port (server_local.c's provenance
 *     checks in the save-version chain);
 *   - CaveWiiSaveVersion below CAVEWII_SAVE_VERSION_CURRENT: the
 *     server_local save-version chain will run ("Converting world...") and
 *     stamp level.dat on this load.
 * Once the chain stamps the current version the tag never shows again for
 * that world. Cancel writes nothing, so it asks again next time.
 *
 * dropin-1029 (maintainer-approved, tmp/jobs/dropin-1029/REPORT.md): a REAL
 * Minecraft Beta world is drop-in -- no tag, no backup question. The chain
 * still runs silently at load: on the three real Beta worlds measured it
 * changes zero region bytes and adds only CaveWii* level.dat keys that Beta
 * ignores (WorldInfo reads named keys only). The convert screen stays ONLY
 * for pre-2026-06-11 CavEX dev saves: no CaveWiiGenerator AND CaveWii-made
 * evidence with no Beta evidence (level_archive_world_origin ==
 * LEVEL_ORIGIN_CAVEWII_LEGACY; that function documents both evidence sets
 * and its bounded, read-only chunk probe). */
static inline bool import_convert_notice_needed(const struct level_archive* la) {
	return level_archive_get_save_version(la) < CAVEWII_SAVE_VERSION_CURRENT
		&& level_archive_world_origin(la) == LEVEL_ORIGIN_CAVEWII_LEGACY;
}

/* ui-build-1026/T09 (approved convert set, tmp/jobs/design-round-1026b/
 * convert): the old three-line Option B notice is gone. A Beta world now
 * shows a red "Must be converted" tag in the Load list, and Play opens
 * screen_convert_backup ("Back up this world?", Cancel focused). Player-
 * visible text says "CavEX" (maintainer name rule 2026-10-23). */
#define IMPORT_CONVERT_TAG "Must be converted"
#define CONVERT_BACKUP_TITLE "Back up this world?"
#define CONVERT_BACKUP_COPYING_TITLE "Backing up world"
#define CONVERT_BACKUP_CLEANING_TITLE "Cleaning up..."
#define CONVERT_BACKUP_ERROR_TITLE "Couldn't back up world"
#define CONVERT_BACKUP_BUTTON_BACKUP "Back up & convert"
#define CONVERT_BACKUP_BUTTON_NO_BACKUP "Convert without backup"
#define CONVERT_BACKUP_BUTTON_CANCEL "Cancel"

#endif
