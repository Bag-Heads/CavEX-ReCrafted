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

#ifndef REDSTONE_REPEATER_H
#define REDSTONE_REPEATER_H

#include <stdbool.h>

// Pure Beta 1.7.3 BlockRedstoneRepeater logic (decompile 740c583), isolated behind an injected
// world view so the SAME code runs over the live server world (production) and a stub cell grid
// driven by the real scheduled-tick queue (golden tests) -- the redstone_logic.c precedent.
//
// The repeater is a SCHEDULED component (like the torch), but with a metadata-selected delay of
// {2,4,6,8} game-ticks -- NOT a constant block tick_delay. It is modelled as TWO block ids that
// swap on the scheduled tick: idle (93) and active (94, output ON). BOTH edges are delayed: the
// schedule is armed in onNeighborBlockChange whenever the input state disagrees with the output
// state, and the swap happens in updateTick. Beta 1.7.3 has NO repeater locking (that is 1.8+).
//
// Metadata layout (low nibble): bits 0-1 (& 3) = facing; bits 2-3 ((& 12) >> 2) = delay index.
// Delay = field_22023_b[idx] * 2, field_22023_b = {1,2,3,4} -> {2,4,6,8}. Do NOT double the *2.
// Facing 0/1/2/3 reads its input from +Z / -X / -Z / +X and outputs to the opposite side.

#define RS_REPEATER_IDLE_ID 93	 // Block.redstoneRepeaterIdle  (output OFF)
#define RS_REPEATER_ACTIVE_ID 94 // Block.redstoneRepeaterActive(output ON)

// field_22023_b * 2 settled here so callers (and the block tick_delay path) read the same table.
extern const int redstone_repeater_delay[4]; // {2, 4, 6, 8} game-ticks, indexed by (meta&12)>>2

struct redstone_repeater_world {
	void* ctx;
	int (*get_type)(void* ctx, int x, int y, int z); // 93/94 at the repeater cell
	int (*get_meta)(void* ctx, int x, int y, int z);
	// Beta World.isBlockIndirectlyProvidingPowerTo(x,y,z,side): the input-side STRONG power read.
	bool (*is_indirectly_providing_power)(void* ctx, int x, int y, int z, int side);
	// Swap the repeater id idle<->active (keeping metadata), with the engine neighbour-notify.
	void (*set_active)(void* ctx, int x, int y, int z, bool active);
	// Beta World.scheduleBlockUpdate(x,y,z,id,delay): arm a scheduled tick `delay` game-ticks out.
	void (*schedule)(void* ctx, int x, int y, int z, int id, int delay);
};

// Beta ignoreTick: is the repeater's INPUT side powered? (the back side per facing, via
// isBlockIndirectlyProvidingPowerTo, OR an adjacent powered redstone wire -- Beta special-cases
// the wire here so a wire dead-ending into the input still drives the repeater).
bool redstone_repeater_input_powered(const struct redstone_repeater_world* w, int x, int y, int z);

// Beta onNeighborBlockChange (scheduling half): if the input state disagrees with the current
// output state, arm the delayed toggle. (The caller does the canBlockStay drop separately.)
void redstone_repeater_neighbour_change(const struct redstone_repeater_world* w, int x, int y,
										int z);

// Beta updateTick (toggle half): swap idle<->active to follow the input; if it just turned ON
// while the input is already OFF (a pulse shorter than the delay), re-arm to turn back off.
void redstone_repeater_tick(const struct redstone_repeater_world* w, int x, int y, int z);

#endif
