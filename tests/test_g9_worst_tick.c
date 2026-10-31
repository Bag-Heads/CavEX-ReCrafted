/* G9 R20 Q3 worst-tick attribution probe (source/platform/g9_worst_tick.h).
 * Drives the REAL probe (thread.c + watchdog.c phase hook + the tick boost's
 * cap instant) with a fake microsecond clock and checks:
 *  - OFF: no record, no take;
 *  - the record is the window's MAX-wt tick, its every srv phase split exactly
 *    at the cap instant, nested waits/drains/checkpoint attached to THAT tick,
 *    window maxima kept independently of the retained tick;
 *  - exactly one take per closed window (once per heartbeat), every tick in
 *    exactly one window;
 *  - off-owner notes are dropped and counted, never folded in;
 *  - the observer line carries the fields verbatim. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "platform/g9_probe.h"
#include "platform/g9_worst_tick.h"
#include "platform/thread.h"
#include "platform/watchdog.h"

static uint32_t s_now = 1000;
static unsigned clock_calls;
static uint32_t fake_now(void) { clock_calls++; return s_now; }
static void adv(uint32_t us) { s_now += us; }

static void tick(uint32_t rt, uint32_t wt, uint32_t len) {
	watchdog_phase_srv(WDS_IDLE_SAMPLE);
	g9_worst_tick_begin();
	watchdog_phase_srv(WDS_WORLD_TICK);
	adv(len);
	g9_worst_tick_components(rt, wt, 7, 0);
	watchdog_phase_srv(WDS_EPILOGUE);
	g9_worst_tick_end();
	watchdog_phase_srv(WDS_IDLE_SLEEP);
}

static void* foreign_note(void* arg) {
	(void)arg;
	g9_worst_tick_wait_us(G9WT_W_IO, 777);
	g9_worst_tick_drain(false, 5, 5);
	return NULL;
}

static void off_test(void) {
	g9_worst_tick_configure(false);
	struct g9_worst_tick_rec r;
	tick(1, 2, 100);
	assert(!g9_worst_tick_enabled());
	assert(!g9_worst_tick_take(&r));
	assert(g9_worst_tick_wait_begin() == 0);
	puts("PASS off: no record, no take");
}

static void worst_tick_test(void) {
	thread_g9_lease_test_clock(fake_now);
	g9_worst_tick_test_clock(fake_now);
	thread_g9_tick_boost_configure(true, 60);
	thread_g9_tick_boost_local(true, 63);
	g9_worst_tick_configure(true);
	struct g9_worst_tick_rec r;
	char line[G9WT_LINE_CAP];
	assert(!g9_worst_tick_take(&r)); /* no tick closed yet */

	/* window 1 = {A} (the first tick end closes the first window) */
	tick(5000, 8000, 9000);
	assert(g9_worst_tick_take(&r));
	assert(r.seq == 1 && r.ticks == 1 && r.wt_us == 8000 && r.tick_us == 9000);
	assert(r.ph_pre[WDS_WORLD_TICK] == 9000 && r.gap_us == 0);
	assert(r.cap_us == UINT32_MAX && !(r.flags & G9WT_F_CAPPED));
	assert(!g9_worst_tick_take(&r)); /* once per window */

	/* window 2 = {X}, closed at the first tick end after the take */
	tick(1, 100, 200);
	/* B (worst) is the next tick */
	thread_g9_tick_boost_begin();
	watchdog_phase_srv(WDS_IDLE_SAMPLE);
	const uint32_t b_t0 = s_now;
	g9_worst_tick_begin();
	watchdog_phase_srv(WDS_WORLD_TICK);
	adv(30000);
	g9_worst_tick_drain(false, 1234, 20000);
	g9_worst_tick_drain(true, 163000, 25000);
	g9_worst_tick_ckpt(41000, true);
	adv(30000);
	g9_worst_tick_test_cap(); /* the 60 ms cap fires inside WORLD_TICK */
	adv(20000);
	g9_worst_tick_components(4000, 80000, 600, 0);
	watchdog_phase_srv(WDS_RPC_SEND);
	adv(9500);
	g9_worst_tick_wait_us(G9WT_W_SEND, 9000);
	g9_worst_tick_wait_us(G9WT_W_DISK, 123);
	g9_worst_tick_pending(37); /* ring occupancy: the tick keeps its max */
	g9_worst_tick_pending(12);
	{
		pthread_t th;
		assert(pthread_create(&th, NULL, foreign_note, NULL) == 0);
		pthread_join(th, NULL);
	}
	watchdog_phase_srv(WDS_SAVE);
	uint32_t w = g9_worst_tick_wait_begin();
	assert(w);
	adv(5000);
	g9_worst_tick_wait_end(G9WT_W_IO, w);
	g9_worst_tick_flag(G9WT_F_SWEEP);
	adv(1000);
	watchdog_phase_srv(WDS_IDLE);
	adv(300);
	watchdog_phase_srv(WDS_EPILOGUE);
	adv(500);
	g9_worst_tick_end();
	thread_g9_tick_boost_end();
	watchdog_phase_srv(WDS_IDLE_SLEEP);
	assert(g9_worst_tick_take(&r)); /* window 2 */
	assert(r.seq == 2 && r.ticks == 1 && r.wt_us == 100);
	/* B ended before that take, so it stays in window 3, which the first
	 * tick end AFTER the take (C) closes: window 3 = {B, C}. */
	tick(90000, 70000, 70000); /* C: bigger rt, smaller wt than B */
	assert(g9_worst_tick_take(&r));
	assert(r.seq == 3 && r.ticks == 2);
	assert(r.tick == 3 && r.wt_us == 80000 && r.rt_us == 4000);
	assert(r.world_time == 600 && r.wt_end_us == 80000);
	assert(r.tick_us == s_now - 70000 - b_t0);
	assert(r.tick_us == 96300);
	assert(r.cap_us == 60000);
	assert(r.ph_pre[WDS_WORLD_TICK] == 60000 && r.ph_post[WDS_WORLD_TICK] == 20000);
	assert(r.ph_pre[WDS_RPC_SEND] == 0 && r.ph_post[WDS_RPC_SEND] == 9500);
	assert(r.ph_post[WDS_SAVE] == 6000 && r.ph_post[WDS_EPILOGUE] == 500);
	assert(r.ph_pre[WDS_IDLE_SAMPLE] == 0 && r.un_us == 300 && r.gap_us == 0);
	{
		uint32_t sum = 0;
		for(unsigned k = 0; k < G9WT_PHASES; k++) sum += r.ph_pre[k] + r.ph_post[k];
		assert(sum + r.gap_us == r.tick_us); /* disjoint partition */
	}
	assert(r.drain_n == 1 && r.drain_pops == 1234 && r.drain_us == 20000);
	assert(r.trial_n == 1 && r.trial_pops == 163000 && r.trial_us == 25000);
	assert(r.ckpt_n == 1 && r.ckpt_us == 41000);
	assert(r.wait_n[G9WT_W_SEND] == 1 && r.wait_us[G9WT_W_SEND] == 9000);
	assert(r.wait_n[G9WT_W_IO] == 1 && r.wait_us[G9WT_W_IO] == 5000);
	assert(r.wait_n[G9WT_W_DISK] == 1 && r.wait_us[G9WT_W_DISK] == 123);
	assert(r.flags == (G9WT_F_COMPONENTS | G9WT_F_CKPT | G9WT_F_CKPT_BATCH
		| G9WT_F_SWEEP | G9WT_F_CAPPED | G9WT_F_FOREIGN));
	assert(r.notes_dropped == 2);
	assert(r.win_rt_max == 90000 && r.win_wt_max == 80000);
	assert(r.rpc_pend_max == 37 && r.win_rpc_pend_max == 37);
	assert(!g9_worst_tick_take(&r));
	printf("PASS max-wt tick retained: tick_us=%u wt=%u cap=%u pre/post WORLD_TICK=%u/%u\n",
		r.tick_us, r.wt_us, r.cap_us, r.ph_pre[WDS_WORLD_TICK], r.ph_post[WDS_WORLD_TICK]);

	size_t n = g9_worst_tick_format(&r, line, sizeof line);
	assert(n > 0 && n < sizeof line && line[n - 1] == '}' && strlen(line) == n);
	assert(!r.cpu_sampled && !strstr(line, "\"cpu_sample\""));
	assert(strstr(line, "{\"event\":\"g9_worst_tick\",\"v\":1,\"seq\":3,\"ticks\":2,"));
	assert(strstr(line, "\"wt_us\":80000") && strstr(line, "\"cap_us\":60000"));
	assert(strstr(line, "\"pre\":[0,0,0,0,60000,"));
	assert(strstr(line, "\"post\":[300,0,0,0,20000,"));
	assert(strstr(line, "\"trial\":[1,163000,25000]") && strstr(line, "\"ckpt\":[1,41000]"));
	assert(strstr(line, "\"io_wait\":[1,5000]") && strstr(line, "\"send_wait\":[1,9000]"));
	assert(strstr(line, "\"disk_wait\":[1,123]"));
	assert(strstr(line, "\"rpc_pend\":[37,37],\"win_rt_max\":90000,\"win_wt_max\":80000,\"dropped\":2"));
	printf("PASS line (%zu B): %s\n", n, line);

	/* an uncapped record says so */
	tick(1, 2, 3);
	assert(g9_worst_tick_take(&r) && r.cap_us == UINT32_MAX);
	n = g9_worst_tick_format(&r, line, sizeof line);
	assert(n && strstr(line, "\"cap_us\":-1"));
	assert(g9_worst_tick_format(&r, line, 64) == 0); /* truncation refused */
	puts("PASS uncapped record + truncation refusal");

	thread_g9_tick_boost_local(false, 63);
	thread_g9_tick_boost_configure(false, 60);
	g9_worst_tick_configure(false);
	g9_worst_tick_test_clock(NULL);
	thread_g9_lease_test_clock(NULL);
}

