/*
	The server tick's chunk-stream pipeline: it must never PARK, and it must never
	LIE about where it is. One fixture, one real superflat Nether world, three claims
	driven through the production tick (server_local_test_update_once).

	(A) WATCHDOG PHASE TAGS of the near-send batch. The 2026-07-27 real-Wii hang was
	    read off a photograph of the frozen phase cell bars, and the tag it showed
	    (srv=8, ENSURE_POPULATED) spanned code that had long since returned. Splitting
	    that span is only an improvement if each new tag is retired the moment its call
	    returns -- a tag that outlives its call is the SAME defect wearing a more
	    convincing label. So: the near batch's SLOT_WAIT must not survive the acquire,
	    and its RPC_SEND must not survive the batch.

	(B) FAR SLOT POOL backpressure. The far batch is best-effort -- every column it
	    skips is re-picked next tick. Acquiring its ~1 KB payload slot with a BLOCKING
	    receive therefore bought nothing and cost everything: with the pool drained
	    (the main thread slow to drain CRPC_FAR_CHUNK, or wedged outright) the prio-64
	    server thread parked inside the acquire and never returned to release chunk
	    slots -- the other end of the two-way bounded-buffer deadlock clin_try_rpc_send
	    already removed from the far SEND. FAR_SEND_BUDGET makes it easy to reach: one
	    tick at a high far_view_distance can claim the whole pool. A deadline thread
	    runs beside each measured tick, so a regression to the blocking acquire dies
	    with a NAMED abort instead of hanging ctest until its timeout.

	(C) The LEAK ALARM that (B) made necessary. A non-blocking bail is silent by
	    construction, so a far slot the client consumed and never released now stops
	    far streaming for good with no symptom -- where the old blocking acquire at
	    least froze visibly. The far batch must therefore tell a run of bails that ends
	    (pressure) from one that does not (a leak), and say so exactly once per episode.
*/

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* A1b: fixture rows and hand-built windows name their world explicitly (the
 * slot-0 fallbacks for unstamped rows / unbound windows are gone). */
