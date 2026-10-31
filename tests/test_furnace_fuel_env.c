/*
	Production furnace-fuel regression guard for the M4 timed furnace.

	test_furnace_burn pins the tick MATH with STUB fuel functions; it cannot catch a
	wrong PRODUCTION fuel value, which is decided by env_fuel_ticks
	(source/network/server_furnace.c) reading each block's getMaterial. The faithful
	Beta rule -- TileEntityFurnace.getItemBurnTime (decompile 740c583):
	    if (id < 256 && Block.blocksList[id].blockMaterial == Material.wood) return 300;
	    else stick 100 / coal 1600 / lavaBucket 20000 / (id == sapling ? 100 : 0)
	-- means ANY block whose getMaterial wrongly returns MATERIAL_WOOD silently becomes
	300-tick furnace fuel. Two such bugs shipped (the furnace exposed them): the TORCH
	(Beta Material.circuits) and the PUMPKIN / jack-o-lantern (Beta Material.pumpkin).
	This test boots the REAL block registry and asserts the production env's fuel value
	for a representative spread, so the off-by-a-material drift is caught, not hoped.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "block/furnace_logic.h"
#include "item/items.h"
#include "network/server_furnace.h"

static int fuel(const struct furnace_env* env, int id, int count) {
	struct item_data it = {.id = (uint16_t)id, .durability = 0, .count = (uint8_t)count};
	return env->fuel_ticks(it);
}

int main(void) {
	blocks_init(); // real registry; PC render_block_init is data-only (headless-safe)
	const struct furnace_env* env = furnace_server_env();
	assert(env && env->fuel_ticks);

	// --- the regressions the adversarial review found (must be NON-fuel) ---
	assert(fuel(env, BLOCK_TORCH, 1) == 0);		  // Beta Material.circuits
	assert(fuel(env, BLOCK_PUMPKIN, 1) == 0);	  // Beta Material.pumpkin
	assert(fuel(env, BLOCK_PUMPKIN_LIT, 1) == 0); // jack-o-lantern, same
	// FH-07: id 95 is Beta's own lockedChest again (Material.wood [740c583
	// Block.java:687]) -- per getItemBurnTime's blockMaterial==wood rule it IS
	// 300-tick fuel in real Beta. The relocated iron chest (200, MATERIAL_STONE
	// by this port's iron-sibling convention) stays never-fuel.
	assert(fuel(env, BLOCK_LOCKED_CHEST, 1) == 300);
	assert(fuel(env, BLOCK_IRON_CHEST, 1) == 0);

	// --- genuine Material.wood blocks: 300 ticks (must STILL burn) ---
	assert(fuel(env, BLOCK_PLANKS, 1) == 300);
	assert(fuel(env, BLOCK_LOG, 1) == 300);
	assert(fuel(env, BLOCK_BOOKSHELF, 1) == 300);
	assert(fuel(env, BLOCK_WORKBENCH, 1) == 300);
	assert(fuel(env, BLOCK_FENCE, 1) == 300);
	assert(fuel(env, 54, 1) == 300); // chest        (Material.wood)
	assert(fuel(env, 53, 1) == 300); // wooden stairs (Material.wood)
	assert(fuel(env, 25, 1) == 300); // noteblock     (Material.wood)
	assert(fuel(env, 84, 1) == 300); // jukebox       (Material.wood)
	assert(fuel(env, 96, 1) == 300); // trapdoor      (Material.wood)
	assert(fuel(env, 72, 1) == 300); // wooden pressure plate (Material.wood)

	// --- special-cased fuels (by item id, not material) ---
	assert(fuel(env, ITEM_COAL, 1) == 1600);
	assert(fuel(env, ITEM_STICK, 1) == 100);
	assert(fuel(env, BLOCK_SAPLING, 1) == 100);
	assert(fuel(env, ITEM_BUCKET_LAVA, 1) == 20000);

	// --- non-fuel blocks (rock / sand / glass / dirt / ore) -> 0 ---
	assert(fuel(env, BLOCK_STONE, 1) == 0);
	assert(fuel(env, BLOCK_COBBLESTONE, 1) == 0);
	assert(fuel(env, 3, 1) == 0);  // dirt
	assert(fuel(env, 15, 1) == 0); // iron ore
	assert(fuel(env, 20, 1) == 0); // glass
	assert(fuel(env, 67, 1) == 0); // cobblestone stairs  (Material.rock)
	assert(fuel(env, 70, 1) == 0); // stone pressure plate (Material.rock)

	// --- count == 0 is the "null ItemStack" -> 0 regardless of id ---
	assert(fuel(env, ITEM_COAL, 0) == 0);

	printf("test_furnace_fuel_env: production getItemBurnTime faithful "
		   "(torch/pumpkin NOT fuel; wood=300; coal/stick/sapling/lava; non-fuels 0)\n");
	return 0;
}
