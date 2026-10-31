/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* colcap-1006 (ASTRA-CC-R5 item 2b): ORDERED GX SCENE HASH.
 *
 * The pool_selftest_3m hw_run profile proves that column-slot placement
 * (lowest-index-first vs the pre-R4 rotating take) cannot change what the GPU
 * is fed: it renders one fixed, settled scene and hashes, IN SUBMISSION
 * ORDER, the exact byte ranges displaylist_render hands to GX_CallDispList
 * (PC: the vertex bytes handed to the GL buffer). The same build is run twice,
 * once per placement, and the two hashes must be identical.
 *
 * Because a cross-run comparison can only blame placement when everything
 * else matched, the frame also carries witnesses that separate a placement
 * failure from an unrelated difference:
 *   gx_ordered       FNV-1a 64 over every submitted byte, list by list, with
 *                    each list's length folded in (the claim);
 *   gx_set           order-independent sum of per-(chunk, pass) list hashes
 *                    (same lists, different order = renderer order, not bytes);
 *   pos_ordered      the (x, y, z, pass) sequence of rendered chunks;
 *   content_ordered  / content_set: every rendered chunk's gathered block
 *                    cells (type, metadata, sky, torch), i.e. the mesher's own
 *                    input -- a world-content difference (random ticks, fluid)
 *                    makes a comparison INCONCLUSIVE, never PASS.
 *
 * Cost when not armed: one load and branch per displaylist_render / per
 * chunk_render. Only pool_selftest.c arms it, for a few frames. */

#ifndef PLATFORM_DISPLAYLIST_SCENE_HASH_H
#define PLATFORM_DISPLAYLIST_SCENE_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DL_SCENE_FNV64_INIT UINT64_C(14695981039346656037)
#define DL_SCENE_FNV64_PRIME UINT64_C(1099511628211)

static inline uint64_t dl_scene_fnv64(uint64_t h, const void* data, size_t n) {
	const uint8_t* p = (const uint8_t*)data;
	for(size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= DL_SCENE_FNV64_PRIME;
	}
	return h;
}

static inline uint64_t dl_scene_fnv64_u32(uint64_t h, uint32_t v) {
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
					(uint8_t)(v >> 24)};
	return dl_scene_fnv64(h, b, sizeof b);
}

/* splitmix64 finaliser: turns a keyed hash into a well-mixed term for the
 * order-independent sums. */
static inline uint64_t dl_scene_mix64(uint64_t z) {
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

struct dl_scene_frame {
	uint64_t gx_ordered, gx_set, pos_ordered, content_ordered, content_set;
	uint64_t bytes;
	uint32_t lists, chunks, content_chunks;
};

/* Read in displaylist_render and chunk_render; written only by the arm/end
 * calls below (main thread, between frames). */
extern volatile bool g_dl_scene_hash_armed;

/* Start accumulating a new frame (arms the hooks). */
void dl_scene_hash_frame_begin(void);
/* Stop accumulating and return the frame's values (disarms). */
void dl_scene_hash_frame_end(struct dl_scene_frame* out);
/* displaylist_render: the exact bytes submitted for one list. */
void dl_scene_hash_list(const void* data, size_t bytes);
/* chunk_render: bracket one chunk's lists for one pass. On the opaque pass
 * the registered content function (if any) hashes the chunk's content. */
void dl_scene_hash_chunk_begin(int32_t x, int32_t y, int32_t z, bool pass,
							   const void* chunk);
void dl_scene_hash_chunk_end(void);
/* One rendered chunk's content hash (called by chunk_begin through the
 * registered function, or directly). */
void dl_scene_hash_content(int32_t x, int32_t y, int32_t z, uint64_t content);
/* The content hash of one `struct chunk` -- main.c registers one that hashes
 * the cells the mesher gathers, so this platform layer and chunk.c stay free
 * of mesher dependencies. NULL = no content witness. */
typedef uint64_t (*dl_scene_content_fn)(const void* chunk);
void dl_scene_hash_set_content(dl_scene_content_fn fn);

#endif
