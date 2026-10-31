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

#include "server_noteblock.h"

#include <stdio.h>

#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// --- position-keyed TE table (mirrors dispenser_te_register/lookup/free) --------

static int noteblock_te_register_st(struct te_store st, w_coord_t x, w_coord_t y,
							w_coord_t z) {
	bool existed;
	int i = te_store_claim(&st, TE_KIND_NOTE, x, y, z, &existed);
	if(i < 0) {
		puts("Too many note blocks (note state dropped)");
		return -1;
	}
	if(!existed) {
		// Beta TileEntityNote()'s field defaults: note=0,
		// previousRedstoneState=false. [decompile 740c583]
		st.notes[i] = 0;
		st.note_prev_powered[i] = false;
	}
	return i;
}

int noteblock_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z) {
	return noteblock_te_register_st(server_world_tile_store(w), x, y, z);
}

static int8_t* noteblock_te_lookup_st(struct te_store st, w_coord_t x, w_coord_t y,
							  w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_NOTE, x, y, z);
	return i < 0 ? NULL : &st.notes[i];
}

int8_t* noteblock_te_lookup_w(struct server_world* w, w_coord_t x, w_coord_t y,
							  w_coord_t z) {
	return noteblock_te_lookup_st(server_world_tile_store(w), x, y, z);
}

/* The LIVE-ONLY previousRedstoneState sidecar follows its owned row: a
 * same-coordinate note block in the other world has its own. */
static bool* noteblock_prev_powered_lookup_st(struct te_store st, w_coord_t x,
									  w_coord_t y, w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_NOTE, x, y, z);
	return i < 0 ? NULL : &st.note_prev_powered[i];
}

bool* noteblock_prev_powered_lookup_w(struct server_world* w, w_coord_t x,
									  w_coord_t y, w_coord_t z) {
	return noteblock_prev_powered_lookup_st(server_world_tile_store(w), x, y, z);
}

static void noteblock_te_free_st(struct te_store st, w_coord_t x, w_coord_t y,
						 w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_NOTE, x, y, z);
	if(i < 0)
		return;
	te_store_free_row(&st, TE_KIND_NOTE, i);
	st.notes[i] = 0;
	st.note_prev_powered[i] = false;
}

void noteblock_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z) {
	noteblock_te_free_st(server_world_tile_store(w), x, y, z);
}

/* LEGACY slot-0 adapters (A1b inventory; consumers: tests, item_record.c,
 * the D4 callback bridge). New code uses the _w forms. */
int noteblock_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	return noteblock_te_register_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

int8_t* noteblock_te_lookup(struct server_local* s, w_coord_t x, w_coord_t y,
							w_coord_t z) {
	return noteblock_te_lookup_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

bool* noteblock_prev_powered_lookup(struct server_local* s, w_coord_t x,
									w_coord_t y, w_coord_t z) {
	return noteblock_prev_powered_lookup_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

void noteblock_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	noteblock_te_free_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}
