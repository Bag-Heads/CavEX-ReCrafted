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

#ifndef SERVER_FURNACE_H
#define SERVER_FURNACE_H

#include "../world.h" // w_coord_t

struct server_local;
struct furnace_te;
struct furnace_env;

// The production furnace_env: the Beta smelt-recipe map (furnace_recipes), Beta
// getItemBurnTime fuel durations, and item max-stack. Injected into furnace_tick.
const struct furnace_env* furnace_server_env(void);

// Position-keyed furnace tile-entity table (mirrors chests). register returns the
// slot index (reusing an existing entry for the same pos) or -1 if full. lookup
// returns NULL if none. free clears the slot (the CALLER drops the items first).
int furnace_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						w_coord_t z);
struct furnace_te* furnace_te_lookup(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z);
void furnace_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					 w_coord_t z);

// Once per server tick: advance every registered furnace by one furnace_tick; on a
// burn-state toggle swap the block id (BLOCK_FURNACE <-> BLOCK_FURNACE_LIT) preserving
// the facing metadata; and if a furnace window is open at that position, push the
// changed slots to the client so smelting is visible live.
void furnace_server_tick(struct server_local* s);

// After a chunk's furnaces are inserted from disk (te_store_insert), restore the two
// pieces the on-disk Furnace compound deliberately omits: recompute each furnace's
// currentItemBurnTime from its fuel slot (Beta TileEntityFurnace.readFromNBT) and make
// its world block id agree with the restored burn state (idle vs lit), preserving the
// facing metadata. cx/cz are CHUNK coordinates.
void furnace_server_load_fixup(struct server_local* s, w_coord_t cx, w_coord_t cz);

/* ===== A1b world-explicit API (split-dimensions D9) ======================
 * The world-qualified forms: `w` is the world whose furnace rows are read /
 * claimed / freed / ticked. The (s, ...) names above are the inventoried
 * LEGACY slot-0 adapters (they forward &s->worlds[0]) retained until A1c / C /
 * the D4 callback conversion consume them; new code uses these. */
struct server_world;
int furnace_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
						  w_coord_t z);
struct furnace_te* furnace_te_lookup_w(struct server_world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z);
void furnace_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
					   w_coord_t z);
void furnace_server_tick_w(struct server_world* w);
void furnace_server_load_fixup_w(struct server_world* w, w_coord_t cx,
								 w_coord_t cz);

#endif
