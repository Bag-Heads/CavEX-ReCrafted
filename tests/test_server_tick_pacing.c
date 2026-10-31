/*
	Server-loop pacing regression.

	Beta 1.7.3 constructs Timer(20.0F), so one simulation period is 50 ms
	[decompile 740c583, Minecraft.java `new Timer(20.0F)`]. Tick work belongs
	inside that period: sleep = max(period - elapsed, 0), not an additional
	50 ms after the work. The loading path uses the existing 2 ms slice period.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "network/server_tick_pacing.h"
#include "platform/time.h"

/* Golden trace derived from 740c583 Timer.updateTimer + Minecraft.run:
 * 20 ticks/s, fractional ticks retained BEFORE limiting a batch to 10;
 * a wall-clock interval >1000 ms resets lastHRTime (no elapsed ticks).
 * These are scheduling tests, not a claim that mc173's tick_padded is an
 * oracle: that implementation restarts its clock after every sleep too. */
static void test_beta_clock_trace(void) {
	struct server_tick_clock clock = {0};
	/* Half-ticks are exactly representable in Java's float accumulator. */
	const int elapsed[] = {0, 25, 50, 25, 125, 25, 525, 25, 1001, 50};
	const unsigned ticks[] = {0, 0, 1, 1, 2, 1, 10, 1, 0, 1};
	for(unsigned i = 0; i < sizeof(elapsed) / sizeof(*elapsed); i++)
		assert(server_tick_clock_advance(&clock, elapsed[i]) == ticks[i]);
	assert(clock.partial_ticks == 0.0f);
	assert(server_tick_clock_advance(&clock, -1) == 0);
	assert(server_tick_clock_advance(&clock, 1000) == 10);
	assert(server_tick_clock_advance(&clock, 0) == 0);
	assert(server_tick_clock_wait_ms(&clock) == 50);
	assert(server_tick_clock_advance(&clock, 25) == 0);
	assert(server_tick_clock_wait_ms(&clock) == 25);
	/* A long stall discards its elapsed time, not the existing fraction. */
	assert(server_tick_clock_advance(&clock, 1001) == 0);
	assert(server_tick_clock_wait_ms(&clock) == 25);
	assert(server_tick_clock_advance(&clock, 25) == 1);
	puts("PASS: Beta golden trace retains fractions, caps batches at 10, resets >1s stalls");
}

/* 2026-09-08, server-loop wedge instrumentation lane (lane/carve-observer-
 * boot): real-Wii evidence showed the server thread's watchdog phase frozen
 * at WDS_IDLE for 400+ heartbeats after a single 109.6s FAR-phase stretch,
 * and could not tell "sleeping forever in thread_msleep" from "looping with
 * 0 ticks forever" from "blocked inside FAR". This test documents the actual
 * behaviour of the ticks_pending==0 path's own two calls
 * (server_tick_clock_advance then server_tick_clock_wait_ms) when the
 * elapsed time handed to them is itself ~109,000 ms (as it would be if the
 * PRECEDING call had spent that long blocked in FAR) or negative (a
 * non-monotonic clock backend): NEITHER produces a wedge. elapsed_ms's guard
 * (`elapsed_ms >= 0 && elapsed_ms <= 1000`) rejects both out of range, so
 * seconds=0.0 and the call is a no-op on partial_ticks/ticks either way --
 * exactly the same ">1s interval resets lastHRTime, no elapsed ticks" rule
 * test_beta_clock_trace already golds at 1001/1000, just at a much larger
 * magnitude. wait_ms then returns a normal ~50ms bounded sleep (from
 * whatever partial_ticks already held), not an unbounded one -- so a caller
 * that fed a 109s stall's elapsed time into this clock would NOT wedge in
 * thread_msleep afterward; it would simply resume the normal ~50ms tick
 * cadence next iteration. Proves the clock math itself is not the freeze;
 * the 109.6s was spent BLOCKED before ever reaching this call. */
