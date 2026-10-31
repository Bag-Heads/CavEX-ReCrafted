/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "sign_text_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <m-lib/m-dict.h>

// Needed ONLY for the TE_SIGN_LINE_COLS/TE_SIGN_SIZE _Static_assert lockstep
// promised by sign_text_store.h -- no function or struct from this header is
// otherwise used, so pulling it in never drags cNBT/items.c into anything
// that links this translation unit (headers-only use costs nothing at link
// time).
#include "../network/tile_entities_nbt.h"

_Static_assert(SIGN_TEXT_COLS == TE_SIGN_LINE_COLS,
			   "sign_text_store's packed-grid column width must match the "
			   "engine's te_sign convention (tile_entities_nbt.h "
			   "TE_SIGN_LINE_COLS)");
_Static_assert(SIGN_TEXT_LINES * SIGN_TEXT_COLS == TE_SIGN_SIZE,
			   "sign_text_store's 64-byte grid must match te_sign.text's "
			   "TE_SIGN_SIZE");

// Block -> chunk coordinate, floor division (correct for negatives). Mirrors
// chunk.h's WCOORD_CHUNK_OFFSET; redefined here so this dependency-light,
// client-thread-only unit needn't pull chunk.h (which drags in the platform
// display headers) -- same reasoning as tile_entities_store.c's te_chunk_of.
#define SIGN_STORE_CHUNK_SIZE 16
static int32_t sign_store_chunk_of(int32_t b) {
	return b < 0 ? (b + 1) / SIGN_STORE_CHUNK_SIZE - 1 : b / SIGN_STORE_CHUNK_SIZE;
}

struct sign_text_entry {
	// Kept alongside the packed dict key (below) so sign_text_store_clear_chunk
	// (the chunk-UNLOAD feed path) can test chunk membership without inverting
	// the packed key -- see SIGN_POS_ID's own comment for why that inversion
	// would be lossy.
	int32_t x, y, z;
	char grid[SIGN_TEXT_LINES * SIGN_TEXT_COLS];
};

// Pack a block position into one 64-bit dict key, mirroring chunk.h's
// CHUNK_TO_ID / world.h's SECTION_TO_ID / server_world.h's S_CHUNK_ID: mask
// the UNSIGNED reinterpretation before shifting, since shifting a negative
// signed value is UB. Unlike those 2D chunk keys, this packs a full 3D BLOCK
// position -- y keeps 8 bits (WORLD_HEIGHT is 128, so this is exact headroom
// with room to spare), leaving x/z 28 bits apiece (+-134,217,728), far beyond
// Beta's own +-30,000,000 world border. This encoding is lossy for a
// coordinate outside that range (mirroring CHUNK_TO_ID's own 30-bit x/z
// truncation), which is why sign_text_entry above ALSO keeps the untouched
// x/y/z rather than ever reconstructing them from the key.
#define SIGN_POS_ID(x, y, z)                                                   \
	((int64_t)((((uint64_t)((uint32_t)(z) & 0xFFFFFFFu)) << 36)                \
			   | (((uint64_t)((uint32_t)(x) & 0xFFFFFFFu)) << 8)               \
			   | ((uint64_t)((uint32_t)(y) & 0xFFu))))

DICT_DEF2(dict_sign_text, int64_t, M_BASIC_OPLIST, struct sign_text_entry,
		  M_POD_OPLIST)

/* Split-dims B2: one sign-text STORE per client world payload. Every legacy
 * sign_text_store_* entry point operates on the SELECTED store; the primary
 * store is static and selected by default, so a process that never selects
 * another store behaves exactly as the old file-scope dictionary did. */
struct sign_text_store {
	dict_sign_text_t dict;
	bool ready;
};
static struct sign_text_store s_sign_text_primary;
static struct sign_text_store* s_sign_store = &s_sign_text_primary;
#define s_sign_text (s_sign_store->dict)
// m-lib's own dictionary contract (M_D1CT_CONTRACT) asserts index/data are
// non-NULL on every operation -- a plain zero-initialized (BSS) dict fails
// that on its very first use. main.c calls sign_text_store_init() once at
// startup, matching gstate.entities' dict_entity_init precedent, but this
// module cannot enforce that every OTHER translation unit that might reach
// these entry points (a future caller, or an existing test that routes a
// sign CRPC through the real clin_process without ever running main())
// always does so after that call. Guard every entry point instead of
// trusting call order across files this module doesn't control: cheap (one
// branch), and turns a would-be assert-crash/NULL-deref into a correct,
// idempotent lazy init.

static void sign_text_store_ensure_ready(void) {
	if(!s_sign_store->ready) {
		dict_sign_text_init(s_sign_text);
		s_sign_store->ready = true;
	}
}

void sign_text_store_init(void) {
	sign_text_store_ensure_ready();
}

void sign_text_store_clear(void) {
	sign_text_store_ensure_ready();
	dict_sign_text_reset(s_sign_text);
}

void sign_text_store_set(int32_t x, int32_t y, int32_t z,
						 const char grid[64]) {
	sign_text_store_ensure_ready();
	struct sign_text_entry entry = {.x = x, .y = y, .z = z};
	memcpy(entry.grid, grid, sizeof(entry.grid));
	dict_sign_text_set_at(s_sign_text, SIGN_POS_ID(x, y, z), entry);
}

