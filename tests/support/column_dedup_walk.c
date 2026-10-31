#include "b1_bind_client.h"
#include "carve_observer.h"
/* Copyright (c) 2026 CaveWii_v2 contributors
 * The production terrain-safe split-anchor autopilot and owned player physics,
 * clocked by a controlled event loop. This freezes consumed RPC prefixes and
 * mesh completion boundaries independently of OS thread arrival times.
 */
#include "column_dedup_walk.h"
#include "split_residency_fixture.h"
#include "column_observer.h"
#include "chunk_mesher.h"
#include "game/autopilot.h"
#include "game/autopilot_keepup.h"
#include "game/game_state.h"
#include "item/recipe.h"
#include "platform/audio.h"
#include "platform/input_owner.h"
#include "worldgen/worldgen.h"

static void (*seed_walk_fixture)(struct server_local*);

static void drain(const struct client_rpc* call, void* unused) {
	(void)call; (void)unused;
	clin_update_drain();
}

static struct server_local* fixture(int64_t seed) {
	test_fixture_dir("test_column_dedup", split_path, sizeof(split_path));
#ifdef _WIN32
	_mkdir(split_path);
#else
	mkdir(split_path, 0777);
#endif
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	server_local_test_create_without_thread(s, false, false, false, false,
		false, false, false);
	string_t path;
	string_init_set_str(path, split_path);
	assert(level_archive_create_new(&s->level, path, seed, (vec3){0, 80, 0}));
	assert(server_world_create_with_populate_scratch(&s->world, path,
		WORLD_DIM_OVERWORLD, seed, false, s->world_populate_scratch));
	string_clear(path);
	s->world.local = s;
	s->worlds[0].session = s; /* A1a: services through the owning session */
	b1_bind_client_world(&s->worlds[0]);
	s->rand_src = (struct random_gen){.seed = (unsigned)seed};
	/* Item-drop velocity uses Beta's separate Math.random stream. Leaving
	 * its lazy clock seed live makes pickup RPC boundaries nondeterministic. */
	server_local_test_seed_math_random(seed ^ 0xC011);
	*server_local_sound_rng() = (struct random_gen){.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen){.seed = 9876};
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.oxygen = MAX_OXYGEN;
	s->autosave_ticks = 0;
	s->render_demo_mob = -1;
	s->far_view_distance = MAX_VIEW_DISTANCE;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	if(seed_walk_fixture) seed_walk_fixture(s);
	return s;
}

static void mesh_at_player(unsigned tick) {
	struct entity* e = gstate.players[0].local_player;
	/* One scheduled completion per logical tick, cycling all eight slices.
 * Real production gather -> worker -> receive, including halo and tint. */
	struct chunk* c = world_find_chunk(&gstate.world, (int)floorf(e->pos[0]),
		(int)(tick % 8) * 16, (int)floorf(e->pos[2]));
	assert(c);
	c->dirty_generation++;
	assert(chunk_mesher_send(c));
	c->mesh_in_flight = true;
	c->rebuild_displist = false;
	c->first_meshed = true;
	ptime_t start = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		assert(time_diff_s(start, time_get()) < 30);
		if(c->mesh_in_flight) thread_msleep(1);
	}
}

static void forced_respawn(struct server_local* s, unsigned death) {
	s->player.spawn_x = (int)death * 1024;
	s->player.spawn_y = 80;
	s->player.spawn_z = 0;
	assert(server_local_set_player_health(s, 0));
	assert(server_local_respawn_request(s));
	unsigned updates = 0;
	do {
		server_local_test_update_once(s);
		clin_update_drain();
		world_pre_render_clear(&gstate.world);
		world_render_completed(&gstate.world, false);
		world_update_lighting(&gstate.world);
		column_observer_client(&gstate.world, -1);
		/* This headless fixture owns the presentation boundary. It explicitly
		 * acknowledges only the published epoch after its collision install;
		 * the normal app's mesher/readiness scheduler remains untouched. */
		if(s->input_ack_pending && gstate.world_loaded)
			assert(svin_rpc_send(&(struct server_rpc){.type = SRPC_INPUT_READY,
				.payload.input_ready = {.epoch = s->input_ack_pending}}));
		assert(++updates < 2000);
	} while(s->world_switch_phase != SERVER_WORLD_SWITCH_IDLE || s->input_ack_pending);
	assert(s->player.health > 0 && gstate.players[0].local_player);
	gstate.players[0].local_player->data.local_player.capture_input = true;
	printf("FORCED_RESPAWN %u committed after %u updates, seed42 x=%.3f z=%.3f\n",
		death, updates, s->player.x, s->player.z);
	fflush(stdout);
}

