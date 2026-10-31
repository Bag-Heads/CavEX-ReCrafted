/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#include "blocks.h"
#include "blocks_data.h"
#include "../entity/entity_primed_tnt.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../network/server_weather.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

// Beta 1.7.3 BlockFire (decompile 740c583), faithfully transcribed. Fire is a SCHEDULED-tick block
// (Beta tickRate 40, setTickOnLoad), NOT a random-tick block: it ages, dies when starved of fuel,
// and probabilistically spreads to flammable blocks and into the air around them. CaveWii adaptations
// (documented deviations, not silent): rand_gen_int is NOT bit-exact to Beta's world.rand.nextInt, so
// the spread is faithful in DISTRIBUTION, not in exact RNG sequence (a byte-exact golden would need a
// true nextInt / mc173 vector). The rain-extinguish branch (weather slice, previously omitted when
// there was no weather sim to check) is now wired via server_weather.h's isRaining/canBlockBeRainedOn.

// --- Beta BlockFire.initializeBlock / setBurnRate: id, encouragement, catch ability ----
// chanceToEncourageFire: per-id strength with which a block ENCOURAGES nearby fire (canBlockCatchFire test +
// the getChanceOfNeighborsEncouragingFire term). abilityToCatchFire: per-id readiness for fire to CONSUME it
// (tryToCatchBlockOnFire compares a bounded random draw with ability). 0 for every unlisted id (never catch / never
// encourage). C99 designated initializers -> const, no lazy-init race; keyed by CaveWii id (== Beta id).
static const uint8_t fire_encourage[256] = {
	[BLOCK_PLANKS] = 5,	  [BLOCK_FENCE] = 5,	  [BLOCK_WOODEN_STAIRS] = 5,
	[BLOCK_LOG] = 5,	  [BLOCK_LEAVES] = 30,	  [BLOCK_BOOKSHELF] = 30,
	[BLOCK_TNT] = 15,	  [BLOCK_TALL_GRASS] = 60, [BLOCK_WOOL] = 30,
};
static const uint8_t fire_catch[256] = {
	[BLOCK_PLANKS] = 20,  [BLOCK_FENCE] = 20,	  [BLOCK_WOODEN_STAIRS] = 20,
	[BLOCK_LOG] = 5,	  [BLOCK_LEAVES] = 60,	  [BLOCK_BOOKSHELF] = 20,
	[BLOCK_TNT] = 100,	  [BLOCK_TALL_GRASS] = 100, [BLOCK_WOOL] = 60,
};


static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockFire.java:31-32: collision is null; inherited selection is full.
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	return TEXTURE_INDEX(3, 0);
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	return 0;
}

// Beta World.getBlockId: an unloaded / out-of-range cell reads as air (id 0).
static int fire_id(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(block_world(s), x, y, z, &b) ? b.type : BLOCK_AIR;
}

// Beta BlockFire.onBlockAdded HEAD, dispatched from the server_world_set_block
// chokepoint on every BLOCK_FIRE write: attempt portal creation only with
// obsidian directly below; on failure continue with survival and scheduling.
// -- the portal attempt runs FIRST, gated on obsidian DIRECTLY below the new
// fire. On success the portal fill has overwritten this fire cell and Beta
// does nothing further; on failure Beta falls through to the fire-survival
// check and scheduling after 40 ticks, which this engine's existing tick machinery
// already performs (with obsidian below, isBlockNormalCube is true, so the
// survival outcome is order-independent -- byte-equivalent either way).
// [decompile 740c583 BlockFire.onBlockAdded:192]
void block_fire_on_added(struct server_local* s, w_coord_t x, w_coord_t y,
						 w_coord_t z) {
	if(fire_id(s, x, y - 1, z) == BLOCK_OBSIDIAN)
		block_portal_try_create(s, x, y, z);
}

