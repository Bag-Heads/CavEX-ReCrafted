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

#ifndef SERVER_SPAWNER_H
#define SERVER_SPAWNER_H

#include <stdbool.h>

#include "../world.h"		// w_coord_t
#include "spawner_mob_id.h" // struct spawner_te (dependency-light; not spawner.h's burst core)

struct server_local;

// FH-17: position-keyed MobSpawner tile-entity table (mirrors
// dispenser_te_register/lookup/free in server_dispenser.c / noteblock_te_
// register/lookup/free in server_noteblock.c). Unlike dispenser/noteblock, a
// spawner TE is never created by a live block-PLACEMENT path -- Beta 1.7.3
// has no way to obtain a mob spawner ITEM in survival, and this port exposes
// no such item either (block_spawner.c's block_item is unreachable from any
// inventory/creative path). Every spawner TE is therefore created by one of
// exactly two writers: dungeon worldgen (server_world.c's quad_apply_spawner)
// or a chunk's saved/migrated NBT (tile_entities_store.c's te_store_insert).
// upsert serves both; a separate "register" (Beta ctor defaults, no data yet)
// is intentionally NOT provided -- there is no live "just placed" caller that
// would need one.

// Create-or-overwrite the TE at (x,y,z): mob_id is copied (truncated to
// SPAWNER_MOB_ID_MAX-1 chars + NUL if longer) and delay is set to Beta's
// TileEntityMobSpawner() constructor default (20 -- see spawner.h; there is
// no prior tick history to persist for either writer). Returns false (no
// mutation) if the table has no free slot and (x,y,z) is not already
// registered.
bool spawner_te_upsert(struct server_local* s, w_coord_t x, w_coord_t y,
					   w_coord_t z, const char* mob_id);
// Returns the TE at (x,y,z), or NULL if none is registered there (an inert
// spawner block -- see spawner_mob_from_id's "" contract).
struct spawner_te* spawner_te_lookup(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z);
// Ownership teardown (block_spawner.c's onBreak): frees the slot so a later
// save never persists a "MobSpawner" tile entity at a position that is no
// longer one. No-op if none is registered at (x,y,z).
void spawner_te_free(struct server_local* s, w_coord_t x, w_coord_t y,
					 w_coord_t z);

/* A1b world-explicit API (split-dimensions D9); the (s, ...) forms above are
 * the inventoried LEGACY slot-0 adapters. */
struct server_world;
bool spawner_te_upsert_w(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z, const char* mob_id);
struct spawner_te* spawner_te_lookup_w(struct server_world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z);
void spawner_te_free_w(struct server_world* w, w_coord_t x, w_coord_t y,
					   w_coord_t z);

#endif
