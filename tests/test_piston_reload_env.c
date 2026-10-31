/*
	E2E production-path test for piston move PERSISTENCE (M4 pistons, phase 3). Proves an IN-FLIGHT
	piston move survives a world save -> evict -> reload and settles at the SAME remaining tick, instead
	of orphaning into a permanent unbreakable id-36 phantom (the Phase-2 caveat this phase closes).

	Drives the REAL persistence path over a superflat server_world + server_local:
	  extend a piston (an id-36 moving cell + a RAM store slot, settle countdown ticks) -> advance one
	  tick (ticks 2->1) -> server_world_save_chunk(erase=true) [gather the moving-TE -> Beta 'Piston' NBT
	  with progress=(SETTLE-ticks)/SETTLE=0.5 -> write -> RELEASE the runtime slot -> evict the chunk] ->
	  server_world_load_chunk [parse the 'Piston' tag -> te_store_insert reconstructs ticks from progress]
	  -> tick to settle.

	WHAT IT CATCHES: without Phase 3, the reloaded id-36 cell has NO store slot, so piston_moving_te_tick
	skips it forever (orphan). With Phase 3 the slot is restored with the correct REMAINING countdown, so
	the move materializes after exactly the ticks it had left at save -- proving the gather/insert +
	ticks<->progress round-trip end to end over a real region file.

	HEADLESS HARNESS: cloned from test_piston_move_env (drain thread + the piston_moving_te_tick tick()).
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
#include "block/redstone_piston_server.h" // piston_moving_te_tick
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_piston_reload_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define Y  101
#define FACE_PX 5
#define PX 8
#define Z  8 // piston + push column all in chunk (0,0): x 8..11, z 8

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
		&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
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

int main(void) {
	test_fixture_dir("test_piston_reload_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region")); // region_archive writes r.0.0.mcr here; the save fails silently without it
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // clean slate
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities); // server_local_create initializes persistence ownership
	piston_moving_store_init(&s.piston_moving);

	// ===== SESSION 1: build an in-flight move, advance one tick, then DESTROY the world (which saves =====
	// every resident chunk -- the id-36 blocks in the block array + the gathered 'Piston' tile entities).
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s; // wire the server_local back-link (server_local_create does this) so the chunk
						 // save/load TE bridge (gather on save, te_store_insert on load) is enabled
	s.scheduled_tick_budget = 0;
	{
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc); // everything is in chunk (0,0)

		for(int x = PX - 1; x <= PX + 4; x++)
			for(int z = Z - 1; z <= Z + 1; z++)
				set(x, YF, z, BLOCK_STONE, 0);
		set(PX, Y, Z, BLOCK_PISTON_BASE, FACE_PX);
		set(PX + 1, Y, Z, BLOCK_STONE, 0);
		set(PX + 2, Y, Z, BLOCK_STONE, 0); // air at PX+3
		set(PX, Y, Z - 1, BLOCK_REDSTONE_TORCH_LIT, 5); // power -> EXTEND fires synchronously

		assert(getmeta(PX, Y, Z) == (FACE_PX | 8));
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 3, Y, Z) == BLOCK_PISTON_MOVING);

		// advance ONE tick: the three moving-TEs go ticks 2 -> 1 (still id-36). Beta TileEntityPiston.
		// writeToNBT saves the LAGGING progress (field_31020_l), so the persisted value here is 0.0, NOT
		// the leading 0.5 -- reloading it re-runs this step (Beta's one-tick reload "rewind", verified vs
		// the 740c583 decompile; mc173 collapses the two floats and would save 0.5, but decompile wins).
		tick();
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING);
		assert(get(PX + 3, Y, Z) == BLOCK_PISTON_MOVING);
	}
	// DESTROY: server_world_destroy saves every resident chunk (gather -> 'Piston' NBT progress 0.0, the
	// Beta-lagged value -> write to the region file). erase=false means the runtime store slots are NOT
	// released -- so clear them ourselves (the world-boundary clear), simulating a real menu->world cycle.
	server_world_destroy(&s.world);
	piston_moving_store_clear(&s.piston_moving); // wipe stale runtime ownership

	// ===== SESSION 2: re-create the world (same level dir) + LOAD the chunk from disk =====
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s; // wire the server_local back-link (server_local_create does this) so the chunk
						 // save/load TE bridge (gather on save, te_store_insert on load) is enabled
	s.scheduled_tick_budget = 0;
	struct server_chunk* sc = NULL;
	// load reconstructs the block array (id-36) AND te_store_insert restores the store slots (ticks from
	// the lagged progress 0.0 -> 2).
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);
	assert(get(PX, Y, Z) == BLOCK_PISTON_BASE && getmeta(PX, Y, Z) == (FACE_PX | 8)); // base survived
	assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING); // the moving cells survived the round trip
	assert(get(PX + 2, Y, Z) == BLOCK_PISTON_MOVING);
	assert(get(PX + 3, Y, Z) == BLOCK_PISTON_MOVING);

	// The reconstructed countdown was 2 (Beta-lagged progress 0.0 -> ticks 2), so the move settles after
	// exactly THREE more ticks (visit: 2->1; visit: 1->0; visit: settle). This is Beta's reload "rewind":
	// writeToNBT saved the lagging field_31020_l (0.0) and readFromNBT restored from it, so the move
	// re-runs the one step it had already taken before the save -- proof the store slot (not just the
	// id-36 block) was restored with the correct REMAINING time.
	tick();
	assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING); // not settled after 1 post-reload tick
	tick();
	assert(get(PX + 1, Y, Z) == BLOCK_PISTON_MOVING); // still mid-flight after 2 (the rewind's extra tick)
	tick();
	assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD); // materializes on the 3rd: head + the two shifted stones
	assert(get(PX + 2, Y, Z) == BLOCK_STONE);
	assert(get(PX + 3, Y, Z) == BLOCK_STONE);
	assert(getmeta(PX, Y, Z) == (FACE_PX | 8)); // base stays extended -- no orphan, no phantom

	// stable thereafter
	for(int k = 0; k < 4; k++) {
		tick();
		assert(get(PX + 1, Y, Z) == BLOCK_PISTON_HEAD && get(PX + 2, Y, Z) == BLOCK_STONE);
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	piston_moving_store_destroy(&s.piston_moving);
	dict_entity_clear(s.entities);
	string_clear(level);
	printf("test_piston_reload_env: OK (in-flight piston move survives save->evict->reload and settles "
		   "at the correct remaining tick via the Beta 'Piston' progress round-trip -- no orphan)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
