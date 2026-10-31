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

#ifndef NETWORK_EXPLOSION_MODEL_H
#define NETWORK_EXPLOSION_MODEL_H

// Pure, world-free pieces of Beta 1.7.3 Explosion.doExplosionA / World.func_675_a
// (block density) that the goldens pin directly, with no copy to drift from the
// server_world.c blast that consumes them. Header-only `static inline` per
// CODESTYLE 1.5 (shared logic tests must pin on the exact production mapping).
//
// Two float-vs-double conventions are load-bearing here and must not be
// "simplified": Beta evaluates ray intensity/resistance in FLOAT and ray
// position/direction in DOUBLE; `-ffp-contract=off` guards the rest.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../worldgen/jmath.h" // java_double_to_int (Java (int) cast)

// Beta Explosion.doExplosionA ray grid: 16 x 16 x 16 lattice, only the cube
// SURFACE cells cast a ray -> 16^3 - 14^3 = 1352 rays per blast, in x/y/z loop
// order (this is also the World.rand draw order: exactly one nextFloat per ray).
// [740c583 Explosion.doExplosionA:31-50]
#define EXPLOSION_GRID 16
#define EXPLOSION_RAY_COUNT 1352
// Ray march step and the per-step decay (step * 12/16 = 0.225F).
// [740c583 Explosion.doExplosionA:55]
#define EXPLOSION_RAY_STEP 0.3F

// Beta Block.getExplosionResistance = blockResistance / 5.0F, with blockResistance
// produced by the Block.java setter chain: setHardness(h) raises it to h*5.0F
// when lower; setResistance(r) then overwrites it with r*3.0F; a block with
// neither keeps 0. Each row is the (h, r) pair read straight off the
// `public static final Block ...` line for that id (r = -1 -> no setResistance
// call); BlockStairs copies its model block's pair (BlockStairs.java:12-13), the
// piston bases/extension setHardness(0.5F) in their constructors, pistonMoving
// and portal setHardness(-1.0F) (0 resistance), cloth (35) is setHardness(0.8F).
// [740c583 Block.java:24-113 (per-block lines), Block.setResistance:186,
//  Block.setHardness:199-205, Block.getExplosionResistance:353]
struct explosion_block_res {
	float hardness;
	float resistance; // -1 = no setResistance call on that block
};

