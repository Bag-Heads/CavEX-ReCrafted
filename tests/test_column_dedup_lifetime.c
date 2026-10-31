/* Copyright (c) 2026 CaveWii_v2 contributors
 * Real dictionary, transport, copied install, worker and GPU-union ownership.
 * Backing counters count physical destruction, independently of residency.
 */
#include "support/split_residency_fixture.h"
#include "game/game_state.h"
#include "item/recipe.h"
#include "config.h"

static void assert_empty(void) {
	uint32_t created, destroyed, references;
	column_backing_test_counts(&created, &destroyed, &references);
	assert(created > 0 && created == destroyed && references == 0);
}

static void client_start(void) {
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD}});
}

static void client_stop(void) {
	clin_drain_discard();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player);
	gstate.players[0].local_player = NULL;
}

static struct chunk_rpc_slot* snapshot(struct server_chunk* sc) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot && !slot->backing);
	clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, sizeof(slot->grass_i));
	memset(slot->grass_j, 0, sizeof(slot->grass_j));
	slot->piston_moving.count = 0;
	return slot;
}

static void publish(struct chunk_rpc_slot* slot, int x, int z) {
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK,
		.payload.chunk = {.x = x * CHUNK_SIZE, .z = z * CHUNK_SIZE,
			.sx = CHUNK_SIZE, .sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
}

static bool install(struct chunk_rpc_slot* slot, int x, int z) {
	return world_build_full_section_leased(&gstate.world, x * 16, z * 16,
		slot->ids, slot->metadata, slot->lighting_sky, slot->lighting_torch,
		slot->heightmap, slot->grass_i, slot->grass_j, slot->backing);
}

static void test_queued_rehash_reload_worker_gpu(void) {
	client_start();
	struct server_local* s = split_fixture();
	struct server_chunk* sc;
	size_t allocations = mem2_stat_alloc_calls();
	struct server_world_light_queue_stats lq_before, lq_after;
	server_world_test_light_queue_stats(&s->worlds[0], &lq_before);
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	server_world_test_light_queue_stats(&s->worlds[0], &lq_after);
	/* G9X-5a: the lazy light queue pages (3,584 B each, one mem2 alloc per
	 * page) are allocated on first load; count chunk planes only. */
	const size_t light_page_bytes = 3584;
	assert(lq_after.resident_bytes >= lq_before.resident_bytes
		&& (lq_after.resident_bytes - lq_before.resident_bytes) % light_page_bytes == 0);
	size_t light_pages = (lq_after.resident_bytes - lq_before.resident_bytes) / light_page_bytes;
	/* PC keeps five planes; the Wii packs them into one MEM2_POOL_COLUMN slot. */
	assert(mem2_stat_alloc_calls() - allocations - light_pages == 5);
	uint32_t live, shared;
	column_backing_get_stats(&live, &shared);
	assert(live == 1 && shared == 0);
	struct column_backing* old = sc->backing;
	uintptr_t dictionary_address = (uintptr_t)sc;
	assert(old && column_backing_references(old) == 1);
	assert(old->ids == sc->ids && old->metadata == sc->metadata
		&& old->lighting_sky == sc->lighting_sky
		&& old->lighting_torch == sc->lighting_torch
		&& old->heightmap == sc->heightmap);
	uint32_t instance = old->world_instance, generation = old->generation;
	struct chunk_rpc_slot* queued = snapshot(sc);
	assert(column_backing_references(old) == 2);
	column_backing_get_stats(&live, &shared);
	assert(live == 1 && shared == 1);
	publish(queued, 0, 0); // deliberately do not consume yet
	for(int x = 1; x <= 64; x++)
		assert(server_world_load_chunk(&s->worlds[0], x, 0, &sc));
	sc = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert((uintptr_t)sc != dictionary_address); // actual dictionary relocation
	assert(sc->backing == old && sc->ids == old->ids);
	assert(!memcmp(queued->ids, old->ids, sizeof(queued->ids)));
	assert(server_world_save_chunk(&s->worlds[0], true, 0, 0));
	assert(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0));
	assert(column_backing_references(old) == 1); // RPC only, no simulation owner
	column_backing_get_stats(&live, &shared);
	assert(live == 65 && shared == 0);
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	struct column_backing* fresh = sc->backing;
	assert(fresh != old && fresh->generation != generation);
	assert(column_backing_matches(old, instance, WORLD_DIM_OVERWORLD, generation));
	assert(!column_backing_matches(fresh, instance, WORLD_DIM_OVERWORLD, generation));
	assert(!column_backing_matches(old, instance, WORLD_DIM_NETHER, generation));
	clin_update_drain(); // the old payload may still install its COPY
	assert(column_backing_references(old) == COLUMN_HEIGHT);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 0, 0);
	assert(c && c->backing == old && c->ownership == LOCAL_SHARED_VIEW);
	assert(!c->blocks && c->view);
	assert(c->blocks != old->ids && chunk_get_block(c, 0, 0, 0).type == old->ids[0]);
	uint8_t original = old->ids[0];
	chunk_set_block_raw(c, 0, 0, 0, (struct block_data){.type = BLOCK_GLASS});
	assert(old->ids[0] == original); // client mutations still own a private copy

	/* Failure at each replacement shell must release every partial lease and
	 * preserve the complete old section. No transport lease can be stranded. */
	for(unsigned fail = 1; fail <= COLUMN_HEIGHT; fail++) {
		struct chunk_rpc_slot* slot = snapshot(sc);
		chunk_test_fail_alloc_after(fail);
		assert(!install(slot, 0, 0));
		clin_chunk_slot_release(slot);
		assert(world_find_chunk(&gstate.world, 0, 0, 0) == c);
		assert(column_backing_references(old) == COLUMN_HEIGHT);
		assert(column_backing_references(fresh) == 1);
	}
	/* Dictionary insertion failure after all eight shells acquired leases. */
	struct world empty;
	world_create(&empty);
	size_t fill = (size_t)empty.sections->upper_limit - 1;
	for(size_t i = 0; i < fill; i++) {
		struct world_section* section = dict_wsection_safe_get(empty.sections, 100 + (int64_t)i);
		memset(section, 0, sizeof(*section));
	}
	struct chunk_rpc_slot* slot = snapshot(sc);
	world_test_fail_section_dict_realloc_after(1);
	assert(!world_build_full_section_leased(&empty, 0, 0,
		slot->ids, slot->metadata, slot->lighting_sky, slot->lighting_torch,
		slot->heightmap, slot->grass_i, slot->grass_j, slot->backing));
	world_test_fail_section_dict_realloc_after(0);
	clin_chunk_slot_release(slot);
	world_destroy(&empty);
	assert(column_backing_references(fresh) == 1);

	assert(chunk_mesher_init());
	assert(chunk_mesher_send(c));
	c->mesh_in_flight = true;
	/* Both views see the same column, then hand ONE union to the GPU. Worker
	 * completion is deliberately not consumed until after GPU retirement. */
	struct camera camera = {.x = 8, .y = 8, .z = 8};
	glm_mat4_identity(camera.view);
	for(unsigned p = 0; p < 6; p++) camera.frustum_planes[p][3] = 1;
	struct camera* views[] = {&camera, &camera};
	gstate.config.fog_distance = 4096;
	assert(world_pre_render_views(&gstate.world, views, 2));
	assert(gstate.world.render_views[0].count > 0);
	assert(gstate.world.render_views[0].count == gstate.world.render_views[1].count);
	world_render_completed(&gstate.world, true);
	world_unload_all(&gstate.world);
	assert(column_backing_references(old) > 1); // GPU union and queued worker
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	assert(column_backing_references(old) == 1); // only the worker's shell
	uint32_t made, freed_before, refs;
	column_backing_test_counts(&made, &freed_before, &refs);
	ptime_t start = time_get();
	while(chunk_mesher_pending()) {
		chunk_mesher_receive();
		assert(time_diff_s(start, time_get()) < 30);
		if(chunk_mesher_pending()) thread_msleep(1);
	}
	uint32_t freed_after;
	column_backing_test_counts(&made, &freed_after, &refs);
	assert(freed_after == freed_before + 1); // exactly one final destruction
	chunk_mesher_shutdown();
	client_stop();
	split_destroy(s);
	assert_empty();
	puts("PASS queued RPC + dictionary rehash + reload + copied provider + partial OOM + delayed two-view/GPU/mesher lifetime");
}

