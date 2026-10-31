/*
	Copyright (c) 2026

	This file is part of CavEX.
*/

#ifndef SERVER_TICK_PACING_H
#define SERVER_TICK_PACING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERVER_TICK_PERIOD_MS 50
#define SERVER_LOAD_PERIOD_MS 2
// An overrun has no period remainder, but it must still block briefly on Wii:
// LWP_YieldThread only offers the core to equal/higher-priority peers and can
// starve the priority-32 mesher behind the priority-64 server.
#define SERVER_TICK_OVERRUN_SLEEP_MS 1

/* 740c583: Timer.updateTimer, used by Minecraft's Timer(20.0F).
 * The monotonic platform clock replaces Java's wall/HR clock calibration.
 * Keep the Java float cast and subtract the WHOLE count before the cap:
 * excess work from a >500 ms interval must not become unbounded debt.
 * A >1000 ms interval resets lastHRTime in Beta; negative intervals do too.
 * Loading/pause reset this state rather than accumulating suspended time. */
struct server_tick_clock {
	float partial_ticks;
};

/* TICK_CLOCK_ACCOUNTING: the same arithmetic, also reporting what the Beta
 * Timer semantics threw away -- the raw whole-tick count before the cap of
 * 10 and whether the interval was discarded (< 0 or > 1000 ms). Pure
 * bookkeeping for the heartbeat trace; server_tick_clock_advance is this
 * with the extras dropped, so its result is byte-identical. */
static inline unsigned server_tick_clock_advance_ex(struct server_tick_clock* clock,
													int32_t elapsed_ms,
													unsigned* raw_ticks,
													bool* discarded) {
	bool ok = elapsed_ms >= 0 && elapsed_ms <= 1000;
	double seconds = ok ? (double)elapsed_ms / 1000.0 : 0.0;
	clock->partial_ticks = (float)((double)clock->partial_ticks + seconds * 20.0);
	unsigned ticks = (unsigned)clock->partial_ticks;
	clock->partial_ticks -= (float)ticks;
	if(raw_ticks)
		*raw_ticks = ticks;
	if(discarded)
		*discarded = !ok;
	return ticks > 10 ? 10 : ticks;
}

static inline unsigned server_tick_clock_advance(struct server_tick_clock* clock,
												 int32_t elapsed_ms) {
	return server_tick_clock_advance_ex(clock, elapsed_ms, NULL, NULL);
}

/* SERVER_TICK_SAMPLE_EACH_TICK (T2 pacing, 2026-09-11, Astra round 8 on the stg30
 * tclk trace): the loop used to sample the clock only once its tick batch had
 * drained ("consume Timer.elapsedTicks in order before sampling again"). A
 * batch of long streaming ticks then made the NEXT sample longer than 500 ms
 * and the per-sample cap of 10 threw ticks away: 5.8% of the wall-time
 * entitlement on a two-anchor run whose quiet seconds ran 20.0. Sampling at
 * every active loop entry keeps each sample the length of one iteration (a
 * normal-speed Beta client samples every ~16 ms frame and never meets the cap
 * either); admitted ticks ADD to the pending count and one is consumed per
 * iteration. Tick results are untouched -- only how many ticks run per wall
 * second. Total debt is bounded at the Beta dedicated server's interval clamp
 * (MinecraftServer.run: 2000 ms = 40 ticks) and the bound is traced as capped
 * ticks, never silent. 0 restores the batch rule (same code path). */
/* PACING_DEFAULT_BATCH_2026_09_11: stg31-39 on the real Wii. Per-tick sampling
 * delivers the entitlement (+1.0 TPS at 42/256) but every per-tick run hit its
 * worst second at the same saturated route point (second 215, -65/465) with
 * 30-39 ticks owed: executing those ticks costs exactly the streaming the
 * frontier needs there (missing max 26-33 vs 17-18 under the batch rule, which
 * DROPS them -- the Beta client Timer's own overload behaviour). Neither the
 * catch-up limiter (stg38) nor the lean threshold (stg39) changed that. The
 * maintainer weighs holes above the last tick: batch rule by default until the
 * saturation itself is reduced (the 219 ms/s of chunk saving on the server
 * thread). Per-tick sampling stays as a tested, switchable experiment. */
#define SERVER_TICK_SAMPLE_EACH_TICK 0
#define SERVER_TICK_PENDING_MAX 40u

struct server_tick_pacer {
	struct server_tick_clock clock;
	unsigned pending;
};
struct server_tick_admission {
	bool sampled;   /* this entry advanced the clock (the caller consumes elapsed) */
	bool discarded; /* the sampled interval was < 0 or > 1000 ms */
	unsigned raw;   /* whole ticks the interval was worth before the cap of 10 */
	unsigned admitted;       /* ticks added to pending this entry (<= 10) */
	unsigned capped_pending; /* ticks dropped by the SERVER_TICK_PENDING_MAX bound */
};

/* One server-loop entry: sample per policy, then consume one pending tick.
 * Returns true when a tick must run now. Pure, so the test suite drives the
 * production policy on scripted timelines. */
