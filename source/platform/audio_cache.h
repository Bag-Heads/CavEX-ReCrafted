/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Pre-baked pack cache, AUDIO slice (pack-cache-0927; intent
	docs/intent/pack-cache.md, Astra rulings astra-pack-0926 item 4 and
	astra-packcacheB-0927). OPT-IN: config "pack.cache" (default false). OFF,
	nothing in this module is reached -- no read, no write, no directory.

	One cache = the COMPLETED sound registry of one scan: the arena bytes
	[0, arena_used) and, per key, the variant table as arena OFFSETS (never a
	pointer), plus the scan's own counters. It is valid only for the exact key
	it was built under (audio_registry.c: content digest of every candidate
	file's name and bytes in both roots, schema/converter/policy versions,
	target, arena capacity, alignment, deferred-cave setting).

	ON-DISK LAYOUT, per cache directory (<effective-pack-root>/.cavewii/audio,
	or .cavewii/bundled-audio for the bundled "assets" root):
	  gNNNNNNNN.bin  one generation: header, table, payload, trailing checksum
	  commit-a.rec   commit record slot A  } the newest VALID record names the
	  commit-b.rec   commit record slot B  } generation to use
	Every integer is serialized explicitly little-endian. PCM payload words are
	in the TARGET's sample order (big-endian for the Wii DSP, native for PC),
	named in the header.

	PUBLICATION (libfat rename is not assumed atomic): a new generation file
	is written completely first; only then is a commit record written into the
	slot that does NOT hold the current record. A torn record fails its own
	checksum and is ignored, leaving the other slot -- the previous valid
	generation, which is kept on disk -- in force. A generation number is
	never reused: new = 1 + the highest number seen in either record or any
	gNNNNNNNN.bin in the directory.

	READING validates, before anything is published into the registry: record
	checksums, magic/schema/target/key/generation, the file length the record
	committed, every table entry (variant counts, 32-byte-aligned offsets,
	frames*2 inside arena_used, sane rates, arena_used within the live arena)
	and the payload checksum. Any failure = MISS with a reason; the caller then
	decodes fresh, exactly as with the cache off.
*/

#ifndef AUDIO_CACHE_H
#define AUDIO_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_sound_table.h"

#define AUDIO_CACHE_SCHEMA 1u
/* Bumped whenever the decode/convert path's OUTPUT for the same input could
   change (WAV parse, ogg decode/downmix, sample order). */
#define AUDIO_CACHE_CONVERTER_VERSION 1u
/* Bumped whenever the scan's arena/pass/variant/fallback policy changes. */
#define AUDIO_CACHE_POLICY_VERSION 1u

enum audio_cache_target {
	AUDIO_CACHE_TARGET_PC_LE = 1, /* little-endian host-order samples */
	AUDIO_CACHE_TARGET_WII_BE = 2 /* big-endian samples (DSP DMA order) */
};

#ifdef PLATFORM_WII
#define AUDIO_CACHE_HOST_TARGET AUDIO_CACHE_TARGET_WII_BE
#else
#define AUDIO_CACHE_HOST_TARGET AUDIO_CACHE_TARGET_PC_LE
#endif

struct audio_cache_variant {
	uint32_t offset; /* into the arena, 32-byte aligned */
	uint32_t frames;
	uint32_t rate;
};

struct audio_cache_image {
	uint64_t key;
	uint32_t target;
	uint32_t arena_used;
	uint8_t counts[AUDIO_SOUND_COUNT];
	struct audio_cache_variant v[AUDIO_SOUND_COUNT][AUDIO_MAX_VARIANTS];
	/* The scan's own counters, so the load record reads the same. */
	uint32_t keys_fallback;
	uint32_t protected_silent;
	uint32_t protected_short;
	uint32_t arena_rejects;
};

enum audio_cache_result {
	AUDIO_CACHE_HIT = 0,
	AUDIO_CACHE_MISS_NONE,		/* no valid commit record at all */
	AUDIO_CACHE_MISS_KEY,		/* newest valid record is for other content */
	AUDIO_CACHE_MISS_CORRUPT,	/* a record matched, its generation failed */
};

/* 64-bit FNV-1a, the digest/checksum used throughout. */
#define AUDIO_CACHE_FNV_INIT UINT64_C(0xcbf29ce484222325)
uint64_t audio_cache_fnv(uint64_t h, const void* data, size_t n);

/* Read the newest valid generation whose key equals `key` into `img` and its
   payload into arena[0..img->arena_used). The arena is written ONLY after the
   header and table validated; on a payload checksum failure its contents are
   garbage and the caller must not publish them (the caller rescans, which
   overwrites them). `reason` (optional) gets a short text for the record. */
enum audio_cache_result audio_cache_read(const char* dir, uint64_t key,
										 uint32_t target,
										 struct audio_cache_image* img,
										 uint8_t* arena, size_t arena_cap,
										 const char** reason);

/* Publish a new generation (see PUBLICATION above). `arena` holds HOST-order
   samples; they are written in img->target order. False = caching unavailable
   for this directory (unwritable/full card, mkdir failure): nothing is
   published, the previous generation stays in force, a partial file is
   removed where possible. Never touches the registry. */
bool audio_cache_write(const char* dir, const struct audio_cache_image* img,
					   const uint8_t* arena);

/* mkdir -p for the cache directory; false if it cannot exist. */
bool audio_cache_ensure_dir(const char* dir);

#endif