static void* retain_release(void* arg) {
	struct column_backing* b = arg;
	for(unsigned i = 0; i < 10000; i++) {
		column_backing_ref(b);
		assert(column_backing_references(b) >= 2);
		column_backing_unref(b);
	}
	column_backing_unref(b); // thread's entry lease
	return NULL;
}

static void test_shutdown_queued_leases(void) {
	client_start();
	struct server_local* s = split_fixture();
	struct server_chunk* sc;
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	publish(snapshot(sc), 0, 0);
	clin_update_drain();
	assert(chunk_mesher_init());
	uint32_t built = chunk_mesher_meshed_count();
	struct chunk* c = world_find_chunk(&gstate.world, 0, 0, 0);
	assert(chunk_mesher_send(c));
	c->mesh_in_flight = true;
	ptime_t start = time_get();
	while(chunk_mesher_meshed_count() == built) {
		assert(time_diff_s(start, time_get()) < 30);
		thread_msleep(1);
	}
	/* One completed result, one dequeued/held job, six queued requests.
	 * All eight tokens are occupied; shutdown cannot borrow a free token. */
	chunk_mesher_test_hold_worker(true);
	for(unsigned i = 1; i < CHUNK_MESHER_QLENGTH; i++) {
		c = world_find_chunk(&gstate.world, 0, (int)i * 16, 0);
		assert(c && chunk_mesher_send(c));
		c->mesh_in_flight = true;
	}
	while(!chunk_mesher_test_worker_held()) {
		assert(time_diff_s(start, time_get()) < 30);
		thread_msleep(1);
	}
	assert(chunk_mesher_pending() == CHUNK_MESHER_QLENGTH);
	struct column_backing* b = sc->backing;
	world_unload_all(&gstate.world);
	assert(column_backing_references(b) == 1 + CHUNK_MESHER_QLENGTH);
	split_destroy(s); // only the eight job shells now own this backing
	chunk_mesher_shutdown();
	assert(chunk_mesher_pending() == 0);
	assert_empty();
	chunk_mesher_test_hold_worker(false);
	chunk_mesher_receive();
	chunk_mesher_shutdown();
	assert(chunk_mesher_init()); // channels and tokens support a new session
	chunk_mesher_shutdown();
	client_stop();
	assert_empty();
	puts("PASS shutdown: result + dequeued job + six queued requests retired on main; zero leases; reinit");
}

