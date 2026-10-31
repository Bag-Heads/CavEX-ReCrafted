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
	Beta 1.7.3 small decorations, ported from ChunkProviderGenerate.populate's
	post-tree sequence + the WorldGen* feature bodies (cross-checked vs the
	BetrockServer reference).

	The whole populate pass shares one Random, so the EXACT number + order of
	draws here is load-bearing -- a single extra/missing draw shifts every later
	decoration. Two recurring Beta idioms that must be reproduced verbatim:
	  - every position offset is `nextInt(N) - nextInt(N)`. C does NOT order the
	    two operands of `a() - b()` like Java (left-to-right), so each pair is
	    drawn into ordered temporaries.
	  - the fixed-count feature bodies (flowers 64, tall grass 128, dead bush 4,
	    pumpkin 64, cactus 10) ALWAYS burn their whole draw budget even when every
	    placement fails -- the block checks gate placement, never the draws.

	CanStay is the one thing that is NOT byte-exact-critical for the RNG: it only
	decides whether a plant block is written, never whether a draw happens. It IS
	still world-content-critical (a wrongly written plant occupies a cell that
	later air checks in the same pass read), so each case is the literal Beta
	canBlockStay -- see the cactus 4-lateral gate and the reed grass/dirt+water
	gate (a reed on sand is impossible in 1.7.3). The Material.water spot gates
	(reed) are draw-critical: they decide whether a height is drawn at all.
*/

#include <stdbool.h>
#include <stdint.h>

#include "../block/blocks_data.h"
#include "decorate.h"
#include "java_random.h"
#include "populate.h"

// Beta block ids written by the decorations (the fork's enum names, or the raw
// id where it has no constant yet).
#define DEC_DANDELION BLOCK_FLOWER		 // 37
#define DEC_ROSE BLOCK_ROSE				 // 38
#define DEC_TALLGRASS BLOCK_TALL_GRASS	 // 31
#define DEC_DEADBUSH 32					 // no BLOCK_ constant in the fork yet
#define DEC_MUSH_BROWN BLOCK_BROWM_MUSHROOM // 39
#define DEC_MUSH_RED BLOCK_RED_MUSHROOM	 // 40
#define DEC_SUGARCANE BLOCK_REED		 // 83
#define DEC_PUMPKIN BLOCK_PUMPKIN		 // 86
#define DEC_CACTUS BLOCK_CACTUS			 // 81
#define DEC_WATER BLOCK_WATER_FLOW		 // 8
#define DEC_LAVA BLOCK_LAVA_FLOW		 // 10

// Shorthand for the per-step Random; keeps the offset triples readable.
static inline int di(struct java_random* r, int bound) {
	return jrand_next_int_bound(r, bound);
}

/* CFR BlockMushroom.java:49-55 uses STATIC opaqueCubeLookup. Leaves'
 * constructor initializes that table TRUE before graphicsLevel is assigned
 * (Block.java constructor / BlockLeavesBase.java), despite later transparency. */
static bool dec_is_opaque(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:
		case BLOCK_SAPLING:
		case BLOCK_WATER_FLOW:
		case BLOCK_WATER_STILL:
		case BLOCK_LAVA_FLOW:
		case BLOCK_LAVA_STILL:
		case BLOCK_GLASS:
		case BLOCK_TALL_GRASS:
		case DEC_DEADBUSH:
		case BLOCK_FLOWER:
		case BLOCK_ROSE:
		case BLOCK_BROWM_MUSHROOM:
		case BLOCK_RED_MUSHROOM:
		case BLOCK_TORCH:
		case BLOCK_FIRE:
		case BLOCK_REDSTONE_WIRE:
		case BLOCK_CROPS:
		case BLOCK_SNOW:
		case BLOCK_REED:
		case BLOCK_PORTAL:
		case BLOCK_ICE:
			return false;
		default: return true;
	}
}

