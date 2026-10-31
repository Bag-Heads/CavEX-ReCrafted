/* Copyright (c) 2026 CaveWii_v2 contributors.
 * REL-10-tests (R10 / BUG-TICK 1017 PLAN): MAIN pacing eligibility over the
 * WHOLE local server tick, not only the normal light-drain window.
 *
 * Drives the production platform/thread.c pacing checkpoint and the existing
 * server tick boundary calls exactly as server_local_thread makes them
 * (server_local.c: thread_g9_lease_local + thread_g9_tick_boost_local(true,
 * SERVER_THREAD_PRIORITY) at thread start, thread_g9_tick_boost_begin() before
 * server_local_update and thread_g9_tick_boost_end() after the epilogue,
 * thread_g9_tick_boost_local(false, ...) at exit) with the tick boost itself
 * OFF (experimental.g9_tick_boost_int=0, the shipped setting). No new API is
 * used: REL-10 must make these existing boundaries publish the tick owner and
 * active state independent of the boost and diagnostic toggles.
 *
 * Harness: time_get is link-wrapped to a fake monotonic clock, so the 2000-us
 * slice is decided exactly, and usleep is link-wrapped so a MAIN-thread
 * pacing sleep is recorded (requested duration, fake clock advanced) instead
 * of sleeping; other threads keep the real usleep. The "server" threads are
 * real threads in the states the plan names; they spin (runnable) unless the
 * case blocks them through the real thread.c wait primitives.
 *
 * Required (preset 1: 1000-us shipped slice and 2000-us opt-out slice, both run; 1-ms sleeps, 8 ms requested per frame):
 *   ELIGIBLE  local owner inside a gameplay tick, runnable, no drain, no lock
 *             or dependency: a checkpoint past the slice sleeps exactly 1 ms;
 *             the slice, the per-frame cap and next-frame recovery hold.
 *   REFUSED   pacing OFF (explicit 0); remote/no local owner; an unregistered
 *             thread's tick begin; idle between ticks; owner torn down during
 *             a tick; server blocked in a channel wait; server sleeping;
 *             server inside a lease dependency (MAIN-owned recovery/COW);
 *             MAIN holding a column stripe the server needs.
 * Harness controls that must hold BEFORE and AFTER REL-10 (exit 2, no RED):
 * the existing drain-window eligibility sleeps past the slice and refuses
 * inside it, and no server at all never sleeps.
 *
 * Failures are controlled: each prints "REL-10-tests RED: <case>" and the
 * process exits 1 (never an assert abort for an expectation). */
#include "platform/thread.h"
#include "platform/time.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- fake clock + recorded MAIN sleeps ---------------------------------- */
static uint64_t s_fake_us = 1000000u;
static pthread_t s_main_tid;
static uint64_t s_main_sleep_calls, s_main_sleep_us;

ptime_t __real_time_get(void);
ptime_t __wrap_time_get(void) {
	uint64_t us = __atomic_load_n(&s_fake_us, __ATOMIC_ACQUIRE);
	ptime_t t;
	t.tv_sec = (time_t)(us / 1000000u);
	t.tv_nsec = (long)(us % 1000000u) * 1000L;
	return t;
}
int __real_usleep(useconds_t us);
int __wrap_usleep(useconds_t us) {
	if(!pthread_equal(pthread_self(), s_main_tid)) return __real_usleep(us);
	s_main_sleep_calls++;
	s_main_sleep_us += us;
	__atomic_fetch_add(&s_fake_us, (uint64_t)us, __ATOMIC_ACQ_REL);
	return 0;
}
static void advance_us(uint64_t us) { __atomic_fetch_add(&s_fake_us, us, __ATOMIC_ACQ_REL); }
static void real_wait_ms(unsigned ms) { __real_usleep((useconds_t)ms * 1000u); }

/* ---- reporting ---------------------------------------------------------- */
static unsigned s_red, s_harness;
static bool s_with_drain; /* see tick_begin */
static bool s_in_controls;
static void expect(bool ok, const char* name, const char* detail) {
	if(ok) return;
	if(s_in_controls) {
		printf("REL-10-tests HARNESS FAIL: %s: %s\n", name, detail);
		s_harness++;
	} else {
		printf("REL-10-tests RED: %s%s: %s\n", name,
			s_with_drain ? " [inside drain window]" : "", detail);
		s_red++;
	}
}

