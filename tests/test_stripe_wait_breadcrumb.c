#include "platform/thread.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static struct thread_mutex stripe;
static uint32_t pause_event, pause_ready, pause_release;
static uint32_t holder_ready, observed;
static enum thread_stripe_tag holder_tag;
static struct thread_stripe_breadcrumb during;

static void pause_hook(unsigned event, void* unused) {
	(void)unused;
	if(event != pause_event) return;
	__atomic_store_n(&pause_ready, 1, __ATOMIC_RELEASE);
	while(!__atomic_load_n(&pause_release, __ATOMIC_ACQUIRE)) usleep(1000);
}

static void* holder(void* unused) {
	(void)unused;
	thread_stripe_phase(holder_tag, 2);
	thread_stripe_lock(&stripe);
	__atomic_store_n(&holder_ready, 1, __ATOMIC_RELEASE);
	usleep(400000);
	thread_stripe_unlock(&stripe);
	return NULL;
}

static void* observer(void* unused) {
	(void)unused;
	for(unsigned i = 0; i < 600; i++) {
		if(thread_stripe_breadcrumb_get(&during) && during.active) {
			observed = 1;
			break;
		}
		usleep(1000);
	}
	return NULL;
}

static void window(unsigned event, uint32_t expected_state, uint32_t uncertain) {
	pause_event = event;
	pause_ready = pause_release = holder_ready = 0;
	holder_tag = THREAD_STRIPE_MESHER;
	thread_stripe_test_hook_set(pause_hook, NULL);
	pthread_t h;
	assert(pthread_create(&h, NULL, holder, NULL) == 0);
	while(!__atomic_load_n(&pause_ready, __ATOMIC_ACQUIRE)) usleep(1000);
	struct thread_stripe_breadcrumb snap;
	thread_stripe_test_snapshot(3, &snap);
	assert(snap.uncertain == uncertain);
	if(expected_state != THREAD_STRIPE_IDLE) {
		assert(snap.holder_id != 0);
		assert(snap.holder_tag == THREAD_STRIPE_MESHER);
		assert(snap.holder_state == expected_state);
	}
	__atomic_store_n(&pause_release, 1, __ATOMIC_RELEASE);
	pthread_join(h, NULL);
	thread_stripe_test_hook_set(NULL, NULL);
}

static void completed_wait(enum thread_stripe_tag tag) {
	holder_tag = tag;
	holder_ready = observed = 0;
	pthread_t h, o;
	assert(pthread_create(&h, NULL, holder, NULL) == 0);
	while(!__atomic_load_n(&holder_ready, __ATOMIC_ACQUIRE)) usleep(1000);
	assert(pthread_create(&o, NULL, observer, NULL) == 0);
	thread_stripe_lock(&stripe);
	thread_stripe_unlock(&stripe);
	pthread_join(h, NULL);
	pthread_join(o, NULL);
	assert(observed);
	assert(during.stripe == 3 && during.holder_tag == tag);
	assert(during.holder_id != 0 && during.holder_phase == 2);
	assert(during.wait_us >= THREAD_STRIPE_WAIT_THRESHOLD_US);
	assert(during.active == 1 && !during.uncertain);
	struct thread_stripe_breadcrumb after;
	assert(thread_stripe_breadcrumb_get(&after));
	assert(after.active == 0 && after.stripe == 3);
	assert(after.holder_tag == tag && !after.uncertain);
}

/* 0. Role identity under thread-id reuse. A thread holds ONE role; a thread
 * id that last claimed SERVER and now claims MESHER must be filed as MESHER.
 * This is what id recycling produces (glibc hands a joined thread's pthread_t
 * to the next pthread_create; libogc reuses freed LWP handles): the stale
 * SERVER slot aliased the new mesher and, because the role scan visits SERVER
 * before MESHER, its locks were recorded under SERVER. Winpthreads never
 * recycles a pthread_t, so only Linux saw it -- here one thread re-roles
 * itself, which pins the same aliasing on every platform. */
static struct thread_stripe_breadcrumb re_role_snap;
static void* re_role(void* unused) {
	(void)unused;
	thread_stripe_phase(THREAD_STRIPE_SERVER, 4);
	thread_stripe_phase(THREAD_STRIPE_MESHER, 2);
	thread_stripe_lock(&stripe);
	thread_stripe_test_snapshot(3, &re_role_snap);
	thread_stripe_unlock(&stripe);
	return NULL;
}
static void role_reuse(void) {
	re_role_snap = (struct thread_stripe_breadcrumb){0};
	pthread_t t;
	assert(pthread_create(&t, NULL, re_role, NULL) == 0);
	pthread_join(t, NULL);
	assert(re_role_snap.holder_tag == THREAD_STRIPE_MESHER);
	assert(re_role_snap.holder_state == THREAD_STRIPE_HELD);
	assert(re_role_snap.holder_phase == 2 && !re_role_snap.uncertain);
}

