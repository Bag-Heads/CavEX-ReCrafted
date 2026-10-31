/* Copyright (c) 2026 CaveWii_v2 contributors
 * PC harness only: wrap real stripe locks, never replace synchronization.
 * The ceiling is asserted on the holder's THREAD CPU time: nothing inside a
 * stripe blocks or does I/O, so CPU time bounds the work done under the lock,
 * while wall time also counts OS preemption of the holder (a loaded host
 * failed 57/60 runs on wall time, quiet worst 1.2 ms; G1-LINUX-diag
 * 2026-09-25). Wall hold is still measured and printed for diagnosis; wait is
 * measured separately. */
#ifndef TESTS_COLUMN_STRIPE_TIMING_H
#define TESTS_COLUMN_STRIPE_TIMING_H
#define STRIPE_HOLD_CEILING_US 5000.0
/* G1 review F5: the thread-CPU ceiling cannot see a holder that BLOCKS (I/O,
 * a nested wait) while holding a stripe, so a loose wall-clock ceiling stays
 * too. 200 ms: 3.3x the worst preemption-inflated wall hold measured with
 * 12 busy loops saturating the 10-core host (59.96 ms, Windows, 2026-09-25;
 * quiet worst 0.5-1.2 ms), so load alone does not trip it, while a stall of
 * a disk read or a lost wakeup under the lock does. */
#define STRIPE_HOLD_WALL_CEILING_US 200000.0
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
struct stripe_times {
	double hold_us;      /* max thread-CPU hold (asserted) */
	double hold_wall_us; /* max wall hold (diagnostic: includes preemption) */
	double wait_us;
	unsigned acquisitions;
};
/* Thread CPU clock in microseconds. POSIX: CLOCK_THREAD_CPUTIME_ID.
 * Windows: QueryThreadCycleTime (GetThreadTimes ticks at the ~15.6 ms
 * scheduler quantum -- coarser than the ceiling), scaled by a cycles-per-us
 * rate calibrated once against time_get(). Calibration keeps the MAX of
 * several short spins: preemption during a spin only lowers the observed
 * rate, so the max is the undisturbed rate and the conversion never
 * under-reports a hold. */
static double stripe_cpu_us(void) {
#ifdef _WIN32
	static double cycles_per_us;
	if(cycles_per_us <= 0.0) {
		double best = 0.0;
		for(int k = 0; k < 8; k++) {
			ULONG64 c0, c1;
			QueryThreadCycleTime(GetCurrentThread(), &c0);
			ptime_t t0 = time_get();
			double us;
			do {
				us = time_diff_s(t0, time_get()) * 1e6;
			} while(us < 2000.0);
			QueryThreadCycleTime(GetCurrentThread(), &c1);
			double rate = (double)(c1 - c0) / us;
			if(rate > best) best = rate;
		}
		cycles_per_us = best > 0.0 ? best : 1.0;
	}
	ULONG64 cycles;
	QueryThreadCycleTime(GetCurrentThread(), &cycles);
	return (double)cycles / cycles_per_us;
#else
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
#endif
}
static struct thread_mutex* measured_stripes[COLUMN_BACKING_LOCK_STRIPES];
static unsigned measured_count;
static bool stripe_timing;
static __thread struct stripe_times stripe_times;
static __thread struct thread_mutex* held_stripe;
static __thread ptime_t held_since;
static __thread double held_since_cpu_us;
static bool measured_stripe(struct thread_mutex* m) {
	if(!stripe_timing) return false;
	for(unsigned i = 0; i < measured_count; i++)
		if(measured_stripes[i] == m) return true;
	return false;
}
static void measure_stripe(struct thread_mutex* m) {
	(void)stripe_cpu_us(); /* Windows: calibrate here, never while a stripe is held */
	for(unsigned i = 0; i < measured_count; i++)
		if(measured_stripes[i] == m) return;
	CHECK(measured_count < COLUMN_BACKING_LOCK_STRIPES);
	measured_stripes[measured_count++] = m;
}
/* Stripe acquisitions reach the column stripes two ways: plain
 * thread_mutex_lock/unlock, and the H1 breadcrumb API
 * thread_stripe_lock/unlock (chunk_mesher gather_shared, column_view). The
 * breadcrumb API takes the mutex inside thread.c (trylock fast path), where
 * the linker --wrap never sees it, so it is wrapped at its own layer too.
 * in_stripe_api keeps a fallback thread_mutex_* call made from inside the
 * wrapped stripe API from being counted a second time. */
static __thread unsigned in_stripe_api;
static void stripe_acquire_begin(struct thread_mutex* m, ptime_t* before) {
	(void)m;
	CHECK(!held_stripe); // no nested stripes, including bulk/fallback paths
	*before = time_get();
}
static void stripe_acquire_end(struct thread_mutex* m, ptime_t before) {
	held_since = time_get();
	held_since_cpu_us = stripe_cpu_us();
	double wait = time_diff_s(before, held_since) * 1e6;
	if(wait > stripe_times.wait_us) stripe_times.wait_us = wait;
	held_stripe = m;
	stripe_times.acquisitions++;
}
static void stripe_release(struct thread_mutex* m) {
	if(held_stripe == m) {
		double hold = stripe_cpu_us() - held_since_cpu_us;
		double hold_wall = time_diff_s(held_since, time_get()) * 1e6;
		if(hold > stripe_times.hold_us) stripe_times.hold_us = hold;
		if(hold_wall > stripe_times.hold_wall_us) stripe_times.hold_wall_us = hold_wall;
		held_stripe = NULL;
	}
}
void __real_thread_mutex_lock(struct thread_mutex*);
void __real_thread_mutex_unlock(struct thread_mutex*);
void __wrap_thread_mutex_lock(struct thread_mutex* m) {
	if(in_stripe_api || !measured_stripe(m)) { __real_thread_mutex_lock(m); return; }
	ptime_t before;
	stripe_acquire_begin(m, &before);
	__real_thread_mutex_lock(m);
	stripe_acquire_end(m, before);
}
void __wrap_thread_mutex_unlock(struct thread_mutex* m) {
	if(!in_stripe_api) stripe_release(m);
	__real_thread_mutex_unlock(m);
}
void __real_thread_stripe_lock(struct thread_mutex*);
void __real_thread_stripe_unlock(struct thread_mutex*);
void __wrap_thread_stripe_lock(struct thread_mutex* m) {
	if(!measured_stripe(m)) { __real_thread_stripe_lock(m); return; }
	ptime_t before;
	stripe_acquire_begin(m, &before);
	in_stripe_api++;
	__real_thread_stripe_lock(m);
	in_stripe_api--;
	stripe_acquire_end(m, before);
}
void __wrap_thread_stripe_unlock(struct thread_mutex* m) {
	stripe_release(m);
	in_stripe_api++;
	__real_thread_stripe_unlock(m);
	in_stripe_api--;
}
static void check_stripe_ceiling(const char* workload, struct stripe_times t) {
	printf("STRIPE %s: max hold=%.2f us (thread CPU) max wall hold=%.2f us "
		"max wait=%.2f us locks=%u ceiling=%.0f us\n",
		workload, t.hold_us, t.hold_wall_us, t.wait_us, t.acquisitions,
		STRIPE_HOLD_CEILING_US);
	CHECK(t.acquisitions && t.hold_us <= STRIPE_HOLD_CEILING_US);
	CHECK(t.hold_wall_us <= STRIPE_HOLD_WALL_CEILING_US);
}
#endif
