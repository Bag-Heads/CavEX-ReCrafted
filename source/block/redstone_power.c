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

#include "redstone_power.h"

// Faithful transcription of Beta 1.7.3's boolean redstone power model (decompile 740c583),
// cross-checked against the mc173 oracle (mc173/src/world/power.rs). See redstone_power.h for
// the method-name map (isPoweringTo = STRONG/re-emittable, isIndirectlyPoweringTo = WEAK/direct)
// and the side numbering.
//
// Recursion is BOUNDED and shallow (Wii-stack-safe): isBlockIndirectlyGettingPowered ->
// isBlockIndirectlyProvidingPowerTo -> (for a normal cube) isBlockGettingPowered ->
// isBlockProvidingPowerTo -> a per-block WEAK method. The weak/strong block methods never call
// back into the World power queries (an opaque cube only re-emits OTHER blocks' power, never its
// own -- mc173 disables testing the current block), so the depth is a small constant and there is no
// cube<->cube loop.

// Beta BlockRedstoneWire.isPowerProviderOrWire: does the cell at the supplied position count
// as a wire connection / power provider for the wire at the call site? Wire always; then any
// block whose canProvidePower returns true (torch lit OR idle, pressure plate, later lever/button);
// then a repeater only if its output faces this way. `side` is consumed ONLY by the repeater
// branch (Beta passes -1 for the diagonal stair probes, which never matches the 0..3 remap).
static bool can_provide_power(int id) {
	switch(id) {
		// BlockRedstoneTorch.canProvidePower returns true for BOTH lit and idle (the idle torch
		// has torchActive false). It provides no power via isPoweringTo, but it
		// still counts as a wire CONNECTION direction here -- faithful to Beta.
		case RSP_TORCH_LIT_ID:
		case RSP_TORCH_IDLE_ID:
		// BlockPressurePlate.canProvidePower returns true.
		case RSP_STONE_PLATE_ID:
		case RSP_WOOD_PLATE_ID:
		// BlockLever.canProvidePower returns true (the lever, id 69, is the canonical toggleable
		// power source -- it counts as a wire CONNECTION direction here and powers via isPoweringTo).
		case RSP_LEVER_ID:
		// BlockButton.canProvidePower returns true (id 77, the momentary press source).
		case RSP_BUTTON_ID:
		// BlockDetectorRail.canProvidePower returns true (CR-05). BlockRail (the shared base
		// for plain rail AND powered rail, distinguished only by its ctor's isPowered
		// flag) never overrides canProvidePower, so it keeps Block's default false --
		// ONLY the detector subclass overrides it true, so only the detector counts as a
		// wire CONNECTION direction here. [740c583: Block.canProvidePower (default
		// false); BlockDetectorRail.canProvidePower (override true)]
		case RSP_DETECTOR_RAIL_ID: return true;
		// The wire itself is handled by the explicit wire-id branch in is_power_provider_or_wire.
		default: return false;
	}
}

// Beta Block.canProvidePower over ALL ids -- can_provide_power above deliberately excludes
// the wire and the repeaters (its caller handles them via dedicated branches); this public
// helper is the complete set for notify-source gating (BlockDoor's var5 check).
bool redstone_id_can_provide_power(int id) {
	return id == RSP_WIRE_ID || id == RSP_REPEATER_IDLE_ID
		|| id == RSP_REPEATER_ACTIVE_ID || can_provide_power(id);
}

static bool is_power_provider_or_wire(const struct redstone_power_world* w, int x, int y, int z,
									  int side) {
	int id = w->get_type(w->ctx, x, y, z);
	if(id == RSP_WIRE_ID)
		return true;
	if(id <= 0) // 0 = air, < 0 = outside the world
		return false;
	if(can_provide_power(id))
		return true;
	if(id != RSP_REPEATER_IDLE_ID && id != RSP_REPEATER_ACTIVE_ID)
		return false;
	// Beta ModelBed.footInvisibleFaceRemap lists 2, 3, 0, 1: the side a repeater of metadata m outputs.
	static const int foot_remap[4] = {2, 3, 0, 1};
	return side == foot_remap[w->get_meta(w->ctx, x, y, z) & 3];
}