static inline struct explosion_block_res
explosion_block_res_row(uint8_t id) {
	switch(id) {
		case 1: return (struct explosion_block_res) {1.5F, 10.0F}; // stone
		case 2: return (struct explosion_block_res) {0.6F, -1.0F}; // grass
		case 3: return (struct explosion_block_res) {0.5F, -1.0F}; // dirt
		case 4: return (struct explosion_block_res) {2.0F, 10.0F}; // cobble
		case 5: return (struct explosion_block_res) {2.0F, 5.0F}; // planks
		case 6: return (struct explosion_block_res) {0.0F, -1.0F}; // sapling
		case 7: return (struct explosion_block_res) {-1.0F, 6000000.0F}; // bedrock
		case 8: return (struct explosion_block_res) {100.0F, -1.0F}; // water
		case 9: return (struct explosion_block_res) {100.0F, -1.0F};
		case 10: return (struct explosion_block_res) {0.0F, -1.0F}; // lava flowing
		case 11: return (struct explosion_block_res) {100.0F, -1.0F}; // lava still
		case 12: return (struct explosion_block_res) {0.5F, -1.0F}; // sand
		case 13: return (struct explosion_block_res) {0.6F, -1.0F}; // gravel
		case 14: return (struct explosion_block_res) {3.0F, 5.0F}; // gold ore
		case 15: return (struct explosion_block_res) {3.0F, 5.0F}; // iron ore
		case 16: return (struct explosion_block_res) {3.0F, 5.0F}; // coal ore
		case 17: return (struct explosion_block_res) {2.0F, -1.0F}; // log
		case 18: return (struct explosion_block_res) {0.2F, -1.0F}; // leaves
		case 19: return (struct explosion_block_res) {0.6F, -1.0F}; // sponge
		case 20: return (struct explosion_block_res) {0.3F, -1.0F}; // glass
		case 21: return (struct explosion_block_res) {3.0F, 5.0F}; // lapis ore
		case 22: return (struct explosion_block_res) {3.0F, 5.0F}; // lapis block
		case 23: return (struct explosion_block_res) {3.5F, -1.0F}; // dispenser
		case 24: return (struct explosion_block_res) {0.8F, -1.0F}; // sandstone
		case 25: return (struct explosion_block_res) {0.8F, -1.0F}; // noteblock
		case 26: return (struct explosion_block_res) {0.2F, -1.0F}; // bed
		case 27: return (struct explosion_block_res) {0.7F, -1.0F}; // powered rail
		case 28: return (struct explosion_block_res) {0.7F, -1.0F}; // detector rail
		case 29: return (struct explosion_block_res) {0.5F, -1.0F}; // sticky piston
		case 30: return (struct explosion_block_res) {4.0F, -1.0F}; // web
		case 31: return (struct explosion_block_res) {0.0F, -1.0F}; // tall grass
		case 32: return (struct explosion_block_res) {0.0F, -1.0F}; // dead bush
		case 33: return (struct explosion_block_res) {0.5F, -1.0F}; // piston
		case 34: return (struct explosion_block_res) {0.5F, -1.0F}; // piston head
		case 35: return (struct explosion_block_res) {0.8F, -1.0F}; // wool
		case 36: return (struct explosion_block_res) {-1.0F, -1.0F}; // piston moving
		case 37: return (struct explosion_block_res) {0.0F, -1.0F}; // dandelion
		case 38: return (struct explosion_block_res) {0.0F, -1.0F}; // rose
		case 39: return (struct explosion_block_res) {0.0F, -1.0F}; // brown mushroom
		case 40: return (struct explosion_block_res) {0.0F, -1.0F}; // red mushroom
		case 41: return (struct explosion_block_res) {3.0F, 10.0F}; // gold block
		case 42: return (struct explosion_block_res) {5.0F, 10.0F}; // iron block
		case 43: return (struct explosion_block_res) {2.0F, 10.0F}; // double slab
		case 44: return (struct explosion_block_res) {2.0F, 10.0F}; // slab
		case 45: return (struct explosion_block_res) {2.0F, 10.0F}; // brick
		case 46: return (struct explosion_block_res) {0.0F, -1.0F}; // tnt
		case 47: return (struct explosion_block_res) {1.5F, -1.0F}; // bookshelf
		case 48: return (struct explosion_block_res) {2.0F, 10.0F}; // mossy cobble
		case 49: return (struct explosion_block_res) {10.0F, 2000.0F}; // obsidian
		case 50: return (struct explosion_block_res) {0.0F, -1.0F}; // torch
		case 51: return (struct explosion_block_res) {0.0F, -1.0F}; // fire
		case 52: return (struct explosion_block_res) {5.0F, -1.0F}; // mob spawner
		case 53: return (struct explosion_block_res) {2.0F, 5.0F}; // wood stairs
		case 54: return (struct explosion_block_res) {2.5F, -1.0F}; // chest
		case 55: return (struct explosion_block_res) {0.0F, -1.0F}; // redstone wire
		case 56: return (struct explosion_block_res) {3.0F, 5.0F}; // diamond ore
		case 57: return (struct explosion_block_res) {5.0F, 10.0F}; // diamond block
		case 58: return (struct explosion_block_res) {2.5F, -1.0F}; // workbench
		case 59: return (struct explosion_block_res) {0.0F, -1.0F}; // crops
		case 60: return (struct explosion_block_res) {0.6F, -1.0F}; // farmland
		case 61: return (struct explosion_block_res) {3.5F, -1.0F}; // furnace
		case 62: return (struct explosion_block_res) {3.5F, -1.0F}; // lit furnace
		case 63: return (struct explosion_block_res) {1.0F, -1.0F}; // sign post
		case 64: return (struct explosion_block_res) {3.0F, -1.0F}; // wood door
		case 65: return (struct explosion_block_res) {0.4F, -1.0F}; // ladder
		case 66: return (struct explosion_block_res) {0.7F, -1.0F}; // rail
		case 67: return (struct explosion_block_res) {2.0F, 10.0F}; // cobble stairs
		case 68: return (struct explosion_block_res) {1.0F, -1.0F}; // wall sign
		case 69: return (struct explosion_block_res) {0.5F, -1.0F}; // lever
		case 70: return (struct explosion_block_res) {0.5F, -1.0F}; // stone plate
		case 71: return (struct explosion_block_res) {5.0F, -1.0F}; // iron door
		case 72: return (struct explosion_block_res) {0.5F, -1.0F}; // wood plate
		case 73: return (struct explosion_block_res) {3.0F, 5.0F}; // redstone ore
		case 74: return (struct explosion_block_res) {3.0F, 5.0F}; // lit redstone ore
		case 75: return (struct explosion_block_res) {0.0F, -1.0F}; // redstone torch off
		case 76: return (struct explosion_block_res) {0.0F, -1.0F}; // redstone torch on
		case 77: return (struct explosion_block_res) {0.5F, -1.0F}; // button
		case 78: return (struct explosion_block_res) {0.1F, -1.0F}; // snow layer
		case 79: return (struct explosion_block_res) {0.5F, -1.0F}; // ice
		case 80: return (struct explosion_block_res) {0.2F, -1.0F}; // snow block
		case 81: return (struct explosion_block_res) {0.4F, -1.0F}; // cactus
		case 82: return (struct explosion_block_res) {0.6F, -1.0F}; // clay
		case 83: return (struct explosion_block_res) {0.0F, -1.0F}; // reed
		case 84: return (struct explosion_block_res) {2.0F, 10.0F}; // jukebox
		case 85: return (struct explosion_block_res) {2.0F, 5.0F}; // fence
		case 86: return (struct explosion_block_res) {1.0F, -1.0F}; // pumpkin
		case 87: return (struct explosion_block_res) {0.4F, -1.0F}; // netherrack
		case 88: return (struct explosion_block_res) {0.5F, -1.0F}; // soul sand
		case 89: return (struct explosion_block_res) {0.3F, -1.0F}; // glowstone
		case 90: return (struct explosion_block_res) {-1.0F, -1.0F}; // portal
		case 91: return (struct explosion_block_res) {1.0F, -1.0F}; // jack-o-lantern
		case 92: return (struct explosion_block_res) {0.5F, -1.0F}; // cake
		case 93: return (struct explosion_block_res) {0.0F, -1.0F}; // repeater off
		case 94: return (struct explosion_block_res) {0.0F, -1.0F}; // repeater on
		case 95: return (struct explosion_block_res) {0.0F, -1.0F}; // locked chest
		case 96: return (struct explosion_block_res) {3.0F, -1.0F}; // trapdoor
		default: return (struct explosion_block_res) {0.0F, -1.0F}; // no Beta block
	}
}

