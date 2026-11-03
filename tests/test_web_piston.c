/*
	ci-1101/45 part C: a piston DESTROYS (drops) a cobweb instead of pushing it,
	like Beta 1.7.3.

	Beta: Material.web is setNoPushMobility (mobility flag 1)
	[mcb173-retromcp/minecraft_server/.../Material.java:30]; Block.web is
	BlockWeb(30) [Block.java:53]. BlockPistonBase.tryExtend
	[BlockPistonBase.java:272-292] treats a flag-1 block as the END of the push
	column: dropBlockAsItem then setBlockWithNotify(0), and the column behind it
	shifts into the vacated cell. canPushBlock refuses a flag-1 block when
	destroy=false [BlockPistonBase.java:211-213], so a sticky piston never PULLS
	a web. The drop is BlockWeb.idDropped -> Item.silk [BlockWeb.java:26-28]
	(no harvest/tool gate on the piston path: dropBlockAsItem, not
	harvestBlock).

	CaveWii's push-vs-destroy decision is piston_mobility_flag() (NOT
	getMaterial(): enum block_material has no web/push notion; the cobweb's
	MATERIAL_STONE is unused by every piston path). This test drives the pure
	piston core (redstone_piston.c) with the REAL mobility table, so a table
	edit or a tryExtend regression that started pushing webs fails here.

	Side numbering (Beta): 0=-Y 1=+Y 2=-Z 3=+Z 4=-X 5=+X.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks_data.h"
#include "block/piston_mobility.h"
#include "block/piston_moving.h"
#include "block/redstone_piston.h"

#define G 16
static int g_id[G][G][G];
static int g_meta[G][G][G];

static int n_drop, drop_x, drop_y, drop_z, drop_id;
static int n_move_web; /* a moving-TE that STORES a web = the web was pushed */

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
	/* Block.java:53 web setHardness(4.0F); stone 1.5F. Never -1 here. */
	return id == BLOCK_COBWEB ? 4.0F : 1.5F;
}
static int t_get_mobility(void* c, int id) {
	(void)c;
	return piston_mobility_flag(id); /* the REAL table the server uses */
}
static bool t_has_te(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
	return false;
}
static bool t_power(void* c, int x, int y, int z, int side) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
	(void)side;
	return false;
}
static void t_set_block(void* c, int x, int y, int z, int id, int meta,
						bool notify) {
	(void)c;
	(void)notify;
	if(in(x, y, z)) {
		g_id[x][y][z] = id;
		g_meta[x][y][z] = meta;
	}
}
static void t_add_moving_te(void* c, int x, int y, int z, int stored_id,
							int stored_meta, int facing, bool extending,
							bool is_source) {
	(void)c;
	(void)facing;
	(void)extending;
	(void)is_source;
	if(stored_id == BLOCK_COBWEB)
		n_move_web++;
	if(in(x, y, z)) {
		g_id[x][y][z] = RS_PISTON_MOVING_ID;
		g_meta[x][y][z] = stored_meta;
	}
}
static void t_settle(void* c, int x, int y, int z) {
	(void)c;
	(void)x;
	(void)y;
	(void)z;
}
static void t_drop(void* c, int x, int y, int z, int id, int meta) {
	(void)c;
	(void)meta;
	n_drop++;
	drop_x = x;
	drop_y = y;
	drop_z = z;
	drop_id = id;
}
static void t_set_ignore(void* c, bool ignore) {
	(void)c;
	(void)ignore;
}

static const struct redstone_piston_world W = {
	.get_type = t_get_type,
	.get_meta = t_get_meta,
	.get_hardness = t_get_hardness,
	.get_mobility_flag = t_get_mobility,
	.has_tile_entity = t_has_te,
	.is_indirectly_providing_power = t_power,
	.set_block = t_set_block,
	.add_moving_te = t_add_moving_te,
	.settle_moving_te = t_settle,
	.spawn_drops = t_drop,
	.set_ignore_updates = t_set_ignore,
};

