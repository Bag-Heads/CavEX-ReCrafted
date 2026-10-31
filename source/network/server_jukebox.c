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

#include "server_jukebox.h"

#include <stdio.h>

#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// --- position-keyed TE table (mirrors dispenser_te_register/lookup/free) --------

static int jukebox_te_register_st(struct te_store st, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	bool existed;
	int i = te_store_claim(&st, TE_KIND_RECORD_PLAYER, x, y, z, &existed);
	if(i < 0) {
		puts("Too many jukeboxes (record state dropped)");
		return -1;
	}
	if(!existed) // Beta TileEntityRecordPlayer()'s field default: record=0.
		st.records[i] = 0; // [decompile 740c583]
	return i;
}

int jukebox_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	return jukebox_te_register_st(server_world_tile_store(w), x, y, z);
}

static int32_t* jukebox_te_lookup_st(struct te_store st, w_coord_t x, w_coord_t y,
							 w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_RECORD_PLAYER, x, y, z);
	return i < 0 ? NULL : &st.records[i];
}

int32_t* jukebox_te_lookup_w(struct server_world* w, w_coord_t x, w_coord_t y,
							 w_coord_t z) {
	return jukebox_te_lookup_st(server_world_tile_store(w), x, y, z);
}

static void jukebox_te_free_st(struct te_store st, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_RECORD_PLAYER, x, y, z);
	if(i < 0)
		return;
	te_store_free_row(&st, TE_KIND_RECORD_PLAYER, i);
	st.records[i] = 0;
}

void jukebox_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	jukebox_te_free_st(server_world_tile_store(w), x, y, z);
}

/* LEGACY slot-0 adapters (A1b inventory; consumers: tests, item_record.c,
 * the D4 callback bridge). New code uses the _w forms. */
int jukebox_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						w_coord_t z) {
	return jukebox_te_register_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

int32_t* jukebox_te_lookup(struct server_local* s, w_coord_t x, w_coord_t y,
						   w_coord_t z) {
	return jukebox_te_lookup_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}

void jukebox_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					 w_coord_t z) {
	jukebox_te_free_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}
