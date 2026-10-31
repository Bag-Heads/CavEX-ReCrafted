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
	Beta 1.7.3 populate (decorate) phase -- the orchestration that runs after a
	chunk's terrain exists. This file currently implements the ORE sub-pass
	(worldgen_ores); lakes, dungeons, clay, trees and the small decorations are
	added here in Beta's exact order as the phase is built up. The whole
	sequence shares ONE per-chunk decorate Random, so each step must consume
	randomness in the exact Beta order -- the positions of every later step
	depend on it.
*/

#include <string.h>

#include "../block/blocks_data.h"
#include "../item/items.h" // struct item_data + ITEM_* ids for dungeon loot
#include "beta_math.h"
#include "java_random.h"
#include "jmath.h"
#include "decorate.h"
#include "populate.h"
#include "tree.h"

// Beta unlit redstone ore is block id 73. The fork has no BLOCK_ constant for
// it yet -- the block still needs registering before it renders in-game -- but
// the WORLD DATA id worldgen must write is 73, so that is what we place.
#define ORE_REDSTONE 73

// One ore pass: `count` veins of `size`, each seeded at a random column in the
// chunk and a y in [0, y_range), replacing stone with `ore`. The draw order
// (x, y, z, then the vein) is load-bearing -- keep it exact.
static void ore_pass(struct wgen_world* w, struct java_random* rand,
					 int block_x, int block_z, int count, int size, int y_range,
					 uint8_t ore) {
	for(int i = 0; i < count; i++) {
		int x = block_x + jrand_next_int_bound(rand, 16);
		int y = jrand_next_int_bound(rand, y_range);
		int z = block_z + jrand_next_int_bound(rand, 16);
		worldgen_minable(w, rand, x, y, z, size, ore, 0);
	}
}

void worldgen_ores(struct wgen_world* w, struct java_random* rand, int block_x,
				   int block_z) {
	// Beta order + parameters (count, size, y-range):
	ore_pass(w, rand, block_x, block_z, 20, 32, 128, BLOCK_DIRT);
	ore_pass(w, rand, block_x, block_z, 10, 32, 128, BLOCK_GRAVEL);
	ore_pass(w, rand, block_x, block_z, 20, 16, 128, BLOCK_COAL_ORE);
	ore_pass(w, rand, block_x, block_z, 20, 8, 64, BLOCK_IRON_ORE);
	ore_pass(w, rand, block_x, block_z, 2, 8, 32, BLOCK_GOLD_ORE);
	ore_pass(w, rand, block_x, block_z, 8, 7, 16, ORE_REDSTONE);
	ore_pass(w, rand, block_x, block_z, 1, 7, 16, BLOCK_DIAMOND_ORE);

	// Lapis is the one irregular pass: a single vein whose y is the SUM of two
	// draws, each uniformly below 16, giving a triangular distribution
	// peaking near y=15. C does not order addition operands like Java, so the
	// two draws use explicit ordered temporaries.
	int x = block_x + jrand_next_int_bound(rand, 16);
	int ly0 = jrand_next_int_bound(rand, 16);
	int ly1 = jrand_next_int_bound(rand, 16);
	int y = ly0 + ly1;
	int z = block_z + jrand_next_int_bound(rand, 16);
	worldgen_minable(w, rand, x, y, z, 6, BLOCK_LAPIS_ORE, 0);
}

// Beta Material.isLiquid: the two fluids (still + flowing).
static bool populate_is_liquid(uint8_t id) {
	return id == BLOCK_WATER_FLOW || id == BLOCK_WATER_STILL
		|| id == BLOCK_LAVA_FLOW || id == BLOCK_LAVA_STILL;
}