// Block.getExplosionResistance(Entity) for an id: the exact float setter chain.
// Unregistered ids (blocksList[id] == null in Beta would NPE; here: 0).
static inline float explosion_block_resistance(uint8_t id) {
	struct explosion_block_res row = explosion_block_res_row(id);
	float blockResistance = 0.0F;
	// setHardness: `if(this.blockResistance < var1 * 5.0F) this.blockResistance = var1 * 5.0F;`
	if(blockResistance < row.hardness * 5.0F)
		blockResistance = row.hardness * 5.0F;
	// setResistance: `this.blockResistance = var1 * 3.0F;`
	if(row.resistance >= 0.0F)
		blockResistance = row.resistance * 3.0F;
	return blockResistance / 5.0F;
}

// One grid ray's unit direction, Beta's exact double chain:
//   double var6 = (double)((float)var3 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
//   ... var12 = Math.sqrt(var6*var6 + var8*var8 + var10*var10); var6 /= var12; ...
// [740c583 Explosion.doExplosionA:43-49]
static inline void explosion_ray_dir(int gx, int gy, int gz, double out[3]) {
	double dx = (double)((float)gx / ((float)EXPLOSION_GRID - 1.0F) * 2.0F - 1.0F);
	double dy = (double)((float)gy / ((float)EXPLOSION_GRID - 1.0F) * 2.0F - 1.0F);
	double dz = (double)((float)gz / ((float)EXPLOSION_GRID - 1.0F) * 2.0F - 1.0F);
	double len = sqrt(dx * dx + dy * dy + dz * dz);
	out[0] = dx / len;
	out[1] = dy / len;
	out[2] = dz / len;
}

// True iff lattice cell (gx,gy,gz) lies on the cube surface (casts a ray).
// [740c583 Explosion.doExplosionA:42]
static inline bool explosion_ray_cell(int gx, int gy, int gz) {
	return gx == 0 || gx == EXPLOSION_GRID - 1 || gy == 0
		|| gy == EXPLOSION_GRID - 1 || gz == 0 || gz == EXPLOSION_GRID - 1;
}

// Per-ray starting intensity: `this.explosionSize * (0.7F + rand.nextFloat() * 0.6F)`
// (float chain). [740c583 Explosion.doExplosionA:50]
static inline float explosion_ray_intensity(float size, float next_float) {
	return size * (0.7F + next_float * 0.6F);
}

// The entity-pass damage, in Beta's raw entity damage points (mob 20-scale;
// the player caller converts x8 at the boundary). `dist_over_size` is Beta's
// distance ratio = getDistance(center)/explosionSize (with explosionSize ALREADY doubled),
// `density` is World.func_675_a's exposure in [0,1]. Returns the impulse through
// `out_impulse` (the motion impulse magnitude along the unit direction) and the
// damage: (int)((i*i + i) / 2.0D * 8.0D * (double)size2 + 1.0D), i = the impulse.
// [740c583 Explosion.doExplosionA:98-103]
static inline int explosion_entity_damage(double dist_over_size, float density,
										  float size2, double* out_impulse) {
	double var41 = (1.0D - dist_over_size) * (double)density;
	if(out_impulse)
		*out_impulse = var41;
	return java_double_to_int((var41 * var41 + var41) / 2.0D * 8.0D
							  * (double)size2 + 1.0D);
}

#endif
