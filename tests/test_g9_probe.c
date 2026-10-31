#include "platform/g9_probe.h"
#include "platform/g9_capture.h"
#include "platform/thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint32_t rows[G9_PROBE_CAPACITY + 64][G9_PROBE_WORDS];
static unsigned count;
static uint32_t loss;
static bool refuse;
static char rejected[2304];
static FILE* tape;
static uint64_t now_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint32_t)t.tv_nsec / 1000u;
}
static bool emit(const char* line) {
	if(refuse) {
		assert(strlen(line) < sizeof rejected);
		memcpy(rejected, line, strlen(line) + 1);
		return false;
	}
	if(rejected[0]) { assert(strcmp(rejected, line) == 0); rejected[0] = 0; }
	if(tape) fprintf(tape, "%s\n", line);
	const char* p = strstr(line, "\"lost\":");
	assert(p); loss = (uint32_t)strtoul(p + 7, NULL, 10);
	p = strstr(line, "\"rows\":[") + 8;
	while(*p != ']') {
		assert(*p++ == '[' || p[-1] == ' ');
		if(p[-1] == ' ') { assert(*p++ == '['); }
		for(unsigned k = 0; k < G9_PROBE_WORDS; k++) {
			char* end;
			rows[count][k] = (uint32_t)strtoul(p, &end, 10);
			assert(end != p); p = end;
			if(k + 1 < G9_PROBE_WORDS) assert(*p++ == ',');
		}
		assert(*p++ == ']'); count++;
		if(*p == ',') { p++; while(*p == ' ') p++; }
	}
	return true;
}
static void pump(void) { while(g9_probe_pump(emit)) {} }
static void* exhaust_and_exit(void* pool) {
	void* token;
	g9_probe_role(2);
	assert(!tchannel_receive(pool, &token, false));
	return NULL;
}

