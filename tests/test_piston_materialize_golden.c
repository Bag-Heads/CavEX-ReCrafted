/*
	Golden behavior test for the Beta 1.7.3 BlockPistonBase MATERIALIZATION (M4 pistons, phase 2a):
	piston_extend (tryExtend) and piston_retract (playBlock eventId==1). Drives the pure source/block/
	redstone_piston.c over a flat STUB cell grid and RECORDS every world-mutating op the move emits
	(add_moving_te / set_block / settle_moving_te / spawn_drops), then asserts the exact sequence against
	the decompile (740c583, BlockPistonBase.tryExtend + playBlock). No server_world, no tile-entity store
	-- this isolates the cell-shift SEQUENCE (the hardest-to-get-right transcription) the way Phase 1
	isolated the decision logic.

	Side numbering (Beta): 0=-Y 1=+Y 2=-Z 3=+Z 4=-X 5=+X.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "block/piston_moving.h"
#include "block/redstone_piston.h"

#define G 24
static int g_id[G][G][G];
static int g_meta[G][G][G];
static bool g_te[G][G][G];
static int g_mte_facing[G][G][G]; // a moving-TE's stored facing (for moving_te_state)
static bool g_mte_ext[G][G][G];	  // a moving-TE's extending flag
static int g_mob[256];
static float g_hard[256];

#define STONE 1
#define BEDROCK 7
#define WOOL 35
#define DESTROY_ID 31	// mobilityFlag 1 (destroyed-on-push)
#define IMMOVABLE_ID 30 // mobilityFlag 2 (immovable)

// --- recorded op log -------------------------------------------------------------------------------
enum op_kind { OP_MOVE, OP_SET, OP_SETTLE, OP_DROP };
struct op {
	enum op_kind kind;
	int x, y, z;
	int a, b, c;	  // MOVE: stored_id, stored_meta, facing | SET: id, meta, notify | DROP: id, meta
	bool extending, is_source;
};
static struct op ops[64];
static int n_ops;

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
	(void)x;
	(void)y;
	(void)z;
	(void)side;
	return false;
}
static void t_set_block(void* c, int x, int y, int z, int id, int meta, bool notify) {
	(void)c;
	ops[n_ops] = (struct op) {.kind = OP_SET, .x = x, .y = y, .z = z, .a = id, .b = meta, .c = notify};
	n_ops++;
	if(in(x, y, z)) {
		g_id[x][y][z] = id;
		g_meta[x][y][z] = meta;
	}
}
static void t_add_moving_te(void* c, int x, int y, int z, int stored_id, int stored_meta, int facing,
							bool extending, bool is_source) {
	(void)c;
	ops[n_ops] = (struct op) {.kind = OP_MOVE,
							  .x = x,
							  .y = y,
							  .z = z,
							  .a = stored_id,
							  .b = stored_meta,
							  .c = facing,
							  .extending = extending,
							  .is_source = is_source};
	n_ops++;
	if(in(x, y, z)) { // mirror Beta: the cell becomes id-36 (so a later get_type sees the moving block)
		g_id[x][y][z] = RS_PISTON_MOVING_ID;
		// BlockPistonBase.tryExtend/playBlock pass the STORED block metadata to setBlockAndMetadata.
		g_meta[x][y][z] = stored_meta;
		g_mte_facing[x][y][z] = facing;
		g_mte_ext[x][y][z] = extending;
	}
}
// Beta TileEntityPiston.getPistonOrientation()/isExtending() for the var14 gate.
static bool t_moving_te_state(void* c, int x, int y, int z, int* facing, bool* extending) {
	(void)c;
	if(!in(x, y, z) || g_id[x][y][z] != RS_PISTON_MOVING_ID)
		return false;
	if(facing)
		*facing = g_mte_facing[x][y][z];
	if(extending)
		*extending = g_mte_ext[x][y][z];
	return true;
}
static void t_set_ignore(void* c, bool ignore) {
	(void)c;
	(void)ignore; // the stub has no neighbour cascade; the reopen toggling is a glue concern (E2E covers it)
}
static void t_settle(void* c, int x, int y, int z) {
	(void)c;
	ops[n_ops] = (struct op) {.kind = OP_SETTLE, .x = x, .y = y, .z = z};
	n_ops++;
}
static void t_drop(void* c, int x, int y, int z, int id, int meta) {
	(void)c;
	ops[n_ops] = (struct op) {.kind = OP_DROP, .x = x, .y = y, .z = z, .a = id, .b = meta};
	n_ops++;
}

static struct redstone_piston_world W = {
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
	.moving_te_state = t_moving_te_state,
};

static void clear(void) {
	memset(g_id, 0, sizeof(g_id));
	memset(g_meta, 0, sizeof(g_meta));
	memset(g_te, 0, sizeof(g_te));
	memset(g_mte_facing, 0, sizeof(g_mte_facing));
	memset(g_mte_ext, 0, sizeof(g_mte_ext));
	n_ops = 0;
}

// assertion helpers (print the whole log on mismatch for debuggability)
static void dump(void) {
	for(int i = 0; i < n_ops; i++) {
		const char* k = ops[i].kind == OP_MOVE	  ? "MOVE"
			: ops[i].kind == OP_SET				  ? "SET "
			: ops[i].kind == OP_SETTLE			  ? "STTL"
												  : "DROP";
		printf("  [%d] %s (%d,%d,%d) a=%d b=%d c=%d ext=%d source=%d\n", i, k, ops[i].x, ops[i].y,
			   ops[i].z, ops[i].a, ops[i].b, ops[i].c, ops[i].extending, ops[i].is_source);
	}
}
#define EXPECT(cond)                                                                                   \
	do {                                                                                               \
		if(!(cond)) {                                                                                  \
			printf("FAIL: %s\n", #cond);                                                               \
			dump();                                                                                    \
			assert(cond);                                                                              \
		}                                                                                              \
	} while(0)

static bool is_move(int i, int x, int y, int z, int sid, int facing, bool ext, bool source) {
	return ops[i].kind == OP_MOVE && ops[i].x == x && ops[i].y == y && ops[i].z == z && ops[i].a == sid
		&& ops[i].c == facing && ops[i].extending == ext && ops[i].is_source == source;
}
static bool is_set(int i, int x, int y, int z, int id) {
	return ops[i].kind == OP_SET && ops[i].x == x && ops[i].y == y && ops[i].z == z && ops[i].a == id;
}
static bool is_settle(int i, int x, int y, int z) {
	return ops[i].kind == OP_SETTLE && ops[i].x == x && ops[i].y == y && ops[i].z == z;
}
static bool is_drop(int i, int x, int y, int z, int id) {
	return ops[i].kind == OP_DROP && ops[i].x == x && ops[i].y == y && ops[i].z == z && ops[i].a == id;
}

/* Exact Beta TileEntityPiston two-float lifecycle (decompile 740c583). */
static void assert_moving_lifecycle(void) {
	struct piston_moving_state s = {
		.stored_id = WOOL,
		.stored_meta = 13,
		.facing = 5,
		.flags = PISTON_MOVING_FLAG_EXTENDING,
	};
	struct piston_moving_tick_result r;
	EXPECT(piston_moving_progress(&s, 0.0F) == 0.0F);
	EXPECT(piston_moving_render_scalar(&s, 0.0F) == -1.0F);
	EXPECT(piston_moving_collision_shift(&s) == 1.0F);

	piston_moving_advance(&s, &r);
	EXPECT(s.last_progress == 0.0F && s.current_progress == 0.5F);
	EXPECT(r.push_entities && !r.settle);
	EXPECT(r.push_progress == 0.5F && r.push_amount == 0.5625F);
	EXPECT(piston_moving_progress(&s, 0.5F) == 0.25F);
	EXPECT(piston_moving_render_scalar(&s, 0.5F) == -0.75F);
	EXPECT(piston_moving_collision_shift(&s) == 1.0F);
	EXPECT(piston_moving_push_shift(&s, r.push_progress) == 0.5F);

	piston_moving_advance(&s, &r);
	EXPECT(s.last_progress == 0.5F && s.current_progress == 1.0F);
	EXPECT(r.push_entities && !r.settle);
	EXPECT(r.push_progress == 1.0F && r.push_amount == 0.5625F);
	EXPECT(piston_moving_collision_shift(&s) == 0.5F);
	EXPECT(piston_moving_push_shift(&s, r.push_progress) == 0.0F);

	piston_moving_advance(&s, &r);
	EXPECT(s.last_progress == 1.0F && s.current_progress == 1.0F);
	EXPECT(r.push_entities && r.settle);
	EXPECT(r.push_progress == 1.0F && r.push_amount == 0.25F);

	/* readFromNBT sets both fields to any valid saved progress; a countdown cannot represent .25. */
	s.current_progress = s.last_progress = 0.25F;
	EXPECT(piston_moving_progress(&s, 0.75F) == 0.25F);
	piston_moving_advance(&s, &r);
	EXPECT(s.last_progress == 0.25F && s.current_progress == 0.75F);
	EXPECT(r.push_progress == 0.75F && r.push_amount == 0.5625F);

	struct piston_moving_state retract = {
		.stored_id = RS_PISTON_BASE_ID,
		.facing = 5,
		.flags = PISTON_MOVING_FLAG_SOURCE,
	};
	EXPECT(!piston_moving_is_extending(&retract));
	EXPECT(piston_moving_is_source(&retract));
	EXPECT(piston_moving_render_scalar(&retract, 0.0F) == 1.0F);
	EXPECT(piston_moving_collision_shift(&retract) == 0.0F);
	EXPECT(piston_moving_push_shift(&retract, 0.0F) == -1.0F);
	piston_moving_advance(&retract, &r);
	EXPECT(!r.push_entities && !r.settle);
	EXPECT(retract.last_progress == 0.0F && retract.current_progress == 0.5F);
	EXPECT(piston_moving_render_scalar(&retract, 1.0F) == 0.5F);

	retract.current_progress = retract.last_progress = 0.25F;
	EXPECT(piston_moving_force_finish(&retract));
	EXPECT(retract.current_progress == 1.0F && retract.last_progress == 1.0F);
	EXPECT(!piston_moving_force_finish(&retract));

	struct piston_moving_render_plan plan;
	struct piston_moving_state head = {
		.stored_id = RS_PISTON_HEAD_ID,
		.facing = 5,
		.flags = PISTON_MOVING_FLAG_EXTENDING,
		.current_progress = 0x1.fffffep-2F,
		.last_progress = 0x1.fffffep-2F,
	};
	piston_moving_make_render_plan(&head, 0.0F, &plan);
	EXPECT(plan.branch == PISTON_MOVING_RENDER_HEAD_SHORT && !plan.full_rod);
	head.current_progress = head.last_progress = 0.5F;
	piston_moving_make_render_plan(&head, 0.0F, &plan);
	EXPECT(plan.branch == PISTON_MOVING_RENDER_STORED && plan.full_rod);
	retract.current_progress = retract.last_progress = 0.25F;
	piston_moving_make_render_plan(&retract, 0.0F, &plan);
	EXPECT(plan.branch == PISTON_MOVING_RENDER_RETRACT_SOURCE
		   && plan.full_rod && plan.stationary_base);
	EXPECT(plan.translation[0] == 0.75F
		   && plan.translation[1] == 0.0F && plan.translation[2] == 0.0F);
	retract.current_progress = retract.last_progress = 1.0F;
	piston_moving_make_render_plan(&retract, 0.0F, &plan);
	EXPECT(plan.branch == PISTON_MOVING_RENDER_NONE);
}