// Beta Material.isSolid: false for air, the fluids, and the passable
// "logic"/transparent/replaceable blocks (plants, fire, circuits, snow layer,
// portal); true for everything else. The lake validates the LOWER shell (y<4)
// only -- always underground after the descend-to-solid -- where the sole
// non-solid blocks are air/water/lava, so this is byte-exact on the path that
// gates RNG and lake placement. The plant cases only ever matter at a lava
// lake's surface rim and never advance the Random stream (the binary edge draw
// is made before this is consulted).
static bool populate_is_solid(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:
		case BLOCK_WATER_FLOW:
		case BLOCK_WATER_STILL:
		case BLOCK_LAVA_FLOW:
		case BLOCK_LAVA_STILL:
		case 6:	 // sapling
		case 31: // tall grass / fern
		case 32: // dead bush
		case 37: // dandelion
		case 38: // rose
		case 39: // brown mushroom
		case 40: // red mushroom
		case 50: // torch
		case 51: // fire
		case 55: // redstone wire
		case 59: // crops
		case 78: // snow layer
		case 83: // sugar cane
		case 90: // portal
			return false;
		default:
			return true;
	}
}

// A 16x16x8 shell-edge cell: empty in the mask but orthogonally adjacent to a
// filled one. Reused verbatim by the validation pass and the lava stone-rim
// pass (same neighbour tests as Beta).
static bool lake_is_edge(const bool* mask, int x, int z, int y) {
	return !mask[(x * 16 + z) * 8 + y]
		&& ((x < 15 && mask[((x + 1) * 16 + z) * 8 + y])
			|| (x > 0 && mask[((x - 1) * 16 + z) * 8 + y])
			|| (z < 15 && mask[(x * 16 + z + 1) * 8 + y])
			|| (z > 0 && mask[(x * 16 + (z - 1)) * 8 + y])
			|| (y < 7 && mask[(x * 16 + z) * 8 + y + 1])
			|| (y > 0 && mask[(x * 16 + z) * 8 + (y - 1)]));
}