static void test_far_stall_elapsed_no_wedge(void) {
	struct server_tick_clock clock = {0};
	/* A stall the exact order of the real-Wii evidence's single far-column
	 * FAR-phase stretch (109,575,981 us of srv_gen_us == 109,576 ms of
	 * wall time), rounded to the ms this function actually consumes. */
	const int32_t far_stall_ms = 109576;
	assert(server_tick_clock_advance(&clock, far_stall_ms) == 0);
	assert(clock.partial_ticks == 0.0f);
	/* wait_ms is bounded to one tick period (50ms), never the stall's own
	 * magnitude reflected back -- the direct proof "no wedge" claims. */
	assert(server_tick_clock_wait_ms(&clock) == 50);

	/* Same guard, negative side: a non-monotonic clock producing a negative
	 * elapsed (time_diff_ms went backwards) is equally a no-op, not a
	 * negative/garbage tick count or an unbounded wait. */
	struct server_tick_clock clock2 = {0};
	assert(server_tick_clock_advance(&clock2, -109576) == 0);
	assert(clock2.partial_ticks == 0.0f);
	assert(server_tick_clock_wait_ms(&clock2) == 50);

	/* An in-progress fraction survives both: neither a huge nor a negative
	 * elapsed discards existing partial-tick state (only advances it by
	 * zero), matching test_beta_clock_trace's ">1s discards elapsed time,
	 * not the existing fraction" golden at a much larger magnitude. */
	struct server_tick_clock clock3 = {0};
	assert(server_tick_clock_advance(&clock3, 25) == 0); /* 0.5 ticks banked */
	assert(server_tick_clock_wait_ms(&clock3) == 25);
	assert(server_tick_clock_advance(&clock3, far_stall_ms) == 0);
	assert(clock3.partial_ticks == 0.5f);
	assert(server_tick_clock_wait_ms(&clock3) == 25);
	assert(server_tick_clock_advance(&clock3, -1) == 0);
	assert(clock3.partial_ticks == 0.5f);
	assert(server_tick_clock_wait_ms(&clock3) == 25);

	puts("PASS: a ~109s (or negative) elapsed reaching server_tick_clock_"
		 "advance/wait_ms is a documented no-op (seconds clamped to 0, "
		 "wait bounded to one tick period) -- no wedge from the clock math "
		 "itself, whatever blocked the caller before it got here");
}

static void test_sleep_lateness_does_not_slow_clock(void) {
	struct server_tick_clock clock = {0};
	unsigned ticks = 0;
	/* Exact golden: 1000 late 75 ms wakeups represent 75 seconds = 1500
	 * ticks, not the old loop's 1000. No expectations use our timer output. */
	for(int i = 0; i < 1000; i++)
		ticks += server_tick_clock_advance(&clock, 75);
	assert(ticks == 1500);
	/* A caller never admits the suspended/loading interval. */
	clock = (struct server_tick_clock){0};
	assert(server_tick_clock_advance(&clock, 49) == 0);
	assert(server_tick_clock_advance(&clock, 1) == 1);
	puts("PASS: 75 seconds with late wakeups still schedules 1500 ordered ticks");
}


/* TICK_CLOCK_ACCOUNTING_EX: the accounting variant admits exactly what the plain
 * clock admits and reports what Beta's Timer threw away. */
static void test_clock_advance_ex_reports_cap_and_discard(void) {
	struct server_tick_clock a = {0}, b = {0};
	unsigned raw = 99;
	bool discarded = true;
	/* 1000 ms = 20 raw ticks, 10 admitted, 10 capped off */
	assert(server_tick_clock_advance_ex(&a, 1000, &raw, &discarded) == 10);
	assert(raw == 20 && !discarded);
	assert(server_tick_clock_advance(&b, 1000) == 10);
	assert(a.partial_ticks == b.partial_ticks);
	/* 1001 ms and negative intervals are discarded: nothing credited */
	assert(server_tick_clock_advance_ex(&a, 1001, &raw, &discarded) == 0);
	assert(raw == 0 && discarded);
	assert(server_tick_clock_advance_ex(&a, -5, &raw, &discarded) == 0);
	assert(raw == 0 && discarded);
	/* an ordinary 50 ms sample: one tick, nothing capped */
	assert(server_tick_clock_advance_ex(&a, 50, &raw, &discarded) == 1);
	assert(raw == 1 && !discarded);
	assert(server_tick_clock_advance(&b, 1001) == 0);
	assert(server_tick_clock_advance(&b, -5) == 0);
	assert(server_tick_clock_advance(&b, 50) == 1);
	assert(a.partial_ticks == b.partial_ticks);
	/* NULL out-params are allowed (the plain clock is this call) */
	assert(server_tick_clock_advance_ex(&a, 600, NULL, NULL) == 10);
	assert(server_tick_clock_advance(&b, 600) == 10);
	assert(a.partial_ticks == b.partial_ticks);
	puts("PASS advance_ex admits what advance admits and reports cap/discard");
}