int main(void) {
	assert_moving_lifecycle();
	for(int i = 0; i < 256; i++) {
		g_mob[i] = RS_MOBILITY_NORMAL;
		g_hard[i] = 1.5F;
	}
	g_hard[BEDROCK] = -1.0F;
	g_hard[RS_PISTON_MOVING_ID] = -1.0F; // Beta BlockPistonMoving.setHardness(-1) -> canPushBlock(36)=false
	g_mob[DESTROY_ID] = RS_MOBILITY_DESTROY;
	g_mob[IMMOVABLE_ID] = RS_MOBILITY_IMMOVABLE;

	const int F = 5; // facing +X for every scenario (push column = +X)

	// --- 1. EXTEND pushes a 2-block column; the head fills the cell in front of the piston ----------
	clear();
	g_id[8][8][8] = RS_PISTON_BASE_ID;
	g_id[9][8][8] = WOOL; // pushed with nonzero color metadata
	g_meta[9][8][8] = 14;
	g_id[10][8][8] = STONE; // pushed
	// air at 11
	EXPECT(piston_extend(&W, 8, 8, 8, F, /*sticky*/ false) == true);
	// reverse walk emits far-cell-first: (11)<-stone, (10)<-stone, (9)<-HEAD(34)
	EXPECT(n_ops == 3);
	EXPECT(is_move(0, 11, 8, 8, STONE, F, /*ext*/ true, /*source*/ false));
	EXPECT(is_move(1, 10, 8, 8, WOOL, F, true, false));
	EXPECT(ops[1].b == 14);          // stored_meta preserves the wool color
	EXPECT(g_meta[10][8][8] == 14); // transient id-36 metadata mirrors stored_meta, not facing
	EXPECT(is_move(2, 9, 8, 8, RS_PISTON_HEAD_ID, F, true, /*source*/ false));

	// --- 2. EXTEND with a destroy-on-push TERMINAL: it is dropped+cleared, then the column shifts ---
	clear();
	g_id[8][8][8] = RS_PISTON_BASE_ID;
	g_id[9][8][8] = STONE;
	g_id[10][8][8] = STONE;
	g_id[11][8][8] = DESTROY_ID; // mobilityFlag 1 -> dropped, ends the chain (the cell it vacates is air)
	EXPECT(piston_extend(&W, 8, 8, 8, F, false) == true);
	// forward scan drops+clears (11) BEFORE the reverse walk; then (11)<-stone,(10)<-stone,(9)<-head
	EXPECT(n_ops == 5);
	EXPECT(is_drop(0, 11, 8, 8, DESTROY_ID));
	EXPECT(is_set(1, 11, 8, 8, 0)); // cleared to air (notify)
	EXPECT(ops[1].c == true);		// the clear NOTIFIES (Beta setBlockWithNotify)
	EXPECT(is_move(2, 11, 8, 8, STONE, F, true, false));
	EXPECT(is_move(3, 10, 8, 8, STONE, F, true, false));
	EXPECT(is_move(4, 9, 8, 8, RS_PISTON_HEAD_ID, F, true, false));

	// --- 3. EXTEND refused at the 12-block limit: a 13-movable column emits NOTHING, returns false --
	clear();
	g_id[8][8][8] = RS_PISTON_BASE_ID;
	for(int x = 9; x <= 21; x++) // 13 movable stone in a row (9..21)
		g_id[x][8][8] = STONE;
	EXPECT(piston_extend(&W, 8, 8, 8, F, false) == false);
	EXPECT(n_ops == 0); // the forward scan bails at n==12 before any write

	// exactly 12 movable -> legal, 12 shifts + the head = 13 moving-TEs
	clear();
	g_id[8][8][8] = RS_PISTON_BASE_ID;
	for(int x = 9; x <= 20; x++) // 12 movable stone (9..20), air at 21
		g_id[x][8][8] = STONE;
	EXPECT(piston_extend(&W, 8, 8, 8, F, false) == true);
	EXPECT(n_ops == 13);
	EXPECT(is_move(0, 21, 8, 8, STONE, F, true, false));	  // farthest stone shifts to 21
	EXPECT(is_move(11, 10, 8, 8, STONE, F, true, false));	  // nearest stone shifts to 10
	EXPECT(is_move(12, 9, 8, 8, RS_PISTON_HEAD_ID, F, true, false)); // head at 9; not source

	// --- 4. RETRACT (non-sticky): front head force-settled, base->moving-TE, head cell -> air --------
	clear();
	g_id[8][8][8] = RS_PISTON_BASE_ID;
	g_id[9][8][8] = RS_PISTON_HEAD_ID; // the extended head occupies the front cell
	piston_retract(&W, 8, 8, 8, F, /*sticky*/ false);
	EXPECT(n_ops == 3);
	EXPECT(is_settle(0, 9, 8, 8)); // clearPistonTileEntity on the front head (no-op if static, recorded)
	EXPECT(is_move(1, 8, 8, 8, RS_PISTON_BASE_ID, F, /*ext*/ false, /*source*/ true)); // base retracts
	EXPECT(is_set(2, 9, 8, 8, 0) && ops[2].c == true);							  // head cell -> air (notify)

	// --- 5. RETRACT (sticky) PULLS a movable block 2 ahead back into the head cell ------------------
	clear();
	g_id[8][8][8] = RS_PISTON_STICKY_ID;
	g_id[9][8][8] = RS_PISTON_HEAD_ID;
	g_id[10][8][8] = WOOL; // 2-ahead: a movable block -> pulled
	g_meta[10][8][8] = 3;
	piston_retract(&W, 8, 8, 8, F, /*sticky*/ true);
	EXPECT(n_ops == 4);
	EXPECT(is_settle(0, 9, 8, 8));
	EXPECT(is_move(1, 8, 8, 8, RS_PISTON_STICKY_ID, F, false, true)); // sticky base retracts
	EXPECT(is_set(2, 10, 8, 8, 0) && ops[2].c == true);				 // vacate the 2-ahead cell
	EXPECT(is_move(3, 9, 8, 8, WOOL, F, /*ext*/ false, /*source*/ false)); // wool pulled into head cell
	EXPECT(ops[3].b == 3);         // stored_meta preserves the pulled wool color
	EXPECT(g_meta[9][8][8] == 3); // transient id-36 metadata mirrors stored_meta, not facing

	// --- 6. RETRACT (sticky) with AIR 2 ahead: nothing pulled, head cell -> air ----------------------
	clear();
	g_id[8][8][8] = RS_PISTON_STICKY_ID;
	g_id[9][8][8] = RS_PISTON_HEAD_ID; // air at 10
	piston_retract(&W, 8, 8, 8, F, true);
	EXPECT(n_ops == 3);
	EXPECT(is_settle(0, 9, 8, 8));
	EXPECT(is_move(1, 8, 8, 8, RS_PISTON_STICKY_ID, F, false, true));
	EXPECT(is_set(2, 9, 8, 8, 0)); // the head cell -> air (the pull gate's aid<=0 branch)

	// --- 7. RETRACT (sticky) with an IMMOVABLE block 2 ahead: not pulled, head cell -> air -----------
	clear();
	g_id[8][8][8] = RS_PISTON_STICKY_ID;
	g_id[9][8][8] = RS_PISTON_HEAD_ID;
	g_id[10][8][8] = IMMOVABLE_ID; // mobilityFlag 2 -> the gate refuses the pull
	piston_retract(&W, 8, 8, 8, F, true);
	EXPECT(n_ops == 3);
	EXPECT(is_settle(0, 9, 8, 8));
	EXPECT(is_move(1, 8, 8, 8, RS_PISTON_STICKY_ID, F, false, true));
	EXPECT(is_set(2, 9, 8, 8, 0)); // immovable not pulled -> head removed

	// --- 8. STICKY retract var14 gate: a 2-ahead moving-TE that is EXTENDING + SAME facing is CONSUMED
	// (force-settled, NOT pulled, head NOT cleared) -- Beta's double-extender edge -----------------------
	clear();
	g_id[8][8][8] = RS_PISTON_STICKY_ID;
	g_id[9][8][8] = RS_PISTON_HEAD_ID;
	// a moving-TE 2-ahead, extending, facing == F (front): the var16.isExtending() && orientation==front gate
	g_id[10][8][8] = RS_PISTON_MOVING_ID;
	g_mte_facing[10][8][8] = F;
	g_mte_ext[10][8][8] = true;
	piston_retract(&W, 8, 8, 8, F, true);
	EXPECT(n_ops == 3);
	EXPECT(is_settle(0, 9, 8, 8));							 // force-settle the front head
	EXPECT(is_move(1, 8, 8, 8, RS_PISTON_STICKY_ID, F, false, true)); // base retracts
	EXPECT(is_settle(2, 10, 8, 8));							 // the 2-ahead moving-TE is CONSUMED (settled), no pull, no head-clear

	// --- 9. STICKY retract var14 gate: a 2-ahead moving-TE that is RETRACTING (extending=false) is NOT
	// consumed -> id-36 is unpushable (hardness -1) -> head cell cleared (the `if(!consumed)` path) -------
	clear();
	g_id[8][8][8] = RS_PISTON_STICKY_ID;
	g_id[9][8][8] = RS_PISTON_HEAD_ID;
	g_id[10][8][8] = RS_PISTON_MOVING_ID;
	g_mte_facing[10][8][8] = F;
	g_mte_ext[10][8][8] = false; // NOT extending -> the var14 gate fails -> not consumed
	piston_retract(&W, 8, 8, 8, F, true);
	EXPECT(n_ops == 3);
	EXPECT(is_settle(0, 9, 8, 8));
	EXPECT(is_move(1, 8, 8, 8, RS_PISTON_STICKY_ID, F, false, true));
	EXPECT(is_set(2, 9, 8, 8, 0)); // not consumed + id-36 unpushable -> head cell cleared (no 2-ahead settle)

	printf("test_piston_materialize_golden: OK (Beta tryExtend column shift + head, destroy terminal "
		   "drop, 12-limit refusal, retract base-retract + non-sticky head-clear + sticky pull/refuse + "
		   "the var14 same-facing-extending consume gate)\n");
	return 0;
}
