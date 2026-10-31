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

/*
	CLIENT-side per-position sign-text cache. Answers "what four lines does
	the sign at (x,y,z) show?" for the world renderer -- today that data lives
	only server-side (server_local's sign_texts table) and in tile-entity NBT;
	nothing client-side was queryable per position before this store.

	CLIENT-THREAD-ONLY. Every writer here is a clin_process handler (world
	load / edit echo / break / multiplayer inbound, all client_interface.c /
	server_remote.c) and the only reader is the render pass -- both run on the
	client/render thread. No lock: never call this from the server thread.

	SIGN_TEXT_LINES/SIGN_TEXT_COLS mirror the engine's te_sign packed-grid
	convention exactly (tile_entities_nbt.h TE_SIGN_LINE_COLS/TE_SIGN_SIZE,
	item/inventory.h SIGN_LINE_COLS/SIGN_SIZE) -- kept in _Static_assert
	lockstep in sign_text_store.c, which already needs tile_entities_nbt.h for
	that reason. This header stays dependency-light (no cNBT/items.h include),
	following tile_entities_nbt.h's own SIGN_LINE_COLS duplication precedent
	(the same fact is independently duplicated in item/inventory.h and
	server_remote.c's SIGN_EDIT_COLS already).
*/

#ifndef SIGN_TEXT_STORE_H
#define SIGN_TEXT_STORE_H

#include <stdbool.h>
#include <stddef.h> // size_t -- devkitPPC's newlib does NOT pull this in
				   // transitively the way the PC toolchain does, so omitting
				   // it builds fine on PC and breaks the Wii build only.
#include <stdint.h>

#define SIGN_TEXT_LINES 4
#define SIGN_TEXT_COLS 16 /* packed grid, right-space-padded, no NULs -- the
                            * engine's te_sign convention (see
                            * tile_entities_nbt.h TE_SIGN_LINE_COLS; kept in
                            * _Static_assert lockstep in sign_text_store.c) */

void sign_text_store_init(void);
void sign_text_store_clear(void); /* world unload/reset */
void sign_text_store_set(int32_t x, int32_t y, int32_t z,
						 const char grid[64]);
void sign_text_store_remove(int32_t x, int32_t y, int32_t z); /* sign broken */
bool sign_text_store_get(int32_t x, int32_t y, int32_t z, char out_grid[64]);

// NOT part of the fixed renderer contract above (init/clear/set/remove/get).
// A sixth entry point for the chunk-UNLOAD feed path only: removes every sign
// whose (x,z) falls in chunk (chunk_x, chunk_z) -- CHUNK-INDEX coordinates
// (already divided by CHUNK_SIZE), the same convention CRPC_UNLOAD_CHUNK's
// payload and world_unload_section already use. Without this, a sign broken
// while its owning chunk is unloaded would leave a stale entry that nothing
// else ever corrects (a reload only re-sends EXISTING signs -- see
// sign_text_store.c's module doc for the reasoning). A renderer-only caller
// never needs this.
void sign_text_store_clear_chunk(int32_t chunk_x, int32_t chunk_z);

// Hard ceiling on one collect call's `cap` (the collector's own scratch
// array is sized to this, so a larger cap is clamped, never overrun).
#define SIGN_TEXT_COLLECT_MAX 32

// One collected sign, ready for the render feed (sign_text_render_feed.c).
struct sign_text_record {
	int32_t x, y, z;
	char grid[SIGN_TEXT_LINES * SIGN_TEXT_COLS];
};

// Integration entry point (2026-08-20 wave 3): the renderer's draw pass owns
// its own culling policy but the store owns the data, so enumeration lives
// here -- BOUNDED by `cap` and by `radius` (blocks, squared-distance test
// against the sign's block centre). Returns how many records were written.
// The bound is deliberate: an unbounded walk of a large world's signs would
// put an O(all signs) scan on the render thread every frame; a bounded
// nearest-first-wins collect keeps the per-frame cost flat, and text is
// unreadable past a few blocks anyway (Beta's own sign text fades out with
// ordinary distance culling). When more than `cap` signs are in range the
// EXTRA ones are dropped silently by distance order -- the caller logs the
// policy, not this function.
size_t sign_text_store_collect_near(float cx, float cy, float cz,
									float radius,
									struct sign_text_record* out, size_t cap);

/* Split-dims B2: sign-text STORES, one per client world payload. Every call
 * above operates on the SELECTED store; the primary store is static and
 * selected by default, so code that never selects another store is
 * unchanged. The client owner selects a payload's store around that world's
 * work and restores the previous one (select returns it). Owner thread
 * only. destroy refuses the primary and the selected store. */
struct sign_text_store;
struct sign_text_store* sign_text_store_primary(void);
struct sign_text_store* sign_text_store_create(void);
void sign_text_store_destroy(struct sign_text_store* store);
struct sign_text_store* sign_text_store_select(struct sign_text_store* store);
size_t sign_text_store_count(struct sign_text_store* store);

#endif