// Beta BlockFire.canBlockCatchFire: true when this id has positive encouragement.
static bool fire_can_block_catch(struct server_local* s, int x, int y, int z) {
	return fire_encourage[fire_id(s, x, y, z)] > 0;
}

// Beta BlockFire.canNeighborCatchFire: any of the 6 neighbours can catch.
static bool fire_can_neighbour_catch(struct server_local* s, int x, int y, int z) {
	return fire_can_block_catch(s, x + 1, y, z) || fire_can_block_catch(s, x - 1, y, z)
		|| fire_can_block_catch(s, x, y - 1, z) || fire_can_block_catch(s, x, y + 1, z)
		|| fire_can_block_catch(s, x, y, z - 1) || fire_can_block_catch(s, x, y, z + 1);
}

// Beta BlockFire.canPlaceBlockAt: solid cube below, or a neighbour can catch fire.
static bool fire_can_place_at(struct server_local* s, int x, int y, int z) {
	return block_is_normal_cube(fire_id(s, x, y - 1, z))
		|| fire_can_neighbour_catch(s, x, y, z);
}

// Exported for EntityLightningBolt's own fire-placement check (server_weather.c
// -- lightning places fire via the SAME Block.fire.canPlaceBlockAt Beta calls).
bool block_fire_can_place_at(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z) {
	return fire_can_place_at(s, x, y, z);
}

