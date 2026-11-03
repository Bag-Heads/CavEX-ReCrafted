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
	The RNG-deterministic core of the dungeon mob-spawner's burst, extracted from
	server_local so it can be exercised headlessly (no client RPC / entity / world
	objects). The block accessor is injected, so the same code runs against the
	live server_world in the game and against a flat buffer in tests/test_spawner.
*/

#ifndef SPAWNER_H
#define SPAWNER_H

#include <stdbool.h>
#include <stdint.h>

#include "../block/blocks_data.h" // w_coord_t, struct block_data, BLOCK_*
// FH-17: struct spawner_te + the Beta EntityId<->engine-id mapping
// (spawner_mob_from_meta/spawner_mob_to_id/spawner_mob_from_id) live in
// spawner_mob_id.h/.c now, split out of this file on purpose -- see that
// header's own comment. Re-included here so every existing "spawner.h"
// caller (server_local.h, server_spawner.h, tile_entities_store.h,
// tests/test_spawner.c) keeps seeing them without changes; region_archive.c
// includes spawner_mob_id.h directly instead, to avoid pulling this file's
// block-registry/RNG dependency into a plain string-table lookup.
#include "spawner_mob_id.h"

struct random_gen;

// Beta TileEntityMobSpawner burst geometry: up to SPAWNER_SPAWN_COUNT mobs per
// fire, each at +-SPAWNER_SPAWN_SPREAD horizontally and +-1 vertically.
#define SPAWNER_SPAWN_COUNT 4
#define SPAWNER_SPAWN_SPREAD 4.0F

// Block accessor: fill *blk with the block at (x,y,z); return false for
// out-of-world / unloaded (treated as "no valid spot here").
typedef bool (*spawner_get_block_fn)(void* ctx, w_coord_t x, w_coord_t y,
									 w_coord_t z, struct block_data* blk);

// A valid mob spot: a 2-tall air gap on a solid floor (opacity>=15 + opaque via
// the global block table). GEOMETRY ONLY -- the Beta getCanSpawnHere LIGHT gate
// (spawner_hostile_light_ok below) is applied separately by spawner_burst, once a
// spot passes this check, exactly as Beta's own TileEntityMobSpawner.updateEntity
// calls setLocationAndAngles (positions the candidate) THEN getCanSpawnHere
// (which rolls light) as two logically-separate steps.
bool spawner_spot_ok(spawner_get_block_fn get, void* ctx, w_coord_t x,
					 w_coord_t y, w_coord_t z);

/* Beta EntityMob.getCanSpawnHere()'s light gate [740c583]:
 *     if (getSavedLightValue(EnumSkyBlock.Sky,x,y,z) > rand.nextInt(32))
 *         return false;
 *     light = getBlockLightValue(x,y,z), i.e. max(rawSky - skylightSubtracted, blockLight);
 *     while thundering, light is re-read with skylightSubtracted forced to 10;
 *     pass when light <= rand.nextInt(8).
 * TWO sequential probability rolls, NOT a flat threshold: roll 1 refuses using the
 * RAW, time-of-day-UNADJUSTED stored sky light (`raw_sky_light`, Chunk.skylightMap's
 * nibble) -- so an open-sky column (raw 15) has up to a 15/32 (~47%) chance of being
 * refused OUTRIGHT regardless of how dark the effective light actually is. Only if
 * that passes does roll 2 run against the day/night-ADJUSTED effective light
 * (max(raw_sky_light - sky_subtract, torch_light) -- exactly World.getBlockLightValue,
 * with `sky_subtract` forced to 10 by the CALLER during a thunderstorm, matching
 * Beta's override): light 0 -> ~100% accept, light 7 -> ~12.5%, light>=8 -> a hard 0%
 * (nextInt(8) can never reach 8). This is the ONE canonical implementation of that
 * gate -- used by the natural hostile spawn passes (server_local.c mob_spawn_spot_ok
 * + the anchor-path duplicate) AND the dungeon mob-spawner burst (spawner_burst
 * below), so a Beta-fidelity fix here can't miss a second copy (the exact class of
 * miss that bit mob_solid_floor/block_is_normal_cube -- see that history). This
 * module has no weather/time dependency; `sky_subtract` is always caller-resolved. */