// Beta BlockCactus.canBlockStay's lateral test [decompile 740c583]:
// `getBlockMaterial(x+-1,y,z).isSolid()` / `(x,y,z+-1).isSolid()` on all 4
// horizontal neighbours -- ANY solid Material fails the placement. Material.
// isSolid() [Material.java] defaults to true and is overridden false ONLY by
// MaterialLogic (plants/circuits/snow-layer), MaterialTransparent (air/fire),
// MaterialLiquid (water/lava) and MaterialPortal -- everything else
// (including glass/ice/leaves, despite being non-opaque for LIGHT) stays
// solid. This is therefore intentionally NOT the same predicate as
// dec_is_opaque above (a light-blocking test, where ice/glass/leaves are
// "false"): a distinct Material.isSolid()-shaped table, restricted to the
// block ids worldgen decoration can plausibly hand it (terrain + the other
// decorations this same populate pass places). Mirrors the runtime's own
// documented approximation (block_cactus.c cactus_side_material_solid).
static bool dec_cactus_neighbour_solid(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:
		case BLOCK_WATER_FLOW:
		case BLOCK_WATER_STILL:
		case BLOCK_LAVA_FLOW:
		case BLOCK_LAVA_STILL:
		case BLOCK_FIRE:
		case BLOCK_PORTAL:
		case BLOCK_TORCH:
		case BLOCK_REDSTONE_WIRE:
		case BLOCK_SAPLING:
		case BLOCK_TALL_GRASS:
		case DEC_DEADBUSH:
		case BLOCK_FLOWER:
		case BLOCK_ROSE:
		case BLOCK_BROWM_MUSHROOM:
		case BLOCK_RED_MUSHROOM:
		case BLOCK_CROPS:
		case BLOCK_REED:
		case BLOCK_SNOW: // the snow LAYER (MaterialLogic), not a solid block
			return false;
		default: return true; // Material.isSolid() defaults true (incl. cactus itself)
	}
}

// Material.water: Block.waterMoving (8) AND Block.waterStill (9)
// [Block.java:31-32] -- no other Beta 1.7.3 block has this material (ice is
// Material.ice). Used wherever Beta tests getBlockMaterial(...) == Material.water.
static bool dec_is_water(uint8_t id) {
	return id == BLOCK_WATER_FLOW || id == BLOCK_WATER_STILL;
}

// Beta canBlockStay for the decoration plant `type` at (x,y,z): does the block
// below support it? Placement-only -- never affects the Random stream.
static bool dec_can_stay(uint8_t type, struct wgen_world* w, int x, int y,
						 int z) {
	uint8_t below = w->get(w->ctx, x, y - 1, z);
	switch(type) {
		case DEC_DANDELION:
		case DEC_ROSE:
		case DEC_TALLGRASS:
		case DEC_DEADBUSH:
			/* CFR BlockFlower.java:53-55 / BlockDeadBush.java:29-31:
			 * use actual saved light or the maintained height's sky visibility.
			 * Original-JAR load(-8,3) rejects shaded grass at(-127,70,39). */
			if(w->full_light && w->full_light(w->ctx, x, y, z) < 8
			   && (!w->height || y < w->height(w->ctx, x, z))) return false;
			if(type == DEC_DEADBUSH) return below == BLOCK_SAND;
			return below == BLOCK_GRASS || below == BLOCK_DIRT || below == BLOCK_FARMLAND;
		case DEC_MUSH_BROWN:
		case DEC_MUSH_RED:
			/* CFR BlockMushroom.java:53-55: stored, not inferred light. */
			return (!w->full_light || w->full_light(w->ctx, x, y, z) < 13)
				&& dec_is_opaque(below);
		case DEC_SUGARCANE:
			// BlockReed.canPlaceBlockAt [BlockReed.java:33-36] (canBlockStay
			// delegates to it; WorldGenReed.generate:14-16 gates each segment):
			// reed below, OR grass/dirt below with Material.water on one of the
			// four sides of that base block. Sand is NOT a Beta 1.7.3 reed base
			// (REED-SAND-DIAG-0926 -- later versions added it, 1.7.3 did not).
			if(below == DEC_SUGARCANE)
				return true;
			if(below != BLOCK_GRASS && below != BLOCK_DIRT)
				return false;
			return dec_is_water(w->get(w->ctx, x - 1, y - 1, z))
				|| dec_is_water(w->get(w->ctx, x + 1, y - 1, z))
				|| dec_is_water(w->get(w->ctx, x, y - 1, z - 1))
				|| dec_is_water(w->get(w->ctx, x, y - 1, z + 1));
		case DEC_CACTUS:
			// Beta BlockCactus.canBlockStay checks all 4 lateral neighbours
			// BEFORE the below-block test (WorldGenCactus.generate calls this
			// full check per stacked segment, not just a below-block probe --
			// see decorate.c's file header: this is the one CanStay the
			// register found actually needed the full Beta gate, since the
			// runtime onNeighbourBlockChange/onRandomTick backstop only masks
			// worldgen placing an impossible-in-Beta cactus, it doesn't
			// prevent it).
			if(dec_cactus_neighbour_solid(w->get(w->ctx, x - 1, y, z))
			   || dec_cactus_neighbour_solid(w->get(w->ctx, x + 1, y, z))
			   || dec_cactus_neighbour_solid(w->get(w->ctx, x, y, z - 1))
			   || dec_cactus_neighbour_solid(w->get(w->ctx, x, y, z + 1)))
				return false;
			return below == DEC_CACTUS || below == BLOCK_SAND;
		default: return below == BLOCK_GRASS || below == BLOCK_DIRT;
	}
}