bool worldgen_lake(struct wgen_world* w, struct java_random* rand, int x, int y,
				   int z, uint8_t fluid_id) {
	bool lava = (fluid_id == BLOCK_LAVA_STILL || fluid_id == BLOCK_LAVA_FLOW);

	x -= 8;
	z -= 8;
	// Descend to the first non-air block (or the world floor), then drop 4.
	for(; y > 0; y--) {
		if(w->get(w->ctx, x, y, z) != BLOCK_AIR)
			break;
	}
	y -= 4;

	// Union of 4..7 ellipsoid blobs. The mask stores y fastest, then z, then x; x/z in [0,16), y in
	// [0,8). The blob radii/centres are the ONLY randomness in a water lake.
	bool mask[2048];
	memset(mask, 0, sizeof(mask));

	int blob_count = jrand_next_int_bound(rand, 4) + 4;
	for(int b = 0; b < blob_count; b++) {
		double radius_x = jrand_next_double(rand) * 6.0 + 3.0;
		double radius_y = jrand_next_double(rand) * 4.0 + 2.0;
		double radius_z = jrand_next_double(rand) * 6.0 + 3.0;
		double center_x
			= jrand_next_double(rand) * (16.0 - radius_x - 2.0) + 1.0
			+ radius_x / 2.0;
		double center_y
			= jrand_next_double(rand) * (8.0 - radius_y - 4.0) + 2.0
			+ radius_y / 2.0;
		double center_z
			= jrand_next_double(rand) * (16.0 - radius_z - 2.0) + 1.0
			+ radius_z / 2.0;

		for(int bx = 1; bx < 15; bx++) {
			for(int bz = 1; bz < 15; bz++) {
				for(int by = 1; by < 7; by++) {
					double dx = ((double)bx - center_x) / (radius_x / 2.0);
					double dy = ((double)by - center_y) / (radius_y / 2.0);
					double dz = ((double)bz - center_z) / (radius_z / 2.0);
					if(dx * dx + dy * dy + dz * dz < 1.0)
						mask[(bx * 16 + bz) * 8 + by] = true;
				}
			}
		}
	}

	// Validate the shell: abort if the upper half would merge with existing
	// liquid, or the lower half would rest against a non-solid block (a hole
	// the fluid would leak through) that is not our own fluid.
	for(int lx = 0; lx < 16; lx++) {
		for(int lz = 0; lz < 16; lz++) {
			for(int ly = 0; ly < 8; ly++) {
				if(!lake_is_edge(mask, lx, lz, ly))
					continue;
				uint8_t bt = w->get(w->ctx, x + lx, y + ly, z + lz);
				if(ly >= 4 && populate_is_liquid(bt))
					return false;
				if(ly < 4 && !populate_is_solid(bt) && bt != fluid_id)
					return false;
			}
		}
	}

	// Fill: lower half (y<4) = fluid, upper half = air.
	for(int lx = 0; lx < 16; lx++)
		for(int lz = 0; lz < 16; lz++)
			for(int ly = 0; ly < 8; ly++)
				if(mask[(lx * 16 + lz) * 8 + ly])
					wgen_set_plain(w, x + lx, y + ly, z + lz,
						   ly >= 4 ? BLOCK_AIR : fluid_id);

	// Exposed dirt directly under the pool becomes grass where sky reaches.
	for(int lx = 0; lx < 16; lx++)
		for(int lz = 0; lz < 16; lz++)
			for(int ly = 4; ly < 8; ly++)
				if(mask[(lx * 16 + lz) * 8 + ly]
				   && w->get(w->ctx, x + lx, y + ly - 1, z + lz) == BLOCK_DIRT
				   && w->skylight
				   && w->skylight(w->ctx, x + lx, y + ly, z + lz) > 0)
					wgen_set_plain(w, x + lx, y + ly - 1, z + lz, BLOCK_GRASS);

	// Lava lakes encase their solid rim in stone (the upper rim only ~half the
	// time -- one binary draw per upper-shell edge cell, load-bearing).
	if(lava) {
		for(int lx = 0; lx < 16; lx++) {
			for(int lz = 0; lz < 16; lz++) {
				for(int ly = 0; ly < 8; ly++) {
					if(lake_is_edge(mask, lx, lz, ly)
					   && (ly < 4 || jrand_next_int_bound(rand, 2) != 0)
					   && populate_is_solid(
						   w->get(w->ctx, x + lx, y + ly, z + lz)))
						wgen_set_plain(w, x + lx, y + ly, z + lz, BLOCK_STONE);
				}
			}
		}
	}

	return true;
}

// Beta casts Math.PI to float in the worldgen blob features.
#define POP_PI ((float)3.141592653589793)
// Mob spawner block data id; worldgen writes this where Beta places the dungeon
// spawner (block/blocks_data.h holds the canonical constant).
#define DUNGEON_SPAWNER BLOCK_MOB_SPAWNER

// MathHelper.floor_double: truncate toward zero then adjust down for negatives.
static int pop_floor(double x) {
	int i = java_double_to_int(x);
	return x < (double)i ? i - 1 : i;
}

// One dungeon-chest loot draw, consuming EXACTLY the RNG of Beta's
// getDungeonLootItem (an eleven-way roll then per-category draws) AND returning the
// resulting item. `present` is whether the roll occupies a slot (the caller
// draws a slot uniformly below 27 iff present, exactly as Beta calls
// setInventorySlotContents only for a non-null roll). A zero item id with
// a true presence flag means the roll is real but unrepresentable here -- only the
// music-disc case (Beta item 2256/2257, beyond the fork's ITEMS_MAX): Beta would
// drop a disc into the slot; we occupy/overwrite the slot but leave it empty, so
// the RNG (which the later ore passes depend on) and every other item stay
// byte-exact.
struct dungeon_loot {
	bool present;
	struct item_data item;
};

