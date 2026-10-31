/* A committed Overworld respawn must retire its old ring BEFORE filling the
 * replacement, even while presentation withholds INPUT_READY. Two actual
 * death/respawn requests, real server ticks, RPC installs, and GPU union refs.
 * Three identical 5x5 flat patches plus their population halos are authored
 * on disk; generator=NULL prevents unrelated population from obscuring counts. */
#include "support/split_residency_fixture.h"
#include "game/game_state.h"
#include "game/client_session.h"
#include "game/gui/screen.h"
#include "item/recipe.h"
#include "platform/pc/heap_stats.h"
#include "network/server_local_guest.h"

static int destination;
static bool check_departed;
static unsigned new_columns;
static unsigned load_failures, input_handoffs;

static bool departed_client(void) {
	dict_wsection_it_t it;
	for(dict_wsection_it(it, gstate.world.sections);
		!dict_wsection_end_p(it); dict_wsection_next(it)) {
		int64_t id = dict_wsection_ref(it)->key;
		if(abs(S_CHUNK_X(id) - destination) > MAX_VIEW_DISTANCE)
			return true;
	}
	return false;
}

static void drain_before_send(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_LOAD_FAILED)
		load_failures++;
	if(rpc->type == CRPC_PLAYER_POS)
		input_handoffs++;
	clin_update_drain();
	/* A completed flip releases the previous frame's GPU union. No rendering
	 * thread or real GL context is needed for the CPU display-list ownership. */
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	if(check_departed && rpc->type == CRPC_CHUNK) {
		assert(!departed_client() && "new ring streamed over retained old ring");
		new_columns++;
	}
}

static void tick(struct server_local* s) {
	server_local_test_update_once(s);
	clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
}