static void test_backing_failures_and_dimension(void) {
	client_start();
	struct server_local* s = split_fixture();
	struct server_chunk* sc;
	{
		uint32_t made, freed, refs, made_after, freed_after, refs_after;
		column_backing_test_counts(&made, &freed, &refs);
		size_t arrays = mem2_stat_alloc_calls() - mem2_stat_free_calls();
		struct server_world_light_queue_stats lq_a, lq_b;
		server_world_test_light_queue_stats(&s->worlds[0], &lq_a);
		column_backing_test_fail_next();
		assert(!server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
		assert(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0));
		server_world_test_light_queue_stats(&s->worlds[0], &lq_b);
		/* lazy light pages (3,584 B, sticky until world teardown) are not planes */
		assert(lq_b.resident_bytes >= lq_a.resident_bytes
			&& (lq_b.resident_bytes - lq_a.resident_bytes) % 3584 == 0);
		assert(mem2_stat_alloc_calls() - mem2_stat_free_calls()
			- (lq_b.resident_bytes - lq_a.resident_bytes) / 3584 == arrays);
		column_backing_test_counts(&made_after, &freed_after, &refs_after);
		assert(made == made_after && freed == freed_after && refs == refs_after);
	}
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	struct column_backing* b = sc->backing;
	/* Repeat adoption failure on disk reload while an older generation is
	 * leased by concurrent readers. A failed new owner cannot destroy it. */
	column_backing_ref(b);
	assert(server_world_save_chunk(&s->worlds[0], true, 0, 0));
	struct thread readers[2];
	for(unsigned i = 0; i < 2; i++) {
		column_backing_ref(b);
		assert(thread_create(&readers[i], retain_release, b, 64));
	}
	{
		column_backing_test_fail_next();
		assert(!server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
		assert(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0));
		assert(b->ids[0] == BLOCK_BEDROCK);
	}
	for(unsigned i = 0; i < 2; i++) thread_join(&readers[i]);
	assert(column_backing_references(b) == 1);
	column_backing_unref(b);
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	b = sc->backing;
	publish(snapshot(sc), 0, 0);
	clin_update_drain();
	uint32_t count = column_backing_references(b);
	struct chunk* c = world_find_chunk(&gstate.world, 0, 0, 0);
	clin_process(&(struct client_rpc){.type = CRPC_BEGIN_WORLD_SWITCH});
	assert(world_find_chunk(&gstate.world, 0, 0, 0) == c);
	assert(column_backing_references(b) == count);
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {0.5f, 5.62f, 0.5f},
			.input_epoch = 1, .oxygen = MAX_OXYGEN, .fresh_body = false}});
	assert(world_find_chunk(&gstate.world, 0, 0, 0) == c);
	assert(column_backing_references(b) == count); // cancelled BEGIN keeps owners
	clin_process(&(struct client_rpc){.type = CRPC_BEGIN_WORLD_SWITCH});
	struct chunk_rpc_slot* abandoned = snapshot(sc);
	publish(abandoned, 0, 0);
	clin_drain_discard(); // cancellation/shutdown of a queued local install
	assert(column_backing_references(b) == count);
	column_backing_ref(b); // explicit lease outlives the entire world instance
	uint32_t instance = b->world_instance, generation = b->generation;
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_NETHER}});
	assert(column_backing_references(b) == 2); // dictionary and explicit lease
	server_world_discard(&s->worlds[0]);
	assert(column_backing_references(b) == 1);
	string_t path;
	string_init_set_str(path, split_path);
	assert(server_world_create_with_populate_scratch(&s->worlds[0], path,
		WORLD_DIM_NETHER, 12345, true, s->world_populate_scratch));
	string_clear(path);
	assert(s->worlds[0].backing_world_instance != instance);
	assert(!column_backing_matches(b, s->worlds[0].backing_world_instance,
		WORLD_DIM_NETHER, generation));
	column_backing_unref(b);
	client_stop();
	split_destroy(s);
	assert_empty();
	puts("PASS allocation failure unwind + cancelled BEGIN + RESET + world-instance stale tags");
}