static struct thread_channel pace_ready, pace_done;
static void* pacing_drain(void* unused) {
	(void)unused;
	thread_drain_wait_begin();
	assert(!thread_main_pacing_checkpoint()); /* never sleep the drain owner */
	assert(tchannel_send(&pace_ready, &pace_ready, true));
	void* token;
	assert(tchannel_receive(&pace_done, &token, true));
	struct thread_drain_waits waits;
	thread_drain_wait_end(&waits);
	return NULL;
}
static void test_main_pacing(void) {
	assert(!thread_main_pacing_enabled());
	assert(!thread_main_pacing_checkpoint());
	thread_main_pacing_configure(true, 500, 1, 2);
	uint32_t stats[THREAD_PACING_WORDS];
	assert(thread_main_pacing_snapshot(stats));
	assert(stats[0] == 1 && stats[1] == 500 && stats[2] == 1 && stats[3] == 2);
	usleep(1000);
	assert(!thread_main_pacing_checkpoint()); /* no drain */
	assert(tchannel_init(&pace_ready, 1) && tchannel_init(&pace_done, 1));
	struct thread worker;
	assert(thread_create(&worker, pacing_drain, NULL, 63));
	void* token;
	assert(tchannel_receive(&pace_ready, &token, true));
	thread_main_pacing_frame();
	for(unsigned i = 0; i < 2; i++) {
		usleep(1000);
		uint64_t start = now_us();
		assert(thread_main_pacing_checkpoint());
		assert(now_us() - start >= 500); /* real timed sleep */
	}
	usleep(1000);
	assert(!thread_main_pacing_checkpoint()); /* per-frame cap */
	assert(!thread_main_pacing_checkpoint()); /* counted once per frame */
	assert(thread_main_pacing_snapshot(stats));
	assert(stats[5] == 4 && stats[6] == 1 && stats[9] == 2);
	assert(stats[10] == 1 && stats[11] == 2 && stats[12] == 2);
	assert(stats[16 + THREAD_PACING_OTHER] == 2);
	assert(stats[13] >= 1000 && stats[14] >= 500 && stats[15] == 0);
	thread_main_pacing_frame();
	usleep(1000);
	assert(thread_main_pacing_checkpoint()); /* next frame recovers */
	assert(tchannel_send(&pace_done, &pace_done, true));
	thread_join(&worker);
	usleep(1000);
	assert(!thread_main_pacing_checkpoint()); /* completion clears signal */
	assert(thread_main_pacing_snapshot(stats));
	assert(stats[10] == 1 && stats[11] == 3 && stats[12] == 3 && stats[6] == 2);
	tchannel_close(&pace_ready); tchannel_close(&pace_done);
	thread_main_pacing_configure(true, -1, -1, -1);
	assert(thread_main_pacing_enabled()); /* malformed tuning stays bounded */
	assert(!thread_main_pacing_checkpoint());
	thread_main_pacing_configure(false, 2000, 1, 8);
	assert(!thread_main_pacing_checkpoint());
}
static unsigned capture_batches, capture_windows, capture_rows, capture_seals;
static unsigned capture_tick, capture_offset, capture_total;
static bool capture_refuse;
static char capture_retry[2304];
static bool capture_emit(const char* line) {
	if(capture_refuse) {
		assert(strlen(line) < sizeof capture_retry);
		memcpy(capture_retry, line, strlen(line) + 1);
		return false;
	}
	if(capture_retry[0]) { assert(!strcmp(capture_retry, line)); capture_retry[0] = 0; }
	if(tape) fprintf(tape, "%s\n", line);
	if(strstr(line, "\"event\":\"g9_capture_seal\"")) { capture_seals++; return true; }
	unsigned tick, offset, total;
	assert(sscanf(strstr(line, "\"tick\":"), "\"tick\":%u,\"offset\":%u,\"total\":%u", &tick, &offset, &total) == 3);
	if(tick != capture_tick) {
		assert(capture_offset == capture_total);
		capture_windows++; capture_tick = tick; capture_offset = 0; capture_total = total;
	}
	assert(offset == capture_offset && total == capture_total);
	const char* p = strstr(line, "\"rows\":[") + 8;
	unsigned n = 0;
	while(*p != ']') {
		assert(*p++ == '[');
		for(unsigned k = 0; k < G9_CAPTURE_WORDS; k++) {
			char* end; (void)strtoul(p, &end, 10); assert(end != p); p = end;
			if(k + 1 < G9_CAPTURE_WORDS) assert(*p++ == ',');
		}
		assert(*p++ == ']'); n++; if(*p == ',') p++;
	}
	assert(n && n <= 8); capture_batches++; capture_offset += n; capture_rows += n;
	return true;
}
static void capture_pump(void) { while(g9_capture_pump(capture_emit)) {} }
static uint64_t capture_test_now;
static unsigned capture_clock_calls;
static uint64_t capture_clock(void) { capture_clock_calls++; return capture_test_now; }
static void test_capture(void) {
	g9_capture_test_clock(capture_clock);
	assert(!g9_capture_enabled());
	g9_capture_tick_begin(); g9_capture_pop(); g9_capture_tick_end();
	g9_capture_arm(); g9_capture_finish();
	assert(!g9_capture_pending() && !g9_capture_pump(capture_emit));
	assert(capture_clock_calls == 0);
	assert(g9_capture_configure(true));
	g9_capture_tick_begin(); g9_capture_tick_end(); /* not armed */
	g9_capture_arm();
	for(unsigned i = 1; i <= 10; i++) {
		capture_test_now = i * 100000;
		g9_capture_main(10, true, 0); /* initial region is preserved at admission */
		g9_capture_tick_begin();
		assert(!g9_capture_pump(capture_emit));
		g9_capture_random(true, 0, 42, i); capture_test_now += i * 1000;
		g9_capture_random(false, 0, 42, i);
		g9_capture_drain_begin(0, 42, i, i == 5 ? 1 : 0, 1);
		g9_capture_pop(); g9_capture_drain_final(i == 4 ? 100000 : i, 0);
		g9_capture_restore_return(); g9_capture_record_enter();
		g9_capture_record_end(11-i); g9_capture_record_return();
		uint32_t o[10] = {12, 2, 63, 63, 2, 0, 0, 0, 10, 0x80001234};
		g9_capture_sample(capture_test_now, o);
		g9_capture_tick_components(i, 11-i);
		g9_capture_main(10, false, 1); g9_capture_tick_end();
	}
	g9_capture_finish(); assert(g9_capture_pending());
	capture_refuse = true; assert(!g9_capture_pump(capture_emit));
	capture_refuse = false; capture_pump();
	assert(!g9_capture_pending() && capture_windows == 8 && capture_seals == 1);
	assert(g9_capture_configure(true)); g9_capture_arm();
	unsigned before = capture_rows;
	capture_tick = capture_offset = capture_total = 0;
	g9_capture_tick_begin();
	for(unsigned i = 0; i < G9_CAPTURE_ROWS + 100; i++) g9_capture_main(11, i % 2 == 0, i);
	g9_capture_tick_end();
	g9_capture_tick_begin(); g9_capture_tick_end(); /* exhausted: no new admission */
	g9_capture_finish(); capture_pump();
	assert(capture_rows-before == G9_CAPTURE_ROWS && capture_seals == 2);
	assert(g9_capture_configure(false));
	assert(!g9_capture_pump(capture_emit));
	g9_capture_test_clock(NULL);
	puts("G9 capture: OFF inert, reserved worst random/drain + full-size/checkpoint, full tick, retry, saturation closes admission PASS");
}
int main(int argc, char** argv) {
	test_main_pacing();
	if(argc == 2) { tape = fopen(argv[1], "w"); assert(tape); }
	uint32_t o[10] = {0x1111, 4, 65, 48, 2, 0, 0, 0, 6, 0x80001234};
	g9_probe_begin(0, 0); g9_probe_sample(now_us(), o); g9_probe_end(7, 1);
	assert(!g9_probe_pump(emit)); /* default dormant */
	g9_probe_configure(true, false);
	g9_probe_mesh(0, 4, false); g9_probe_mesh(0, 1, false);
	g9_probe_begin(1, 0);
	assert(!g9_probe_pump(emit)); /* pump cannot touch a live drain */
	g9_probe_sample(now_us() + 10000, o);
	g9_probe_mesh(0, 2, false); g9_probe_mesh(0, 3, true);
	g9_probe_end(123, 500);
	refuse = true; assert(!g9_probe_pump(emit));
	refuse = false; pump(); /* failed send retains the identical batch */
	assert(count == 3 && rows[1][2] == 1 && rows[2][2] == 3);
	assert(rows[1][7] == 0 && rows[1][10] == 1); /* off; queued */
	assert(rows[2][6] == 123 && rows[2][8] == 0); /* pops; no samples */
	assert(rows[2][13] == 1 && rows[2][16] == 1); /* completed; stale */
	g9_probe_configure(true, true);
	const unsigned begin_index = count + 1; /* role then begin */
	g9_probe_begin(0, 1);
	g9_probe_sample(0, o); /* early callbacks must not create observations */
	usleep(2500);
	uint64_t first = now_us();
	g9_probe_sample(first, o); g9_probe_sample(first, o); /* no catch-up duplicate */
	usleep(5000);
	o[4] = 4; o[6] = 0x1234;
	uint64_t second = now_us(); g9_probe_sample(second, o);
	g9_probe_end(12530, (uint32_t)(second - first + 2500));
	pump();
	assert(count == begin_index + 4);
	uint32_t* begin = rows[begin_index];
	uint64_t start = ((uint64_t)begin[4] << 32) | begin[5];
	uint32_t* a = rows[begin_index + 1];
	uint32_t* b = rows[begin_index + 2];
	uint32_t* end = rows[begin_index + 3];
	assert(a[2] == 2 && b[2] == 2 && end[2] == 3);
	assert(a[6] == o[0] && a[8] == 65 && a[9] == 48 && a[15] == o[9]);
	assert(a[17] == (first - start - 1000) / 1000);
	assert(b[17] == (second - start) / 1000 - (first - start) / 1000 - 1);
	assert(end[8] == 2 && end[10] >= a[17] + b[17]);
	struct thread_channel pool;
	assert(tchannel_init(&pool, 2)); g9_probe_rpc_pool(&pool);
	unsigned token = 1; void* acquired;
	unsigned rpc_start = count;
	g9_probe_rpc(1, NULL);
	assert(tchannel_send(&pool, &token, true));
	assert(tchannel_receive(&pool, &acquired, false) && acquired == &token);
	pump();
	assert(count == rpc_start + 3);
	for(unsigned i = 0; i < 3; i++) {
		assert(rows[rpc_start + i][2] == 4);
		assert(rows[rpc_start + i][7] == i + 1);
		assert(rows[rpc_start + i][6] == rows[rpc_start][6]);
	}
	assert(rows[rpc_start + 1][9] == rows[rpc_start + 2][9]);
	g9_probe_rpc(1, NULL); g9_probe_rpc(4, NULL); pump();
	struct thread departed;
	assert(thread_create(&departed, exhaust_and_exit, &pool, 63));
	thread_join(&departed); pump();
	assert(rows[count - 1][2] == 4 && rows[count - 1][7] == 4);
	unsigned joined_count = count;
	assert(tchannel_send(&pool, &token, true)); pump();
	assert(count == joined_count); /* no stale/freed waiter read after join */
	g9_probe_rpc_pool(NULL); tchannel_close(&pool);
	unsigned before = count;
	for(unsigned i = 0; i < G9_PROBE_CAPACITY + 7; i++) g9_probe_role(3);
	pump();
	assert(count - before == G9_PROBE_CAPACITY && loss == 7);
	/* Record a real paced sleep, then observe the same foreign owner with OFF. */
	thread_main_pacing_configure(true, 500, 1, 2);
	assert(tchannel_init(&pace_ready, 1) && tchannel_init(&pace_done, 1));
	struct thread pace_worker;
	assert(thread_create(&pace_worker, pacing_drain, NULL, 63));
	assert(tchannel_receive(&pace_ready, &acquired, true));
	usleep(1000);
	assert(thread_main_pacing_checkpoint_at(THREAD_PACING_RPC));
	unsigned sleep_index = count; pump();
	assert(count == sleep_index + 1 && rows[sleep_index][2] == 6);
	assert(rows[sleep_index][6] == THREAD_PACING_RPC);
	assert(rows[sleep_index][7] && rows[sleep_index][8] == 1);
	assert(rows[sleep_index][9] >= 500 && rows[sleep_index][10] == 1);
	thread_main_pacing_configure(false, 2000, 1, 8);
	assert(!thread_main_pacing_checkpoint());
	uint32_t pace_stats[THREAD_PACING_WORDS];
	assert(thread_main_pacing_snapshot(pace_stats));
	assert(pace_stats[0] == 0 && pace_stats[5] == 1 && pace_stats[11] == 0);
	assert(tchannel_send(&pace_done, &pace_done, true)); thread_join(&pace_worker);
	tchannel_close(&pace_ready); tchannel_close(&pace_done);
	thread_drain_wait_begin(); assert(!thread_main_pacing_checkpoint());
	struct thread_drain_waits dw; thread_drain_wait_end(&dw);
	assert(thread_main_pacing_snapshot(pace_stats) && pace_stats[7] == 1);
	unsigned trace_start = count;
	g9_probe_main_frame(true); pump(); assert(count == trace_start); /* default OFF */
	g9_probe_main_configure(true);
	g9_probe_main_phase(4); /* outside frame: no unmatched phase boundary */
	g9_probe_main_frame(true);
	unsigned world = 0;
	g9_probe_light_push(&world, 1); usleep(1000);
	g9_probe_light_push(&world, 2);
	g9_probe_light_pop(&world, 2);
	assert(g9_probe_light_age(&world, 1, true) >= 500);
	g9_probe_main_mark(4, true, 0, 2, g9_probe_light_age(&world, 1, true));
	g9_probe_main_mark(4, false, 256, 1, g9_probe_light_age(&world, 1, true));
	g9_probe_inbox(0, true); usleep(1000);
	uint32_t backlog, age; g9_probe_inbox_stats(&backlog, &age);
	assert(backlog == 1 && age >= 500);
	g9_probe_inbox(0, false); g9_probe_inbox_stats(&backlog, &age);
	assert(backlog == 0 && age == 0);
	g9_probe_light_clear(&world);
	assert(g9_probe_light_age(&world, 0, false) == 0);
	g9_probe_main_phase(10);
	g9_probe_main_frame(false); pump();
	assert(count == trace_start + 8);
	assert(rows[trace_start][2] == 7 && rows[trace_start][6] == 1);
	assert(rows[trace_start+3][9] == 256);
	assert(g9_probe_light_age(&world, 8193, false) == UINT32_MAX);
	g9_probe_light_push(&world, 8193);
	g9_probe_role(1); pump(); assert(loss == 8);
	trace_start++;
	g9_probe_main_configure(false);
	g9_probe_main_frame(true); pump(); assert(count == trace_start + 8);
	g9_probe_configure(false, false);
	assert(!thread_main_pacing_snapshot(pace_stats));
	g9_probe_begin(0, 0); g9_probe_end(1, 2); assert(!g9_probe_pump(emit));
	test_capture();
	if(tape) fclose(tape);
	puts("G9 sampler: dormant/on-off, timestamps/priority/PC, missed callbacks, ring overflow, retry, mesh, RPC ordering PASS");
	return 0;
}
