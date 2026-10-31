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

#ifndef SERVER_DISPENSER_H
#define SERVER_DISPENSER_H

#include "../world.h" // w_coord_t

struct server_local;
struct dispenser_te;

// Position-keyed dispenser tile-entity table (mirrors furnace_te_register/
// lookup/free in server_furnace.c, which itself mirrors the chest table).
// register returns the slot index (reusing an existing entry for the same
// pos) or -1 if full; a freshly registered slot's dispenser_random is seeded
// here (Beta `new Random()` at TileEntityDispenser construction -- non-
// deterministic per real play, and NOT persisted, so every placement and
// every chunk-load reconstruction gets a fresh seed). lookup returns NULL if
// none. free clears the slot (the CALLER drops the items first).
int dispenser_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						  w_coord_t z);
struct dispenser_te* dispenser_te_lookup(struct server_local* s, w_coord_t x,
										 w_coord_t y, w_coord_t z);
void dispenser_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z);

// After a chunk's dispensers are inserted from disk (te_store_insert), seed
// each restored dispenser_random. Beta's TileEntityDispenser.readFromNBT only
// restores "Items" (dispenserRandom is a fresh `new Random()` on every
// construction, chunk load included) -- mirrors furnace_server_load_fixup's
// slot, minus a burn-state recompute this TE has no equivalent of. cx/cz are
// CHUNK coordinates.
void dispenser_server_load_fixup(struct server_local* s, w_coord_t cx,
								 w_coord_t cz);

/* A1b world-explicit API (split-dimensions D9); the (s, ...) forms above are
 * the inventoried LEGACY slot-0 adapters. */
struct server_world;
int dispenser_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z);
struct dispenser_te* dispenser_te_lookup_w(struct server_world* w,
										   w_coord_t x, w_coord_t y,
										   w_coord_t z);
void dispenser_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z);
void dispenser_server_load_fixup_w(struct server_world* w, w_coord_t cx,
								   w_coord_t cz);

#endif
