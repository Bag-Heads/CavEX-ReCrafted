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
	Beta 1.7.3 faithful finite-fluid spread (water first).

	The flow algorithm is a port of Beta's BlockFlowing/BlockFluids: a source
	(metadata 0) spreads outward losing one level per horizontal step to level 7
	then dries up; it falls straight down (metadata bit 0x8 = "falling") whenever
	there is space below and only spreads sideways once it lands; horizontal
	spread prefers the directions that reach the nearest downward drop within four
	blocks (getOptimalFlowDirections); two source blocks adjacent over solid ground
	form a new source (infinite water).

	It is written as pure logic over a `fluid_world` accessor so it is
	unit-testable on a fixed grid (tests/test_fluid_spread.c) independent of
	server_world -- the same pattern as the worldgen and chest-store layers. The
	in-game adapter lives in block_water.c. Tick rate / scheduling (Beta water:
	every 5 game ticks) is the host's concern, not this module's.
*/

#ifndef FLUID_SPREAD_H
#define FLUID_SPREAD_H

#include <stdbool.h>
#include <stdint.h>

// Block ids (match blocks_data.h).
#define FLUID_AIR		  0
#define FLUID_WATER_FLOW  8
#define FLUID_WATER_STILL 9
#define FLUID_LAVA_FLOW	  10
#define FLUID_LAVA_STILL  11
// Accessor the spread runs over. (x,y,z) are world block coords.
struct fluid_world {
	void* ctx;
	// Block type id at (x,y,z); *meta_out receives its metadata (0..15). For
	// cells outside the loaded world, return a flow-blocking type so the spread
	// treats the edge as a wall.
	uint8_t (*get)(void* ctx, int x, int y, int z, uint8_t* meta_out);
	// Set (type, meta) at (x,y,z) and schedule its fluid tick. Used to place
	// flowing water, change a level, convert flowing<->still, or clear to air.
	void (*set)(void* ctx, int x, int y, int z, uint8_t type, uint8_t meta);
	// True if `type` blocks fluid flow (a solid / movement-blocking block). Air
	// and replaceable plants are NOT blocking. This is Beta BlockFlowing's
	// `blockBlocksFlow`, which is deliberately distinct from material solidity:
	// ladder/reed block flow without being solid. The block table lives outside
	// this unit, so the host supplies this predicate.
	bool (*blocks_flow)(void* ctx, uint8_t type);
	// Beta Material.isSolid(), used only by the two-source infinite-water rule.
	// Optional compatibility fallback: blocks_flow(ctx, type).
	bool (*material_is_solid)(void* ctx, uint8_t type);
	// Beta Random.nextInt(bound), used by lava's 3-in-4 decay resistance. Optional
	// compatibility fallback: return 0 (accept the newly computed decay).
	int (*next_int)(void* ctx, int bound);
	// Horizontal water replacement calls dropBlockAsItem for the displaced block.
	// Downward flow deliberately bypasses this callback, matching Beta. Optional.
	void (*drop_block)(void* ctx, int x, int y, int z, uint8_t type,
					   uint8_t meta);
	// Horizontal lava replacement of a non-air block triggers the lava-mix
	// effect, but drops no item. Downward flow bypasses this too. Optional.
	void (*lava_mix)(void* ctx, int x, int y, int z);
	/* CFR BlockFlowing.java:19,func_4035_j: returned directions are one
	 * instance array; recursive immediate ticks overwrite a parent's view. */
	bool* shared_directions;
};

/*
	Resumable form of one BlockFlowing.updateTick.  `fluid_tick_state_step`
	runs until the tick is complete or until exactly one `world->set` call has
	returned.  It returns false after a set so a host which implements Beta's
	`scheduleBlockUpdate(... scheduledUpdatesAreImmediate ...)` can finish the
	nested block lifecycle before resuming the parent tick without using the C
	call stack.  Ordinary callers should keep using fluid_water_tick /
	fluid_lava_tick below; those wrappers drive this state to completion and are
	byte-identical to the former one-shot implementation.

	The fields are public solely so hosts can store states in an explicit action
	stack.  Treat them as opaque outside fluid_spread.c.
*/
struct fluid_tick_state {
	int x, y, z;
	int decay;
	int next_dir;
	uint8_t phase;
	uint8_t kind;
	bool nether;
	bool dirs[4];
};

void fluid_tick_state_init(struct fluid_tick_state* state, int x, int y, int z,
						   bool lava, bool nether);
/* true = complete; false = one set completed and the state must be resumed */
bool fluid_tick_state_step(const struct fluid_world* world,
						  struct fluid_tick_state* state);

// Run one Beta flow tick at (x,y,z), where the block there is the matching fluid
// (flowing or still). Reads up to 4 cells out (the flow-toward-drops search) and
// writes neighbours through the accessor. Water loses one level per horizontal
// step and forms infinite sources; lava loses two (reaching ~3 blocks in the
// overworld) and never forms a source. Tick rate / scheduling is the host's job.
// `nether` gives lava its Nether 1-level step (reaches 7, like water) instead of
// the overworld's 2 (reaches 3); ignored for water (always steps 1).
void fluid_water_tick(const struct fluid_world* w, int x, int y, int z,
					  bool nether);
void fluid_lava_tick(const struct fluid_world* w, int x, int y, int z,
					 bool nether);

#endif
