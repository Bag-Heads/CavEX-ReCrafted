/* REL-16a: soak_idle_play pure timer/phase core against fake time. Every
   threshold in LIM below is a TEST-ONLY mock: the approved soak values come
   from the measured baseline (tmp/jobs/release-plan/REL-16a/CONTRACT.md). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/soak_idle_play_core.h"

/* Reports every failed check (not only the first) and fails the test at exit. */
static int fails;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while(0)

static const struct soak_ip_limits LIM = {.idle_s=600, .play_s=600,
	.warmup_s=1200, .min_play_blocks=16, .min_play_chunks=2, .max_drop_kb=512};

enum { NONE, NO_MOVE, GAP, CRASH, DOG, NEW_PID, BACK, LEAK, FLOOR, REBEGIN, EDGE };

/* Feeds one 1 Hz heartbeat per fake second from uptime 100 for `secs`. */
static enum soak_ip_fail run(struct soak_ip_limits lim, unsigned secs, int fault,
		struct soak_ip_state* st) {
	struct soak_ip_sample s = {.process_id=7, .world_id=9, .mem1_free_kb=8000,
		.carve_free_kb=4000};
	unsigned t;
	assert(soak_ip_begin(st, &lim, 100));
	for(t = 0; t < secs; t++) {
		s.uptime_s = 100 + t;
		s.hb_seq++;
		if(fault == GAP && t == 5000) s.uptime_s += 6, t += 6;
		if(fault == EDGE) { /* counters move only at idle->play edges */
			if(soak_ip_phase_at(st, s.uptime_s) == SOAK_IP_PLAY
			   && soak_ip_phase_at(st, s.uptime_s - 1) == SOAK_IP_IDLE)
				s.moved_blocks += 100, s.chunks_loaded += 10;
		} else if(soak_ip_phase_at(st, s.uptime_s) == SOAK_IP_PLAY && fault != NO_MOVE) {
			s.moved_blocks++;
			if(t % 60 == 0) s.chunks_loaded++;
		} else if(fault == LEAK && t % 1200 == 0) {
			s.mem1_free_kb -= 100; /* 100 KiB per cycle: below 512 per cycle */
		}
		s.crashed = fault == CRASH && t == 7000;
		s.watchdog_fired = fault == DOG && t == 7000;
		if(fault == NEW_PID && t == 7000) s.process_id = 8;
		if(fault == BACK && t == 7000) s.moved_blocks = 0;
		if(fault == FLOOR && t == 7000) s.carve_free_kb = 2047;
		if(fault == REBEGIN && t == 7000) {
			struct soak_ip_limits loose = lim;
			loose.max_drop_kb = 100000;
			assert(!soak_ip_begin(st, &loose, s.uptime_s)); /* refused */
			assert(st->lim.max_drop_kb == lim.max_drop_kb);
		}
		if(fault == REBEGIN && t % 1200 == 0) s.mem1_free_kb -= 100;
		soak_ip_observe(st, &s);
	}
	return soak_ip_finish(st);
}

int main(void) {
	struct soak_ip_state st;
	struct soak_ip_limits zero = {0}, all_idle = LIM, late = LIM;
	all_idle.idle_s = 20000;
	late.warmup_s = 20000; /* baseline never reached inside the 3 h run */

	/* Timer alternates idle -> play -> idle on the contract's segments. */
	memset(&st, 0, sizeof st);
	CHECK(soak_ip_begin(&st, &LIM, 100));
	CHECK(soak_ip_phase_at(&st, 100) == SOAK_IP_IDLE);
	CHECK(soak_ip_phase_at(&st, 699) == SOAK_IP_IDLE);
	CHECK(soak_ip_phase_at(&st, 700) == SOAK_IP_PLAY);
	CHECK(soak_ip_phase_at(&st, 1300) == SOAK_IP_IDLE);
	CHECK(soak_ip_phase_at(&st, 99) == SOAK_IP_IDLE);

	/* Three continuous hours of alternating engagement: PASS in no real time. */
	memset(&st, 0, sizeof st);
	CHECK(run(LIM, 10801, NONE, &st) == SOAK_IP_PASS);
	CHECK(st.idle_segments >= 9 && st.play_segments >= 9);
	CHECK(st.last_uptime_s - st.start_s >= SOAK_IP_MIN_DURATION_S);

	/* Fail-closed controls, each naming its own cause. Slow LEAK: 100 KiB per
	   cycle (under the 512 per-cycle mock), yet the post-warmup idle baseline
	   survives every phase change, so the cumulative trend fails; REBEGIN
	   shows a second begin cannot erase that history. */
	static const struct { int fault; unsigned secs; int lim; enum soak_ip_fail want; } c[] = {
		{NONE, 10799, 0, SOAK_IP_FAIL_SHORT}, {NONE, 10801, 1, SOAK_IP_FAIL_UNAPPROVED_LIMITS},
		{NONE, 10801, 2, SOAK_IP_FAIL_MISSING_PHASE}, {NONE, 0, 0, SOAK_IP_FAIL_MISSING_PHASE},
		{NO_MOVE, 10801, 0, SOAK_IP_FAIL_NO_PROGRESS}, {GAP, 10801, 0, SOAK_IP_FAIL_TELEMETRY_GAP},
		{CRASH, 10801, 0, SOAK_IP_FAIL_CRASH}, {DOG, 10801, 0, SOAK_IP_FAIL_WATCHDOG},
		{NEW_PID, 10801, 0, SOAK_IP_FAIL_PROCESS_RESET}, {BACK, 10801, 0, SOAK_IP_FAIL_PROCESS_RESET},
		{FLOOR, 10801, 0, SOAK_IP_FAIL_MEMORY_FLOOR}, {LEAK, 10801, 0, SOAK_IP_FAIL_LEAK},
		{REBEGIN, 10801, 0, SOAK_IP_FAIL_LEAK},
		/* attempt-2 negatives: missing leak evidence and edge-only "play" */
		{NONE, 10801, 3, SOAK_IP_FAIL_NO_MEMORY_HISTORY}, {EDGE, 10801, 0, SOAK_IP_FAIL_NO_PROGRESS}};
	for(unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		const struct soak_ip_limits* l = c[i].lim == 1 ? &zero : c[i].lim == 2 ? &all_idle
			: c[i].lim == 3 ? &late : &LIM;
		memset(&st, 0, sizeof st);
		enum soak_ip_fail got = run(*l, c[i].secs, c[i].fault, &st);
		if(got != c[i].want)
			fprintf(stderr, "case %u: fault %d got %d want %d\n", i, c[i].fault, got, c[i].want);
		CHECK(got == c[i].want);
		if(c[i].want == SOAK_IP_FAIL_LEAK)
			CHECK(st.mem1_base_kb > 0 && st.fail_uptime_s > st.start_s + LIM.warmup_s);
	}
	if(fails)
		return 1;
	puts("test_soak_idle_play_core: PASS");
	return 0;
}
