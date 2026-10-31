/*
	Golden behavior test for Beta 1.7.3 BlockPistonBase DECISION logic (M4 pistons, phase 1),
	authored before the in-game wiring (golden-suite-first). Drives the pure source/block/
	redstone_piston.c over a flat STUB cell grid via struct redstone_piston_world -- no
	server_world, no power model (the power query is a stub source-set so this isolates the
	PISTON cell-selection logic; the boolean power model has its own golden). Every expected
	value is transcribed from the decompile (740c583, BlockPistonBase).

	Pins: the quasi-connectivity power check (incl. the "block above powers the piston" BUD quirk
	and the front-face exclusion); canPushBlock per mobility class; canExtend at the 12-block limit;
	the updatePistonState extend/retract truth table; determineOrientation.

	Side numbering (Beta): 0=-Y 1=+Y 2=-Z 3=+Z 4=-X 5=+X.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/redstone_piston.h"

#define G 16
static int g_id[G][G][G];
static int g_meta[G][G][G];
static bool g_power[G][G][G]; // a cell that "provides indirect power" (the stub power query)
static bool g_te[G][G][G];	 // a cell with a tile entity
static int g_mob[256];		 // per-id mobility flag
static float g_hard[256];	 // per-id hardness

#define STONE 1		// movable, hardness 1.5
#define BEDROCK 7	// hardness -1 (unbreakable)
#define DESTROY_ID 31	// mobilityFlag 1 (destroyed-on-push, ends a chain)
#define IMMOVABLE_ID 30 // mobilityFlag 2 (blocks a push)

static bool in(int x, int y, int z) {
	return x >= 0 && x < G && y >= 0 && y < G && z >= 0 && z < G;
}
static int t_get_type(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) ? g_id[x][y][z] : 0;
}
static int t_get_meta(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) ? g_meta[x][y][z] : 0;
}
static float t_get_hardness(void* c, int id) {
	(void)c;
	return g_hard[id & 0xFF];
}
static int t_get_mobility(void* c, int id) {
	(void)c;
	return g_mob[id & 0xFF];
}
static bool t_has_te(void* c, int x, int y, int z) {
	(void)c;
	return in(x, y, z) && g_te[x][y][z];
}
static bool t_power(void* c, int x, int y, int z, int side) {
	(void)c;
	(void)side; // the stub ignores the side: "does this cell provide power" (cell-selection test)
	return in(x, y, z) && g_power[x][y][z];
}

static struct redstone_piston_world W = {
	.get_type = t_get_type,
	.get_meta = t_get_meta,
	.get_hardness = t_get_hardness,
	.get_mobility_flag = t_get_mobility,
	.has_tile_entity = t_has_te,
	.is_indirectly_providing_power = t_power,
};

static void clear(void) {
	memset(g_id, 0, sizeof(g_id));
	memset(g_meta, 0, sizeof(g_meta));
	memset(g_power, 0, sizeof(g_power));
	memset(g_te, 0, sizeof(g_te));
}

int main(void) {
	for(int i = 0; i < 256; i++) {
		g_mob[i] = RS_MOBILITY_NORMAL;
		g_hard[i] = 1.5F;
	}
	g_hard[BEDROCK] = -1.0F;
	g_mob[DESTROY_ID] = RS_MOBILITY_DESTROY;
	g_mob[IMMOVABLE_ID] = RS_MOBILITY_IMMOVABLE;

	// piston at (8,8,8). Tests use facing 0 (front = -Y, the cell below) unless noted.

	// --- 1. QUASI-CONNECTIVITY: the block-above quirk powers the piston -----------------------
	clear();
	g_power[8][10][8] = true; // a source at (x, y+2, z) -- the BUD "block above" cell
	assert(piston_is_indirectly_powered(&W, 8, 8, 8, 0) == true);

	// --- 2. The FRONT face is EXCLUDED (a piston ignores power on its output face) ------------
	clear();
	g_power[8][7][8] = true; // a source on the front face (front = side 0 = -Y, cell y-1)
	assert(piston_is_indirectly_powered(&W, 8, 8, 8, 0) == false);
	// but the SAME source powers a piston facing a DIFFERENT way (front != 0), as a -Y face source:
	assert(piston_is_indirectly_powered(&W, 8, 8, 8, 5) == true); // facing +X -> the -Y face counts

	// --- 3. A side face source powers; the piston's own cell powers ---------------------------
	clear();
	g_power[9][8][8] = true; // +X face (side 5), with front=0 -> checked
	assert(piston_is_indirectly_powered(&W, 8, 8, 8, 0) == true);
	clear();
	g_power[8][8][8] = true; // the piston's own cell
	assert(piston_is_indirectly_powered(&W, 8, 8, 8, 0) == true);
	clear(); // no source anywhere
	assert(piston_is_indirectly_powered(&W, 8, 8, 8, 0) == false);

	// --- 4. canPushBlock per mobility class ---------------------------------------------------
	clear();
	assert(piston_can_push_block(&W, STONE, 8, 8, 8, false) == true);		 // normal: pushable
	assert(piston_can_push_block(&W, RS_PISTON_OBSIDIAN_ID, 8, 8, 8, true) == false); // obsidian
	assert(piston_can_push_block(&W, BEDROCK, 8, 8, 8, true) == false);		 // hardness -1
	assert(piston_can_push_block(&W, IMMOVABLE_ID, 8, 8, 8, true) == false);	 // mobility 2
	assert(piston_can_push_block(&W, DESTROY_ID, 8, 8, 8, false) == false);	 // mobility 1, !destroy
	assert(piston_can_push_block(&W, DESTROY_ID, 8, 8, 8, true) == true);	 // mobility 1, destroy
	g_te[8][8][8] = true;
	assert(piston_can_push_block(&W, STONE, 8, 8, 8, true) == false);		 // a tile entity blocks
	g_te[8][8][8] = false;
	// a piston base is pushable only when unpowered:
	g_meta[8][8][8] = 0;
	assert(piston_can_push_block(&W, RS_PISTON_BASE_ID, 8, 8, 8, true) == true);
	g_meta[8][8][8] = 8; // powered/extended
	assert(piston_can_push_block(&W, RS_PISTON_BASE_ID, 8, 8, 8, true) == false);

	// --- 5. canExtend: the 12-block limit -----------------------------------------------------
	// piston at (2,8,8) facing 5 (+X) -> the push column is x = 3..15.
	clear();
	for(int x = 3; x <= 14; x++) // 12 movable stone, then air at x=15
		g_id[x][8][8] = STONE;
	assert(piston_can_extend(&W, 2, 8, 8, 5) == true); // push exactly 12 -> legal
	g_id[15][8][8] = STONE;							   // now 13 movable in a row
	assert(piston_can_extend(&W, 2, 8, 8, 5) == false); // 13 > the 12-block limit
	// a destroy-on-push block ENDS the chain -> pushable even past where solids would fail:
	clear();
	for(int x = 3; x <= 14; x++)
		g_id[x][8][8] = STONE;
	g_id[15][8][8] = DESTROY_ID; // the 13th is destroyed, not pushed -> the chain ends -> legal
	assert(piston_can_extend(&W, 2, 8, 8, 5) == true);
	// an IMMOVABLE block in the column blocks the push:
	clear();
	g_id[3][8][8] = STONE;
	g_id[4][8][8] = IMMOVABLE_ID;
	assert(piston_can_extend(&W, 2, 8, 8, 5) == false);
	// a clear column is trivially extendable:
	clear();
	assert(piston_can_extend(&W, 2, 8, 8, 5) == true);

	// --- 6. updatePistonState truth table (piston facing 0, push column = below, clear) -------
	// Use facing 5 (+X) so the push column (x=9..) is clear air -> canExtend always true here, and
	// drive the power via the block-above quirk cell (8,10,8) which is independent of the column.
	clear();
	g_meta[8][8][8] = 5; // facing 5, unpowered
	g_power[8][10][8] = true; // powered (block-above)
	assert(piston_should_extend(&W, 8, 8, 8) == true);	// powered + unpowered-bit + canExtend
	assert(piston_should_retract(&W, 8, 8, 8) == false);
	g_meta[8][8][8] = 5 | 8; // now powered/extended, still has power
	assert(piston_should_extend(&W, 8, 8, 8) == false);	// already extended
	assert(piston_should_retract(&W, 8, 8, 8) == false); // still powered -> stay extended
	g_power[8][10][8] = false; // power removed, still extended
	assert(piston_should_retract(&W, 8, 8, 8) == true);	// -> retract
	assert(piston_should_extend(&W, 8, 8, 8) == false);
	g_meta[8][8][8] = 5; // unpowered bit, no power
	assert(piston_should_extend(&W, 8, 8, 8) == false);	// idle
	assert(piston_should_retract(&W, 8, 8, 8) == false);
	// canExtend gates extend: powered but an immovable block blocks the push -> NO extend.
	clear();
	g_meta[8][8][8] = 5;
	g_power[8][10][8] = true;
	g_id[9][8][8] = IMMOVABLE_ID; // directly in front (the +X push cell)
	assert(piston_should_extend(&W, 8, 8, 8) == false); // powered but can't push -> no extend

	// --- 7. determineOrientation (player eye offset 1.62) -------------------------------------
	// placer next to the block, level: facing comes from the yaw octant.
	assert(piston_determine_orientation(8.5F, 8.0F, 8.5F, 1.62F, 0.0F, 8, 8, 8) == 2);	// yaw 0  -> -Z
	assert(piston_determine_orientation(8.5F, 8.0F, 8.5F, 1.62F, 90.0F, 8, 8, 8) == 5);	// yaw 90 -> +X
	assert(piston_determine_orientation(8.5F, 8.0F, 8.5F, 1.62F, 180.0F, 8, 8, 8) == 3); // yaw 180-> +Z
	assert(piston_determine_orientation(8.5F, 8.0F, 8.5F, 1.62F, 270.0F, 8, 8, 8) == 4); // yaw 270-> -X
	// placer well above the block (within 2 horiz) -> UP; well below -> DOWN.
	assert(piston_determine_orientation(8.5F, 12.0F, 8.5F, 1.62F, 0.0F, 8, 8, 8) == 1); // above -> UP
	assert(piston_determine_orientation(8.5F, 5.0F, 8.5F, 1.62F, 0.0F, 8, 8, 8) == 0);	 // below -> DOWN
	// placer FAR horizontally (>2): the up/down special-case does NOT fire -> yaw octant.
	assert(piston_determine_orientation(20.0F, 12.0F, 8.5F, 1.62F, 90.0F, 8, 8, 8) == 5); // far -> +X

	printf("test_piston_logic_golden: OK (Beta BlockPistonBase -- quasi-connectivity incl. the "
		   "block-above quirk + front exclusion, canPushBlock per mobility, canExtend 12-limit, the "
		   "extend/retract truth table, determineOrientation)\n");
	return 0;
}
