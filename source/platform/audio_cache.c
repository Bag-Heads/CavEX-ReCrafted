/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Pack cache, audio slice: the registry image format. Publication, commit
   records, generations and checksums are pack_cache.c's (shared with the
   texture slice); this file only serializes and validates the audio body.
   Contract: audio_cache.h. */

#include "audio_cache.h"

#include <stdlib.h>
#include <string.h>

#include "pack_cache.h"

#define CACHE_MAGIC "CWAC"
#define HEADER_BYTES 56u
#define TABLE_KEY_BYTES (1u + (unsigned)AUDIO_MAX_VARIANTS * 12u)
#define TABLE_BYTES ((unsigned)AUDIO_SOUND_COUNT * TABLE_KEY_BYTES)
#define IO_CHUNK 4096u
#define RATE_MAX 192000u

uint64_t audio_cache_fnv(uint64_t h, const void* data, size_t n) {
	return pack_cache_fnv(h, data, n);
}

bool audio_cache_ensure_dir(const char* dir) {
	return pack_cache_ensure_dir(dir);
}

static const char* read_generation(const char* dir,
								   const struct pack_cache_record* r,
								   uint32_t target,
								   struct audio_cache_image* img,
								   uint8_t* arena, size_t arena_cap) {
	struct pack_cache_reader rd;
	const char* why = pack_cache_read_open(&rd, dir, r);
	if(why)
		return why;
	uint8_t head[HEADER_BYTES];
	uint8_t* table = NULL;
	if(!pack_cache_read(&rd, head, HEADER_BYTES)) {
		why = "short header";
		goto out;
	}
	if(memcmp(head, CACHE_MAGIC, 4) != 0
	   || pack_cache_get_u32(head + 4) != AUDIO_CACHE_SCHEMA) {
		why = "bad magic or schema";
		goto out;
	}
	if(pack_cache_get_u32(head + 8) != target) {
		why = "built for another target";
		goto out;
	}
	if(pack_cache_get_u64(head + 12) != r->key
	   || pack_cache_get_u32(head + 20) != r->generation) {
		why = "header disagrees with its commit record";
		goto out;
	}
	uint32_t used = pack_cache_get_u32(head + 24);
	if(pack_cache_get_u32(head + 28) != AUDIO_SOUND_COUNT
	   || pack_cache_get_u32(head + 32) != AUDIO_MAX_VARIANTS) {
		why = "table shape differs";
		goto out;
	}
	if(used > arena_cap || (used & 31u)
	   || (uint64_t)HEADER_BYTES + TABLE_BYTES + used + 8u != r->file_len) {
		why = "payload length out of bounds";
		goto out;
	}
	memset(img, 0, sizeof(*img));
	img->key = r->key;
	img->target = target;
	img->arena_used = used;
	img->keys_fallback = pack_cache_get_u32(head + 36);
	img->protected_silent = pack_cache_get_u32(head + 40);
	img->protected_short = pack_cache_get_u32(head + 44);
	img->arena_rejects = pack_cache_get_u32(head + 48);

	table = malloc(TABLE_BYTES);
	if(!table) {
		why = "no memory for the table";
		goto out;
	}
	if(!pack_cache_read(&rd, table, TABLE_BYTES)) {
		why = "short table";
		goto out;
	}
	for(unsigned id = 0; id < AUDIO_SOUND_COUNT; id++) {
		const uint8_t* k = table + id * TABLE_KEY_BYTES;
		if(k[0] > AUDIO_MAX_VARIANTS) {
			why = "variant count out of range";
			goto out;
		}
		img->counts[id] = k[0];
		for(unsigned v = 0; v < AUDIO_MAX_VARIANTS; v++) {
			struct audio_cache_variant* e = &img->v[id][v];
			e->offset = pack_cache_get_u32(k + 1 + v * 12);
			e->frames = pack_cache_get_u32(k + 5 + v * 12);
			e->rate = pack_cache_get_u32(k + 9 + v * 12);
			if(v >= k[0])
				continue;
			if((e->offset & 31u) || e->frames == 0 || e->rate == 0
			   || e->rate > RATE_MAX || e->offset > used
			   || (uint64_t)e->frames * 2u > (uint64_t)(used - e->offset)) {
				why = "variant entry out of bounds";
				goto out;
			}
		}
	}
	for(uint32_t off = 0; off < used;) {
		uint32_t n = used - off < IO_CHUNK ? used - off : IO_CHUNK;
		if(!pack_cache_read(&rd, arena + off, n)) {
			why = "short payload";
			goto out;
		}
		off += n;
	}
	why = pack_cache_read_finish(&rd);
out:
	free(table);
	pack_cache_read_close(&rd);
	return why;
}

