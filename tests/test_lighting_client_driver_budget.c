/*
	Production-driver guard for the client relight frame budget.

	test_lighting_client_budget_equiv.c proves the resumable flood core is byte-exact
	against the old unbudgeted flood. This test drives world_update_lighting itself, so
	the production loop is pinned too: stale deferred entries charge one unit of work,
	active floods are paced through the real client world/chunk path, and no call may
	exceed LIGHTING_CLIENT_POPS_PER_FRAME.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "config.h"
#include "game/game_state.h"
#include "lighting.h"
#include "platform/thread.h"
#include "world.h"
#include "settle_observer.h"
#include "support/lighting_client_golden.h"

static void assert_last_frame_bounded(void) {
	assert(world_lighting_driver_last_work() <= LIGHTING_CLIENT_POPS_PER_FRAME);
}

static void test_stale_backlog_is_budgeted(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;
	world_lighting_driver_diag_reset();

	const size_t total = LIGHTING_CLIENT_POPS_PER_FRAME * 2u + 17u;
	for(size_t i = 0; i < total; i++) {
		struct world_modification_entry e = {
			.x = (w_coord_t)(8192 + (int)i),
			.y = 64,
			.z = -8192,
			.blk = {.type = BLOCK_AIR},
		};
		stack_push(&w.lighting_updates, &e);
	}
	assert(stack_size(&w.lighting_updates) == total);

	world_update_lighting(&w);
	assert_last_frame_bounded();
	assert(world_lighting_driver_last_work() == LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(world_lighting_driver_last_stale_drops()
		   == LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(stack_size(&w.lighting_updates) == total - LIGHTING_CLIENT_POPS_PER_FRAME);

	world_update_lighting(&w);
	assert_last_frame_bounded();
	assert(world_lighting_driver_last_work() == LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(world_lighting_driver_last_stale_drops()
		   == LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(stack_size(&w.lighting_updates) == 17u);

	world_update_lighting(&w);
	assert_last_frame_bounded();
	assert(world_lighting_driver_last_work() == 17u);
	assert(world_lighting_driver_last_stale_drops() == 17u);
	assert(stack_empty(&w.lighting_updates));

	world_update_lighting(&w);
	assert(world_lighting_driver_last_work() == 0u);
	assert(world_lighting_driver_last_stale_drops() == 0u);
	assert(world_lighting_driver_max_work() == LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(world_lighting_driver_max_stale_drops()
		   == LIGHTING_CLIENT_POPS_PER_FRAME);

	world_destroy(&w);
	printf("  stale backlog: %zu entries drained across 3 bounded frames\n", total);
}

static void build_zero_light_air_section(struct world* w) {
	const size_t cells = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = calloc(cells, sizeof(*ids));
	uint8_t* metadata = calloc(cells / 2u, sizeof(*metadata));
	uint8_t* sky = calloc(cells / 2u, sizeof(*sky));
	uint8_t* torch = calloc(cells / 2u, sizeof(*torch));
	uint8_t* heightmap = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*heightmap));
	uint8_t* grass_i = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*grass_i));
	uint8_t* grass_j = calloc((size_t)CHUNK_SIZE * CHUNK_SIZE, sizeof(*grass_j));
	assert(ids && metadata && sky && torch && heightmap && grass_i && grass_j);

	world_build_full_section(w, 0, 0, ids, metadata, sky, torch, heightmap, grass_i,
							 grass_j);

	free(grass_j);
	free(grass_i);
	free(heightmap);
	free(torch);
	free(sky);
	free(metadata);
	free(ids);
}

static void test_active_flood_is_budgeted(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;
	world_lighting_driver_diag_reset();
	build_zero_light_air_section(&w);

	// The section is all air with heightmap 0 and stored skylight 0. One real deferred
	// client relight therefore expands into a large sky flood through the production
	// world/chunk callbacks.
	world_set_block(&w, 8, 64, 8,
					(struct block_data) {
						.type = BLOCK_AIR,
						.metadata = 0,
						.sky_light = 0,
						.torch_light = 0,
					},
					true);
	assert(stack_size(&w.lighting_updates) == 1u);

	size_t frames = 0;
	size_t frames_at_cap = 0;
	size_t total_work = 0;
	while(!stack_empty(&w.lighting_updates)
		  || !lighting_flood_queue_empty(&w.lighting_flood_queue)
		  || world_light_repair_pending(&w)) {
		world_update_lighting(&w);
		assert_last_frame_bounded();
		if(world_lighting_driver_last_work() == LIGHTING_CLIENT_POPS_PER_FRAME)
			frames_at_cap++;
		total_work += world_lighting_driver_last_work();
		assert(++frames < 256u && "active relight did not drain");
	}

	assert(frames > 1u && "fixture did not exercise a multi-frame active flood");
	assert(frames_at_cap > 0u && "fixture never reached the production frame cap");
	assert(world_lighting_driver_max_work() == LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(world_lighting_driver_max_stale_drops() == 0u);
	assert(total_work > LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(world_lighting_driver_last_work() < LIGHTING_CLIENT_POPS_PER_FRAME);

	world_destroy(&w);
	printf("  active flood: %zu pops across %zu bounded frames (%zu frames at cap)\n",
		   total_work, frames, frames_at_cap);
}

static void test_nether_far_section_has_no_skylight(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_NETHER;
	// Beta WorldProviderHell.registerWorldChunkManager sets hasNoSky=true.
	// Dimensionless missing-world fallbacks must not synthesize sky=15 either.
	assert(world_get_block(&w, 4096, WORLD_HEIGHT, 4096).sky_light == 0);

	uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t top_id[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE] = {0};
	uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE] = {0};
	memset(skin_y, 40, sizeof(skin_y));
	memset(top_id, BLOCK_NETHERRACK, sizeof(top_id));
	// One lava column verifies that a compact liquid surface remains a real lava
	// block in the synthetic section. One no-surface column exercises its all-air
	// fill. Neither may receive Overworld skylight in DIM-1.
	top_id[0] = BLOCK_LAVA_STILL;
	skin_y[1] = 0xFF;

	world_build_far_section(&w, 0, 0, skin_y, top_id, grass_i, grass_j);
	struct block_data lava = world_get_block(&w, 0, 40, 0);
	assert(lava.type == BLOCK_LAVA_STILL);
	assert(lava.sky_light == 0);
	assert(lava.torch_light == 0);
	// Beta Block.lavaMoving/lavaStill setLightValue(1.0F). The fluid renderer
	// maxes sampled light against this registry luminance, so the far lava top
	// remains full-bright without inventing a synthetic block-light halo.
	assert(blocks[BLOCK_LAVA_STILL]);
	assert(blocks[BLOCK_LAVA_STILL]->luminance == 15);
	for(int y = 41; y <= 47; y++) {
		struct block_data air = world_get_block(&w, 0, y, 0);
		assert(air.type == BLOCK_AIR);
		assert(air.sky_light == 0);
	}
	for(int y = 32; y <= 47; y++) {
		struct block_data air = world_get_block(&w, 0, y, 1);
		assert(air.type == BLOCK_AIR);
		assert(air.sky_light == 0);
	}
	struct chunk* far_chunk = world_find_chunk(&w, 0, 32, 0);
	assert(far_chunk);
	assert(chunk_lookup_block(far_chunk, -1, 8, 0).sky_light == 0);
	// Overworld control: the same missing-neighbour approximation remains lit.
	w.dimension = WORLD_DIM_OVERWORLD;
	assert(chunk_lookup_block(far_chunk, -1, 8, 0).sky_light == 15);

	world_destroy(&w);
	puts("  Nether far section: lava retained; every synthetic air cell has sky=0");
}

static void test_pacing_frame_equivalence(bool handoff) {
	struct config cfg = {.root=json_value_init_object()};
	config_set_int(&cfg, "experimental.g9_priority_lease_int", 1);
	config_set_int(&cfg, "experimental.g9_main_handoff_int", handoff);
	struct config_g9_lease lease = config_read_g9_lease(&cfg);
	thread_g9_lease_configure(lease.enabled, lease.max_us, lease.service_us);
	struct world baseline, paced;
	world_create(&baseline); world_create(&paced);
	baseline.dimension = paced.dimension = WORLD_DIM_OVERWORLD;
	build_zero_light_air_section(&baseline);
	build_zero_light_air_section(&paced);
	struct block_data edit = {.type = BLOCK_AIR};
	world_set_block(&baseline, 8, 64, 8, edit, true);
	world_set_block(&paced, 8, 64, 8, edit, true);
	unsigned frames = 0;
	do {
		thread_main_pacing_configure(false, 2000, 1, 8);
		thread_g9_handoff_configure(false);
		world_update_lighting(&baseline);
		size_t work = world_lighting_driver_last_work();
		thread_main_pacing_configure(true, 2000, 1, 8);
		thread_g9_handoff_configure(lease.main_handoff);
		world_update_lighting(&paced);
		assert(work == world_lighting_driver_last_work());
		assert(stack_size(&baseline.lighting_updates) == stack_size(&paced.lighting_updates));
		assert(stack_size(&baseline.lighting_flood_queue) == stack_size(&paced.lighting_flood_queue));
		for(int y = 0; y < WORLD_HEIGHT; y++)
			for(int z = 0; z < CHUNK_SIZE; z++)
				for(int x = 0; x < CHUNK_SIZE; x++) {
					struct block_data a = world_get_block(&baseline, x, y, z);
					struct block_data b = world_get_block(&paced, x, y, z);
					assert(a.type == b.type && a.metadata == b.metadata);
					assert(a.sky_light == b.sky_light && a.torch_light == b.torch_light);
				}
		for(int y = 0; y < WORLD_HEIGHT; y += CHUNK_SIZE) {
			struct chunk* a = world_find_chunk(&baseline, 0, y, 0);
			struct chunk* b = world_find_chunk(&paced, 0, y, 0);
			assert((a != NULL) == (b != NULL));
			if(a) {
				assert(a->dirty_generation == b->dirty_generation);
				assert(a->rebuild_displist == b->rebuild_displist);
				assert(a->mesh_in_flight == b->mesh_in_flight);
			}
		}
		assert(++frames < 256);
	} while(!stack_empty(&baseline.lighting_updates)
		|| !lighting_flood_queue_empty(&baseline.lighting_flood_queue));
	assert(stack_empty(&paced.lighting_updates));
	assert(lighting_flood_queue_empty(&paced.lighting_flood_queue));
	thread_main_pacing_configure(false, 2000, 1, 8);
	thread_g9_handoff_configure(false);
	thread_g9_lease_configure(false, 8000, 500);
	config_destroy(&cfg);
	world_destroy(&baseline); world_destroy(&paced);
	printf("  pacing ON/OFF, handoff=%u/no owner: work, light bytes, dirties and completion identical\n",
		(unsigned)handoff);
}

/* REL-MP-FIX3d1: production remote admission (world_set_block in a remote
 * session) past one frame's budget and the legacy stack's capacity, after
 * injected reservation failures: bounded nonzero frames, then Beta's answer
 * (all air, heightmap 0: every cell sees the sky, sky=15) with no lost cell. */