// Beta BlockRedstoneTorch.isPoweringTo (the STRONG query). Reached only for the LIT torch (id 76;
// the idle torch returns false because torchActive is false). A torch powers every face EXCEPT
// the one it is mounted on (metadata 5/3/4/1/2 -> the opposite face 1/3/2/5/4 is unpowered).
static bool torch_is_powering_to(const struct redstone_power_world* w, int x, int y, int z,
								 int side) {
	int meta = w->get_meta(w->ctx, x, y, z);
	if(meta == 5 && side == 1)
		return false;
	if(meta == 3 && side == 3)
		return false;
	if(meta == 4 && side == 2)
		return false;
	if(meta == 1 && side == 5)
		return false;
	return !(meta == 2 && side == 4);
}

// Beta BlockRedstoneWire connection probe -- the var6/7/8/9 (west/east/north/south) block that is
// shared VERBATIM between BlockRedstoneWire.isPoweringTo and RenderBlocks.renderBlockRedstoneWire
// (its var26-29). Extracted so the wire's logical power connectivity and its rendered cross/line
// shape are computed by the SAME code. Index 0=west(-X), 1=east(+X), 2=north(-Z), 3=south(+Z).
void redstone_wire_connections(const struct redstone_power_world* w, int x, int y, int z,
							   bool conn[4]) {
	conn[0] = is_power_provider_or_wire(w, x - 1, y, z, 1)
		|| (!w->is_normal_cube(w->ctx, x - 1, y, z)
			&& is_power_provider_or_wire(w, x - 1, y - 1, z, -1)); // west
	conn[1] = is_power_provider_or_wire(w, x + 1, y, z, 3)
		|| (!w->is_normal_cube(w->ctx, x + 1, y, z)
			&& is_power_provider_or_wire(w, x + 1, y - 1, z, -1)); // east
	conn[2] = is_power_provider_or_wire(w, x, y, z - 1, 2)
		|| (!w->is_normal_cube(w->ctx, x, y, z - 1)
			&& is_power_provider_or_wire(w, x, y - 1, z - 1, -1)); // north (-Z)
	conn[3] = is_power_provider_or_wire(w, x, y, z + 1, 0)
		|| (!w->is_normal_cube(w->ctx, x, y, z + 1)
			&& is_power_provider_or_wire(w, x, y - 1, z + 1, -1)); // south (+Z)

	// Stair-UP links: a wire climbs onto a solid horizontal neighbour to reach a provider on top
	// of it, but only when this wire's own ceiling is open.
	if(!w->is_normal_cube(w->ctx, x, y + 1, z)) {
		if(w->is_normal_cube(w->ctx, x - 1, y, z)
		   && is_power_provider_or_wire(w, x - 1, y + 1, z, -1))
			conn[0] = true;
		if(w->is_normal_cube(w->ctx, x + 1, y, z)
		   && is_power_provider_or_wire(w, x + 1, y + 1, z, -1))
			conn[1] = true;
		if(w->is_normal_cube(w->ctx, x, y, z - 1)
		   && is_power_provider_or_wire(w, x, y + 1, z - 1, -1))
			conn[2] = true;
		if(w->is_normal_cube(w->ctx, x, y, z + 1)
		   && is_power_provider_or_wire(w, x, y + 1, z + 1, -1))
			conn[3] = true;
	}
}

// Beta BlockRedstoneWire.isPoweringTo (the STRONG query, gated by wiresProvidePower). A powered
// wire powers the block directly below it (side 1) unconditionally; for a horizontal face it
// powers the block only when it is "pointing into" that block -- i.e. the wire has a connection
// on the OPPOSITE horizontal side and none perpendicular (or it is an unconnected "dot", which
// powers all four sides). var6/7/8/9 = connected on the wire's west/east/north/south side,
// including the +-1 vertical "stair" links over solid neighbours.
static bool wire_is_powering_to(const struct redstone_power_world* w, int x, int y, int z,
								int side, bool wpp) {
	if(!wpp)
		return false;
	if(w->get_meta(w->ctx, x, y, z) == 0)
		return false;
	if(side == 1)
		return true;

	bool conn[4];
	redstone_wire_connections(w, x, y, z, conn);
	bool var6 = conn[0], var7 = conn[1], var8 = conn[2], var9 = conn[3];

	if(!var8 && !var7 && !var6 && !var9 && side >= 2 && side <= 5)
		return true; // unconnected "dot": powers all four horizontal sides
	if(side == 2 && var8 && !var6 && !var7)
		return true;
	if(side == 3 && var9 && !var6 && !var7)
		return true;
	if(side == 4 && var6 && !var8 && !var9)
		return true;
	return side == 5 && var7 && !var8 && !var9;
}

