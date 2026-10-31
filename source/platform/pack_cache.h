/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Pack cache: the GENERIC publication layer shared by the audio slice
	(audio_cache.c) and the texture slice (tex_cache.c). pack-cache-0927;
	Astra astra-pack-0926 item 4, astra-packcacheB-0927, astra-packkey-0927.

	Per cache directory:
	  gNNNNNNNN.bin  one generation: a slice-specific body followed by an
	                 8-byte little-endian FNV-1a 64 of every preceding byte
	  commit-a.rec   commit record slot A  } the newest VALID record names
	  commit-b.rec   commit record slot B  } the generation to use

	PUBLICATION (libfat rename is not assumed atomic): the generation file is
	written completely first; only then is a commit record written into the
	slot that does NOT hold the current record. A torn record fails its own
	checksum and is ignored, leaving the other slot -- the previous valid
	generation, which is kept -- in force. Generation numbers are never
	reused: new = 1 + the highest number seen in either record or any
	gNNNNNNNN.bin. Every integer is serialized explicitly little-endian.
*/

#ifndef PACK_CACHE_H
#define PACK_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PACK_CACHE_FNV_INIT UINT64_C(0xcbf29ce484222325)
uint64_t pack_cache_fnv(uint64_t h, const void* data, size_t n);
uint64_t pack_cache_fnv_u32(uint64_t h, uint32_t v);

/* Fable M3 (pack-cache review 2026-09-27): the source-tree identity every
   cache key folds in (the tools/worktree-stamp.sh part of CW_BUILD_ID: short
   git hash + dirty-tree digest, without the date/-dev suffix), so a build with
   ANY change to the load/convert code never serves an older build's cache. */
uint64_t pack_cache_fold_code_identity(uint64_t h);
#ifdef CAVEWII_TESTING
void pack_cache_test_set_code_identity(const char* id); /* NULL = the build's */
#endif

void pack_cache_put_u32(uint8_t* p, uint32_t v);
void pack_cache_put_u64(uint8_t* p, uint64_t v);
uint32_t pack_cache_get_u32(const uint8_t* p);
uint64_t pack_cache_get_u64(const uint8_t* p);

struct pack_cache_record {
	bool valid;
	uint32_t generation;
	uint64_t key;
	uint32_t file_len;
	uint64_t file_checksum;
};

/* Both commit slots; out[0] = slot A, out[1] = slot B (invalid = absent,
   torn, foreign or corrupt). `schema` must match the record's. */
void pack_cache_records(const char* dir, uint32_t schema,
						struct pack_cache_record out[2]);
/* The two slot indices ordered newest valid first (an invalid slot last). */
void pack_cache_order(const struct pack_cache_record r[2], int order[2]);
void pack_cache_gen_path(char* out, size_t size, const char* dir, uint32_t gen);
bool pack_cache_ensure_dir(const char* dir);

/* ---- writing one generation ---- */
struct pack_cache_txn {
	char dir[400];
	char path[512];
	FILE* f;
	uint32_t schema;
	uint32_t generation;
	int cur_slot; /* slot holding the current record, -1 = none */
	uint32_t prev_generation;
	uint64_t h;
	uint64_t n;
	bool ok;
};
/* Picks the new generation number and opens its file. False = caching
   unavailable (unwritable/full card, mkdir failure). */
bool pack_cache_begin(struct pack_cache_txn* t, const char* dir,
					  uint32_t schema);
void pack_cache_put(struct pack_cache_txn* t, const void* data, size_t n);
/* Appends the checksum, closes, commits the record for `key`, prunes all
   but the new and the previous generation. False = nothing published (the
   partial file is removed; the previous record stays in force). */
bool pack_cache_commit(struct pack_cache_txn* t, uint64_t key);
void pack_cache_abort(struct pack_cache_txn* t);

/* ---- reading: the committed file behind a record ---- */
/* Opens the generation file a valid record names and checks its length
   against the record. The caller streams the body with pack_cache_read
   (which hashes it) and ends with pack_cache_read_finish, which checks the
   trailing checksum against the running hash AND the record. */
struct pack_cache_reader {
	FILE* f;
	uint64_t h;
	uint64_t remaining; /* body bytes left (excludes the 8-byte trailer) */
	const struct pack_cache_record* rec;
};
const char* pack_cache_read_open(struct pack_cache_reader* r, const char* dir,
								 const struct pack_cache_record* rec);
bool pack_cache_read(struct pack_cache_reader* r, void* dst, size_t n);
const char* pack_cache_read_finish(struct pack_cache_reader* r);
void pack_cache_read_close(struct pack_cache_reader* r);

#endif
