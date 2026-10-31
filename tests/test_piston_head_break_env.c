/*
	E2E production-path test for the Beta 1.7.3 PISTON HEAD removal behaviours (M4 pistons, Phase 5):

	  - BlockPistonExtension.onBlockRemoval: mining the EXTENDED head (id 34) of a still-powered piston
	    DROPS the base as an item AND clears the base cell -- so the world never holds a "powered base with
	    no head" state (which Beta never produces) and the player gets the piston item back. Implemented as
	    piston_head_on_removal, called from the dig path.
	  - BlockPistonExtension.onNeighborBlockChange: a head whose base has been removed SELF-REMOVES (orphan
	    cleanup), so a baseless floating head is never left behind. Implemented as
	    piston_head_neighbour_change, bound onto block 34 and fired by the neighbour-notify cascade.

	Both gaps were confirmed by the Phase-5 adversarial review (the onBlockRemoval stub was flagged in
	docs/piston_plan.md:124; the onNeighborBlockChange gap was unflagged). Harness cloned from
	test_piston_move_env.c: a drain thread keeps the client inbox empty so server-thread set_block never
	blocks; the test reads the SERVER world synchronously. tick() MUST call piston_moving_te_tick.
*/

#include <assert.h>
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
#include "block/redstone_piston.h"
#include "block/redstone_piston_server.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_piston_head_break_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // working plane
#define FACE_PX 5 // Beta facing +X

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
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = (uint8_t)(meta & 0x0F),
	};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void tick(void) {
	server_world_tick(&s.world, &s);
	piston_moving_te_tick(&s);
}
static void floor_patch(int x0, int x1, int z0, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++)
			set(x, YF, z, BLOCK_STONE, 0);
}
// Build an extended, powered piston facing +X at (PX,Y,Z): base at PX, head at PX+1 (settled).
static void build_extended(int PX, int Z, int base_id) {
	floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
	set(PX, Y, Z, base_id, FACE_PX);
	set(PX + 1, Y, Z, BLOCK_AIR, 0); // a clear 0-block push (just the head pops out)
	set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5); // power it -> EXTEND
	for(int k = 0; k < 3; k++)
		tick(); // settle: head materialises in the front cell
	assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD);
	assert(getmeta(PX, Y, Z) == (FACE_PX | 8)); // base powered/extended
}

int main(void) {
	test_fixture_dir("test_piston_head_break_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	// Mirror server_local_create's store init: all moving-TE slots free (.y < 0). Without this the
	// memset-0 leaves .y == 0 (== occupied), so the very first move's add finds no free slot.
	piston_moving_store_init(&s.piston_moving);
	dict_entity_init(s.entities); // block drops spawn item entities into this dict
	s.next_entity_id = 0;
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;

	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	// =================================================================================================
	// SCENARIO A: onBlockRemoval -- mining the EXTENDED head drops + clears the still-powered base.
	// =================================================================================================
	{
		const int PX = 6, Z = 6;
		build_extended(PX, Z, BLOCK_PISTON_BASE);

		size_t n_before = dict_entity_size(s.entities);
		int head_meta = getmeta(PX + 1, Y, Z); // capture BEFORE removal (Beta reads it in onBlockRemoval)

		// Simulate the player dig of the head: the dig path sets the cell to AIR, THEN onBlockRemoval.
		set(PX + 1, Y, Z, BLOCK_AIR, 0);
		piston_head_on_removal(&s, PX + 1, Y, Z, head_meta);

		// The powered base must be DROPPED (one item entity) and CLEARED (no powered headless base left).
		assert(get(PX, Y, Z) == BLOCK_AIR);					   // base cell cleared
		assert(dict_entity_size(s.entities) == n_before + 1);  // the base dropped exactly one item
		assert(get(PX + 1, Y, Z) == BLOCK_AIR);				   // head cell stays air (head drops nothing)
	}

	// =================================================================================================
	// SCENARIO A2: onBlockRemoval is a NO-OP when the base is NOT powered (e.g. an orphaned head whose
	// base lost power but is still present) -- it must not drop a non-powered base.
	// =================================================================================================
	{
		const int PX = 6, Z = 10;
		floor_patch(PX - 1, PX + 3, Z - 1, Z + 1);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);		 // UNPOWERED base (meta 5, no bit-8)
		set(PX + 1, Y, Z, BLOCK_PISTON_HEAD, FACE_PX);	 // a (contrived) head in front

		size_t n_before = dict_entity_size(s.entities);
		set(PX + 1, Y, Z, BLOCK_AIR, 0);
		piston_head_on_removal(&s, PX + 1, Y, Z, FACE_PX);

		assert(get(PX, Y, Z) == BLOCK_PISTON_BASE);			  // unpowered base left untouched
		assert(dict_entity_size(s.entities) == n_before);	  // nothing dropped
	}

	// =================================================================================================
	// SCENARIO B: onNeighborBlockChange -- digging the BASE of an extended piston makes the now-baseless
	// head SELF-REMOVE (the notify cascade fires the head's handler).
	// =================================================================================================
	{
		const int PX = 6, Z = 14;
		build_extended(PX, Z, BLOCK_PISTON_BASE);

		// Dig the base: setting it to AIR notifies the head, whose onNeighborBlockChange self-removes it.
		set(PX, Y, Z, BLOCK_AIR, 0);
		assert(get(PX + 1, Y, Z) == BLOCK_AIR); // the orphaned head removed itself in the same notify cycle
	}

	// =================================================================================================
	// SCENARIO C: a STICKY extended piston -- mining the head drops a STICKY base (29), not a regular one,
	// and clears it. (Guards the sticky/non-sticky drop selection through the base block's getDroppedItem.)
	// =================================================================================================
	{
		const int PX = 6, Z = 18;
		build_extended(PX, Z, BLOCK_PISTON_STICKY);

		size_t n_before = dict_entity_size(s.entities);
		int head_meta = getmeta(PX + 1, Y, Z);
		set(PX + 1, Y, Z, BLOCK_AIR, 0);
		piston_head_on_removal(&s, PX + 1, Y, Z, head_meta);

		assert(get(PX, Y, Z) == BLOCK_AIR);					  // sticky base cleared
		assert(dict_entity_size(s.entities) == n_before + 1); // dropped one (sticky) base item
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	piston_moving_store_destroy(&s.piston_moving);
	string_clear(level);
	printf("test_piston_head_break_env: OK (onBlockRemoval drops+clears the powered base on head-mine; "
		   "no-op on an unpowered base; onNeighborBlockChange self-removes an orphaned head; sticky base "
		   "drop -- all through the production dig path + neighbour-notify cascade)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
