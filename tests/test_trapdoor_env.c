/*
	FAITHFULNESS env test (Beta 1.7.3): BlockTrapDoor.onNeighborBlockChange
	(support-drop + redstone open/close).

	Before this test's production fix (source/block/block_trapdoor.c), the
	handler started `if (!info->neighbours) return;` -- but EVERY dispatch site
	(redstone_server.c:121, blocks.c:392/398, server_world.c:964/971) constructs
	block_info with `.neighbours = NULL`, so that guard fired unconditionally and
	the whole function was dead: a powered trapdoor never opened/closed on its
	own, and an unsupported trapdoor floated forever. This drives the REAL
	production path (server_world_set_block -> notifyNeighbours -> the
	trapdoor's onNeighbourBlockChange) over a live superflat server_world, the
	same harness shape as test_ladder_pop.c / test_dispenser_env.c.

	Citation [740c583 BlockTrapDoor.onNeighborBlockChange]:
	  m = getBlockMetadata(x,y,z);
	  the support cell starts at (x, z);
	  (m&3)==0 -> its z = z+1;
	  (m&3)==1 -> its z = z-1;
	  (m&3)==2 -> its x = x+1;
	  (m&3)==3 -> its x = x-1;
	  no normal cube at the support cell -> set to air with notify, then dropBlockAsItem;
	  a non-air neighbour id whose block can provide power ->
	      onPoweredBlockChange(x,y,z, isBlockIndirectlyGettingPowered(x,y,z));
	i.e. Beta's facing metadata 0/1/2/3 = supporting wall at +Z/-Z/+X/-X -- the
	SAME scheme this file's own getBoundingBox open-panel translations use.

	Two rigs prove the support-drop mapping (meta 0 -> +Z wall, meta 2 -> +X
	wall), each with an unrelated-neighbour negative control (a decoy wall on
	the WRONG side must not pop it -- the mapping is side-specific, matching
	test_ladder_pop.c's rigor for the identical kind of fix). A third rig proves
	the redstone open/close round-trip via a real lever, toggled through the
	production block_lever.onRightClick path exactly as test_dispenser_env.c's
	redstone rig does.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_trapdoor_env_tmp' (a literal) */
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

// Toggle a floor lever at (x,y,z) via the REAL production activation path
// (matching test_dispenser_env.c's redstone rig), returning its new ON state.
static void lever_toggle(int x, int y, int z) {
	struct block_data lb;
	assert(server_world_get_block(&s.world, x, y, z, &lb));
	struct block_info li = {.block = &lb, .x = x, .y = y, .z = z};
	block_lever.onRightClick(&s, &s.player, NULL, &li, &li, SIDE_TOP);
}

int main(void) {
	test_fixture_dir("test_trapdoor_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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

	// ------------------------------------------------------------------
	// Rig 1 (meta 0, support +Z): support-drop + wrong-side decoy control.
	// ------------------------------------------------------------------
	set(2, Y, 3, BLOCK_STONE, 0);	 // rig1 support (+Z of the trapdoor)
	set(1, Y, 2, BLOCK_STONE, 0);	 // rig1 decoy (-X -- would be rig2's support)
	set(2, Y, 2, BLOCK_TRAP_DOOR, 0); // closed, facing meta 0

	// ------------------------------------------------------------------
	// Rig 2 (meta 2, support +X): support-drop, second axis.
	// ------------------------------------------------------------------
	set(7, Y, 2, BLOCK_STONE, 0);	 // rig2 support (+X of the trapdoor)
	set(6, Y, 2, BLOCK_TRAP_DOOR, 2); // closed, facing meta 2

	// ------------------------------------------------------------------
	// Rig 3 (meta 0, support +Z): redstone open/close via a real lever.
	// A FLOOR lever sits directly below the trapdoor (2,Y-1,6), mounted on
	// stone at (2,Y-2,6) -- that puts the lever exactly in the trapdoor's own
	// side-0 (-Y) cell, one of the 6 sides World.isBlockIndirectlyGettingPowered
	// scans for the trapdoor's own (x,y,z).
	// ------------------------------------------------------------------
	set(2, Y - 2, 6, BLOCK_STONE, 0); // mount for the floor lever
	set(2, Y - 1, 6, BLOCK_LEVER, 5); // floor lever, OFF (meta 5 = floor-mounted)
	set(2, Y, 7, BLOCK_STONE, 0);	  // rig3 support (+Z of the trapdoor)
	set(2, Y, 6, BLOCK_TRAP_DOOR, 0);  // closed, facing meta 0

	for(int k = 0; k < 5; k++)
		tick();

	// --- 1) Baseline: all three trapdoors placed, correct facing, closed. ---
	assert(get(2, Y, 2) == BLOCK_TRAP_DOOR && getmeta(2, Y, 2) == 0);
	assert(get(6, Y, 2) == BLOCK_TRAP_DOOR && getmeta(6, Y, 2) == 2);
	assert(get(2, Y, 6) == BLOCK_TRAP_DOOR && (getmeta(2, Y, 6) & 0x04) == 0);
	size_t baseline_entities = entity_count();
	printf("  [1] baseline: 3 trapdoors placed, closed, correct facing meta\n");

	// --- 2) Wrong-side decoy removal must NOT pop rig 1 (side-specific map). ---
	set(1, Y, 2, BLOCK_AIR, 0); // rig1 decoy gone; rig1's real support (2,Y,3) stands
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(2, Y, 2) == BLOCK_TRAP_DOOR && getmeta(2, Y, 2) == 0);
	assert(entity_count() == baseline_entities); // nothing dropped
	printf("  [2] wrong-side decoy removal leaves a supported trapdoor untouched\n");

	// --- 3) Redstone: lever ON -> trapdoor opens (bit 0x04 set); lever OFF ->
	// it closes again. Round-trip, driven through the real onRightClick path.
	lever_toggle(2, Y - 1, 6);
	assert((getmeta(2, Y - 1, 6) & 0x08) != 0); // lever really ON
	for(int k = 0; k < 5; k++)
		tick();
	assert((getmeta(2, Y, 6) & 0x04) != 0
		   && "powered trapdoor must open (BlockTrapDoor.onPoweredBlockChange)");
	assert((getmeta(2, Y, 6) & 0x03) == 0 && "facing bits must survive the toggle");

	lever_toggle(2, Y - 1, 6);
	assert((getmeta(2, Y - 1, 6) & 0x08) == 0); // lever really OFF
	for(int k = 0; k < 5; k++)
		tick();
	assert((getmeta(2, Y, 6) & 0x04) == 0
		   && "depowered trapdoor must close again");
	printf("  [3] redstone open/close round-trip via a real lever OK\n");

	// --- 4) Removing the CORRECT support pops rig 1 and rig 2: block -> air,
	// exactly one item dropped each.
	set(2, Y, 3, BLOCK_AIR, 0); // rig1 loses its +Z support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(2, Y, 2) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 1);

	set(7, Y, 2, BLOCK_AIR, 0); // rig2 loses its +X support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(6, Y, 2) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 2);
	printf("  [4] support removal pops both facings (meta 0 -> +Z, meta 2 -> "
		   "+X) to air, dropping exactly one item each\n");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_trapdoor_env: OK (support-drop mapping incl. wrong-side "
		   "negative control; redstone open/close round-trip via a real "
		   "lever)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