// WorldGenFlowers (dandelion / rose / both mushrooms): 64 unconditional tries of
// a +-{8,4,8} jittered spot, place `type` over air that CanStay. 6 draws/try.
static void gen_flowers(struct wgen_world* w, struct java_random* r, uint8_t type,
						int px, int py, int pz) {
	for(int i = 0; i < 64; i++) {
		int xa = di(r, 8), xb = di(r, 8);
		int ya = di(r, 4), yb = di(r, 4);
		int za = di(r, 8), zb = di(r, 8);
		int ox = px + xa - xb, oy = py + ya - yb, oz = pz + za - zb;
		if(w->get(w->ctx, ox, oy, oz) == BLOCK_AIR
		   && dec_can_stay(type, w, ox, oy, oz))
			wgen_set_plain(w, ox, oy, oz, type);
	}
}

// Descend (no draws) to the first non-air/non-leaves block or the floor.
static int dec_descend(struct wgen_world* w, int px, int py, int pz) {
	while(py > 0) {
		uint8_t b = w->get(w->ctx, px, py, pz);
		if(b != BLOCK_AIR && b != BLOCK_LEAVES)
			break;
		py--;
	}
	return py;
}

// WorldGenTallGrass: drop to ground (no draws), then 128 unconditional tries.
static void gen_tallgrass(struct wgen_world* w, struct java_random* r,
						  uint8_t meta, int px, int py, int pz) {
	py = dec_descend(w, px, py, pz);
	for(int i = 0; i < 128; i++) {
		int xa = di(r, 8), xb = di(r, 8);
		int ya = di(r, 4), yb = di(r, 4);
		int za = di(r, 8), zb = di(r, 8);
		int ox = px + xa - xb, oy = py + ya - yb, oz = pz + za - zb;
		if(w->get(w->ctx, ox, oy, oz) == BLOCK_AIR
		   && dec_can_stay(DEC_TALLGRASS, w, ox, oy, oz))
			w->set(w->ctx, ox, oy, oz, DEC_TALLGRASS, meta);
	}
}

// WorldGenDeadBush: drop to ground (no draws), then 4 unconditional tries.
static void gen_deadbush(struct wgen_world* w, struct java_random* r, int px,
						 int py, int pz) {
	py = dec_descend(w, px, py, pz);
	for(int i = 0; i < 4; i++) {
		int xa = di(r, 8), xb = di(r, 8);
		int ya = di(r, 4), yb = di(r, 4);
		int za = di(r, 8), zb = di(r, 8);
		int ox = px + xa - xb, oy = py + ya - yb, oz = pz + za - zb;
		if(w->get(w->ctx, ox, oy, oz) == BLOCK_AIR
		   && dec_can_stay(DEC_DEADBUSH, w, ox, oy, oz))
			wgen_set_plain(w, ox, oy, oz, DEC_DEADBUSH);
	}
}

