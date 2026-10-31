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

#include "server_spawner.h"

#include <string.h>

#include "server_local.h"
#include "server_local_state.h" // full server state (split from server_local.h)

// --- position-keyed TE table (mirrors dispenser_te_register/lookup/free) ---

static bool spawner_te_upsert_st(struct te_store st, w_coord_t x, w_coord_t y,
						 w_coord_t z, const char* mob_id) {
	if(!mob_id)
		return false;
	int slot = te_store_claim(&st, TE_KIND_SPAWNER, x, y, z, NULL);
	if(slot < 0)
		return false;
	struct spawner_te* te = &st.spawners[slot];
	memset(te, 0, sizeof(*te));
	size_t n = strlen(mob_id);
	if(n >= sizeof(te->mob_id))
		n = sizeof(te->mob_id) - 1; // defensive truncation (see spawner.h)
	memcpy(te->mob_id, mob_id, n);
	te->mob_id[n] = '\0';
	te->delay = 20; // Beta TileEntityMobSpawner() ctor default [740c583]
	return true;
}

bool spawner_te_upsert_w(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z, const char* mob_id) {
	return spawner_te_upsert_st(server_world_tile_store(w), x, y, z, mob_id);
}

static struct spawner_te* spawner_te_lookup_st(struct te_store st, w_coord_t x,
									   w_coord_t y, w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_SPAWNER, x, y, z);
	return i < 0 ? NULL : &st.spawners[i];
}

struct spawner_te* spawner_te_lookup_w(struct server_world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z) {
	return spawner_te_lookup_st(server_world_tile_store(w), x, y, z);
}

static void spawner_te_free_st(struct te_store st, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	int i = te_store_find(&st, TE_KIND_SPAWNER, x, y, z);
	if(i < 0)
		return;
	te_store_free_row(&st, TE_KIND_SPAWNER, i);
	memset(&st.spawners[i], 0, sizeof(st.spawners[i]));
}

void spawner_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
					   w_coord_t z) {
	spawner_te_free_st(server_world_tile_store(w), x, y, z);
}

/* LEGACY slot-0 adapters (A1b inventory; consumers: tests, item_record.c,
 * the D4 callback bridge). New code uses the _w forms. */
bool spawner_te_upsert(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z, const char* mob_id) {
	return s && spawner_te_upsert_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z, mob_id);
}

struct spawner_te* spawner_te_lookup(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z) {
	return s ? spawner_te_lookup_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z) : NULL;
}

void spawner_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					 w_coord_t z) {
	if(s)
		spawner_te_free_st(server_local_tile_store_in(s, &s->worlds[0]), x, y, z);
}
