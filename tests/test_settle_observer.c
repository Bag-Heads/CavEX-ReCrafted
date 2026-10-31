/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "chunk.h"
#include "parson/parson.h"
#include "platform/hw_telemetry.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "settle_observer.h"
#include "tps_observer.h"
#include "world.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static uint64_t now;
static unsigned records, max_record;
static FILE* capture;
static uint64_t fake_clock(void) {
	return ++now;
}
static bool sink(const char* line) {
	size_t len = strlen(line);
	assert(len <= SETTLE_RECORD_BODY_MAX);
	JSON_Value* v = json_parse_string(line);
	assert(v && json_value_get_type(v) == JSONObject);
	json_value_free(v);
	if(len > max_record)
		max_record = (unsigned)len;
	records++;
	if(capture)
		fprintf(capture, "%s\n", line);
	return true;
}
static bool discard(const char* line) {
	(void)line;
	return true;
}
static bool fail_sink(const char* line) {
	(void)line;
	return false;
}
static void note(enum settle_stage st, struct chunk* c, const void* tok) {
	SETTLE(mesher_settle_observe, st, c, tok, 0, 0, 0, 0);
}
static struct settle_counts check(void) {
	struct settle_counts c;
	settle_observer_counts(&c);
	assert(c.valid);
	assert(c.enqueue == c.dequeue + c.rpc_outstanding);
	assert(c.submitted == c.completed + c.cancelled + c.outstanding);
	assert(c.submitted
		   == c.worker_dequeued + c.queue_cancelled + c.queue_outstanding);
	assert(c.completed
		   == c.accepted + c.stale + c.retry + c.result_cancelled
			   + c.result_outstanding);
	return c;
}
static void finished(struct chunk* c, const void* tok,
					 enum settle_stage terminal) {
	note(SET_GATHER, c, tok);
	check();
	note(SET_SUBMIT, NULL, tok);
	check();
	note(SET_JOB_DEQUEUE, NULL, tok);
	check();
	note(SET_WORKER, NULL, tok);
	check();
	note(SET_EMIT_BEGIN, NULL, tok);
	note(SET_VL_BEGIN, NULL, tok);
	note(SET_EMIT_BEGIN, NULL, tok);
	note(SET_SLEEP_BEGIN, NULL, NULL);
	note(SET_SLEEP_END, NULL, NULL);
	note(SET_FIN_BEGIN, NULL, tok);
	note(SET_BFS_BEGIN, NULL, tok);
	struct settle_phases phases = {9, 2, 3, 1, 1};
	SETTLE_DETAIL(mesher_settle_observe, SET_PHASES, tok, &phases);
	note(SET_COMPLETE, NULL, tok);
	check();
	if(terminal == SET_CANCEL)
		note(terminal, NULL, tok);
	else {
		note(SET_CONSUME, NULL, tok);
		SETTLE_DETAIL(mesher_settle_observe, terminal, tok, c);
	}
	check();
}
/* THE SETTLE STALL (2026-09-07, lane/settle-stall-repro).
 *
 * The chunk sidecar is an open-addressed table whose scan used to run the FULL
 * SETTLE_CHUNK_CAP on every miss, because the only early exit is a NEVER-USED
 * slot and one world reset guarantees every slot has been used: world.c's
 * world_release_section_chunks emits SET_RETIRE with create=true for EVERY
 * chunk in the old world before any of them is destroyed, so the table is
 * driven past capacity in a single teardown, and from then on every chunk-
 * bearing callback -- SET_DIRTY_*, SET_VISIT, SET_GATE_*, SET_DRAW, on MAIN,
 * under the observer mutex -- walked 1,024 slots to conclude "not here".
 *
 * MEASURED, not assumed (PC split_anchor_6m seed 42 sep 256 fv6 --cw-settle,
 * 147 s): 105,973,129 callbacks, 81,392,199 of them lost in exactly this path
 * = ~8.3e10 probe iterations, ~5.7e8/s. The pre-existing census (census_scan)
 * missed it by construction: it builds 121 * COLUMN_HEIGHT = 968 chunks, just
 * UNDER the 1,024 slots, so it never crosses the cliff it was meant to bound.
 *
 * This test crosses it deliberately and asserts the per-callback cost is
 * O(SETTLE_CHUNK_PROBES), not O(SETTLE_CHUNK_CAP). It FAILS on the pre-fix
 * observer: build with -DSETTLE_CHUNK_PROBES=1024u and the single post-
 * saturation lookup below costs 1,024 probes instead of at most 16. */
/* ABSOLUTE, deliberately NOT written in terms of SETTLE_CHUNK_PROBES: the
 * claim under test is "O(1) per callback, independent of the table size", so
 * an assert that scales with the window would pass on the pre-fix observer
 * too. Measured with -DSETTLE_CHUNK_PROBES=1024u (the pre-fix full-table
 * scan) this same storm costs 1,050,624 probes over 2,049 calls = 513/call
 * and the asserts below fail; at the shipped window it costs 17,424 = 8.5. */