// Beta BlockRedstoneRepeater.isPoweringTo (same result as isIndirectlyPoweringTo): a powered repeater powers
// ONLY its output face. Reached only for the active id (94); the idle id (93) provides no power.
// facing (low two metadata bits) 0/1/2/3 outputs to side 3/4/2/5 (opposite its input).
static bool repeater_is_powering_to(const struct redstone_power_world* w, int x, int y, int z,
									int side) {
	int facing = w->get_meta(w->ctx, x, y, z) & 3;
	if(facing == 0 && side == 3)
		return true;
	if(facing == 1 && side == 4)
		return true;
	if(facing == 2 && side == 2)
		return true;
	return facing == 3 && side == 5;
}

// Beta Block.isPoweringTo dispatch (the STRONG query: what an opaque cube would re-emit).
static bool block_is_powering_to(const struct redstone_power_world* w, int id, int x, int y, int z,
								 int side, bool wpp) {
	switch(id) {
		case RSP_TORCH_LIT_ID: return torch_is_powering_to(w, x, y, z, side);
		case RSP_TORCH_IDLE_ID: return false; // !torchActive
		case RSP_WIRE_ID: return wire_is_powering_to(w, x, y, z, side, wpp);
		case RSP_STONE_PLATE_ID:
		case RSP_WOOD_PLATE_ID:
			return w->get_meta(w->ctx, x, y, z) > 0; // BlockPressurePlate.isPoweringTo
		// BlockLever.isPoweringTo: an ON lever (bit 8 set) powers EVERY side strongly (no side gate).
		case RSP_LEVER_ID: return (w->get_meta(w->ctx, x, y, z) & 8) > 0;
		// BlockButton.isPoweringTo: a PRESSED button (bit 8 set) powers every side (no side gate).
		case RSP_BUTTON_ID: return (w->get_meta(w->ctx, x, y, z) & 8) > 0;
		case RSP_REPEATER_ACTIVE_ID: return repeater_is_powering_to(w, x, y, z, side);
		case RSP_REPEATER_IDLE_ID: return false; // !isRepeaterPowered
		// BlockDetectorRail.isPoweringTo (CR-05): true exactly when bit 8 is set; the `side`
		// parameter is never consulted, so (like the lever/button) a powered detector rail
		// is a STRONG source on every face. [740c583:
		// BlockDetectorRail.isPoweringTo, block-access overload]
		case RSP_DETECTOR_RAIL_ID: return (w->get_meta(w->ctx, x, y, z) & 8) != 0;
		default: return false;
	}
}

// Beta Block.isIndirectlyPoweringTo dispatch (the WEAK/direct query: what an adjacent component
// detects, and what an opaque cube reads from its neighbours to decide if it is itself powered).
static bool block_is_indirectly_powering_to(const struct redstone_power_world* w, int id, int x,
											int y, int z, int side, bool wpp) {
	switch(id) {
		// BlockRedstoneTorch.isIndirectlyPoweringTo: only side 0 (downward into the block below),
		// and only when lit.
		case RSP_TORCH_LIT_ID: return side == 0 ? torch_is_powering_to(w, x, y, z, 0) : false;
		case RSP_TORCH_IDLE_ID: return false;
		// BlockRedstoneWire.isIndirectlyPoweringTo: gated by wiresProvidePower, then matches isPoweringTo.
		case RSP_WIRE_ID: return wpp ? wire_is_powering_to(w, x, y, z, side, wpp) : false;
		// BlockPressurePlate.isIndirectlyPoweringTo: only the block directly below (side 1).
		case RSP_STONE_PLATE_ID:
		case RSP_WOOD_PLATE_ID: return w->get_meta(w->ctx, x, y, z) > 0 && side == 1;
		// BlockLever.isIndirectlyPoweringTo (transcribed verbatim from 740c583 BlockLever.java): an
		// ON lever (bit 8 set) directly powers only its mounted face -- orientation uses the low three bits;
		// 6/5 -> side 1 (floor lever, up), 4 -> 2, 3 -> 3, 2 -> 4, 1 -> 5.
		case RSP_LEVER_ID: {
			int meta = w->get_meta(w->ctx, x, y, z);
			if((meta & 8) == 0) return false;
			int o = meta & 7;
			if(o == 6 && side == 1) return true;
			if(o == 5 && side == 1) return true;
			if(o == 4 && side == 2) return true;
			if(o == 3 && side == 3) return true;
			if(o == 2 && side == 4) return true;
			return o == 1 && side == 5;
		}
		// BlockButton.isIndirectlyPoweringTo (transcribed verbatim from 740c583 BlockButton.java):
		// a PRESSED button (bit 8 set) directly powers only its mounted face -- orientation uses
		// the low three bits: 5 -> side 1, 4 -> 2, 3 -> 3, 2 -> 4, 1 -> 5. (Orientation 5 is unreachable for
		// the wall-only button but transcribed anyway, matching the decompile.)
		case RSP_BUTTON_ID: {
			int meta = w->get_meta(w->ctx, x, y, z);
			if((meta & 8) == 0) return false;
			int o = meta & 7;
			if(o == 5 && side == 1) return true;
			if(o == 4 && side == 2) return true;
			if(o == 3 && side == 3) return true;
			if(o == 2 && side == 4) return true;
			return o == 1 && side == 5;
		}
		// BlockRedstoneRepeater.isIndirectlyPoweringTo matches isPoweringTo (output face only, if active).
		case RSP_REPEATER_ACTIVE_ID: return repeater_is_powering_to(w, x, y, z, side);
		case RSP_REPEATER_IDLE_ID: return false;
		// BlockDetectorRail.isIndirectlyPoweringTo (CR-05):
		// An unpowered rail returns false; a powered rail accepts only side 1.
		// side numbering here is 0=-Y,1=+Y,2=-Z,3=+Z,4=-X,5=+X (redstone_power.h), and this
		// function is always called with `side` = the direction from the QUERYING cell to
		// THIS block -- so side 1 means the rail is the neighbour ABOVE the querying cell,
		// i.e. the rail directly/weakly powers only the block it sits on (the support cube
		// below it), matching BlockPressurePlate's identical "only the block directly below"
		// carve-out just above. [740c583:
		// BlockDetectorRail.isIndirectlyPoweringTo, world overload]
		case RSP_DETECTOR_RAIL_ID:
			return (w->get_meta(w->ctx, x, y, z) & 8) != 0 && side == 1;
		default: return false;
	}
}

