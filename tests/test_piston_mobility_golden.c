/*
	Golden for the Beta 1.7.3 piston MOBILITY table (M4 pistons, phase 2a). Re-pins every non-zero entry
	of source/block/piston_mobility.c against the decompile (740c583): Material.getMaterialMobility() +
	the BlockBed/BlockDoor/BlockPressurePlate getMobilityFlag overrides. Guards against a silent table
	edit drifting from Beta (a faithfulness regression that is otherwise invisible). Also asserts the
	DELIBERATE absences (tile-entity blocks, obsidian/bedrock, the snow BLOCK vs the snow LAYER).
*/

#include <assert.h>
#include <stdio.h>

#include "block/blocks_data.h"
#include "block/piston_mobility.h"
#include "block/redstone_piston.h"

#define MOV RS_MOBILITY_NORMAL
#define DES RS_MOBILITY_DESTROY
#define IMM RS_MOBILITY_IMMOVABLE

int main(void) {
	// --- flag 1 (destroy-on-push), grouped by the Beta Material/override that sets them --------------
	// Material.plants
	assert(piston_mobility_flag(BLOCK_SAPLING) == DES);
	assert(piston_mobility_flag(BLOCK_DEADBUSH) == DES); // BlockDeadBush : BlockFlower (Material.plants)
	assert(piston_mobility_flag(BLOCK_TALL_GRASS) == DES);
	assert(piston_mobility_flag(BLOCK_FLOWER) == DES);
	assert(piston_mobility_flag(BLOCK_ROSE) == DES);
	assert(piston_mobility_flag(BLOCK_BROWM_MUSHROOM) == DES);
	assert(piston_mobility_flag(BLOCK_RED_MUSHROOM) == DES);
	assert(piston_mobility_flag(BLOCK_CROPS) == DES);
	assert(piston_mobility_flag(BLOCK_REED) == DES);
	// Material.water / lava
	assert(piston_mobility_flag(BLOCK_WATER_FLOW) == DES);
	assert(piston_mobility_flag(BLOCK_WATER_STILL) == DES);
	assert(piston_mobility_flag(BLOCK_LAVA_FLOW) == DES);
	assert(piston_mobility_flag(BLOCK_LAVA_STILL) == DES);
	// Material.leaves / fire / cactus / pumpkin / snow-LAYER
	assert(piston_mobility_flag(BLOCK_LEAVES) == DES);
	assert(piston_mobility_flag(BLOCK_FIRE) == DES);
	assert(piston_mobility_flag(BLOCK_CACTUS) == DES);
	assert(piston_mobility_flag(BLOCK_PUMPKIN) == DES);
	assert(piston_mobility_flag(BLOCK_PUMPKIN_LIT) == DES);
	assert(piston_mobility_flag(BLOCK_SNOW) == DES);
	// Material.circuits (the redstone family + torch)
	assert(piston_mobility_flag(BLOCK_TORCH) == DES);
	assert(piston_mobility_flag(BLOCK_REDSTONE_WIRE) == DES);
	assert(piston_mobility_flag(BLOCK_REDSTONE_TORCH) == DES);
	assert(piston_mobility_flag(BLOCK_REDSTONE_TORCH_LIT) == DES);
	assert(piston_mobility_flag(BLOCK_REDSTONE_REPEATER) == DES);	  // BlockRedstoneRepeater: circuits
	assert(piston_mobility_flag(BLOCK_REDSTONE_REPEATER_LIT) == DES);
	assert(piston_mobility_flag(BLOCK_LADDER) == DES);				  // BlockLadder: Material.circuits
	assert(piston_mobility_flag(BLOCK_LEVER) == DES);
	assert(piston_mobility_flag(BLOCK_STONE_BUTTON) == DES);
	// Material.web / Material.cakeMaterial
	assert(piston_mobility_flag(BLOCK_COBWEB) == DES);
	assert(piston_mobility_flag(BLOCK_CAKE) == DES);
	// the three Block-level getMobilityFlag overrides
	assert(piston_mobility_flag(BLOCK_BED) == DES);
	assert(piston_mobility_flag(BLOCK_DOOR_WOOD) == DES);
	assert(piston_mobility_flag(BLOCK_DOOR_IRON) == DES);
	assert(piston_mobility_flag(BLOCK_STONE_PRESSURE_PLATE) == DES);
	assert(piston_mobility_flag(BLOCK_WOOD_PRESSURE_PLATE) == DES);

	// --- flag 2 (immovable) --------------------------------------------------------------------------
	assert(piston_mobility_flag(BLOCK_PORTAL) == IMM);
	assert(piston_mobility_flag(BLOCK_PISTON_STICKY) == IMM);
	assert(piston_mobility_flag(BLOCK_PISTON_BASE) == IMM);
	assert(piston_mobility_flag(BLOCK_PISTON_HEAD) == IMM);
	assert(piston_mobility_flag(BLOCK_PISTON_MOVING) == IMM);

	// --- flag 0 (movable / DELIBERATE absences) ------------------------------------------------------
	assert(piston_mobility_flag(BLOCK_STONE) == MOV);
	assert(piston_mobility_flag(BLOCK_DIRT) == MOV);
	assert(piston_mobility_flag(BLOCK_WOOL) == MOV);
	assert(piston_mobility_flag(BLOCK_TNT) == MOV); // Material.tnt is flag 0 (only translucent, not no-push)
	// obsidian + bedrock are special-cased in piston_can_push_block, NOT via the table -> flag 0 here
	assert(piston_mobility_flag(BLOCK_OBSIDIAN) == MOV);
	assert(piston_mobility_flag(BLOCK_BEDROCK) == MOV);
	// tile-entity blocks: blocked by has_tile_entity, NOT the table -> flag 0
	assert(piston_mobility_flag(BLOCK_CHEST) == MOV);
	assert(piston_mobility_flag(BLOCK_FURNACE) == MOV);
	assert(piston_mobility_flag(BLOCK_FURNACE_LIT) == MOV);
	assert(piston_mobility_flag(BLOCK_DISPENSER) == MOV);
	assert(piston_mobility_flag(BLOCK_NOTEBLOCK) == MOV);
	assert(piston_mobility_flag(BLOCK_MOB_SPAWNER) == MOV);
	assert(piston_mobility_flag(BLOCK_SIGN) == MOV);
	// BlockIce + BlockRail override getMobilityFlag to 0 (despite circuits/ice material) -> movable
	assert(piston_mobility_flag(BLOCK_ICE) == MOV);
	assert(piston_mobility_flag(BLOCK_RAIL) == MOV);
	// out-of-range is safely movable
	assert(piston_mobility_flag(-1) == MOV);
	assert(piston_mobility_flag(4096) == MOV);
	assert(piston_mobility_flag(0) == MOV); // air

	printf("test_piston_mobility_golden: OK (Beta Material.getMaterialMobility + the bed/door/plate "
		   "overrides; tile-entity/obsidian/bedrock deliberate flag-0 absences pinned)\n");
	return 0;
}
