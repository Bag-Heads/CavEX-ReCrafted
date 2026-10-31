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

#ifndef FURNACE_LOGIC_H
#define FURNACE_LOGIC_H

#include <stdbool.h>

#include "../item/item_types.h"

// Faithful Beta 1.7.3 TileEntityFurnace (decompile 740c583). The per-tick burn/cook
// MATH is isolated here as a pure function so it is headless golden-testable
// (tests/test_furnace_burn.c), mirroring the fluid_spread / lighting callback pattern;
// the item-registry recipe + fuel-duration tables and the world block-state swap are
// injected, not hard-linked. A slot with count==0 is the "null ItemStack".

enum {
	FURNACE_SLOT_INPUT_TE = 0,	// TileEntityFurnace.furnaceItemStacks[0]
	FURNACE_SLOT_FUEL_TE = 1,	// [1]
	FURNACE_SLOT_OUTPUT_TE = 2, // [2]
	FURNACE_COOK_TIME = 200,	// TileEntityFurnace cookTime threshold (10s @ 20tps)
	FURNACE_STACK_LIMIT = 64,	// getInventoryStackLimit()
};

struct furnace_te {
	int burn_time;				 // furnaceBurnTime  (ticks of the lit fuel remaining)
	int cook_time;				 // furnaceCookTime  (0..200)
	int current_item_burn_time;	 // currentItemBurnTime (the lit fuel's full duration)
	struct item_data items[3];	 // [input, fuel, output]; count==0 == null
};

// Injected from the item registry (production) or test stubs (golden). fuel_ticks =
// Beta getItemBurnTime in TICKS (0 = not a fuel). smelt_result = FurnaceRecipes:
// returns true + fills *out (count 1) iff `in` has a smelting result. max_stack =
// ItemStack.getMaxStackSize() for the canSmelt output-room check.
struct furnace_env {
	int (*fuel_ticks)(struct item_data fuel);
	bool (*smelt_result)(struct item_data in, struct item_data* out);
	int (*max_stack)(struct item_data it);
};

bool furnace_is_burning(const struct furnace_te* f);
bool furnace_can_smelt(const struct furnace_te* f, const struct furnace_env* env);

// Beta BlockFurnace.onBlockPlacedBy facing: maps the placer's yaw to the furnace
// facing metadata. yaw_deg is in DEGREES and is the Beta rotationYaw convention
// (0 = facing +Z/south, 90 = -X/west, 180 = -Z/north, 270/-90 = +X/east -- the
// same convention server_local stores in player.rx). Beta:
//   int var6 = floor_double(yaw * 4 / 360 + 0.5) & 3;  // == floor(yaw/90 + 0.5)&3
//   var6 0->2, 1->5, 2->3, 3->4  (front-face metadata N/E/S/W)
// The front ends up facing the placer. Pure + golden-tested (test_furnace_burn.c).
uint8_t furnace_facing_meta(float yaw_deg);

// One Beta TileEntityFurnace.updateEntity tick (server-side). Mutates *f in place.
// Returns true iff the BURNING state toggled this tick -- the caller then runs
// BlockFurnace.updateFurnaceBlockState (swap idle<->active block id, PRESERVING the
// facing metadata; never write burn state into metadata).
bool furnace_tick(struct furnace_te* f, const struct furnace_env* env);

// IMP-TE-CAPS (T1, golden: tests/test_furnace_burn.c dormant_skip_*): true iff
// furnace_tick(f, env) is provably a no-op this tick -- burnTime == 0,
// cookTime == 0 and !canSmelt [740c583 TileEntityFurnace.updateEntity: no
// decrement, no re-light, cookTime forced to 0, burning state unchanged]. The
// server tick skips such a furnace (no block read, no tick); idleness is
// re-evaluated every tick, so a window edit that makes it smeltable wakes it
// on the very next tick with no extra bookkeeping.
bool furnace_tick_is_idle(const struct furnace_te* f,
						  const struct furnace_env* env);

#endif