/* One MAIN checkpoint with the slice satisfied (or not, by `advance`).
 * Returns whether it slept; cross-checks the return against the recorded
 * MAIN sleep so a hidden sleep or a phantom "true" is itself a failure. */
static bool checkpoint_after(uint64_t advance, const char* name) {
	advance_us(advance);
	const uint64_t calls = s_main_sleep_calls, us = s_main_sleep_us;
	const bool slept = thread_main_pacing_checkpoint_at(THREAD_PACING_OTHER);
	const uint64_t dc = s_main_sleep_calls - calls, du = s_main_sleep_us - us;
	if(slept) expect(dc == 1 && du == 1000u, name, "a reported sleep must be exactly one 1-ms request");
	else expect(dc == 0 && du == 0, name, "a refused checkpoint slept anyway");
	return slept;
}
#define PAST_SLICE 2500u

/* ---- server-side states ------------------------------------------------- */
enum mode {
	M_DRAIN_ONLY,   /* control: the existing drain-window donation */
	M_TICK,         /* local owner, gameplay tick active, runnable */
	M_IDLE,         /* local owner, between ticks (tick ended) */
	M_UNREGISTERED, /* tick begin from a thread that never registered */
	M_TEARDOWN,     /* owner unregistered while its tick was active */
	M_BLOCK_RECV,   /* inside a tick, blocked in a channel receive */
	M_BLOCK_SLEEP,  /* inside a tick, sleeping in thread_msleep */
	M_DEPENDENCY,   /* inside a tick, inside a lease dependency */
	M_TWO_TICKS,    /* overdue escalation: tick 1 (release 1), then tick 2 (release 2) */
};
static struct thread_channel s_wake;
static int s_stage, s_release;
static int stage_get(void) { return __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE); }
static void stage_set(int v) { __atomic_store_n(&s_stage, v, __ATOMIC_RELEASE); }
static void spin_until_release(int value) {
	while(__atomic_load_n(&s_release, __ATOMIC_ACQUIRE) < value) {}
}
static void wait_stage(int v) {
	for(unsigned n = 0; stage_get() < v; n++) {
		if(n > 20000) { printf("REL-10-tests HARNESS FAIL: server stage %d timeout\n", v); exit(2); }
		real_wait_ms(1);
	}
}
static void register_owner(void) {
	thread_g9_lease_local(true, 63);       /* server_local.c thread start */
	thread_g9_tick_boost_local(true, 63);
}
/* Production drains run INSIDE the gameplay tick (server_world.c light slice
 * arm); with s_with_drain the tick-mode server also owns the drain window, so
 * the refusal cases are not vacuous against today's drain-only eligibility. */
static void tick_begin(void) {
	thread_g9_tick_boost_begin();
	if(s_with_drain) thread_drain_wait_begin();
}
static void tick_end(void) {
	if(s_with_drain) { struct thread_drain_waits w; thread_drain_wait_end(&w); }
	thread_g9_tick_boost_end();
}
static void unregister_owner(void) {
	thread_g9_tick_boost_local(false, 63); /* server_local.c thread exit */
	thread_g9_lease_local(false, 63);
}
static void* server_entry(void* arg) {
	const enum mode m = (enum mode)(intptr_t)arg;
	switch(m) {
	case M_DRAIN_ONLY: {
		thread_drain_wait_begin();
		stage_set(1);
		spin_until_release(1);
		struct thread_drain_waits w;
		thread_drain_wait_end(&w);
		break;
	}
	case M_TICK:
		register_owner();
		tick_begin();
		stage_set(1);
		spin_until_release(1);
		tick_end();
		unregister_owner();
		break;
	case M_IDLE:
		register_owner();
		thread_g9_tick_boost_begin();
		thread_g9_tick_boost_end();
		stage_set(1);
		spin_until_release(1);
		unregister_owner();
		break;
	case M_UNREGISTERED:
		thread_g9_tick_boost_begin();
		stage_set(1);
		spin_until_release(1);
		thread_g9_tick_boost_end();
		break;
	case M_TEARDOWN:
		register_owner();
		thread_g9_tick_boost_begin();
		unregister_owner();
		stage_set(1);
		spin_until_release(1);
		thread_g9_tick_boost_end();
		break;
	case M_BLOCK_RECV: {
		register_owner();
		tick_begin();
		stage_set(1);
		void* msg;
		if(!tchannel_receive(&s_wake, &msg, true)) {
			printf("REL-10-tests HARNESS FAIL: channel receive\n");
			exit(2);
		}
		stage_set(2);
		spin_until_release(1);
		tick_end();
		unregister_owner();
		break;
	}
	case M_BLOCK_SLEEP:
		register_owner();
		tick_begin();
		stage_set(1);
		thread_msleep(400);
		stage_set(2);
		spin_until_release(1);
		tick_end();
		unregister_owner();
		break;
	case M_TWO_TICKS:
		register_owner();
		tick_begin();
		stage_set(1);
		spin_until_release(1);
		tick_end();
		tick_begin();
		stage_set(2);
		spin_until_release(2);
		tick_end();
		unregister_owner();
		break;
	case M_DEPENDENCY:
		register_owner();
		tick_begin();
		thread_g9_lease_dependency();
		stage_set(1);
		spin_until_release(1);
		thread_g9_lease_dependency_done();
		stage_set(2);
		spin_until_release(2);
		tick_end();
		unregister_owner();
		break;
	}
	return NULL;
}
static struct thread s_server;
static void server_start(enum mode m) {
	stage_set(0);
	__atomic_store_n(&s_release, 0, __ATOMIC_RELEASE);
	if(!thread_create(&s_server, server_entry, (void*)(intptr_t)m, 63)) {
		printf("REL-10-tests HARNESS FAIL: thread_create\n");
		exit(2);
	}
	wait_stage(1);
}
static void server_release(int v) { __atomic_store_n(&s_release, v, __ATOMIC_RELEASE); }
static void server_finish(void) {
	server_release(1000);
	thread_join(&s_server);
}
static unsigned s_slice_us = 2000; /* REL-10-slice: the guards run at the 2000 opt-out slice and at the shipped 1000 slice */
/* The REL-10 base rules are pinned with the overdue escalation OFF; the
 * escalation has its own contract below (overdue_escalation). */
