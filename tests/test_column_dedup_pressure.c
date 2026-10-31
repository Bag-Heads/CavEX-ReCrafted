/* Copyright (c) 2026 CaveWii_v2 contributors
 * Production 256-slot pool, real delayed RPCs and real NULL allocations.
 * No page-limit seam. Independent packed snapshots pin presentation values. */
#include "support/split_residency_fixture.h"
#include "column_view.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/recipe.h"
#include "lighting.h"
#include "tps_observer.h"

static struct thread_channel entered, proceed;
static uint32_t fail_alloc, hold_alloc;
static uint32_t hold_success;
static void* unused_success;
static unsigned successful_allocations;
static unsigned failures, recover_requests;
static uint32_t burst_fail_all;
void* __real_mem2_alloc(size_t);
void* __wrap_mem2_alloc(size_t bytes) {
	if(bytes == COLUMN_VIEW_PRIVATE_BYTES && __atomic_load_n(&burst_fail_all, __ATOMIC_ACQUIRE))
		return NULL;
	if(bytes == COLUMN_VIEW_PRIVATE_BYTES
	   && __atomic_exchange_n(&hold_success, 0, __ATOMIC_ACQ_REL)) {
		unused_success = __real_mem2_alloc(bytes);
		assert(unused_success);
		successful_allocations++;
		tchannel_send(&entered, &entered, true);
		void* token;
		assert(tchannel_receive(&proceed, &token, true));
		return unused_success;
	}
	if(bytes == COLUMN_VIEW_PRIVATE_BYTES
	   && __atomic_exchange_n(&fail_alloc, 0, __ATOMIC_ACQ_REL)) {
		failures++;
		if(__atomic_exchange_n(&hold_alloc, 0, __ATOMIC_ACQ_REL)) {
			tchannel_send(&entered, &entered, true);
			void* token;
			assert(tchannel_receive(&proceed, &token, true));
		}
		return NULL;
	}
	return __real_mem2_alloc(bytes);
}
void __real_mem2_free_sized(void*, size_t);
void __wrap_mem2_free_sized(void* data, size_t bytes) {
	bool unused = data && data == unused_success;
	__real_mem2_free_sized(data, bytes);
	if(unused) {
		unused_success = NULL;
		tchannel_send(&entered, &entered, true);
		void* token;
		assert(tchannel_receive(&proceed, &token, true));
	}
}
static struct column_view_stats stats(void) {
	struct column_view_stats s;
	column_view_get_stats(&s);
	return s;
}
static struct server_chunk* columns[18];
static struct chunk_rpc_slot* expected[18];
static unsigned column_x(unsigned n) { return (n == 1 ? 16 : n == 16 ? 1 : n) * 16; }
static struct chunk* client(unsigned n) {
	return world_find_chunk(&gstate.world, column_x(n), 64, 0);
}
static void queue(unsigned n, const struct chunk_rpc_slot* image) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, columns[n]);
	if(image) {
		memcpy(slot->ids, image->ids, 32768);
		memcpy(slot->metadata, image->metadata, 16384);
		memcpy(slot->lighting_sky, image->lighting_sky, 16384);
		memcpy(slot->lighting_torch, image->lighting_torch, 16384);
	}
	memset(slot->grass_i, 0, 256); memset(slot->grass_j, 0, 256);
	slot->piston_moving.count = 0;
	clin_rpc_send(&(struct client_rpc){.type = CRPC_CHUNK, .payload.chunk = {
		.x = column_x(n), .sx = 16, .sy = 128, .sz = 16, .slot = slot}});
}
static void compare(unsigned n) {
	struct column_view* v = client(n)->view;
	for(unsigned i = 0; i < 32768; i++) {
		struct block_data b = column_view_get(v, i);
		assert(b.type == expected[n]->ids[i]);
		assert(b.metadata == nibble_read(expected[n]->metadata, i));
		assert(b.sky_light == nibble_read(expected[n]->lighting_sky, i));
		assert(b.torch_light == nibble_read(expected[n]->lighting_torch, i));
	}
}
static void* writer(void* unused) {
	(void)unused;
	column_backing_set_cell(columns[0]->backing, COLUMN_IDS, 64, BLOCK_DIRT);
	return NULL;
}
static void capture_recovery(struct server_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == SRPC_STREAM_RECOVER) {
		assert(rpc->payload.stream_recover.epoch == 1);
		recover_requests++;
	}
}
static void fill(struct server_local* s) {
	for(unsigned n = 0; n < 18; n++) {
		server_world_begin_update(&s->world);
		assert(server_world_load_chunk(&s->world, column_x(n) / 16, 0, &columns[n]));
		struct server_chunk* sc = columns[n];
		memset(sc->ids, BLOCK_STONE, 32768);
		memset(sc->metadata, 0, 16384);
		memset(sc->lighting_sky, 0, 16384);
		memset(sc->lighting_torch, 0, 16384);
		expected[n] = calloc(1, sizeof(*expected[n]));
		assert(expected[n]);
		memset(expected[n]->ids, BLOCK_STONE, 32768);
		/* 16 columns x 16 ID pages fills the real 256-page pool.
		 * Victims 0/1 then push divergence beyond capacity. */
		if(n >= 2)
			memset(expected[n]->ids, BLOCK_GLASS,
				COLUMN_VIEW_POOL_PAGES / 16 * COLUMN_VIEW_PAGE_CELLS);
		queue(n, expected[n]); clin_update_drain();
		compare(n);
	}
	/* Dictionary growth relocates its entries. Resolve the final addresses
	 * before the writer or any of the subsequent gap/replacement checks. */
	for(unsigned n = 0; n < 18; n++) {
		columns[n] = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(column_x(n) / 16, 0));
		assert(columns[n] && columns[n]->backing);
	}
	assert(stats().pages == COLUMN_VIEW_POOL_PAGES && stats().live_views == 18);
}
static void run(unsigned mode) {
	struct column_view_stats before_stats = stats();
	clin_test_reset_input_epoch_guard();
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	struct server_local* s = split_fixture();
	fill(s);
	/* Accept a tagged marker, as real local sessions do. Recovery uses its
	 * existing FIFO handshake even while the presentation gate is pending. */
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {0.5, 65.62, 0.5},
			.input_epoch = 1}});
	svin_process_messages(capture_recovery, NULL, false);
	struct entity* player = gstate.players[0].local_player;
	struct window_container* inventory = gstate.players[0].windows[WINDOWC_INVENTORY];
	assert(player && inventory);
	size_t entities = dict_entity_size(gstate.entities);
	if(mode == 3) {
		/* Occupy the ONE preallocated emergency image with a client light
		 * write, without freeing any of the 256 ordinary pages. */
		fail_alloc = 1;
		chunk_set_light(client(1), 0, 0, 0, 0x47);
		assert(stats().fallback_live == 1 && stats().pages == COLUMN_VIEW_POOL_PAGES);
		assert(chunk_get_block(client(1), 0, 0, 0).torch_light == 4);
		assert(columns[1]->lighting_torch[32] == 0);
	}
	uint32_t previous_fallback = stats().fallback_total;
	fail_alloc = hold_alloc = mode != 4;
	hold_success = mode == 4;
	struct thread worker;
	assert(thread_create(&worker, writer, NULL, 64));
	void* token;
	assert(tchannel_receive(&entered, &token, true));
	assert(columns[0]->ids[64] == BLOCK_STONE); // mandatory write still pending
	if(mode == 1 || mode == 4) {
		clin_rpc_send(&(struct client_rpc){.type = CRPC_UNLOAD_CHUNK,
			.payload.unload_chunk = {.x = 0, .z = 0}});
		clin_update_drain();
		assert(!client(0));
	} else if(mode == 2) {
		/* Full-pool churn: a real light flood, delayed block RPC, replacement
		 * and retirement overlap the failed server allocation gap. */
		clin_rpc_send(&(struct client_rpc){.type = CRPC_SET_BLOCK,
			.payload.set_block = {.x = 32, .y = 64, .z = 0,
				.block = {.type = BLOCK_TORCH, .metadata = 5, .sky_light = 15}}});
		queue(3, NULL); // delayed replacement rejoins current backing
		assert(stats().pages == COLUMN_VIEW_POOL_PAGES);
		clin_update_drain();
		world_update_lighting(&gstate.world);
		assert(world_lighting_driver_last_work() > 0);
		assert(columns[2]->ids[64] == BLOCK_STONE);
		assert(columns[2]->lighting_torch[32] == 0);
		assert(stats().pages < COLUMN_VIEW_POOL_PAGES);
		/* The relight itself may materialize views under the smaller pool.
		 * The paused server writer must need no ADDITIONAL fallback. */
		previous_fallback = stats().fallback_total;
	}
	tchannel_send(&proceed, &proceed, true);
	if(mode == 4) {
		/* The successful spare is now unused and freed outside the stripe.
		 * Reinstall while it is released: the next rescan is exhausted again.
		 * It must try the heap afresh, not inherit an emergency token. */
		assert(tchannel_receive(&entered, &token, true));
		queue(0, NULL); clin_update_drain();
		fail_alloc = 1;
		unsigned before = failures;
		tchannel_send(&proceed, &proceed, true);
		thread_join(&worker);
		assert(successful_allocations == 1 && failures == before + 1);
		assert(!fail_alloc && !unused_success);
		compare(0);
		assert(stats().fallback_total == previous_fallback + 1);
		assert(!column_view_recovery_pending());
		assert(columns[0]->ids[64] == BLOCK_DIRT);
	} else if(mode == 3) {
		ptime_t start = time_get();
		while(!column_view_recovery_pending()) {
			assert(time_diff_s(start, time_get()) < 10);
			thread_msleep(1);
		}
		/* The server may already have hit its deadline if main was preempted.
		 * Do not read its live bytes until the join below. */
		/* Retain an obsolete shell through reset: it must not block the
		 * server or become eligible for preservation again after restream. */
		struct chunk* retained = client(0);
		chunk_ref(retained);
		do {
			clin_recover_column_storage();
			if(stats().recovery_entries == before_stats.recovery_entries) thread_msleep(1);
			assert(time_diff_s(start, time_get()) < 10);
		} while(stats().recovery_entries == before_stats.recovery_entries);
		assert(gstate.players[0].current_screen == &screen_load_world);
		assert(!gstate.world_loaded && gstate.world_switch_pending);
		assert(!world_loaded_chunks(&gstate.world));
		assert(gstate.players[0].local_player == player);
		assert(gstate.players[0].windows[WINDOWC_INVENTORY] == inventory);
		assert(dict_entity_size(gstate.entities) == entities);
		svin_process_messages(capture_recovery, NULL, false);
		assert(recover_requests == 1);
		thread_join(&worker);
		assert(columns[0]->ids[64] == BLOCK_DIRT);
		clin_process(&(struct client_rpc){.type = CRPC_STREAM_RESET});
		assert(!column_view_recovery_pending());
		chunk_set_light(retained, 0, 0, 0, 0xff); // retired epoch stays ineligible
		assert(columns[0]->lighting_torch[32] == 0);
		queue(0, NULL); clin_update_drain();
		assert(chunk_get_block(client(0), 0, 0, 0).type == BLOCK_DIRT);
		column_backing_set_cell(columns[0]->backing, COLUMN_IDS, 65, BLOCK_DIRT);
		assert(chunk_get_block(client(0), 0, 1, 0).type == BLOCK_STONE);
		chunk_unref(retained);
	} else {
		thread_join(&worker);
		assert(!column_view_recovery_pending());
		assert(columns[0]->ids[64] == BLOCK_DIRT);
		if(mode != 1) compare(0);
		assert(stats().fallback_total == previous_fallback + (mode == 0));
	}
	struct column_view_stats after_stats = stats();
	assert(after_stats.peak_pages == COLUMN_VIEW_POOL_PAGES);
	assert(after_stats.peak_pages >= before_stats.peak_pages);
	assert(after_stats.page_failures > before_stats.page_failures);
	assert(after_stats.emergency_uses == before_stats.emergency_uses
		+ (mode == 0 || mode == 3 || mode == 4));
	assert(after_stats.recovery_entries == before_stats.recovery_entries + (mode == 3));
	world_unload_all(&gstate.world);
	assert(stats().peak_pages == after_stats.peak_pages);
	assert(column_view_pool_shutdown() && !column_view_pool_present());
	assert(!stats().live_views && !stats().pages && !stats().fallback_live);
	for(unsigned n = 0; n < 18; n++) free(expected[n]);
	split_destroy(s);
	printf("PASS real 256-slot pressure mode %u: %s; simulation write retained\n",
		mode, (const char*[]){"first allocation NULL -> emergency",
		"retired in failed-allocation gap -> rescan",
		"RPC/relight/replacement freed pages in failed-allocation gap -> rescan",
		"heap + emergency exhausted -> loading/stream recovery",
		"unused successful spare released -> fresh failure before emergency"}[mode]);
}
struct burst_times { uint64_t wall, cpu; };
static struct burst_times burst_server, burst_main, burst_recovery;
static struct burst_times burst_peaks[2][3];
static uint32_t burst_done;
static void measured_edit(unsigned n, unsigned i, bool server, struct burst_times* max) {
	uint64_t wall = tps_wall_now_ns(), cpu = tps_cpu_now_ns();
	if(server) column_backing_set_cell(columns[n]->backing, COLUMN_IDS, i,
		(i & 1) ? BLOCK_DIRT : BLOCK_GLASS);
	else column_view_set(client(n)->view, i,
		(struct block_data){.type = BLOCK_STONE, .sky_light = 7, .torch_light = 4}, true);
	cpu = tps_cpu_now_ns() - cpu; wall = tps_wall_now_ns() - wall;
	if(cpu > max->cpu) max->cpu = cpu;
	if(wall > max->wall) max->wall = wall;
}
static void* burst_writer(void* unused) {
	(void)unused;
	void* token;
	assert(tchannel_receive(&proceed, &token, true));
	/* Anchor 1 owns all 256 ID/meta pages. A fresh sky write forces the
	 * worst occupied-page materialisation, or the independent deadline. */
	uint64_t wall = tps_wall_now_ns(), cpu = tps_cpu_now_ns();
	column_backing_set_cell(columns[1]->backing, COLUMN_SKY, 8192, 3);
	burst_server.cpu = tps_cpu_now_ns() - cpu;
	burst_server.wall = tps_wall_now_ns() - wall;
	/* Both player anchors' authoritative edits execute on the ONE server
	 * thread, concurrent with main's presentation edits at both anchors. */
	for(unsigned i = 0; i < 4096; i++)
		for(unsigned anchor = 0; anchor < 2; anchor++)
			measured_edit(anchor, i, true, &burst_server);
	__atomic_store_n(&burst_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void editing_burst(unsigned exhausted) {
	burst_server = burst_main = burst_recovery = (struct burst_times){0};
	clin_test_reset_input_epoch_guard();
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	struct server_local* s = split_fixture(); fill(s);
	/* SPLIT-SPILL (pool 256 -> 1024): this scenario is DEFINED at a 256-page
	 * capacity (the burst's 128 ID + 128 meta pages exactly fill it, and the
	 * non-exhausted run must then need a private fallback). Pin admissions at
	 * 256 so it keeps that meaning at any configured capacity >= 256. */
	_Static_assert(COLUMN_VIEW_POOL_PAGES >= 256, "burst needs a 256-page admission limit");
	column_view_test_page_limit(256);
	/* Replace fixture snapshot divergence with a real editing burst that
	 * puts all 256 decoded ID/meta pages on one anchor. Timed materialisation
	 * must patch that worst occupancy, not just memcpy an empty-page victim. */
	for(unsigned n = 2; n < 18; n++) { queue(n, NULL); clin_update_drain(); }
	assert(stats().pages == 0);
	for(unsigned page = 0; page < 128; page++)
		column_view_set(client(1)->view, page * 256,
			(struct block_data){.type = BLOCK_GLASS, .metadata = 1}, false);
	assert(stats().pages == 256);
	clin_process(&(struct client_rpc){.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {0.5, 65.62, 0.5}, .input_epoch = 1}});
	svin_process_messages(capture_recovery, NULL, false);
	struct column_view_stats before = stats();
	assert(chunk_mesher_init());
	chunk_mesher_test_hold_worker(true);
	assert(chunk_mesher_send(client(0))); client(0)->mesh_in_flight = true;
	ptime_t start = time_get();
	while(!chunk_mesher_test_worker_held()) {
		assert(time_diff_s(start, time_get()) < 5); thread_msleep(1);
	}
	assert(chunk_mesher_pending());
	__atomic_store_n(&burst_fail_all, exhausted, __ATOMIC_RELEASE);
	if(exhausted) {
		/* The emergency tenant holds no pool pages: all 256 remain occupied.
		 * This is a real client materialisation, timed as part of main work. */
		measured_edit(0, 64, false, &burst_main);
		assert(stats().emergency_uses == before.emergency_uses + 1);
		assert(stats().pages == 256);
	}
	burst_done = 0;
	struct thread server; assert(thread_create(&server, burst_writer, NULL, 64));
	tchannel_send(&proceed, &proceed, true);
	if(!exhausted) chunk_mesher_test_hold_worker(false);
	start = time_get();
	for(unsigned i = 0; i < 4096; i++)
		for(unsigned anchor = 0; anchor < 2; anchor++)
			measured_edit(anchor, i, false, &burst_main);
	/* Main deliberately NEVER accepts recovery until the server has finished.
	 * The worker also remains pinned in this mode. The old implementation
	 * hangs here; the deadline must release the write without either owner. */
	while(!__atomic_load_n(&burst_done, __ATOMIC_ACQUIRE)) {
		assert(time_diff_s(start, time_get()) < 2); thread_msleep(1);
	}
	thread_join(&server);
	for(unsigned anchor = 0; anchor < 2; anchor++)
		for(unsigned i = 0; i < 32768; i++) {
			assert(columns[anchor]->ids[i] == (i < 4096 ? ((i & 1) ? BLOCK_DIRT : BLOCK_GLASS) : BLOCK_STONE));
			assert(!nibble_read(columns[anchor]->metadata, i));
			assert(nibble_read(columns[anchor]->lighting_sky, i) == (anchor == 1 && i == 8192 ? 3 : 0));
			assert(!nibble_read(columns[anchor]->lighting_torch, i));
			if(!exhausted) {
				struct block_data b = column_view_get(client(anchor)->view, i);
				bool edited = anchor == 1 && i % 256 == 0;
				assert(b.type == (edited ? BLOCK_GLASS : BLOCK_STONE));
				assert(b.metadata == (edited ? 1 : 0));
				assert(b.sky_light == (i < 4096 ? 7 : 0));
				assert(b.torch_light == (i < 4096 ? 4 : 0));
			}
		}
	if(exhausted) {
		assert(column_view_recovery_pending() && chunk_mesher_test_worker_held());
		assert(stats().recovery_cycles == before.recovery_cycles + 1);
		assert(stats().recovery_expiries == before.recovery_expiries + 1);
		assert(stats().recovery_wait_max_us >= COLUMN_VIEW_RECOVERY_WAIT_MS * 1000);
		assert(stats().recovery_wait_max_us >= before.recovery_wait_max_us);
		uint64_t wall = tps_wall_now_ns(), cpu = tps_cpu_now_ns();
		clin_recover_column_storage();
		cpu = tps_cpu_now_ns() - cpu; wall = tps_wall_now_ns() - wall;
		if(wall > burst_recovery.wall) burst_recovery.wall = wall;
		if(cpu > burst_recovery.cpu) burst_recovery.cpu = cpu;
		assert(stats().recovery_entries == before.recovery_entries + 1);
		svin_process_messages(capture_recovery, NULL, false);
		clin_process(&(struct client_rpc){.type = CRPC_STREAM_RESET});
		/* Reinstall while the obsolete worker shell is still pinned. The new
		 * epoch must see all writes, never revive the discarded snapshot. */
		for(unsigned anchor = 0; anchor < 2; anchor++) {
			queue(anchor, NULL); clin_update_drain();
			for(unsigned i = 0; i < 32768; i++) {
				struct block_data b = column_view_get(client(anchor)->view, i);
				assert(b.type == columns[anchor]->ids[i] && !b.metadata && !b.torch_light);
				assert(b.sky_light == (anchor == 1 && i == 8192 ? 3 : 0));
			}
		}
	} else {
		assert(!column_view_recovery_pending());
		assert(stats().fallback_total >= before.fallback_total + 1);
		assert(stats().recovery_cycles == before.recovery_cycles);
		assert(stats().recovery_expiries == before.recovery_expiries);
		assert(stats().recovery_wait_max_us == before.recovery_wait_max_us);
	}
	__atomic_store_n(&burst_fail_all, 0, __ATOMIC_RELEASE);
	world_unload_all(&gstate.world);
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	chunk_mesher_test_hold_worker(false); chunk_mesher_shutdown();
	assert(column_view_pool_shutdown());
	for(unsigned n = 0; n < 18; n++) free(expected[n]);
	split_destroy(s);
	struct burst_times samples[] = {burst_main, burst_server, burst_recovery};
	for(unsigned i = 0; i < 3; i++) {
		if(samples[i].wall > burst_peaks[exhausted][i].wall) burst_peaks[exhausted][i].wall = samples[i].wall;
		if(samples[i].cpu > burst_peaks[exhausted][i].cpu) burst_peaks[exhausted][i].cpu = samples[i].cpu;
	}
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	assert(tchannel_init(&entered, 1) && tchannel_init(&proceed, 1));
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	for(unsigned mode = 0; mode < 5; mode++) run(mode);
	assert(failures == 6);
	assert(tps_cpu_probe_step_ns());
	/* Qualify/report the existing CPU probe's mode, then retire callbacks
	 * before measuring. Never silently label the wall fallback as CPU. */
	char clock_dir[128], clock_prefix[160];
	test_fixture_dir("test_pressure_clock", clock_dir, sizeof(clock_dir));
#ifdef _WIN32
	assert(_mkdir(clock_dir) == 0);
#else
	assert(mkdir(clock_dir, 0777) == 0);
#endif
	snprintf(clock_prefix, sizeof(clock_prefix), "%s/", clock_dir);
	assert(tps_observer_start(clock_prefix));
	struct tps_cpu_evidence clock_evidence;
	tps_observer_snapshot(&clock_evidence);
	assert(tps_observer_stop());
	test_fixture_dir_cleanup(clock_dir);
	for(unsigned repeat = 0; repeat < 8; repeat++) {
		editing_burst(0); editing_burst(1);
	}
	/* Windows sleep(1) may consume a scheduler quantum; 100 ms is the
	 * explicit PC scheduling/lock allowance, NOT a claimed Wii bound. */
	assert(burst_peaks[1][1].wall >= COLUMN_VIEW_RECOVERY_WAIT_MS * UINT64_C(1000000));
	assert(burst_peaks[1][1].wall < (COLUMN_VIEW_RECOVERY_WAIT_MS + 100) * UINT64_C(1000000));
	for(unsigned mode = 0; mode < 2; mode++)
		printf("PASS two-anchor d256 editing mode %u (%s), 8 bursts x 8193 server + 8192 main edits, busy/pinned real worker, real 256-page exhaustion: max main wall/CPU %.3f/%.3f ms; server %.3f/%.3f ms; main recovery %.3f/%.3f ms; clock mode %u; recovery deadline %u ms (plus scheduling/stripe), all authoritative writes retained\n",
			mode, mode ? "emergency + deadline" : "heap materialisation",
			burst_peaks[mode][0].wall / 1e6, burst_peaks[mode][0].cpu / 1e6,
			burst_peaks[mode][1].wall / 1e6, burst_peaks[mode][1].cpu / 1e6,
			burst_peaks[mode][2].wall / 1e6, burst_peaks[mode][2].cpu / 1e6,
			clock_evidence.clock_mode, COLUMN_VIEW_RECOVERY_WAIT_MS);
	struct column_view_stats final_stats = stats();
	printf("PASS recovery telemetry: cycles=%u expiries=%u accepted=%u max_wait_us=%u (completed waits, boot maximum)\n",
		final_stats.recovery_cycles, final_stats.recovery_expiries,
		final_stats.recovery_entries, final_stats.recovery_wait_max_us);
	world_destroy(&gstate.world);
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_clear(gstate.entities);
	free(gstate.players[0].local_player); gstate.players[0].local_player = NULL;
	tchannel_close(&entered); tchannel_close(&proceed);
	return 0;
}