// Beta World.isBlockProvidingPowerTo: the WEAK query of the block at the supplied position and side.
static bool is_block_providing_power_to(const struct redstone_power_world* w, int x, int y, int z,
										int side, bool wpp) {
	int id = w->get_type(w->ctx, x, y, z);
	if(id <= 0)
		return false;
	return block_is_indirectly_powering_to(w, id, x, y, z, side, wpp);
}

// Beta World.isBlockGettingPowered: true if ANY of the 6 neighbours weakly powers this
// cell. The per-neighbour `side` is the direction from this cell to the neighbour. Public (see
// redstone_power.h): also the WEAK/direct query BlockNote.onNeighborBlockChange uses directly
// (noteblock-slice), distinct from the "normal cube re-emits" caller below.
bool redstone_is_getting_powered(const struct redstone_power_world* w, int x, int y, int z,
								 bool wpp) {
	return is_block_providing_power_to(w, x, y - 1, z, 0, wpp)
		|| is_block_providing_power_to(w, x, y + 1, z, 1, wpp)
		|| is_block_providing_power_to(w, x, y, z - 1, 2, wpp)
		|| is_block_providing_power_to(w, x, y, z + 1, 3, wpp)
		|| is_block_providing_power_to(w, x - 1, y, z, 4, wpp)
		|| is_block_providing_power_to(w, x + 1, y, z, 5, wpp);
}

bool redstone_is_indirectly_providing_power_to(const struct redstone_power_world* w, int x, int y,
											   int z, int side, bool wpp) {
	// A normal opaque cube re-emits whatever power it is itself getting (strong power "through"
	// the block); any other block answers its own STRONG (isPoweringTo) query.
	if(w->is_normal_cube(w->ctx, x, y, z))
		return redstone_is_getting_powered(w, x, y, z, wpp);
	int id = w->get_type(w->ctx, x, y, z);
	if(id <= 0)
		return false;
	return block_is_powering_to(w, id, x, y, z, side, wpp);
}

bool redstone_is_indirectly_getting_powered(const struct redstone_power_world* w, int x, int y,
											int z, bool wpp) {
	return redstone_is_indirectly_providing_power_to(w, x, y - 1, z, 0, wpp)
		|| redstone_is_indirectly_providing_power_to(w, x, y + 1, z, 1, wpp)
		|| redstone_is_indirectly_providing_power_to(w, x, y, z - 1, 2, wpp)
		|| redstone_is_indirectly_providing_power_to(w, x, y, z + 1, 3, wpp)
		|| redstone_is_indirectly_providing_power_to(w, x - 1, y, z, 4, wpp)
		|| redstone_is_indirectly_providing_power_to(w, x + 1, y, z, 5, wpp);
}
