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

#ifndef DISPENSER_LOGIC_H
#define DISPENSER_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "../item/item_types.h"
#include "../util.h" // struct random_gen

// Faithful Beta 1.7.3 TileEntityDispenser (decompile 740c583). The direction
// table, spawn-point math and reservoir-sampling pick are isolated here as pure
// functions so they are headless golden-testable (tests/test_dispenser_env.c),
// mirroring furnace_logic.h's split of pure math from the world/entity glue in
// server_dispenser.c / block_dispenser.c. A slot with count==0 is the "null
// ItemStack" (matches the engine-wide item_data convention).

enum { DISPENSER_TE_SLOTS = 9 }; // TileEntityDispenser.getSizeInventory()

// A dispensed arrow/egg/snowball has NO real thrower in Beta -- the
// position-only EntityArrow / EntityEgg constructors dispenseItem uses
// leave owner/thrower null (doesArrowBelongToPlayer is force-set true
// SEPARATELY, only for arrows, and is a damage-scale selector, not a
// targeting exemption -- see combat.h's arrow_should_hit). This engine's
// entity_arrow/entity_egg/entity_snowball ctors take a uint32_t owner_id with
// no "null" value, and 0 is reserved for the local player (arrow_should_hit's
// "id-0 skip is the caller's job" contract) -- passing 0 here would wrongly
// grant the PLAYER a 5-tick immunity window against a dispensed projectile.
// This sentinel can never alias a live entity id (ids are allocated from a
// live nonzero counter), so every real entity is hittable from tick 0, which
// is what "no owner" actually means.
#define DISPENSER_PROJECTILE_OWNER_SENTINEL UINT32_MAX

struct dispenser_te {
	struct item_data items[DISPENSER_TE_SLOTS];
	// Beta `dispenserRandom` -- a PER-TILE-ENTITY java.util.Random, DISTINCT
	// from the world's rand (which drives the generic-item eject jitter).
	// Not persisted (Beta's read/writeToNBT only round-trip "Items"): every
	// fresh placement AND every chunk-load reconstruction reseeds it (dead
	// code path if left at seed 0 -- rand_gen's xorshift zero-locks).
	struct random_gen dispenser_random;
};

// BlockDispenser.dispenseItem direction table (Beta's test ORDER -- the last case is
// the catch-all, not a 4th explicit metadata 4 case):
//   metadata 3 faces +Z, 2 faces -Z, 5 faces +X, anything else faces -X
// [decompile 740c583: BlockDispenser.dispenseItem]
void dispenser_direction(uint8_t metadata, int* dx, int* dz);

// BlockDispenser.dispenseItem spawn point: the block centre (+0.5 on each axis)
// pushed 0.6 along the facing on X/Z. [decompile 740c583: BlockDispenser.dispenseItem]
void dispenser_spawn_point(int x, int y, int z, int dx, int dz, float* sx,
						   float* sy, float* sz);

// TileEntityDispenser.getRandomStackFromInventory's reservoir-sampling SELECT
// half, as a pure function over an "is this slot
// occupied" view (so it is testable without a real item registry):
//   start with no selection and a counter of 1; walk the slots in order and,
//   for each occupied slot, draw a uniform roll below the counter (then bump it);
//   a zero roll makes that slot the selection (uniform over occupied slots).
// The bump (and the draw) happens ONLY for occupied slots -- a null slot is
// skipped without touching the RNG or the counter. Returns -1 (no draw ever
// made) iff every slot is empty. [decompile 740c583:
// TileEntityDispenser.getRandomStackFromInventory]
int dispenser_reservoir_select(const bool occupied[DISPENSER_TE_SLOTS],
							   struct random_gen* rng);

// The full Beta getRandomStackFromInventory: picks a slot via
// dispenser_reservoir_select over d->items[].count>0, then Beta decrStackSize
// on that slot for one item -- decrements the source slot by exactly one unit, clearing it to
// the null ItemStack (id=0) if that empties it, and writes a COUNT-1 copy
// (same id/durability) to *out. Returns false (out untouched, *out_slot
// untouched) iff the dispenser is empty (matches the empty-click / SFX-1001
// path). *out_slot receives the slot the stack was taken from (0..8) on
// success -- the caller needs it to precisely UNDO the take via
// dispenser_restore_stack if the ejection that was supposed to follow (an
// entity spawn) turns out not to actually happen (adversarial review D4:
// this engine's entity/queue capacity can fail where Beta's never does;
// losing the item to a silent capacity failure is not faithful, Beta always
// materializes what it decremented). out_slot may be NULL if the caller has
// no possible need to restore (e.g. pure unit tests of the pick itself).
bool dispenser_take_random_stack(struct dispenser_te* d, struct item_data* out,
								 int* out_slot);

// Undo EXACTLY one dispenser_take_random_stack call: `stack` is the *out it
// produced and `slot` is the *out_slot it produced. Puts the one unit back
// -- restores the null ItemStack (id/durability/count=1) if the slot was
// emptied by the take, else increments count by 1. Used only on the D4
// failure path (an attempted spawn did not actually materialize).
void dispenser_restore_stack(struct dispenser_te* d, int slot,
							 struct item_data stack);

#endif