#define PROBE_BUDGET 32u
static struct chunk saturate_shells[SETTLE_CHUNK_CAP * 2];
static void test_chunk_probe_bounded(void) {
	assert(settle_observer_start());
	struct settle_counts armed;
	settle_observer_counts(&armed);
	assert(armed.probes == 0);
	/* A world teardown: every chunk retired (create=true), none destroyed yet. */
	for(unsigned i = 0; i < SETTLE_CHUNK_CAP * 2; i++)
		SETTLE(world_settle_observe, SET_RETIRE, &saturate_shells[i], NULL, 0,
			   0, 0, 0);
	struct settle_counts filled;
	settle_observer_counts(&filled);
	/* Linear in the callbacks, never callbacks x table size. */
	assert(filled.probes <= (uint64_t)SETTLE_CHUNK_CAP * 2 * PROBE_BUDGET);
	/* The reload's first dirty chunk, arriving on a saturated table: this is
	 * the exact callback the hardware stall spends its main thread inside. */
	struct chunk fresh = {0};
	SETTLE(chunk_settle_observe, SET_DIRTY_LIGHT, &fresh, NULL, 0, 0, 0, 0);
	struct settle_counts after;
	settle_observer_counts(&after);
	assert(after.probes - filled.probes <= PROBE_BUDGET);
	/* Bounding the window must not cost a LOOKUP: an entry is never displaced
	 * further than the window it was inserted through, so a chunk the table
	 * did admit is still found in O(1) with its identity intact. */
	uint64_t hit_before = after.probes;
	struct settle_counts hit;
	SETTLE(world_settle_observe, SET_RETIRE, &saturate_shells[0], NULL, 0, 0, 0,
		   0);
	settle_observer_counts(&hit);
	assert(hit.probes - hit_before <= PROBE_BUDGET);
	printf("PASS settle chunk probe bound: %u slots, window %u, saturating "
		   "storm %" PRIu64 " probes over %" PRIu64 " calls (<= %u/call)\n",
		   (unsigned)SETTLE_CHUNK_CAP, (unsigned)SETTLE_CHUNK_PROBES,
		   after.probes, after.calls, (unsigned)PROBE_BUDGET);
	settle_observer_stop(discard);
}
static void accounting(void) {
	assert(settle_observer_start());
	struct chunk c = {0}, before;
	c.x = -32;
	c.z = 48;
	c.y = 16;
	c.dirty_generation = 7;
	memcpy(&before, &c, sizeof(c));
	int tokens[8];
	for(unsigned i = 0; i < 8; i++) {
		note(SET_DIRTY_INSTALL, &c, NULL);
		finished(&c, &tokens[i],
				 i == 0		? SET_ACCEPT :
					 i == 1 ? SET_STALE :
					 i == 2 ? SET_RETRY :
							  SET_CANCEL);
	}
	/* Shutdown removes queued requests as well as completed results. */
	note(SET_GATHER, &c, &tokens[0]);
	note(SET_SUBMIT, NULL, &tokens[0]);
	note(SET_CANCEL, NULL, &tokens[0]);
	/* Worker dequeued but stop arrived before build. */
	note(SET_GATHER, &c, &tokens[0]);
	note(SET_SUBMIT, NULL, &tokens[0]);
	note(SET_JOB_DEQUEUE, NULL, &tokens[0]);
	note(SET_WORKER, NULL, &tokens[0]);
	note(SET_CANCEL, NULL, &tokens[0]);
	struct settle_counts stats = check();
	assert(stats.submitted == 10 && stats.completed == 8
		   && stats.cancelled == 2);
	assert(stats.accepted == 1 && stats.stale == 1 && stats.retry == 1
		   && stats.result_cancelled == 5);
	assert(stats.queue_cancelled == 1 && !stats.queue_outstanding);
	/* Same envelope address reused immediately after main completes install. */
	for(unsigned far = 0; far < 2; far++)
		for(unsigned i = 0; i < 3; i++) {
			const enum settle_stage seq[]
				= {SET_READY,		  SET_SLOT_BEGIN, SET_SLOT_END,
				   SET_SEND_BEGIN,	  SET_ENQUEUE,	  SET_DEQUEUE,
				   SET_INSTALL_BEGIN, SET_INSTALL_OK, SET_SEND_END};
			for(unsigned j = 0; j < sizeof(seq) / sizeof(seq[0]); j++) {
				SETTLE(client_settle_observe, seq[j], NULL, &tokens[0], 1, -2,
					   3, far);
				check();
			}
		}
	assert(check().enqueue == 6 && !check().rpc_outstanding);
	assert(memcmp(&before, &c, sizeof(c)) == 0);
	unsigned before_records = records;
	settle_observer_publish(sink);
	assert(records == before_records + 1);
	check();
	settle_observer_stop(sink);
}
static void invalidation(void) {
	assert(settle_observer_start());
	struct chunk c = {0};
	for(unsigned i = 0; i < SETTLE_RING_CAP + 7; i++)
		note(SET_DIRTY_LIGHT, &c, NULL);
	struct settle_counts stats;
	settle_observer_counts(&stats);
	assert(stats.valid && stats.overflow == 7
		   && stats.ring_used == SETTLE_RING_CAP);
	/* No per-pop timestamps: only the startup clock has been read. */
	assert(stats.clock_reads == 1);
	settle_observer_publish(discard);
	settle_observer_counts(&stats);
	assert(stats.valid && stats.overflow == 7);
	settle_observer_stop(discard);
	assert(settle_observer_start());
	settle_observer_publish(fail_sink);
	settle_observer_counts(&stats);
	assert(!stats.valid && stats.lost > 0);
	settle_observer_stop(discard);
}
static void episode(void) {
	assert(settle_observer_start());
	struct world w;
	world_create(&w);
	struct chunk c = {0};
	c.rebuild_displist = true;
	c.dirty_generation = 1;
	struct world_section sec = {0};
	sec.column[0] = &c;
	dict_wsection_set_at(w.sections, SECTION_TO_ID(0, 0), sec);
	settle_observer_frame(&w, false, 0, 0, 0, false, 0);
	note(SET_DIRTY_INSTALL, &c, NULL);
	int token;
	finished(&c, &token, SET_ACCEPT);
	c.rebuild_displist = false;
	settle_observer_frame(&w, false, 0, 0, 0, true, 8);
	note(SET_DRAW, &c, NULL);
	struct settle_event raw[SETTLE_PULL_CAP];
	size_t used = 0;
	assert(!settle_observer_pull(raw, 1, &used) && !used);
	assert(settle_observer_pull(raw, SETTLE_PULL_CAP, &used) && used);
	assert(used * sizeof(*raw) <= 16384);
	assert((raw[0].stage & 63) == SET_EP_BEGIN);
	assert((raw[used - 1].stage & 63) == SET_EP_END);
	/* Cancelled crossing, then an unfinished final episode. */
	settle_observer_frame(&w, true, 1, 0, 0, true, 8);
	settle_observer_frame(&w, true, 2, 0, 0, true, 8);
	unsigned before_records = records;
	settle_observer_publish(sink);
	assert(records == before_records + 1);
	check();
	settle_observer_finish();
	settle_observer_publish(sink);
	settle_observer_stop(sink);
	/* This test's shell is stack-owned, not world-owned. */
	dict_wsection_get(w.sections, SECTION_TO_ID(0, 0))->column[0] = NULL;
	world_destroy(&w);
}
static void bounded_history(void) {
	assert(settle_observer_start());
	struct world w;
	world_create(&w);
	struct chunk c = {0};
	struct world_section sec = {0};
	sec.column[0] = &c;
	dict_wsection_set_at(w.sections, SECTION_TO_ID(0, 0), sec);
	c.rebuild_displist = true;
	settle_observer_frame(&w, false, 0, 0, 0, true, 8);
	for(unsigned i = 0; i < SETTLE_RING_CAP + 1; i++)
		note(SET_DIRTY_LIGHT, &c, NULL);
	int token;
	finished(&c, &token, SET_ACCEPT);
	c.rebuild_displist = false;
	settle_observer_frame(&w, false, 0, 0, 0, true, 8);
	struct settle_event raw[SETTLE_PULL_CAP];
	size_t used = 99;
	assert(!settle_observer_pull(raw, SETTLE_PULL_CAP, &used) && !used);
	assert(check().overflow > 0); /* counters survive bounded ring eviction */
	settle_observer_publish(sink);
	settle_observer_stop(NULL);
	dict_wsection_get(w.sections, SECTION_TO_ID(0, 0))->column[0] = NULL;
	world_destroy(&w);

	assert(settle_observer_start());
	static struct chunk shells[SETTLE_CHUNK_CAP + 1];
	for(unsigned i = 0; i <= SETTLE_CHUNK_CAP; i++)
		note(SET_DIRTY_LIGHT, &shells[i], NULL);
	struct settle_counts stats;
	settle_observer_counts(&stats);
	assert(!stats.valid && stats.lost == 1);
	settle_observer_stop(NULL);
}