/* ------------------------------------------------------------------------
 * Forced interleavings (Astra review 6). A GATE pauses ONE chosen thread at
 * the Nth occurrence of one hook event; every other thread runs through.
 * x86 cannot show a missing fence, so these pin the protocol (retry, give-up,
 * re-validation, window states); the fences themselves are verified in the
 * devkitPPC objdump.
 * ---------------------------------------------------------------------- */
static pthread_t gate_thread;
static uint32_t gate_armed, gate_event, gate_nth, gate_count;
static uint32_t gate_ready, gate_release;

static void gate_hook(unsigned event, void* unused) {
	(void)unused;
	if(!__atomic_load_n(&gate_armed, __ATOMIC_ACQUIRE)) return;
	if(!pthread_equal(pthread_self(), gate_thread) || event != gate_event) return;
	if(++gate_count != gate_nth) return;
	__atomic_store_n(&gate_ready, 1, __ATOMIC_RELEASE);
	while(!__atomic_load_n(&gate_release, __ATOMIC_ACQUIRE)) usleep(1000);
}

static void gate_arm(pthread_t who, unsigned event, uint32_t nth) {
	gate_thread = who;
	gate_event = event;
	gate_nth = nth;
	gate_count = 0;
	gate_ready = gate_release = 0;
	thread_stripe_test_hook_set(gate_hook, NULL);
	__atomic_store_n(&gate_armed, 1, __ATOMIC_RELEASE);
}

static void gate_disarm(void) {
	__atomic_store_n(&gate_armed, 0, __ATOMIC_RELEASE);
	thread_stripe_test_hook_set(NULL, NULL);
}

static void wait_flag(uint32_t* flag) {
	while(!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) usleep(1000);
}

/* A mesher-tag holder driven by flags: lock, report, hold until told. */
static uint32_t h_locked, h_unlock;
static void* flag_holder(void* unused) {
	(void)unused;
	thread_stripe_phase(THREAD_STRIPE_MESHER, 2);
	thread_stripe_lock(&stripe);
	__atomic_store_n(&h_locked, 1, __ATOMIC_RELEASE);
	wait_flag(&h_unlock);
	thread_stripe_unlock(&stripe);
	return NULL;
}

/* The reader starts, then is gated. */
static uint32_t r_go;
static struct thread_stripe_breadcrumb r_snap;
static uint32_t r_ret;
static void* snapshot_reader(void* unused) {
	(void)unused;
	wait_flag(&r_go);
	thread_stripe_test_snapshot(3, &r_snap);
	return NULL;
}

/* 1. A role-record read races a writer: the reader is gated after its first
 * sequence load (READ_BEGIN) or after its payload loads (READ_VALIDATE) on the
 * MESHER record (the 4th record a snapshot reads: OTHER, MAIN, SERVER,
 * MESHER), the mesher then acquires, and the reader resumes. The first
 * attempt must fail validation and the retry must report the NEW state as
 * certain -- never the stale IDLE with uncertain=0. */
static void reader_vs_writer(unsigned event) {
	h_locked = h_unlock = r_go = 0;
	r_snap = (struct thread_stripe_breadcrumb){0};
	pthread_t r, h;
	assert(pthread_create(&r, NULL, snapshot_reader, NULL) == 0);
	gate_arm(r, event, 4);
	__atomic_store_n(&r_go, 1, __ATOMIC_RELEASE);
	wait_flag(&gate_ready);
	assert(pthread_create(&h, NULL, flag_holder, NULL) == 0);
	wait_flag(&h_locked);
	__atomic_store_n(&gate_release, 1, __ATOMIC_RELEASE);
	pthread_join(r, NULL);
	gate_disarm();
	assert(r_snap.holder_tag == THREAD_STRIPE_MESHER);
	assert(r_snap.holder_state == THREAD_STRIPE_HELD);
	assert(r_snap.holder_id != 0 && !r_snap.uncertain);
	__atomic_store_n(&h_unlock, 1, __ATOMIC_RELEASE);
	pthread_join(h, NULL);
}

/* 2. The active snapshot races the main thread finishing its wait. The main
 * thread (tag MAIN) blocks on the stripe the mesher holds; after the wait
 * passes the threshold an observer takes a breadcrumb and is gated at
 * `event`; the mesher then unlocks, the main thread acquires (clearing the
 * active-wait record and publishing the completed one) and releases the
 * observer.
 *   ACTIVE_HOLDER (after the holder scan): the triple no longer describes one
 *     moment -> active=1 but uncertain=1.
 *   ACTIVE_SLOT (inside the active-record read): the read fails validation,
 *     the retry sees no active wait -> the COMPLETED record, active=0,
 *     certain -- never the old stripe with a new start time. */
