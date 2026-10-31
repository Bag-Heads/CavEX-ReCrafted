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

#include "spawner.h"

#include <math.h>

#include "../block/blocks.h" // blocks[], struct block
#include "../util.h"		 // struct random_gen, rand_gen_flt/int
#include "../world.h"		 // WORLD_HEIGHT

// spawner_mob_from_meta/spawner_mob_to_id/spawner_mob_from_id moved to
// spawner_mob_id.c (FH-17) -- see spawner.h's include comment.

// Beta isBlockNormalCube -- THE canonical helper (blocks.c:47), never open-coded.
//
// This was `b->opacity >= 15 && !b->transparent`, the same defect 1398e65 fixed
// in server_local.c's natural-spawn copy and MISSED here: `.opacity` is a
// designated-initializer field that the plain solid blocks never set (stone,
// grass, dirt, cobblestone declare only .transparent/.can_see_through, so it
// defaults to 0). The predicate was therefore false for cobblestone and mossy
// cobblestone -- i.e. for every Beta dungeon floor -- so dungeon mob spawners
// could never place a mob and were 100% dead on top of the world-wide failure.
// Found by adversarial review 2026-07-31; fixing one of two copies is exactly
// the class of miss that review exists for.
static bool solid_floor(uint8_t type) {
	return block_is_normal_cube(type);
}

bool spawner_spot_ok(spawner_get_block_fn get, void* ctx, w_coord_t x,
					 w_coord_t y, w_coord_t z) {
	if(y < 1 || y >= WORLD_HEIGHT - 1)
		return false;
	struct block_data here, above, below;
	if(!get(ctx, x, y, z, &here) || !get(ctx, x, y + 1, z, &above)
	   || !get(ctx, x, y - 1, z, &below))
		return false;
	return here.type == BLOCK_AIR && above.type == BLOCK_AIR
		&& solid_floor(below.type);
}

// See spawner.h's citation for the exact Beta EntityMob.getCanSpawnHere() source
// and roll order. THE canonical implementation -- never re-open-code this (the
// exact class of miss that bit mob_solid_floor's own canonical helper, above).
#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*spawner_test_cell_hook)(w_coord_t x, w_coord_t y, w_coord_t z,
							   int raw_sky, int torch, int subtract) = NULL;
#endif
bool spawner_hostile_light_value_ok(struct random_gen* rng, int raw_sky_light,
									int block_light_value) {
	if(raw_sky_light > rand_gen_int(rng, 32))
		return false; // Beta: open-sky suppression, unadjusted by time of day
	return block_light_value <= rand_gen_int(rng, 8);
}

bool spawner_hostile_light_ok(struct random_gen* rng, int raw_sky_light,
							  int torch_light, int sky_subtract) {
	int sky = raw_sky_light - sky_subtract;
	int eff = (sky > torch_light) ? sky : torch_light;
	return spawner_hostile_light_value_ok(rng, raw_sky_light, eff);
}

int spawner_burst(struct random_gen* rng, w_coord_t sx, w_coord_t sy, w_coord_t sz,
				  int sky_subtract, spawner_get_block_fn get, void* get_ctx,
				  const struct spawner_member_ops* ops, void* ctx) {
	int n = 0;
	for(int i = 0; i < SPAWNER_SPAWN_COUNT; i++) {
		/* I04: createEntityInWorld first (the member's id + own Random)
		 * [CFR TileEntityMobSpawner.java:62]. */
		if(ops->construct)
			ops->construct(ctx);
		/* G6 (i05-1005): the crowd count runs per member right after
		 * createEntityInWorld and before any position draw; >= 6 ENDS the
		 * burst (`updateDelay(); return;`), it never skips to the next
		 * member [CFR TileEntityMobSpawner.java:66-70]. */
		if(ops->too_many_nearby && ops->too_many_nearby(ctx))
			return n;
		/* :72-74, in Java evaluation order: x's two nextDouble, y's
		 * nextInt(3), z's two nextDouble. One stream draw per nextDouble,
		 * scaled by 2^-32 (the server_local.c dispenser nextDouble precedent
		 * on this xorshift stream). */
		double d1 = (double)rand_gen(rng) / 4294967296.0;
		double d2 = (double)rand_gen(rng) / 4294967296.0;
		const double px = (double)sx + (d1 - d2) * (double)SPAWNER_SPAWN_SPREAD;
		const double py = (double)(sy + rand_gen_int(rng, 3) - 1);
		double d3 = (double)rand_gen(rng) / 4294967296.0;
		double d4 = (double)rand_gen(rng) / 4294967296.0;
		const double pz = (double)sz + (d3 - d4) * (double)SPAWNER_SPAWN_SPREAD;
		/* setLocationAndAngles(..., worldObj.rand.nextFloat() * 360.0f, 0)
		 * [:75]: the WORLD yaw draw, before getCanSpawnHere. */
		(void)rand_gen_flt(rng);
		/* EntityMob.getCanSpawnHere reads floor(posX), floor(bb.minY),
		 * floor(posZ) [EntityMob.java:91-92]. */
		w_coord_t x = (w_coord_t)floor(px);
		w_coord_t y = (w_coord_t)floor(py);
		w_coord_t z = (w_coord_t)floor(pz);
		if(!spawner_spot_ok(get, get_ctx, x, y, z))
			continue;
		// Beta TileEntityMobSpawner.updateEntity gates each burst member on
		// getCanSpawnHere() -- for the 3 dungeon-spawnable mobs (Skeleton/
		// Zombie/Spider, all EntityMob) that is THIS two-roll light test, the
		// SAME gate natural hostile spawning uses (2026-08-31 fix: this dungeon
		// path previously had NO light test at all, so a torch-lit/open room
		// spawned mobs 100% of the time). A member that fails it is simply
		// skipped -- no emit, no crash -- and the burst tries its remaining
		// slots, matching Beta's for(var8<4) loop continuing past one refused
		// member (the delay re-roll cadence is unchanged; see server_local.c's
		// spawner_fire/tick_spawners, an intentionally separate concern).
		/* I04: the rolls are the MEMBER's own Random (EntityMob.java:92-
		 * 102), not the world stream: the caller's can_spawn draws them. */
		struct block_data here;
		if(!get(get_ctx, x, y, z, &here))
			continue;
		int sky = (int)here.sky_light - sky_subtract;
		int eff = (sky > (int)here.torch_light) ? sky : (int)here.torch_light;
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(spawner_test_cell_hook)
			spawner_test_cell_hook(x, y, z, (int)here.sky_light,
								   (int)here.torch_light, sky_subtract);
#endif
		if(!ops->can_spawn(ctx, (int)here.sky_light, eff))
			continue;
		ops->emit(ctx, px, py, pz);
		n++;
	}
	return n;
}
