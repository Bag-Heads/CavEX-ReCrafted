/*
	BUG-HUNT env test (2026-06-29): interleave the two Wii-freeze pressure
	sources that were previously tested only in isolation.

	Redstone tests assert bounded notify/wire work. Lighting tests assert the
	client relight flood is paced by LIGHTING_CLIENT_POPS_PER_FRAME. This test
	drives both in the same frame loop: a live redstone loop-completion repro on
	the server side while the client world drains large deferred relight floods.

	Diagnostic, not a Beta golden. It changes no simulation behavior; it only
	asserts the production counters stay bounded when both workloads are active.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "block/redstone_server.h"
#include "item/items.h"
#include "lighting.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"
#include "world.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_lighting_churn_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define Y  101

#define REDSTONE_REPRO_NOTIFY_DISPATCH_CAP 4096
#define REDSTONE_REPRO_WIRE_POP_CAP REDSTONE_WIRE_WORK_MAX
#define COMBINED_FRAME_WORK_CAP \
	(LIGHTING_CLIENT_POPS_PER_FRAME + REDSTONE_REPRO_WIRE_POP_CAP \
	 + (2u * REDSTONE_REPRO_NOTIFY_DISPATCH_CAP))

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

static void set_server_block(int x, int y, int z, int type, int meta) {
	server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
}

static int get_server_meta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F)
														 : -1;
}

static const char* g_phase = "init";
static void tick_wd(void) {
	clock_t t0 = clock();
	server_world_tick(&s.world, &s);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	if(ms > 2000.0) {
		fprintf(stderr,
				"WATCHDOG: server_world_tick took %.0f ms in phase [%s] -- "
				"sched=%zu drops=%u wire_depth=%d ncas_us=%u\n",
				ms, g_phase, s.world.scheduled_tick_count,
				(unsigned)s.world.scheduled_tick_drops,
				redstone_server_wire_flood_depth_max(),
				(unsigned)server_world_notify_cascade_max_us());
		abort();
	}
}

static bool place_redstone_dust(int x, int y, int z) {
	struct item_data it = {.id = ITEM_REDSTONE, .count = 1};
	struct block_data on_blk;
	if(!server_world_get_block(&s.world, x, y - 1, z, &on_blk))
		return false;
	struct block_info where = {.x = x, .y = y, .z = z};
	struct block_info on = {.x = x, .y = y - 1, .z = z, .block = &on_blk};
	struct item* ir = item_get(&it);
	assert(ir && ir->onItemPlace);
	return ir->onItemPlace(&s, &s.player, &it, &where, &on, SIDE_TOP);
}

static void break_block(int x, int y, int z) {
	server_world_set_block(&s.worlds[0], x, y, z,
						   (struct block_data) {.type = BLOCK_AIR,
												.metadata = 0});
	notifyNeighbours(&s, x, y, z);
}

static void reset_redstone_work_bounds(void) {
	server_world_notify_diag_reset();
	blocks_notify_neighbours_diag_reset();
	redstone_server_wire_flood_diag_reset();
}

static void assert_redstone_bounds(const char* phase) {
	int sw_depth = server_world_notify_depth_max();
	int sw_hits = server_world_notify_depth_cap_hits();
	int sw_dispatch = server_world_notify_cascade_max_dispatches();
	int nn_depth = blocks_notify_neighbours_depth_max();
	int nn_hits = blocks_notify_neighbours_depth_cap_hits();
	int nn_dispatch = blocks_notify_neighbours_cascade_max_dispatches();
	int wire_depth = redstone_server_wire_flood_depth_max();
	int wire_hits = redstone_server_wire_flood_depth_cap_hits();
	int wire_pops = redstone_wire_update_max_pops();
	int wire_full = redstone_wire_update_full_hits();
	if(sw_depth > SERVER_WORLD_NOTIFY_MAX_DEPTH || sw_hits != 0
	   || sw_dispatch > REDSTONE_REPRO_NOTIFY_DISPATCH_CAP
	   || nn_depth > BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH || nn_hits != 0
	   || nn_dispatch > REDSTONE_REPRO_NOTIFY_DISPATCH_CAP
	   || wire_depth > RS_WIRE_FLOOD_MAX_DEPTH || wire_hits != 0
	   || wire_pops > REDSTONE_REPRO_WIRE_POP_CAP || wire_full != 0
	   || s.world.scheduled_tick_drops != 0) {
		fprintf(stderr,
				"REDSTONE BOUND FAIL [%s]: sw_depth=%d/%d sw_hits=%d "
				"sw_dispatch=%d nn_depth=%d/%d nn_hits=%d nn_dispatch=%d "
				"wire_depth=%d/%d wire_hits=%d wire_pops=%d/%d wire_full=%d "
				"scheduled_drops=%u\n",
				phase, sw_depth, SERVER_WORLD_NOTIFY_MAX_DEPTH, sw_hits,
				sw_dispatch, nn_depth, BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH,
				nn_hits, nn_dispatch, wire_depth, RS_WIRE_FLOOD_MAX_DEPTH,
				wire_hits, wire_pops, REDSTONE_REPRO_WIRE_POP_CAP, wire_full,
				(unsigned)s.world.scheduled_tick_drops);
	}
	assert(sw_depth <= SERVER_WORLD_NOTIFY_MAX_DEPTH);
	assert(sw_hits == 0);
	assert(sw_dispatch <= REDSTONE_REPRO_NOTIFY_DISPATCH_CAP);
	assert(nn_depth <= BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH);
	assert(nn_hits == 0);
	assert(nn_dispatch <= REDSTONE_REPRO_NOTIFY_DISPATCH_CAP);
	assert(wire_depth <= RS_WIRE_FLOOD_MAX_DEPTH);
	assert(wire_hits == 0);
	assert(wire_pops <= REDSTONE_REPRO_WIRE_POP_CAP);
	assert(wire_full == 0);
	assert(s.world.scheduled_tick_drops == 0);
}

static size_t combined_frame_work(void) {
	return world_lighting_driver_last_work()
		   + (size_t)server_world_notify_cascade_max_dispatches()
		   + (size_t)blocks_notify_neighbours_cascade_max_dispatches()
		   + (size_t)redstone_wire_update_max_pops();
}

static void assert_combined_bounds(const char* phase) {
	assert_redstone_bounds(phase);
	assert(world_lighting_driver_last_work() <= LIGHTING_CLIENT_POPS_PER_FRAME);
	assert(combined_frame_work() <= COMBINED_FRAME_WORK_CAP);
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

	world_build_full_section(w, 0, 0, ids, metadata, sky, torch, heightmap,
							 grass_i, grass_j);

	free(grass_j);
	free(grass_i);
	free(heightmap);
	free(torch);
	free(sky);
	free(metadata);
	free(ids);
}

static void seed_client_light_flood(struct world* w) {
	world_set_block(w, 8, 64, 8,
					(struct block_data) {
						.type = BLOCK_AIR,
						.metadata = 0,
						.sky_light = 0,
						.torch_light = 0,
					},
					true);
	assert(!stack_empty(&w->lighting_updates));
}

static void build_powered_cycle_with_gap(void) {
	for(int x = 0; x <= 15; x++)
		for(int z = 0; z <= 15; z++)
			set_server_block(x, YF, z, BLOCK_STONE, 0);

	set_server_block(1, Y, 1, BLOCK_LEVER, 5 | 8);
	set_server_block(2, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	set_server_block(3, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	/* Gap at (4,Y,1), the loop-completion cell. */
	set_server_block(5, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	set_server_block(6, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	set_server_block(6, Y, 2, BLOCK_REDSTONE_WIRE, 0);
	set_server_block(6, Y, 3, BLOCK_REDSTONE_WIRE, 0);
	for(int x = 6; x >= 1; x--)
		set_server_block(x, Y, 3, BLOCK_REDSTONE_WIRE, 0);
	set_server_block(1, Y, 2, BLOCK_REDSTONE_WIRE, 0);

	for(int k = 0; k < 20; k++)
		tick_wd();
	assert(get_server_meta(2, Y, 1) > 0);
}

int main(void) {
	test_fixture_dir("test_rs_lighting_churn_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	redstone_torch_reset_burnout();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = FCAVEX_SCHEDULED_TICK_BUDGET;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	struct world client_world;
	world_create(&client_world);
	client_world.dimension = WORLD_DIM_OVERWORLD;
	build_zero_light_air_section(&client_world);
	world_lighting_driver_diag_reset();
	seed_client_light_flood(&client_world);

	g_phase = "build-redstone-cycle";
	build_powered_cycle_with_gap();

	size_t frames_at_light_cap = 0;
	size_t frames_with_light_work = 0;
	size_t frames_with_redstone_work = 0;
	size_t combined_max = 0;

	g_phase = "interleaved-redstone-lighting";
	for(int frame = 0; frame < 160; frame++) {
		char phase[64];
		snprintf(phase, sizeof(phase), "frame-%03d", frame);
		reset_redstone_work_bounds();

		if((frame & 1) == 0) {
			bool placed = place_redstone_dust(4, Y, 1);
			assert(placed);
			assert(get_server_meta(4, Y, 1) > 0);
		} else {
			break_block(4, Y, 1);
		}
		tick_wd();

		if(stack_empty(&client_world.lighting_updates)
		   && lighting_flood_queue_empty(&client_world.lighting_flood_queue))
			seed_client_light_flood(&client_world);
		world_update_lighting(&client_world);

		if(world_lighting_driver_last_work() == LIGHTING_CLIENT_POPS_PER_FRAME)
			frames_at_light_cap++;
		if(world_lighting_driver_last_work() > 0)
			frames_with_light_work++;
		if(redstone_wire_update_max_pops() > 0
		   || server_world_notify_cascade_max_dispatches() > 0)
			frames_with_redstone_work++;
		size_t combined = combined_frame_work();
		if(combined > combined_max)
			combined_max = combined;
		assert_combined_bounds(phase);
	}

	assert(frames_at_light_cap > 0);
	assert(frames_with_light_work > 100);
	assert(frames_with_redstone_work > 100);
	printf("test_redstone_lighting_churn_env: OK (%zu light-work frames, "
		   "%zu redstone-work frames, %zu light-cap frames, combined_max=%zu/%u)\n",
		   frames_with_light_work, frames_with_redstone_work, frames_at_light_cap,
		   combined_max, (unsigned)COMBINED_FRAME_WORK_CAP);

	world_destroy(&client_world);
	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