struct stale_hook_race {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool loaded, resume;
	settle_callback hook;
	uint32_t generation;
};
static void* stale_hook_worker(void* opaque) {
	struct stale_hook_race* race = opaque;
	/* This is the production macro's load order, stopped immediately after
	 * the hook has been captured and before it is invoked. */
	race->generation
		= __atomic_load_n(&settle_observer_control, __ATOMIC_ACQUIRE) & ~1u;
	race->hook = __atomic_load_n(&mesher_settle_observe, __ATOMIC_ACQUIRE);
	assert(race->hook);
	pthread_mutex_lock(&race->mutex);
	race->loaded = true;
	pthread_cond_signal(&race->cond);
	while(!race->resume)
		pthread_cond_wait(&race->cond, &race->mutex);
	pthread_mutex_unlock(&race->mutex);
	const struct settle_input in = {.stage = SET_DIRTY_LIGHT,
		.observer_generation = race->generation};
	race->hook(&in);
	return NULL;
}
static void test_stale_hook_dropped_after_restart(void) {
	struct stale_hook_race race = {.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER};
	pthread_t worker;
	assert(settle_observer_start());
	assert(!pthread_create(&worker, NULL, stale_hook_worker, &race));
	pthread_mutex_lock(&race.mutex);
	while(!race.loaded)
		pthread_cond_wait(&race.cond, &race.mutex);
	pthread_mutex_unlock(&race.mutex);
	settle_observer_stop(NULL);
	assert(settle_observer_start());
	pthread_mutex_lock(&race.mutex);
	race.resume = true;
	pthread_cond_signal(&race.cond);
	pthread_mutex_unlock(&race.mutex);
	assert(!pthread_join(worker, NULL));
	struct settle_counts stats;
	settle_observer_counts(&stats);
	assert(stats.calls == 0 && stats.ring_used == 0);
	settle_observer_stop(NULL);
	pthread_cond_destroy(&race.cond);
	pthread_mutex_destroy(&race.mutex);
}

