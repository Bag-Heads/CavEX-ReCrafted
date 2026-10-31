/* Copyright (c) 2026 CaveWii_v2 contributors
 * One process, one initialized transport: local -> remote -> local. Drives
 * real remote retention/inflate/promotion/cancellation without a live socket. */
#include "support/split_residency_fixture.h"
#include "column_observer.h"
#include "column_view.h"
#include "game/game_state.h"
#include "item/recipe.h"
#include "network/server_remote_internal.h"

static bool remote_path;
static unsigned remote_slots;
static unsigned gpu_waits;
void __real_gfx_wait_idle(void);
void __wrap_gfx_wait_idle(void) {
	assert(column_view_pool_present() && chunk_mesher_pending());
	gpu_waits++;
	__real_gfx_wait_idle();
}
void __real_column_backing_ref(struct column_backing*);
void __real_column_backing_unref(struct column_backing*);
uint32_t __real_column_backing_references(struct column_backing*);
bool __real_column_backing_matches(const struct column_backing*, uint32_t,
	enum world_dim, uint32_t);
void __wrap_column_backing_ref(struct column_backing* b) {
	assert(!remote_path || !b);
	__real_column_backing_ref(b);
}
void __wrap_column_backing_unref(struct column_backing* b) {
	assert(!remote_path || !b);
	__real_column_backing_unref(b);
}
uint32_t __wrap_column_backing_references(struct column_backing* b) {
	assert(!remote_path || !b);
	return __real_column_backing_references(b);
}
bool __wrap_column_backing_matches(const struct column_backing* b,
	uint32_t instance, enum world_dim dimension, uint32_t generation) {
	assert(!remote_path || !b);
	return __real_column_backing_matches(b, instance, dimension, generation);
}
static void slot_observer(int delta, const void* ptr) {
	const struct chunk_rpc_slot* slot = ptr;
	if(remote_path) {
		assert(!slot->backing);
		if(delta > 0) remote_slots++;
	}
}

static void queue_local(struct server_chunk* sc) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot && !slot->backing);
	clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, sizeof(slot->grass_i));
	memset(slot->grass_j, 0, sizeof(slot->grass_j));
	slot->piston_moving.count = 0;
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK,
		.payload.chunk = {.sx = 16, .sy = 128, .sz = 16, .slot = slot}});
}

static void views(void) {
	struct camera camera = {.x = 8, .y = 8, .z = 8};
	glm_mat4_identity(camera.view);
	for(unsigned i = 0; i < 6; i++) camera.frustum_planes[i][3] = 1;
	struct camera* cameras[] = {&camera, &camera};
	gstate.config.fog_distance = 4096;
	assert(world_pre_render_views(&gstate.world, cameras, 2));
	assert(gstate.world.render_views[0].count && gstate.world.render_views[1].count);
	world_render_completed(&gstate.world, true);
}

static void reset_client(void) {
	clin_drain_discard();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	assert(!world_loaded_chunks(&gstate.world));
	for(unsigned i = 0; i < WORLD_RENDER_VIEWS; i++)
		assert(!gstate.world.render_views[i].count);
	assert(ilist_chunks_empty_p(gstate.world.render));
	assert(ilist_chunks2_empty_p(gstate.world.gpu_busy_chunks));
}

static void retain_remote(struct server_remote_impl* remote, uint8_t id) {
	const size_t length = 32768 + 3 * 16384;
	uint8_t* data = calloc(length, 1);
	assert(data);
	memset(data, id, 32768);
	memset(data + 32768 + 2 * 16384, 0xFF, 16384);
	uLongf compressed = compressBound(length);
	struct mp_event event = {.type = MP_EVENT_MAP_CHUNK,
		.payload.map_chunk = {.sx = 16, .sy = 128, .sz = 16,
			.inflated_length = length, .compressed = malloc(compressed)}};
	assert(event.payload.map_chunk.compressed);
	assert(compress2(event.payload.map_chunk.compressed, &compressed,
		data, length, 1) == Z_OK);
	free(data);
	event.payload.map_chunk.compressed_length = compressed;
	assert(remote_retain_map_locked(remote, &event));
	assert(!event.payload.map_chunk.compressed); // real transfer of ownership
}