// WorldGenReed: 20 tries; y is NOT redrawn (uses the seed y). 2 draws/try, plus
// a data-dependent 2-draw height ONLY on a water-adjacent air spot. The spot
// gate is getBlockMaterial(..) == Material.water [WorldGenReed.java:10], i.e.
// flowing (8) OR still (9) water: a still-only test skipped the two height
// draws beside a spring's flowing source and shifted the rest of the chunk's
// decoration stream (REED-SAND-DIAG-0926, secondary finding).
static void gen_sugarcane(struct wgen_world* w, struct java_random* r, int px,
						  int py, int pz) {
	for(int i = 0; i < 20; i++) {
		int xa = di(r, 4), xb = di(r, 4);
		int ox = px + xa - xb;
		int za = di(r, 4), zb = di(r, 4);
		int oz = pz + za - zb;
		if(w->get(w->ctx, ox, py, oz) != BLOCK_AIR)
			continue;
		bool water = dec_is_water(w->get(w->ctx, ox - 1, py - 1, oz))
			|| dec_is_water(w->get(w->ctx, ox + 1, py - 1, oz))
			|| dec_is_water(w->get(w->ctx, ox, py - 1, oz - 1))
			|| dec_is_water(w->get(w->ctx, ox, py - 1, oz + 1));
		if(!water)
			continue;
		int inner = di(r, 3);
		int height = 2 + di(r, inner + 1);
		for(int h = 0; h < height; h++)
			if(dec_can_stay(DEC_SUGARCANE, w, ox, py + h, oz))
				wgen_set_plain(w, ox, py + h, oz, DEC_SUGARCANE);
	}
}

// WorldGenPumpkin: 64 tries (6 draws each), plus a data-dependent facing meta
// draw ONLY when the spot is air over grass.
static void gen_pumpkins(struct wgen_world* w, struct java_random* r, int px,
						 int py, int pz) {
	for(int i = 0; i < 64; i++) {
		int xa = di(r, 8), xb = di(r, 8);
		int ya = di(r, 4), yb = di(r, 4);
		int za = di(r, 8), zb = di(r, 8);
		int ox = px + xa - xb, oy = py + ya - yb, oz = pz + za - zb;
		if(w->get(w->ctx, ox, oy, oz) == BLOCK_AIR
		   && w->get(w->ctx, ox, oy - 1, oz) == BLOCK_GRASS) {
			uint8_t facing = (uint8_t)di(r, 4);
			w->set(w->ctx, ox, oy, oz, DEC_PUMPKIN, facing);
		}
	}
}

// WorldGenCactus: 10 tries (6 draws each), plus a data-dependent 2-draw height
// ONLY on an air spot.
static void gen_cacti(struct wgen_world* w, struct java_random* r, int px,
					  int py, int pz) {
	for(int i = 0; i < 10; i++) {
		int xa = di(r, 8), xb = di(r, 8);
		int ya = di(r, 4), yb = di(r, 4);
		int za = di(r, 8), zb = di(r, 8);
		int ox = px + xa - xb, oy = py + ya - yb, oz = pz + za - zb;
		if(w->get(w->ctx, ox, oy, oz) != BLOCK_AIR)
			continue;
		int inner = di(r, 3);
		int height = 1 + di(r, inner + 1);
		for(int h = 0; h < height; h++)
			if(dec_can_stay(DEC_CACTUS, w, ox, oy + h, oz))
				wgen_set_plain(w, ox, oy + h, oz, DEC_CACTUS);
	}
}