static uint32_t owner_id(void) {
	uintptr_t native = (uintptr_t)pthread_self();
	uint32_t id = (uint32_t)(native ^ (native >> 32));
	return id ? id : UINT32_MAX;
}
static unsigned raw_lines;
static unsigned raw_samples;
static bool raw_emit(const char* line) {
	assert(strstr(line, "\"event\":\"g9_cpu\""));
	/* Raw sample kind=2 lives after seq,lost in each flat recorder row. */
	const char* p = strstr(line, "\"rows\":[") + 8;
	while(*p != ']') {
		unsigned seq, lost, kind;
		assert(sscanf(p, "[%u,%u,%u", &seq, &lost, &kind) == 3);
		if(kind == 2) raw_samples++;
		p = strchr(p, ']') + 1;
		if(*p == ',') p++;
	}
	raw_lines++;
	return true;
}
static void sample(uint32_t t0, uint32_t at, bool server, uint32_t state,
	uint32_t suspend) {
	s_now = t0 + at;
	uint32_t o[10] = {server ? owner_id() : owner_id() ^ 1u, 0, 0, 0,
		state, suspend, 0, 0, 0, 0};
	g9_probe_sample(s_now, o);
}
static void row(const struct g9_worst_tick_rec* r, unsigned i,
	uint32_t elapsed, uint32_t server, uint32_t runnable, uint32_t blocked,
	uint32_t late, uint32_t missed) {
	const uint32_t want[] = {elapsed, server, runnable, blocked, late, missed};
	if(memcmp(r->cpu_sample[i], want, sizeof want)) {
		fprintf(stderr, "CPU row %u:", i);
		for(unsigned j = 0; j < G9WT_CPU_WORDS; j++)
			fprintf(stderr, " %u (want %u)", r->cpu_sample[i][j], want[j]);
		fputc('\n', stderr);
	}
	assert(!memcmp(r->cpu_sample[i], want, sizeof want));
}
static void cpu_sample_test(void) {
	g9_worst_tick_test_clock(fake_now);
	s_now = 1000000;
	const uint32_t t0 = s_now;
	/* Raw probe explicitly OFF: sampler alone must feed the worst-tick mode. */
	g9_probe_configure(false, true);
	g9_worst_tick_configure(true);
	g9_worst_tick_begin();
	g9_worst_tick_wt(true);
	g9_probe_begin(0, 0);
	sample(t0, 999, true, 2, 0); /* before deadline: ignored */
	sample(t0, 1000, true, 2, 0);
	sample(t0, 2051, false, 2, 0); /* runnable, 51us late */
	sample(t0, 5000, false, 3, 0); /* waiting, TWO missed, one observation */
	sample(t0, 6000, true, 3, 1); /* interrupted server wins over state */
	s_now = t0 + 6500; g9_probe_end(0, 6500); g9_worst_tick_random(true);
	sample(t0, 7000, true, 2, 0); /* random: tick only */
	s_now = t0 + 7500; g9_worst_tick_random(false); g9_probe_begin(0, 2);
	sample(t0, 8000, false, 2, 0);
	s_now = t0 + 8500; g9_probe_end(0, 1000);
	s_now = t0 + 9000; g9_probe_begin(0, 1); /* overdue edge belongs to old rows */
	sample(t0, 10000, false, 4, 0); /* waiting mutex */
	s_now = t0 + 10500; g9_probe_end(0, 1500); g9_probe_begin(0, 3);
	sample(t0, 11060, false, 2, 1); /* suspended RUNNING is blocked */
	sample(t0, 12000, true, 2, 0);
	s_now = t0 + 12500; g9_probe_end(0, 2000); g9_probe_begin(0, 4);
	sample(t0, 13000, false, 2, 0);
	sample(t0, 14000, false, 0, 0); /* other non-running state is blocked */
	s_now = t0 + 15500; g9_probe_end(0, 3000); g9_worst_tick_wt(false);
	sample(t0, 16000, true, 2, 0); /* entity: tick only */
	g9_worst_tick_random(true); g9_worst_tick_random(false); /* cannot reopen wt */
	s_now = t0 + 17500;
	g9_worst_tick_components(1000, 14500, 5, 0);
	g9_worst_tick_end(); /* missed IRQ-disabled tail deadline */
	struct g9_worst_tick_rec r;
	assert(g9_worst_tick_take(&r) && r.cpu_sampled);
	row(&r, G9WT_CPU_TICK, 17500, 5, 3, 4, 3, 5);
	row(&r, G9WT_CPU_WT, 14500, 3, 3, 4, 3, 4);
	row(&r, G9WT_CPU_NORMAL, 6500, 2, 1, 1, 2, 2);
	row(&r, G9WT_CPU_TRIAL, 1500, 0, 0, 1, 0, 0);
	row(&r, G9WT_CPU_SNAPSHOT, 1000, 0, 1, 0, 0, 0);
	row(&r, G9WT_CPU_SAVE, 2000, 1, 0, 1, 1, 0);
	row(&r, G9WT_CPU_RESTORE, 3000, 0, 1, 1, 0, 1);
	char line[G9WT_LINE_CAP];
	size_t n = g9_worst_tick_format(&r, line, sizeof line);
	assert(n && n < sizeof line && strlen(line) == n && line[n-1] == '}');
	assert(strstr(line, "\"cpu_sample\":[[17500,5,3,4,3,5],[14500,3,3,4,3,4],"
		"[6500,2,1,1,2,2],[1500,0,0,1,0,0],[1000,0,1,0,0,0],"
		"[2000,1,0,1,1,0],[3000,0,1,1,0,1]]}"));
	assert(g9_worst_tick_format(&r, line, n) == 0);
	assert(g9_worst_tick_format(&r, line, n + 1) == n);
	printf("PASS cpu_sample (%zu B): %s\n", n, line);
	/* Post-stop samples and hooks cannot mutate the published record. */
	sample(t0, 19000, true, 2, 0);
	g9_probe_begin(0, 0); g9_probe_end(0, 0); g9_worst_tick_wt(true);
	assert(!g9_worst_tick_take(&r) && !g9_probe_pump(raw_emit) && raw_lines == 0);

	/* Early return with stage/wt open: tick end must close BOTH and disarm. */
	s_now = t0 + 20000; g9_worst_tick_begin();
	g9_worst_tick_wt(true); g9_probe_begin(0, 3);
	sample(t0 + 20000, 1000, false, 2, 0);
	s_now = t0 + 22500; g9_worst_tick_end();
	assert(g9_worst_tick_take(&r));
	row(&r, G9WT_CPU_TICK, 2500, 0, 1, 0, 0, 1);
	row(&r, G9WT_CPU_WT, 2500, 0, 1, 0, 0, 1);
	row(&r, G9WT_CPU_SAVE, 2500, 0, 1, 0, 0, 1);
	/* Repeated stages accumulate, and their rows belong to the retained
	 * MAX-wt tick even when a smaller tick closes the next window. */
	tick(0, 1, 100); /* ready window: keep it pending while B runs */
	uint32_t b0 = s_now;
	g9_worst_tick_begin(); g9_worst_tick_wt(true); g9_probe_begin(0, 0);
	sample(b0, 1000, true, 2, 0);
	s_now = b0 + 1500; g9_probe_end(0, 1500);
	s_now = b0 + 2000; g9_probe_begin(0, 0);
	sample(b0, 3050, true, 2, 0); /* exactly 50us: NOT late */
	s_now = b0 + 3500; g9_probe_end(0, 1500); g9_worst_tick_wt(false);
	g9_worst_tick_components(0, 3500, 6, 0); g9_worst_tick_end();
	assert(g9_worst_tick_take(&r) && r.wt_us == 1);
	tick(0, 2, 100);
	assert(g9_worst_tick_take(&r) && r.wt_us == 3500 && r.ticks == 2);
	row(&r, G9WT_CPU_TICK, 3500, 2, 0, 0, 0, 1);
	row(&r, G9WT_CPU_NORMAL, 3000, 2, 0, 0, 0, 0);

	/* Both raw probe + sampler on still emit ONLY the worst-tick field. */
	g9_probe_configure(true, true);
	assert(!g9_probe_enabled());
	tick(1, 2, 100);
	assert(g9_worst_tick_take(&r) && r.cpu_sampled);
	assert(!g9_probe_pump(raw_emit) && raw_lines == 0);
	/* Bounds: phase partition over a uint32 tick, uint32 existing fields,
	 * occupancy/missed <= ceil(UINT32_MAX/1000). No field is capped to fit. */
	memset(&r, 0xff, sizeof r);
	r.flags = 127; r.cap_us = INT32_MAX;
	for(unsigned i = 0; i < G9WT_PHASES; i++) {
		r.ph_pre[i] = 100000000;
		r.ph_post[i] = i < 19 ? 100000000 : 10000000;
	}
	for(unsigned i = 0; i < G9WT_CPU_ROWS; i++)
		for(unsigned j = 1; j < G9WT_CPU_WORDS; j++)
			r.cpu_sample[i][j] = UINT32_MAX / 1000u + 1u;
	n = g9_worst_tick_format(&r, line, sizeof line);
	assert(n && n < sizeof line && line[n-1] == '}' && strlen(line) == n);
	printf("PASS wide counter line fits MAIN buffer: %zu/%u B\n", n, G9WT_LINE_CAP);
	g9_probe_configure(false, false);
	g9_worst_tick_configure(false);
	g9_worst_tick_test_clock(NULL);
}