static struct dungeon_loot dungeon_loot_roll(struct java_random* rand) {
	struct dungeon_loot r = {false, {0, 0, 0}};
	switch(jrand_next_int_bound(rand, 11)) {
		case 0: // saddle
			r = (struct dungeon_loot) {true, {ITEM_SADDLE, 0, 1}};
			break;
		case 1: // iron ingot x(1..4)
			r = (struct dungeon_loot) {
				true, {ITEM_IRON, 0, (uint8_t)(jrand_next_int_bound(rand, 4) + 1)}};
			break;
		case 2: // bread
			r = (struct dungeon_loot) {true, {ITEM_BREAD, 0, 1}};
			break;
		case 3: // wheat x(1..4)
			r = (struct dungeon_loot) {
				true,
				{ITEM_WHEAT, 0, (uint8_t)(jrand_next_int_bound(rand, 4) + 1)}};
			break;
		case 4: // gunpowder x(1..4)
			r = (struct dungeon_loot) {
				true,
				{ITEM_GUNPOWDER, 0, (uint8_t)(jrand_next_int_bound(rand, 4) + 1)}};
			break;
		case 5: // string x(1..4)
			r = (struct dungeon_loot) {
				true,
				{ITEM_STRING, 0, (uint8_t)(jrand_next_int_bound(rand, 4) + 1)}};
			break;
		case 6: // empty bucket
			r = (struct dungeon_loot) {true, {ITEM_BUCKET, 0, 1}};
			break;
		case 7: // golden apple (1 in 100)
			if(jrand_next_int_bound(rand, 100) == 0)
				r = (struct dungeon_loot) {true, {ITEM_APPLE_GOLDEN, 0, 1}};
			break;
		case 8: // redstone x(1..4) (1 in 2)
			if(jrand_next_int_bound(rand, 2) == 0)
				r = (struct dungeon_loot) {
					true,
					{ITEM_REDSTONE, 0,
					 (uint8_t)(jrand_next_int_bound(rand, 4) + 1)}};
			break;
		case 9: // music disc (1 in 10) -- unrepresentable, slot left empty
			if(jrand_next_int_bound(rand, 10) == 0) {
				jrand_next_int_bound(rand, 2); // record13 vs recordCat
				r.present = true;			   // occupy the slot, item stays empty
			}
			break;
		case 10: // cocoa beans (dye, damage 3)
			r = (struct dungeon_loot) {true, {ITEM_DYE, 3, 1}};
			break;
	}
	return r;
}

// FH-17: Beta WorldGenDungeons.pickMobSpawner [decompile 740c583] transcribed
// directly (a literal switch on the SAME four-way roll worldgen_dungeon
// already drew for the metadata-encoding this replaces) -- deliberately NOT
// routed through network/spawner.h's spawner_mob_from_meta/spawner_mob_to_id
// (the server-side migration helpers): worldgen must stay decoupled from the
// network/server_local layer, and this one-line map is the whole of Beta's
// EntityId selection anyway. 0=Skeleton, 1=Zombie, 2=Zombie, 3=Spider.
static const char* dungeon_pick_mob_spawner(int roll) {
	static const char* const map[4] = {"Skeleton", "Zombie", "Zombie",
										"Spider"};
	return map[roll & 3];
}

