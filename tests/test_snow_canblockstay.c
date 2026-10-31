/*
	Golden test for Beta 1.7.3 BlockSnow.canSnowStay support-loss removal
	(block_snow.c had NO .onNeighbourBlockChange at all before this fix -- a
	snow layer whose support block was destroyed just floated in mid-air
	forever; only the light-driven melt in onRandomTick, unrelated to support,
	could ever remove it).

	Rig shape mirrors test_sapling_canblockstay.c's proven support-drop harness
	(live superflat server_world, server_world_set_block to drive the REAL
	notifyNeighbours -> onNeighbourBlockChange path, dict_entity_size to count
	drops).

	Decompile [740c583]:
	    canPlaceBlockAt(world, x, y, z): with id = the block below (y - 1),
	        true only when id is non-air and its block isOpaqueCube(),
	        and then only when the material below
	            getIsSolid(); otherwise false.
	    }
	    onNeighborBlockChange: runs canSnowStay.
	    canSnowStay:
	        when canPlaceBlockAt fails,
	            dropBlockAsItem runs,
	            the cell is set to air with notify,
	            and the result is false;
	        otherwise true.
	    }
	    quantityDropped: 0.
	quantityDropped() returning 0 means dropBlockAsItem's own spawn loop runs
	ZERO iterations -- support-loss removal is SILENT (no snowball), exactly
	like the melt path in onRandomTick. Case 1 below asserts that silence
	directly: entity_count is unchanged across the support-removal pop.
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

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_snow_canblockstay_tmp' (a literal) */
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
static size_t entity_count(void) { return dict_entity_size(s.entities); }

int main(void) {
	test_fixture_dir("test_snow_canblockstay_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	rand_gen_seed(&s.rand_src); // a pop (if any) spawns a dropped-item entity (needs RNG + the dict)
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // x<=8, z<=8 all fit chunk (0,0)

	// ========================================================================
	// 1. Support removed: onNeighbourBlockChange pops the snow layer to AIR --
	//    and, per the decompile above (quantityDropped == 0), drops NOTHING.
	//    This is the exact bug report: a snow layer left floating in mid-air
	//    when the block beneath it is destroyed.
	// ========================================================================
	set(1, Y - 1, 1, BLOCK_STONE, 0);
	set(1, Y, 1, BLOCK_SNOW, 0);
	assert(get(1, Y, 1) == (int)BLOCK_SNOW); // precondition: placed and still there
	size_t baseline = entity_count();

	set(1, Y - 1, 1, BLOCK_AIR, 0); // destroy the support
	assert(get(1, Y, 1) == BLOCK_AIR
		   && "removing support must pop the snow layer via onNeighbourBlockChange, "
			  "not leave it floating");
	assert(entity_count() == baseline
		   && "support-loss removal is SILENT (quantityDropped==0) -- no snowball "
			  "entity, unlike a player-mined snow layer");

	printf("  support removed (onNeighbourBlockChange): snow -> air, no item "
		   "entity spawned (silent, quantityDropped==0) OK\n");

	// ========================================================================
	// 2. Valid support: an UNRELATED neighbour changing does not pop it --
	//    canPlaceBlockAt only inspects the cell directly below, exactly like
	//    test_sapling_canblockstay.c's "wrong-side" proof that the check is
	//    side-SPECIFIC, not "any neighbour changing".
	// ========================================================================
	set(3, Y - 1, 1, BLOCK_STONE, 0);
	set(3, Y, 1, BLOCK_SNOW, 0);
	assert(get(3, Y, 1) == (int)BLOCK_SNOW);
	baseline = entity_count();

	set(2, Y, 1, BLOCK_STONE, 0); // a SIDE cell, not the support below
	set(2, Y, 1, BLOCK_AIR, 0);
	assert(get(3, Y, 1) == (int)BLOCK_SNOW
		   && "an unrelated neighbour change must not pop a validly-supported "
			  "snow layer");
	assert(entity_count() == baseline);

	printf("  valid support + unrelated neighbour change: snow layer stays "
		   "put OK\n");

	// ========================================================================
	// 3. Non-opaque / non-solid support: snow cannot even be PLACED there
	//    (onItemPlace), and canSnowStay agrees a block already resting on one
	//    (constructed directly, bypassing placement) is not stayable.
	//    Beta: isOpaqueCube() must ALSO hold, not just a solid material --
	//    glass (opaque? no -- transparent) and a torch (thin, circuits
	//    material) both fail.
	// ========================================================================
	set(5, Y - 1, 3, BLOCK_GLASS, 0);
	assert(!block_snow.block_item.onItemPlace(&s, &s.player, NULL,
		&(struct block_info) {.x = 5, .y = Y, .z = 3},
		&(struct block_info) {.x = 5, .y = Y - 1, .z = 3}, SIDE_TOP)
		   && "onItemPlace must refuse to place snow on glass (not isOpaqueCube)");

	set(6, Y - 1, 3, BLOCK_TORCH, 0);
	assert(!block_snow.block_item.onItemPlace(&s, &s.player, NULL,
		&(struct block_info) {.x = 6, .y = Y, .z = 3},
		&(struct block_info) {.x = 6, .y = Y - 1, .z = 3}, SIDE_TOP)
		   && "onItemPlace must refuse to place snow on a torch (not solid, not "
			  "opaque)");

	// Force a snow layer onto the glass directly (bypassing placement, the
	// same way test_sapling_canblockstay builds its dark-box fixture) and
	// confirm ANY neighbour notify (here, a SIDE cell -- Beta's
	// notifyBlocksOfNeighborChange fires the hook for all 6 directions; only
	// canPlaceBlockAt's OWN check is below-specific, per case 2 above) makes
	// it re-evaluate its support and pop -- the exact floating-snow scenario
	// when support degrades to something non-opaque instead of air.
	set(5, Y, 3, BLOCK_SNOW, 0);
	assert(get(5, Y, 3) == (int)BLOCK_SNOW);
	baseline = entity_count();
	set(4, Y, 3, BLOCK_STONE, 0); // a SIDE cell of the snow at (5,Y,3)
	set(4, Y, 3, BLOCK_AIR, 0);   // notify fires on (5,Y,3); support is still glass
	assert(get(5, Y, 3) == BLOCK_AIR
		   && "a snow layer resting on non-opaque support must not stay once "
			  "any neighbour notify re-checks it");
	assert(entity_count() == baseline);

	printf("  non-opaque support (glass/torch): placement refused, and an "
		   "already-resting layer is not stayable OK\n");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("PASS test_snow_canblockstay\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