#define A1B_STAMP(s, kind, k, w) \
	((s)->te_owners.kind##_slot[k] = (w)->world_index, \
	 (s)->te_owners.kind##_gen[k] = (w)->generation)
#define A1B_BIND(player, w) ((player)->window_world = server_world_handle_of(w))
#ifdef _WIN32
#include <direct.h>
#include <process.h> // _getpid
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h> // getpid
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h" // MAX_PLAYER_HEALTH
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/watchdog.h"

// PID-SUFFIXED, not a fixed literal: this fixture exercises a real on-disk
// world under CPU-contention stress (converge_near_ring's deadline loop). A
// STALE process from a prior run (e.g. one still finishing a slow tick when
// the test harness re-invokes the binary -- exactly the "builds timing out,
// targets not relinking" environment 5704beb's session describes) can hold
// this same relative directory open, and TWO processes writing/reading the
// same region files concurrently produces spurious failures with no relation
// to this test's own logic -- reproduced 3x while investigating the
// far-slot/converge_near_ring history (2026-08-05, CaveWii Dev channel). A
// per-PID path makes that collision structurally impossible: no two live
// processes ever share one.
static char g_tmp_world[64];
#define TMP_WORLD g_tmp_world
#define WORLD_SEED INT64_C(1440714694106570529)
// The pools are 16 slots (RPC_INBOX_SIZE); the caps only have to exceed that.
#define HELD_SLOTS_MAX 64
// Wall-clock deadline for one tick. A converged superflat Nether tick is well under a
// second here; 5s means "the thread is parked", not "the machine is slow".
#define TICK_WAIT_MS 5000
// Ticks allowed for the drained pool to be CONSULTED (see the loop for why > 1).
#define FAR_BAIL_TICKS 16
// (A) budget. Ticks retried to reach the busy acquire (generation can defer).
#define PHASE_PARK_TICKS 8
// (C) budgets. FAR_SLOT_LEAK_BAILS is private to server_local.c, so drive the loops on
// the pool's own bail counter and leave generous headroom above it.
#define LEAK_BAILS_TARGET 130 // comfortably past the threshold, cheap in ticks
#define LEAK_BAILS_QUIET 10   // a run this short must NOT have reported anything
#define LEAK_TICK_CAP 4000
static char s_fixture_LEAK_LOG[128]; /* was #define LEAK_LOG='test_far_slot_deadlock_stderr.txt' (a literal) */
#define LEAK_LOG s_fixture_LEAK_LOG
#define LEAK_MARK "far slot pool starved"

static atomic_bool tick_done = ATOMIC_VAR_INIT(false);
static atomic_bool drain_run = ATOMIC_VAR_INIT(false);
static struct thread drain_thread;

static bool flag_get(const atomic_bool* flag) {
	return atomic_load_explicit(flag, memory_order_acquire);
}

static void flag_set(atomic_bool* flag, bool value) {
	atomic_store_explicit(flag, value, memory_order_release);
}

// The client's consumer role. Setup ticks emit far more than RPC_INBOX_SIZE RPCs, and
// clin_rpc_send BLOCKS on an exhausted token pool, so the fixture needs a real drainer
// running beside the tick -- exactly as the game's main thread runs clin_update.
static void* drain_entry(void* arg) {
	(void)arg;
	while(flag_get(&drain_run)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void drain_start(void) {
	flag_set(&drain_run, true);
	assert(thread_create(&drain_thread, drain_entry, NULL, 64));
}

static void drain_stop(void) {
	flag_set(&drain_run, false);
	thread_join(&drain_thread);
	clin_drain_discard();
}

// Deadline thread for the measured tick. It also plays the client's consumer role,
// which is safe off the world-owner thread: clin_drain_discard touches only the RPC
// channels and the slot pools, never the world or its region archive.
static void* guard_entry(void* arg) {
	(void)arg;
	// WALL-CLOCK, not an iteration count: thread_msleep(1) is a >= 1ms floor whose
	// real granularity is the host scheduler's, so counting sleeps would stretch the
	// deadline by an unknown factor.
	ptime_t start = time_get();
	while(!flag_get(&tick_done)
		  && time_diff_ms(start, time_get()) < TICK_WAIT_MS) {
		clin_drain_discard();
		thread_msleep(1);
	}
	if(!flag_get(&tick_done)) {
		// A parked server tick cannot be unwound, so name it and die rather than let
		// ctest's timeout report an anonymous hang.
		fputs("test_far_slot_deadlock_env: server tick PARKED with the far slot "
			  "pool drained (blocking far acquire regression)\n",
			  stderr);
		abort();
	}
	return NULL;
}

// One production tick, ON THE OWNER THREAD (region_archive asserts its creator owns
// every access), with a deadline thread proving it returned. The far pool stays
// drained across the whole window: with no free slot the tick cannot emit a
// CRPC_FAR_CHUNK, so the guard's drain can never hand a far slot back mid-measurement.
static void tick_once_bounded(struct server_local* server) {
	flag_set(&tick_done, false);
	struct thread guard;
	assert(thread_create(&guard, guard_entry, NULL, 64));
	/* This fixture measures streaming, not simulation. Keep every production
	 * update on a non-spawn tick so a random Nether hostile cannot kill the
	 * player and divert the test into the unrelated death/world-switch path. */
	server->world_time = 1;
	server_local_test_update_once(server);
	assert(server->player.health == MAX_PLAYER_HEALTH);
	flag_set(&tick_done, true);
	thread_join(&guard);
	clin_drain_discard();
}

static void streaming_tick(struct server_local* server) {
	server->world_time = 1;
	server_local_test_update_once(server);
	assert(server->player.health == MAX_PLAYER_HEALTH);
}

static int near_sent_count(struct server_local* server) {
	int n = 0;
	for(w_coord_t z = -MAX_VIEW_DISTANCE; z <= MAX_VIEW_DISTANCE; z++)
		for(w_coord_t x = -MAX_VIEW_DISTANCE; x <= MAX_VIEW_DISTANCE; x++)
			if(server_world_is_chunk_sent(&server->world, x, z))
				n++;
	return n;
}

#define NEAR_RING_COLUMNS ((2 * MAX_VIEW_DISTANCE + 1) * (2 * MAX_VIEW_DISTANCE + 1))

// WALL-CLOCK, not a fixed attempt count (mirrors guard_entry/tick_once_bounded
// above -- the same reasoning, applied to a multi-tick convergence loop instead
// of one tick). A fixed count silently assumes roughly-constant per-tick
// streaming throughput; two things in the production path scale that
// throughput with available CPU time instead: SERVER_GEN_BUDGET_MS
// (server_world.c) gates world-gen on wall-clock, guaranteeing only "at least
// one attempt" regardless of elapsed time, and the tick's first near-send
// chunk-slot acquire BLOCKS on this fixture's own drain_thread, which can be
// descheduled far past its normal 1ms poll under contention. Under heavy CPU
// pressure (e.g. a concurrent full rebuild) both can make a single
// streaming_tick() do much less real work without the call itself taking
// noticeably longer -- a fixed count(512) was hit before the ring converged,
// tripping assert(near_complete).
//
// HISTORY (2026-08-05): this is almost certainly the root cause of the
// "test_far_slot_deadlock_env CRASHES nondeterministically with 0xc0000409
// (stack buffer overrun), 4/5 runs" note in 5704beb's commit message,
// observed during a session with a concurrent full rebuild running -- exactly
// this contention. 0xc0000409 (STATUS_STACK_BUFFER_OVERRUN) is NOT evidence
// of memory corruption here: on Windows UCRT, assert() -> abort() ->
// __fastfail() reports that NTSTATUS for every fastfail regardless of cause,
// so a plain assert(near_complete) failure under load and a genuine stack
// smash are indistinguishable by exit code alone. Reproduced: 0/11 failures
// at low load, 2/4 failures under artificial heavy contention (concurrent
// build + 4 parallel test runs), always via this exact assert -- never
// "stack smashing detected" (gcc's real overrun message) and never an ASan
// finding. No live memory-corruption bug was found in the project from this
// investigation. Full writeup: the CaveWii Dev channel thread, 2026-08-05.
#define NEAR_RING_CONVERGE_TIMEOUT_MS 30000
static void converge_near_ring(struct server_local* server) {
	bool near_complete = false;
	drain_start(); // a load-burst tick blocks MID-send without a live consumer
	ptime_t start = time_get();
	while(!near_complete
		  && time_diff_ms(start, time_get()) < NEAR_RING_CONVERGE_TIMEOUT_MS) {
		streaming_tick(server);
		clin_drain_discard();
		near_complete = near_sent_count(server) == NEAR_RING_COLUMNS;
	}
	drain_stop();
	assert(near_complete);
}

// ---------------------------------------------------------------- fixture ----

static string_t g_level;

// TMP_WORLD is now a runtime (PID-suffixed) path, not a string-literal token,
// so it can no longer feed C's adjacent-literal concatenation. Small helper
// instead of open-coding a snprintf at every call site below. NOT reentrant
// (one static buffer) -- fine here, every call site uses its result
// immediately and this fixture is single-threaded on the owner thread.
static const char* tmp_path(const char* suffix) {
	static char buf[96];
	int n = snprintf(buf, sizeof(buf), "%s%s", g_tmp_world, suffix);
	assert(n > 0 && (size_t)n < sizeof(buf));
	return buf;
}

static struct server_local* fixture_create(void) {
	int n = snprintf(g_tmp_world, sizeof(g_tmp_world),
					 "test_far_slot_deadlock_tmp_%d", (int)GETPID());
	assert(n > 0 && (size_t)n < sizeof(g_tmp_world));

	MKDIR(TMP_WORLD);
	MKDIR(tmp_path("/DIM-1"));
	MKDIR(tmp_path("/DIM-1/region"));
	remove(tmp_path("/DIM-1/region/r.-1.-1.mcr"));
	remove(tmp_path("/DIM-1/region/r.-1.0.mcr"));
	remove(tmp_path("/DIM-1/region/r.0.-1.mcr"));
	remove(tmp_path("/DIM-1/region/r.0.0.mcr"));
	remove(tmp_path("/level.dat"));
	remove(tmp_path("/level.dat_new"));
	remove(tmp_path("/level.dat_old"));

	struct server_local* server = calloc(1, sizeof(*server));
	assert(server);
	/* Production's server_local is an uninitialized stack object. Poison the
	 * sleep fields specifically so this fixture proves init establishes the RPC
	 * gate/timer baseline instead of inheriting a lucky calloc zero. */
	server->player.sleeping = true;
	server->player.sleep_timer = 77;
	server->player.sleep_x = 123;
	server->player.sleep_y = -45;
	server->player.sleep_z = 678;
	server_local_test_create_without_thread(
		server, false, false, false, false, false, true, false);
	assert(!server->player.sleeping && server->player.sleep_timer == 0);
	assert(server->player.sleep_x == 0 && server->player.sleep_y == 0
		   && server->player.sleep_z == 0);
	string_init_set_str(g_level, TMP_WORLD);
	vec3 world_spawn = {0.0F, 4.0F, 0.0F};
	vec3 player_spawn = {0.5F, 4.0F + PLAYER_EYE_HEIGHT, 0.5F};
	assert(level_archive_create_new(
		&server->level, g_level, WORLD_SEED, world_spawn));
	assert(level_archive_write_player(
		&server->level, player_spawn, (vec2) {0.0F, 0.0F},
		(vec3) {0.0F, 0.0F, 0.0F}, WORLD_DIM_NETHER));
	assert(level_archive_save(&server->level));
	assert(server_world_create_with_populate_scratch(
		&server->world, g_level, WORLD_DIM_NETHER, WORLD_SEED, true,
		server->world_populate_scratch));
	server->world.local = server;
	server->worlds[0].session = server; /* A1a (D1): owning session for services/callbacks */
	server->player.has_pos = true;
	server->player.finished_loading = true;
	server->player.dimension = WORLD_DIM_NETHER;
	server->player.x = 0.5;
	server->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	server->player.z = 0.5;
	server->player.health = MAX_PLAYER_HEALTH;
	server->player.oxygen = MAX_OXYGEN;
	server->far_view_distance = MAX_VIEW_DISTANCE + 1;
	server->scheduled_tick_budget = 0;
	server->autosave_ticks = 0;
	return server;
}

static void fixture_destroy(struct server_local* server) {
	server_world_discard(&server->world);
	level_archive_discard(&server->level);
	server_world_populate_scratch_destroy(server->world_populate_scratch);
	server->world_populate_scratch = NULL;
	inventory_destroy(&server->player.inventory, &server->player);
	dict_entity_clear(server->entities);
	dict_autosave_saved_clear(server->autosave_saved_this_epoch); // FIX #3(b)
	string_clear(server->level_name);
	string_clear(g_level);
	free(server);
}

// ------------------------------------------- (A) near-send phase tags --------

// Synchronous TRANSITION recorder for the server phase tag. The old free-running
// sampler made the test perturb the scheduling it measured: its same-priority busy
// spin plus ASan overhead pushed every send past the production 12 ms batch budget,
// so no tick could reach iteration two. Observing the PC watchdog setter directly
// records every real store in order without depending on a scheduler timeslice.
#define PHASE_TRACE_MAX 256
static int trace_n = 0;
static bool trace_overflow = false;
static uint8_t trace_buf[PHASE_TRACE_MAX];

static void trace_observe(enum wd_phase_srv p, void* user) {
	(void)user;
	assert((unsigned)p < 256u);
	if(trace_n > 0 && trace_buf[trace_n - 1] == (uint8_t)p)
		return;
	if(trace_n >= PHASE_TRACE_MAX) {
		trace_overflow = true;
		return;
	}
	trace_buf[trace_n++] = (uint8_t)p;
}

static void trace_start(void) {
	trace_n = 0;
	trace_overflow = false;
	watchdog_test_set_srv_phase_observer(trace_observe, NULL);
}

static void trace_stop(void) {
	watchdog_test_set_srv_phase_observer(NULL, NULL);
	assert(!trace_overflow && "server phase trace overflowed");
}

static bool trace_saw(enum wd_phase_srv p) {
	for(int i = 0; i < trace_n; i++)
		if(trace_buf[i] == (uint8_t)p)
			return true;
	return false;
}

// Did the real setter sequence show iteration N's send, iteration N+1's re-scan
// and ensure, then iteration N+1's send? The final post-loop NEAR_SCAN cannot satisfy
// this because no ENSURE_POPULATED + RPC_SEND follows it in an incomplete near ring.
static bool trace_rescan_between_sends(void) {
	enum { WANT_SEND, WANT_RESCAN, WANT_ENSURE, WANT_SECOND_SEND } state
		= WANT_SEND;
	for(int i = 0; i < trace_n; i++) {
		enum wd_phase_srv p = (enum wd_phase_srv)trace_buf[i];
		if(state == WANT_SEND && p == WDS_RPC_SEND)
			state = WANT_RESCAN;
		else if(state == WANT_RESCAN && p == WDS_NEAR_SCAN)
			state = WANT_ENSURE;
		else if(state == WANT_ENSURE && p == WDS_ENSURE_POPULATED)
			state = WANT_SECOND_SEND;
		else if(state == WANT_SECOND_SEND && p == WDS_RPC_SEND)
			return true;
	}
	return false;
}

// Take every free chunk slot. Called between ticks with the drainer stopped and the
// inbox drained, so nothing is in flight and the pool cannot refill behind us.
static int hold_all_chunk_slots(struct chunk_rpc_slot** held) {
	int n = 0;
	while(n < HELD_SLOTS_MAX) {
		struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
		if(!slot)
			break;
		held[n++] = slot;
	}
	assert(n > 0 && n < HELD_SLOTS_MAX);
	return n;
}

static struct server_chunk* prepare_unsent_near_chunk(
	struct server_local* server, w_coord_t x, w_coord_t z) {
	struct server_chunk* chunk = NULL;
	bool ready = false;
	for(int attempt = 0; attempt < 64 && !ready; attempt++)
		ready = server_world_ensure_populated(
			&server->world, server, x, z, &chunk);
	assert(ready && chunk && "near-send fixture could not pre-populate its chunk");
	assert(!chunk->sent && "pre-population must not pre-send the measured chunk");
	return chunk;
}

static void test_near_send_phase_tags(struct server_local* server) {
	// The far batch is the OTHER writer of WDS_CHUNK_SEND, and it is gated on a
	// COMPLETE near ring -- so while the ring is still filling, every CHUNK_SEND
	// transition below can only have come from the near send. That gate is asserted
	// after the measured tick.
	drain_start();
	// The production nearest scan selects center (0,0), then north (0,-1) for
	// this fixed spawn. Make both cheap and ready without marking either sent;
	// the test-only minimum then bypasses only the elapsed cutoff after send 1.
	prepare_unsent_near_chunk(server, 0, 0);
	prepare_unsent_near_chunk(server, 0, -1);
	assert(near_sent_count(server) == 0);

	int before = near_sent_count(server);
	server_local_test_set_near_send_minimum(2);
	trace_start();
	streaming_tick(server);
	server_local_test_set_near_send_minimum(0);
	trace_stop();
	int after = near_sent_count(server);
	drain_stop();

	// Only a tick that left the ring INCOMPLETE is attributable: the tick that
	// sends the last near column also permits the far batch to stamp CHUNK_SEND.
	assert(after < NEAR_RING_COLUMNS
		   && "the measured near batch unexpectedly completed the whole ring");
	assert(after - before >= 2
		   && "the deterministic fixture did not exercise near-send iteration two");
	assert(watchdog_phase_srv_current() != WDS_RPC_SEND
		   && "near-send batch leaked WDS_RPC_SEND past the loop");
	assert(watchdog_phase_srv_current() != WDS_SLOT_WAIT
		   && "near-send batch leaked WDS_SLOT_WAIT past the loop");
	// F2a: the post-stream world tail finishes before the network phase.
	// Preserve H1's distinct tail tag and prove the tick ends in the later
	// RPC phase, rather than leaking a near-batch tag past its work.
	assert(trace_n >= 2 && trace_buf[trace_n - 2] == WDS_TAIL
		   && trace_buf[trace_n - 1] == WDS_RPC
		   && watchdog_phase_srv_current() == WDS_RPC
		   && "the world tail did not precede the final network phase");
	// THE D1 REGRESSION GUARD. With the far batch unreachable (below), WDS_CHUNK_SEND
	// can only come from the near send's payload fill -- the five memcpys (~80 KB),
	// piston_moving_server_snapshot and worldgen_climate that ran with the tag still
	// reading "parked in clin_chunk_slot_acquire". A hang or fault in any of them was
	// reported at a call that had already returned: a specific WRONG answer, worse for
	// the next investigation than the vague tag the split replaced. Asserted BEFORE
	// attributability so the regression fails on its own message.
	assert(trace_saw(WDS_CHUNK_SEND)
		   && "near-send payload fill never reported WDS_CHUNK_SEND -- the acquire's "
			  "tag still spans it");
	// THE D2 REGRESSION GUARD, half two: the scan tag is stamped per ITERATION. A
	// hoisted stamp runs iteration N+1's 441-column double scan under iteration N's
	// send tag, and cannot produce this ordering at all (see trace_rescan_between_sends).
	assert(trace_rescan_between_sends()
		   && "the batch never re-entered WDS_NEAR_SCAN between two sends -- the scan "
			  "tag is hoisted out of the loop and iteration N+1 runs under RPC_SEND");
	// The four tags of the batch decode apart on a hang photograph; none is another's
	// leftover.
	assert(WDS_NEAR_SCAN != WDS_CHUNK_SEND && WDS_CHUNK_SEND != WDS_SLOT_WAIT
		   && WDS_SLOT_WAIT != WDS_RPC_SEND && WDS_NEAR_SCAN != WDS_RPC_SEND);
	printf("  near batch: one forced batch sent %d chunks, ended on RPC after TAIL, and "
		   "re-entered NEAR_SCAN synchronously between sends\n",
		   after - before);

	// A busy near acquire must return with every slot still held. Record its
	// short phase synchronously; a polling observer cannot see a nonblocking
	// call reliably. The guard aborts if the old blocking acquire returns.
	struct chunk_rpc_slot* held[HELD_SLOTS_MAX];
	bool busy_seen = false;
	for(int attempt = 0;
		attempt < PHASE_PARK_TICKS && !busy_seen; attempt++) {
		clin_drain_discard(); // nothing in flight -> the pool is whole
		int held_count = hold_all_chunk_slots(held);
		struct chunk_rpc_slot* spare = clin_chunk_slot_try_acquire();
		assert(!spare && "the fixture must hold every chunk slot");
		const int sent = near_sent_count(server);
		trace_start();
		tick_once_bounded(server);
		trace_stop();
		busy_seen = trace_saw(WDS_SLOT_WAIT);
		for(int i = 0; i < trace_n; i++)
			if(trace_buf[i] == WDS_SLOT_WAIT)
				assert(i + 1 < trace_n && trace_buf[i + 1] == WDS_NEAR_SCAN
					&& "busy acquire's tag must end when the call returns");
		assert(near_sent_count(server) == sent);
		assert(watchdog_phase_srv_current() != WDS_SLOT_WAIT);
		for(int i = 0; i < held_count; i++)
			clin_chunk_slot_release(held[i]);
		clin_drain_discard();
	}
	assert(busy_seen && "the measured batch never reached the busy slot acquire");
	drain_start();
	const int before_retry = near_sent_count(server);
	streaming_tick(server);
	drain_stop();
	assert(near_sent_count(server) > before_retry);
	printf("  near batch: busy acquire returns; unsent chunks publish after slots return\n");
}

// ------------------------------------ (B)+(C) far slot pool + leak alarm -----

// Count LEAK_MARK lines in the captured stderr log, and echo the log to stdout so a
// failing assertion after the redirect is still readable.
static int leak_reports(void) {
	static int echoed = 0; // echo each captured line once, however often we re-count
	fflush(stderr);
	FILE* f = fopen(LEAK_LOG, "rb");
	if(!f)
		return 0;
	int n = 0, seen = 0;
	char line[512];
	while(fgets(line, sizeof(line), f)) {
		if(seen++ >= echoed) {
			fputs("    [captured stderr] ", stdout);
			fputs(line, stdout);
			echoed++;
		}
		if(strstr(line, LEAK_MARK))
			n++;
	}
	fclose(f);
	return n;
}

// Tick until the pool has refused `want` more acquires, or the cap trips. Returns the
// bails observed. The drainer runs throughout and is safe: with no free far slot the
// tick cannot emit a CRPC_FAR_CHUNK, so there is nothing for it to hand back.
static uint32_t tick_until_bails(struct server_local* server, uint32_t base,
								 uint32_t want) {
	uint32_t got = clin_far_slot_busy_count() - base;
	for(int i = 0; i < LEAK_TICK_CAP && got < want; i++) {
		streaming_tick(server);
		clin_drain_discard();
		got = clin_far_slot_busy_count() - base;
	}
	return got;
}

static void test_far_send_bails_when_pool_is_drained(struct server_local* server) {
	// Positive control: with the pool free the far batch really does stream here,
	// so a later "nothing was sent" is attributable to the pool and nothing else.
	drain_start();
	for(int attempt = 0;
		attempt < 64 && dict_far_sent_size(server->world.far_sent) == 0; attempt++) {
		streaming_tick(server);
		clin_drain_discard();
	}
	assert(dict_far_sent_size(server->world.far_sent) > 0);
	// Park the drainer: from here the FIXTURE owns the client side, so no far slot
	// can be recycled behind the measurement.
	drain_stop();

	// Take every free far slot and HOLD it: the client half of the pipe has stalled.
	struct far_rpc_slot* held[HELD_SLOTS_MAX];
	int held_count = 0;
	while(held_count < HELD_SLOTS_MAX) {
		struct far_rpc_slot* slot = clin_far_slot_try_acquire();
		if(!slot)
			break;
		held[held_count++] = slot;
	}
	assert(held_count > 0 && held_count < HELD_SLOTS_MAX);
	assert(!clin_far_slot_try_acquire());

	const size_t far_before = dict_far_sent_size(server->world.far_sent);
	const uint32_t busy_before = clin_far_slot_busy_count();

	// THE regression guard: EVERY tick RETURNS. Under the old blocking acquire the
	// first one never completes -- the server thread parks holding the whole
	// gen/stream pipeline, which is how the near ring stops draining too.
	//
	// The far batch is gated per tick (a deferred near unload closes it for that
	// tick), so requiring exactly ONE tick to reach the pool would be a flake rather
	// than a stronger claim. Tick until the pool is consulted; the bail assertions
	// hold for every tick in between.
	uint32_t busy_after = busy_before;
	for(int i = 0; i < FAR_BAIL_TICKS && busy_after == busy_before; i++) {
		tick_once_bounded(server);
		busy_after = clin_far_slot_busy_count();
		// No column may be marked far_sent while the pool is drained: the batch
		// DEFERS, so every skipped column stays claimable next tick.
		assert(dict_far_sent_size(server->world.far_sent) == far_before);
	}
	// ... and it really took the bail, not a lucky empty scan.
	assert(busy_after > busy_before);
	puts("  far slot pool: exhaustion defers the batch, never parks the tick");

	// ---- (C) the leak alarm ------------------------------------------------
	// Everything from here writes to a captured stderr; leak_reports() echoes it back
	// to stdout so an assertion that fires after the redirect is still readable.
	FILE* captured = freopen(LEAK_LOG, "w", stderr);
	assert(captured);
	drain_start();

	// A SHORT run of bails is ordinary backpressure and must stay silent, or the
	// alarm is noise and gets ignored -- which is the same as not having one.
	uint32_t got = tick_until_bails(server, busy_before, LEAK_BAILS_QUIET);
	assert(got >= LEAK_BAILS_QUIET);
	int quiet = leak_reports();

	// A run that does NOT end is a leak: the pool has slots the client will never
	// return, and far streaming is dead. Exactly one line per episode.
	got = tick_until_bails(server, busy_before, LEAK_BAILS_TARGET);
	assert(got >= LEAK_BAILS_TARGET);
	int reported = leak_reports();
	// The number the SHIPPING build can actually read. The stderr line above is the whole
	// alarm on PC, but main.c calls SYS_STDIO_Report(true) only under !NDEBUG and the Wii
	// Makefile compiles -DNDEBUG, so on hardware it goes nowhere: the run is published
	// through server_local_far_slot_starve() to the F3 far row and the watchdog hang
	// report instead. Sampled here, where the pool is provably starved.
	const uint32_t starve_peak = server_local_far_slot_starve();

	// Hand the pool back and stream again, which ENDS the episode and resets the run
	// -- then starve it a second time. A missing reset would leave the counter far
	// past its threshold, and the second episode would pass in total silence.
	drain_stop();
	for(int i = 0; i < held_count; i++)
		clin_far_slot_release(held[i]);
	drain_start();
	for(int attempt = 0;
		attempt < 64 && dict_far_sent_size(server->world.far_sent) == far_before;
		attempt++) {
		streaming_tick(server);
		clin_drain_discard();
	}
	const size_t far_after_release = dict_far_sent_size(server->world.far_sent);
	const uint32_t starve_after_release = server_local_far_slot_starve();
	drain_stop();

	held_count = 0;
	while(held_count < HELD_SLOTS_MAX) {
		struct far_rpc_slot* slot = clin_far_slot_try_acquire();
		if(!slot)
			break;
		held[held_count++] = slot;
	}
	drain_start();
	const uint32_t busy_second = clin_far_slot_busy_count();
	got = tick_until_bails(server, busy_second, LEAK_BAILS_TARGET);
	int reported_twice = leak_reports();
	drain_stop();
	for(int i = 0; i < held_count; i++)
		clin_far_slot_release(held[i]);

	// stderr is a captured file from here on; assertions below print to stdout first.
	printf("  far slot pool: %d report(s) after %u quiet bails, %d after the leak "
		   "run, %d after a second episode (%u bails)\n",
		   quiet, (uint32_t)LEAK_BAILS_QUIET, reported, reported_twice, got);
	assert(quiet == 0 && "ordinary far backpressure must not report a leak");
	assert(reported == 1
		   && "a far slot pool that never recovers must report EXACTLY once");
	assert(far_after_release > far_before
		   && "releasing the pool did not restore far streaming");
	assert(got >= LEAK_BAILS_TARGET && "the second episode never reached the pool");
	assert(reported_twice == 2
		   && "a second starvation episode was silent -- the consecutive-bail run is "
			  "not reset by a successful acquire");
	puts("  far slot pool: a run that never ends reports once per episode");

	// The published dial, which is what the F3 far row and the watchdog report show. A
	// getter stubbed out (or a run that never resets) makes a real leak read as ordinary
	// backpressure on the only instrument hardware has.
	printf("  far slot pool: starve run peaked at %u, back to %u after the pool "
		   "recovered\n",
		   starve_peak, starve_after_release);
	assert(starve_peak >= (uint32_t)LEAK_BAILS_TARGET
		   && "server_local_far_slot_starve() does not report the consecutive-bail run "
			  "the F3 row and the hang report read");
	assert(starve_after_release == 0
		   && "the published starvation run survived a successful acquire, so ordinary "
			  "backpressure would read as a dead pool");
}

// --------------------------------------------- FIX #1: disk-probe reorder ---
// server_world_disk_has_chunk pays a real SD probe. The near-send scan gates
// on `generator != NULL || disk_has_chunk(...)`; every real world (this
// fixture's Nether included) always has a generator, so the probe must never
// be reached once the check is ordered correctly. Declared here (not in
// server_local.h) matching this file's existing extern-in-the-test-TU
// convention for narrow test-only hooks (see test_portal_travel.c).
extern unsigned server_local_test_near_send_disk_probe_calls(void);
extern void server_local_test_reset_near_send_disk_probe_calls(void);

static void test_near_send_disk_probe_order(void) {
	struct server_local* server = fixture_create();
	server_local_test_reset_near_send_disk_probe_calls();
	// converge_near_ring drives the near-send scan across the WHOLE ring
	// (441 candidate checks per tick, many ticks) -- the strongest exercise
	// of the predicate available, and it also re-proves the reordered
	// operands are equivalent: the ring still converges completely, exactly
	// as before the reorder (OR is commutative; only which operand pays for
	// the SD probe changed).
	converge_near_ring(server);
	assert(near_sent_count(server) == NEAR_RING_COLUMNS);
	unsigned probes = server_local_test_near_send_disk_probe_calls();
	printf("  disk probe order: %u SD probe(s) during a full near-ring "
		   "convergence (generator != NULL the whole time)\n",
		   probes);
	assert(probes == 0
		   && "server_world_disk_has_chunk was reached despite a live "
			  "generator -- the near-send predicate is not short-circuiting "
			  "on the always-true generator check first");
	fixture_destroy(server);
}

// ------------------------- FIX (movement flake, 2026-09-02): edge-column ---
// -------------------------------------------------- transient-miss latency --
// Maintainer field report on a moving player: walking a few chunks from
// spawn into already-loaded terrain, the blocks right in front of the
// player stayed at the far-ring skin quality ("smudged / far-away look")
// and never promoted to near-detail -- the near ring never catching up
// while the player moves. Root cause: 17e921c's stuck-chunk fix jumps a
// chunk straight to the full NEAR_SEND_FAIL_RETRY_TICKS (20-tick) backoff
// on its VERY FIRST ensure_populated miss, but a chunk newly entering view
// as the player walks is, by construction, ALWAYS seeing that chunk for
// the first time -- a single transient "not ready yet" is the NORMAL case,
// not evidence of a stuck chunk, yet it paid the same 20-tick penalty as a
// permanently-failing one.
//
// This test walks the player one chunk at a time across freshly-generated
// terrain (a real superflat Nether world, not a modeled/frontier fixture),
// forcing exactly the FIRST ensure_populated attempt for each newly-exposed
// edge column to fail (server_world_test_fail_gen_attempts(1), armed right
// before the step -- the same fault-injection seam test_near_send_skips_
// stuck_chunk below uses for a PERMANENTLY-failing chunk; here it fires
// once and self-clears, modeling a transiently-not-ready one). The new
// edge column -- offset (MAX_VIEW_DISTANCE, 0) from the player's new
// position -- is the unique nearest unsent candidate after each step (the
// rest of the ring stayed fully converged from the previous step), so the
// scan's own nearest-first order guarantees the forced miss lands on
// exactly that chunk, not some other one.
//
// WALK_EDGE_CONVERGE_TICKS_BOUND is the regression gate: pre-fix, the one
// forced miss backs the chunk off for NEAR_SEND_FAIL_RETRY_TICKS (20)
// ticks before it is even eligible for retry again, so it CANNOT converge
// within a small bound -- this assertion fails on tip code. Post-fix, a
// single miss (streak 1, below NEAR_SEND_FAIL_STREAK_THRESHOLD=3) costs at
// most one tick before retry, and the real generator's own natural
// convergence for one already-warm superflat Nether quadrant is fast, so
// it converges well inside the bound. The outer loop itself stays
// generously wall-clock-bounded (mirrors converge_near_ring's own
// reasoning above) so machine load can never turn this into an unrelated
// timeout -- only the STRICT tick-count assertion below encodes the actual
// regression proof.
#define WALK_STEPS 6
#define WALK_EDGE_CONVERGE_TICKS_BOUND 10 // generous vs. a real 1-2 tick converge; pre-fix needs >= 20
#define WALK_EDGE_CONVERGE_TIMEOUT_MS 10000 // load-tolerant outer bound; not the regression gate
static void test_near_send_walk_edge_transient_miss(void) {
	struct server_local* server = fixture_create();

	// Converge the initial near ring around spawn with NO fault injection --
	// every edge chunk the walk exposes below therefore starts completely
	// untouched, exactly like a player walking into fresh terrain rather
	// than a pre-warmed one.
	converge_near_ring(server);

	for(int step = 1; step <= WALK_STEPS; step++) {
		// Move the player exactly one chunk east. The near ring's new
		// rightmost column (px+MAX_VIEW_DISTANCE) is now entirely unsent;
		// the rest of the window was already converged by the previous
		// step (or the initial converge_near_ring call).
		server->player.x += CHUNK_SIZE;
		w_coord_t new_edge_x
			= WCOORD_CHUNK_OFFSET((w_coord_t)server->player.x) + MAX_VIEW_DISTANCE;
		w_coord_t center_z = WCOORD_CHUNK_OFFSET((w_coord_t)server->player.z);
		assert(!server_world_is_chunk_sent(&server->world, new_edge_x, center_z)
			   && "the new edge column must start unsent -- this walk step "
				  "does not actually exercise a freshly-exposed chunk");

		// Force exactly the FIRST ensure_populated attempt for this brand-new
		// edge chunk to fail -- a transiently-not-ready chunk, not a
		// permanently-stuck one (server_world_test_fail_gen_attempts(500)
		// below models the latter).
		server_world_test_fail_gen_attempts(1);

		drain_start();
		bool sent = false;
		int ticks_to_converge = 0;
		ptime_t step_start = time_get();
		while(!sent
			  && time_diff_ms(step_start, time_get())
					 < WALK_EDGE_CONVERGE_TIMEOUT_MS) {
			streaming_tick(server);
			clin_drain_discard();
			ticks_to_converge++;
			sent = server_world_is_chunk_sent(&server->world, new_edge_x, center_z);
		}
		drain_stop();

		printf("  walk step %d: edge chunk (%d,%d) sent=%d after %d tick(s) "
			   "(regression bound %d)\n",
			   step, (int)new_edge_x, (int)center_z, sent, ticks_to_converge,
			   WALK_EDGE_CONVERGE_TICKS_BOUND);
		// assert() -> abort() never flushes stdio buffers, so a fully-
		// buffered (redirected-to-file) run would otherwise silently lose
		// every "walk step" line printed before an imminent failed
		// assertion -- flush explicitly so the bisect proof survives.
		fflush(stdout);
		assert(sent && "edge chunk never converged -- see WALK_EDGE_CONVERGE_"
					   "TIMEOUT_MS, a load/generator problem unrelated to "
					   "the backoff regression this test targets");
		assert(ticks_to_converge <= WALK_EDGE_CONVERGE_TICKS_BOUND
			   && "a single transient ensure_populated miss on a chunk newly "
				  "entering view cost far more than a couple of ticks -- the "
				  "near-send backoff is applying its long delay on the FIRST "
				  "failure instead of only after repeated ones, exactly the "
				  "movement-flake regression this test guards");
		assert(server_world_test_gen_failures_remaining() == 0
			   && "the single forced failure was never consumed -- the walk "
				  "step's near-send scan did not actually attempt the new "
				  "edge chunk first, so this run did not exercise what it "
				  "claims to");
	}

	fixture_destroy(server);
}

// --------------------------------- FIX #2: near-send stuck-chunk backoff ---
// A chunk whose server_world_ensure_populated call keeps failing (here:
// forced via server_world_test_fail_gen_attempts, the existing gen-failure
// fault-injection seam) must not freeze the whole near-send batch behind it
// forever -- a farther, already-populated-and-unsent chunk must still stream.
static void test_deferred_generation_respects_batch_budget(void) {
	struct server_local* server = fixture_create();
	prepare_unsent_near_chunk(server, 0, -1);
	server_world_test_fail_gen_attempts(500);
	server_local_test_expire_near_budget(true);
	drain_start();
	streaming_tick(server);
	assert(server_world_test_gen_failures_remaining() < 500);
	assert(!server_world_is_chunk_sent(&server->world, 0, 0));
	assert(!server_world_is_chunk_sent(&server->world, 0, -1));
	server_local_test_expire_near_budget(false);
	for(int i = 0; i < 200 && !server_world_is_chunk_sent(&server->world,0,-1); i++)
		streaming_tick(server);
	assert(server_world_is_chunk_sent(&server->world,0,-1));
	drain_stop();
	fixture_destroy(server);
	puts("Deferred budget PASS: expired failed attempt stops batch; next ticks still bypass a stuck chunk");
}

static void test_near_send_skips_stuck_chunk(void) {
	struct server_local* server = fixture_create();

	// Fully prepare the SECOND-nearest column from spawn (north, (0,-1) --
	// the same deterministic pick order test_near_send_phase_tags documents
	// for this fixed spawn) WITHOUT marking it sent, and WITHOUT arming the
	// failure seam yet -- this chunk must reach full readiness normally.
	prepare_unsent_near_chunk(server, 0, -1);
	assert(!server_world_is_chunk_sent(&server->world, 0, -1));
	assert(!server_world_is_chunk_sent(&server->world, 0, 0));

	// Now force every FRESH generation attempt to fail. (0,0) has not been
	// touched above, so its covering quadrants still need real generation and
	// this makes that generation permanently fail; (0,-1)'s covering quadrants
	// (including its own origin) are already fully populated, so it needs no
	// further server_world_load_chunk call and is unaffected by the seam.
	server_world_test_fail_gen_attempts(500);

	drain_start();
	bool north_sent = false;
	for(int i = 0; i < 200 && !north_sent; i++) {
		streaming_tick(server);
		clin_drain_discard();
		north_sent = server_world_is_chunk_sent(&server->world, 0, -1);
	}
	drain_stop();

	printf("  stuck chunk: (0,-1) sent=%d after up to 200 ticks with (0,0) "
		   "permanently deferring; %u forced gen-failure budget left\n",
		   north_sent, server_world_test_gen_failures_remaining());
	// The permanently-deferring nearest chunk must NEVER itself succeed here
	// -- this is the datum that proves the scenario really exercises a chunk
	// that keeps failing, not one that eventually converges on its own.
	assert(!server_world_is_chunk_sent(&server->world, 0, 0)
		   && "the forced-failure chunk unexpectedly got sent -- the fixture "
			  "does not actually exercise a permanently-deferring chunk");
	// THE regression guard: pre-fix, the near-send loop `break`s the entire
	// batch on (0,0)'s first ensure_populated failure and re-picks the SAME
	// nearest chunk every following tick, so (0,-1) is never reached and this
	// fails. Post-fix, (0,0)'s failure is marked backed-off and the scan
	// advances to (0,-1), which sends within a handful of ticks.
	assert(north_sent
		   && "a farther, already-populated chunk never streamed while the "
			  "nearest chunk kept deferring -- the near-send batch froze "
			  "behind the stuck chunk");
	// The countdown must still have plenty left: a premature exhaustion would
	// mean (0,0) somehow stopped being attempted (e.g. wrongly treated as
	// permanently skippable) rather than genuinely backing off and retrying.
	assert(server_world_test_gen_failures_remaining() > 0);

	fixture_destroy(server);
}

// ------------------------------- FIX #4: orphan-container retry sweep gate --
// server_local_retry_orphaned_container_drops (a MAX_CHESTS+MAX_FURNACES
// linear scan) used to run unconditionally every tick. It must now run ONLY
// while something is actually pending, and must still recover a genuinely
// retained row once it does run. Declared here (not server_local.h) matching
// this file's existing extern-in-the-test-TU convention.
extern unsigned server_local_test_orphan_retry_sweep_calls(void);
extern bool server_local_test_orphan_retry_pending(
	const struct server_local* s);

static void test_orphan_retry_sweep_gate(void) {
	struct server_local* server = fixture_create();
	// A resident, populated chunk to hold the chest -- reuses the same
	// helper the near-send tests use to guarantee (0,0) is loaded.
	prepare_unsent_near_chunk(server, 0, 0);
	// A real consumer MUST run beside every streaming_tick here: random ticks
	// (grass spread, etc.) and the chest-break RPC below all queue CRPC_*
	// messages, and clin_rpc_send BLOCKS once the bounded token pool fills
	// with nothing draining it -- exactly the drain_thread every OTHER
	// multi-tick loop in this file already runs beside its ticks.
	drain_start();

	// FIX #4b: server_local_load_world_impl_ex now ARMS the pending flag on
	// every load (instead of clearing it), so a container row that was
	// already orphaned when the save was written (a save taken mid-retry, or
	// a crash while a row was retained -- the chest/furnace tables persist
	// with the world) still gets one recovery pass on the very first tick
	// after finished_loading, instead of waiting for some unrelated
	// container to be destroyed first. fixture_create() wires up level/world
	// by hand and sets finished_loading=true directly rather than routing
	// through the real load function (see its body), so model exactly what
	// that function now leaves behind immediately after a load: the flag
	// armed, nothing yet pending.
	assert(!server_local_test_orphan_retry_pending(server));
	server->orphan_container_retry_pending = true;

	// Case 1: an ordinary load with nothing actually orphaned. The armed
	// flag must still cost exactly one sweep pass on the first tick (finds
	// nothing, self-clears via the existing `= !complete`), then go idle.
	unsigned calls_before_load_tick
		= server_local_test_orphan_retry_sweep_calls();
	streaming_tick(server);
	unsigned load_tick_calls = server_local_test_orphan_retry_sweep_calls()
								- calls_before_load_tick;
	printf("  orphan retry gate: %u sweep call(s) on first tick after load "
		   "(want 1)\n",
		   load_tick_calls);
	assert(load_tick_calls == 1
		   && "the load-time arm did not run exactly one sweep pass");
	assert(!server_local_test_orphan_retry_pending(server)
		   && "the load-time pass found nothing yet left the flag armed");

	unsigned calls_before = server_local_test_orphan_retry_sweep_calls();
	// Nothing pending: several more ticks must NOT invoke the sweep at all.
	for(int i = 0; i < 10; i++)
		streaming_tick(server);
	unsigned calls_idle = server_local_test_orphan_retry_sweep_calls();
	printf("  orphan retry gate: %u sweep call(s) over 10 idle ticks (want 0)\n",
		   calls_idle - calls_before);
	assert(calls_idle == calls_before
		   && "the per-tick sweep ran despite nothing being pending");

	// Case 2 (FIX #4b positive case): a row that was ALREADY orphaned at
	// load time -- no destroy event fires for it this session at all. Seeded
	// at a second slot exactly the way the archive reader would have
	// repopulated chest_pos/chest_items from a save taken mid-retry: the
	// block is gone, but the row still names it.
	const w_coord_t lx = 3, ly = 4, lz = 2;
	(void)server_world_set_block(
		&server->worlds[0], lx, ly, lz, (struct block_data) {.type = BLOCK_AIR});
	struct block_data load_confirm_air;
	assert(server_world_get_block(&server->world, lx, ly, lz, &load_confirm_air)
		   && load_confirm_air.type != BLOCK_CHEST
		   && load_confirm_air.type != BLOCK_IRON_CHEST);
	server->chest_pos[1] = (struct complex_block_pos) {lx, ly, lz};
	A1B_STAMP(server, chest, 1, &server->worlds[0]);
	memset(server->chest_items[1], 0, sizeof(server->chest_items[1]));
	server->chest_items[1][0]
		= (struct item_data) {.id = ITEM_APPLE, .durability = 0, .count = 2};
	// Model server_local_load_world_impl_ex's unconditional arm directly
	// (fixture_create bypasses the real load call -- see the comment above);
	// no destroy-event entry point is invoked for this row.
	server->orphan_container_retry_pending = true;
	unsigned calls_load_seeded = server_local_test_orphan_retry_sweep_calls();
	streaming_tick(server);
	unsigned load_seeded_calls
		= server_local_test_orphan_retry_sweep_calls() - calls_load_seeded;
	printf("  orphan retry gate: chest_pos[1].y=%d after load-time pass, "
		   "%u sweep call(s) (want 1)\n",
		   server->chest_pos[1].y, load_seeded_calls);
	assert(server->chest_pos[1].y < 0
		   && "the load-time pass never recovered a row orphaned before load");
	assert(load_seeded_calls == 1
		   && "the load-time pass ran more than the one expected sweep");
	assert(!server_local_test_orphan_retry_pending(server)
		   && "the pending flag survived a fully-resolved load-time pass");

	unsigned calls_after_load_seeded
		= server_local_test_orphan_retry_sweep_calls();
	for(int i = 0; i < 10; i++)
		streaming_tick(server);
	assert(server_local_test_orphan_retry_sweep_calls() == calls_after_load_seeded
		   && "the sweep kept running after the load-seeded row was resolved");

	// Case 3 (unchanged): manufacture a retained row exactly like block_chest.c's onBreak leaves
	// one on a failed spill: the block is already gone (AIR), but chest_pos/
	// chest_items still name it -- then arm the SAME OOM fault-injection
	// seam block_chest.c's caller would hit, and call the SAME production
	// entry point (server_local_spawn_destroyed_container_items) it calls.
	const w_coord_t cx = 2, cy = 4, cz = 2;
	// NOT asserted on its return: server_world_set_block reports "changed"
	// (false for an identical id+metadata, e.g. this superflat Nether world's
	// spawn-height column may already BE air) -- what this test actually
	// needs is confirmed below instead: the final block is not a chest.
	(void)server_world_set_block(
		&server->worlds[0], cx, cy, cz, (struct block_data) {.type = BLOCK_AIR});
	struct block_data confirm_air;
	assert(server_world_get_block(&server->world, cx, cy, cz, &confirm_air)
		   && confirm_air.type != BLOCK_CHEST
		   && confirm_air.type != BLOCK_IRON_CHEST);
	server->chest_pos[0] = (struct complex_block_pos) {cx, cy, cz};
	A1B_STAMP(server, chest, 0, &server->worlds[0]);
	memset(server->chest_items[0], 0, sizeof(server->chest_items[0]));
	server->chest_items[0][0]
		= (struct item_data) {.id = ITEM_APPLE, .durability = 0, .count = 3};
	server_local_test_fail_next_container_drop();
	bool spill_ok = server_local_spawn_destroyed_container_items(
		server, cx, cy, cz, server->chest_items[0], CHEST_SIZE_STORAGE);
	assert(!spill_ok && "the armed OOM seam did not fail the spill");
	assert(server->chest_pos[0].y >= 0 && "row cleared despite a failed spill");
	assert(server_local_test_orphan_retry_pending(server)
		   && "a failed spawn_destroyed_container_items call did not arm the "
			  "retry-pending flag");

	// The gate must now let the sweep run and actually recover the row (the
	// fail seam was one-shot, so this retry succeeds).
	unsigned calls_pending = server_local_test_orphan_retry_sweep_calls();
	for(int i = 0; i < 5 && server->chest_pos[0].y >= 0; i++)
		streaming_tick(server);
	printf("  orphan retry gate: chest_pos[0].y=%d after retry, %u sweep "
		   "call(s) while pending\n",
		   server->chest_pos[0].y,
		   server_local_test_orphan_retry_sweep_calls() - calls_pending);
	assert(server->chest_pos[0].y < 0
		   && "the gated sweep never recovered the retained row");
	assert(server_local_test_orphan_retry_sweep_calls() > calls_pending
		   && "the row recovered without the gate ever invoking the sweep");
	assert(!server_local_test_orphan_retry_pending(server)
		   && "the pending flag survived a fully-resolved sweep");

	// Back to idle: the now-clear flag must stop the sweep again.
	unsigned calls_after_clear = server_local_test_orphan_retry_sweep_calls();
	for(int i = 0; i < 10; i++)
		streaming_tick(server);
	assert(server_local_test_orphan_retry_sweep_calls() == calls_after_clear
		   && "the sweep kept running after the pending row was resolved");

	drain_stop();
	fixture_destroy(server);
}

// ------------------------------------ FIX #5: weather send change-gate -----
// CRPC_WEATHER_SET used to go out on every single non-loading tick regardless
// of whether anything changed. Declared here (not server_local.h) matching
// this file's existing extern-in-the-test-TU convention.
extern unsigned server_local_test_weather_set_send_count(void);
extern void server_local_test_reset_weather_set_send_count(void);

static void test_weather_send_change_gate(void) {
	struct server_local* server = fixture_create();
	server_local_test_reset_weather_set_send_count();
	// See test_orphan_retry_sweep_gate's comment: any multi-tick streaming_tick
	// loop needs a live consumer or the RPC token pool eventually fills and
	// clin_rpc_send blocks forever (near-send chunk/sign RPCs queue up here
	// too, independent of weather).
	drain_start();

	// This fixture is a Nether world, and server_weather_tick is a
	// Beta-faithful no-op there (weather never runs in the Nether -- strengths/
	// timers stay frozen at whatever they last held). That makes it a
	// deterministic "nothing ever changes" clock for this send-gate test with
	// no random-toggle risk, independent of the (unrelated, already-faithful)
	// question of what Beta's real Nether weather behaviour is.
	assert(!server->weather.raining && !server->weather.thundering);
	assert(server->weather.rain_strength == 0.0f
		   && server->weather.thunder_strength == 0.0f);
	for(int i = 0; i < 20; i++)
		streaming_tick(server);
	unsigned sends_quiet = server_local_test_weather_set_send_count();
	printf("  weather send gate: %u CRPC_WEATHER_SET send(s) over 20 "
		   "unchanged ticks (want exactly 1 -- the forced first send)\n",
		   sends_quiet);
	assert(sends_quiet == 1
		   && "the change-gate sent more than the one forced first-tick "
			  "CRPC_WEATHER_SET despite the state never changing");

	// Force an observable change directly (this fixture's frozen Nether tick
	// will never do this on its own) and confirm exactly one fresh send
	// follows -- the gate must not silently swallow a real change.
	server->weather.raining = true;
	streaming_tick(server);
	unsigned sends_after_change = server_local_test_weather_set_send_count();
	assert(sends_after_change == sends_quiet + 1
		   && "a genuine weather-state change did not trigger a resend");

	// Settle again at the new state: further unchanged ticks must not add
	// more sends -- the SAME property proven above, now exercised against a
	// non-default snapshot too.
	for(int i = 0; i < 10; i++)
		streaming_tick(server);
	assert(server_local_test_weather_set_send_count() == sends_after_change
		   && "the gate kept sending after the new state stabilized");

	drain_stop();
	fixture_destroy(server);
}

// FIX #3: autosave sweep.
// (a) dispenser/note/jukebox/spawner content edits must re-dirty their
//     owning chunk at each autosave interval, exactly like chest/sign/
//     furnace already do.
// (b) a chunk that keeps re-dirtying itself every tick (an active redstone
//     clock / flowing fluid) must be saved AT MOST ONCE per autosave epoch,
//     not re-saved every tick forever.
//
// This fixture's world keeps ticking during the test (random ticks etc. can
// dirty OTHER chunks throughout), so the test does not try to reach a
// globally "nothing left dirty" state -- it tracks only the ONE chunk it
// cares about, via the dict membership test hook and the chunk's own
// `modified` flag, both of which are meaningful regardless of how much
// unrelated background churn the sweep is also working through.
// NOTE (found while authoring this test): a FRESH dict_server_chunks_get()
// call made from here, for a chunk id confirmed present by iterating the
// same dict seconds earlier (dict_server_chunks_it()), returned NULL for
// more than one coordinate pair tried (including the origin chunk and (0,-1))
// -- yet the SAME id resolves correctly via server_world_ensure_populated's
// own internal lookup. This looks like an m-lib DICT_DEF2 / index-probe
// characteristic independent of this fix (not reproduced through any
// production call path, which never re-queries a chunk this way from outside
// the world/near-send machinery); filed as a follow-up rather than
// investigated further here. Sidestepped below by reusing the pointer
// prepare_unsent_near_chunk already resolved, instead of re-querying.
extern bool server_local_test_autosave_saved_this_epoch(
	struct server_local* s, w_coord_t cx, w_coord_t cz);

#define AUTOSAVE_TEST_CX 0
#define AUTOSAVE_TEST_CZ (-1)

static void test_autosave_sweep(void) {
	struct server_local* server = fixture_create();
	// Reuse the chunk pointer prepare_unsent_near_chunk already resolved
	// successfully, rather than re-querying dict_server_chunks_get from
	// here: a fresh lookup for this exact id was observed to return NULL
	// while dict_server_chunks_it() still enumerated the same entry (see
	// the note above) -- the already-proven-good pointer is used directly
	// instead of chasing that mismatch separately.
	struct server_chunk* chunk = prepare_unsent_near_chunk(
		server, AUTOSAVE_TEST_CX, AUTOSAVE_TEST_CZ);
	// Freeze all FURTHER chunk generation (the existing gen-failure seam --
	// see test_near_send_skips_stuck_chunk above): without this, the
	// ordinary near-send scan keeps streaming/generating brand new chunks
	// every tick for the rest of this test, each one marked modified=true
	// by its own populate() call (server_world.c), so the 1-per-tick sweep
	// would be racing an UNBOUNDED, ever-growing backlog instead of the
	// small, fixed one (the handful of chunks prepare_unsent_near_chunk's
	// own populate spillover already created) this test actually means to
	// exercise.
	server_world_test_fail_gen_attempts(100000);
	drain_start();

	// A dispenser tile entity in the target chunk -- with no other edit ever
	// touching chunk->modified, the ONLY thing that can mark this chunk
	// dirty via the ordinary gameplay path is the interval's own re-dirty
	// pass (FIX #3(a)). This test ALSO drives the chunk dirty directly every
	// tick below, standing in for an always-dirty mechanism (redstone clock /
	// flowing fluid) to exercise FIX #3(b) at the same time.
	const w_coord_t dx = 3, dy = 4, dz = -13; // inside chunk (0,-1): z in [-16,-1]
	server->dispenser_pos[0] = (struct complex_block_pos) {dx, dy, dz};
	A1B_STAMP(server, dispenser, 0, &server->worlds[0]);
	assert(chunk);

	// The interval must span AT LEAST as many ticks as there are resident
	// chunks that can compete for the sweep's one-pick-per-tick attention
	// within a single epoch: prepare_unsent_near_chunk's populate spillover
	// leaves ~9 chunks resident, and random ticks (e.g. grass spread) can
	// keep re-dirtying some of them indefinitely even with generation
	// frozen below. A SHORT interval (tried first; did not work) resets the
	// epoch before the sweep ever reaches a chunk that is not early in dict
	// iteration order -- effectively starving this test's target if it
	// happens to sit late in that (fixed, hash-order) iteration. A longer
	// interval gives the sweep enough picks per epoch to reach every
	// resident candidate, including this one, before the next reset.
	server->autosave_ticks = 20;
	server->autosave_timer = 0;
	assert(!server_local_test_autosave_saved_this_epoch(
		server, AUTOSAVE_TEST_CX, AUTOSAVE_TEST_CZ));

	// Run ticks WITHOUT ever touching chunk->modified directly here: the
	// dispenser registered above is the ONLY thing that can dirty this
	// chunk (nothing else ever edits it), so reaching saved-this-epoch is a
	// clean proof that FIX #3(a)'s dispenser re-dirty loop actually ran and
	// marked it. Bounded generously: several OTHER resident chunks (from
	// prepare_unsent_near_chunk's populate spillover, kept alive by ordinary
	// random ticks even with generation frozen below) also compete for the
	// sweep's one-pick-per-tick attention.
	bool saved = false;
	int wait_ticks;
	for(wait_ticks = 0; wait_ticks < 600 && !saved; wait_ticks++) {
		streaming_tick(server);
		saved = server_local_test_autosave_saved_this_epoch(
			server, AUTOSAVE_TEST_CX, AUTOSAVE_TEST_CZ);
	}
	printf("  autosave sweep: dispenser-only chunk reached saved-this-epoch "
		   "after %d tick(s) (%zu other resident chunks also competed for "
		   "the sweep this epoch)\n",
		   wait_ticks,
		   dict_autosave_saved_size(server->autosave_saved_this_epoch) - 1);
	assert(saved
		   && "the dispenser-only chunk was never re-dirtied/saved by the "
			  "autosave interval within 600 ticks -- FIX #3(a)'s extra "
			  "re-dirty loops are not covering dispenser_pos");

	// FIX #3(b): the chunk is now recorded saved for this epoch. Force it
	// dirty again and run exactly ONE more tick. If the per-epoch dedup is
	// working, the sweep SKIPS an already-saved chunk and `modified` is left
	// untouched (still true, from the forced set just above); the pre-fix
	// behaviour re-saves it on sight, which clears `modified` back to false
	// as part of a successful save (see the WEDGE GUARD comment in
	// server_local.c). This is a single, direct, unambiguous check -- it
	// does not depend on the sweep ever fully disarming. Reset the timer
	// first so this one extra tick cannot coincidentally land on the NEXT
	// interval's own re-arm (which would legitimately reset the epoch and
	// re-save, a false failure unrelated to the dedup this checks).
	server->autosave_timer = 0;
	chunk->modified = true;
	streaming_tick(server);
	printf("  autosave sweep: chunk->modified=%d one tick after its first "
		   "save this epoch (want 1 -- skipped, not re-saved)\n",
		   chunk->modified);
	assert(chunk->modified
		   && "the chunk was saved a SECOND time within the same autosave "
			  "epoch -- FIX #3(b)'s per-epoch dedup did not skip an "
			  "already-saved chunk");

	drain_stop();
	fixture_destroy(server);
}

static void test_far_send_preserves_dirty_blocks(void) {
	struct server_local* server = fixture_create();
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server->world, 6, 0, &chunk));
	assert(chunk);
	const size_t cell = (3*16+4)*128+70;
	chunk->ids[cell] = 45;
	chunk->metadata[cell/2] = 0xB7;
	chunk->lighting_sky[cell/2] = 0;
	chunk->lighting_torch[cell/2] = 0x92;
	chunk->populated = true;
	chunk->modified = true;
	static uint8_t expected[32768+3*16384+256];
	memcpy(expected,chunk->ids,32768);
	memcpy(expected+32768,chunk->metadata,16384);
	memcpy(expected+49152,chunk->lighting_sky,16384);
	memcpy(expected+65536,chunk->lighting_torch,16384);
	memcpy(expected+81920,chunk->heightmap,256);
	struct far_rpc_slot slot = {0};
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	assert(!server_local_test_finish_far_column(server,6,0,chunk,&slot));
	assert(server_world_is_chunk_loaded(&server->world,6,0));
	assert(chunk->modified && chunk->ids[cell] == 45);
	assert(server_local_test_finish_far_column(server,6,0,chunk,&slot));
	assert(!server_world_is_chunk_loaded(&server->world,6,0));
	assert(server_world_load_chunk(&server->world,6,0,&chunk));
	assert(chunk->populated);
	assert(memcmp(expected,chunk->ids,32768)==0);
	assert(memcmp(expected+32768,chunk->metadata,16384)==0);
	assert(memcmp(expected+49152,chunk->lighting_sky,16384)==0);
	assert(memcmp(expected+65536,chunk->lighting_torch,16384)==0);
	assert(memcmp(expected+81920,chunk->heightmap,256)==0);
	/* A completed far column can still support the unfinished near edge. */
	server->world.gen_pending[0] = true;
	server->world.gen_pending_x[0] = 5;
	server->world.gen_pending_z[0] = 0;
	assert(server_local_test_finish_far_column(server,6,0,chunk,&slot));
	assert(server_world_is_chunk_loaded(&server->world,6,0));
	assert(chunk->ids[cell] == 45);
	server->world.gen_pending[0] = false;
	assert(server_local_test_finish_far_column(server,6,0,chunk,&slot));
	assert(!server_world_is_chunk_loaded(&server->world,6,0));
	puts("Far overlap PASS: completion retains unfinished near support, releases after completion");
	fixture_destroy(server);
	puts("Far send persistence PASS: failed write retains owner; dirty blocks, metadata, sky/block light and population survive retry/eviction/reload");
}

int main(void) {
	test_fixture_dir("test_far_slot_deadlock_stderr.txt", s_fixture_LEAK_LOG, sizeof s_fixture_LEAK_LOG);
	assert(clin_init());
	blocks_init();
	struct server_local* server = fixture_create();
	test_near_send_phase_tags(server);
	converge_near_ring(server);
	test_far_send_bails_when_pool_is_drained(server);
	fixture_destroy(server);
	test_near_send_disk_probe_order();
	test_near_send_walk_edge_transient_miss();
	test_near_send_skips_stuck_chunk();
	test_deferred_generation_respects_batch_budget();
	test_orphan_retry_sweep_gate();
	test_weather_send_change_gate();
	test_autosave_sweep();
	test_far_send_preserves_dirty_blocks();
	printf("test_far_slot_deadlock_env: all assertions passed\n");
	test_fixture_dir_cleanup(LEAK_LOG);
	return 0;
}
