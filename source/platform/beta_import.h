/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* In-game "Import Beta 1.7.3" (contract tmp/jobs/ingame-import-1030).
 *
 * The player copies their own b1.7.3.jar to sd:/beta/ (any *.jar there is
 * accepted) and optionally the .minecraft "resources" folder to
 * sd:/beta/resources/. The importer writes a pack equal to the PC Pack
 * Helper's output into a staging folder, validates it, then swaps it into
 * texturepacks/Beta-1.7.3/ and selects it. Nothing Mojang ships is bundled.
 *
 * Owners: A = this header, beta_import.c, beta_import_jar.c;
 *         B = beta_import_sound.c (bi_sounds_import);
 *         C = the Texture Packs button + progress screen.
 *
 * Threading: beta_import_start() spawns one low-priority worker (below the
 * server thread). beta_import_poll() and beta_import_cancel() are called from
 * the main thread and never block. Only the worker touches the SD card. */

#ifndef BETA_IMPORT_H
#define BETA_IMPORT_H

#include <stdbool.h>

typedef enum {
	BI_IDLE,
	BI_SCANNING,
	BI_TEXTURES,
	BI_SOUNDS,
	BI_SWAPPING,
	BI_DONE,
	BI_FAILED,
	BI_CANCELLED
} bi_state;

typedef struct {
	bi_state state;
	int done, total;      /* current phase progress; total 0 = unknown */
	char msg[128];        /* plain-words status / failure reason */
	bool sounds_imported; /* any sound file written */
	int sounds_count;
} bi_progress;

/* A jar exists in sd:/beta/ (b1.7.3.jar preferred, else any *.jar). */
bool beta_import_available(void);
/* Starts the worker. Returns false if an import is already running. */
bool beta_import_start(void);
/* Main thread, non-blocking snapshot of the worker's progress. */
void beta_import_poll(bi_progress* out);
/* Request cancel; the worker stops at the next entry/file boundary, removes
 * staging, and leaves the previously selected pack untouched. */
void beta_import_cancel(void);

/* Selecting the pack reloads textures, which only the main thread may do and
 * whose in-game deferral policy lives in the Texture Packs screen. The screen
 * registers its texture-apply here; beta_import_poll() calls it ONCE, on the
 * main thread, after the swap finished (never before, so a half pack can
 * never be selected). `pack_value` is "texturepacks/Beta-1.7.3" (the folder;
 * the list shows "Beta 1.7.3" from the pack's manifest). Return true
 * when the selection was applied or queued. With no selector registered the
 * import still finishes and msg tells the player to pick the pack. */
typedef bool (*bi_select_fn)(const char* pack_value);
void beta_import_set_selector(bi_select_fn fn);

/* Boot-time power-cut recovery (call once before textures load): finishes or
 * rolls back an interrupted swap and removes stale staging. Never blocks
 * long: at most one rename plus removal of the importer's own temp folders. */
void beta_import_recover(void);

/* ---- internal, shared between A and B ---------------------------------- */

/* Shared progress sink used by the phases. The worker owns the struct;
 * phases update it only through these helpers (they take the lock). */
typedef struct bi_ctx bi_ctx;
void bi_progress_set(bi_ctx* ctx, bi_state state, int done, int total,
					 const char* msg);
void bi_progress_step(bi_ctx* ctx, int done); /* updates done only */
bool bi_cancel_requested(bi_ctx* ctx);

/* (A) Stream the jar's textures into staging_dir/, compose chest/gui_2/anim
 * like the helper, and validate. Returns false with ctx msg set on failure
 * or cancel. */
bool bi_jar_extract_textures(const char* jar, const char* staging_dir,
							 bi_ctx* ctx);

/* (B) Convert resources_dir/ sounds into staging_dir/ exactly like the
 * helper. Returns the number of files written (0 = no sounds found, which is
 * not a failure), or -1 on error/cancel (ctx msg set). */
int bi_sounds_import(const char* resources_dir, const char* staging_dir,
					 bi_ctx* ctx);

#endif