int main(void) {
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init() && chunk_mesher_init());
	clin_column_observe_slot = slot_observer;
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	reset_client();
	struct server_local* local = split_fixture();
	struct server_chunk* sc;
	assert(server_world_load_chunk(&local->world, 0, 0, &sc));
	queue_local(sc); clin_update_drain();
	struct chunk* old = world_find_chunk(&gstate.world, 0, 0, 0);
	assert(old && old->backing);
	uint32_t old_instance = old->backing->world_instance;
	chunk_mesher_test_hold_worker(true);
	assert(chunk_mesher_send(old)); old->mesh_in_flight = true;
	ptime_t start = time_get();
	while(!chunk_mesher_test_worker_held()) {
		assert(time_diff_s(start, time_get()) < 30);
		thread_msleep(1);
	}
	views();
	queue_local(sc); queue_local(sc); // queued local payloads at session exit
	reset_client(); // drain/discard both leases and retire both views
	split_destroy(local);
	uint32_t made, freed, refs;
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed + 1 && refs == 1); // delayed local worker only

	assert(column_view_pool_present() && !column_view_pool_shutdown());
	/* Stuck-worker double: the production drain must return at its deadline,
	 * retain the lease/pool, and refuse the provider transition. Main reports
	 * this failure through its existing telemetry dependency. */
	start = time_get();
	assert(!world_finish_local_teardown(&gstate.world));
	float elapsed = time_diff_s(start, time_get());
	assert(elapsed >= 0.5f && elapsed < 2.0f);
	assert(!gstate.remote_session);
	assert(chunk_mesher_pending() && column_view_pool_present());
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed + 1 && refs == 1);
	printf("PASS stuck worker: teardown refused after %.3f s; provider and storage retained\n", elapsed);
	/* Quiescent provider boundary: the pool cannot follow local worker refs
	 * into remote inflate. Receive the delayed local job before swapping. */
	chunk_mesher_test_hold_worker(false);
	assert(world_finish_local_teardown(&gstate.world));
	assert(gpu_waits == 2 && !column_view_pool_present());
	column_backing_test_counts(&made, &freed, &refs);
	assert(made == freed && refs == 0);
	gstate.remote_session = true;
	struct server_remote_impl* remote = calloc(1, sizeof(*remote));
	assert(remote);
	assert(thread_mutex_init(&remote->lock));
	remote->retained_byte_cap = 2 * 1024 * 1024;
	remote_path = true;
	thread_mutex_lock(&remote->lock);
	retain_remote(remote, BLOCK_GLASS);
	assert(remote_promotion_step_locked(remote));
	assert(remote->promotion.active && remote->promotion.slot
		&& !remote->promotion.slot->backing);
	assert(remote->promotion.stream.total_out > 0
		&& remote->promotion.stream.total_out < 81920);
	/* Same cancellation used by disconnect/dimension teardown, while inflate
	 * owns a slot. The next promotion must acquire a clean, unleased slot. */
	remote_chunks_clear_locked(remote);
	assert(!remote->promotion.active && !remote->promotion.slot);
	assert(!remote->retained_bytes && !remote->retained_entries);
	retain_remote(remote, BLOCK_STONE);
	for(unsigned i = 0; i < 4; i++) assert(remote_promotion_step_locked(remote));
	assert(!remote->promotion.active && world_loaded_chunks(&gstate.world) == 1);
	for(unsigned y = 0; y < 128; y += 16) {
		struct chunk* c = world_find_chunk(&gstate.world, 0, y, 0);
		assert(c && !c->backing && c->ownership == OWNED_VOXEL);
		assert(chunk_get_block(c, 0, 0, 0).type == BLOCK_STONE);
	}
	thread_mutex_unlock(&remote->lock);
	remote_path = false;
	uint32_t made2, freed2, refs2;
	column_backing_test_counts(&made2, &freed2, &refs2);
	assert(made2 == made && freed2 == freed && refs2 == refs && remote_slots == 2);
	assert(!column_view_pool_present());
	views();
	remote_path = true;
	thread_mutex_lock(&remote->lock);
	remote_chunks_clear_locked(remote);
	thread_mutex_unlock(&remote->lock);
	reset_client();
	remote_path = false;
	thread_mutex_destroy(&remote->lock); free(remote);
	gstate.remote_session = false;
	local = split_fixture();
	assert(server_world_load_chunk(&local->world, 0, 0, &sc));
	assert(sc->backing->world_instance != old_instance);
	queue_local(sc); clin_update_drain();
	assert(world_find_chunk(&gstate.world, 0, 0, 0)->backing == sc->backing);
	assert(column_view_pool_present());
	views(); reset_client(); split_destroy(local);
	chunk_mesher_shutdown();
	world_destroy(&gstate.world); dict_entity_clear(gstate.entities);
	client_session_clear_windows(&gstate.players[0]);
	free(gstate.players[0].local_player); gstate.players[0].local_player = NULL;
	column_backing_test_counts(&made2, &freed2, &refs2);
	assert(made2 == freed2 && refs2 == 0);
	/* All transport tokens, including cancelled promotions, are reusable. */
	struct chunk_rpc_slot* slots[16]; // existing transport pool: RPC_INBOX_SIZE
	for(unsigned i = 0; i < 16; i++) {
		slots[i] = clin_chunk_slot_try_acquire();
		assert(slots[i] && !slots[i]->backing);
	}
	assert(!clin_chunk_slot_try_acquire());
	for(unsigned i = 0; i < 16; i++) clin_chunk_slot_release(slots[i]);
	clin_column_observe_slot = NULL;
	puts("PASS local -> remote -> local: queued payload discard, promotion cancellation, "
		"delayed local mesh retired before swap; pool absent remotely; remote backing accesses=0; leases/views=0; all slots reusable");
	return 0;
}
