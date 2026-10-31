/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "server_dispenser.h"

#include <stdio.h>

#include "../block/dispenser_logic.h"
#include "../util.h" // rand_gen_seed
#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// --- position-keyed TE table (mirrors furnace_te_register/lookup/free) -----------

static int dispenser_te_register_st(struct te_store st, w_coord_t x, w_coord_t y,
							w_coord_t z) {
	bool existed;
	int i = te_store_claim(&st, TE_KIND_DISPENSER, x, y, z, &existed);
	if(i < 0) {
		puts("Too many dispensers (dispenser state dropped)");
		return -1;
	}
	if(!existed) {
		st.dispensers[i] = (struct dispenser_te) {0};
		// Beta `new Random()` at TileEntityDispenser construction --
		// non-deterministic per real play. seed 0 would zero-lock the
		// xorshift (rand_gen_seed's own |1 guard), so every fresh slot
		// MUST be seeded here, not left at the {0} above.
		rand_gen_seed(&st.dispensers[i].dispenser_random);
	}
	return i;
}

int dispenser_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z) {
	return dispenser_te_register_st(server_world_tile_store(w), x, y, z);
}

static struct dispenser_te* dispenser_te_lookup_st(struct te_store st, w_coord_t x, w_coord_t y,
										   w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_DISPENSER, x, y, z);
	return i < 0 ? NULL : &st.dispensers[i];
}

struct dispenser_te* dispenser_te_lookup_w(struct server_world* w, w_coord_t x, w_coord_t y,
										   w_coord_t z) {
	return dispenser_te_lookup_st(server_world_tile_store(w), x, y, z);
}

static void dispenser_te_free_st(struct te_store st, w_coord_t x, w_coord_t y,
						 w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_DISPENSER, x, y, z);
	if(i < 0)
		return;
	te_store_free_row(&st, TE_KIND_DISPENSER, i);
	st.dispensers[i] = (struct dispenser_te) {0};
}

void dispenser_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z) {
	dispenser_te_free_st(server_world_tile_store(w), x, y, z);
}

// Block -> chunk coordinate, floor division (correct for negatives). 16 ==
// CHUNK_SIZE; inlined (like furnace_server_load_fixup / tile_entities_store.c)
// so this needn't pull chunk.h's display headers.
static int dispenser_chunk_of(w_coord_t b) {
	return b < 0 ? (b + 1) / 16 - 1 : b / 16;
}

void dispenser_server_load_fixup_w(struct server_world* w, w_coord_t cx,
								   w_coord_t cz) {
	struct te_store st = server_world_tile_store(w);
	for(int i = 0; i < st.max_dispenser; i++) {
		if(!te_store_row_owned(&st, TE_KIND_DISPENSER, i))
			continue;
		struct te_pos* p = &st.dispenser_pos[i];
		if(dispenser_chunk_of(p->x) != cx || dispenser_chunk_of(p->z) != cz)
			continue;
		rand_gen_seed(&st.dispensers[i].dispenser_random);
	}
}

/* LEGACY slot-0 adapters (A1b inventory; consumers: tests, item_record.c,
 * the D4 callback bridge). New code uses the _w forms. */
int dispenser_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	return dispenser_te_register_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

struct dispenser_te* dispenser_te_lookup(struct server_local* s, w_coord_t x,
										 w_coord_t y, w_coord_t z) {
	return dispenser_te_lookup_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

void dispenser_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	dispenser_te_free_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

void dispenser_server_load_fixup(struct server_local* s, w_coord_t cx,
								 w_coord_t cz) {
	dispenser_server_load_fixup_w(&s->worlds[0], cx, cz);
}
