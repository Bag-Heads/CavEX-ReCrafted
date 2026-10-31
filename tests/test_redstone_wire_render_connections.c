/*
	Golden test for the Beta 1.7.3 redstone-wire RENDER connectivity + shape selection.

	render_block_redstone_wire (render_block.c) picks the dust's rendered shape from the SAME
	connection probe the power model uses: redstone_wire_connections() (redstone_power.c) -- which is
	Beta BlockRedstoneWire.isPoweringTo's four side flags, BYTE-IDENTICAL to RenderBlocks.renderBlockRed-
	stoneWire's four side flags (decompile 740c583, cross-checked vs mc173 power.rs). This test pins:

	  1. the 4 connection booleans for every wire topology a player builds -- isolated dot, straight
	     E-W / N-S, L-corner, T-junction, +-cross, connect-to-source (lever/torch), DON'T connect to a
	     plain solid wall, and the +-1 vertical "stair" links (down a step, up a step, up blocked by a
	     closed ceiling);
	  2. the Beta wire shape index the renderer derives from those booleans (0 = CROSS tile, 1 = LINE E-W,
	     2 = LINE N-S) -- proving L-corners / T's / +'s render as the (trimmed) CROSS, never a line.

	The renderer is graphics code (needs an atlas + display list), so the shape mapping is mirrored
	here as wire_shape() -- it MUST stay identical to render_block_redstone_wire. Connectivity itself
	is the real shared function, so it cannot drift.

	Side numbering (Beta), as consumed by is_power_provider_or_wire: 0=-Y 1=+Y 2=-Z 3=+Z 4=-X 5=+X.
	conn[] index (redstone_wire_connections): 0=west(-X) 1=east(+X) 2=north(-Z) 3=south(+Z).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/redstone_power.h"

#define STONE_ID 1 // a normal opaque cube (floor / wall / wire support)
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
	memset(g_id, 0, sizeof(g_id));
	memset(g_meta, 0, sizeof(g_meta));
}
static void put(int x, int y, int z, int id, int meta) {
	g_id[x][y][z] = id;
	g_meta[x][y][z] = meta;
}

// MUST stay identical to render_block_redstone_wire (render_block.c): Beta var35.
static int wire_shape(const bool conn[4]) {
	bool cw = conn[0], ce = conn[1], cn = conn[2], cs = conn[3];
	int shape = 0;
	if((cw || ce) && !cn && !cs)
		shape = 1; // LINE East-West
	if((cn || cs) && !ce && !cw)
		shape = 2; // LINE North-South
	return shape;
}

// Probe the wire at the fixed centre (8,8,8) and assert its 4 connections + derived shape.
static struct redstone_power_world W;
static void check(const char* name, bool ew, bool ee, bool en, bool es, int want_shape) {
	bool conn[4];
	redstone_wire_connections(&W, 8, 8, 8, conn);
	if(conn[0] != ew || conn[1] != ee || conn[2] != en || conn[3] != es) {
		fprintf(stderr,
				"FAIL %s: conn = {W%d E%d N%d S%d}, want {W%d E%d N%d S%d}\n", name,
				conn[0], conn[1], conn[2], conn[3], ew, ee, en, es);
		assert(0);
	}
	int got = wire_shape(conn);
	if(got != want_shape) {
		fprintf(stderr, "FAIL %s: shape %d, want %d\n", name, got, want_shape);
		assert(0);
	}
}

// Centre wire on a stone floor; helper resets to that baseline each case.
static void base_wire(void) {
	clear();
	put(8, 7, 8, STONE_ID, 0);	   // floor under the centre wire
	put(8, 8, 8, RSP_WIRE_ID, 0);  // the wire under test
}
// Put a floored wire neighbour at the same y as the centre.
static void wire_at(int x, int y, int z) {
	put(x, y - 1, z, STONE_ID, 0);
	put(x, y, z, RSP_WIRE_ID, 0);
}

int main(void) {
	W = (struct redstone_power_world) {
		.ctx = NULL,
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.is_normal_cube = t_is_normal_cube,
	};

	// 1. Isolated dot -> no connections -> CROSS (drawn as the full dust dot, untrimmed).
	base_wire();
	check("isolated dot", false, false, false, false, 0);

	// 2. Straight East-West -> LINE E-W (var35 == 1).
	base_wire();
	wire_at(7, 8, 8); // west
	wire_at(9, 8, 8); // east
	check("straight E-W", true, true, false, false, 1);

	// 3. Straight North-South -> LINE N-S (var35 == 2).
	base_wire();
	wire_at(8, 8, 7); // north (-Z)
	wire_at(8, 8, 9); // south (+Z)
	check("straight N-S", false, false, true, true, 2);

	// 4. L-corner (east + south) -> CROSS, trimmed to an L (NOT a line). This is the headline
	//    Beta behaviour the old renderer got wrong (it only ever drew the straight line).
	base_wire();
	wire_at(9, 8, 8); // east
	wire_at(8, 8, 9); // south
	check("L-corner E+S", false, true, false, true, 0);

	// 5. T-junction (west + east + south) -> CROSS, trimmed to a T.
	base_wire();
	wire_at(7, 8, 8);
	wire_at(9, 8, 8);
	wire_at(8, 8, 9);
	check("T-junction W+E+S", true, true, false, true, 0);

	// 6. Full + cross -> CROSS, all four arms.
	base_wire();
	wire_at(7, 8, 8);
	wire_at(9, 8, 8);
	wire_at(8, 8, 7);
	wire_at(8, 8, 9);
	check("plus cross", true, true, true, true, 0);

	// 7. Connect to a power SOURCE (lever) on the east -> single arm E -> LINE E-W.
	base_wire();
	put(9, 7, 8, STONE_ID, 0);				// the lever's support (a cube)
	put(9, 8, 8, RSP_LEVER_ID, 1 | 8);		// wall lever, ON (canProvidePower regardless of on/off)
	check("connect to lever (E)", false, true, false, false, 1);

	// 8. Connect to a lit redstone torch on the east -> single arm E.
	base_wire();
	put(9, 7, 8, STONE_ID, 0);
	put(9, 8, 8, RSP_TORCH_LIT_ID, 5); // torch standing on the cube below it
	check("connect to torch (E)", false, true, false, false, 1);

	// 9. A plain solid wall on the east is NOT a connection (not wire/source, and being a normal
	//    cube suppresses the diagonal-down probe). The wire stays an isolated dot.
	base_wire();
	put(9, 8, 8, STONE_ID, 0);
	check("no connect to wall (E)", false, false, false, false, 0);

	// 10. Stair DOWN to the east: air at (9,8,8), wire one step down at (9,7,8). The dust bends east.
	base_wire();
	// (9,8,8) left as air; (9,7,8) wire on a floor at (9,6,8)
	put(9, 6, 8, STONE_ID, 0);
	put(9, 7, 8, RSP_WIRE_ID, 0);
	check("stair-down (E)", false, true, false, false, 1);

	// 11. Stair UP to the east: open ceiling above the centre, a solid cube east, wire on TOP of it.
	base_wire();
	put(9, 8, 8, STONE_ID, 0);	  // solid cube to climb
	put(9, 9, 8, RSP_WIRE_ID, 0); // wire on top of that cube
	// (8,9,8) is air -> ceiling open -> the climb-up link is allowed.
	check("stair-up (E), ceiling open", false, true, false, false, 1);

	// 12. Same as 11 but the centre wire's ceiling is CLOSED (solid block above) -> the stair-up
	//     link is suppressed -> no connection (isolated dot). Guards the ceiling gate.
	base_wire();
	put(9, 8, 8, STONE_ID, 0);
	put(9, 9, 8, RSP_WIRE_ID, 0);
	put(8, 9, 8, STONE_ID, 0); // closed ceiling over the centre wire
	check("stair-up (E) blocked by closed ceiling", false, false, false, false, 0);

	// 13. Repeater on the east connects ONLY when its output faces the wire (Beta foot remap).
	//     conn[1] probes the east neighbour with side 3; the repeater branch returns
	//     side == footInvisibleFaceRemap[meta&3] == {2,3,0,1}[meta&3], so meta&3 == 1 connects.
	base_wire();
	put(9, 7, 8, STONE_ID, 0);
	put(9, 8, 8, RSP_REPEATER_ACTIVE_ID, 1); // meta&3 == 1 -> foot_remap == 3 -> connects
	check("repeater facing wire (E)", false, true, false, false, 1);
	base_wire();
	put(9, 7, 8, STONE_ID, 0);
	put(9, 8, 8, RSP_REPEATER_ACTIVE_ID, 0); // meta&3 == 0 -> foot_remap == 2 != 3 -> no connect
	check("repeater not facing wire (E)", false, false, false, false, 0);

	printf("test_redstone_wire_render_connections: OK (Beta var26-29 connectivity + var35 cross/line "
		   "shape -- dot, straight, L, T, +, source connect, no-wall, stair up/down/ceiling, "
		   "repeater facing)\n");
	return 0;
}