static void remote_seed_overflow_makes_frame_progress(void) {
	struct world w;
	world_create(&w); // world_create starts in WORLD_DIM_OVERWORLD
	const bool was_remote = gstate.remote_session;
	gstate.remote_session = true;
	world_lighting_driver_diag_reset();
	build_zero_light_air_section(&w);
	const size_t slots = 9, bytes = lighting_remote_relight_bytes(slots);
	void* store = malloc(bytes);
	assert(bytes && store);
	assert(!world_remote_relight_begin(&w, NULL, bytes, slots));
	assert(!world_remote_relight_begin(&w, store, bytes - 1, slots));
	assert(!world_remote_relight_pending(&w));
	/* A failed reservation latches: remote seeds never reach the legacy
	 * stack (the client also closes readiness; see the R17 test). */
	assert(w.remote_relight_failed);
	world_set_block(&w, 3, 5, 3, (struct block_data) {.type = BLOCK_AIR}, true);
	assert(stack_empty(&w.lighting_updates) && !world_remote_relight_pending(&w));
	world_update_lighting(&w);
	assert(world_lighting_driver_last_work() == 0u);
	const bool reserved = world_remote_relight_begin(&w, store, bytes, slots);
	const size_t seeds = LIGHTING_CLIENT_POPS_PER_FRAME + 513u;
	for(size_t i = 0; i < seeds; i++) // distinct cells, all in column (0,0)
		world_set_block(&w, (w_coord_t)(i % 16), (w_coord_t)(1 + i / 256),
						(w_coord_t)((i / 16) % 16),
						(struct block_data) {.type = BLOCK_AIR}, true);
	const size_t legacy_depth = stack_size(&w.lighting_updates);
	size_t frames = 0, at_cap = 0, total = 0;
	while(world_remote_relight_pending(&w) || !stack_empty(&w.lighting_updates)
		  || !lighting_flood_queue_empty(&w.lighting_flood_queue)) {
		world_update_lighting(&w);
		const size_t work = world_lighting_driver_last_work();
		assert(work <= LIGHTING_CLIENT_POPS_PER_FRAME);
		assert(work > 0u && "frame with pending relight made no progress");
		at_cap += work == LIGHTING_CLIENT_POPS_PER_FRAME;
		total += work;
		assert(++frames < 1024u && "remote relight did not drain");
	}
	size_t wrong = 0;
	for(int i = 0; i < WORLD_HEIGHT * CHUNK_SIZE * CHUNK_SIZE; i++) {
		struct block_data b = world_get_block(&w, i % 16, i / 256, (i / 16) % 16);
		wrong += b.sky_light != 15 || b.torch_light != 0;
	}
	printf("  FIX3d: %zu seeds -> legacy lighting_updates depth %zu, reservation %s,"
		   " %zu frames (%zu at cap), %zu work, %zu cells wrong\n", seeds,
		   legacy_depth, reserved ? "attached" : "MISSING", frames, at_cap, total, wrong);
	fflush(stdout);
	assert(wrong == 0u && "remote relight lost an obligation");
	assert(frames > 1u && at_cap > 0u && "fixture never exceeded one frame");
	assert(legacy_depth == 0u
		   && "RED: remote seeds still go to the growing lighting_updates stack");
	assert(reserved && "RED: no bounded remote relight reservation exists");
	world_remote_relight_end(&w);
	gstate.remote_session = was_remote;
	world_destroy(&w);
	free(store);
	puts("  remote seed overflow: bounded nonzero frames, lossless drain PASS");
}