static void reset(void) {
	memset(g_id, 0, sizeof g_id);
	memset(g_meta, 0, sizeof g_meta);
	n_drop = n_move_web = 0;
	drop_x = drop_y = drop_z = drop_id = -1;
}

int main(void) {
	/* (0) the table entry itself: Material.web setNoPushMobility -> 1. */
	assert(piston_mobility_flag(BLOCK_COBWEB) == RS_MOBILITY_DESTROY);

	/* (1) canPushBlock: destroy=true allows (tryExtend), destroy=false
	 * refuses (the sticky pull) [BlockPistonBase.java:211-213]. */
	reset();
	g_id[6][5][5] = BLOCK_COBWEB;
	assert(piston_can_push_block(&W, BLOCK_COBWEB, 6, 5, 5, true));
	assert(!piston_can_push_block(&W, BLOCK_COBWEB, 6, 5, 5, false)
		   && "a sticky piston must never pull a web (flag 1)");

	/* (2) web directly in front: extend DROPS it at its cell and the head
	 * takes that cell; the web is never carried as a moving block. */
	reset();
	g_id[5][5][5] = RS_PISTON_BASE_ID;
	g_meta[5][5][5] = 5; /* facing +X */
	g_id[6][5][5] = BLOCK_COBWEB;
	assert(piston_can_extend(&W, 5, 5, 5, 5));
	assert(piston_extend(&W, 5, 5, 5, 5, false));
	printf("web in front: drops=%d at (%d,%d,%d) id=%d, web moved=%d, "
		   "cell now %d\n", n_drop, drop_x, drop_y, drop_z, drop_id,
		   n_move_web, g_id[6][5][5]);
	assert(n_drop == 1 && drop_id == BLOCK_COBWEB && drop_x == 6
		   && drop_y == 5 && drop_z == 5);
	assert(n_move_web == 0 && "web destroyed, not pushed");
	assert(g_id[6][5][5] == RS_PISTON_MOVING_ID && "head moves into the web cell");

	/* (3) stone then web: the stone is pushed INTO the web's cell; the web is
	 * the destroy-terminal (dropped), not shifted to the next cell. */
	reset();
	g_id[5][5][5] = RS_PISTON_BASE_ID;
	g_meta[5][5][5] = 5;
	g_id[6][5][5] = BLOCK_STONE;
	g_id[7][5][5] = BLOCK_COBWEB;
	assert(piston_extend(&W, 5, 5, 5, 5, false));
	printf("stone+web: drops=%d at (%d,%d,%d) id=%d, web moved=%d, "
		   "cell8=%d\n", n_drop, drop_x, drop_y, drop_z, drop_id, n_move_web,
		   g_id[8][5][5]);
	assert(n_drop == 1 && drop_id == BLOCK_COBWEB && drop_x == 7);
	assert(n_move_web == 0);
	assert(g_id[7][5][5] == RS_PISTON_MOVING_ID && "stone moves into web cell");
	assert(g_id[8][5][5] == 0 && "nothing pushed past the web");

	/* (4) the web ends the column even behind 12 movable blocks: 12 stone +
	 * web is legal (the web is not the 13th MOVABLE block). */
	reset();
	g_id[1][5][5] = RS_PISTON_BASE_ID;
	g_meta[1][5][5] = 5;
	for(int i = 0; i < 12; i++)
		g_id[2 + i][5][5] = BLOCK_STONE;
	g_id[14][5][5] = BLOCK_COBWEB;
	assert(piston_can_extend(&W, 1, 5, 5, 5));
	assert(piston_extend(&W, 1, 5, 5, 5, false));
	assert(n_drop == 1 && drop_x == 14 && n_move_web == 0);

	puts("PASS web piston (Beta: Material.web setNoPushMobility -> destroyed "
		 "+ dropped, never pushed or pulled)");
	return 0;
}