static void test_server_respawn_clearance(struct server_local* s) {
	const int xs[2] = {4, 8};
	const int feet[2] = {32, 64};
	for(unsigned i = 0; i < 2; i++) {
		for(int x = xs[i]; x <= xs[i] + 1; x++)
			for(int z = 4; z <= 5; z++)
				for(int y = feet[i] - 1; y <= feet[i] + 2; y++)
					(void)server_world_set_block(&s->worlds[0], x, y, z,
						(struct block_data) {.type = y == feet[i] - 1
							? BLOCK_STONE : BLOCK_AIR});
		double eye = (double)((float)feet[i] + PLAYER_EYE_HEIGHT);
		assert(server_local_test_respawn_target_clear(
			s, xs[i] + 0.5, eye, 4.5));
		(void)server_world_set_block(&s->worlds[0], xs[i], feet[i], 4,
			(struct block_data) {.type = BLOCK_STONE});
		assert(!server_local_test_respawn_target_clear(
			s, xs[i] + 0.5, eye, 4.5));
		(void)server_world_set_block(&s->worlds[0], xs[i], feet[i], 4,
			(struct block_data) {.type = BLOCK_AIR});
	}
	/* Bed plans retain their authored fractional +0.1 feet clearance. */
	double bed_eye = (double)((float)32 + PLAYER_EYE_HEIGHT) + 0.1;
	assert(server_local_test_respawn_target_clear(s, 4.5, bed_eye, 4.5));
	struct piston_moving_state moving = {.stored_id = BLOCK_STONE,
		.facing = 0, .flags = PISTON_MOVING_FLAG_EXTENDING,
		.current_progress = 0.5F, .last_progress = 0.5F};
	assert(server_world_set_block(&s->worlds[0], 4, 32, 4,
		(struct block_data) {.type = BLOCK_PISTON_MOVING}));
	assert(piston_moving_store_upsert_owned( /* A1b: world-tagged */
		&s->piston_moving, server_world_piston_owner(&s->worlds[0]), 4, 32, 4, &moving, NULL));
	assert(!server_local_test_respawn_target_clear(
		s, 4.5, (double)(32.0F + PLAYER_EYE_HEIGHT), 4.5));
	assert(piston_moving_store_remove(&s->piston_moving, 4, 32, 4));
	assert(server_world_set_block(&s->worlds[0], 4, 32, 4,
		(struct block_data) {.type = BLOCK_AIR}));
	/* The same stored full cube at y31 with zero shift only touches the
	 * body's feet face, so Beta's strict overlap accepts it. */
	moving.current_progress = moving.last_progress = 1.0F;
	assert(server_world_set_block(&s->worlds[0], 4, 31, 4,
		(struct block_data) {.type = BLOCK_PISTON_MOVING}));
	assert(piston_moving_store_upsert_owned( /* A1b: world-tagged */
		&s->piston_moving, server_world_piston_owner(&s->worlds[0]), 4, 31, 4, &moving, NULL));
	assert(server_local_test_respawn_target_clear(
		s, 4.5, (double)(32.0F + PLAYER_EYE_HEIGHT), 4.5));
	assert(piston_moving_store_remove(&s->piston_moving, 4, 31, 4));
	assert(server_world_set_block(&s->worlds[0], 4, 31, 4,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(!server_local_test_respawn_target_clear(
		s, 100000.5, (double)(64.0F + PLAYER_EYE_HEIGHT), 100000.5));
	clin_drain_discard();
}

/* A separate real Join/Accept/Confirm/GamePlay lifecycle pins the stream-only
 * owner-0 anchor without weakening this file's legacy global-respawn census. */
static void test_host_same_world_single_anchor(void) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	(void)split_populate(s, 0, 0);
	struct server_chunk* target = split_populate(s, 64, 0);
	struct server_chunk* corpse = dict_server_chunks_get(
		s->world.chunks, S_CHUNK_ID(0, 0));
	assert(corpse);
	test_server_respawn_clearance(s);
	corpse->sent = true;
	target->sent = false;
	s->player.finished_loading = true;
	assert(server_local_set_split_enabled(s, true));
	const uint64_t ticket = 0x51u;
	struct server_rpc join = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket, .action = LOCAL_GUEST_JOIN}};
	server_local_guest_control(s, (struct server_rpc_source) {0}, &join);
	assert(s->guest_owned && s->guest_actor && s->guest_generation);
	struct server_rpc_source guest = {.slot = 1,
		.generation = s->guest_generation};
	assert(client_session_bind_local(&gstate.players[1], guest.generation));
	join.payload.local_guest.generation = guest.generation;
	join.payload.local_guest.action = LOCAL_GUEST_ACCEPT;
	server_local_guest_control(s, guest, &join);
	join.payload.local_guest.action = LOCAL_GUEST_CONFIRM;
	server_local_guest_control(s, guest, &join);
	join.payload.local_guest.action = LOCAL_GUEST_GAMEPLAY;
	server_local_guest_control(s, guest, &join);
	clin_drain_discard();
	assert(s->guest_confirmed && s->guest_gameplay
		&& s->guest_actor->finished_loading);
	/* Put owner 1 into its real S3 LOADING phase first. Owner 0 must still
	 * choose the local relationship when both bodies are dead. */
	bool guest_hit = false;
	assert(!server_local_damage_actor_result(s, s->guest_actor,
		MAX_PLAYER_HEALTH, 0.0f, 0.0f, &guest_hit));
	assert(guest_hit && s->guest_actor->health == 0);
	struct server_rpc guest_respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = guest.generation}};
	assert(svin_rpc_send_from(guest, &guest_respawn));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(s->guest_respawn_phase == GUEST_RESPAWN_LOADING
		&& !s->guest_actor->finished_loading);

	s->player.spawn_x = 64 * CHUNK_SIZE;
	s->player.spawn_y = 4;
	s->player.spawn_z = 0;
	assert(server_local_set_player_health(s, 0));
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 0}};
	assert(svin_rpc_send(&respawn));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
		&& s->host_respawn_anchor && s->player.health == 0
		&& s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
	uint64_t epoch = s->host_respawn_epoch;
	double corpse_x = s->player.x, corpse_z = s->player.z;
	assert(s->guest_actor->health == 0
		&& s->guest_respawn_phase == GUEST_RESPAWN_LOADING
		&& "both-dead guest LOADING must not fall back to a world switch");
	struct server_rpc guest_ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket,
			.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
			.generation = guest.generation,
			.respawn_epoch = s->guest_respawn_epoch}};
	assert(svin_rpc_send_from(guest, &guest_ready));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(s->guest_actor->health == MAX_PLAYER_HEALTH
		&& s->guest_actor->finished_loading
		&& s->guest_respawn_phase == GUEST_RESPAWN_IDLE);
	tick(s); /* retire the guest commit's same-update stale-input fence */
	/* The admitted survivor remains independently movable while owner 0 is
	 * hidden behind readiness. */
	double guest_x = s->guest_actor->x;
	struct server_rpc guest_move = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = guest_x + 0.25, .y = s->guest_actor->y,
			.z = s->guest_actor->z, .rx = s->guest_actor->rx,
			.ry = s->guest_actor->ry,
			.correction_seq = s->guest_actor->correction_seq}};
	assert(svin_rpc_send_from(guest, &guest_move));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(s->guest_actor->x == guest_x + 0.25
		&& s->player.x == corpse_x && s->player.z == corpse_z);

	/* Leave after the active plan: owner 0 must retain its target as the sole
	 * stream anchor and finish without falling into the legacy switch. */
	assert(server_local_guest_leave(s, guest));
	clin_drain_discard();
	assert(!s->guest_actor && !server_local_two_anchor(s)
		&& server_local_host_respawn_active(s));
	assert(server_local_startup_class(s, 64, 0) == CARVE_A_PLAYER);
	struct server_chunk* target_after_leave = dict_server_chunks_get(s->world.chunks,
		S_CHUNK_ID(64, 0));
	assert(target_after_leave);
	target_after_leave->sent = false; /* create real one-anchor publication debt */
	respawn.payload.respawn.respawn_epoch = epoch;
	uint32_t corpse_seq = s->player.correction_seq;
	assert(svin_rpc_send(&respawn));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
		&& s->host_respawn_ready && s->player.health == 0
		&& s->player.correction_seq == corpse_seq
		&& !server_world_is_chunk_sent(&s->world, 64, 0)
		&& s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
	uint64_t tick_before = s->world.current_tick;
	unsigned streamed_ticks = 0;
	while(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
		&& streamed_ticks++ < 600) {
		tick(s);
		if(s->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING)
			assert(s->player.x == corpse_x && s->player.z == corpse_z);
	}
	assert(s->player.health == MAX_PLAYER_HEALTH
		&& s->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE
		&& s->host_respawn_epoch_counter == epoch
		&& s->world_switch_phase == SERVER_WORLD_SWITCH_IDLE
		&& streamed_ticks > 0 && s->world.current_tick > tick_before
		&& s->player.x == s->host_respawn_x
		&& s->player.y == s->host_respawn_y
		&& s->player.z == s->host_respawn_z
		&& s->player.correction_seq != corpse_seq
		&& server_world_is_chunk_sent(&s->world, 64, 0));
	/* With the guest durably gone before this new request, a fresh host death
	 * retains the legacy global transaction. The completed local lifetime is
	 * not reopened and its monotonic counter remains evidence. */
	assert(server_local_set_player_health(s, 0));
	respawn.payload.respawn.respawn_epoch = 0;
	assert(svin_rpc_send(&respawn));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(s->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE
		&& !server_local_host_respawn_active(s)
		&& s->host_respawn_epoch_counter == epoch
		&& s->world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(client_session_unbind_local(&gstate.players[1], guest.generation));
	split_destroy(s);
	puts("PASS split host respawn: real guest admission, guest Leave after plan, "
		 "single target anchor, exact ACK commit, retained epoch counter");
}