static void preset1(bool on) {
	thread_main_pacing_configure(on, s_slice_us, 1, 8);
	thread_main_pacing_overdue_configure(0, THREAD_PACING_OVERDUE_BUDGET_MS_DEFAULT);
}

/* ---- harness controls (GREEN before and after REL-10) -------------------- */
static void controls(void) {
	s_in_controls = true;
	preset1(true);
	expect(!checkpoint_after(PAST_SLICE, "control/no-server"), "control/no-server",
		"no server and no drain must never sleep");
	server_start(M_DRAIN_ONLY);
	thread_main_pacing_frame();
	expect(checkpoint_after(PAST_SLICE, "control/drain"), "control/drain",
		"existing runnable drain-window donation must sleep past the slice");
	expect(!checkpoint_after(1999u, "control/drain-slice"), "control/drain-slice",
		"fake clock: 1999 us after a sleep is inside the 2000-us slice");
	expect(checkpoint_after(1u, "control/drain-slice-edge"), "control/drain-slice-edge",
		"fake clock: exactly 2000 us after a sleep is past the slice");
	server_finish();
	expect(!checkpoint_after(PAST_SLICE, "control/drain-ended"), "control/drain-ended",
		"a completed drain must stop donation");
	preset1(false);
	s_in_controls = false;
}

/* ---- REL-10 requirements ------------------------------------------------- */
static void eligible_whole_tick(void) {
	preset1(true);
	server_start(M_TICK);
	thread_main_pacing_frame();
	expect(checkpoint_after(PAST_SLICE, "eligible"), "non-drain active tick",
		"runnable local tick outside the light-drain window did not donate");
	expect(!checkpoint_after(1999u, "slice"), "non-drain slice",
		"slept inside the 2000-us MAIN slice");
	expect(checkpoint_after(1u, "slice-edge"), "non-drain slice edge",
		"did not sleep once the 2000-us slice had elapsed");
	/* Per-frame cap: 8 x 1 ms requested, then refuse until the next frame. */
	thread_main_pacing_frame();
	const uint64_t frame_us = s_main_sleep_us;
	unsigned sleeps = 0;
	for(unsigned i = 0; i < 12; i++)
		if(checkpoint_after(PAST_SLICE, "frame-cap")) sleeps++;
	char detail[128];
	snprintf(detail, sizeof detail, "%u sleeps / %" PRIu64 " us requested in one frame (want 8 / 8000)",
		sleeps, s_main_sleep_us - frame_us);
	expect(sleeps == 8 && s_main_sleep_us - frame_us == 8000u, "non-drain frame cap", detail);
	thread_main_pacing_frame();
	expect(checkpoint_after(PAST_SLICE, "frame-recover"), "non-drain next frame",
		"next frame did not recover donation during the active tick");
	server_finish();
	expect(!checkpoint_after(PAST_SLICE, "tick-ended"), "tick ended",
		"slept after the server tick ended and the owner exited");
	uint32_t st[THREAD_PACING_WORDS];
	if(thread_main_pacing_snapshot(st)) {
		snprintf(detail, sizeof detail, "pacing counters sleeps=%u requested_ms=%u (want 11/11)",
			st[11], st[12]);
		expect(st[11] == 11u && st[12] == 11u, "non-drain counters", detail);
	}
	preset1(false);
}

