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

#ifndef SERVER_NOTEBLOCK_H
#define SERVER_NOTEBLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "../world.h" // w_coord_t

struct server_local;

// Position-keyed note-block tile-entity table (mirrors dispenser_te_register/
// lookup/free in server_dispenser.c, which itself mirrors the furnace/chest
// tables). register returns the slot index (reusing an existing entry for the
// same pos) or -1 if full; a freshly registered slot's note starts at 0 and
// prev_powered starts false (Beta TileEntityNote()'s field defaults). lookup
// returns NULL if none. free clears the slot.
int noteblock_te_register(struct server_local* s, w_coord_t x, w_coord_t y,
						  w_coord_t z);
// Returns a pointer to the persisted 0..24 note value, or NULL if no TE is
// registered at (x,y,z).
int8_t* noteblock_te_lookup(struct server_local* s, w_coord_t x, w_coord_t y,
							w_coord_t z);
// Returns a pointer to the LIVE-ONLY previousRedstoneState sidecar, or NULL if
// no TE is registered at (x,y,z). Never persisted (see tile_entities_nbt.h's
// te_note / tile_entities_store.h's note_prev_powered).
bool* noteblock_prev_powered_lookup(struct server_local* s, w_coord_t x,
									w_coord_t y, w_coord_t z);
void noteblock_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z);

/* A1b world-explicit API (split-dimensions D9); the (s, ...) forms above are
 * the inventoried LEGACY slot-0 adapters. */
struct server_world;
int noteblock_te_register_w(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z);
int8_t* noteblock_te_lookup_w(struct server_world* w, w_coord_t x, w_coord_t y,
							  w_coord_t z);
bool* noteblock_prev_powered_lookup_w(struct server_world* w, w_coord_t x,
									  w_coord_t y, w_coord_t z);
void noteblock_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z);

#endif
