/* Included by test_hw_telemetry.c: tests the production FIFO and byte sender
 * with a socket call held on another thread until main finishes publishing. */
#include "../source/platform/hw_tel_ring.h"
#include "../source/platform/hw_tel_trace.h"
#include <pthread.h>
#include <time.h>
static struct cw_tel_ring tel_test_ring;
static pthread_mutex_t tel_stall_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tel_stall_cond = PTHREAD_COND_INITIALIZER;
static bool tel_entered, tel_release;
static char tel_order[256];
static size_t tel_order_size;
static int tel_stalled_send(const char* data, size_t len, void* user) {
	(void)user;
	pthread_mutex_lock(&tel_stall_lock);
	tel_entered = true;
	pthread_cond_signal(&tel_stall_cond);
	while(!tel_release) pthread_cond_wait(&tel_stall_cond, &tel_stall_lock);
	pthread_mutex_unlock(&tel_stall_lock);
	/* Positive short writes must resume at the exact byte, not start over. */
	size_t n = len > 3 ? 3 : len;
	assert(tel_order_size+n <= sizeof(tel_order));
	memcpy(tel_order+tel_order_size, data, n); tel_order_size += n;
	return (int)n;
}
static void* tel_test_consumer(void* unused) {
	(void)unused;
	const char* line;
	while((line = cw_tel_peek(&tel_test_ring))) {
		assert(hw_telemetry_test_send_line_with(line, tel_stalled_send, NULL));
		cw_tel_pop(&tel_test_ring);
	}
	return NULL;
}
static void test_bounded_sender(void) {
	hw_telemetry_test_preconnect_reset();
	/* O20: the preconnect buffer is a packed arena of up to 64 lines (was
	 * eight one-line slots); the 65th line still evicts and is counted. */
	for(unsigned i = 0; i < 64; i++)
		assert(!hw_telemetry_test_preconnect_store("buffered"));
	assert(hw_telemetry_test_preconnect_store("overwrites-counted"));
	hw_telemetry_test_preconnect_reset();
	memset(&tel_test_ring, 0, sizeof(tel_test_ring));
	tel_entered = tel_release = false; tel_order_size = 0;
	assert(cw_tel_publish(&tel_test_ring, "first"));
	pthread_t worker;
	assert(pthread_create(&worker, NULL, tel_test_consumer, NULL) == 0);
	pthread_mutex_lock(&tel_stall_lock);
	while(!tel_entered) pthread_cond_wait(&tel_stall_cond, &tel_stall_lock);
	pthread_mutex_unlock(&tel_stall_lock);
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	anchor_alloc_calls = 0; anchor_alloc_guard = true;
	assert(cw_tel_publish(&tel_test_ring, "second"));
	assert(cw_tel_publish(&tel_test_ring, "third"));
	assert(cw_tel_publish(&tel_test_ring, "fourth"));
	for(unsigned i = 0; i < 10000; i++) assert(!cw_tel_publish(&tel_test_ring, "drop"));
	anchor_alloc_guard = false;
	assert(anchor_alloc_calls == 0);
	clock_gettime(CLOCK_MONOTONIC, &end);
	double ms = (end.tv_sec-start.tv_sec)*1000.0 + (end.tv_nsec-start.tv_nsec)/1e6;
	assert(ms < 250); /* 10,003 publishes while socket is still blocked */
	assert(tel_test_ring.drops == 10000);
	pthread_mutex_lock(&tel_stall_lock);
	assert(tel_entered && !tel_release);
	tel_release = true; pthread_cond_signal(&tel_stall_cond);
	pthread_mutex_unlock(&tel_stall_lock);
	assert(pthread_join(worker, NULL) == 0);
	const char expected[] = "first\nsecond\nthird\nfourth\n";
	assert(tel_order_size == sizeof(expected)-1);
	assert(memcmp(tel_order, expected, sizeof(expected)-1) == 0);
	assert(!cw_tel_peek(&tel_test_ring));
	/* Counter wrap, producer contention, and capacity boundary. */
	tel_test_ring.head = tel_test_ring.tail = UINT32_MAX;
	assert(cw_tel_publish(&tel_test_ring, "wrap"));
	assert(strcmp(cw_tel_peek(&tel_test_ring), "wrap") == 0);
	cw_tel_pop(&tel_test_ring); assert(!cw_tel_peek(&tel_test_ring));
	tel_test_ring.producer = 1;
	assert(!cw_tel_publish(&tel_test_ring, "busy"));
	tel_test_ring.producer = 0;
	static char oversized[CW_TEL_LINE+1];
	memset(oversized, 'x', sizeof(oversized)); oversized[CW_TEL_LINE] = 0;
	assert(!cw_tel_publish(&tel_test_ring, oversized));
	oversized[CW_TEL_LINE-1] = 0;
	assert(cw_tel_publish(&tel_test_ring, oversized)); cw_tel_pop(&tel_test_ring);
	assert(tel_test_ring.drops == 10002);
	assert(cw_tel_publish(&tel_test_ring, "drain-before-stop"));
	cw_tel_close(&tel_test_ring);
	assert(!cw_tel_publish(&tel_test_ring, "after-stop"));
	assert(!cw_tel_drained(&tel_test_ring));
	assert(strcmp(cw_tel_peek(&tel_test_ring), "drain-before-stop") == 0);
	cw_tel_pop(&tel_test_ring); assert(cw_tel_drained(&tel_test_ring));
	tel_test_ring.producer = 1; assert(!cw_tel_drained(&tel_test_ring));
	tel_test_ring.producer = 0;
	printf("PASS stalled sender: 10003 publishes %.3f ms, 10000 drops, FIFO/partials/wrap exact\n", ms);
}