// WorldGenLiquids: a one-block spring. Draws NOTHING (all randomness is the
// position in the caller). Places `fluid` only in a stone pocket open on exactly
// one lateral side.
static void gen_spring(struct wgen_world* w, struct java_random* r,
					   uint8_t fluid, int px, int py, int pz) {
	if(w->get(w->ctx, px, py + 1, pz) != BLOCK_STONE)
		return;
	if(w->get(w->ctx, px, py - 1, pz) != BLOCK_STONE)
		return;
	uint8_t c = w->get(w->ctx, px, py, pz);
	if(c != BLOCK_AIR && c != BLOCK_STONE)
		return;
	uint8_t n[4] = {
		w->get(w->ctx, px - 1, py, pz),
		w->get(w->ctx, px + 1, py, pz),
		w->get(w->ctx, px, py, pz - 1),
		w->get(w->ctx, px, py, pz + 1),
	};
	int stone = 0, air = 0;
	for(int k = 0; k < 4; k++) {
		if(n[k] == BLOCK_STONE)
			stone++;
		if(n[k] == BLOCK_AIR)
			air++;
	}
	if(stone == 3 && air == 1) {
		wgen_set_plain(w, px, py, pz, fluid);
		if(w->tick_fluid_immediate)
			w->tick_fluid_immediate(w->ctx, r, px, py, pz, fluid);
	}
}

void worldgen_decorate(struct wgen_world* w, struct java_random* r,
					   enum biome b, int block_x, int block_z) {
	int dandelions = 0, grass = 0, deadbush = 0, cacti = 0;
	switch(b) {
		case BIOME_TAIGA:
		case BIOME_FOREST: dandelions = 2; break;
		case BIOME_SEASONAL_FOREST: dandelions = 4; break;
		case BIOME_PLAINS: dandelions = 3; break;
		default: break;
	}
	switch(b) {
		case BIOME_SEASONAL_FOREST:
		case BIOME_FOREST: grass = 2; break;
		case BIOME_PLAINS:
		case BIOME_RAINFOREST: grass = 10; break;
		case BIOME_TAIGA: grass = 1; break;
		default: break;
	}
	if(b == BIOME_DESERT) {
		deadbush = 2;
		cacti = 10;
	}

	// Dandelions.
	for(int i = 0; i < dandelions; i++) {
		int x = block_x + di(r, 16) + 8;
		int y = di(r, 128);
		int z = block_z + di(r, 16) + 8;
		gen_flowers(w, r, DEC_DANDELION, x, y, z);
	}

	// Tall grass / fern (the fern meta roll fires ONLY in rainforest).
	for(int i = 0; i < grass; i++) {
		uint8_t meta = 1; // grass
		if(b == BIOME_RAINFOREST && di(r, 3) != 0)
			meta = 2; // fern
		int x = block_x + di(r, 16) + 8;
		int y = di(r, 128);
		int z = block_z + di(r, 16) + 8;
		gen_tallgrass(w, r, meta, x, y, z);
	}

	// Dead bushes (desert).
	for(int i = 0; i < deadbush; i++) {
		int x = block_x + di(r, 16) + 8;
		int y = di(r, 128);
		int z = block_z + di(r, 16) + 8;
		gen_deadbush(w, r, x, y, z);
	}

	// Roses (1-in-2), brown mushrooms (1-in-4), red mushrooms (1-in-8). The gate
	// roll is always drawn; the position + body only on success.
	if(di(r, 2) == 0) {
		int x = block_x + di(r, 16) + 8, y = di(r, 128), z = block_z + di(r, 16) + 8;
		gen_flowers(w, r, DEC_ROSE, x, y, z);
	}
	if(di(r, 4) == 0) {
		int x = block_x + di(r, 16) + 8, y = di(r, 128), z = block_z + di(r, 16) + 8;
		gen_flowers(w, r, DEC_MUSH_BROWN, x, y, z);
	}
	if(di(r, 8) == 0) {
		int x = block_x + di(r, 16) + 8, y = di(r, 128), z = block_z + di(r, 16) + 8;
		gen_flowers(w, r, DEC_MUSH_RED, x, y, z);
	}

	// Sugar cane: 10 tries.
	for(int i = 0; i < 10; i++) {
		int x = block_x + di(r, 16) + 8;
		int y = di(r, 128);
		int z = block_z + di(r, 16) + 8;
		gen_sugarcane(w, r, x, y, z);
	}

	// Pumpkin patch (1-in-32).
	if(di(r, 32) == 0) {
		int x = block_x + di(r, 16) + 8, y = di(r, 128), z = block_z + di(r, 16) + 8;
		gen_pumpkins(w, r, x, y, z);
	}

	// Cacti (desert).
	for(int i = 0; i < cacti; i++) {
		int x = block_x + di(r, 16) + 8;
		int y = di(r, 128);
		int z = block_z + di(r, 16) + 8;
		gen_cacti(w, r, x, y, z);
	}

	// One-block water springs (50). y is the doubly-nested nextInt; compute the
	// inner bound FIRST (C does not order nested calls like Java).
	for(int i = 0; i < 50; i++) {
		int x = block_x + di(r, 16) + 8;
		int inner = di(r, 120);
		int y = di(r, inner + 8);
		int z = block_z + di(r, 16) + 8;
		gen_spring(w, r, DEC_WATER, x, y, z);
	}

	// One-block lava springs (20). y is the TRIPLY-nested nextInt.
	for(int i = 0; i < 20; i++) {
		int x = block_x + di(r, 16) + 8;
		int i0 = di(r, 112);
		int i1 = di(r, i0 + 8);
		int y = di(r, i1 + 8);
		int z = block_z + di(r, 16) + 8;
		gen_spring(w, r, DEC_LAVA, x, y, z);
	}

	// SNOW pass is worldgen_snow() below: it draws no randomness (so its position
	// in the sequence can't desync) and needs the per-column temperature, so the
	// caller samples that from the generator and runs it after this.
}