/* Sixteen real far claims exceed one unload batch. They must also retire
 * before the first destination full-column RPC, including their voxel backing. */
static void install_far(struct server_local* s) {
	uint8_t heights[CHUNK_SIZE * CHUNK_SIZE], ids[sizeof(heights)];
	uint8_t tint[sizeof(heights)];
	memset(heights, 3, sizeof(heights));
	memset(ids, BLOCK_GRASS, sizeof(ids));
	memset(tint, 0, sizeof(tint));
	for(int z = -8; z < 8; z++) {
		int x = destination + MAX_VIEW_DISTANCE + 1;
		assert(world_build_far_section(&gstate.world, x * CHUNK_SIZE,
			z * CHUNK_SIZE, heights, ids, tint, tint));
		server_world_mark_far_sent(&s->world, x, z);
	}
}

struct census {
	size_t sections, chunks, server_chunks, gpu, mem2;
	size_t view_count[2], view_capacity[2];
#ifdef _WIN32
	struct pc_heap_stats heap;
#endif
};

static struct census census(struct server_local* s) {
	struct census result = {
		.sections = world_loaded_chunks(&gstate.world),
		.server_chunks = dict_server_chunks_size(s->world.chunks),
		.mem2 = mem2_stat_alloc_calls() - mem2_stat_free_calls(),
	};
	dict_wsection_it_t it;
	for(dict_wsection_it(it, gstate.world.sections);
		!dict_wsection_end_p(it); dict_wsection_next(it)) {
		struct world_section* section = &dict_wsection_ref(it)->value;
		for(unsigned k = 0; k < COLUMN_HEIGHT; k++) {
			struct chunk* c = section->column[k];
			if(!c) continue;
			result.chunks++;
			memset(c->reachable, 0xff, sizeof(c->reachable));
			if(!c->has_displist[0]) {
				/* Real owned CPU display-list storage, deliberately unfinalized:
				 * a headless test has no GL VBO to destroy. */
				displaylist_init(&c->mesh[0], 4, 22);
				c->mesh[0].data = malloc(128);
				assert(c->mesh[0].data);
				c->has_displist[0] = true;
			}
		}
	}
	struct camera camera = {
		.x = destination * CHUNK_SIZE + 8, .y = 8, .z = 8,
	};
	glm_mat4_identity(camera.view);
	for(unsigned p = 0; p < 6; p++) camera.frustum_planes[p][3] = 1;
	struct camera* views[] = {&camera, &camera};
	gstate.config.fog_distance = 4096;
	assert(world_pre_render_views(&gstate.world, views, 2));
	for(unsigned v = 0; v < 2; v++) {
		result.view_count[v] = gstate.world.render_views[v].count;
		result.view_capacity[v] = gstate.world.render_views[v].capacity;
	}
	world_render_completed(&gstate.world, true);
	ilist_chunks2_it_t gpu;
	for(ilist_chunks2_it(gpu, gstate.world.gpu_busy_chunks);
		!ilist_chunks2_end_p(gpu); ilist_chunks2_next(gpu)) {
		assert(ilist_chunks2_ref(gpu)->reference_count == 2);
		result.gpu++;
	}
	assert(result.gpu > 0 && result.gpu <= result.chunks);
#ifdef _WIN32
	result.heap = pc_heap_stats();
	assert(result.heap.valid);
#endif
	return result;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	fprintf(stderr, "respawn lifetime fixture start\n");
	assert(clin_init() && svin_init());
	blocks_init();
	items_init();
	recipe_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	test_host_same_world_single_anchor();
	clin_process(&(struct client_rpc) {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD, .local_entity = 0,
		},
	});
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	for(unsigned island = 0; island < 3; island++)
		for(int z = -2; z <= 2; z++)
			for(int x = -2; x <= 2; x++)
				split_populate(s, island * 64 + x, z);
	while(dict_server_chunks_size(s->world.chunks)) {
		dict_server_chunks_it_t it;
		dict_server_chunks_it(it, s->world.chunks);
		int64_t id = dict_server_chunks_ref(it)->key;
		assert(server_world_save_chunk(
			&s->world, true, S_CHUNK_X(id), S_CHUNK_Z(id)));
	}
	/* Warm destination disk reads before the heap baseline. Measured on Windows:
	 * these retain 115 heap allocations once, unchanged by subsequent reads. */
	struct worldgen* generator = s->world.generator;
	s->world.generator = NULL;
	for(unsigned island = 1; island < 3; island++) {
		struct server_chunk* c;
		assert(server_world_load_chunk(&s->world, island * 64, 0, &c));
		assert(server_world_save_chunk(&s->world, true, island * 64, 0));
	}
	/* Disk-only near streaming. Explicitly hold the transport epoch so the
	 * production loading path runs without simulation/mob/random-tick noise. */
	s->input_ack_pending = 1;
	s->input_epoch_counter = 1;
	s->player.finished_loading = true;
	clin_test_set_rpc_observer(drain_before_send, NULL);
	for(unsigned n = 0; n < 180; n++) tick(s);
	assert(server_local_test_close_active_inventory(s));
	clin_update_drain();
	install_far(s);
	struct census baseline = census(s);
	assert(baseline.sections > 0 && baseline.gpu > 0);
	printf("baseline sections=%zu chunks=%zu server=%zu gpu=%zu mem2=%zu",
		baseline.sections, baseline.chunks, baseline.server_chunks,
		baseline.gpu, baseline.mem2);