#include "../source/platform/hw_tel_stop.h"
static struct cw_tel_stop tel_stop_state;
static pthread_t tel_stop_sender;
static bool tel_cancel_entered, tel_cancel_release;
static bool tel_expect_drained;
static unsigned tel_cancel_calls, tel_join_calls;
static struct hw_telemetry_send_fail_state tel_stop_fail;

static uint64_t tel_now_ms(void* unused) {
	(void)unused;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}
static void tel_sleep_ms(void* unused) {
	(void)unused;
	struct timespec delay = {.tv_nsec = 1000000};
	nanosleep(&delay, NULL);
}
static int tel_block_until_shutdown(const char* data, size_t len, void* unused) {
	(void)unused;
	pthread_mutex_lock(&tel_stall_lock);
	if(!tel_entered) {
		assert(len >= 3);
		memcpy(tel_order, data, 3); tel_order_size = 3;
		tel_entered = true;
		pthread_cond_broadcast(&tel_stall_cond);
	}
	while(!tel_release) pthread_cond_wait(&tel_stall_cond, &tel_stall_lock);
	pthread_mutex_unlock(&tel_stall_lock);
	// Simulate SDK hidden progress followed by peer/shutdown failure.
	return -ECONNRESET;
}
static void* tel_stop_consumer(void* unused) {
	(void)unused;
	assert(!hw_telemetry_test_send_line_with_state("abcdef", tel_block_until_shutdown,
		NULL, &tel_stop_fail, NULL));
	assert(!hw_telemetry_test_send_line_with_state("abcdef", tel_block_until_shutdown,
		NULL, &tel_stop_fail, NULL));
	// Same descriptor-lifetime barrier as the production sender before close.
	while(!__atomic_load_n(&tel_stop_state.cancelled, __ATOMIC_ACQUIRE))
		tel_sleep_ms(NULL);
	return NULL;
}
static void tel_cancel_socket(void* unused) {
	(void)unused;
	if(tel_expect_drained) assert(cw_tel_drained(&tel_test_ring));
	pthread_mutex_lock(&tel_stall_lock);
	tel_cancel_calls++;
	tel_cancel_entered = true;
	pthread_cond_broadcast(&tel_stall_cond);
	while(!tel_cancel_release) pthread_cond_wait(&tel_stall_cond, &tel_stall_lock);
	tel_release = true;
	pthread_cond_broadcast(&tel_stall_cond);
	pthread_mutex_unlock(&tel_stall_lock);
}
static void tel_join_sender(void* unused) {
	(void)unused;
	assert(__atomic_load_n(&tel_stop_state.cancelled, __ATOMIC_ACQUIRE));
	assert(pthread_join(tel_stop_sender, NULL) == 0);
	tel_join_calls++;
}
static void* tel_stop_worker(void* unused) {
	(void)unused;
	while(!__atomic_load_n(&tel_stop_state.requested, __ATOMIC_ACQUIRE))
		tel_sleep_ms(NULL);
	cw_tel_stop_run(&tel_stop_state, tel_cancel_socket, tel_join_sender, NULL);
	return NULL;
}
static bool tel_ring_drained(void* unused) {
	(void)unused;
	return cw_tel_drained(&tel_test_ring);
}
static bool tel_never_drained(void* unused) {
	(void)unused;
	return false;
}
static void test_stop_drains_seal(void) {
	memset(&tel_stop_state, 0, sizeof(tel_stop_state));
	memset(&tel_test_ring, 0, sizeof(tel_test_ring));
	tel_entered = tel_cancel_entered = false;
	tel_release = tel_cancel_release = tel_expect_drained = true;
	tel_cancel_calls = tel_join_calls = 0; tel_order_size = 0;
	assert(cw_tel_publish(&tel_test_ring, "{\"event\":\"heartbeat\"}"));
	assert(cw_tel_publish(&tel_test_ring, "{\"event\":\"tps_summary\",\"count\":7}"));
	assert(cw_tel_publish(&tel_test_ring, "{\"event\":\"tps_end\",\"records\":7}"));
	cw_tel_close(&tel_test_ring);
	assert(pthread_create(&tel_stop_sender, NULL, tel_test_consumer, NULL) == 0);
	pthread_t worker;
	assert(pthread_create(&worker, NULL, tel_stop_worker, NULL) == 0);
	assert(cw_tel_stop_orderly(&tel_stop_state, tel_ring_drained,
		tel_now_ms, tel_sleep_ms, NULL));
	assert(pthread_join(worker, NULL) == 0);
	const char expected[] = "{\"event\":\"heartbeat\"}\n"
		"{\"event\":\"tps_summary\",\"count\":7}\n"
		"{\"event\":\"tps_end\",\"records\":7}\n";
	assert(tel_order_size == sizeof(expected)-1);
	assert(!memcmp(tel_order, expected, sizeof(expected)-1));
	assert(tel_cancel_calls == 1 && tel_join_calls == 1);
	tel_expect_drained = false;
	puts("PASS orderly stop: heartbeat, final summary and seal drained byte-exact before cancellation");
}
static void test_stop_blocked_sender(void) {
	for(unsigned stuck_cancel = 0; stuck_cancel < 2; stuck_cancel++) {
		hw_telemetry_test_trace_reset();
		memset(&tel_stop_state, 0, sizeof(tel_stop_state));
		memset(&tel_stop_fail, 0, sizeof(tel_stop_fail));
		tel_entered = tel_release = tel_cancel_entered = false;
		tel_cancel_release = !stuck_cancel;
		tel_cancel_calls = tel_join_calls = 0; tel_order_size = 0;
		assert(pthread_create(&tel_stop_sender, NULL, tel_stop_consumer, NULL) == 0);
		pthread_t worker;
		assert(pthread_create(&worker, NULL, tel_stop_worker, NULL) == 0);
		pthread_mutex_lock(&tel_stall_lock);
		while(!tel_entered) pthread_cond_wait(&tel_stall_cond, &tel_stall_lock);
		pthread_mutex_unlock(&tel_stall_lock);
		uint64_t start = tel_now_ms(NULL);
		bool joined = cw_tel_stop_orderly(&tel_stop_state, tel_never_drained,
			tel_now_ms, tel_sleep_ms, NULL);
		uint64_t elapsed = tel_now_ms(NULL) - start;
		assert(elapsed >= CW_TEL_DRAIN_MS);
		assert(joined == !stuck_cancel);
		if(!stuck_cancel) assert(elapsed < CW_TEL_STOP_MS);
		else {
			assert(elapsed >= CW_TEL_STOP_MS && elapsed < CW_TEL_STOP_MS + 100);
			// Timeout leaves worker state intact. A late shutdown may finish safely.
			pthread_mutex_lock(&tel_stall_lock);
			assert(!tel_release);
			tel_cancel_release = true;
			pthread_cond_broadcast(&tel_stall_cond);
			pthread_mutex_unlock(&tel_stall_lock);
			assert(cw_tel_stop_wait(&tel_stop_state, tel_now_ms, tel_sleep_ms, NULL));
		}
		assert(pthread_join(worker, NULL) == 0);
		assert(tel_cancel_calls == 1 && tel_join_calls == 1);
		assert(tel_order_size == 3 && !memcmp(tel_order, "abc", 3));
		assert(tel_stop_fail.fail_count == 1 && tel_stop_fail.quiesced);
		const struct cw_tel_trace* trace = hw_telemetry_test_trace();
		assert(trace->reason == CW_TEL_SEND && trace->result == -ECONNRESET);
		printf("PASS stop blocked sender: cancellation_stuck=%u elapsed=%llu ms; exact abc prefix, no retry, joined\n",
			stuck_cancel, (unsigned long long)elapsed);
	}
}
