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

#include <math.h>

#include "furnace_logic.h"

bool furnace_is_burning(const struct furnace_te* f) {
	return f->burn_time > 0; // TileEntityFurnace.isBurning()
}

// Beta BlockFurnace.onBlockPlacedBy (decompile 740c583): the front faces the placer.
uint8_t furnace_facing_meta(float yaw_deg) {
	static const uint8_t face[4] = {2, 5, 3, 4}; // yaw quadrant 0/1/2/3 -> N/E/S/W front
	// floorf + &3 matches MathHelper.floor_double + &3, incl. negative yaw (e.g.
	// -90: floorf(-0.5) == -1, (-1)&3 == 3 -> west front, same as +270).
	int l = ((int)floorf(yaw_deg / 90.0F + 0.5F)) & 3;
	return face[l];
}

// Beta TileEntityFurnace.canSmelt() (decompile 740c583:149-165), transcribed.
bool furnace_can_smelt(const struct furnace_te* f, const struct furnace_env* env) {
	const struct item_data* in = &f->items[FURNACE_SLOT_INPUT_TE];
	if(in->count == 0)
		return false; // input slot 0 is empty
	struct item_data result;
	if(!env->smelt_result(*in, &result))
		return false; // no smelting result for the input
	const struct item_data* out = &f->items[FURNACE_SLOT_OUTPUT_TE];
	if(out->count == 0)
		return true; // empty output -> always room
	// isItemEqual: same item id AND same damage/metadata.
	if(out->id != result.id || out->durability != result.durability)
		return false;
	// Beta's literal two-branch room test (getInventoryStackLimit() then maxStackSize).
	if(out->count < FURNACE_STACK_LIMIT && out->count < env->max_stack(*out))
		return true;
	return out->count < env->max_stack(result);
}

// Beta TileEntityFurnace.smeltItem() (decompile 740c583:167-181), transcribed.
static void furnace_smelt(struct furnace_te* f, const struct furnace_env* env) {
	if(!furnace_can_smelt(f, env))
		return;
	struct item_data result;
	env->smelt_result(f->items[FURNACE_SLOT_INPUT_TE], &result);
	struct item_data* out = &f->items[FURNACE_SLOT_OUTPUT_TE];
	if(out->count == 0)
		*out = result; // copy result into output slot 2, including its count
	else if(out->id == result.id)
		out->count++; // add one item to the output stack

	if(--f->items[FURNACE_SLOT_INPUT_TE].count == 0) // consume one input; clear the exhausted stack
		f->items[FURNACE_SLOT_INPUT_TE] = (struct item_data) {0};
}

// Beta TileEntityFurnace.updateEntity() (decompile 740c583:105-148), transcribed.
// Beta tracks prior burning and inventory changes. We RETURN whether burning
// toggled so the caller does updateFurnaceBlockState.
bool furnace_tick_is_idle(const struct furnace_te* f,
						  const struct furnace_env* env) {
	return f->burn_time == 0 && f->cook_time == 0 && !furnace_can_smelt(f, env);
}

bool furnace_tick(struct furnace_te* f, const struct furnace_env* env) {
	/* Imported ambiguous records are immutable, including their scope's
	 * burn/cook state. Ordinary Beta records take the unchanged path below. */
	for(size_t k=0;k<3;k++) if(item_data_pinned(&f->items[k])) return false;
	bool was_burning = f->burn_time > 0; // prior burning state

	if(f->burn_time > 0)
		f->burn_time--; // decrement remaining burn time FIRST

	// Server side (this engine is single-player; the !multiplayerWorld branch always runs).
	if(f->burn_time == 0 && furnace_can_smelt(f, env)) {
		// Re-light: set current and remaining burn times to the fuel duration.
		f->current_item_burn_time = f->burn_time
			= env->fuel_ticks(f->items[FURNACE_SLOT_FUEL_TE]);
		if(f->burn_time > 0) {
			if(f->items[FURNACE_SLOT_FUEL_TE].count > 0) {
				// consume ONE fuel item; empty -> null
				if(--f->items[FURNACE_SLOT_FUEL_TE].count == 0)
					f->items[FURNACE_SLOT_FUEL_TE] = (struct item_data) {0};
			}
		}
	}

	if(furnace_is_burning(f) && furnace_can_smelt(f, env)) {
		if(++f->cook_time == FURNACE_COOK_TIME) {
			f->cook_time = 0;
			furnace_smelt(f, env);
		}
	} else {
		f->cook_time = 0; // RESETS to 0 (not paused) when flame dies / can't smelt
	}

	return was_burning != (f->burn_time > 0);
}