static unsigned anchor_delivered[2];
static void drain_before_send(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	if(rpc->type == CRPC_CHUNK) {
		assert(rpc->payload.chunk.slot->backing);
		anchor_delivered[rpc->payload.chunk.x >= 512]++;
	}
}

static void tick(struct server_local* s) {
	server_local_test_update_once(s);
	clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
}

static void test_two_anchors_and_respawns(void) {
	client_start();
	struct server_local* s = split_fixture();
	assert(server_local_set_anchor_sep(s, 1024));
	split_populate(s, 0, 0);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	s->far_view_distance = MAX_VIEW_DISTANCE;
	s->player.finished_loading = true;
	s->input_ack_pending = s->input_epoch_counter = 1;
	clin_test_set_rpc_observer(drain_before_send, NULL);
	for(unsigned n = 0; n < 24; n++) tick(s);
	assert(anchor_delivered[0] && anchor_delivered[1]);
	/* Keep an old generation leased across both REAL respawn transactions.
	 * Its presence must never prevent the old simulation ring from retiring. */
	struct server_chunk* sc = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(sc && sc->backing);
	struct column_backing* departed = sc->backing;
	column_backing_ref(departed);
	for(unsigned death = 1; death <= 2; death++) {
		s->input_ack_pending = 0;
		s->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s->player.spawn_x = (int)death * 2048;
		s->player.spawn_y = 4;
		s->player.spawn_z = 0;
		assert(server_local_set_player_health(s, 0));
		assert(server_local_respawn_request(s));
		unsigned n;
		for(n = 0; n < 600; n++) {
			tick(s);
			if(s->input_ack_pending) break;
		}
		assert(n < 600 && !s->load_failure_terminal);
		assert(!server_world_is_chunk_loaded(&s->worlds[0], 0, 0));
		assert(!world_find_chunk(&gstate.world, 0, 0, 0));
		assert(column_backing_references(departed) == 1);
		printf("PASS respawn %u: old simulation/client rings retired with old backing still leased\n", death);
	}
	column_backing_unref(departed);
	clin_test_set_rpc_observer(NULL, NULL);
	client_stop();
	split_destroy(s);
	assert_empty();
	puts("PASS two-anchor scheduler + two respawns: all backing references return to zero");
}

static struct column_light_publication* queued_light;
static void remember_light(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_LOCAL_SETTLED_LIGHT)
		queued_light = rpc->payload.settled_light;
}

