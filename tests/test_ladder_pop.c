/*
	FAITHFULNESS env test (Beta 1.7.3): BlockLadder.onNeighborBlockChange support-drop (FH-03).

	Before this test's production fix (source/block/block_ladder.c), block_ladder had NO
	.onNeighbourBlockChange entry: break the wall behind a ladder and it floated in place
	forever -- no drop, no removal. This drives the REAL production path
	(server_world_set_block -> notifyNeighbours -> the ladder's onNeighbourBlockChange ->
	support-drop) over a live superflat server_world, the same harness shape as
	test_redstone_torch_support_drop_env.c.

	Citation [740c583 BlockLadder.onNeighborBlockChange]:
	  m = the ladder's metadata; it stays when
	  one of these holds:
	  m == 2 and a normal cube at (x, y, z + 1),
	  m == 3 and a normal cube at (x, y, z - 1),
	  m == 4 and a normal cube at (x + 1, y, z),
	  m == 5 and a normal cube at (x - 1, y, z).
	  Otherwise:
	      it drops as an item (metadata m)
	      and the cell is set to air with notify.
	  (end)
	i.e. Beta raw metadata 2/3/4/5 = supporting neighbour at +Z/-Z/+X/-X, the SAME scheme
	block_ladder.c's onItemPlace already places with (SIDE_FRONT/BACK/LEFT/RIGHT -> 2/3/4/5).

	Four independent rigs, one per metadata value, each carrying its OWN wrong-side "decoy"
	wall (the correct support cell for one of the OTHER three metadata values) plus an
	unrelated vertical neighbour below the ladder. Per rig this proves, in order:
	  1) baseline: ladder placed, both decoy and correct support standing.
	  2) UNRELATED neighbour changes (the cell below the ladder, then the decoy wall on a
	     different cardinal side) do NOT pop the ladder -- the mapping is side-SPECIFIC, not
	     "any solid neighbour keeps it up".
	  3) removing the CORRECT support cell for that metadata pops the ladder: the block
	     becomes air AND a dropped ladder item entity appears (server_local_spawn_block_drops).
	Across the four rigs this also cross-checks the metadata->cell map is mutually exclusive:
	rig N's decoy is rig M's correct cell, so "wrong side ignored" is proven with a REAL
	support cell, not an arbitrary block.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_ladder_pop_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 101 // test plane; well above the superflat terrain so only what we set() exists

static struct server_local s;

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void tick(void) { server_world_tick(&s.world, &s); }
static size_t entity_count(void) { return dict_entity_size(s.entities); }

int main(void) {
	test_fixture_dir("test_ladder_pop_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src); // the support-drop spawns a dropped-item entity (needs RNG + the dict)
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // x<=8, z<=8 all fit chunk (0,0)

	// --- Rig layout (all x<=8, z<=8, single chunk) --------------------------------------
	// meta 2 (support +Z): ladder (2,Y,2); support (2,Y,3); decoy (2,Y,1) == rig3's support.
	// meta 3 (support -Z): ladder (6,Y,2); support (6,Y,1); decoy (6,Y,3) == rig2's support.
	// meta 4 (support +X): ladder (2,Y,6); support (3,Y,6); decoy (1,Y,6) == rig5's support.
	// meta 5 (support -X): ladder (6,Y,6); support (5,Y,6); decoy (7,Y,6) == rig4's support.
	set(2, Y, 3, BLOCK_STONE, 0); // rig2 support
	set(2, Y, 1, BLOCK_STONE, 0); // rig2 decoy == rig3's support
	set(2, Y, 2, BLOCK_LADDER, 2);

	set(6, Y, 1, BLOCK_STONE, 0); // rig3 support
	set(6, Y, 3, BLOCK_STONE, 0); // rig3 decoy == rig2's support
	set(6, Y, 2, BLOCK_LADDER, 3);

	set(3, Y, 6, BLOCK_STONE, 0); // rig4 support
	set(1, Y, 6, BLOCK_STONE, 0); // rig4 decoy == rig5's support
	set(2, Y, 6, BLOCK_LADDER, 4);

	set(5, Y, 6, BLOCK_STONE, 0); // rig5 support
	set(7, Y, 6, BLOCK_STONE, 0); // rig5 decoy == rig4's support
	set(6, Y, 6, BLOCK_LADDER, 5);

	for(int k = 0; k < 5; k++)
		tick();

	// 1) Baseline: all four ladders placed and intact, correct metadata each.
	assert(get(2, Y, 2) == BLOCK_LADDER && getmeta(2, Y, 2) == 2);
	assert(get(6, Y, 2) == BLOCK_LADDER && getmeta(6, Y, 2) == 3);
	assert(get(2, Y, 6) == BLOCK_LADDER && getmeta(2, Y, 6) == 4);
	assert(get(6, Y, 6) == BLOCK_LADDER && getmeta(6, Y, 6) == 5);
	size_t baseline_entities = entity_count();

	// 2a) UNRELATED vertical neighbour change (below each ladder) must NOT pop it: touches
	// a cell that is nobody's support cell for any of these four ladders.
	set(2, Y - 1, 2, BLOCK_STONE, 0);
	set(2, Y - 1, 2, BLOCK_AIR, 0);
	set(6, Y - 1, 2, BLOCK_STONE, 0);
	set(6, Y - 1, 2, BLOCK_AIR, 0);
	set(2, Y - 1, 6, BLOCK_STONE, 0);
	set(2, Y - 1, 6, BLOCK_AIR, 0);
	set(6, Y - 1, 6, BLOCK_STONE, 0);
	set(6, Y - 1, 6, BLOCK_AIR, 0);
	for(int k = 0; k < 5; k++)
		tick();

	assert(get(2, Y, 2) == BLOCK_LADDER && getmeta(2, Y, 2) == 2);
	assert(get(6, Y, 2) == BLOCK_LADDER && getmeta(6, Y, 2) == 3);
	assert(get(2, Y, 6) == BLOCK_LADDER && getmeta(2, Y, 6) == 4);
	assert(get(6, Y, 6) == BLOCK_LADDER && getmeta(6, Y, 6) == 5);
	assert(entity_count() == baseline_entities); // nothing dropped

	// 2b) WRONG-SIDE decoy removal must NOT pop it either -- each decoy IS a real solid wall
	// (another rig's correct support cell), so this proves the mapping is side-specific and
	// not "any adjacent normal cube keeps the ladder up".
	set(2, Y, 1, BLOCK_AIR, 0); // rig1 decoy gone (rig1 is meta 2, support is still (2,Y,3))
	set(6, Y, 3, BLOCK_AIR, 0); // rig2 decoy gone (rig2 is meta 3, support is still (6,Y,1))
	set(1, Y, 6, BLOCK_AIR, 0); // rig3 decoy gone (rig3 is meta 4, support is still (3,Y,6))
	set(7, Y, 6, BLOCK_AIR, 0); // rig4 decoy gone (rig4 is meta 5, support is still (5,Y,6))
	for(int k = 0; k < 5; k++)
		tick();

	assert(get(2, Y, 2) == BLOCK_LADDER && getmeta(2, Y, 2) == 2);
	assert(get(6, Y, 2) == BLOCK_LADDER && getmeta(6, Y, 2) == 3);
	assert(get(2, Y, 6) == BLOCK_LADDER && getmeta(2, Y, 6) == 4);
	assert(get(6, Y, 6) == BLOCK_LADDER && getmeta(6, Y, 6) == 5);
	assert(entity_count() == baseline_entities); // still nothing dropped

	// 3) Removing the CORRECT support cell pops each ladder: block -> air, item dropped.
	// One rig at a time, so the entity count strictly increases by exactly 1 per pop and we
	// can attribute each drop to its own rig (no aliasing between rigs).
	set(2, Y, 3, BLOCK_AIR, 0); // rig1 (meta 2) loses its +Z support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(2, Y, 2) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 1);

	set(6, Y, 1, BLOCK_AIR, 0); // rig2 (meta 3) loses its -Z support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(6, Y, 2) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 2);

	set(3, Y, 6, BLOCK_AIR, 0); // rig3 (meta 4) loses its +X support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(2, Y, 6) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 3);

	set(5, Y, 6, BLOCK_AIR, 0); // rig4 (meta 5) loses its -X support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(6, Y, 6) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 4);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_ladder_pop: OK (4/4 metadata orientations map to the correct support cell; "
		   "unrelated + wrong-side neighbour changes leave a supported ladder untouched; "
		   "support removal pops the ladder to air and drops exactly one item each)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