static bool run(int64_t seed, const char* prefix, unsigned trace, bool capture,
				unsigned ticks) {
	assert(column_observer_start(prefix, trace, !capture));
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD}});
	struct server_local* s = fixture(seed);
	clin_test_set_rpc_observer(drain, NULL);
	for(int z = -2; z <= 2; z++)
		for(int x = -2; x <= 2; x++) split_populate(s, x, z);
	struct server_chunk* center = dict_server_chunks_get(s->world.chunks,
		S_CHUNK_ID(0, 0));
	assert(center);
	s->player.x = s->player.z = 0.5;
	s->player.y = center->heightmap[0] + PLAYER_EYE_HEIGHT;
	/* Install through the actual local pooled RPC path before releasing the
 * controlled clock. All normal reach and generation budgets remain intact. */
	for(unsigned n = 0; n < 4; n++) {
		server_local_test_update_once(s);
		clin_update_drain();
	}
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {s->player.x, s->player.y, s->player.z},
			.oxygen = MAX_OXYGEN}});
	gstate.world_loaded = true;
	gstate.players[0].local_player->data.local_player.capture_input = true;
	memset(&gstate.autopilot, 0, sizeof(gstate.autopilot));
	gstate.autopilot.active = true;
	gstate.autopilot.mode = 2;
	gstate.autopilot.terrain_safe = true;
	autopilot_keepup_reset(&gstate.autopilot.keepup, true);
	if(trace & 2) assert(chunk_mesher_init());
	column_observer_window(true);
	for(unsigned tick = 0; tick < ticks && column_observer_ok(); tick++) {
		if(!trace && (tick == 600 || tick == 1400))
			forced_respawn(s, tick == 600 ? 1 : 2);
		struct entity* e = gstate.players[0].local_player;
		autopilot_tick();
		struct input_player_frame frame = {.connected = true};
		const enum input_button buttons[] = {IB_FORWARD, IB_BACKWARD, IB_LEFT,
			IB_RIGHT, IB_JUMP, IB_SNEAK};
		for(unsigned k = 0; k < sizeof(buttons) / sizeof(*buttons); k++)
			autopilot_force_button(buttons[k], &frame.down[buttons[k]]);
		entity_local_player_walk_owned(e, &gstate.players[0], &frame);
		assert(svin_rpc_send(&(struct server_rpc){.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = e->pos[0], .y = e->pos[1], .z = e->pos[2],
				.rx = e->orient[0], .ry = e->orient[1], .vel_y = e->vel[1],
				.on_ground = e->on_ground,
				.correction_seq = gstate.players[0].player_sync.correction_seq}}));
		server_local_test_update_once(s);
		clin_update_drain();
		world_update_lighting(&gstate.world);
		column_observer_client(&gstate.world, -1);
		if(trace & 2) mesh_at_player(tick);
		if(tick % 200 == 0) {
			printf("seed=%lld step=%u server_tick=%u pos=%.3f,%.3f,%.3f\n",
				(long long)seed, tick, s->world.current_tick,
				e->pos[0], e->pos[1], e->pos[2]);
			fflush(stdout);
		}
	}
	if(column_observer_ok()) assert(s->world.current_tick >= ticks);
	if(trace & 2) chunk_mesher_shutdown();
	assert(!mem2_stat_oom() && !mem2_canary_hits()
		&& !__atomic_load_n(&g_mesh_truncations, __ATOMIC_RELAXED));
	clin_test_set_rpc_observer(NULL, NULL);
	bool ok = column_observer_stop();
	world_destroy(&gstate.world);
	dict_entity_clear(gstate.entities);
	split_destroy(s);
	uint32_t created, destroyed, references;
	column_backing_test_counts(&created, &destroyed, &references);
	assert(created == destroyed && references == 0);
	return ok;
}

int column_dedup_walk_main(int argc, char** argv, unsigned trace) {
	bool census = getenv("CW_CARVE_GOLDENS") != NULL;
	if(census) assert(carve_observer_start());
	blocks_init(); items_init(); recipe_init();
	if(argc == 3 && !strcmp(argv[1], "--respawn-census"))
		return run(42, argv[2], 0, true, 2000) ? 0 : 1;
	bool capture = argc >= 2 && !strcmp(argv[1], "--capture");
	unsigned ticks = 2000;
	if(capture && argc >= 4) ticks = (unsigned)strtoul(argv[3], NULL, 10);
	for(unsigned i = 0; i < 2; i++) {
		int64_t seed = i ? 42 : 12345;
		char prefix[1024];
		const char* root = argc >= 3 && (capture || !strcmp(argv[1], "--verify-root")) ? argv[2]
			: "tests/golden/column_dedup/ms2";
		snprintf(prefix, sizeof(prefix), "%s_%lld_", root, (long long)seed);
		if(!run(seed, prefix, trace, capture, ticks)) return 1;
	}
	if(census) assert(carve_observer_stop());
	puts("PASS column dedup frozen deterministic walk (both seeds)");
	return 0;
}

int column_dedup_walk_main_seeded(int argc, char** argv, unsigned trace,
	void (*seed_fixture)(struct server_local*)) {
	assert(!seed_walk_fixture);
	seed_walk_fixture = seed_fixture;
	int result = column_dedup_walk_main(argc, argv, trace);
	seed_walk_fixture = NULL;
	return result;
}