/* REL-10-slice: 999/1000-us boundary, 1-ms sleeps and the 8-ms cap at the shipped slice. */
static void slice_1000_boundary(void) {
	preset1(true);
	server_start(M_TICK);
	thread_main_pacing_frame();
	expect(checkpoint_after(PAST_SLICE, "s1000"), "1000/eligible",
		"runnable local tick did not donate at the 1000-us slice");
	expect(!checkpoint_after(999u, "s1000-in"), "1000/inside",
		"slept 999 us after a sleep (inside the 1000-us slice)");
	expect(checkpoint_after(1u, "s1000-edge"), "1000/edge",
		"did not sleep at exactly 1000 us after a sleep");
	thread_main_pacing_frame();
	const uint64_t frame_us = s_main_sleep_us;
	unsigned sleeps = 0;
	for(unsigned i = 0; i < 12; i++)
		if(checkpoint_after(1000u, "s1000-cap")) sleeps++;
	expect(sleeps == 8 && s_main_sleep_us - frame_us == 8000u, "1000/frame cap",
		"1000-us slice did not keep the 8 x 1 ms per-frame cap");
	server_finish();
	preset1(false);
}

static void refused_off(void) {
	preset1(false); /* explicit experimental.g9_main_pacing_int=0 */
	server_start(M_TICK);
	thread_main_pacing_frame();
	expect(!checkpoint_after(PAST_SLICE, "off"), "pacing off",
		"explicit OFF slept during an eligible tick");
	server_finish();
}

static void refused_simple(enum mode m, const char* name, const char* detail) {
	preset1(true);
	server_start(m);
	thread_main_pacing_frame();
	expect(!checkpoint_after(PAST_SLICE, name), name, detail);
	server_finish();
	preset1(false);
}

static void refused_remote(void) {
	/* Remote Beta MP: no local server thread exists at all; MAIN keeps
	 * checkpointing while a non-server thread is busy. */
	preset1(true);
	server_start(M_DRAIN_ONLY); /* drain-control thread, then ended */
	server_finish();
	thread_main_pacing_frame();
	expect(!checkpoint_after(PAST_SLICE, "remote"), "remote/no local owner",
		"slept with no local server owner");
	preset1(false);
}

static void refused_blocked_recv(void) {
	preset1(true);
	if(!tchannel_init(&s_wake, 1)) { printf("REL-10-tests HARNESS FAIL: channel\n"); exit(2); }
	server_start(M_BLOCK_RECV);
	real_wait_ms(150); /* the owner is parked in the real channel wait */
	thread_main_pacing_frame();
	expect(!checkpoint_after(PAST_SLICE, "blocked-recv"), "blocked server (channel wait)",
		"slept while the tick owner was blocked in a wait (no CPU demand)");
	static int token = 1;
	if(!tchannel_send(&s_wake, &token, true)) { printf("REL-10-tests HARNESS FAIL: send\n"); exit(2); }
	wait_stage(2);
	expect(checkpoint_after(PAST_SLICE, "blocked-recv-resumed"), "blocked server resumed",
		"did not donate once the owner was runnable again inside its tick");
	server_finish();
	tchannel_close(&s_wake);
	preset1(false);
}

static void refused_blocked_sleep(void) {
	preset1(true);
	server_start(M_BLOCK_SLEEP);
	real_wait_ms(100); /* inside the server's own 400-ms thread_msleep */
	thread_main_pacing_frame();
	if(stage_get() != 1) { printf("REL-10-tests HARNESS FAIL: server left its sleep early\n"); exit(2); }
	expect(!checkpoint_after(PAST_SLICE, "blocked-sleep"), "blocked server (sleeping)",
		"slept while the tick owner was itself sleeping");
	wait_stage(2);
	server_finish();
	preset1(false);
}