bool worldgen_dungeon(struct wgen_world* w, struct java_random* rand, int px,
					  int py, int pz) {
	const int height = 3;
	// Two width draws happen FIRST and are consumed even when the dungeon
	// aborts -- load-bearing for everything after.
	int wx = jrand_next_int_bound(rand, 2) + 2;
	int wz = jrand_next_int_bound(rand, 2) + 2;

	// Validate: solid floor + ceiling, and 1..5 wall openings (air at y and
	// y+1 on the perimeter). No randomness here.
	int entrances = 0;
	for(int xi = px - wx - 1; xi <= px + wx + 1; xi++) {
		for(int yi = py - 1; yi <= py + height + 1; yi++) {
			for(int zi = pz - wz - 1; zi <= pz + wz + 1; zi++) {
				uint8_t bt = w->get(w->ctx, xi, yi, zi);
				if(yi == py - 1 && !populate_is_solid(bt))
					return false;
				if(yi == py + height + 1 && !populate_is_solid(bt))
					return false;
				if((xi == px - wx - 1 || xi == px + wx + 1 || zi == pz - wz - 1
					|| zi == pz + wz + 1)
				   && yi == py && bt == BLOCK_AIR
				   && w->get(w->ctx, xi, yi + 1, zi) == BLOCK_AIR)
					entrances++;
			}
		}
	}
	if(entrances < 1 || entrances > 5)
		return false;

	// Hollow the room and wrap it in cobblestone; the floor row is mossy 3/4 of
	// the time (one four-way draw per solid floor block, in iteration order).
	for(int xi = px - wx - 1; xi <= px + wx + 1; xi++) {
		for(int yi = py + height; yi >= py - 1; yi--) {
			for(int zi = pz - wz - 1; zi <= pz + wz + 1; zi++) {
				bool interior = xi != px - wx - 1 && yi != py - 1
					&& zi != pz - wz - 1 && xi != px + wx + 1
					&& yi != py + height + 1 && zi != pz + wz + 1;
				if(interior) {
					wgen_set_plain_notify(w, xi, yi, zi, BLOCK_AIR);
				} else if(yi >= 0
						  && !populate_is_solid(
							  w->get(w->ctx, xi, yi - 1, zi))) {
					wgen_set_plain_notify(w, xi, yi, zi, BLOCK_AIR);
				} else if(populate_is_solid(w->get(w->ctx, xi, yi, zi))) {
					if(yi == py - 1 && jrand_next_int_bound(rand, 4) != 0)
						wgen_set_plain_notify(w, xi, yi, zi, BLOCK_MOSSY_COBBLE);
					else
						wgen_set_plain_notify(w, xi, yi, zi, BLOCK_COBBLESTONE);
				}
			}
		}
	}

	// Up to two chests, each tried up to 3 times at a random in-room column
	// with exactly one solid neighbour. Each placed chest rolls 8 loot slots.
	for(int chest = 0; chest < 2; chest++) {
		for(int attempt = 0; attempt < 3; attempt++) {
			int cx = px + jrand_next_int_bound(rand, wx * 2 + 1) - wx;
			int cz = pz + jrand_next_int_bound(rand, wz * 2 + 1) - wz;
			if(w->get(w->ctx, cx, py, cz) != BLOCK_AIR)
				continue;
			int adj = 0;
			if(populate_is_solid(w->get(w->ctx, cx - 1, py, cz)))
				adj++;
			if(populate_is_solid(w->get(w->ctx, cx + 1, py, cz)))
				adj++;
			if(populate_is_solid(w->get(w->ctx, cx, py, cz - 1)))
				adj++;
			if(populate_is_solid(w->get(w->ctx, cx, py, cz + 1)))
				adj++;
			if(adj == 1) {
				wgen_set_plain_notify(w, cx, py, cz, BLOCK_CHEST);
				// Beta fills 8 loot slots: each rolls an item and, if present,
				// drops it at a random slot (uniformly below 27); a later item may
				// overwrite an earlier one, exactly as setInventorySlotContents.
				// Accumulate into a local inventory, then hand the finished chest
				// to the container store (set_chest is NULL in unit tests/tools,
				// which still run the identical RNG above).
				struct item_data loot[DUNGEON_CHEST_SLOTS];
				memset(loot, 0, sizeof(loot));
				for(int slot = 0; slot < 8; slot++) {
					struct dungeon_loot d = dungeon_loot_roll(rand);
					if(d.present)
						loot[jrand_next_int_bound(rand, DUNGEON_CHEST_SLOTS)]
							= d.item;
				}
				if(w->set_chest)
					w->set_chest(w->ctx, cx, py, cz, loot);
				break;
			}
		}
	}

	// Mob spawner at the centre. Beta's pickMobSpawner draws exactly one
	// four-way roll (0=Skeleton, 1=Zombie, 2=Zombie, 3=Spider) -- the draw is the
	// dungeon's last and this whole block writes no randomness, so the
	// decorate stream is UNCHANGED from before FH-17: same draw, same
	// position in the stream. Only its destination changed -- the block's
	// metadata is now always 0 (Beta's own faithful spawner-block value; the
	// legacy 2-bit encoding this port used to smuggle the pick into it is
	// gone), and the pick instead selects the Beta EntityId string
	// materialised as a MobSpawner tile entity through w->set_spawner.
	int mob_roll = jrand_next_int_bound(rand, 4);
	wgen_set_plain_notify(w, px, py, pz, DUNGEON_SPAWNER);
	if(w->set_spawner)
		w->set_spawner(
			w->ctx, px, py, pz, dungeon_pick_mob_spawner(mob_roll));
	return true;
}

