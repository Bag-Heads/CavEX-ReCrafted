/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/*
	Pre-baked pack cache, TEXTURE slice (pack-cache-0927; Astra
	astra-packcacheB-0927 and astra-packkey-0927). OPT-IN: config
	"pack.cache" (the same key as the audio slice, default false). OFF,
	nothing in this module does anything -- tex_init is byte-identical.

	One cache = the COMPLETED output of one tex_init pack load, for one
	target: every pack texture slot as the backend holds it (Wii: the GX
	texel bytes, including the 4-LOD atlas mip chains; PC: the RGBA image
	the GL upload takes), the sign board colour, both biome colormaps, both
	atlas lookup tables and the live-water atlas capture. A HIT installs all
	of it without decoding a PNG, building an atlas or converting a texel.

	KEY (checked before anything is read): schema, cache policy version,
	GX converter version, target, the slot table (file, format, filter,
	atlas), the atlas table size, whether the pack IS the bundled root, and
	for a HELPER-MANAGED pack its manifest digest (pack_manifest.h). The
	generation additionally carries the list of every other source file the
	load examined -- bundled-default fallbacks, the bundled-only controller
	sheets, and for an unmanaged pack the pack's own files -- with one
	digest over their names, sizes and EVERY byte; a hit re-hashes exactly
	those files. The load is a deterministic function of the files it
	examines, so an unchanged examined set means an identical result. A
	managed pack's files are trusted to its manifest: a manual edit that is
	not re-imported is NOT detected (Astra's documented limit).

	MEMORY: a hit reads and validates the WHOLE generation (every texel
	buffer allocated where the backend's own loader puts it) before it
	installs anything, so for that moment the previous pack's textures and
	the restored ones are both resident. A failed allocation is a miss: the
	fresh load runs, which replaces textures one at a time as before.

	PUBLICATION, commit records and generations: pack_cache.c (shared with
	the audio slice). Directory: <pack>/.cavewii/textures, or
	.cavewii/bundled-textures for the bundled "assets" root.
*/

#ifndef TEXTURE_CACHE_H
#define TEXTURE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tex_gfx;

#define TEX_CACHE_SCHEMA 1u
/* Bumped whenever the load's slot/fallback/atlas/colormap policy changes. */
/* 3: DC-GRASS G2 -- grass atlas cells untinted + tile-38 overlay/item cells
   appended; a cached pre-G2 atlas must never be restored. */
/* 4: BG-BAKE-1028/3 -- menu background: no bundled scenery under a
   non-default pack; author art or the verified generated backdrop only. */
/* 5: raw-jar packs (tex_beta_compose.h) -- gui/container.png chest, composed
   gui_2.png/anim.png; a cached pre-5 load of such a pack holds E&A sheets. */
#define TEX_CACHE_POLICY_VERSION 5u

enum tex_cache_target {
	TEX_CACHE_TARGET_PC = 1, /* RGBA8 images (GL uploads) */
	TEX_CACHE_TARGET_WII = 2 /* GX texel bytes, big-endian */
};

#ifdef PLATFORM_WII
#define TEX_CACHE_HOST_TARGET TEX_CACHE_TARGET_WII
#else
#define TEX_CACHE_HOST_TARGET TEX_CACHE_TARGET_PC
#endif

/* tex_init prologue. True = every slot and side table was restored from
   the cache (the caller skips the fresh load). */
bool tex_cache_try_restore(void);
/* A load site ran for this slot (fresh load only; the order is checked
   against the slot table before anything is published). */
void tex_cache_note_slot(struct tex_gfx* tex);
/* tex_init epilogue of a fresh load, before any FX touches a texture:
   publishes when the cache was on and missed and the load was complete. */
void tex_cache_after_load(bool loaded_ok);

/* Diagnostics of the last tex_init: "off", "bypass", "hit", "miss",
   "written", "unwritable", and the miss reason. */
const char* tex_cache_status(void);
const char* tex_cache_reason(void);
/* Microseconds of the last tex_init's key+dependency check, cache read and
   cache write. */
void tex_cache_timings(unsigned* key_us, unsigned* read_us,
					   unsigned* write_us);

/* HOST TOOL: the next tex_init with the cache on skips the read and, after
   its fresh load, publishes one generation per target in `targets_mask`
   (bit 1 << target). 0 = normal behaviour. Returns nothing; the outcome is
   tex_cache_status() ("written" = every requested target published). */
void tex_cache_tool_targets(uint32_t targets_mask);
/* The slot table (tests / tools): count, the k-th texture object. */
size_t tex_cache_slot_count(void);
struct tex_gfx* tex_cache_slot(size_t k);
#ifdef CAVEWII_TESTING
/* The reader's per-slot shape check (Fable M1 fixture). */
bool tex_cache_test_slot_shape_ok(uint32_t target, size_t k, uint8_t lods,
								  uint32_t width, uint32_t height,
								  uint32_t bytes);
/* Index of a slot's texture object in the slot table, or SIZE_MAX. */
size_t tex_cache_test_slot_index(const struct tex_gfx* tex);
#endif
/* The cache directory of the current pack. */
void tex_cache_dir(char* out, size_t size);

#endif