// Beta BlockFire.getChanceOfNeighborsEncouragingFire: 0 unless this cell is air; else the MAX
// chanceToEncourageFire over its 6 neighbours.
static int fire_chance_encourage_cell(struct server_local* s, int x, int y, int z) {
	if(fire_id(s, x, y, z) != BLOCK_AIR)
		return 0;
	static const int d[6][3]
		= {{1, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	int m = 0;
	for(int i = 0; i < 6; i++) {
		int e = fire_encourage[fire_id(s, x + d[i][0], y + d[i][1], z + d[i][2])];
		if(e > m)
			m = e;
	}
	return m;
}

static void fire_set(struct server_local* s, int x, int y, int z, uint8_t age) {
	server_world_set_block(block_world(s), x, y, z,
						   (struct block_data) {.type = BLOCK_FIRE,
												.metadata = age,
												.sky_light = 0,
												.torch_light = 15});
}

static void fire_clear(struct server_local* s, int x, int y, int z) {
	server_world_set_block(block_world(s), x, y, z,
						   (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
}

// Beta BlockFire.tryToCatchBlockOnFire: a block with abilityToCatchFire may catch (lower `chance` =
// more likely). When it does it either burns into fire (carrying the source age) or is consumed to
// air; TNT detonates (Beta onBlockDestroyedByPlayer -> here: primed in place).
static void fire_try_catch(struct server_local* s, int x, int y, int z, int chance, int age) {
	int id = fire_id(s, x, y, z);
	if(rand_gen_int(&s->rand_src, chance) < fire_catch[id]) {
		/* Beta chooses and writes the FIRE/AIR replacement before calling
		 * BlockTNT.onBlockDestroyedByPlayer. These draws are mandatory for TNT
		 * too: skipping them changes both the visible cell and every later World.rand
		 * result. Reserve the entity inside the replacement transaction so OOM still
		 * retains the original TNT rather than consuming it without a fuse. */
		/* BlockFire.java:119: draw below age plus 10, requiring a result below 5;
		 * then a rained-on cell does not keep fire. The draw is always consumed. */
		bool leave_fire = rand_gen_int(&s->rand_src, age + 10) < 5
			&& !world_can_block_be_rained_on(s, x, y, z);
		int m = age;
		if(leave_fire)
			m += rand_gen_int(&s->rand_src, 5) / 4;
		if(m > 15)
			m = 15;

		if(id == BLOCK_TNT) {
			struct block_data replacement = leave_fire
				? (struct block_data) {.type = BLOCK_FIRE,
									   .metadata = (uint8_t)m,
									   .torch_light = 15}
				: (struct block_data) {.type = BLOCK_AIR};
			(void)server_local_prime_tnt_block_replacing(
				s, x, y, z, TNT_FUSE_DEFAULT, replacement);
		} else if(leave_fire) {
			fire_set(s, x, y, z, (uint8_t)(m > 15 ? 15 : m));
		} else {
			fire_clear(s, x, y, z);
		}
	}
}

#ifdef CAVEWII_BLOCK_FIRE_TEST
void block_fire_test_try_catch(struct server_local* s, int x, int y, int z,
							   int chance, int age) {
	fire_try_catch(s, x, y, z, chance, age);
}
#endif

// Beta BlockFire.updateTick (scheduled at tickRate 40).
static void onWorldTick(struct server_local* s, struct block_info* info) {
	int x = info->x, y = info->y, z = info->z;

	// Lost its support / fuel -> remove. (Beta falls through onto air, where the no-fuel branch below
	// removes it again with no spread; returning here is observably identical and skips the no-op work.)
	if(!fire_can_place_at(s, x, y, z)) {
		fire_clear(s, x, y, z);
		return;
	}

	// weather slice (was previously omitted -- "no weather in CaveWii", now fixed): rain
	// extinguishes fire NOT directly above netherrack, checked BEFORE the age bump/
	// reschedule below (and skipping both on extinguish, exactly like the no-support
	// branch above) -- this cell OR any of its 4 horizontal neighbours being rained on
	// is enough. [740c583: BlockFire.updateTick var6 + isRaining branch]
	bool eternal_now = fire_id(s, x, y - 1, z) == BLOCK_NETHERRACK;
	if(!eternal_now && server_weather_is_raining(s)
	   && (world_can_block_be_rained_on(s, x, y, z)
		   || world_can_block_be_rained_on(s, x - 1, y, z)
		   || world_can_block_be_rained_on(s, x + 1, y, z)
		   || world_can_block_be_rained_on(s, x, y, z - 1)
		   || world_can_block_be_rained_on(s, x, y, z + 1))) {
		fire_clear(s, x, y, z);
		return;
	}

	int age = info->block->metadata & 0x0F;
	if(age < 15) {
		// Beta setBlockMetadata (NO neighbour notify): age by a three-way draw halved with integer division (0 or 1).
		struct block_data bumped = *info->block;
		bumped.metadata = (uint8_t)(age + rand_gen_int(&s->rand_src, 3) / 2);
		server_world_set_block_silent(block_world(s), x, y, z, bumped);
	}

	// Beta scheduleBlockUpdate uses tickRate: every scheduled callback is one-shot,
	// whether or not the age metadata changed, so explicitly re-arm in 40 ticks
	// (the queue's keep-first dedup makes this safe).
	server_world_schedule_block_tick(block_world(s), x, y, z, BLOCK_FIRE);

	int below = fire_id(s, x, y - 1, z);
	// Beta BlockFire.updateTick's eternal flag: fire directly above NETHERRACK is ETERNAL -- it never dies from
	// lost fuel or from the age-15 burn-out, only ever spreading. the flag is a plain id compare (consumes
	// NO RNG), and Java short-circuits the four-way burn-out roll away when the flag is true, so
	// gating the two death branches on non-eternal fire preserves the exact RNG draw order (the roll is skipped
	// for eternal fire, as in Beta). [decompile 740c583 BlockFire.updateTick]
	bool eternal = (below == BLOCK_NETHERRACK);

	if(!eternal && !fire_can_neighbour_catch(s, x, y, z)) {
		// No fuel around: die if floating or too old; an unspread fire on a solid cube just sits.
		if(!block_is_normal_cube(below) || age > 3)
			fire_clear(s, x, y, z);
		return;
	}

	if(!eternal && fire_encourage[below] == 0 && age == 15
	   && rand_gen_int(&s->rand_src, 4) == 0) {
		// Old fire whose base can't itself catch: 1-in-4 burn-out.
		fire_clear(s, x, y, z);
		return;
	}

	// Consume the 6 directly-adjacent blocks (Beta chances: sides 300, up/down 250).
	fire_try_catch(s, x + 1, y, z, 300, age);
	fire_try_catch(s, x - 1, y, z, 300, age);
	fire_try_catch(s, x, y - 1, z, 250, age);
	fire_try_catch(s, x, y + 1, z, 250, age);
	fire_try_catch(s, x, y, z - 1, 300, age);
	fire_try_catch(s, x, y, z + 1, 300, age);

	// Ignite NEW fire in the air pockets of the 3x3 column x6-tall box around the source, weighted by
	// how much the surrounding flammables encourage it and by the source age (older fire spreads less).
	// weather slice (F1, adversarial review): each candidate ALSO carries Beta's own 5-clause
	// canBlockBeRainedOn rain gate INSIDE this loop -- previously this engine had the rain gate only
	// on the EXTINGUISH branch above, so fire could still spread new ignitions through rain. Ported
	// VERBATIM, RNG draw order intact (the base-chance nextInt is unconditional -- it is the first
	// conjunction operand in Beta too -- the 5 rain checks are pure booleans with no draws, so their position
	// only matters for short-circuit skipping, not for determinism).
	// [740c583: BlockFire.updateTick, the bounded chance draw compared inclusively with
	// the spread threshold, followed by five rain checks before setBlockAndMetadataWithNotify]
	for(int bx = x - 1; bx <= x + 1; bx++) {
		for(int bz = z - 1; bz <= z + 1; bz++) {
			for(int by = y - 1; by <= y + 4; by++) {
				if(bx == x && by == y && bz == z)
					continue;
				int base = 100;
				if(by > y + 1)
					base += (by - (y + 1)) * 100; // harder to ignite higher up
				int enc = fire_chance_encourage_cell(s, bx, by, bz);
				if(enc > 0) {
					int t = (enc + 40) / (age + 30);
					if(t > 0 && rand_gen_int(&s->rand_src, base) <= t
					   // The decompile accepts either no rain or a cell that cannot be rained on --
					   // canBlockBeRainedOn already returns false whenever rain is absent, so this
					   // reduces to the plain negation (world_can_block_be_rained_on IS that
					   // isRaining-gated definition; see its own doc comment).
					   && !world_can_block_be_rained_on(s, bx, by, bz)
					   // NOTCH'S OWN TYPO, replicated on purpose (house precedent: the
					   // chain-armor-from-fire ruling -- ship Beta's shipped behavior, not the
					   // behavior we'd have written): this clause tests `z`, the FIRE SOURCE's
					   // pre-loop z coordinate, not `bz`, the current candidate cell's z. Every
					   // other neighbour clause below correctly uses `bz`. Do not "fix" this --
					   // it is not a CaveWii bug, it is Beta 1.7.3's actual shipped ignition gate.
					   && !world_can_block_be_rained_on(s, bx - 1, by, z)
					   && !world_can_block_be_rained_on(s, bx + 1, by, bz)
					   && !world_can_block_be_rained_on(s, bx, by, bz - 1)
					   && !world_can_block_be_rained_on(s, bx, by, bz + 1)) {
						int m = age + rand_gen_int(&s->rand_src, 5) / 4;
						fire_set(s, bx, by, bz, (uint8_t)(m > 15 ? 15 : m));
					}
				}
			}
		}
	}
}

struct block block_fire = {
	.name = "Fire",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = NULL,
	.onWorldTick = onWorldTick, // Beta scheduled fire spread (tickRate 40)
	.tick_delay = 40,			// Beta BlockFire.tickRate
	.onRightClick = NULL,
	.transparent = true,
	.renderBlock = render_block_fire,
	.renderBlockAlways = NULL,
	.luminance = 15,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 0,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = true,
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