static void test_publication_delayed_owners(void) {
	client_start();
	struct server_local* s = split_fixture();
	struct server_chunk* sc;
	assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc));
	static const int delta[7][3] = {
		{0,0,0},{0,1,0},{0,-1,0},{-1,0,0},{1,0,0},{0,0,1},{0,0,-1}};
	for(unsigned n = 0; n < 7; n++)
		assert(server_world_set_block(&s->worlds[0], 8 + delta[n][0],
			8 + delta[n][1], 8 + delta[n][2], (struct block_data){.type = BLOCK_STONE}));
	assert(server_world_light_drain(&s->worlds[0]));
	clin_drain_discard();
	publish(snapshot(sc), 0, 0);
	clin_update_drain();
	struct column_backing* b = sc->backing;
	struct chunk* c = world_find_chunk(&gstate.world, 8, 8, 8);
	assert(c && c->ownership == LOCAL_SHARED_VIEW && chunk_mesher_init());
	chunk_mesher_test_hold_worker(true);
	assert(chunk_mesher_send(c));
	c->mesh_in_flight = true;
	ptime_t start = time_get();
	while(!chunk_mesher_test_worker_held()) {
		assert(time_diff_s(start, time_get()) < 30);
		thread_msleep(1);
	}
	struct camera camera = {.x = 8, .y = 8, .z = 8};
	glm_mat4_identity(camera.view);
	for(unsigned n = 0; n < 6; n++) camera.frustum_planes[n][3] = 1;
	struct camera* views[] = {&camera, &camera};
	gstate.config.fog_distance = 4096;
	assert(world_pre_render_views(&gstate.world, views, 2));
	world_render_completed(&gstate.world, true);
	clin_test_set_rpc_observer(remember_light, NULL);
	assert(server_world_set_block(&s->worlds[0], 8, 8, 8,
		(struct block_data){.type = BLOCK_STONE, .metadata = 1}));
	assert(server_world_light_drain(&s->worlds[0]) && queued_light);
	struct column_light_publication frozen = *queued_light;
	assert(server_world_set_block_metadata_quiet(&s->worlds[0], 9, 8, 8, 3));
	assert(!memcmp(&frozen, queued_light, sizeof(frozen))); /* immutable bytes */
	struct column_light_stats before, after;
	column_light_get_stats(&before);
	clin_update_drain();
	assert(gstate.world.settled_light);
	world_update_lighting(&gstate.world); /* stale version takes baseline */
	column_light_get_stats(&after);
	assert(after.hits == before.hits && after.live == 0);
	queued_light = NULL;
	/* Another actual producer publication is discarded while all owners live. */
	assert(server_world_set_block(&s->worlds[0], 8, 8, 8,
		(struct block_data){.type = BLOCK_STONE, .metadata = 2}));
	assert(server_world_light_drain(&s->worlds[0]) && queued_light);
	clin_drain_discard();
	column_light_get_stats(&after);
	assert(after.live == 0 && after.bytes == 0);
	queued_light = NULL;
	clin_test_set_rpc_observer(NULL, NULL);
	world_unload_all(&gstate.world);
	split_destroy(s);
	assert(column_backing_references(b) > 1); /* GPU union and held worker */
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	assert(column_backing_references(b) == 1);
	chunk_mesher_test_hold_worker(false);
	while(chunk_mesher_pending()) {
		chunk_mesher_receive();
		assert(time_diff_s(start, time_get()) < 30);
		if(chunk_mesher_pending()) thread_msleep(1);
	}
	chunk_mesher_shutdown();
	client_stop();
	assert_empty();
	puts("PASS A ON: immutable queued bytes + later server mutation + rejection/discard + two-view GPU/held worker retirement");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	test_queued_rehash_reload_worker_gpu();
	test_shutdown_queued_leases();
	test_backing_failures_and_dimension();
	test_two_anchors_and_respawns();
	struct config config = {.root = json_value_init_object()};
	assert(config.root && !config_g9_settled_light_reuse(&config));
	assert(config_set_int(&config, "experimental.g9_settled_light_reuse_int", 1));
	column_light_configure(config_g9_settled_light_reuse(&config));
	config_destroy(&config);
	test_publication_delayed_owners();
	test_queued_rehash_reload_worker_gpu();
	test_shutdown_queued_leases();
	test_backing_failures_and_dimension();
	column_light_configure(false);
	return 0;
}
