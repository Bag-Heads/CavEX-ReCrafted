/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Internals of the in-game Beta importer (beta_import.h), shared by
 * beta_import.c, beta_import_jar.c and the PC tests. Not a game API. */

#ifndef BETA_IMPORT_INTERNAL_H
#define BETA_IMPORT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "beta_import.h"

#define BI_PACK_NAME "Beta-1.7.3"	   /* folder: no space (rig/FAT safe) */
#define BI_PACK_DISPLAY "Beta 1.7.3"   /* manifest "name": what the list shows */
#define BI_LEGACY_NAME "Beta 1.7.3"	   /* earlier helper/importer folder */
#define BI_LEGACY_HIDDEN ".beta-import-legacy"
#define BI_MANIFEST ".cavewii-pack.json" /* helper core.MANIFEST_NAME */
#define BI_STAGING_NAME ".beta-import-tmp" /* inside texturepacks/ (hidden) */
#define BI_OLD_NAME ".beta-import-old"	   /* previous pack during the swap */
#define BI_PATH_MAX 512

/* Every location the importer touches. The game fills it from the SD root
 * (Wii) or the CWD (PC); tests point it at a temp folder. */
struct bi_paths {
	char beta_dir[BI_PATH_MAX];		 /* sd:/beta */
	char resources_dir[BI_PATH_MAX]; /* sd:/beta/resources */
	char packs_dir[BI_PATH_MAX];	 /* sd:/texturepacks */
	char assets_dir[BI_PATH_MAX];	 /* bundled assets (gui_2, pointer, beta-base) */
};

struct bi_ctx {
	void* lock; /* struct thread_mutex*, NULL = single-threaded (tests) */
	bi_progress p;
	volatile bool cancel;
	const char* assets_dir; /* bundled assets root (bi_paths.assets_dir) */
	/* Test-only: simulate a power cut. The swap stops dead (no cleanup) after
	 * step N (1-based) when > 0; see bi_swap_into_place. */
	int crash_after_step;
};

/* Fills the game's default locations. */
void bi_default_paths(struct bi_paths* out);

/* The synchronous pipeline the worker runs: scan, textures, sounds,
 * manifest, swap. Returns the final state (BI_DONE/BI_FAILED/BI_CANCELLED);
 * ctx->p.msg says why. Selection is NOT done here (main thread, poll). */
bi_state bi_run_import(const struct bi_paths* paths, bi_ctx* ctx);

/* Swap staging into packs_dir/BI_PACK_NAME. Returns false on I/O error
 * (the previous pack is restored when possible). */
bool bi_swap_into_place(const struct bi_paths* paths, bi_ctx* ctx);
/* Recovery for an interrupted import/swap (beta_import_recover's body). */
void bi_recover(const struct bi_paths* paths);
/* After the new pack is selected: hide an earlier "Beta 1.7.3" import (only
 * one with a manifest) by renaming it to BI_LEGACY_HIDDEN; bi_recover
 * deletes it. Returns true when it renamed something. */
bool bi_retire_legacy(const struct bi_paths* paths);

/* Finds the jar: beta_dir/b1.7.3.jar, else the first *.jar (sorted by name
 * so the choice is stable). */
bool bi_find_jar(const struct bi_paths* paths, char* out, size_t out_len);

/* Recursive delete of a folder the importer owns. */
bool bi_rmtree(const char* path);
bool bi_mkdirs(const char* path);

/* ---- SHA-256 (manifest; helper writes sha256 per file) ----------------- */
typedef struct {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t fill;
} bi_sha256;
void bi_sha256_init(bi_sha256* s);
void bi_sha256_update(bi_sha256* s, const void* data, size_t n);
void bi_sha256_final(bi_sha256* s, uint8_t out[32]);

/* ---- streaming zip reader (beta_import_jar.c) -------------------------- */
enum bi_zip_err {
	BI_ZIP_OK = 0,
	BI_ZIP_IO,			/* read/seek failed */
	BI_ZIP_NOT_ZIP,		/* no end-of-central-directory record */
	BI_ZIP_ZIP64,		/* zip64 / multi-disk: not a Beta jar */
	BI_ZIP_BAD_CD,		/* central directory malformed */
	BI_ZIP_TOO_BIG,		/* limits (entries, CD size, entry size, ratio) */
	BI_ZIP_ENCRYPTED,
	BI_ZIP_METHOD,		/* not store/deflate */
	BI_ZIP_BAD_LOCAL,	/* local header missing/mismatched */
	BI_ZIP_CORRUPT,		/* inflate error, size or CRC mismatch */
	BI_ZIP_SINK,		/* sink refused (write error) */
	BI_ZIP_CANCELLED,
};

typedef struct bi_zip bi_zip;
typedef struct {
	char name[256];
	uint16_t flags, method;
	uint32_t crc, csize, usize, local_offset;
} bi_zip_entry;

typedef bool (*bi_zip_sink)(void* user, const uint8_t* data, size_t n);

enum bi_zip_err bi_zip_open(const char* path, bi_zip** out);
void bi_zip_close(bi_zip* z);
size_t bi_zip_count(const bi_zip* z);
bool bi_zip_entry_at(const bi_zip* z, size_t i, bi_zip_entry* out);
/* Streams one entry through `sink` in bounded chunks, verifying the CRC and
 * uncompressed size from the central directory (so data-descriptor entries
 * work). `cancel` may be NULL. */
enum bi_zip_err bi_zip_extract(bi_zip* z, const bi_zip_entry* e,
							   bi_zip_sink sink, void* user,
							   volatile bool* cancel);
const char* bi_zip_err_str(enum bi_zip_err e);

/* The 55 textures a b1.7.3 client jar supplies (helper
 * core.client_jar_textures over conversion_map.json) with their canonical
 * sizes. tests/test_beta_import.py checks this table against the map. */
#define BI_TEXTURE_COUNT 55
struct bi_texture {
	const char* dest;	   /* pack path */
	const char* source[2]; /* jar names (chest: container.png or chest.png) */
	uint16_t w, h;		   /* canonical size */
};
extern const struct bi_texture bi_textures[];

#endif