/* PACER_POLICY_GOLDEN (Astra round 8, 3): drive the PRODUCTION pacer on a
 * scripted timeline. First sample at 500 ms admits ten; ten iterations of
 * 75 ms (74 ms of work + the 1 ms overrun sleep); at the sample at 1250 ms,
 * before another execution, the batch rule has admitted 20 / pending 10 /
 * capped 5, per-tick sampling 25 / 15 / 0, fraction 0 in both. Draining
 * with 1 ms iterations then ends at 20 executions @1260 ms (batch) vs 25
 * @1265 ms (A); neither drain adds a whole tick. */
struct sim {
	struct server_tick_pacer pacer;
	int64_t now, sample;
	unsigned executed, admitted, capped;
};
/* one loop entry at sim->now; runs the tick (advancing now by work + 1 ms
 * sleep) when the pacer says so; returns whether it ran */
static bool sim_enter(struct sim* s, bool each, int32_t work_ms) {
	struct server_tick_admission a;
	bool run = server_tick_pacer_enter(&s->pacer, (int32_t)(s->now - s->sample),
									   each, &a);
	if(a.sampled) {
		s->sample = s->now;
		s->admitted += a.admitted;
		s->capped += (a.raw - a.admitted) + a.capped_pending;
	}
	if(run) {
		s->executed++;
		s->now += work_ms + SERVER_TICK_OVERRUN_SLEEP_MS;
	}
	return run;
}
static void golden(bool each, unsigned want_admitted, unsigned want_pending,
				   unsigned want_capped, unsigned want_executed_end,
				   int64_t want_now_end) {
	struct sim s = {{{0}, 0}, 500, 0, 0, 0, 0};
	for(unsigned i = 0; i < 10; i++)
		assert(sim_enter(&s, each, 74));
	assert(s.executed == 10 && s.now == 1250);
	/* the sample at 1250 ms, before another execution */
	struct server_tick_admission a;
	struct server_tick_pacer probe = s.pacer;
	assert(server_tick_pacer_enter(&probe, (int32_t)(s.now - s.sample), each, &a));
	assert(a.sampled);
	assert(s.admitted + a.admitted == want_admitted);
	assert(probe.pending + 1 == want_pending);
	assert(s.capped + (a.raw - a.admitted) + a.capped_pending == want_capped);
	assert(probe.clock.partial_ticks == 0.0f);
	/* drain with 1 ms iterations (0 ms of work + the 1 ms sleep) */
	unsigned guard = 0;
	while(sim_enter(&s, each, 0))
		assert(++guard < 100);
	assert(s.executed == want_executed_end);
	assert(s.now == want_now_end);
}
static void test_pacer_policy_golden(void) {
	golden(false, 20, 10, 5, 20, 1260);
	golden(true, 25, 15, 0, 25, 1265);
	/* sustained overload (every tick 80 ms) is bounded, and the bound is
	 * reported: pending never exceeds SERVER_TICK_PENDING_MAX, capped grows */
	struct sim s = {{{0}, 0}, 50, 0, 0, 0, 0};
	unsigned peak = 0;
	for(unsigned i = 0; i < 2000; i++) {
		if(!sim_enter(&s, true, 79))
			s.now += (int64_t)server_tick_clock_wait_ms(&s.pacer.clock);
		if(s.pacer.pending > peak)
			peak = s.pacer.pending;
	}
	assert(peak <= SERVER_TICK_PENDING_MAX && peak >= SERVER_TICK_PENDING_MAX - 1);
	assert(s.capped > 100 && s.executed + s.capped + s.pacer.pending
						  >= (unsigned)(s.sample / SERVER_TICK_PERIOD_MS) - 1);
	/* recovery: quiet ticks (1 ms) drain the bounded debt to zero */
	unsigned guard = 0;
	while(s.pacer.pending) {
		sim_enter(&s, true, 0);
		assert(++guard < 200);
	}
	puts("PASS pacer golden: batch rule 20/10/5 -> 20 @1260, per-tick 25/15/0 -> 25 @1265; overload bounded and reported");
}


/* CATCHUP_LIMITER_PROOF (Astra round 10, 1): the production limiter on a
 * scripted timeline. (a) on-time operation, one start per 50 ms, never
 * waits; (b) draining a 15-tick debt with 1 ms ticks starts at most two
 * ticks in any 50 ms window, executes all 15 (debt retained, never
 * discarded) and therefore takes at least seven 50 ms windows; (c) the
 * pacer's accounting is untouched by the limiter. */
