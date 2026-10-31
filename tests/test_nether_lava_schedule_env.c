/*
	Regression guard for the Nether DIM-1 disk-load path: lava random-tick lifecycle
	and repair of legacy non-zero skylight.

	SCHEDULING SCOPE: moving lava has BOTH an explicit scheduled-update path and
	Beta's Block.setTickOnLoad(true) random-tick path. A newly placed spring is
	scheduled by BlockFlowing.onBlockAdded; an already-existing moving cell loaded
	from disk is NOT blanket-scheduled by Beta, but is eligible for random ticks.
	The FALL/FLOW mechanic itself is pinned byte-for-byte by test_fluid_spread.

	WHAT THIS TEST DOES NOT PROVE: it does not exercise the distinct fresh-populate
	path that arms/generated springs. This fixture specifically guards disk reload:
	a loaded moving fluid must wait for a genuine saved/new schedule or a random tick,
	not receive CaveWii's former blanket synthetic schedule.

	It drives the REAL production load path (region_archive save -> server_world_create ->
	server_world_load_chunk -> deferred 3x3-complete schedule_chunk_ticks) for a WORLD_DIM_NETHER chunk and
	inspects the scheduled-tick queue directly:
	  - FLOWING lava (id 10): has onWorldTick + onRandomTick and BlockFluid's
	    onNeighbourBlockChange harden hook. It is NOT spuriously scheduled on load.
	  - STILL lava (id 11, BLOCK_LAVA_STILL): has onNeighbourBlockChange (re-wakes on a neighbour
	    change), NO onWorldTick -> it is correctly SKIPPED (the still lava SEA must not self-tick
	    every load; any later neighbour notification performs the quiet wake).
	FIRE (51) is the positive control (onWorldTick, no neighbour hook): it MUST be
	scheduled, proving schedule_chunk_ticks actually ran over the loaded chunk.

	The same cross-session fixture deliberately writes poisoned SkyLight=0xFF,
	then verifies the production loader zeros it, marks the chunk modified, and a
	real Nether block edit neither restores skylight nor hits the flood ceiling.
	Sibling of tests/test_redstone_repeater_reload_env.c, but on the DIM-1 path.
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
#include "lighting.h"
#include "network/client_interface.h"
#include "network/region_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "util.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE	 (CHUNK_SIZE * CHUNK_SIZE)
static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_nether_lava_schedule_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

// Chunk (33,5) lives in region (1,0) -- same not-region-0,0 chunk the sibling tests use.
#define TEST_CX 33
#define TEST_CZ 5

// S_CHUNK_IDX (server_world.c, private): idx = y + (localz + localx*CHUNK_SIZE) * WORLD_HEIGHT.
static size_t cell(int lx, int y, int lz) {
	return (size_t)y + ((size_t)lz + (size_t)lx * CHUNK_SIZE) * WORLD_HEIGHT;
}

// block local positions in the chunk (rest is air). Roof-height so it reads like a
// real Nether spring hanging under the bedrock ceiling.
#define FLOW_LX 5
#define FLOW_LZ 5
#define STILL_LX 8
#define STILL_LZ 8
#define FIRE_LX 2
#define FIRE_LZ 2
#define SPRING_Y 118

static void test_far_skin_includes_lava_sea(void) {
	struct server_world w = {0};
	w.dimension = WORLD_DIM_NETHER;
	struct server_chunk sc = {0};
	sc.ids = calloc(IDS_SIZE, 1);
	assert(sc.ids);

	// Synthetic Hell column: netherrack floor, transparent still-lava sea,
	// air cavern, then a realistic opaque roof shell. Beta worldgen leaves
	// netherrack directly below and between the randomized bedrock cap blocks;
	// the far extractor must peel that whole ceiling shell, not only bedrock.
	// The compact far skin must select the liquid surface, not the hidden floor.
	for(int x = 0; x < CHUNK_SIZE; x++)
		for(int z = 0; z < CHUNK_SIZE; z++) {
			for(int y = 0; y <= 20; y++)
				sc.ids[cell(x, y, z)] = BLOCK_NETHERRACK;
			for(int y = 21; y <= 31; y++)
				sc.ids[cell(x, y, z)] = BLOCK_LAVA_STILL;
			for(int y = 120; y < WORLD_HEIGHT; y++)
				sc.ids[cell(x, y, z)] = BLOCK_NETHERRACK;
			sc.ids[cell(x, 125, z)] = BLOCK_BEDROCK;
			sc.ids[cell(x, WORLD_HEIGHT - 1, z)] = BLOCK_BEDROCK;
		}
	// Flowing liquid is normalized to a static far tile. A nearby land column
	// proves lava buried below an opaque surface is not pulled through it.
	sc.ids[cell(0, 31, 0)] = BLOCK_LAVA_FLOW;
	sc.ids[cell(0, 32, 1)] = BLOCK_NETHERRACK;

	struct far_rpc_slot out = {0};
	server_world_extract_far_column(&w, &sc, 0, 0, &out);
	for(int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; i++) {
		if(i == 1) {
			assert(out.skin_y[i] == 32);
			assert(out.top_id[i] == BLOCK_NETHERRACK);
		} else {
			assert(out.skin_y[i] == 31);
			assert(out.top_id[i] == BLOCK_LAVA_STILL);
		}
	}

	free(sc.ids);
	puts("  far skin: still/flowing Nether lava survives compact extraction");
}

int main(void) {
	test_fixture_dir("test_nether_lava_schedule_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	blocks_init(); // real registry: moving lava is random-ticked; still lava wakes;
				   // fire is the scheduled-load positive control.
	assert(clin_init());
	test_far_skin_includes_lava_sea();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/DIM-1"));		  // Nether world folder
	MKDIR(test_fixture_join(TMP_WORLD, "/DIM-1/region")); // Nether region path (region_archive.c:144)
	remove(test_fixture_join(TMP_WORLD, "/DIM-1/region/r.1.0.mcr")); // clean slate

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	// ---- session 1: save moving lava + still-lava sea + one fire control ---------------------
	{
		struct region_archive ra;
		assert(region_archive_create_new(&ra, level, 1, 0, WORLD_DIM_NETHER));

		struct server_chunk sc = {0};
		sc.ids = calloc(IDS_SIZE, 1); // all air
		sc.metadata = calloc(NIB_SIZE, 1);
		sc.lighting_sky = malloc(NIB_SIZE);
		sc.lighting_torch = calloc(NIB_SIZE, 1);
		sc.heightmap = calloc(HM_SIZE, 1);
		assert(sc.ids && sc.metadata && sc.lighting_sky && sc.lighting_torch && sc.heightmap);
		/* R8 original-JAR plain setter witnesses can leave nonzero stored
		 * Nether Sky. CFR World.java:493-508 reads it; :1300-1301 suppresses
		 * Sky queues, not saved bytes. Load must preserve this complete plane. */
		memset(sc.lighting_sky, 0xFF, NIB_SIZE);

		sc.ids[cell(FLOW_LX, SPRING_Y, FLOW_LZ)] = BLOCK_LAVA_FLOW;	 // 10 (moving/random-ticked)
		sc.ids[cell(STILL_LX, SPRING_Y, STILL_LZ)] = BLOCK_LAVA_STILL; // 11 (still sea -> skipped)
		sc.ids[cell(FIRE_LX, SPRING_Y, FIRE_LZ)] = BLOCK_FIRE;		 // 51 (control, scheduled)
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

	// ---- session 2: complete the load neighbourhood; lava skips, fire does not ----------------
	{
		struct server_local s = {0};
		server_world_create(&s.world, level, WORLD_DIM_NETHER, 1337,
							false /*real hell generator*/);
		s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */

		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.world, TEST_CX, TEST_CZ, &sc) && sc);
		// sanity: the saved blocks survived the Nether-region round trip
		assert(sc->ids[cell(FLOW_LX, SPRING_Y, FLOW_LZ)] == BLOCK_LAVA_FLOW);
		assert(sc->ids[cell(STILL_LX, SPRING_Y, STILL_LZ)] == BLOCK_LAVA_STILL);
		assert(sc->ids[cell(FIRE_LX, SPRING_Y, FIRE_LZ)] == BLOCK_FIRE);
		assert(!sc->load_ticks_rearmed);
		assert(s.world.scheduled_tick_count == 0);
		for(size_t i = 0; i < NIB_SIZE; i++)
			assert(sc->lighting_sky[i] == 0xFF);
		assert(!sc->modified); // loading saved Sky does not rewrite the chunk

		/* A whole-chunk reconstruction scan is deferred until the union of all
		 * cell +/-8 boxes (the centred 3x3) is resident. Load it incrementally;
		 * the saved fire must not arm early or be lost at completion. */
		static const int neighbour_offset[8][2] = {
			{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
			{1, 0},   {-1, 1}, {0, 1},  {1, 1},
		};
		struct server_chunk* neighbour = NULL;
		for(size_t i = 0; i < 8; i++) {
			assert(server_world_load_chunk(
				&s.world, TEST_CX + neighbour_offset[i][0],
				TEST_CZ + neighbour_offset[i][1], &neighbour));
			if(i != 7)
				assert(s.world.scheduled_tick_count == 0);
		}
		sc = dict_server_chunks_get(
			s.world.chunks, S_CHUNK_ID(TEST_CX, TEST_CZ));
		assert(sc && sc->load_ticks_rearmed);

		// A metadata-API edit in the Nether chunk must preserve stored Sky and not
		// consume the pathological 500k-pop ceiling seen in the hardware proof.
		uint32_t ceiling_before = lighting_flood_ceiling_hits();
		assert(server_world_set_block(
			&s.worlds[0], TEST_CX * CHUNK_SIZE + 12, SPRING_Y, TEST_CZ * CHUNK_SIZE + 12,
			(struct block_data) {.type = BLOCK_STONE}));
		clin_drain_discard();
		assert(lighting_flood_ceiling_hits() == ceiling_before);
		for(size_t i = 0; i < NIB_SIZE; i++)
			assert(sc->lighting_sky[i] == 0xFF);

		// Drain every scheduled tick the chunk load armed and classify by block_type.
		int flow_ticks = 0, still_ticks = 0, fire_ticks = 0, other_ticks = 0;
		struct scheduled_tick t;
		while(server_world_scheduled_tick_pop(&s.world, &t)) {
			if(t.block_type == BLOCK_LAVA_FLOW)
				flow_ticks++;
			else if(t.block_type == BLOCK_LAVA_STILL)
				still_ticks++;
			else if(t.block_type == BLOCK_FIRE)
				fire_ticks++;
			else
				other_ticks++;
		}

		// Positive control: fire IS auto-scheduled -- proves the load scan ran.
		assert(fire_ticks == 1);
		// Existing moving lava is random-tick-enabled, not blanket-scheduled on load.
		assert(blocks[BLOCK_LAVA_FLOW]->onRandomTick != NULL);
		assert(flow_ticks == 0);
		// The still-lava sea is NOT self-scheduled on load; a real neighbour
		// notification performs BlockStationary's quiet wake instead.
		assert(still_ticks == 0);
		// nothing else in an otherwise-air chunk
		assert(other_ticks == 0);

		server_world_destroy(&s.world);
	}

	// ---- session 3: the edit persisted without erasing stored Sky ----
	{
		struct server_world w = {0};
		server_world_create(&w, level, WORLD_DIM_NETHER, 1337,
							false /*real hell generator*/);
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&w, TEST_CX, TEST_CZ, &sc) && sc);
		for(size_t i = 0; i < NIB_SIZE; i++)
			assert(sc->lighting_sky[i] == 0xFF);
		assert(!sc->modified);
		assert(sc->ids[cell(12, SPRING_Y, 12)] == BLOCK_STONE);
		server_world_destroy(&w);
	}

	string_clear(level);
	printf("test_nether_lava_schedule_env: OK (DIM-1 saved skylight retained, "
		   "edit bounded; moving lava random-ticked/not blanket-scheduled, "
		   "still sea skipped, fire control armed)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