bool spawner_hostile_light_ok(struct random_gen* rng, int raw_sky_light,
							  int torch_light, int sky_subtract);
/* The same two-roll gate with roll 2 against a caller-resolved getBlockLightValue
 * (`block_light_value` = var4, EntityMob.java:76-82 -- server_local_hostile_spawn_light, which
 * carries the slab/stairs/farmland neighbour max, the rain darkening and the thunder override 10).
 * spawner_hostile_light_ok above is exactly this with block_light_value =
 * max(raw_sky_light - sky_subtract, torch_light), the value for a cell without the neighbour
 * branch. No draw is added or moved: resolving var4 draws nothing. [next-gaps-0924/F1] */
bool spawner_hostile_light_value_ok(struct random_gen* rng, int raw_sky_light,
									int block_light_value);

// One burst around (sx,sy,sz): roll SPAWNER_SPAWN_COUNT random offsets and, for
// each that lands on a valid spot (geometry via spawner_spot_ok, THEN the
// getCanSpawnHere light roll via spawner_hostile_light_ok -- Beta's exact order),
// invoke emit(emit_ctx, x, y, z). A member that fails the light gate is skipped
// (no emit, no crash) and the burst tries its remaining slots, exactly as Beta's
// `for(var8<4)` loop keeps going past a single getCanSpawnHere()==false member.
// Consumes the RNG exactly as Beta's spawner. `sky_subtract` is the caller-resolved
// day/night value (10 during a thunderstorm; see spawner_hostile_light_ok). Returns
// how many spots were emitted.
/* I04: per-member hooks. construct: createEntityInWorld (the member's id and
 * own Random), called at the top of each member before any world draw;
 * can_spawn: getCanSpawnHere's light rolls on THAT Random (raw sky light,
 * the subtract-adjusted light value); emit: spawn the accepted member. */
/* too_many_nearby (nullable): TileEntityMobSpawner.java:66-70 -- after
 * createEntityInWorld and BEFORE the member's position draws, the count of
 * the member's class inside the spawner cell expand(8, 4, 8); >= 6 ENDS the
 * burst (updateDelay(); return), it does not skip to the next member.
 * emit receives the member's exact double position (setLocationAndAngles
 * var11/var13/var15, :72-75), not a block-centred cell. */
/* spot_ok (ci-1101/45, HUNT-14 #2b): the member's EntityLiving.getCanSpawnHere
 * GEOMETRY at its exact setLocationAndAngles position (x, y = feet/bb.minY,
 * z): checkIfAABBIsClear && no colliding block box && !getIsAnyLiquid over
 * the member's own width x height box [EntityLiving.java:781-783]. Beta has
 * NO floor clause and NO "cell is air" clause on this path. The live server
 * ALWAYS supplies it (server_local.c sl_spawner_spot_ok: the same canonical
 * box helpers the natural spawns use). NULL falls back to spawner_spot_ok's
 * legacy cell test -- kept ONLY for the headless block-table-less harness
 * (tests/test_spawner.c), which has no collision boxes to test against. */
struct spawner_member_ops {
	void (*construct)(void* ctx);
	bool (*too_many_nearby)(void* ctx);
	bool (*spot_ok)(void* ctx, double x, double y, double z);
	bool (*can_spawn)(void* ctx, int raw_sky_light, int block_light_value);
	void (*emit)(void* ctx, double x, double y, double z);
};
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* G9 item 3: each burst member's light read (raw sky, block, subtract). */
extern void (*spawner_test_cell_hook)(w_coord_t x, w_coord_t y, w_coord_t z,
									  int raw_sky, int torch, int subtract);
#endif
int spawner_burst(struct random_gen* rng, w_coord_t sx, w_coord_t sy, w_coord_t sz,
				  int sky_subtract, spawner_get_block_fn get, void* get_ctx,
				  const struct spawner_member_ops* ops, void* ctx);

#endif