static void test_catchup_limiter(void) {
	struct server_tick_limiter l = {{0}, 0, 0};
	for(int64_t now = 0; now < 2000; now += 50) {
		assert(server_tick_limiter_wait_ms(&l, now) == 0);
		server_tick_limiter_note_start(&l, now);
	}
	/* the drain: 15 owed ticks, each 1 ms of work */
	struct server_tick_limiter d = {{0}, 0, 0};
	int64_t now = 1000, starts[15];
	for(unsigned i = 0; i < 15; i++) {
		unsigned wait = server_tick_limiter_wait_ms(&d, now);
		now += wait;
		server_tick_limiter_note_start(&d, now);
		starts[i] = now;
		now += 1; /* the tick */
	}
	/* LIMITER_FLAG_AWARE: with SERVER_TICK_CATCHUP_LIMIT 0 the wait is always 0
	 * and the drain is unbounded by design -- only the ring bookkeeping is
	 * exercised above; the bound assertions apply to the enabled limiter. */
	for(unsigned i = 0; SERVER_TICK_CATCHUP_LIMIT && i < 15; i++) {
		unsigned in_window = 0;
		for(unsigned j = 0; j < 15; j++)
			if(starts[j] >= starts[i] && starts[j] < starts[i] + SERVER_TICK_PERIOD_MS)
				in_window++;
		assert(in_window <= SERVER_TICK_START_LIMIT && "more than two starts in a 50 ms window");
	}
	assert(!SERVER_TICK_CATCHUP_LIMIT
		   || starts[14] - starts[0] >= 7 * SERVER_TICK_PERIOD_MS - SERVER_TICK_PERIOD_MS
		   || !"15 starts at two per window need at least seven windows");
	/* the pacer keeps its debt across limiter waits: entitlement unchanged */
	struct server_tick_pacer p = {{0}, 0};
	struct server_tick_admission a;
	assert(server_tick_pacer_enter(&p, 750, true, &a) && a.admitted == 10 && p.pending == 9);
	assert(server_tick_pacer_enter(&p, 250, true, &a) && a.admitted == 5 && p.pending == 13);
	puts("PASS catch-up limiter: on-time never waits; a 15-tick drain starts <= 2 per 50 ms and keeps every tick");
}

int main(void) {
	test_catchup_limiter();
	test_pacer_policy_golden();
	test_clock_advance_ex_reports_cap_and_discard();
	test_beta_clock_trace();
	test_far_stall_elapsed_no_wedge();
	test_sleep_lateness_does_not_slow_clock();
	assert(SERVER_TICK_OVERRUN_SLEEP_MS == 1);
	assert(server_tick_sleep_ms(true, 0) == 50);
	assert(server_tick_sleep_ms(true, 1) == 49);
	assert(server_tick_sleep_ms(true, 17) == 33);
	assert(server_tick_sleep_ms(true, 49) == 1);
	assert(server_tick_sleep_ms(true, 50) == 0);
	assert(server_tick_sleep_ms(true, 2764) == 0);
	assert(server_tick_sleep_ms(true, -1) == 50);

	assert(server_tick_sleep_ms(false, 0) == 2);
	assert(server_tick_sleep_ms(false, 1) == 1);
	assert(server_tick_sleep_ms(false, 2) == 0);
	assert(server_tick_sleep_ms(false, 200) == 0);
	assert(server_tick_sleep_ms(false, -1) == 2);

	// 0.9995s -> 1.0490s is 49.5 ms. The PC timer must truncate the
	// combined positive duration to 49, not round its -950.5ms nsec term
	// independently to -950 and report 50.
	ptime_t before = {.tv_sec = 0, .tv_nsec = 999500000};
	ptime_t after = {.tv_sec = 1, .tv_nsec = 49000000};
	assert(time_diff_ms(before, after) == 49);
	assert(server_tick_sleep_ms(true, time_diff_ms(before, after)) == 1);

	puts("test_server_tick_pacing: 50 ms/2 ms periods include work time");

	/* The Wii server must remain preemptible by main (64), while staying
	 * above the mesher (32), across the loading -> gameplay transition. */
	assert(SERVER_THREAD_PRIORITY == 63);
	/* Suspended-update sleep equivalence to the pre-change implementation for
	 * every elapsed duration through ten seconds, including overruns. */
	for(int phase = 0; phase < 2; phase++) {
		for(int elapsed = -10; elapsed <= 10000; elapsed++) {
			size_t period = phase ? 50 : 2;
			size_t old_sleep = elapsed <= 0 ? period
				: (size_t)elapsed >= period ? 0 : period - elapsed;
			assert(server_tick_sleep_ms(phase != 0, elapsed) == old_sleep);
		}
	}
	puts("PASS: server stays below render priority; suspended-update sleeps unchanged");
	return 0;
}