/* Reset-time teardown stall (hardware 2026-09-07, split_anchor_6m).
 *
 * A callback is parked INSIDE observe_body holding the observer mutex while the
 * main thread tears the observer down. The OLD teardown drained by BLOCKING on
 * that mutex, so main could not leave stop() until the callback did -- on the
 * Wii, behind a prio-32/40 holder with no priority inheritance. The NEW one
 * flips the generation and waits out the bounded in-flight gate instead.
 *
 * Both behaviours are exercised in the same run through the production seam
 * settle_observer_test_blocking_stop(), so "fails on the old code" is
 * DEMONSTRATED here rather than asserted in a comment: the legacy variant is
 * measured blocking to the wedge's own release, the fixed one measured
 * returning on its 200 ms drain budget. The wedge self-releases on a bounded
 * deadline, so no watchdog thread is created and nothing burns a fixed 3 s. */
#define WEDGE_PARK_MS 800u
struct wedged_callback {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t worker; /* published by the worker itself, never by pthread_create */
	bool worker_valid, parked, release, released_before_stop_returned,
		stop_returned;
	uint64_t parked_ns; /* wall stamp taken as the worker parks */
};
static struct wedged_callback* wedge;
/* Installed as the observer's clock, so it runs INSIDE observe_body with the
 * observer mutex held -- the production hold this test is about. */
static uint64_t wedging_clock(void) {
	uint64_t value = ++now;
	struct wedged_callback* w = wedge;
	if(!w || !__atomic_load_n(&w->worker_valid, __ATOMIC_ACQUIRE)
	   || !pthread_equal(pthread_self(), w->worker))
		return value;
	pthread_mutex_lock(&w->mutex);
	w->parked_ns = tps_wall_now_ns();
	w->parked = true;
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	uint64_t deadline
		= tps_wall_now_ns() + (uint64_t)WEDGE_PARK_MS * UINT64_C(1000000);
	for(;;) {
		pthread_mutex_lock(&w->mutex);
		bool released = w->release;
		pthread_mutex_unlock(&w->mutex);
		if(released || tps_wall_now_ns() >= deadline)
			break;
		thread_msleep(5);
	}
	pthread_mutex_lock(&w->mutex);
	if(!w->stop_returned)
		w->released_before_stop_returned = true;
	pthread_mutex_unlock(&w->mutex);
	return value;
}
static void* wedge_worker(void* opaque) {
	struct wedged_callback* w = opaque;
	/* The id is published by the thread that owns it; pthread_create's
	 * out-parameter in the parent is not synchronized with this read. */
	w->worker = pthread_self();
	__atomic_store_n(&w->worker_valid, true, __ATOMIC_RELEASE);
	/* SET_SUBMIT takes the stamp() path (the dirty/visit stages deliberately
	 * reuse the frame boundary instead of reading the clock). */
	note(SET_SUBMIT, NULL, w);
	return NULL;
}
/* Returns the milliseconds stop() took; reports whether the wedge had to be
 * released before stop() could return, how many retirements were deferred,
 * and (blocked_ms) how long stop() returned after the worker PARKED.
 *
 * blocked_ms is what the legacy assertion measures: the wedge's 800 ms
 * deadline is armed at the park, so a stop() that waits for the wedge returns
 * >= 800 ms after it on the monotonic clock whatever the host load. The
 * stop()-call duration alone starts only once this thread has woken from the
 * park broadcast; under a saturated host that wake took > 400 ms and the
 * legacy floor failed (G1 full run 2026-09-25, test 499 overlapping a
 * 12-busy-loop stress run; quiet legacy_stop ~810 ms). */