void sign_text_store_remove(int32_t x, int32_t y, int32_t z) {
	sign_text_store_ensure_ready();
	dict_sign_text_erase(s_sign_text, SIGN_POS_ID(x, y, z));
}

bool sign_text_store_get(int32_t x, int32_t y, int32_t z, char out_grid[64]) {
	sign_text_store_ensure_ready();
	struct sign_text_entry* entry
		= dict_sign_text_get(s_sign_text, SIGN_POS_ID(x, y, z));
	if(!entry)
		return false;
	memcpy(out_grid, entry->grid, sizeof(entry->grid));
	return true;
}

size_t sign_text_store_collect_near(float cx, float cy, float cz,
									float radius,
									struct sign_text_record* out, size_t cap) {
	if(!out || cap == 0 || radius <= 0.0f)
		return 0;
	sign_text_store_ensure_ready();

	// NEAREST-WINS insertion: keep `out` sorted by squared distance so that
	// when more signs are in range than `cap`, the ones that survive are the
	// closest -- the only selection a reader would notice. A full sort of the
	// whole store would be O(n log n) per frame; this is O(n * cap) with cap
	// small (the caller's stack array), and cap-bounded insertion never
	// touches memory beyond `out`.
	float best[SIGN_TEXT_COLLECT_MAX];
	size_t n = 0;
	const float radius_sq = radius * radius;

	dict_sign_text_it_t it;
	dict_sign_text_it(it, s_sign_text);
	while(!dict_sign_text_end_p(it)) {
		const struct dict_sign_text_pair_s* pair = dict_sign_text_cref(it);
		dict_sign_text_next(it);

		// Block CENTRE, matching how the draw pass anchors its geometry.
		const float dx = ((float)pair->value.x + 0.5f) - cx;
		const float dy = ((float)pair->value.y + 0.5f) - cy;
		const float dz = ((float)pair->value.z + 0.5f) - cz;
		const float d_sq = dx * dx + dy * dy + dz * dz;
		if(d_sq > radius_sq)
			continue;

		size_t limit = cap < SIGN_TEXT_COLLECT_MAX ? cap : SIGN_TEXT_COLLECT_MAX;
		if(n == limit && d_sq >= best[n - 1])
			continue; // farther than everything already kept

		size_t pos = n < limit ? n : limit - 1;
		while(pos > 0 && best[pos - 1] > d_sq) {
			best[pos] = best[pos - 1];
			out[pos] = out[pos - 1];
			pos--;
		}
		best[pos] = d_sq;
		out[pos].x = pair->value.x;
		out[pos].y = pair->value.y;
		out[pos].z = pair->value.z;
		memcpy(out[pos].grid, pair->value.grid, sizeof(out[pos].grid));
		if(n < limit)
			n++;
	}
	return n;
}

void sign_text_store_clear_chunk(int32_t chunk_x, int32_t chunk_z) {
	sign_text_store_ensure_ready();
	// Two-pass: collect matching keys first, erase after. m-lib's dict
	// iterator documents erase-during-iteration as unsafe (an erase can
	// trigger a resize-down, invalidating the iteration order mid-walk).
	// IMP-TE-CAPS: a chunk is no longer limited to 256 signs (Beta has no
	// cap, and the server tables grow), so the fixed scratch batch is
	// REPEATED until a pass finds fewer matches than the batch holds: every
	// matching entry is erased, never just the first 256.
	int64_t matched[TE_SIGN_RECORDS_MAX];
	size_t n;
	do {
		n = 0;
		dict_sign_text_it_t it;
		dict_sign_text_it(it, s_sign_text);
		while(!dict_sign_text_end_p(it) && n < TE_SIGN_RECORDS_MAX) {
			const struct dict_sign_text_pair_s* pair
				= dict_sign_text_cref(it);
			if(sign_store_chunk_of(pair->value.x) == chunk_x
			   && sign_store_chunk_of(pair->value.z) == chunk_z)
				matched[n++] = pair->key;
			dict_sign_text_next(it);
		}
		for(size_t i = 0; i < n; i++)
			dict_sign_text_erase(s_sign_text, matched[i]);
	} while(n == TE_SIGN_RECORDS_MAX);
}

/* ---- Split-dims B2: sign-text stores ---------------------------------- */

struct sign_text_store* sign_text_store_primary(void) {
	return &s_sign_text_primary;
}

struct sign_text_store* sign_text_store_create(void) {
	return calloc(1, sizeof(struct sign_text_store));
}

void sign_text_store_destroy(struct sign_text_store* store) {
	if(!store || store == &s_sign_text_primary || store == s_sign_store)
		return;
	if(store->ready)
		dict_sign_text_clear(store->dict);
	free(store);
}

struct sign_text_store* sign_text_store_select(struct sign_text_store* store) {
	struct sign_text_store* previous = s_sign_store;
	s_sign_store = store ? store : &s_sign_text_primary;
	return previous;
}

size_t sign_text_store_count(struct sign_text_store* store) {
	return store && store->ready ? dict_sign_text_size(store->dict) : 0;
}