bool worldgen_clay(struct wgen_world* w, struct java_random* rand, int i, int j,
				   int k, int size) {
	// Clay only forms where the seed block is water; the check happens BEFORE
	// any draw, so off-water attempts consume no randomness.
	uint8_t at = w->get(w->ctx, i, j, k);
	if(at != BLOCK_WATER_STILL && at != BLOCK_WATER_FLOW)
		return false;

	beta_math_init();

	// Same swept-blob geometry as worldgen_minable (identical casts/PRNG order),
	// but replacing SAND with CLAY instead of STONE with ore.
	float angle = jrand_next_float(rand) * POP_PI;
	float si = mh_sin(angle);
	float co = mh_cos(angle);
	float fi8 = (float)(i + 8);
	float fk8 = (float)(k + 8);
	float sterm = si * (float)size / 8.0f;
	float cterm = co * (float)size / 8.0f;
	double x_start = (double)(fi8 + sterm);
	double x_end = (double)(fi8 - sterm);
	double z_start = (double)(fk8 + cterm);
	double z_end = (double)(fk8 - cterm);

	int ry0 = jrand_next_int_bound(rand, 3);
	double y_start = (double)(j + ry0 + 2);
	int ry1 = jrand_next_int_bound(rand, 3);
	double y_end = (double)(j + ry1 + 2);

	for(int l = 0; l <= size; l++) {
		double cx = x_start + (x_end - x_start) * (double)l / (double)size;
		double cy = y_start + (y_end - y_start) * (double)l / (double)size;
		double cz = z_start + (z_end - z_start) * (double)l / (double)size;
		double scale = jrand_next_double(rand) * (double)size / 16.0;
		float bell = mh_sin((float)l * POP_PI / (float)size) + 1.0f;
		double rxz = (double)bell * scale + 1.0;
		double ry = (double)bell * scale + 1.0;

		int x_lo = pop_floor(cx - rxz / 2.0);
		int x_hi = pop_floor(cx + rxz / 2.0);
		int y_lo = pop_floor(cy - ry / 2.0);
		int y_hi = pop_floor(cy + ry / 2.0);
		int z_lo = pop_floor(cz - rxz / 2.0);
		int z_hi = pop_floor(cz + rxz / 2.0);

		for(int bx = x_lo; bx <= x_hi; bx++) {
			double dx = ((double)bx + 0.5 - cx) / (rxz / 2.0);
			if(dx * dx < 1.0) {
				for(int by = y_lo; by <= y_hi; by++) {
					double dy = ((double)by + 0.5 - cy) / (ry / 2.0);
					if(dx * dx + dy * dy < 1.0) {
						for(int bz = z_lo; bz <= z_hi; bz++) {
							double dz = ((double)bz + 0.5 - cz) / (rxz / 2.0);
							if(dx * dx + dy * dy + dz * dz < 1.0
							   && w->get(w->ctx, bx, by, bz) == BLOCK_SAND)
								wgen_set_plain(w, bx, by, bz, BLOCK_CLAY);
						}
					}
				}
			}
		}
	}
	return true;
}

