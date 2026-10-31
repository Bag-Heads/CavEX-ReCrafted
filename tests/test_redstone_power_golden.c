/*
	Golden behavior test for Beta 1.7.3's BOOLEAN redstone power model (M4 redstone step 3),
	authored to pin the faithful replacement of the documented INTERIM power model.

	Drives the pure source/block/redstone_power.c over a flat STUB cell grid injected via
	struct redstone_power_world -- no server_world/get_block, no gfx (the redstone_logic /
	furnace_logic / test_relight precedent). Every expected value is transcribed from the
	decompile (740c583: World.isBlock{,Indirectly}{Getting,Providing}PowerTo,
	BlockRedstoneWire/Torch/PressurePlate.isPoweringTo/isIndirectlyPoweringTo) and
	cross-checked against the mc173 oracle (mc173/src/world/power.rs). Beta has NO integer
	power query -- these are all booleans.

	Side numbering (Beta): 0=-Y(below) 1=+Y(above) 2=-Z 3=+Z 4=-X 5=+X.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/redstone_power.h"

#define STONE_ID 1 // a normal opaque cube (the only is_normal_cube id in this stub)
#define G 16

static int g_id[G][G][G];
static int g_meta[G][G][G];

static bool in(int x, int y, int z) {
	return x >= 0 && x < G && y >= 0 && y < G && z >= 0 && z < G;
}
static int t_get_type(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) ? g_id[x][y][z] : -1; // < 0 = outside the world
}
static int t_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) ? g_meta[x][y][z] : 0;
}
static bool t_is_normal_cube(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) && g_id[x][y][z] == STONE_ID;
}

static void clear(void) {
	memset(g_id, 0, sizeof(g_id)); // 0 = air everywhere
	memset(g_meta, 0, sizeof(g_meta));
}
static void put(int x, int y, int z, int id, int meta) {
	g_id[x][y][z] = id;
	g_meta[x][y][z] = meta;
}

int main(void) {
	struct redstone_power_world w = {
		.ctx = NULL,
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.is_normal_cube = t_is_normal_cube,
	};

	// --- 1. Lit floor torch directly powers an adjacent cell (the wire's flat-15 source) ----
	// torch lit, mounted on the floor (meta 5), at (8,8,8). The cell to its east (9,8,8) asks
	// isBlockIndirectlyGettingPowered: its -X neighbour (the torch, side 4) is a non-cube, so
	// torch.isPoweringTo(meta5, side4) = true (a torch powers every face but its mount's). The
	// wire flood passes wires_provide_power=false; irrelevant here (the source is a torch).
	clear();
	put(8, 8, 8, RSP_TORCH_LIT_ID, 5);
	assert(redstone_is_indirectly_getting_powered(&w, 9, 8, 8, false) == true);
	// an IDLE torch (75) is not a source: torchActive=false -> isPoweringTo=false.
	put(8, 8, 8, RSP_TORCH_IDLE_ID, 5);
	assert(redstone_is_indirectly_getting_powered(&w, 9, 8, 8, false) == false);

	// --- 2. Strong power re-emitted THROUGH a normal cube (torch under stone, wire on top) ---
	// torch lit (meta5) at (8,8,8); stone at (8,9,8); query the cell on top (8,10,8). The stone
	// is a normal cube, so isBlockIndirectlyProvidingPowerTo(stone) = isBlockGettingPowered(stone),
	// whose -Y neighbour (the torch, side0) weakly powers it (torch.isIndirectlyPoweringTo(0) =
	// isPoweringTo(0) = true). So the cube re-emits up -> the cell on top is indirectly powered.
	clear();
	put(8, 8, 8, RSP_TORCH_LIT_ID, 5);
	put(8, 9, 8, STONE_ID, 0);
	assert(redstone_is_indirectly_getting_powered(&w, 8, 10, 8, false) == true);
	// torch on the SIDE of a block does NOT power it (only the torch directly under it does):
	// move the torch beside the stone instead of under it -> the cell on top is NOT powered.
	clear();
	put(7, 9, 8, RSP_TORCH_LIT_ID, 1); // torch on the stone's -X face (mount meta 1)
	put(8, 9, 8, STONE_ID, 0);
	assert(redstone_is_indirectly_getting_powered(&w, 8, 10, 8, false) == false);

	// --- 3. The wiresProvidePower toggle (the headline of the boolean model) -----------------
	// A powered wire on TOP of a stone block powers the block downward (Beta side1 -> always).
	// The torch-mount query (wires_provide_power=true) sees that power; the wire's own flood
	// query (wires_provide_power=false) does NOT -- a wire never counts another wire as a source.
	clear();
	put(8, 8, 8, STONE_ID, 0);
	put(8, 9, 8, RSP_WIRE_ID, 15); // a wire sitting on the stone, strength 15
	assert(redstone_is_indirectly_providing_power_to(&w, 8, 8, 8, 0, true) == true);
	assert(redstone_is_indirectly_providing_power_to(&w, 8, 8, 8, 0, false) == false);
	// an UNPOWERED wire (strength 0) powers nothing, in either mode.
	put(8, 9, 8, RSP_WIRE_ID, 0);
	assert(redstone_is_indirectly_providing_power_to(&w, 8, 8, 8, 0, true) == false);

	// --- 4. The faithful NOT-gate input: a wire RUN dead-ending into a block's face ----------
	// Floor stone at y=7. A 2-cell wire run at (8,8,8)<->(9,8,8), strength 15, runs east-west.
	// Block B at (7,8,8) sits at the wire run's WEST end; a lit torch on TOP of B at (7,9,8).
	// The torch reads its mount (the block below it, B, side 0): B is a normal cube, so
	// isBlockGettingPowered(B) checks B's +X neighbour -- the wire at (8,8,8), side 5. That wire
	// is connected EAST (to the wire at (9,8,8)) and also links west up onto B (torch on top),
	// so wire.isPoweringTo(side5) = true (connected east, no perpendicular). B is powered ->
	// the torch's input is ON -> the inverter would turn it OFF. THIS is the case the interim
	// model and the one-hop reach (R3) both used to get wrong.
	clear();
	put(8, 7, 8, STONE_ID, 0); // floor under the wire run
	put(9, 7, 8, STONE_ID, 0);
	put(8, 8, 8, RSP_WIRE_ID, 15);
	put(9, 8, 8, RSP_WIRE_ID, 15);
	put(7, 8, 8, STONE_ID, 0);			// block B at the wire's west end
	put(7, 9, 8, RSP_TORCH_LIT_ID, 5);	// torch on top of B
	assert(redstone_is_indirectly_providing_power_to(&w, 7, 8, 8, 0, true) == true);
	// drop the wire run to strength 0 -> B no longer powered -> torch input OFF (stays lit).
	put(8, 8, 8, RSP_WIRE_ID, 0);
	put(9, 8, 8, RSP_WIRE_ID, 0);
	assert(redstone_is_indirectly_providing_power_to(&w, 7, 8, 8, 0, true) == false);

	// --- 5. A lone lit torch does NOT power its own mount (no self-oscillation) ---------------
	// torch lit (meta5) on stone B at (8,8,8), nothing else. The torch's only neighbour that
	// could power B is itself (above, side1), but torch.isIndirectlyPoweringTo(side1) = false
	// (a torch only weakly powers side 0). So B is unpowered -> the torch stays lit. Critical:
	// were this true, every torch on a block would flicker.
	clear();
	put(8, 8, 8, STONE_ID, 0);
	put(8, 9, 8, RSP_TORCH_LIT_ID, 5);
	assert(redstone_is_indirectly_providing_power_to(&w, 8, 8, 8, 0, true) == false);

	// --- 6. Pressure plate as a power source -------------------------------------------------
	// A pressed stone plate (meta 1) at (8,9,8). A cell beside it (9,9,8) is indirectly powered:
	// the plate is a non-cube, plate.isPoweringTo(any side) = (meta>0) = true. Unpressed -> off.
	clear();
	put(8, 9, 8, RSP_STONE_PLATE_ID, 1);
	assert(redstone_is_indirectly_getting_powered(&w, 9, 9, 8, false) == true);
	put(8, 9, 8, RSP_STONE_PLATE_ID, 0);
	assert(redstone_is_indirectly_getting_powered(&w, 9, 9, 8, false) == false);
	// and the plate weakly powers the block directly under it (side1) but a cube under it
	// re-emits that: stone under a pressed wood plate -> the cell under the stone is powered.
	clear();
	put(8, 9, 8, RSP_WOOD_PLATE_ID, 1);
	put(8, 8, 8, STONE_ID, 0);
	assert(redstone_is_indirectly_getting_powered(&w, 8, 7, 8, false) == true);

	// --- 7. Repeater powers ONLY its output face (directional), and only when active -----------
	// An ACTIVE repeater facing 0 (input from +Z, output to -Z) at (8,8,8). The cell on its output
	// side (8,8,7) is indirectly powered; the input side (8,8,9) and a perpendicular side (9,8,8)
	// are NOT. The IDLE repeater (93) powers nothing.
	clear();
	put(8, 8, 8, RSP_REPEATER_ACTIVE_ID, 0); // facing 0, delay idx 0
	assert(redstone_is_indirectly_getting_powered(&w, 8, 8, 7, false) == true);	 // output (-Z)
	assert(redstone_is_indirectly_getting_powered(&w, 8, 8, 9, false) == false); // input  (+Z)
	assert(redstone_is_indirectly_getting_powered(&w, 9, 8, 8, false) == false); // side   (+X)
	put(8, 8, 8, RSP_REPEATER_IDLE_ID, 0);
	assert(redstone_is_indirectly_getting_powered(&w, 8, 8, 7, false) == false); // idle: no power

	printf("test_redstone_power_golden: OK (Beta boolean power model -- torch direct/through-cube, "
		   "wiresProvidePower toggle, faithful NOT-gate input, no torch self-power, plate source, "
		   "directional repeater output)\n");
	return 0;
}