static void refused_dependency(void) {
	preset1(true);
	server_start(M_DEPENDENCY);
	thread_main_pacing_frame();
	expect(!checkpoint_after(PAST_SLICE, "dependency"), "lease dependency",
		"slept while the server was inside a MAIN-progress dependency");
	server_release(1);
	wait_stage(2);
	expect(checkpoint_after(PAST_SLICE, "dependency-done"), "dependency released",
		"did not donate after the dependency ended inside the tick");
	server_finish();
	preset1(false);
}

static struct thread_mutex s_stripe;
static void refused_main_holds_stripe(void) {
	preset1(true);
	server_start(M_TICK);
	thread_main_pacing_frame();
	thread_stripe_lock(&s_stripe);
	expect(!checkpoint_after(PAST_SLICE, "main-lock"), "MAIN holds a server stripe",
		"slept while MAIN held a column stripe the server needs");
	thread_stripe_unlock(&s_stripe);
	expect(checkpoint_after(PAST_SLICE, "main-lock-released"), "MAIN released the stripe",
		"did not donate after MAIN released the stripe");
	server_finish();
	preset1(false);
}

/* ---- REL-10-overdue: escalation for an overdue owner tick ------------------
 * hw1030f (perf_stress_5m, bc82e65): one 129 ms server tick at ~20 ms of CPU
 * while MAIN ran saturated (vsync 0.0 ms) and the 8 ms/frame cap limited the
 * donation. Contract (shipped defaults 12 ms / 40 ms, slice 1000, sleep 1,
 * cap 8): an owner tick older than 12 ms bypasses the frame cap and halves
 * the slice; at most 40 ms of escalated sleep per tick, reset by the next
 * tick; a foreign drain, an idle owner and escalation OFF keep the base
 * rules. */