enum audio_cache_result audio_cache_read(const char* dir, uint64_t key,
										 uint32_t target,
										 struct audio_cache_image* img,
										 uint8_t* arena, size_t arena_cap,
										 const char** reason) {
	const char* dummy;
	if(!reason)
		reason = &dummy;
	struct pack_cache_record r[2];
	pack_cache_records(dir, AUDIO_CACHE_SCHEMA, r);
	int order[2];
	pack_cache_order(r, order);
	bool any_valid = false, any_key = false;
	const char* why = NULL;
	for(int i = 0; i < 2; i++) {
		const struct pack_cache_record* rec = &r[order[i]];
		if(!rec->valid)
			continue;
		any_valid = true;
		if(rec->key != key)
			continue; /* another content state: never reused */
		any_key = true;
		why = read_generation(dir, rec, target, img, arena, arena_cap);
		if(!why) {
			*reason = "hit";
			return AUDIO_CACHE_HIT;
		}
	}
	if(any_key) {
		*reason = why;
		return AUDIO_CACHE_MISS_CORRUPT;
	}
	*reason = any_valid ? "content changed" : "no valid commit record";
	return any_valid ? AUDIO_CACHE_MISS_KEY : AUDIO_CACHE_MISS_NONE;
}

bool audio_cache_write(const char* dir, const struct audio_cache_image* img,
					   const uint8_t* arena) {
	struct pack_cache_txn t;
	if(!pack_cache_begin(&t, dir, AUDIO_CACHE_SCHEMA))
		return false;

	uint8_t head[HEADER_BYTES];
	memset(head, 0, sizeof(head));
	memcpy(head, CACHE_MAGIC, 4);
	pack_cache_put_u32(head + 4, AUDIO_CACHE_SCHEMA);
	pack_cache_put_u32(head + 8, img->target);
	pack_cache_put_u64(head + 12, img->key);
	pack_cache_put_u32(head + 20, t.generation);
	pack_cache_put_u32(head + 24, img->arena_used);
	pack_cache_put_u32(head + 28, AUDIO_SOUND_COUNT);
	pack_cache_put_u32(head + 32, AUDIO_MAX_VARIANTS);
	pack_cache_put_u32(head + 36, img->keys_fallback);
	pack_cache_put_u32(head + 40, img->protected_silent);
	pack_cache_put_u32(head + 44, img->protected_short);
	pack_cache_put_u32(head + 48, img->arena_rejects);
	pack_cache_put(&t, head, sizeof(head));

	uint8_t k[TABLE_KEY_BYTES];
	for(unsigned id = 0; id < AUDIO_SOUND_COUNT; id++) {
		memset(k, 0, sizeof(k));
		k[0] = img->counts[id];
		for(unsigned v = 0; v < AUDIO_MAX_VARIANTS; v++) {
			pack_cache_put_u32(k + 1 + v * 12, img->v[id][v].offset);
			pack_cache_put_u32(k + 5 + v * 12, img->v[id][v].frames);
			pack_cache_put_u32(k + 9 + v * 12, img->v[id][v].rate);
		}
		pack_cache_put(&t, k, sizeof(k));
	}

	/* Payload in the TARGET's sample order. */
	bool swap = img->target != AUDIO_CACHE_HOST_TARGET;
	static uint8_t chunk[IO_CHUNK];
	for(uint32_t off = 0; off < img->arena_used && t.ok;) {
		uint32_t n = img->arena_used - off < IO_CHUNK ? img->arena_used - off
													  : IO_CHUNK;
		memcpy(chunk, arena + off, n);
		if(swap)
			for(uint32_t b = 0; b + 1 < n; b += 2) {
				uint8_t x = chunk[b];
				chunk[b] = chunk[b + 1];
				chunk[b + 1] = x;
			}
		pack_cache_put(&t, chunk, n);
		off += n;
	}
	return pack_cache_commit(&t, img->key);
}