static uint64_t wedged_stop(bool legacy, bool* released_first,
							uint32_t* deferred_delta, uint64_t* blocked_ms) {
	struct wedged_callback race = {.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER};
	pthread_t worker;
	uint32_t deferred_before = settle_observer_deferred_retirements();
	assert(settle_observer_test_clock(wedging_clock));
	assert(settle_observer_test_blocking_stop(legacy));
	assert(settle_observer_start());
	wedge = &race;
	assert(!pthread_create(&worker, NULL, wedge_worker, &race));
	pthread_mutex_lock(&race.mutex);
	while(!race.parked)
		pthread_cond_wait(&race.cond, &race.mutex);
	pthread_mutex_unlock(&race.mutex);

	uint64_t begin = tps_wall_now_ns();
	settle_observer_stop(NULL);
	uint64_t end = tps_wall_now_ns();
	uint64_t elapsed_ms = (end - begin) / UINT64_C(1000000);
	assert(!settle_observer_active());
	pthread_mutex_lock(&race.mutex);
	race.stop_returned = true;
	race.release = true;
	pthread_cond_broadcast(&race.cond);
	pthread_mutex_unlock(&race.mutex);
	assert(!pthread_join(worker, NULL));
	wedge = NULL;
	*released_first = race.released_before_stop_returned;
	*blocked_ms = (end - race.parked_ns) / UINT64_C(1000000);
	*deferred_delta = settle_observer_deferred_retirements() - deferred_before;
	assert(settle_observer_test_blocking_stop(false));
	pthread_cond_destroy(&race.cond);
	pthread_mutex_destroy(&race.mutex);
	return elapsed_ms;
}
/* ROUND-3 FINDING 1: the double-timeout shutdown leak. A stop() whose drain
 * expires leaves active=false with the 150,936 B buffer still carried, and a
 * start() whose drain also expires returns false without re-arming. The old
 * `if(!settle_observer_active()) return;` then took the shutdown stop straight
 * past the module's only mem2_free_sized -- the block was leaked for the
 * remainder of the process. Shutdown must retire unconditionally once the
 * drain is clean, which on the real path it always is (main.c stops after
 * every emitting thread has been joined). */
static void test_shutdown_frees_after_double_timeout(void) {
	struct wedged_callback race = {.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER};
	pthread_t worker;
	size_t allocs = mem2_stat_alloc_calls(), frees = mem2_stat_free_calls();
	uint32_t deferred_before = settle_observer_deferred_retirements();
	assert(settle_observer_test_clock(wedging_clock));
	assert(settle_observer_start());
	assert(mem2_stat_alloc_calls() == allocs + 1);
	wedge = &race;
	assert(!pthread_create(&worker, NULL, wedge_worker, &race));
	pthread_mutex_lock(&race.mutex);
	while(!race.parked)
		pthread_cond_wait(&race.cond, &race.mutex);
	pthread_mutex_unlock(&race.mutex);

	/* 1. stop(): drain expires, observer disarms, buffer is CARRIED. */
	settle_observer_stop(NULL);
	assert(!settle_observer_active());
	assert(mem2_stat_free_calls() == frees);
	assert(settle_observer_deferred_retirements() == deferred_before + 1);
	/* 2. re-arm: drain expires too, so it refuses -- and never allocates a
	 *    second block. The observer is now inactive AND holding memory. */
	assert(!settle_observer_start());
	assert(!settle_observer_active());
	assert(mem2_stat_alloc_calls() == allocs + 1);
	assert(settle_observer_deferred_retirements() == deferred_before + 2);

	/* 3. shutdown. Release the straggler first: that is the real ordering --
	 *    main.c calls settle_observer_stop() after the server join, the mesher
	 *    shutdown and the telemetry shutdown, so no thread can still be inside
	 *    observe_body and the drain is clean. */
	pthread_mutex_lock(&race.mutex);
	race.stop_returned = true;
	race.release = true;
	pthread_cond_broadcast(&race.cond);
	pthread_mutex_unlock(&race.mutex);
	assert(!pthread_join(worker, NULL));
	wedge = NULL;
	settle_observer_stop(NULL);
	/* THE ASSERTION THIS TEST EXISTS FOR: frees == allocs. */
	assert(mem2_stat_free_calls() - frees == mem2_stat_alloc_calls() - allocs);
	assert(mem2_stat_free_calls() == frees + 1);
	/* Idempotent: nothing is left to free, and no double free. */
	settle_observer_stop(NULL);
	assert(mem2_stat_free_calls() == frees + 1);
	assert(settle_observer_test_clock(fake_clock));
	pthread_cond_destroy(&race.cond);
	pthread_mutex_destroy(&race.mutex);
	printf("PASS shutdown retires after stop-timeout + start-timeout: "
		   "mem2 allocs=%zu frees=%zu deferred=+2\n",
		   mem2_stat_alloc_calls() - allocs, mem2_stat_free_calls() - frees);
}
static void test_stop_not_blocked_by_wedged_callback(void) {
	bool released_first = false;
	uint32_t deferred = 0;
	/* OLD behaviour, same scenario: stop() sits on the observer mutex until the
	 * wedge lets go, i.e. past the drain budget and past the wedge's release. */
	uint64_t legacy_blocked_ms = 0, fixed_blocked_ms = 0;
	uint64_t legacy_ms
		= wedged_stop(true, &released_first, &deferred, &legacy_blocked_ms);
	assert(released_first);
	/* G1 review F6: measured from the park, a stop() that really waits for the
	 * wedge returns >= WEDGE_PARK_MS after it -- deterministically (the
	 * deadline is armed at the park on the same monotonic clock). The full
	 * 800, not 400: main-thread delay between the park and the stop() call can
	 * no longer make up the difference for a stop that does not wait, unless
	 * that delay alone exceeds the whole park window -- and released_first
	 * above already proves the stop outlived the wedge's hold. The stop()-call
	 * window must sit inside the park window (sanity: it cannot start before
	 * the park). */
	assert(legacy_blocked_ms >= WEDGE_PARK_MS);
	assert(legacy_ms <= legacy_blocked_ms);
	assert(deferred == 0);

	/* NEW behaviour: bounded drain, returns on its own budget, and records the
	 * carried retirement instead of freeing under a live writer. */
	size_t allocs = mem2_stat_alloc_calls(), frees = mem2_stat_free_calls();
	uint64_t fixed_ms
		= wedged_stop(false, &released_first, &deferred, &fixed_blocked_ms);
	assert(!released_first);
	assert(fixed_ms < WEDGE_PARK_MS / 2);
	assert(fixed_ms < legacy_blocked_ms);
	assert(deferred == 1);
	/* The timed-out drain reports its real cost, so a hardware rerun can tell
	 * "drain was slow" from "never reached the drain". */
	uint32_t timed_out_drain_us = settle_observer_last_drain_us();
	assert(timed_out_drain_us >= 150000);
	/* It kept the buffer rather than freeing it under the writer... */
	assert(mem2_stat_free_calls() == frees);
	/* ...and the next arm REUSES that same block: no second allocation, no
	 * unreachable block, and the observer is usable again. */
	assert(settle_observer_start());
	assert(mem2_stat_alloc_calls() == allocs + 1);
	settle_observer_stop(NULL);
	assert(mem2_stat_alloc_calls() == allocs + 1
		   && mem2_stat_free_calls() == frees + 1);
	assert(settle_observer_deferred_retirements() >= 1);
	assert(settle_observer_test_clock(fake_clock));
	printf("PASS wedged teardown (old vs new, same scenario): legacy_stop=%" PRIu64
		   "ms (blocked %" PRIu64 "ms after park) fixed_stop=%" PRIu64
		   "ms timed_out_drain=%uus rearm_drain=%uus "
		   "deferred=%u mem2_allocs=+1 mem2_frees=+1\n",
		   legacy_ms, legacy_blocked_ms, fixed_ms, timed_out_drain_us,
		   settle_observer_last_drain_us(),
		   settle_observer_deferred_retirements());
}