// Beta Material.isSolid: a surface snow can rest on (not air, fluids, or the
// passable/replaceable plant + logic blocks). Placement-only, no draws.
static bool snow_is_solid(uint8_t id) {
	switch(id) {
		case BLOCK_AIR:
		case BLOCK_WATER_FLOW:
		case BLOCK_WATER_STILL:
		case BLOCK_LAVA_FLOW:
		case BLOCK_LAVA_STILL:
		case BLOCK_SAPLING:
		case BLOCK_TALL_GRASS:
		case DEC_DEADBUSH:
		case BLOCK_FLOWER:
		case BLOCK_ROSE:
		case BLOCK_BROWM_MUSHROOM:
		case BLOCK_RED_MUSHROOM:
		case BLOCK_TORCH:
		case BLOCK_FIRE:
		case BLOCK_REDSTONE_WIRE:
		case BLOCK_CROPS:
		case BLOCK_SNOW:
		case BLOCK_REED:
		case BLOCK_PORTAL:
			return false;
		default: return true;
	}
}

void worldgen_snow(struct wgen_world* w, const double* temp, int block_x,
				   int block_z) {
	int bx = block_x + 8, bz = block_z + 8; // the +8 decorate region
	for(int lx = 0; lx < 16; lx++) {
		for(int lz = 0; lz < 16; lz++) {
			int x = bx + lx, z = bz + lz;
			// Beta getPrecipitationHeight: highest solid surface via a fresh
			// top-down scan (NOT the stale terrain heightmap), so snow lands on
			// the tree canopies populate just placed, not the ground beneath.
			int highest = 0;
			for(int y = 127; y >= 0 && !highest; y--)
				if(snow_is_solid(w->get(w->ctx, x, y, z)))
					highest = y + 1;
			// Altitude lapse -- higher columns read colder (Beta's 0.3/64).
			double t = temp[lx * 16 + lz] - (double)(highest - 64) / 64.0 * 0.3;
			if(t >= 0.5 || highest <= 0 || highest >= 128)
				continue;
			if(w->get(w->ctx, x, highest, z) != BLOCK_AIR)
				continue;
			uint8_t below = w->get(w->ctx, x, highest - 1, z);
			if(snow_is_solid(below) && below != BLOCK_ICE)
				wgen_set_plain_notify(w, x, highest, z, BLOCK_SNOW); // snow layer (78)
		}
	}
}
