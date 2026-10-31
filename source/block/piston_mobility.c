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

#include "piston_mobility.h"

#include <stdint.h>

#include "blocks_data.h"
#include "redstone_piston.h" // RS_MOBILITY_* + RS_PISTON_*_ID (pinned below)

// Beta 1.7.3 Block.getMobilityFlag() == blockMaterial.getMaterialMobility() (Block.java:588), EXCEPT the
// six overrides found by `git grep getMobilityFlag`: BlockBed->1, BlockDoor->1, BlockPressurePlate->1,
// BlockIce->0, BlockRail->0, and BlockPistonBase (no override -> Material.piston=2). The flag-1 materials
// (Material.java setNoPushMobility): water, lava, leaves, plants, fire, circuits, snow(LAYER), cactus,
// pumpkin, web, cake, coral. Flag-2 (setImmovableMobility): portal, piston. Default 0 (movable).
//
// C99 designated-initializer const table: a compile-time constant id-keyed by the blocks_data.h enum, so
// a renumber/delete of any referenced id is a COMPILE error -- the strongest possible pin. No lazy init
// (no race on the single-threaded Wii server tick). Entries cite the Beta Material/override that sets them.
//
// DELIBERATELY ABSENT (do NOT add):
//  - tile-entity blocks (chest 54, iron_chest 95, furnace 61/62, dispenser 23, noteblock 25,
//    jukebox 84, spawner 52, signs 63/68): flag 0 -- the push is blocked by canPushBlock's final
//    getBlockTileEntity()==null gate (ps_has_tile_entity), NOT by a mobility flag.
//  - obsidian 49 / bedrock 7: special-cased earlier in piston_can_push_block (id==obsidian; hardness==-1).
static const uint8_t s_mobility[256] = {
	// --- flag 0 (normal mobility; both BlockSign ids are stopped by their TE gate) ---
	[BLOCK_SIGN_POST] = RS_MOBILITY_NORMAL,
	[BLOCK_SIGN] = RS_MOBILITY_NORMAL,
	// --- flag 1 (destroy-on-push: a movable but non-pushed block that ENDS a push chain) ---
	[BLOCK_SAPLING] = RS_MOBILITY_DESTROY,				// Material.plants
	[BLOCK_DEADBUSH] = RS_MOBILITY_DESTROY,				// Material.plants (BlockDeadBush : BlockFlower)
	[BLOCK_COBWEB] = RS_MOBILITY_DESTROY,				// Material.web
	[BLOCK_CAKE] = RS_MOBILITY_DESTROY,					// Material.cakeMaterial
	[BLOCK_LADDER] = RS_MOBILITY_DESTROY,				// Material.circuits (BlockLadder, no override)
	[BLOCK_LEVER] = RS_MOBILITY_DESTROY,				// Material.circuits (BlockLever)
	[BLOCK_STONE_BUTTON] = RS_MOBILITY_DESTROY,		// Material.circuits (BlockButton)
	[BLOCK_WATER_FLOW] = RS_MOBILITY_DESTROY,			// Material.water
	[BLOCK_WATER_STILL] = RS_MOBILITY_DESTROY,			// Material.water
	[BLOCK_LAVA_FLOW] = RS_MOBILITY_DESTROY,			// Material.lava
	[BLOCK_LAVA_STILL] = RS_MOBILITY_DESTROY,			// Material.lava
	[BLOCK_LEAVES] = RS_MOBILITY_DESTROY,				// Material.leaves
	[BLOCK_BED] = RS_MOBILITY_DESTROY,					// BlockBed.getMobilityFlag override -> 1
	[BLOCK_TALL_GRASS] = RS_MOBILITY_DESTROY,			// Material.plants (BlockTallGrass extends BlockFlower)
	[BLOCK_FLOWER] = RS_MOBILITY_DESTROY,				// Material.plants
	[BLOCK_ROSE] = RS_MOBILITY_DESTROY,					// Material.plants
	[BLOCK_BROWM_MUSHROOM] = RS_MOBILITY_DESTROY,		// Material.plants
	[BLOCK_RED_MUSHROOM] = RS_MOBILITY_DESTROY,			// Material.plants
	[BLOCK_TORCH] = RS_MOBILITY_DESTROY,				// Material.circuits (BlockTorch)
	[BLOCK_FIRE] = RS_MOBILITY_DESTROY,					// Material.fire
	[BLOCK_REDSTONE_WIRE] = RS_MOBILITY_DESTROY,		// Material.circuits
	[BLOCK_CROPS] = RS_MOBILITY_DESTROY,				// Material.plants (BlockCrops extends BlockFlower)
	[BLOCK_DOOR_WOOD] = RS_MOBILITY_DESTROY,			// BlockDoor.getMobilityFlag override -> 1
	[BLOCK_DOOR_IRON] = RS_MOBILITY_DESTROY,			// BlockDoor.getMobilityFlag override -> 1
	[BLOCK_STONE_PRESSURE_PLATE] = RS_MOBILITY_DESTROY, // BlockPressurePlate override -> 1
	[BLOCK_WOOD_PRESSURE_PLATE] = RS_MOBILITY_DESTROY,	// BlockPressurePlate override -> 1
	[BLOCK_REDSTONE_TORCH] = RS_MOBILITY_DESTROY,		// Material.circuits (BlockRedstoneTorch:BlockTorch)
	[BLOCK_REDSTONE_TORCH_LIT] = RS_MOBILITY_DESTROY,	// Material.circuits
	[BLOCK_SNOW] = RS_MOBILITY_DESTROY,					// Material.snow (the snow LAYER, not the snow block)
	[BLOCK_CACTUS] = RS_MOBILITY_DESTROY,				// Material.cactus
	[BLOCK_REED] = RS_MOBILITY_DESTROY,					// Material.plants (sugar cane)
	[BLOCK_PUMPKIN] = RS_MOBILITY_DESTROY,				// Material.pumpkin
	[BLOCK_PUMPKIN_LIT] = RS_MOBILITY_DESTROY,			// Material.pumpkin (jack-o-lantern)
	[BLOCK_REDSTONE_REPEATER] = RS_MOBILITY_DESTROY,	// Material.circuits (BlockRedstoneRepeater)
	[BLOCK_REDSTONE_REPEATER_LIT] = RS_MOBILITY_DESTROY, // Material.circuits
	// --- flag 2 (immovable: blocks a push entirely) ---
	[BLOCK_PORTAL] = RS_MOBILITY_IMMOVABLE,			   // Material.portal
	[BLOCK_PISTON_STICKY] = RS_MOBILITY_IMMOVABLE,	   // Material.piston (the powered-base exception is in
	[BLOCK_PISTON_BASE] = RS_MOBILITY_IMMOVABLE,	   //   piston_can_push_block, not here)
	[BLOCK_PISTON_HEAD] = RS_MOBILITY_IMMOVABLE,	   // Material.piston
	[BLOCK_PISTON_MOVING] = RS_MOBILITY_IMMOVABLE,	   // Material.piston (id-36 also blocked by its TE)
};

// The RS_PISTON_* / RS_MOBILITY_* names are #defines in redstone_piston.h (not pinned by the
// designated-init above) -- assert they still equal the engine enum so the table can't silently drift.
_Static_assert(RS_PISTON_STICKY_ID == BLOCK_PISTON_STICKY, "piston sticky id drift");
_Static_assert(RS_PISTON_BASE_ID == BLOCK_PISTON_BASE, "piston base id drift");
_Static_assert(RS_PISTON_HEAD_ID == BLOCK_PISTON_HEAD, "piston head id drift");
_Static_assert(RS_PISTON_MOVING_ID == BLOCK_PISTON_MOVING, "piston moving id drift");
_Static_assert(RS_PISTON_OBSIDIAN_ID == BLOCK_OBSIDIAN, "piston obsidian id drift");

int piston_mobility_flag(int id) {
	return (id >= 0 && id < 256) ? s_mobility[id] : RS_MOBILITY_NORMAL;
}
