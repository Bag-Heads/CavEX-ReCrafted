/* Copyright (c) 2026 CaveWii_v2 contributors
 * Real server/client FIFO handshake, production 256-page pressure and NULLs.
 * Only the initial WORLD_RESET is fixture setup; all recovery RESETs and
 * PLAYER_POS markers are emitted by the production server update. */
#include "support/split_residency_fixture.h"
#include "column_view.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/recipe.h"

static bool fail_private;
static unsigned failures, resets, markers, terminal;
static uint32_t update_done;
void* __real_mem2_alloc(size_t);
void* __wrap_mem2_alloc(size_t bytes) {
	if(bytes == COLUMN_VIEW_PRIVATE_BYTES && fail_private) {
		fail_private = false;
		failures++;
		return NULL;
	}
	return __real_mem2_alloc(bytes);
}
static void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_STREAM_RESET) resets++;
	if(rpc->type == CRPC_PLAYER_POS && rpc->payload.player_pos.input_epoch) markers++;
	if(rpc->type == CRPC_LOAD_FAILED) terminal++;
}
static void* update_server(void* ptr) {
	server_local_test_update_once(ptr);
	__atomic_store_n(&update_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void step(struct server_local* s) {
	struct thread worker;
	update_done = 0;
	assert(thread_create(&worker, update_server, s, 64));
	ptime_t start = time_get();
	while(!__atomic_load_n(&update_done, __ATOMIC_ACQUIRE)) {
		clin_update_drain();
		assert(time_diff_s(start, time_get()) < 10);
		thread_msleep(1);
	}
	thread_join(&worker);
	clin_update_drain();
}
static void stream_to_marker(struct server_local* s) {
	unsigned before = markers;
	for(unsigned i = 0; i < 1000 && markers == before; i++) step(s);
	assert(markers == before + 1 && s->input_ack_pending);
	assert(gstate.world_loaded && gstate.world_switch_pending);
	assert(!column_view_recovery_pending());
	assert(world_find_chunk(&gstate.world, 0, 4, 0));
}
static void acknowledge(struct server_local* s) {
	/* Exercise the real 30 s readiness fallback (no worker meshes in this rig).
	 * A valid, rebuilt world can ACK; an invalid/empty world below cannot. */
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	step(s);
	assert(!s->input_ack_pending && !s->stream_recovery_epoch);
	assert(gstate.world_loaded && !gstate.world_switch_pending);
	assert(!column_view_recovery_pending() && column_view_pool_present());
	struct chunk* c = world_find_chunk(&gstate.world, 0, 4, 0);
	assert(c && c->view);
	struct block_data b = chunk_get_block(c, 0, 4, 0);
	uint8_t light = (b.torch_light << 4) | (b.sky_light ^ 1);
	chunk_set_light(c, 0, 4, 0, light);
	assert(chunk_get_block(c, 0, 4, 0).sky_light == (b.sky_light ^ 1));
	chunk_set_light(c, 0, 4, 0, (b.torch_light << 4) | b.sky_light);
}
static void exhaust_storage(struct server_local* s) {
	struct column_view_stats before;
	column_view_get_stats(&before);
	/* Sixteen partially divergent snapshots consume every physical pool slot.
	 * Two base-equal columns then need private storage without releasing pages. */
	struct chunk* victims[2];
	for(unsigned n = 0; n < 18; n++) {
		struct server_chunk* sc = dict_server_chunks_get(
			s->world.chunks, S_CHUNK_ID(32 + n, 0));
		server_world_begin_update(&s->world);
		if(!sc) assert(server_world_load_chunk(&s->world, 32 + n, 0, &sc));
		struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
		assert(slot);
		clin_chunk_slot_copy_local(slot, sc);
		if(n >= 2) {
			for(unsigned i = 0; i < COLUMN_VIEW_POOL_PAGES / 16
				* COLUMN_VIEW_PAGE_CELLS; i++) slot->ids[i] ^= 1;
		}
		memset(slot->grass_i, 0, 256); memset(slot->grass_j, 0, 256);
		slot->piston_moving.count = 0;
		clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK, .payload.chunk = {
			.x = (32 + n) * 16, .sx = 16, .sy = 128, .sz = 16, .slot = slot}});
		clin_update_drain();
		if(n < 2) victims[n] = world_find_chunk(&gstate.world, (32 + n) * 16, 0, 0);
	}
	struct column_view_stats stats;
	column_view_get_stats(&stats);
	assert(stats.pages == COLUMN_VIEW_POOL_PAGES && stats.fallback_live == 0);
	for(unsigned i = 0; i < 2; i++) {
		struct block_data b = chunk_get_block(victims[i], 0, 0, 0);
		fail_private = true;
		chunk_set_light(victims[i], 0, 0, 0, (b.torch_light << 4) | (b.sky_light ^ 1));
		assert(!fail_private);
		assert(column_view_recovery_pending() == (i == 1));
	}
	clin_recover_column_storage();
	clin_recover_column_storage(); // accepting the same failure twice is harmless
	column_view_get_stats(&stats);
	assert(stats.peak_pages == COLUMN_VIEW_POOL_PAGES && stats.peak_pages >= before.peak_pages);
	assert(stats.page_failures >= before.page_failures + 4);
	assert(stats.emergency_uses == before.emergency_uses + 1);
	assert(stats.recovery_entries == before.recovery_entries + 1);
	assert(stats.recovery_cycles == before.recovery_cycles + 1);
	assert(stats.recovery_expiries == before.recovery_expiries);
	assert(stats.recovery_wait_max_us == before.recovery_wait_max_us);
	assert(column_view_recovery_pending());
	assert(!world_loaded_chunks(&gstate.world) && !gstate.world_loaded);
	assert(gstate.world_switch_pending);
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
	struct server_local* s = split_fixture();
	s->paused = true; // preserve simulation and owner state while stepping transport
	stream_to_marker(s);
	acknowledge(s);
	gstate.paused = true;
	screen_set(&screen_pause);
	struct entity* player = gstate.players[0].local_player;
	struct window_container* inventory = gstate.players[0].windows[WINDOWC_INVENTORY];
	exhaust_storage(s);
	stream_to_marker(s);
	assert(resets == 1 && s->input_handoff_retry_count == 1);
	uint32_t failed_epoch = s->input_ack_pending;
	/* The reported hole: another OOM AFTER recovery PLAYER_POS, BEFORE ACK. */
	exhaust_storage(s);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness(); // must NACK, even after the fallback deadline
	assert(gstate.world_switch_pending && !gstate.world_loaded);
	assert(s->input_ack_pending == failed_epoch);
	stream_to_marker(s);
	assert(resets == 2 && s->input_ack_pending != failed_epoch);
	acknowledge(s);
	assert(s->input_handoff_retry_count == 2);
	assert(gstate.paused && gstate.players[0].current_screen == &screen_pause);
	assert(gstate.players[0].local_player == player);
	assert(gstate.players[0].windows[WINDOWC_INVENTORY] == inventory);
	puts("PASS second OOM between recovery PLAYER_POS and ACK: NACK -> RESET -> rebuilt -> ACK; storage enabled, owners retained");
	for(unsigned i = 1; i < INPUT_HANDOFF_CLEAN_TICKS; i++) step(s);
	assert(s->input_handoff_retry_count == 2);
	step(s);
	assert(s->input_handoff_retry_count == 0 && !s->input_handoff_clean_ticks);
	for(unsigned attempt = 1; attempt <= INPUT_HANDOFF_RETRY_MAX; attempt++) {
		exhaust_storage(s);
		stream_to_marker(s);
		acknowledge(s);
		assert(s->input_handoff_retry_count == attempt);
	}
	unsigned before = resets;
	exhaust_storage(s);
	step(s);
	assert(resets == before && terminal == 1 && s->load_failure_terminal);
	assert(s->input_handoff_retry_count == INPUT_HANDOFF_RETRY_MAX + 1);
	assert(gstate.world_load_failed && !gstate.world_loaded && gstate.world_switch_pending);
	assert(failures == 12);
	puts("PASS recovery budget survives successful ACKs, clears only at 100 clean updates, and reports terminal exhaustion on episode 4");
	clin_test_set_consume_observer(NULL, NULL);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	world_destroy(&gstate.world);
	split_destroy(s);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player);
	gstate.players[0].local_player = NULL;
	assert(column_view_pool_shutdown() && !column_view_pool_present());
	return 0;
}