static inline bool server_tick_pacer_enter(struct server_tick_pacer* p,
										   int32_t elapsed_ms, bool sample_each_tick,
										   struct server_tick_admission* a) {
	a->sampled = p->pending == 0 || sample_each_tick;
	a->discarded = false;
	a->raw = a->admitted = a->capped_pending = 0;
	if(a->sampled) {
		a->admitted = server_tick_clock_advance_ex(&p->clock, elapsed_ms, &a->raw,
													&a->discarded);
		p->pending += a->admitted;
		if(p->pending > SERVER_TICK_PENDING_MAX) {
			a->capped_pending = p->pending - SERVER_TICK_PENDING_MAX;
			p->pending = SERVER_TICK_PENDING_MAX;
		}
	}
	if(p->pending == 0)
		return false;
	p->pending--;
	return true;
}

/* SERVER_TICK_CATCHUP_LIMIT (T2 pacing EXPERIMENT, 2026-09-11, Astra round 10 on
 * the paired A/B): per-tick sampling recovered the entitlement but the catch-up
 * bursts (27-34 ticks in one second after a saturated stretch) crowded out
 * streaming and the mesher for exactly that second -- the worst-second holes
 * went from 17-18 to 22-26 missing columns in both A/B orders. The limiter
 * allows at most SERVER_TICK_START_LIMIT tick STARTS per rolling 50 ms; debt
 * is retained (the pacer's pending count is untouched, its 40-tick bound
 * stays) and the loop SLEEPS until a start is eligible, which is exactly the
 * blocking window the lower-priority mesher needs. On-time operation (one
 * start per 50 ms) never waits. 0 disables it on the same path. */
/* stg38 (2026-09-11): the limiter engaged (38.8 ms/s of limiter sleep) but the
 * worst-second holes stayed deep (missing max 27) and TPS/cap loss got worse
 * -> OFF; the code stays as the documented experiment. */
#define SERVER_TICK_CATCHUP_LIMIT 0
#define SERVER_TICK_START_LIMIT 2u
/* SERVER_TICK_LEAN_PENDING (T2 pacing experiment, 2026-09-11): per-tick sampling
 * raised the share of LEAN iterations (no generation, no far work) from 24-25%
 * of ticks under the batch rule to 31-34%, because small debts (1-2 ticks) are
 * now visible on most catch-up entries. Busy seconds therefore lose generation
 * exactly when the frontier needs it (the deeper worst-second holes of the
 * A/B). Lean only when the debt is real: at least this many ticks still owed
 * after the current one (3 = 150 ms behind). 1 restores the old rule. */
#define SERVER_TICK_LEAN_PENDING 1u /* stg39: 3 did not help; the old rule */
struct server_tick_limiter {
	int64_t start_ms[SERVER_TICK_START_LIMIT]; /* ring of the latest starts; head = oldest */
	unsigned count, head;
};
/* Milliseconds to wait before a tick may start at now_ms (0 = start now). */
static inline unsigned server_tick_limiter_wait_ms(const struct server_tick_limiter* l,
													 int64_t now_ms) {
	if(!SERVER_TICK_CATCHUP_LIMIT || l->count < SERVER_TICK_START_LIMIT)
		return 0;
	int64_t eligible = l->start_ms[l->head] + SERVER_TICK_PERIOD_MS;
	return now_ms >= eligible ? 0u : (unsigned)(eligible - now_ms);
}
static inline void server_tick_limiter_note_start(struct server_tick_limiter* l,
												   int64_t now_ms) {
	if(l->count < SERVER_TICK_START_LIMIT) {
		l->start_ms[(l->head + l->count) % SERVER_TICK_START_LIMIT] = now_ms;
		l->count++;
	} else {
		l->start_ms[l->head] = now_ms; /* the oldest slot becomes the newest */
		l->head = (l->head + 1) % SERVER_TICK_START_LIMIT;
	}
}

static inline size_t server_tick_clock_wait_ms(const struct server_tick_clock* clock) {
	/* Round up to a whole millisecond; the next sample retains any lateness. */
	double remaining = (1.0 - (double)clock->partial_ticks) * SERVER_TICK_PERIOD_MS;
	size_t ms = (size_t)remaining;
	return ms + (remaining > (double)ms);
}

/*
	Return the inter-iteration sleep. Kept as a pure helper so the Wii server
	thread's wall-clock pacing can be regression-tested without starting its
	infinite loop.
*/
static inline size_t server_tick_sleep_ms(bool finished_loading,
									  int32_t elapsed_ms) {
	const size_t period
		= finished_loading ? SERVER_TICK_PERIOD_MS : SERVER_LOAD_PERIOD_MS;
	if(elapsed_ms <= 0)
		return period;
	if((size_t)elapsed_ms >= period)
		return 0;
	return period - (size_t)elapsed_ms;
}

/* Wii's libogc shim maps larger public priorities to smaller kernel ones.
 * KThreadReschedule only preempts a STRICTLY lower-priority current thread.
 * Main is 64: server=64 delayed VI/sleep wakeups until the server blocked.
 * Keep the server at 63 throughout play as well as loading. The 50 ms tick
 * period, update order, RNG stream and overrun sleep are unchanged. Hardware
 * frame-wait telemetry and server tick progress gate this scheduling change.
 */
#define SERVER_THREAD_PRIORITY 63

#endif
