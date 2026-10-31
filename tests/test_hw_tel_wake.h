/* Included by test_hw_telemetry.c. Exercise the production inline consumer
 * and the SAME object signalled by real observer/heartbeat publication.
 * Host scheduling cannot bound elapsed wake latency or a minimum idle rate.
 * Instead inspect requested timeout budgets, then rendezvous with a genuinely
 * parked consumer whose timer escape is disabled. No product code is changed. */
#include "../source/platform/hw_tel_wake.h"
#include <pthread.h>
#include <time.h>
#include <stdint.h>

#define TEL_WAKE_TIMEOUT_MS 10u

struct tel_wake_loop {
	struct cw_tel_wake* wake;
	struct thread_mutex lock;
	struct thread_cond parked;
	uint32_t parks;      /* guarded by lock */
	uint32_t drained;    /* published to main by the next park/join */
	bool stop;           /* atomic: main asks the loop to exit */
};

static bool tel_wake_virtual_timeout;
static uint32_t tel_wake_wait_calls, tel_wake_budget_ms;
static struct tel_wake_loop* tel_wake_active_loop;

/* The macro at the first hw_tel_wake.h include redirects only its call in
 * this test TU. Default mode retains the real timed wait. Virtual mode
 * inspects idle pacing without confusing preemption with requested sleep.
 * Threaded mode retains the real mutex/condvar but has no timer fallback:
 * only a production signal can release the already-parked consumer. */
static bool tel_wake_test_timedwait(struct thread_cond* cond,
	struct thread_mutex* mutex, uint32_t timeout_ms) {
	if(tel_wake_virtual_timeout) {
		assert(timeout_ms == TEL_WAKE_TIMEOUT_MS);
		tel_wake_wait_calls++;
		tel_wake_budget_ms += timeout_ms;
		return false;
	}
	struct tel_wake_loop* loop = tel_wake_active_loop;
	if(!loop)
		return thread_cond_timedwait(cond, mutex, timeout_ms);
	assert(cond == &loop->wake->cond && mutex == &loop->wake->lock);
	assert(timeout_ms == TEL_WAKE_TIMEOUT_MS);
	thread_mutex_lock(&loop->lock);
	loop->parks++;
	thread_cond_signal(&loop->parked);
	thread_mutex_unlock(&loop->lock);
	/* cw_tel_wake_wait holds the wake mutex. The producer cannot set pending
	 * until this cond wait atomically releases it, so the park acknowledgement
	 * cannot race publication ahead of the actual wait. Ignore spurious wakes. */
	while(!loop->wake->pending)
		thread_cond_wait(cond, mutex);
	return true;
}

static int tel_wake_sink(const char* data, size_t len, void* user) {
	(void)data;
	(void)user;
	return (int)len;
}

/* Mirrors sender_entry: drain work, otherwise park with its 10 ms budget. */
static void* tel_wake_loop_entry(void* arg) {
	struct tel_wake_loop* loop = arg;
	for(;;) {
		if(__atomic_load_n(&loop->stop, __ATOMIC_ACQUIRE))
			return NULL;
		if(hw_telemetry_test_async_sender_drain_n(tel_wake_sink, NULL, 1)) {
			loop->drained++;
			continue;
		}
		assert(cw_tel_wake_wait(loop->wake, TEL_WAKE_TIMEOUT_MS));
	}
}

static void tel_wake_await_park(struct tel_wake_loop* loop, uint32_t parks,
	uint32_t drained) {
	thread_mutex_lock(&loop->lock);
	while(loop->parks < parks)
		thread_cond_wait(&loop->parked, &loop->lock);
	assert(loop->parks == parks && loop->drained == drained);
	thread_mutex_unlock(&loop->lock);
}

static void test_wake_pending_is_never_lost(void) {
	struct cw_tel_wake wake;
	assert(cw_tel_wake_init(&wake));
	tel_wake_virtual_timeout = true;
	tel_wake_wait_calls = tel_wake_budget_ms = 0;
	/* Coalesced pre-signals must return on pending without entering a wait. */
	cw_tel_wake_signal(&wake);
	cw_tel_wake_signal(&wake);
	assert(cw_tel_wake_wait(&wake, TEL_WAKE_TIMEOUT_MS));
	assert(!wake.pending && tel_wake_wait_calls == 0);

	/* Each idle pass requests one 10 ms wait, not the old 1 ms polling budget.
	 * This proves 100 timeout passes request 1 s of sleep regardless of how
	 * long the host leaves either test thread descheduled. */
	for(unsigned i = 0; i < 100; i++)
		assert(!cw_tel_wake_wait(&wake, TEL_WAKE_TIMEOUT_MS));
	assert(tel_wake_wait_calls == 100 && tel_wake_budget_ms == 1000);
	cw_tel_wake_signal(&wake);
	assert(cw_tel_wake_wait(&wake, TEL_WAKE_TIMEOUT_MS));
	assert(!wake.pending && tel_wake_wait_calls == 100);
	tel_wake_virtual_timeout = false;
	/* Retain an actual portable timed-wait timeout check without an elapsed
	 * upper bound or a scheduler-dependent minimum iteration rate. The LOWER
	 * bound stays (Astra final review 2): preemption only lengthens a wait, so
	 * an immediately-returning platform timeout (idle spinning) still fails. */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	assert(!cw_tel_wake_wait(&wake, TEL_WAKE_TIMEOUT_MS));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	uint64_t waited_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull
		+ (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
	assert(waited_ns >= 5ull * 1000000ull);
	cw_tel_wake_destroy(&wake);
	puts("PASS wake pending consumed without wait; 100 idle waits request 1000 ms");
}

static void test_wake_drives_the_sender_loop(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "wake", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	struct tel_wake_loop loop = {.wake = hw_telemetry_test_sender_wake()};
	assert(loop.wake);
	assert(thread_mutex_init(&loop.lock));
	assert(thread_cond_init(&loop.parked));
	/* Prior sessions may leave a pending hint; start with an empty queue and
	 * consume that hint before requiring an actual park. */
	thread_mutex_lock(&loop.wake->lock);
	loop.wake->pending = false;
	thread_mutex_unlock(&loop.wake->lock);
	tel_wake_active_loop = &loop;
	pthread_t worker;
	assert(pthread_create(&worker, NULL, tel_wake_loop_entry, &loop) == 0);
	tel_wake_await_park(&loop, 1, 0);
	assert(hw_telemetry_emit_observer("{\"event\":\"carve_attrib\",\"f\":1}"));
	tel_wake_await_park(&loop, 2, 1);
	struct hw_telemetry_heartbeat hb = {.tick = 1};
	hw_telemetry_emit_heartbeat(&hb);
	tel_wake_await_park(&loop, 3, 2);

	/* Same signal as orderly-stop sleep: the parked consumer must exit
	 * without a timeout ever rescuing a missing notification. */
	__atomic_store_n(&loop.stop, true, __ATOMIC_RELEASE);
	cw_tel_wake_signal(loop.wake);
	assert(pthread_join(worker, NULL) == 0);
	assert(loop.parks == 3 && loop.drained == 2);
	tel_wake_active_loop = NULL;
	thread_cond_destroy(&loop.parked);
	thread_mutex_destroy(&loop.lock);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS parked sender drains real observer + heartbeat and stops on signals; no timer fallback");
}