#ifdef _WIN32
#include <windows.h>
static uint64_t cpu_counter(void) {
	ULONG64 cycles = 0;
	assert(QueryThreadCycleTime(GetCurrentThread(), &cycles));
	return cycles;
}
#else
static uint64_t cpu_counter(void) {
	return tps_cpu_now_ns();
}
#endif
static void sample_job(struct chunk* c, const void* token) {
	note(SET_DIRTY_INSTALL, c, NULL);
	note(SET_VISIT, c, NULL);
	note(SET_GATHER, c, token);
	for(unsigned i = 0; i < 27; i++) {
		note(SET_STRIPE_BEGIN, NULL, NULL);
		note(SET_STRIPE_END, NULL, NULL);
	}
	note(SET_SUBMIT, NULL, token);
	note(SET_JOB_DEQUEUE, NULL, token);
	note(SET_WORKER, NULL, token);
	note(SET_EMIT_BEGIN, NULL, token);
	note(SET_VL_BEGIN, NULL, token);
	note(SET_EMIT_BEGIN, NULL, token);
	note(SET_FIN_BEGIN, NULL, token);
	note(SET_BFS_BEGIN, NULL, token);
	note(SET_COMPLETE, NULL, token);
	note(SET_CONSUME, NULL, token);
	note(SET_ACCEPT, NULL, token);
}
static void census_scan(void) {
	static struct chunk shells[121 * COLUMN_HEIGHT];
	struct world w;
	world_create(&w);
	unsigned n = 0;
	for(int z = -5; z <= 5; z++)
		for(int x = -5; x <= 5; x++) {
			struct world_section sec = {0};
			for(unsigned cy = 0; cy < COLUMN_HEIGHT; cy++) {
				struct chunk* c = &shells[n++];
				c->x = x * 16;
				c->z = z * 16;
				c->y = (int)cy * 16;
				c->rebuild_displist = true;
				sec.column[cy] = c;
			}
			dict_wsection_set_at(w.sections, SECTION_TO_ID(x, z), sec);
		}
	for(unsigned trial = 0; trial < 5; trial++) {
		uint64_t wall[2] = {0}, cpu[2] = {0};
		for(unsigned armed = 0; armed < 2; armed++) {
			if(armed)
				assert(settle_observer_start());
			uint64_t t = tps_wall_now_ns(), ct = cpu_counter();
			for(unsigned frame = 0; frame < 100; frame++) {
				for(unsigned pass = 0; pass < 3; pass++)
					for(unsigned i = 0; i < n; i++)
						note(SET_VISIT, &shells[i], NULL);
				settle_observer_frame(&w, false, 0, 0, 5, true, 8);
			}
			cpu[armed] = cpu_counter() - ct;
			wall[armed] = tps_wall_now_ns() - t;
		}
		check();
		printf("settle_scan trial=%u chunks=%u passes=3 off_ns/frame=%.1f "
			   "armed_ns/frame=%.1f off_cpu/frame=%.1f armed_cpu/frame=%.1f\n",
			   trial, n, (double)wall[0] / 100, (double)wall[1] / 100,
			   (double)cpu[0] / 100, (double)cpu[1] / 100);
		settle_observer_stop(discard);
	}
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w.sections); !dict_wsection_end_p(it);
		dict_wsection_next(it))
		memset(dict_wsection_ref(it)->value.column, 0,
			   sizeof(dict_wsection_ref(it)->value.column));
	world_destroy(&w);
}
static void census(void) {
	assert(settle_observer_test_clock(NULL));
	struct chunk c = {0};
	int token;
	const unsigned batches = 100, batch = 64, n = batches * batch;
	for(unsigned trial = 0; trial < 5; trial++) {
		uint64_t wall[2] = {0}, cpu[2] = {0}, drain = 0;
		for(unsigned armed = 0; armed < 2; armed++) {
			if(armed)
				assert(settle_observer_start());
			for(unsigned b = 0; b < batches; b++) {
				uint64_t t = tps_wall_now_ns(), ct = cpu_counter();
				for(unsigned i = 0; i < batch; i++)
					sample_job(&c, &token);
				cpu[armed] += cpu_counter() - ct;
				wall[armed] += tps_wall_now_ns() - t;
				if(armed) {
					t = tps_wall_now_ns();
					settle_observer_publish(discard);
					drain += tps_wall_now_ns() - t;
				}
			}
		}
		struct settle_counts stats = check();
		printf("settle_census trial=%u jobs=%u off_ns/job=%.1f "
			   "armed_ns/job=%.1f off_cpu/job=%.1f armed_cpu/job=%.1f "
			   "drain_ns/batch=%.1f hooks=%" PRIu64 " stamps=%" PRIu64
			   " static=%zu ring=%zu\n",
			   trial, n, (double)wall[0] / n, (double)wall[1] / n,
			   (double)cpu[0] / n, (double)cpu[1] / n, (double)drain / batches,
			   stats.calls, stats.clock_reads, settle_observer_static_bytes(),
			   sizeof(struct settle_event) * SETTLE_RING_CAP);
		settle_observer_stop(discard);
	}
	census_scan();
	puts("PC cpu_counter units: Windows raw QueryThreadCycleTime cycles; POSIX "
		 "thread CPU ns. Wall is separate. 27 stripe brackets/job; no engine "
		 "work or transport I/O included.");
}
/* The heartbeat's "up_ms" is the device wall clock (heartbeat_uptime_ms,
 * real CLOCK_MONOTONIC ms on a PC build without CAVEWII_HW_TELEMETRY_TEST), so
 * two formats a few microseconds apart differ whenever a millisecond boundary
 * falls between them (2-5% of runs on Windows and Linux; G1-LINUX-diag
 * 2026-09-25). Compare with exactly that digit span removed from both lines:
 * every other byte, and the length outside the span, must still match. */
