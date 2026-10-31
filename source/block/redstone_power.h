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

#ifndef REDSTONE_POWER_H
#define REDSTONE_POWER_H

#include <stdbool.h>

// Pure Beta 1.7.3 BOOLEAN redstone power model (decompile 740c583), isolated behind an
// injected world view so the SAME code runs over the live server world (production) and a
// flat stub cell grid (golden tests) -- the redstone_logic.c / furnace_logic.c precedent.
//
// This REPLACES the documented INTERIM power model (see docs/redstone_plan.md step 3): the
// wire flood and the torch inverter query their is_indirectly_powered / is_indirectly_-
// providing_power accessors through this faithful boolean model instead of the two crude
// stand-ins (adjacent-lit-torch / adjacent-live-wire).
//
// CENTRAL FACTS (Beta has NO integer power query):
//   * World.isBlockGettingPowered / isBlockIndirectlyGettingPowered / isBlockProvidingPowerTo
//     / isBlockIndirectlyProvidingPowerTo are ALL boolean. The 0-15 gradient lives ONLY inside
//     the wire (redstone_logic.c getMaxCurrentStrength); a non-wire source injects a flat 15.
//   * Beta's two per-block power methods are confusingly named (transcribed verbatim):
//       - Block.isPoweringTo           = the STRONG query: power that an opaque cube RE-EMITS
//         (World.isBlockIndirectlyProvidingPowerTo uses it for non-cubes; a normal cube instead
//         re-emits whatever it is itself getting via isBlockGettingPowered).
//       - Block.isIndirectlyPoweringTo = the WEAK/direct query: power detected directly by an
//         adjacent component (World.isBlockProvidingPowerTo uses it; a wire reads it).
//     (Cross-checked against the mc173 oracle's power.rs `indirect` vs `passive` split -- they
//     agree case-for-case; the decompile is authoritative.)
//   * Side numbering (Beta): 0=-Y(below), 1=+Y(above), 2=-Z, 3=+Z, 4=-X, 5=+X. The `side`
//     passed to a block is the direction from the QUERYING block to this block.

// Block ids (Beta 1.7.3 == CaveWii blocks_data.h). Kept local so this module links without the
// engine block registry (golden tests inject a stub grid); the production glue static-asserts
// these equal the BLOCK_* ids.
#define RSP_WIRE_ID 55			// BLOCK_REDSTONE_WIRE
#define RSP_TORCH_IDLE_ID 75	// BLOCK_REDSTONE_TORCH      (unlit; canProvidePower yet isPoweringTo=false)
#define RSP_TORCH_LIT_ID 76		// BLOCK_REDSTONE_TORCH_LIT
#define RSP_LEVER_ID 69			// BLOCK_LEVER (BlockLever.canProvidePower()==true)
#define RSP_STONE_PLATE_ID 70	// BLOCK_STONE_PRESSURE_PLATE
#define RSP_WOOD_PLATE_ID 72	// BLOCK_WOOD_PRESSURE_PLATE
#define RSP_BUTTON_ID 77		// BLOCK_STONE_BUTTON (BlockButton.canProvidePower()==true)
#define RSP_REPEATER_IDLE_ID 93 // not yet placeable in-game; the wire connection logic
#define RSP_REPEATER_ACTIVE_ID 94 // (isPowerProviderOrWire) references the ids -- faithful now.
#define RSP_DETECTOR_RAIL_ID 28 // BLOCK_DETECTOR_RAIL (BlockDetectorRail.canProvidePower()==true;
								 // isPoweringTo/isIndirectlyPoweringTo below, CR-05)

// Injected world view: just the three reads Beta's power model needs. get_type returns the
// block id (0 = air, < 0 = outside the world); get_meta the low metadata nibble; is_normal_cube
// is Beta World.isBlockNormalCube (a full opaque solid cube -- power re-emits through these and
// the wire stair-steps over them).
struct redstone_power_world {
	void* ctx;
	int (*get_type)(void* ctx, int x, int y, int z);
	int (*get_meta)(void* ctx, int x, int y, int z);
	bool (*is_normal_cube)(void* ctx, int x, int y, int z);
};

// Beta World.isBlockIndirectlyGettingPowered(x,y,z) -- "is this cell receiving power that an
// opaque cube would re-emit?" This is what a WIRE cell queries to decide its flat-15 injection.
// wires_provide_power mirrors BlockRedstoneWire.wiresProvidePower: FALSE during a wire's own
// current recompute (calculateCurrentChanges forces it false so a wire never counts itself / any
// other wire as a source), TRUE for every other query (a torch mount, a repeater input).
bool redstone_is_indirectly_getting_powered(const struct redstone_power_world* w, int x, int y,
											int z, bool wires_provide_power);

// Beta World.isBlockGettingPowered(x,y,z) -- "is this cell WEAKLY (directly-adjacent-only)
// powered?" The 6-neighbour isBlockProvidingPowerTo/isIndirectlyPoweringTo check, WITHOUT the
// "through a solid cube" hop isBlockIndirectlyGettingPowered adds -- e.g. a torch mounted on the
// FAR side of a solid block does NOT weakly-power a cell on the near side, even though it WOULD
// strongly-power it. This is what BlockNote.onNeighborBlockChange queries (noteblock-slice) --
// deliberately narrower than the dispenser/door's isBlockIndirectlyGettingPowered. Already used
// internally as the "normal cube re-emits" branch of redstone_is_indirectly_providing_power_to;
// exposed here as its own entry point for callers that need the weak/direct check specifically.
bool redstone_is_getting_powered(const struct redstone_power_world* w, int x, int y, int z,
								 bool wires_provide_power);

// Beta World.isBlockIndirectlyProvidingPowerTo(x,y,z,side) -- "does the block at (x,y,z) deliver
// (strong) power onto the given face?" This is what a TORCH mount queries (per the mount face).
bool redstone_is_indirectly_providing_power_to(const struct redstone_power_world* w, int x, int y,
											   int z, int side, bool wires_provide_power);

// Beta BlockRedstoneWire connection probe (the four side flags in BlockRedstoneWire.isPoweringTo, and the
// BYTE-IDENTICAL four side flags in RenderBlocks.renderBlockRedstoneWire): fills conn[4] with whether the
// wire at (x,y,z) connects on each horizontal side -- including the +-1 vertical "stair" links over
// solid neighbours. Index 0=west(-X), 1=east(+X), 2=north(-Z), 3=south(+Z). Shared by the power
// model and the renderer so the wire's logical and visual connectivity can never disagree.
void redstone_wire_connections(const struct redstone_power_world* w, int x, int y, int z,
							   bool conn[4]);

// Beta Block.canProvidePower() by id -- the FULL set (wire, both torches, plates, lever,
// button, both repeater ids). Used by consumers that transcribe Beta's
// "non-air neighbour whose block can provide power" notify-source gate (BlockDoor).
bool redstone_id_can_provide_power(int id);

#endif