#ifdef _WIN32
	printf(" heap=%zu ws=%zu", baseline.heap.blocks, baseline.heap.working_set);
#endif
	puts("");
	for(unsigned death = 1; death <= 2; death++) {
		destination = death * 64;
		s->input_ack_pending = 0;
		s->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s->player.spawn_x = destination * CHUNK_SIZE;
		s->player.spawn_y = 4;
		s->player.spawn_z = 0;
		assert(server_local_set_player_health(s, 0));
		assert(server_local_respawn_request(s));
		assert(s->world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
		/* A transient media error must still retry and complete. Doing this
		 * on both respawns also exercises the per-respawn budget reset. */
		struct server_chunk* dirty = dict_server_chunks_get(
			s->world.chunks, S_CHUNK_ID((death - 1) * 64, 0));
		assert(dirty);
		dirty->modified = true;
		/* Native no-op saves now skip I/O; inject into a real payload edit. */
		dirty->ids[0] = dirty->ids[0] == 1 ? 0 : 1;
		unsigned failures_before = g_evict_stuck;
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		check_departed = true;
		new_columns = 0;
		for(unsigned n = 0; n < 180; n++) tick(s);
		assert(g_evict_stuck == failures_before + 1);
		assert(s->input_ack_pending
			&& "test must retain the presentation ACK barrier");
		assert(new_columns && !departed_client());
		assert(dict_far_sent_size(s->world.far_sent) == 0);
		install_far(s);
		struct census after = census(s);
		printf("respawn %u sections=%zu chunks=%zu server=%zu gpu=%zu mem2=%zu",
			death, after.sections, after.chunks, after.server_chunks,
			after.gpu, after.mem2);
#ifdef _WIN32
		printf(" heap=%zu ws=%zu", after.heap.blocks, after.heap.working_set);
#endif
		puts("");
		assert(after.sections == baseline.sections);
		assert(after.chunks == baseline.chunks);
		assert(after.server_chunks == baseline.server_chunks);
		assert(after.gpu == baseline.gpu && after.mem2 == baseline.mem2);
		for(unsigned v = 0; v < 2; v++) {
			assert(after.view_count[v] == baseline.view_count[v]);
			assert(after.view_capacity[v] == baseline.view_capacity[v]);
		}
		assert(s->world.current_tick == 0);
#ifdef _WIN32
		/* CRT/file-cache bookkeeping may retain up to 8 busy heap allocations;
		 * chunk, MEM2 and union counts have ZERO tolerance. */
		assert(after.heap.blocks <= baseline.heap.blocks + 8);
#endif
	}
	/* Same-center respawn: the existing collision center must not exempt a
	 * permanently failed departed-chunk drain from terminal load failure. */
	check_departed = false;
	s->input_ack_pending = 0;
	s->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(s, 0));
	assert(server_local_respawn_request(s));
	struct server_chunk* dirty = NULL;
	assert(server_world_load_chunk(&s->world, 64, 0, &dirty));
	dirty->ids[0] = BLOCK_BRICKS;
	dirty->modified = true;
	assert(server_world_is_chunk_sent(&s->world, destination, 0));
	unsigned failures_before = g_evict_stuck;
	unsigned handoffs_before = input_handoffs;
	unsigned failed_at;
	for(failed_at = 0; failed_at < 80 && !s->load_failure_terminal; failed_at++) {
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		tick(s);
	}
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_NONE);
	assert(s->load_failure_terminal && gstate.world_load_failed);
	assert(g_evict_stuck == failures_before + 3);
	assert(load_failures == 1 && input_handoffs == handoffs_before);
	assert(!s->player.finished_loading && !gstate.world_loaded);
	assert(s->world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(server_world_is_chunk_sent(&s->world, destination, 0));
	assert(server_world_is_chunk_loaded(&s->world, 64, 0));
	assert(dirty->modified && dirty->ids[0] == BLOCK_BRICKS);
	for(unsigned n = 0; n < 40; n++) tick(s);
	assert(g_evict_stuck == failures_before + 3 && load_failures == 1);
	assert(input_handoffs == handoffs_before && s->world.current_tick == 0);
	printf("PASS persistent respawn save failure: 3 attempts, terminal at "
		"update %u despite ready center; dirty owner retained; estk/wl_failed "
		"telemetry latched; no handoff or simulation\n", failed_at);
	clin_test_set_rpc_observer(NULL, NULL);
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	client_session_clear_windows(&gstate.players[0]);
	free(gstate.players[0].local_player);
	dict_entity_clear(gstate.entities);
	s->world.generator = generator;
	split_destroy(s);
	puts("PASS two respawns: departed ring retired before replacement, "
		 "heap and GPU union bounded");
	return 0;
}