static size_t strip_up_ms(const char* src, size_t n, char* dst) {
	static const char key[] = "\"up_ms\":";
	const char* at = strstr(src, key);
	assert(at && at < src + n);
	size_t head = (size_t)(at - src) + sizeof(key) - 1;
	size_t digits = 0;
	while(src[head + digits] >= '0' && src[head + digits] <= '9')
		digits++;
	assert(digits > 0);
	memcpy(dst, src, head);
	memcpy(dst + head, src + head + digits, n + 1 - head - digits);
	return n - digits;
}
static bool heartbeat_identical(const char* before, size_t nb,
								const char* armed, size_t na) {
	char a[HW_TELEMETRY_HEARTBEAT_LINE_SIZE], b[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t la = strip_up_ms(before, nb, a), lb = strip_up_ms(armed, na, b);
	return la == lb && !memcmp(a, b, la + 1);
}
static void assert_heartbeat_identical(const char* before, size_t nb,
									   const char* armed, size_t na) {
	assert(heartbeat_identical(before, nb, armed, na));
}
/* The comparator ignores ONLY the up_ms digits: a changed / longer up_ms is
 * equal, any other changed byte is not. */
static void heartbeat_compare_selfcheck(const char* line, size_t n) {
	char t[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	const char* at = strstr(line, "\"up_ms\":");
	assert(at);
	size_t head = (size_t)(at - line) + 8;
	/* up_ms -> "9" followed by the original digits (one byte longer) */
	assert(n + 1 < sizeof(t));
	memcpy(t, line, head);
	t[head] = '9';
	memcpy(t + head + 1, line + head, n + 1 - head);
	assert(heartbeat_identical(line, n, t, n + 1));
	/* one byte outside the span: not equal */
	memcpy(t, line, n + 1);
	const char* tick = strstr(t, "\"tick\":123");
	assert(tick);
	t[(size_t)(tick - t) + 7] = '4';
	assert(!heartbeat_identical(line, n, t, n));
}
static void heartbeat_identity(void) {
	struct hw_telemetry_heartbeat hb = {0};
	hb.tick = 123;
	hb.world_loaded = true;
	char before[HW_TELEMETRY_HEARTBEAT_LINE_SIZE],
		armed[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(before, sizeof(before),
											 "settle-test", "far_walk_6m", &hb);
	assert(n < sizeof(before));
	heartbeat_compare_selfcheck(before, n);
	assert(settle_observer_start());
	struct chunk c = {0};
	int token;
	sample_job(&c, &token);
	size_t na = hw_telemetry_format_heartbeat(armed, sizeof(armed),
											  "settle-test", "far_walk_6m", &hb);
	assert(na < sizeof(armed));
	assert_heartbeat_identical(before, n, armed, na);
	/* Exercise the production profile boundary, not just the collector API.

	 * * On PC it synchronously drains before telemetry can discard the run ID.
	 */
	hw_telemetry_emit_profile_done("ok");
	struct settle_counts final;
	settle_observer_counts(&final);
	assert(final.finished && final.valid && final.ring_used);
	settle_observer_stop(discard);
	na = hw_telemetry_format_heartbeat(armed, sizeof(armed), "settle-test",
									   "far_walk_6m", &hb);
	assert(na < sizeof(armed));
	assert_heartbeat_identical(before, n, armed, na);
	printf("PASS heartbeat byte identity armed/dormant: %zu bytes\n", n);
}
int main(int argc, char** argv) {
	if(argc > 1 && !strcmp(argv[1], "--census")) {
		census();
		return 0;
	}
	assert(settle_observer_test_clock(fake_clock));
	/* Dormant macro arguments must not even be evaluated. */
	unsigned evaluated = 0;
	SETTLE(mesher_settle_observe, SET_SUBMIT, NULL, NULL, ++evaluated, 0, 0, 0);
	assert(!evaluated && !now);
	assert(settle_observer_static_bytes() <= 1024);
	assert(settle_observer_armed_bytes() <= 160 * 1024);
	size_t allocs = mem2_stat_alloc_calls(), frees = mem2_stat_free_calls();
	assert(settle_observer_start());
	assert(mem2_stat_alloc_calls() == allocs + 1);
	struct chunk pre_reset = {0};
	note(SET_DIRTY_INSTALL, &pre_reset, NULL);
	struct settle_counts around;
	settle_observer_counts(&around);
	assert(around.calls == 1 && around.ring_used == 1);
	uint32_t retires = settle_observer_reset_retirements();
	uint32_t deferred_at_reset = settle_observer_deferred_retirements();
	uint32_t drain_at_reset = settle_observer_last_drain_us();
	SETTLE(client_settle_observe, SET_RESET, NULL, NULL, 0, 0, 0, 0);
	/* RETIRE-IN-PLACE: a world reset recycles the armed buffer. It never
	 * disarms, and performs NO MEM2 traffic -- the free + strict re-allocation
	 * that used to run here, from inside a callback while the carve was being
	 * rebuilt, is gone. */
	assert(settle_observer_active());
	assert(mem2_stat_alloc_calls() == allocs + 1
		   && mem2_stat_free_calls() == frees);
	/* ROUND-3 FINDING 2/3: the reset path's cost is measured in place and read
	 * out AFTER the reset, so the path itself carries one memset and nothing
	 * else -- no MEM2 traffic, no drain, and (since round 3) no SD write. The
	 * teardown pair below cannot see this path at all, which is exactly why it
	 * needs counters of its own. */
	assert(settle_observer_reset_retirements() == retires + 1);
	assert(settle_observer_last_reset_us() < 1000000u);
	assert(settle_observer_deferred_retirements() == deferred_at_reset
		   && settle_observer_last_drain_us() == drain_at_reset);
	settle_observer_counts(&around);
	assert(around.calls == 0 && around.ring_used == 0 && around.valid);
	settle_observer_stop(NULL);
	assert(mem2_stat_alloc_calls() == allocs + 1
		   && mem2_stat_free_calls() == frees + 1);
	printf("footprint dormant=%zu armed=%zu; reset path = one memset(%zu B), "
		   "measured %u us, no MEM2/drain/SD I/O\n",
		   settle_observer_static_bytes(), settle_observer_armed_bytes(),
		   settle_observer_armed_bytes(), settle_observer_last_reset_us());
	test_stale_hook_dropped_after_restart();
	test_stop_not_blocked_by_wedged_callback();
	test_shutdown_frees_after_double_timeout();
	test_chunk_probe_bounded();
	accounting();
	invalidation();
	heartbeat_identity();
	bounded_history();
	if(argc > 1) {
		capture = fopen(argv[1], "w");
		assert(capture);
	}
	episode();
	if(capture)
		fclose(capture);
	printf("PASS settle reconciliation, cancellation, token reuse, dirty "
		   "no-clock, overflow/loss, dormant identity; records=%u "
		   "max_record=%u static=%zu event=%zu\n",
		   records, max_record, settle_observer_static_bytes(),
		   sizeof(struct settle_event));
	return 0;
}