static void cpu_off_test(void) {
	g9_worst_tick_test_clock(fake_now);
	/* Worst tick OFF, sampler ON: hooks remain inert, no clock calls. */
	g9_worst_tick_configure(false); g9_probe_configure(false, true);
	unsigned calls = clock_calls;
	g9_worst_tick_begin(); g9_worst_tick_wt(true);
	g9_worst_tick_random(true); g9_worst_tick_random(false);
	g9_probe_begin(0, 0); sample(s_now, 1000, true, 2, 0); g9_probe_end(0, 0);
	g9_worst_tick_wt(false); g9_worst_tick_end();
	assert(clock_calls == calls);
	struct g9_worst_tick_rec r;
	assert(!g9_worst_tick_take(&r) && !g9_probe_pump(raw_emit));
	/* Worst tick ON, sampler OFF: original record has no CPU field/counters. */
	g9_worst_tick_configure(true); g9_probe_configure(false, false);
	g9_worst_tick_begin(); calls = clock_calls;
	g9_worst_tick_wt(true); g9_probe_begin(0, 4);
	g9_worst_tick_random(true); g9_worst_tick_random(false);
	sample(s_now, 1000, true, 2, 0); g9_probe_end(0, 0); g9_worst_tick_wt(false);
	assert(clock_calls == calls);
	g9_worst_tick_end(); assert(g9_worst_tick_take(&r) && !r.cpu_sampled);
	uint32_t zero[G9WT_CPU_ROWS][G9WT_CPU_WORDS] = {{0}};
	assert(!memcmp(zero, r.cpu_sample, sizeof zero));
	char line[G9WT_LINE_CAP];
	assert(g9_worst_tick_format(&r, line, sizeof line) && !strstr(line, "cpu_sample"));
	/* Existing raw mode still works when worst tick is OFF (also sampler=0). */
	g9_worst_tick_configure(false); g9_probe_configure(true, false);
	assert(g9_probe_enabled()); g9_probe_begin(0, 0); g9_probe_end(0, 1);
	while(g9_probe_pump(raw_emit)) {}
	assert(raw_lines > 0);
	/* Native legacy 1-kHz core still accepts raw samples when worst is OFF. */
	g9_probe_configure(true, true); g9_probe_begin(0, 0);
	struct timespec ts;
	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	uint64_t tb = (uint64_t)ts.tv_sec * 1000000u + (uint32_t)ts.tv_nsec / 1000u;
	uint32_t observation[10] = {owner_id(), 0, 0, 0, 2, 0, 0, 0, 0, 0};
	g9_probe_sample(tb + 5000, observation); g9_probe_end(0, 5000);
	while(g9_probe_pump(raw_emit)) {}
	assert(raw_samples == 1);
	g9_probe_configure(false, false); g9_worst_tick_test_clock(NULL);
	puts("PASS CPU off modes inert; legacy raw probe retained");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	off_test();
	worst_tick_test();
	cpu_sample_test();
	cpu_off_test();
	puts("test_g9_worst_tick: ALL PASS");
	return 0;
}
