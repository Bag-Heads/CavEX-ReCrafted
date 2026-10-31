/*
	FAITHFULNESS env test (Beta 1.7.3): BlockRail.onNeighborBlockChange's
	support-drop head, which was MISSING entirely -- a rail whose supporting
	block was removed (or, for an ascending slope, whose ascending-side block
	was removed) floated in place forever instead of dropping. This drives the
	REAL production path (server_world_set_block -> notifyNeighbours -> the
	rail's onNeighbourBlockChange) over a live superflat server_world, the
	same harness shape as test_ladder_pop.c / test_trapdoor_env.c.

	Citation [740c583 BlockRail.onNeighborBlockChange, ~86-104]:
	  shape = meta, or meta & 7 on a powered rail; // shape-only view
	  drop = no normal cube below (x, y-1, z);
	  shape 2 without a normal cube at (x+1,y,z) -> drop;
	  shape 3 without a normal cube at (x-1,y,z) -> drop;
	  shape 4 without a normal cube at (x,y,z-1) -> drop;
	  shape 5 without a normal cube at (x,y,z+1) -> drop;
	  on drop: dropBlockAsItem, then the cell is set to air with notify.
	i.e. always check below; an ASCENDING SLOPE (shape 2-5) ALSO checks the
	block on its ascending side (+X/-X/-Z/+Z respectively). Beta's if/else-if
	means a drop here skips the shape/power refresh entirely for that call.

	Three rigs: a straight rail (below-only), an ascending slope on a PLAIN
	rail (below + side, plus a wrong-side decoy negative control matching
	test_ladder_pop.c's rigor), and the SAME slope shape on a POWERED rail
	with its power bit (0x8) set -- proving rail_shape()'s isPowered mask
	(meta & 7) is applied before the shape comparison, not raw metadata.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rail_support_drop_env_tmp' (a literal) */
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
static void tick(void) { server_world_tick(&s.world, &s); }
static size_t entity_count(void) { return dict_entity_size(s.entities); }

int main(void) {
	test_fixture_dir("test_rail_support_drop_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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

	// NOTE ON RIG ISOLATION: the block's EXISTING (pre-this-fix) shape-refresh
	// half of onNeighbourBlockChange (calcRailMeta) re-derives a rail's shape
	// from real adjacent-rail track geometry on EVERY notify it receives. An
	// isolated slope rail with no real track around it (as these synthetic
	// rigs are) gets silently flattened back to a straight shape the FIRST
	// time anything notifies it -- unrelated to this fix, but it means each
	// slope rig below may receive AT MOST ONE notify before the assertion
	// that depends on its shape staying 2, or the earlier touch's own
	// (unrelated, pre-existing) shape recompute would invalidate the test.

	// --- Rig 1 (straight, meta 0, EW): below-only support. -------------------
	set(2, Y - 1, 2, BLOCK_STONE, 0); // support
	set(2, Y, 2, BLOCK_RAIL, 0);	  // plain rail, straight EW

	// --- Rig 2 (plain rail, ascending slope, meta 2 -> +X side): below AND
	// +X support. Receives exactly ONE notify below (the real removal).
	set(5, Y - 1, 2, BLOCK_STONE, 0); // below support
	set(6, Y, 2, BLOCK_STONE, 0);	  // +X support (the ascending side)
	set(5, Y, 2, BLOCK_RAIL, 2);	  // plain rail, ascending slope toward +X

	// --- Rig 2b (SAME shape, separate cell): below + +X support PLUS a -X
	// decoy (a REAL solid wall that is nobody's support for shape 2). Proves
	// the check is side-specific -- receives its one notify (the decoy
	// removal) and is never touched again.
	set(5, Y - 1, 5, BLOCK_STONE, 0); // below support
	set(6, Y, 5, BLOCK_STONE, 0);	  // +X support (the ascending side)
	set(4, Y, 5, BLOCK_STONE, 0);	  // -X decoy (irrelevant to shape 2)
	set(5, Y, 5, BLOCK_RAIL, 2);	  // plain rail, ascending slope toward +X

	// --- Rig 3 (POWERED rail, SAME slope shape 2, power bit 0x8 set): proves
	// rail_shape()'s isPowered mask (meta & 7) is used, not raw metadata (0xA
	// would not equal shape 2 unmasked). One notify (the real removal).
	set(5, Y - 1, 8, BLOCK_STONE, 0); // below support
	set(6, Y, 8, BLOCK_STONE, 0);	  // +X support
	set(5, Y, 8, BLOCK_POWERED_RAIL, 0x8 | 2); // powered, shape 2, power bit set

	for(int k = 0; k < 5; k++)
		tick();

	// 1) Baseline: all four rails placed and intact, correct metadata each.
	assert(get(2, Y, 2) == BLOCK_RAIL);
	assert(get(5, Y, 2) == BLOCK_RAIL);
	assert(get(5, Y, 5) == BLOCK_RAIL);
	assert(get(5, Y, 8) == BLOCK_POWERED_RAIL);
	size_t baseline_entities = entity_count();
	printf("  [1] baseline: 4 rails placed (straight, 2x plain slope, powered "
		   "slope)\n");

	// 2) Wrong-side decoy removal must NOT pop rig 2b (side-specific map).
	// This is rig 2b's ONLY notify -- it is not touched again below.
	set(4, Y, 5, BLOCK_AIR, 0); // rig2b decoy gone; its real +X support stands
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(5, Y, 5) == BLOCK_RAIL);
	assert(entity_count() == baseline_entities); // nothing dropped
	printf("  [2] wrong-side decoy removal leaves a supported ascending rail "
		   "untouched\n");

	// 3) Removing the CORRECT support pops each rig: block -> air, exactly
	// one item dropped each. Rig 1/2/3 each receive their FIRST and ONLY
	// notify here, so their placed shape is exactly what onNeighbourBlockChange
	// sees.
	set(2, Y - 1, 2, BLOCK_AIR, 0); // rig1 loses its below support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(2, Y, 2) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 1);

	set(6, Y, 2, BLOCK_AIR, 0); // rig2 loses its +X (ascending-side) support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(5, Y, 2) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 2);

	set(6, Y, 8, BLOCK_AIR, 0); // rig3 (powered) loses its +X support
	for(int k = 0; k < 5; k++)
		tick();
	assert(get(5, Y, 8) == BLOCK_AIR);
	assert(entity_count() == baseline_entities + 3);
	printf("  [3] support removal pops a straight rail (below-only), a plain "
		   "ascending slope (below+side), and a POWERED ascending slope (same "
		   "shape mask through the power bit) -- each to air, one item "
		   "dropped\n");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_rail_support_drop_env: OK (below-only + ascending-slope-side "
		   "support map incl. wrong-side negative control; powered-rail shape "
		   "mask honored)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
