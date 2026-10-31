/*
	E2E production-path regression guard for the chunk-load re-arm policy (M4 step 6, from the
	adversarial review of the repeater production wiring).

	THE BUG (critical, review-found): server_world_schedule_chunk_ticks re-arms EVERY onWorldTick
	block in a freshly loaded chunk at the blanket delay-1 -- the self-healing trick for IDEMPOTENT
	tickers (a torch recomputes lit/idle; fluids recompute flow). The repeater is NOT idempotent:
	redstone_repeater_tick's `else if(!active)` branch turns an idle, unpowered repeater ON. So before
	the fix, EVERY idle repeater emitted a spurious output pulse on every world reload (a wired
	lamp/door blinks; a clock gains a phantom edge Beta never emits, since Beta only ticks a block from
	a PERSISTED NextTickList entry, which CaveWii does not persist).

	THE FIRST FIX: server_world_schedule_chunk_ticks SKIPS blocks that define onNeighbourBlockChange (those
	self-schedule via their own neighbour-change path). This now covers TWO redstone components:
	  - the REPEATER (93/94): always had onNeighbourBlockChange (its metadata-varying delay).
	  - the REDSTONE TORCH (75/76): gained onNeighbourBlockChange when the BlockTorch support-drop
	    landed (block_torch.c) -- Beta BlockRedstoneTorch.onNeighborBlockChange does a SYNCHRONOUS
	    canBlockStay drop AND arms the inverter (updateTick, tickRate 2), so the C torch self-schedules
	    its inverter there too. It therefore correctly skips the non-Beta blanket re-arm, exactly like
	    the repeater. (Re-arming an idempotent torch on load was harmless; not re-arming it is MORE
	    Beta-faithful -- Beta relies on the authoritative saved block state, not a load-time re-tick.)

	THE LOAD-ORDER BUG: Beta gates both scheduleBlockUpdate and TickUpdates on the
	cell's +/-8 resident box. Scanning a lone newly inserted chunk through that
	checked API therefore schedules nothing, and the old loader never rescanned it
	after neighbouring chunks arrived. Scheduled-only blocks such as fire stayed
	inert forever. The loader now waits for the complete 3x3 union around a chunk,
	scans once per neighbourhood-residency epoch, and invalidates affected epochs
	when any member unloads.

	This test drives the REAL production load path (region_archive save -> server_world_create ->
	server_world_load_chunk -> deferred server_world_schedule_chunk_ticks) and inspects the scheduled-tick queue
	directly -- no set_block / client RPC / tick driving needed. FIRE is the positive control:
	Beta marks it setTickOnLoad(true), and CaveWii gives it onWorldTick with no neighbour
	hook, so it MUST be scheduled -- proving
	schedule_chunk_ticks actually ran and the repeater/torch absences are deliberate skips, not a load
	that silently did nothing. TNT is deliberately not the control: Beta's placed block has no
	updateTick, while an ignited TNT is an NBT-persisted EntityTNTPrimed with its own Fuse.
	Modelled on test_region_boundary's cross-session harness.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "network/region_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "util.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE	 (CHUNK_SIZE * CHUNK_SIZE)
static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_redstone_repeater_reload_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

// Chunk (33,5) lives in region (1,0) -- the same not-region-0,0 chunk the boundary test uses.
#define TEST_CX 33
#define TEST_CZ 5

// S_CHUNK_IDX (server_world.c, private): idx = y + (localz + localx*CHUNK_SIZE) * WORLD_HEIGHT.
static size_t cell(int lx, int y, int lz) {
	return (size_t)y + ((size_t)lz + (size_t)lx * CHUNK_SIZE) * WORLD_HEIGHT;
}

// repeater + torch + fire local positions in the chunk (rest is air).
#define REP_LX 5
#define REP_LZ 5
#define TORCH_LX 8
#define TORCH_LZ 8
#define FIRE_LX 2
#define FIRE_LZ 2
#define FIRE2_LX 14
#define FIRE2_LZ 14
#define CIRCUIT_Y 70

struct tick_counts {
	int torch;
	int repeater;
	int fire;
	int other;
};

static struct tick_counts drain_ticks(struct server_world* w) {
	struct tick_counts counts = {0};
	struct scheduled_tick t;
	while(server_world_scheduled_tick_pop(w, &t)) {
		if(t.block_type == BLOCK_REDSTONE_TORCH_LIT
		   || t.block_type == BLOCK_REDSTONE_TORCH)
			counts.torch++;
		else if(t.block_type == BLOCK_REDSTONE_REPEATER
				|| t.block_type == BLOCK_REDSTONE_REPEATER_LIT)
			counts.repeater++;
		else if(t.block_type == BLOCK_FIRE)
			counts.fire++;
		else
			counts.other++;
	}
	return counts;
}

static void assert_only_one_fire(struct tick_counts counts) {
	assert(counts.fire == 1);
	assert(counts.repeater == 0);
	assert(counts.torch == 0);
	assert(counts.other == 0);
}

int main(void) {
	test_fixture_dir("test_redstone_repeater_reload_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	blocks_init(); // real registry: repeater + lit torch have
				   // onNeighbourBlockChange; fire is scheduled-only here.

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.1.0.mcr")); // clean slate

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	// ---- session 1: save a chunk holding one idle repeater + one lit torch + one fire ------------
	{
		struct region_archive ra;
		assert(region_archive_create_new(&ra, level, 1, 0, WORLD_DIM_OVERWORLD));

		struct server_chunk sc = {0};
		sc.ids = calloc(IDS_SIZE, 1); // all air
		sc.metadata = calloc(NIB_SIZE, 1);
		sc.lighting_sky = calloc(NIB_SIZE, 1);
		sc.lighting_torch = calloc(NIB_SIZE, 1);
		sc.heightmap = calloc(HM_SIZE, 1);
		assert(sc.ids && sc.metadata && sc.lighting_sky && sc.lighting_torch && sc.heightmap);

		sc.ids[cell(REP_LX, CIRCUIT_Y, REP_LZ)] = BLOCK_REDSTONE_REPEATER;	   // 93 (idle, skipped)
		sc.ids[cell(TORCH_LX, CIRCUIT_Y, TORCH_LZ)] = BLOCK_REDSTONE_TORCH_LIT; // 76 (now skipped too)
		sc.ids[cell(FIRE_LX, CIRCUIT_Y, FIRE_LZ)] = BLOCK_FIRE; // scheduled control
		sc.modified = true;
		sc.populated = true; // a populated chunk LOADS (does not regenerate)

		assert(region_archive_set_blocks(&ra, TEST_CX, TEST_CZ, &sc, NULL));
		region_archive_destroy(&ra);
		free(sc.ids);
		free(sc.metadata);
		free(sc.lighting_sky);
		free(sc.lighting_torch);
		free(sc.heightmap);
	}

	// ---- session 2: incremental neighbourhood load defers, then re-arms exactly once --------------
	{
		struct server_world w = {0};
		server_world_create(&w, level, WORLD_DIM_OVERWORLD, 1337, true /*superflat generator*/);

		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&w, TEST_CX, TEST_CZ, &sc) && sc);
		// sanity: the saved circuit blocks survived the round trip
		assert(sc->ids[cell(REP_LX, CIRCUIT_Y, REP_LZ)] == BLOCK_REDSTONE_REPEATER);
		assert(sc->ids[cell(TORCH_LX, CIRCUIT_Y, TORCH_LZ)] == BLOCK_REDSTONE_TORCH_LIT);
		assert(sc->ids[cell(FIRE_LX, CIRCUIT_Y, FIRE_LZ)] == BLOCK_FIRE);
		assert(!sc->load_ticks_rearmed);
		assert(w.scheduled_tick_count == 0);

		/* A whole-chunk scan needs the union of every cell's +/-8 box: all
		 * eight neighbours. Loading any strict subset must neither schedule
		 * early nor consume the sole chance to re-arm the saved fire. */
		static const int neighbour_offset[8][2] = {
			{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
			{1, 0},   {-1, 1}, {0, 1},  {1, 1},
		};
		struct server_chunk* neighbour = NULL;
		for(size_t i = 0; i < 8; i++) {
			/* Make the completion edge encounter a transiently full ring. The
			 * load scan must stay pending, then retry deterministically after a
			 * later drain instead of recording a false completion. Raw insertion
			 * here is queue-capacity fault injection, never production scheduling. */
			if(i == 7) {
				server_world_scheduled_ticks_destroy(&w);
				server_world_scheduled_ticks_create(&w, 1);
				assert(server_world_schedule_tick_raw(
					&w, 0, 0, 0, BLOCK_STONE, 1000));
			}
			assert(server_world_load_chunk(
				&w, TEST_CX + neighbour_offset[i][0],
				TEST_CZ + neighbour_offset[i][1], &neighbour));
			assert(neighbour);
			if(i != 7) {
				sc = dict_server_chunks_get(
					w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
				assert(sc && !sc->load_ticks_rearmed);
				assert(w.scheduled_tick_count == 0);
			}
		}
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && !sc->load_ticks_rearmed);
		assert(sc->load_tick_rearm_cursor
			   == (uint32_t)cell(FIRE_LX, CIRCUIT_Y, FIRE_LZ));
		assert(w.scheduled_tick_count == 1);
		struct scheduled_tick capacity_blocker;
		assert(server_world_scheduled_tick_pop(&w, &capacity_blocker));
		assert(capacity_blocker.block_type == BLOCK_STONE);
		struct server_local tick_owner = {0};
		server_world_tick(&w, &tick_owner);
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && sc->load_ticks_rearmed);
		assert(sc->load_tick_rearm_cursor == 0);
		assert(w.scheduled_tick_count == 1);

		/* Positive control plus the redstone exclusions: the delayed scan ran,
		 * but only the scheduled-only fire was armed. */
		assert_only_one_fire(drain_ticks(&w));

		/* Losing one neighbourhood member starts a new readiness epoch. Once it
		 * returns, the still-resident target must be rescanned (no permanently
		 * lost callback). Repeating that transition while the callback is already
		 * pending must keep exactly one entry via Beta keep-first dedup. */
		const int last_cx = TEST_CX + neighbour_offset[7][0];
		const int last_cz = TEST_CZ + neighbour_offset[7][1];
		assert(server_world_save_chunk(&w, true, last_cx, last_cz));
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && !sc->load_ticks_rearmed);
		assert(server_world_load_chunk(&w, last_cx, last_cz, &neighbour));
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && sc->load_ticks_rearmed);
		assert(w.scheduled_tick_count == 1);

		assert(server_world_save_chunk(&w, true, last_cx, last_cz));
		assert(server_world_load_chunk(&w, last_cx, last_cz, &neighbour));
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && sc->load_ticks_rearmed);
		assert(w.scheduled_tick_count == 1);
		assert_only_one_fire(drain_ticks(&w));

		/* High-occupancy proof for the cursor + reconstruction-only always-dedup
		 * path. Seed a >256-entry full ring containing the first fire's old tick,
		 * then add a later fire to the chunk. A restart-from-zero scan using the
		 * normal queue policy would append/fail on the prefix forever as one slot
		 * frees at a time. The load cursor must pass the old duplicate, stop at
		 * FIRE2, then admit exactly FIRE2 after one dummy drains. */
		sc->ids[cell(FIRE2_LX, CIRCUIT_Y, FIRE2_LZ)] = BLOCK_FIRE;
		sc->modified = true;
		server_world_scheduled_ticks_destroy(&w);
		server_world_scheduled_ticks_create(&w, 301);
		for(int i = 0; i < 300; i++)
			assert(server_world_schedule_tick_raw(
				&w, 100000 + i, 0, 0, BLOCK_STONE, 1000));
		assert(server_world_schedule_tick_raw(
			&w, TEST_CX * CHUNK_SIZE + FIRE_LX, CIRCUIT_Y,
			TEST_CZ * CHUNK_SIZE + FIRE_LZ, BLOCK_FIRE, 1000));
		assert(w.scheduled_tick_count == 301);

		assert(server_world_save_chunk(&w, true, last_cx, last_cz));
		assert(server_world_load_chunk(&w, last_cx, last_cz, &neighbour));
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && !sc->load_ticks_rearmed);
		assert(sc->load_tick_rearm_cursor
			   == (uint32_t)cell(FIRE2_LX, CIRCUIT_Y, FIRE2_LZ));
		assert(w.scheduled_tick_count == 301);

		struct scheduled_tick drained_dummy;
		assert(server_world_scheduled_tick_pop(&w, &drained_dummy));
		assert(drained_dummy.block_type == BLOCK_STONE);
		server_world_tick(&w, &tick_owner);
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && sc->load_ticks_rearmed);
		assert(sc->load_tick_rearm_cursor == 0);
		assert(w.scheduled_tick_count == 301);
		struct tick_counts crowded = drain_ticks(&w);
		assert(crowded.fire == 2);
		assert(crowded.repeater == 0);
		assert(crowded.torch == 0);
		assert(crowded.other == 299);

		server_world_destroy(&w);
	}

	string_clear(level);
	printf("test_redstone_repeater_reload_env: OK (incremental 3x3 completion and >256-entry cursor "
		   "retry have no early/lost/double re-arm; repeater/torch still skip fire controls)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