/* ---- populate-time BlockLeaves.onBlockRemoval (see populate.h) ---- */

static int lr_java_shr4(int v) { /* Java arithmetic right shift by four: divide by 16, rounding down. */
	return v >= 0 ? v >> 4 : -((-v - 1) / 16) - 1;
}

/* World.checkChunksExist [740c583 World.java:327-348] over the 2x2 populate
 * window (chunk_x..+1, chunk_z..+1). */
static bool lr_checkChunksExist(int chunk_x, int chunk_z, int x1, int y1,
								int z1, int x2, int y2, int z2) {
	if(!(y2 >= 0 && y1 < 128))
		return false;
	for(int cx = lr_java_shr4(x1); cx <= lr_java_shr4(x2); cx++)
		for(int cz = lr_java_shr4(z1); cz <= lr_java_shr4(z2); cz++)
			if(!(cx >= chunk_x && cx <= chunk_x + 1 && cz >= chunk_z
				 && cz <= chunk_z + 1))
				return false;
	return true;
}

/* BlockLeaves.onBlockRemoval [740c583 BlockLeaves.java:33-50], called by the
 * writer after it stored a non-leaf id over a LEAVES cell. */
void wgen_leaves_on_removal(struct wgen_world* w, int chunk_x, int chunk_z,
							int x, int y, int z) {
	if(!w->get_meta)
		return;
	if(!(w->chunks_exist ? w->chunks_exist(w->ctx, x-2,y-2,z-2,x+2,y+2,z+2)
		: lr_checkChunksExist(chunk_x, chunk_z, x-2,y-2,z-2,x+2,y+2,z+2)))
		return;
	for(int dx = -1; dx <= 1; dx++)
		for(int dy = -1; dy <= 1; dy++)
			for(int dz = -1; dz <= 1; dz++) {
				if(w->get(w->ctx, x + dx, y + dy, z + dz) != BLOCK_LEAVES)
					continue;
				uint8_t m = w->get_meta(w->ctx, x + dx, y + dy, z + dz);
				if(w->set_meta)
					w->set_meta(w->ctx, x + dx, y + dy, z + dz, (uint8_t)(m | 8));
				else w->set(w->ctx, x + dx, y + dy, z + dz, BLOCK_LEAVES,
					   (uint8_t)(m | 8));
			}
}

/* CFR BlockLog.java:38-53: radius FOUR, requiring radius FIVE residency;
 * unlike leaf removal, already flagged leaves do not receive another store. */
void wgen_log_on_removal(struct wgen_world* w, int chunk_x, int chunk_z,
	int x, int y, int z) {
	if(!w->get_meta || !w->set_meta)
		return;
	if(!(w->chunks_exist ? w->chunks_exist(w->ctx, x-5,y-5,z-5,x+5,y+5,z+5)
		: lr_checkChunksExist(chunk_x, chunk_z, x-5,y-5,z-5,x+5,y+5,z+5)))
		return;
	for(int dx = -4; dx <= 4; dx++)
		for(int dy = -4; dy <= 4; dy++)
			for(int dz = -4; dz <= 4; dz++) {
				if(w->get(w->ctx, x+dx, y+dy, z+dz) != BLOCK_LEAVES)
					continue;
				uint8_t meta = w->get_meta(w->ctx, x+dx, y+dy, z+dz);
				if(!(meta & 8))
					w->set_meta(w->ctx, x+dx, y+dy, z+dz, meta | 8);
			}
}

