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
	Beta 1.7.3 BlockSand/BlockGravel falling-block mechanic (batch I, audit
	register worldgen-saves #1). BlockGravel extends BlockSand in Beta and
	inherits onBlockAdded/onNeighborBlockChange/updateTick/tryToFall/
	canFallBelow UNCHANGED (only idDropped -- the 1/10 flint roll -- differs,
	already implemented in block_gravel.c's getDroppedItem). This header is the
	shared surface both block_sand.c and block_gravel.c wire their hooks
	through, and that entity_falling_sand.c's landing check calls back into.

	[decompile 740c583 BlockSand.java, BlockGravel.java]
*/

#ifndef BLOCK_SAND_H
#define BLOCK_SAND_H

#include <stdbool.h>
#include <stdint.h>

#include "blocks_data.h" // w_coord_t

struct server_local;

// Beta BlockSand.tickRate(): both onBlockAdded and onNeighborBlockChange
// schedule at this rate (the default Block.tickRate() is 10; BlockSand
// overrides it to 3). [decompile 740c583 BlockSand.tickRate]
#define BLOCK_SAND_TICK_RATE 3

// Beta EntityFallingSand's in-air give-up: fallTime > 100 with no landing --
// drop as an item and self-destroy (falling forever off the edge of loaded
// terrain, or into a void). [decompile 740c583 EntityFallingSand.onUpdate]
#define FALLING_SAND_VOID_GIVE_UP_TICKS 100

// Beta BlockSand.canFallBelow(world,x,y,z): true iff the cell is air, fire,
// or water/lava (moving or still) -- the direct id-based reimplementation of
// Beta's `blockMaterial == Material.water || blockMaterial == Material.lava`
// (this engine's own getMaterial() is a coarser render/footstep-only bucket
// that folds water AND lava into the SAME MATERIAL_STONE value as ordinary
// stone -- verified by reading every block_*.c getMaterial() implementation
// -- so it cannot stand in for Beta's Material identity check here; direct
// block-id comparison is the faithful, and only correct, translation).
// [decompile 740c583 BlockSand.canFallBelow]
bool block_sand_can_fall_below(struct server_local* s, w_coord_t x, w_coord_t y,
							   w_coord_t z);

// Beta World.canBlockBePlacedAt(blockID, x, y, z, ignoreEntityCollision=true,
// side=1), as called by EntityFallingSand's landing check. With
// ignoreEntityCollision=true the AABB/entity-collision branch is skipped
// entirely, leaving two independent checks against the block CURRENTLY at
// (x,y,z): (a) it must be air, water/lava (moving or still), fire, or the
// thin snow layer (those four material kinds are treated as "null" by the
// outer check); AND (b) Block.canPlaceBlockOnSide -> the default
// Block.canPlaceBlockAt: `id==0 || material.getIsGroundCover()`. Only
// Material.snow (the thin layer, block id 78 -- NOT the solid snow block, id
// 80) sets groundCover in Beta's static init; water/lava/fire do not. The AND
// of (a) and (b) therefore collapses to exactly "air, or the thin snow
// layer" -- a falling block that lands IN water, lava, or fire FAILS to
// place and drops as an item instead, matching Beta exactly (a well-known
// period-accurate quirk, not an omission). [decompile 740c583
// World.canBlockBePlacedAt, Block.canPlaceBlockOnSide, Block.canPlaceBlockAt,
// Material's static init]
bool block_sand_can_place_at(struct server_local* s, w_coord_t x, w_coord_t y,
							 w_coord_t z);

// Beta BlockSand.tryToFall, fired from the block's scheduled tick
// (onBlockAdded/onNeighborBlockChange -> tickRate()==3 -> updateTick). If the
// cell below is passable (block_sand_can_fall_below) and y>=0: when the
// 32-radius area around (x,y,z) is loaded, spawns an EntityFallingSand
// (server_local_spawn_falling_sand); otherwise (Beta's `!fallInstantly &&
// checkChunksExist` combined gate reduced to its only reachable branch here
// -- see block_sand.c for why fallInstantly is never true on any path this
// port's worldgen can reach) settles the column directly: clear (x,y,z) to
// air, then walk straight down while the cell below stays passable, and
// re-place the carried block at the first supported cell (or nowhere, if the
// search reaches the bottom of the world -- Beta's own "falls into
// bedrock/void" outcome for this branch). `block_id` is the carried block
// (BLOCK_SAND or BLOCK_GRAVEL). [decompile 740c583 BlockSand.tryToFall]
void block_sand_try_to_fall(struct server_local* s, w_coord_t x, w_coord_t y,
							w_coord_t z, uint8_t block_id);

#endif
