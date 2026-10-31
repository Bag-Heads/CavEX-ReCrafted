/*
	Golden test for Beta 1.7.3 BlockFlower.canBlockStay self-removal, inherited
	UNCHANGED by BlockSapling (block_sapling.c did not implement it before this
	fix; block_flower.c/block_rose.c already did, per their own onRandomTick/
	onNeighbourBlockChange). A sapling must pop as a dropped item when either:
	  - its support (grass/dirt/farmland directly below) is gone, or
	  - it falls dark (this server's light model: max(sky,torch) < 8 -- see
	    block_flower.c's flower_can_block_stay comment for the documented
	    night-skylightSubtracted deviation from Beta's real getFullBlockLightValue),
	and must NOT pop while both hold -- checked on BOTH paths Beta wires it to:
	  - BlockSapling.updateTick's `super.updateTick(...)` call (the random tick), and
	  - BlockFlower.onNeighborBlockChange (a neighbour block changing, independent
	    of any random tick ever firing).

	Rig shape mirrors test_ladder_pop.c's proven support-drop harness (live
	superflat server_world, server_world_set_block to drive the REAL
	notifyNeighbours -> onNeighbourBlockChange path, dict_entity_size to count
	drops) and test_bonemeal.c's test_sapling_random_tick_cadence (calling
	block_sapling.onRandomTick directly on a fresh block_info snapshot).

	Decompile [740c583]: BlockFlower.canBlockStay/checkFlowerChange/
	onNeighborBlockChange, BlockSapling.updateTick (the super.updateTick call
	that runs BEFORE the grow-cadence roll).
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_sapling_canblockstay_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 101 // test plane; well above superflat terrain so only what we set() exists

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
static struct block_data at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b;
}
static int get(int x, int y, int z) { return at(x, y, z).type; }
static void tick(void) { server_world_tick(&s.world, &s); }
static size_t entity_count(void) { return dict_entity_size(s.entities); }

// Direct random-tick call on a fresh snapshot -- the block_info.block a real
// server_world_random_tick call would build (server_world.c's dispatcher
// hands onRandomTick a LOCAL `struct block_data` copy, not a live chunk
// pointer -- test_bonemeal.c's test_sapling_random_tick_cadence establishes
// this exact idiom).
static void random_tick_at(int x, int y, int z) {
	struct block_data blk = at(x, y, z);
	struct block_info info
		= {.block = &blk, .neighbours = NULL, .x = x, .y = y, .z = z};
	block_sapling.onRandomTick(&s, &info);
}

int main(void) {
	test_fixture_dir("test_sapling_canblockstay_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	rand_gen_seed(&s.rand_src); // the pop spawns a dropped-item entity (needs RNG + the dict)
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // x<=8, z<=8 all fit chunk (0,0)

	// ========================================================================
	// 1. Valid support + light: neither path pops it.
	// ========================================================================
	set(1, Y - 1, 1, BLOCK_DIRT, 0);
	set(1, Y, 1, BLOCK_SAPLING, 0);
	for(int k = 0; k < 3; k++)
		tick();
	assert(at(1, Y, 1).sky_light >= 8); // precondition: open sky, canBlockStay's light OK
	size_t baseline = entity_count();

	// 1a. Random tick: still a sapling, nothing dropped (whether or not the
	// grow-cadence roll also happened to fire is irrelevant to this claim --
	// it only ever flips the age metadata bit or replaces the block with a
	// tree/log, never with air, so `get(...) != BLOCK_AIR` alone would not be
	// a strong enough check; assert the type is UNCHANGED instead).
	random_tick_at(1, Y, 1);
	assert(get(1, Y, 1) == BLOCK_SAPLING);
	assert(entity_count() == baseline);

	// 1b. An UNRELATED neighbour change (a SIDE cell, not the support below)
	// must not pop it either -- canBlockStay only inspects the cell below and
	// the sapling's own light, exactly like test_ladder_pop.c's "wrong-side"
	// proof that the check is side-SPECIFIC, not "any neighbour changing".
	set(2, Y, 1, BLOCK_STONE, 0);
	set(2, Y, 1, BLOCK_AIR, 0);
	assert(get(1, Y, 1) == BLOCK_SAPLING);
	assert(entity_count() == baseline);

	printf("  valid support+light: random tick and unrelated neighbour change "
		   "both leave the sapling intact, nothing dropped OK\n");

	// ========================================================================
	// 2. Support removed: onNeighbourBlockChange pops it immediately -- no
	//    random tick involved at all.
	// ========================================================================
	set(3, Y - 1, 1, BLOCK_DIRT, 0);
	set(3, Y, 1, BLOCK_SAPLING, 0);
	for(int k = 0; k < 3; k++)
		tick();
	assert(at(3, Y, 1).sky_light >= 8); // light is fine -- ONLY soil is being pulled
	baseline = entity_count();

	set(3, Y - 1, 1, BLOCK_STONE, 0); // stone is not grass/dirt/farmland -> invalid soil
	assert(get(3, Y, 1) == BLOCK_AIR
		   && "losing support must pop the sapling via onNeighbourBlockChange, "
			  "with no random tick required");
	assert(entity_count() == baseline + 1 && "the pop must drop exactly one item");

	printf("  support removed (onNeighbourBlockChange): sapling -> air + 1 "
		   "dropped item, immediately on the notify, no random tick needed OK\n");

	// ========================================================================
	// 3. Darkness: a validly-SOILED sapling sealed in a light-tight box (no
	//    sky, no torch) pops on its random tick. onNeighbourBlockChange can't
	//    catch this case on its own (no adjacent BLOCK changed -- the sapling
	//    was built already dark), so this is the random-tick path's own proof.
	// ========================================================================
	set(5, Y - 1, 3, BLOCK_DIRT, 0); // valid soil
	// Seal a 1x1 box at (5,Y,3): solid walls on all four sides + a solid roof,
	// opaque dirt floor already below -- no sky, no torch source anywhere in
	// reach, so lighting settles to 0 inside.
	set(4, Y, 3, BLOCK_STONE, 0);
	set(6, Y, 3, BLOCK_STONE, 0);
	set(5, Y, 2, BLOCK_STONE, 0);
	set(5, Y, 4, BLOCK_STONE, 0);
	set(5, Y + 1, 3, BLOCK_STONE, 0); // roof
	set(5, Y, 3, BLOCK_SAPLING, 0);
	for(int k = 0; k < 3; k++)
		tick();
	struct block_data dark_cell = at(5, Y, 3);
	assert(dark_cell.sky_light < 8 && dark_cell.torch_light < 8
		   && "test setup must actually be dark -- sealed box precondition");
	baseline = entity_count();

	random_tick_at(5, Y, 3);
	assert(get(5, Y, 3) == BLOCK_AIR
		   && "darkness must pop the sapling on its random tick, support alone "
			  "is not enough");
	assert(entity_count() == baseline + 1 && "the pop must drop exactly one item");

	printf("  darkness (random tick, valid support): sapling -> air + 1 dropped "
		   "item OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("PASS test_sapling_canblockstay\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