void worldgen_populate(int64_t seed, int chunk_x, int chunk_z,
					   struct wgen_world* w, enum biome corner_biome,
					   double tree_density) {
	// Per-chunk decorate Random, byte-exact to Beta (same shape as the cave
	// generator's region seeding): the exclusive-or with the seed happens after the two
	// products are summed.
	struct java_random rand;
	jrand_set_seed(&rand, seed);
	int64_t l1 = jrand_next_long(&rand) / 2LL * 2LL + 1LL;
	int64_t l2 = jrand_next_long(&rand) / 2LL * 2LL + 1LL;
	uint64_t s = ((uint64_t)(int64_t)chunk_x * (uint64_t)l1
				  + (uint64_t)(int64_t)chunk_z * (uint64_t)l2)
		^ (uint64_t)seed;
	jrand_set_seed(&rand, (int64_t)s);

	int block_x = chunk_x * 16;
	int block_z = chunk_z * 16;

	// Beta decorate sequence (one shared Random, exact draw order is
	// load-bearing). IMPLEMENTED: water lake -> lava lake -> ores.
	//
	// Water lake: a 1-in-4 chance, then a column at +8 offset and a y anywhere
	// in [0,128).
	if(jrand_next_int_bound(&rand, 4) == 0) {
		int lx = block_x + jrand_next_int_bound(&rand, 16) + 8;
		int ly = jrand_next_int_bound(&rand, 128);
		int lz = block_z + jrand_next_int_bound(&rand, 16) + 8;
		worldgen_lake(w, &rand, lx, ly, lz, BLOCK_WATER_STILL);
	}

	// Lava lake: a 1-in-8 chance; draw a value below 120, add 8 for the bound,
	// then draw y below that bound (compute the inner bound FIRST -- C does not
	// order nested calls like Java otherwise). Generate below sea level OR, above
	// it, on a further 1-in-10 roll, drawn only at y of at least 64, exactly as
	// Java short-circuits the disjunction.
	if(jrand_next_int_bound(&rand, 8) == 0) {
		int lx = block_x + jrand_next_int_bound(&rand, 16) + 8;
		int inner = jrand_next_int_bound(&rand, 120);
		int ly = jrand_next_int_bound(&rand, inner + 8);
		int lz = block_z + jrand_next_int_bound(&rand, 16) + 8;
		if(ly < 64 || jrand_next_int_bound(&rand, 10) == 0)
			worldgen_lake(w, &rand, lx, ly, lz, BLOCK_LAVA_STILL);
	}

	// Dungeons: 8 tries, each at a +8-offset column and any y in [0,128). Each
	// try draws its position then runs the dungeon (which itself consumes a
	// variable, terrain-dependent amount of randomness -- see worldgen_dungeon).
	for(int i = 0; i < 8; i++) {
		int dx = block_x + jrand_next_int_bound(&rand, 16) + 8;
		int dy = jrand_next_int_bound(&rand, 128);
		int dz = block_z + jrand_next_int_bound(&rand, 16) + 8;
		worldgen_dungeon(w, &rand, dx, dy, dz);
	}

	// Clay: 10 tries, each at a NON-offset column (unlike lakes/dungeons) and
	// any y in [0,128); worldgen_clay only draws (and places) when the column
	// hits water.
	for(int i = 0; i < 10; i++) {
		int cx = block_x + jrand_next_int_bound(&rand, 16);
		int cy = jrand_next_int_bound(&rand, 128);
		int cz = block_z + jrand_next_int_bound(&rand, 16);
		worldgen_clay(w, &rand, cx, cy, cz, 32);
	}

	// With water lake -> lava lake -> dungeons -> clay all preceding it, the ore
	// pass now consumes the Random at Beta's exact offset, so the ore POSITIONS
	// are byte-exact in-game (matching the per-vein geometry already proven).
	worldgen_ores(w, &rand, block_x, block_z);

	// Trees come straight after the ore passes (Beta's exact decorate order),
	// using the biome + treeDensity the caller sampled from the generator.
	worldgen_trees(w, &rand, corner_biome, tree_density, block_x, block_z);

	// ...then the small decorations (flowers, grass, mushrooms, sugar cane,
	// pumpkins, cacti, springs) in Beta's order, sharing the same Random.
	worldgen_decorate(w, &rand, corner_biome, block_x, block_z);
}