static uint32_t o_go;
static struct thread_stripe_breadcrumb o_snap;
static void* crumb_observer(void* unused) {
	(void)unused;
	wait_flag(&o_go);
	usleep(350000); /* past THREAD_STRIPE_WAIT_THRESHOLD_US */
	r_ret = thread_stripe_breadcrumb_get(&o_snap);
	return NULL;
}
static void* release_when_gated(void* unused) {
	(void)unused;
	wait_flag(&gate_ready);
	__atomic_store_n(&h_unlock, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void active_vs_completion(unsigned event) {
	h_locked = h_unlock = o_go = 0;
	o_snap = (struct thread_stripe_breadcrumb){0};
	pthread_t h, o;
	assert(pthread_create(&h, NULL, flag_holder, NULL) == 0);
	wait_flag(&h_locked);
	assert(pthread_create(&o, NULL, crumb_observer, NULL) == 0);
	gate_arm(o, event, 1);
	__atomic_store_n(&o_go, 1, __ATOMIC_RELEASE);
	/* Unlock the holder only once the observer is gated. */
	pthread_t releaser;
	assert(pthread_create(&releaser, NULL, release_when_gated, NULL) == 0);
	thread_stripe_lock(&stripe); /* the main thread waits here */
	__atomic_store_n(&gate_release, 1, __ATOMIC_RELEASE);
	pthread_join(o, NULL);
	thread_stripe_unlock(&stripe);
	pthread_join(releaser, NULL);
	pthread_join(h, NULL);
	gate_disarm();
	assert(r_ret);
	assert(o_snap.stripe == 3 && o_snap.holder_tag == THREAD_STRIPE_MESHER);
	assert(o_snap.wait_us >= THREAD_STRIPE_WAIT_THRESHOLD_US);
	if(event == THREAD_STRIPE_TEST_ACTIVE_HOLDER) {
		assert(o_snap.active == 1 && o_snap.uncertain == 1);
	} else {
		assert(o_snap.active == 0 && o_snap.uncertain == 0);
	}
}

/* 3. The post-unlock/pre-idle window: the mesher is gated right after its
 * mutex unlock, before its record goes IDLE. The snapshot must say
 * RELEASING (uncertain); once the main thread takes the free mutex in that
 * same window, the main thread's HELD record wins, certain. */
static void post_unlock_window(void) {
	h_locked = h_unlock = 0;
	pthread_t h;
	assert(pthread_create(&h, NULL, flag_holder, NULL) == 0);
	wait_flag(&h_locked);
	gate_arm(h, THREAD_STRIPE_TEST_UNLOCKED, 1);
	__atomic_store_n(&h_unlock, 1, __ATOMIC_RELEASE);
	wait_flag(&gate_ready);
	struct thread_stripe_breadcrumb snap;
	thread_stripe_test_snapshot(3, &snap);
	assert(snap.holder_tag == THREAD_STRIPE_MESHER);
	assert(snap.holder_state == THREAD_STRIPE_RELEASING && snap.uncertain);
	thread_stripe_lock(&stripe); /* free already: the mutex is unlocked */
	thread_stripe_test_snapshot(3, &snap);
	assert(snap.holder_tag == THREAD_STRIPE_MAIN);
	assert(snap.holder_state == THREAD_STRIPE_HELD && !snap.uncertain);
	thread_stripe_unlock(&stripe);
	__atomic_store_n(&gate_release, 1, __ATOMIC_RELEASE);
	pthread_join(h, NULL);
	gate_disarm();
	thread_stripe_test_snapshot(3, &snap);
	assert(snap.holder_state == THREAD_STRIPE_IDLE || snap.holder_tag == 0);
}

int main(void) {
	assert(thread_mutex_init(&stripe));
	thread_stripe_register(&stripe, 3);
	thread_stripe_phase(THREAD_STRIPE_MAIN, 6);
	struct thread_stripe_breadcrumb fast;
	thread_stripe_lock(&stripe);
	thread_stripe_unlock(&stripe);
	assert(!thread_stripe_breadcrumb_get(&fast));
	window(THREAD_STRIPE_TEST_ACQUIRED, THREAD_STRIPE_ACQUIRING, 1);
	window(THREAD_STRIPE_TEST_RELEASING, THREAD_STRIPE_RELEASING, 1);
	window(THREAD_STRIPE_TEST_ODD, THREAD_STRIPE_IDLE, 1);
	completed_wait(THREAD_STRIPE_MESHER);
	completed_wait(THREAD_STRIPE_SERVER);
	role_reuse();
	puts("  thread id re-claimed by another role: filed under the new role PASS");
	reader_vs_writer(THREAD_STRIPE_TEST_READ_BEGIN);
	reader_vs_writer(THREAD_STRIPE_TEST_READ_VALIDATE);
	puts("  role-record read vs writer: retried, new state certain PASS");
	active_vs_completion(THREAD_STRIPE_TEST_ACTIVE_HOLDER);
	puts("  active snapshot vs wait completion after the holder scan: uncertain PASS");
	active_vs_completion(THREAD_STRIPE_TEST_ACTIVE_SLOT);
	puts("  active record read vs wait completion: completed record, certain PASS");
	post_unlock_window();
	puts("  post-unlock/pre-idle window: releasing uncertain, then main held PASS");
	thread_mutex_destroy(&stripe);
	puts("stripe wait breadcrumb PASS");
	return 0;
}
