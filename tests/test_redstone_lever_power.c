/*
	Golden behavior test for the Beta 1.7.3 LEVER (BlockLever, id 69) as a redstone power source.

	Wires the lever the way PRODUCTION does (redstone_server.c): the wire flood
	(redstone_logic.c redstone_wire_update) reads its source via is_indirectly_powered, which is
	Beta World.isBlockIndirectlyGettingPowered with wiresProvidePower=false -- implemented here by
	calling the REAL redstone_power.c (redstone_is_indirectly_getting_powered) over a shared stub
	grid. So this test drives BOTH layers exactly as the live server stacks them, and asserts the
	wire's integer 0-15 strength (the value the player sees as wire brightness):

	  - lever OFF (meta = orientation, bit 8 clear): BlockLever.isPoweringTo = (meta&8)>0 = false
	    -> the wire is not powered -> strength 0.
	  - lever ON  (meta |= 8): isPoweringTo = true for every side -> the adjacent wire's source is
	    ON -> a non-wire source injects a flat 15 -> the adjacent wire settles to strength 15.
	  - lever OFF again: back to 0 (the toggle is reversible).

	Every expected value is transcribed from the decompile (740c583 BlockLever.isPoweringTo /
	isIndirectlyPoweringTo, BlockRedstoneWire.calculateCurrentChanges flat-15 source). Beta has NO
	integer power query: the 0-15 gradient lives only in wire metadata; the lever is a boolean source.

	Side numbering (Beta): 0=-Y 1=+Y 2=-Z 3=+Z 4=-X 5=+X.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/redstone_logic.h"
#include "block/redstone_power.h"

#define STONE_ID 1 // a normal opaque cube (the floor / wire support)
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
static void t_set_meta(void* c, int x, int y, int z, int m) {
	(void)c;
	if(in(x, y, z))
		g_meta[x][y][z] = m;
}
static bool t_is_normal_cube(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) && g_id[x][y][z] == STONE_ID;
}

// The wire flood's source query: Beta World.isBlockIndirectlyGettingPowered with
// wiresProvidePower=false, delegated to the REAL redstone_power.c -- identical to redstone_server.c
// rs_wire_is_powered. This is what makes the lever count (or not) as a flat-15 source.
static bool t_is_indirectly_powered(void* c, int x, int y, int z) {
	struct redstone_power_world pw = {
		.ctx = c,
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.is_normal_cube = t_is_normal_cube,
	};
	return redstone_is_indirectly_getting_powered(&pw, x, y, z, false);
}
static void t_mark(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
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
	struct redstone_world w = {
		.ctx = NULL,
		.get_type = t_get_type,
		.get_meta = t_get_meta,
		.set_meta = t_set_meta,
		.is_normal_cube = t_is_normal_cube,
		.is_indirectly_powered = t_is_indirectly_powered,
		.mark_needs_update = t_mark,
	};

	// Layout (a player's "lever -> wire" hookup): a stone block at (8,8,8); a redstone WIRE on the
	// ground at (9,8,8) on a stone floor (9,7,8); a LEVER at (10,8,8) mounted on the stone face of a
	// support block at (11,8,8), so the lever is the wire's +X neighbour-of-neighbour... actually we
	// keep it adjacent: lever directly +X of the wire so isPoweringTo feeds the wire's source query.
	//
	// Concretely: wire at (9,8,8); lever at (10,8,8) with orientation 1 (mounted on the -X face of a
	// cube at (11,8,8), i.e. a wall lever facing the wire). The wire's isBlockIndirectlyGettingPowered
	// checks its +X neighbour (the lever, side 5): a non-cube whose isPoweringTo (when ON) = true.
	clear();
	put(9, 7, 8, STONE_ID, 0);				   // floor under the wire
	put(9, 8, 8, RSP_WIRE_ID, 0);			   // the wire, starts unpowered
	put(11, 8, 8, STONE_ID, 0);				   // the lever's mounting block (cube)
	put(10, 8, 8, RSP_LEVER_ID, 1);			   // lever, orientation 1 (wall, -X face), bit 8 clear = OFF

	// --- lever OFF: wire is not powered -> strength 0 ---------------------------------------------
	// Sanity: the boolean source query must be false for an OFF lever (BlockLever.isPoweringTo=false).
	assert(t_is_indirectly_powered(NULL, 9, 8, 8) == false);
	redstone_wire_update(&w, 9, 8, 8);
	assert(g_meta[9][8][8] == 0); // OFF lever -> wire strength 0

	// --- lever ON: flat-15 source -> adjacent wire settles to 15 ----------------------------------
	g_meta[10][8][8] |= 8; // toggle the lever ON (set bit 8); orientation (meta&7) stays 1
	assert(t_is_indirectly_powered(NULL, 9, 8, 8) == true); // BlockLever.isPoweringTo(any side)=true
	redstone_wire_update(&w, 9, 8, 8);
	assert(g_meta[9][8][8] == 15); // ON lever -> wire strength 15 (flat-15 non-wire source)

	// --- lever OFF again: reversible toggle, back to 0 --------------------------------------------
	g_meta[10][8][8] &= ~8; // toggle OFF
	assert(t_is_indirectly_powered(NULL, 9, 8, 8) == false);
	redstone_wire_update(&w, 9, 8, 8);
	assert(g_meta[9][8][8] == 0); // OFF -> wire strength 0 again

	// --- the lever's STRONG power re-emits through its MOUNT cube (isIndirectlyPoweringTo) ----------
	// Beta BlockLever orientation 1 is a WALL lever mounted on the cube at its -X: setBlockBoundsBasedOnState
	// (orientation 1) hugs the box to x=0 (the -X wall), and onBlockPlaced sets meta 1 only when clicking the face
	// whose support cube is at x-1. isIndirectlyPoweringTo(o==1) returns true for side 5 -- it strong-powers
	// the block that queries it from side 5, i.e. its -X mount cube. That cube then re-emits the lever's power
	// to a wire sitting on top of it. (This is why a lever placed on a block face powers redstone routed up
	// and over that block. The mount cube is at the lever's -X, NOT +X.)
	clear();
	put(9, 8, 8, STONE_ID, 0);		// the lever's MOUNT cube, at the lever's -X (orientation 1)
	put(10, 8, 8, RSP_LEVER_ID, 1); // wall lever, orientation 1 (mounted on the -X cube), OFF
	put(9, 9, 8, RSP_WIRE_ID, 0);	// wire on TOP of the mount cube
	// OFF: the cube is not powered -> the wire on top reads 0.
	assert(t_is_indirectly_powered(NULL, 9, 9, 8) == false);
	redstone_wire_update(&w, 9, 9, 8);
	assert(g_meta[9][9][8] == 0);
	// ON: lever.isIndirectlyPoweringTo(side 5) = true -> the -X cube is powered -> re-emits up ->
	// the wire on top settles to a flat-15 source.
	g_meta[10][8][8] |= 8;
	assert(t_is_indirectly_powered(NULL, 9, 9, 8) == true);
	redstone_wire_update(&w, 9, 9, 8);
	assert(g_meta[9][9][8] == 15);

	printf("test_redstone_lever_power: OK (Beta BlockLever as a toggleable source -- OFF=0, ON=15 "
		   "adjacent wire via isPoweringTo, plus the mount-face weak power re-emitted through a cube)\n");
	return 0;
}