static void overdue_escalation(void) {
	char detail[160];
	uint32_t os[THREAD_PACING_OVERDUE_WORDS];
	thread_main_pacing_configure(true, 1000, 1, 8); /* shipped preset 1 + defaults */
	expect(thread_main_pacing_overdue_snapshot(os) && os[0] == 12000u && os[1] == 40u,
		"overdue/defaults", "configure() must apply 12 ms / 40 ms escalation defaults");

	server_start(M_TWO_TICKS);
	thread_main_pacing_frame();
	/* Each iteration: +1000 us, then a 1 ms sleep -> the tick ages 2 ms per
	 * sleep. Ages 1..11 ms: six base sleeps (cap 8 -> 2 left). Age 13+: overdue:
	 * two more inside the cap, then four the cap would have refused. */
	unsigned sleeps = 0;
	for(unsigned i = 0; i < 12; i++)
		if(checkpoint_after(1000u, "overdue/escalate")) sleeps++;
	thread_main_pacing_overdue_snapshot(os);
	snprintf(detail, sizeof detail,
		"%u/12 sleeps, ticks=%u escalated=%u cap_bypassed=%u exhausted=%u (want 12, 1, 6, 4, 0)",
		sleeps, os[2], os[3], os[4], os[5]);
	expect(sleeps == 12 && os[2] == 1u && os[3] == 6u && os[4] == 4u && os[5] == 0u,
		"overdue/escalate", detail);
	/* Half slice while overdue: 499 us after the last sleep refuses, 500 sleeps. */
	expect(!checkpoint_after(499u, "overdue/half-slice"), "overdue/half-slice",
		"slept 499 us into the 500-us escalated slice");
	expect(checkpoint_after(1u, "overdue/half-slice-edge"), "overdue/half-slice-edge",
		"did not sleep at the 500-us escalated slice edge");
	/* Budget: 3 ms of escalated sleep for this tick, then the base rules
	 * (frame cap already hit) refuse. */
	thread_main_pacing_overdue_configure(12, 3);
	sleeps = 0;
	for(unsigned i = 0; i < 6; i++)
		if(checkpoint_after(1000u, "overdue/budget")) sleeps++;
	thread_main_pacing_overdue_snapshot(os);
	snprintf(detail, sizeof detail, "%u/6 sleeps after a 3 ms budget, escalated=%u exhausted=%u (want 3, 3, >=1)",
		sleeps, os[3], os[5]);
	expect(sleeps == 3 && os[3] == 3u && os[5] >= 1u, "overdue/budget", detail);
	/* A new tick resets the budget: tick 2 at age 0 donates under the base
	 * rules; once 12 ms old it escalates again for another 3 ms. */
	server_release(1);
	wait_stage(2);
	thread_main_pacing_frame();
	expect(checkpoint_after(1000u, "overdue/tick2-base"), "overdue/tick2 base sleep",
		"a fresh tick inside the cap must still donate under the base rules");
	advance_us(12000u);
	sleeps = 0;
	for(unsigned i = 0; i < 3; i++)
		if(checkpoint_after(1000u, "overdue/tick2")) sleeps++;
	thread_main_pacing_overdue_snapshot(os);
	snprintf(detail, sizeof detail, "tick2: %u/3 escalated sleeps, ticks=%u escalated=%u (want 3, 2, 6)",
		sleeps, os[2], os[3]);
	expect(sleeps == 3 && os[2] == 2u && os[3] == 6u, "overdue/tick2 budget reset", detail);
	/* OFF restores REL-10 exactly: 8 sleeps per frame, then the cap refuses,
	 * even though tick 2 is far overdue. */
	thread_main_pacing_overdue_configure(0, 40);
	thread_main_pacing_frame();
	sleeps = 0;
	for(unsigned i = 0; i < 10; i++)
		if(checkpoint_after(1000u, "overdue/off")) sleeps++;
	snprintf(detail, sizeof detail, "%u/10 sleeps with escalation OFF on an overdue tick (want 8)", sleeps);
	expect(sleeps == 8, "overdue/off", detail);
	server_release(2);
	server_finish();
	expect(!checkpoint_after(2500u, "overdue/ended"), "overdue/ended",
		"slept after both ticks ended and the owner exited");

	/* Foreign drain donee (no tick owner) never escalates: cap holds. */
	thread_main_pacing_configure(true, 1000, 1, 8);
	server_start(M_DRAIN_ONLY);
	thread_main_pacing_frame();
	advance_us(30000u);
	sleeps = 0;
	for(unsigned i = 0; i < 12; i++)
		if(checkpoint_after(1000u, "overdue/foreign")) sleeps++;
	thread_main_pacing_overdue_snapshot(os);
	snprintf(detail, sizeof detail, "%u/12 sleeps for a foreign drain, ticks escalated=%u (want 8, 0)", sleeps, os[2]);
	expect(sleeps == 8 && os[2] == 0u, "overdue/foreign drain", detail);
	server_finish();
	/* An idle owner (between ticks) never donates, however old the clock. */
	server_start(M_IDLE);
	advance_us(30000u);
	expect(!checkpoint_after(2500u, "overdue/idle"), "overdue/idle",
		"slept for an idle owner with escalation enabled");
	server_finish();
	preset1(false);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	s_main_tid = pthread_self();
	thread_g9_tick_boost_configure(false, 50); /* shipped: boost OFF */
	thread_stripe_phase(THREAD_STRIPE_MAIN, 0);
	if(!thread_mutex_init(&s_stripe)) { printf("REL-10-tests HARNESS FAIL: mutex\n"); return 2; }
	thread_stripe_register(&s_stripe, 0);

	controls();
	if(s_harness) {
		printf("REL-10-tests HARNESS FAIL: %u control(s); eligibility not evaluated\n", s_harness);
		return 2;
	}
	for(int slice = 0; slice < 2; slice++) {
	s_slice_us = slice ? 1000u : 2000u;
	if(slice) slice_1000_boundary(); else eligible_whole_tick();
	refused_off();
	refused_remote();
	refused_simple(M_IDLE, "idle between ticks", "slept while the local server was between ticks");
	refused_simple(M_UNREGISTERED, "unregistered tick begin", "slept for a tick begun by a non-owner thread");
	refused_simple(M_TEARDOWN, "owner teardown", "slept after the tick owner unregistered mid-tick");
	for(int pass = 0; pass < 2; pass++) {
		s_with_drain = pass == 1; /* today's drain-only rule must also refuse these */
		refused_blocked_recv();
		refused_blocked_sleep();
		refused_dependency();
		refused_main_holds_stripe();
	}
	s_with_drain = false;
	}
	s_slice_us = 2000u;
	overdue_escalation();
	if(s_harness) {
		printf("REL-10-tests HARNESS FAIL: %u\n", s_harness);
		return 2;
	}
	if(s_red) {
		printf("REL-10-tests RED: %u whole-tick pacing eligibility expectation(s) unmet\n", s_red);
		return 1;
	}
	printf("test_main_tick_pacing: PASS\n");
	return 0;
}