int main(int argc, char** argv) {
	blocks_init();
	if(argc > 1 && !strcmp(argv[1], "--bench")) {
		uint64_t sum = 0;
		for(unsigned n=0; n<12; n++) sum += client_fixture(2);
		printf("client kernel benchmark: %llu CPU units / 12 repeats\n", (unsigned long long)sum);
		return 0;
	}
	client_private = argc > 1 && !strcmp(argv[1], "--golden-private");
	const char* golden_path = argc > 2 ? argv[2]
		: "../tests/golden/lighting_client_r16.bin.gz";
	client_golden(golden_path, argc > 1 && !strcmp(argv[1], "--record"));
	if(!client_record && !client_private) {
		client_private = true;
		client_golden(golden_path, false);
		client_private = false;
	}
	test_stale_backlog_is_budgeted();
	test_active_flood_is_budgeted();
	thread_main_pacing_configure(true, 2000, 1, 8);
	test_stale_backlog_is_budgeted();
	test_active_flood_is_budgeted();
	thread_main_pacing_configure(false, 2000, 1, 8);
	test_nether_far_section_has_no_skylight();
	test_pacing_frame_equivalence(false);
	test_pacing_frame_equivalence(true);
	remote_seed_overflow_makes_frame_progress();
	printf("test_lighting_client_driver_budget: production driver stayed <= %u "
		   "work/frame\n",
		   LIGHTING_CLIENT_POPS_PER_FRAME);
	return 0;
}
