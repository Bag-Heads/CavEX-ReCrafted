#include <assert.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/game/autopilot_keepup.h"
#include "../source/game/autopilot_unstick.h"
#include "../source/game/hw_run.h"
#include "../source/game/mp_reconnect_soak_core.h"
#include "../source/game/split_portal_quit_core.h"
#include "../source/platform/hw_telemetry.h"
#include "../source/platform/thread.h"
/* Instrument only this TU's inline consumer wait; production publication
 * still signals the real sender object from hw_telemetry.c. */
static bool tel_wake_test_timedwait(struct thread_cond*, struct thread_mutex*,
	uint32_t);
#define thread_cond_timedwait tel_wake_test_timedwait
#include "../source/platform/hw_tel_wake.h"
#undef thread_cond_timedwait
#include "fixtures/ref_heartbeat_de7dd71_tick200.h"
#include "support/test_fixture_dir.h"

#ifdef _WIN32
#include <direct.h>
#define HWTEL_MKDIR(p) _mkdir(p)
#define HWTEL_CHDIR(p) _chdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define HWTEL_MKDIR(p) mkdir(p, 0777)
#define HWTEL_CHDIR(p) chdir(p)
#endif

/* hwtel-flake-0927: every file this test touches by a RELATIVE name -- the PC
 * crash-mailbox paths (HW_TELEMETRY_*_REPORT_*PATH: "hang_report.txt",
 * "fbguard_report.txt", their _fallback and _<seq> variants) and the
 * args-file fixture -- used to land in ctest's working directory (the shared
 * build tree). A leftover report from an interrupted run, or a second
 * instance of this test running in the same tree, made the mailbox tests'
 * assert(!file_exists(...)) abort (ctest: 0xc0000409 after ~1 s; the G5
 * Windows suite). Run inside a per-process sandbox directory instead, wiped
 * on entry and removed on a passing exit. */
static char s_sandbox[128];

static void enter_sandbox(void) {
	test_fixture_dir("test_hw_telemetry_tmp", s_sandbox, sizeof(s_sandbox));
	HWTEL_MKDIR(s_sandbox);
	int rc = HWTEL_CHDIR(s_sandbox);
	assert(rc == 0);
	(void)rc;
}

static void leave_sandbox(void) {
	int rc = HWTEL_CHDIR("..");
	assert(rc == 0);
	(void)rc;
	test_fixture_dir_cleanup(s_sandbox);
}
extern void hw_telemetry_split_guest_eye(float, int, int);

static uint32_t test_rpcdrop;
uint32_t svin_dropped_count(void) { return test_rpcdrop; }

/* Linker-wrapped allocation mocks: while armed, any heap request is counted
 * and rejected. Outside the measurement fixture, preserve normal libc use. */
static bool anchor_alloc_guard;
static size_t anchor_alloc_calls, anchor_alloc_bytes;
void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* ptr, size_t size);

void* __wrap_malloc(size_t size) {
	if(!anchor_alloc_guard)
		return __real_malloc(size);
	anchor_alloc_calls++;
	anchor_alloc_bytes += size;
	return NULL;
}

void* __wrap_calloc(size_t count, size_t size) {
	if(!anchor_alloc_guard)
		return __real_calloc(count, size);
	anchor_alloc_calls++;
	anchor_alloc_bytes += count * size;
	return NULL;
}

void* __wrap_realloc(void* ptr, size_t size) {
	if(!anchor_alloc_guard)
		return __real_realloc(ptr, size);
	anchor_alloc_calls++;
	anchor_alloc_bytes += size;
	return NULL;
}

struct anchor_sample_sink {
	uint32_t samples;
	bool reject;
};

#include "platform/hw_tel_ring.h"
static struct cw_tel_ring observer_ring;
/* Narrow fixture: prove the production lifecycle asks the observer to seal
 * while telemetry still admits records, without linking the full collector. */
static unsigned observer_close_checks;
void tps_observer_close(bool complete) {
	assert(!complete);
	if(observer_close_checks) {
		assert(!hw_telemetry_quiesced());
		assert(hw_telemetry_emit_observer("{\"event\":\"tps_end\",\"stream_valid\":false}"));
		observer_close_checks++;
	}
}

static bool observer_packet_sink(const char* line, void* user) {
	unsigned* calls = user;
	assert(strlen(line) < 3072);
	assert(cw_tel_publish(&observer_ring, line));
	assert(strcmp(cw_tel_peek(&observer_ring), line) == 0);
	cw_tel_pop(&observer_ring);
	assert(strstr(line, "\"run_id\":\"observer\\\"escape\""));
	(*calls)++;
	return true;
}

static void test_observer_packet(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "observer\"escape", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	unsigned calls = 0;
	hw_telemetry_test_set_sink(observer_packet_sink, &calls);
	assert(hw_telemetry_emit_observer("{\"event\":\"tps_tick\"}"));
	assert(calls == 1);
	char census[2305];
	const char* prefix = "{\"event\":\"carve_census\",\"pad\":\"";
	memset(census, 'x', sizeof(census));
	memcpy(census, prefix, strlen(prefix));
	census[2301] = '"'; census[2302] = '}'; census[2303] = 0;
	assert(hw_telemetry_emit_observer(census));
	assert(calls == 2); /* full 2303-byte observer input, including escaped run ID */
	census[2301] = 'x'; census[2302] = '"'; census[2303] = '}'; census[2304] = 0;
	assert(!hw_telemetry_emit_observer(census));
	assert(!hw_telemetry_emit_observer("unterminated"));
	hw_telemetry_quiesce();
	assert(!hw_telemetry_emit_observer("{\"event\":\"tps_tick\"}"));
	hw_telemetry_test_set_sink(NULL, NULL);
	hw_telemetry_shutdown();
}

/* SPLIT-SPILL: overlay fallback + carve-spill attribution record. */
static char overlay_spill_wire[3072];
static bool overlay_spill_sink(const char* line, void* user) {
	unsigned* calls = user;
	assert(strlen(line) < sizeof overlay_spill_wire);
	strcpy(overlay_spill_wire, line);
	(*calls)++;
	return true;
}
static void test_overlay_spill_record(void) {
	char buf[HW_TELEMETRY_OVERLAY_SPILL_LINE_SIZE];
	/* Emitted values, each in its own slot (distinct values catch swaps). */
	struct hw_overlay_spill s = {.tick = 7, .fallback_live = 3,
		.fallback_total = 41, .spill = 28, .last_spill_bytes = 82176};
	size_t n = hw_telemetry_format_overlay_spill(buf, sizeof buf, &s);
	const char* want = "{\"event\":\"overlay_spill\",\"v\":1,\"tick\":7,"
		"\"fallback_live\":3,\"fallback_total\":41,\"spill\":28,"
		"\"mem2_last_spill_bytes\":82176}";
	assert(n == strlen(want) && !strcmp(buf, want));
	/* Maximum width: every field saturated must fit the buffer, and the
	 * buffer itself must fit the 2,303-byte observer input cap. */
	struct hw_overlay_spill wide = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
		UINT32_MAX, UINT32_MAX};
	n = hw_telemetry_format_overlay_spill(buf, sizeof buf, &wide);
	fprintf(stderr, "max overlay_spill bytes: %zu / %u\n", n,
		HW_TELEMETRY_OVERLAY_SPILL_LINE_SIZE);
	assert(n == 158 && n < sizeof buf && strlen(buf) == n && buf[n - 1] == '}');
	assert(strstr(buf, "\"mem2_last_spill_bytes\":4294967295}"));
	/* One byte short of the widest record truncates to an explicit 0. */
	assert(hw_telemetry_format_overlay_spill(buf, 158, &wide) == 0 && buf[0] == 0);
	assert(hw_telemetry_format_overlay_spill(NULL, sizeof buf, &wide) == 0);
	assert(hw_telemetry_format_overlay_spill(buf, sizeof buf, NULL) == 0);
	/* The widest record is admitted by the real observer path with a run ID. */
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39018, .run_id = "overlay-spill-run", .profile = "split_play_8m"};
	assert(hw_telemetry_init(&cfg));
	unsigned calls = 0;
	hw_telemetry_test_set_sink(overlay_spill_sink, &calls);
	n = hw_telemetry_format_overlay_spill(buf, sizeof buf, &wide);
	assert(hw_telemetry_emit_observer_retry(buf));
	assert(calls == 1);
	assert(!strncmp(overlay_spill_wire, buf, n - 1));
	assert(strstr(overlay_spill_wire, ",\"run_id\":\"overlay-spill-run\"}"));
	hw_telemetry_test_set_sink(NULL, NULL);
	hw_telemetry_shutdown();
	/* Change-driven cadence: all-zero (healthy) state is never due; any
	 * counter change is; a tick-only change is not. */
	struct hw_overlay_spill sent = {0}, cur = {.tick = 99};
	assert(!hw_telemetry_overlay_spill_due(&cur, &sent));
	cur.fallback_live = 1; assert(hw_telemetry_overlay_spill_due(&cur, &sent));
	sent = cur; cur.tick++; assert(!hw_telemetry_overlay_spill_due(&cur, &sent));
	cur.fallback_total = 5; assert(hw_telemetry_overlay_spill_due(&cur, &sent));
	sent = cur; cur.spill = 1; assert(hw_telemetry_overlay_spill_due(&cur, &sent));
	sent = cur; cur.last_spill_bytes = 82176;
	assert(hw_telemetry_overlay_spill_due(&cur, &sent));
	sent = cur; assert(!hw_telemetry_overlay_spill_due(&cur, &sent));
	assert(!hw_telemetry_overlay_spill_due(NULL, &sent));
	puts("PASS overlay_spill record: values, 158-byte maximum width, observer admission, change-driven cadence");
}

struct async_wire_capture {
	char bytes[32768];
	size_t size;
};

static int async_wire_send(const char* data, size_t len, void* user) {
	struct async_wire_capture* capture = user;
	assert(capture->size + len < sizeof(capture->bytes));
	memcpy(capture->bytes + capture->size, data, len);
	capture->size += len;
	capture->bytes[capture->size] = '\0';
	return (int)len;
}

/* lead-1010/23: real tapes w_g9a/w_g9b end at heartbeat tick 2 before
 * its FIFO guard context. Exercise the production queue/drain, including
 * replacement and a full observer FIFO; a separate printf model misses it. */
static void test_g9_guard_heartbeat_transport(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "pull",
		.pull = true, .port = 39017, .run_id = "g9_send",
		.profile = "perf_stress_5m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	for(unsigned i = 0; i < 3; i++)
		assert(hw_telemetry_emit_observer("{\"event\":\"noise\"}"));
	struct hw_telemetry_heartbeat hb = {.tick = 1, .perf_guard_present = true,
		.player_health = 160,
		.death_present = true, .death_record = {1, 0, 0, 0, 0},
		.perf_guard_words = {2, 1, 1, 0, 0, 160}};
	hb.perf_guard_words[46] = 1; /* loading exclusion */
	hw_telemetry_emit_heartbeat(&hb);
	hb.tick = 2; /* supersede the whole pair, never just its heartbeat */
	hw_telemetry_emit_heartbeat(&hb);
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain_n(async_wire_send, &capture, 1) == 1);
	assert(strncmp(capture.bytes, "{\"event\":\"perf_survival_guard\",", 30) == 0);
	assert(strstr(capture.bytes, "\"heartbeat_tick\":2,"));
	char* heartbeat = strchr(capture.bytes, '\n');
	assert(heartbeat && !strncmp(heartbeat + 1, "{\"event\":\"heartbeat\",", 21));
	assert(strstr(heartbeat, "\"tick\":2,"));
	assert(strstr(heartbeat, "\"g1\":1"));
	assert(!strstr(capture.bytes, "\"heartbeat_tick\":1,"));
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 3);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS G9 guard precedes matching heartbeat under replacement/full FIFO");
}

struct g9_negative_send {
	unsigned calls;
	size_t queued;
	size_t fail_after;
	int failure;
};
static int g9_hidden_prefix_send(const char* data, size_t len, void* user) {
	(void)data;
	struct g9_negative_send* f = user;
	f->calls++;
	assert(len <= 1024);
	if(f->queued >= f->fail_after) {
		/* IOS may queue a prefix and return only the negative rc. */
		f->queued += len > 1 ? 1 : 0;
		return f->failure;
	}
	f->queued += len;
	return (int)len;
}
static void test_g9_negative_send_never_retries(void) {
	/* w_g9a mailbox is older B_hb tick94: 4445 B, queued=3072.
	 * w_g9b mailbox is w_g9a tick2: 201 B guard, queued=0.
	 * -128 is Wii ENOTCONN; separately model the brief's -EAGAIN trap. */
	static char line[4446];
	for(unsigned case_id = 0; case_id < 4; case_id++) {
		size_t length = case_id & 1 ? 201 : 4445;
		memset(line, 'x', length);
		line[length] = 0;
		struct g9_negative_send f = {.fail_after = case_id & 1 ? 0 : 3072,
			.failure = case_id < 2 ? -ENOTCONN : -EAGAIN};
		struct hw_telemetry_send_fail_state st;
		hw_telemetry_send_fail_state_reset(&st);
		bool close_socket;
		assert(!hw_telemetry_test_send_line_with_state(line,
			g9_hidden_prefix_send, &f, &st, &close_socket));
		assert(f.calls == (case_id & 1 ? 1u : 4u));
		assert(st.fail_count == 1 && st.last_errno == f.failure && st.resync);
		assert(st.quiesced == (case_id < 2));
		assert(close_socket == (case_id < 2));
	}
	puts("PASS G9 4445/201 B partial+negative sends consumed once; no retry");
}

static void test_g9_pair_fallback_and_failure(bool print_wire) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "pull",
		.pull = true, .port = 39017, .run_id = "g9_send",
		.profile = "perf_stress_5m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	struct hw_telemetry_heartbeat hb = {.tick = 3, .perf_guard_present = true,
		.player_health = 160, .death_present = true,
		.death_record = {1, 0, 0, 0, 0},
		.perf_guard_words = {2, 1, 1, 0, 0, 160}};
	hb.perf_guard_words[46] = 1;
	hw_telemetry_emit_heartbeat(&hb);
	assert(hw_telemetry_test_async_sender_peek()); /* consumer owns first pair */
	hb.tick = 4;
	hw_telemetry_emit_heartbeat(&hb); /* fallback to FIFO while lane is owned */
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 2);
	char* first = strstr(capture.bytes, "\"heartbeat_tick\":3,");
	char* second = strstr(capture.bytes, "\"heartbeat_tick\":4,");
	assert(first && second && first < second);
	assert(first < strstr(capture.bytes, "\"tick\":3,"));
	assert(second < strstr(capture.bytes, "\"tick\":4,"));
	if(print_wire) fputs(capture.bytes, stdout);
	hw_telemetry_test_async_sender_begin();
	hb.tick = 5;
	hw_telemetry_emit_heartbeat(&hb);
	struct g9_negative_send f = {.failure = -EAGAIN};
	assert(hw_telemetry_test_async_sender_drain(g9_hidden_prefix_send, &f) == 0);
	assert(f.calls == 1); /* failed guard must suppress heartbeat, never retry */
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 0);
	hb.tick = 6;
	hw_telemetry_emit_heartbeat(&hb);
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS G9 FIFO fallback retains pair; failed context suppresses heartbeat");
}

static bool g9_service_sink(const char* line, void* user) {
	unsigned* calls = user;
	assert(strstr(line, "\"event\":\"service_observer\""));
	(*calls)++;
	return true;
}
static void test_g9_service_uses_normal_transport(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "pull",
		.pull = true, .port = 39017, .run_id = "normal_service"};
	assert(hw_telemetry_init(&cfg));
	unsigned calls = 0;
	hw_telemetry_test_set_sink(g9_service_sink, &calls);
	hw_telemetry_test_set_send_lock_busy(true); /* exception try-lock is refused */
	assert(hw_telemetry_emit_service_observer("{\"event\":\"service_observer\",\"v\":2}"));
	assert(calls == 1 && hw_telemetry_test_send_lock_blocks() == 0);
	hw_telemetry_test_set_send_lock_busy(false);
	hw_telemetry_test_set_sink(NULL, NULL);
	hw_telemetry_shutdown();
	puts("PASS G9 service observer uses normal transport, not exception admission");
}

static void test_watchdog_start_never_takes_sender_wake(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "watchdog_probe", .profile = "far_idle_2m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	struct cw_tel_wake* wake = hw_telemetry_test_sender_wake();
	assert(wake);
	thread_mutex_lock(&wake->lock);
	/* The watchdog publication must return even with the wake mutex held. */
	assert(hw_telemetry_emit_watchdog_start(
		"{\"event\":\"watchdog_start\",\"main\":20,\"srv\":1}"));
	thread_mutex_unlock(&wake->lock);
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
	assert(strstr(capture.bytes, "\"event\":\"watchdog_start\""));
	assert(strstr(capture.bytes, "\"run_id\":\"watchdog_probe\""));
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
}

/* O22 (real-Wii HW1-HW3 vehicle smokes): the vehicle driver is a LOSSLESS
 * retrying producer -- on a refused emit it keeps the same bytes/seq and
 * resends next frame (vehicle_smoke_emit). Each refusal used to increment the
 * ring's drop counter, i.e. tel_drop, and host_vehicle_smoke_8m's collector
 * fails on any nonzero tel_drop (wii_telemetry_server.py:7132): every
 * hardware vehicle run died at the first heartbeat after its first burst.
 * PC has no ring (synchronous emit), which is why PC always passed. A retried
 * refusal must not count as a drop; it is counted as deferred, and every
 * line still reaches the wire, in order. */
static void test_retry_producer_refusal_is_not_a_drop(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "o22", .profile = "host_vehicle_smoke_8m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	const uint32_t drops0 = hw_telemetry_test_async_sender_drops();
	const uint32_t defer0 = hw_telemetry_observer_deferred_count();
	char line[64];
	/* the observer allowance is 3 FIFO slots (one stays for the heartbeat) */
	for(int i = 0; i < 3; i++) {
		snprintf(line, sizeof line, "{\"event\":\"vehicle_smoke.x\",\"seq\":%d}", i + 1);
		assert(hw_telemetry_emit_observer_retry(line));
	}
	snprintf(line, sizeof line, "{\"event\":\"vehicle_smoke.x\",\"seq\":%d}", 4);
	assert(!hw_telemetry_emit_observer_retry(line) && "ring full: refused");
	assert(!hw_telemetry_emit_observer_retry(line) && "still full: refused");
	assert(hw_telemetry_test_async_sender_drops() == drops0
		   && "a retried refusal is NOT a lost record (tel_drop unchanged)");
	assert(hw_telemetry_observer_deferred_count() == defer0 + 2
		   && "each refusal is counted as deferred");
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 3);
	assert(hw_telemetry_emit_observer_retry(line) && "the retry is admitted");
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
	const char* p = capture.bytes;
	for(int i = 1; i <= 4; i++) {
		char want[32];
		snprintf(want, sizeof want, "\"seq\":%d", i);
		const char* q = strstr(p, want);
		assert(q && "every line reaches the wire, in order");
		p = q;
	}
	/* the ordinary (non-retrying) observer path still counts its drops */
	for(int i = 0; i < 3; i++)
		assert(hw_telemetry_emit_observer("{\"event\":\"diag\"}"));
	assert(!hw_telemetry_emit_observer("{\"event\":\"diag\"}"));
	assert(hw_telemetry_test_async_sender_drops() == drops0 + 1);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS O22 retry producer: refusal deferred, not dropped; lossless, in order");
}

/* integ-1011/i_piston2/split-offer-piston-wire.ndjson: 27 delivered
 * region_stat rows + 71 refusals by heartbeat tick 1 (f939bd2_10.11).
 * Exercise the real save-path producer against the real blocked sender. */
static void test_save_path_burst(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "pull",
		.pull = true, .port = 39017, .run_id = "piston_burst",
		.profile = "split_offer_piston_2m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	uint32_t stats[3] = {0};
	unsigned cache[3] = {1, 0, 533};
	for(unsigned i = 0; i < 98; i++) {
		stats[2] += 25; cache[1]++;
		hw_telemetry_save_path_note("region_stat", 25, stats, cache);
	}
	assert(hw_telemetry_test_async_sender_drops() == 0
		&& "save-path create burst must retain evidence without tel_drop");
	/* Other diagnostics have already used the three observer slots. */
	for(unsigned i = 0; i < 3; i++)
		assert(hw_telemetry_emit_observer("{\"event\":\"diag\"}"));
	assert(!hw_telemetry_save_path_pump(UINT32_MAX - 100u));
	assert(!hw_telemetry_save_path_pump(UINT32_MAX - 90u));
	assert(hw_telemetry_observer_deferred_count() == 2);
	assert(hw_telemetry_test_async_sender_drops() == 0);
	/* New producer updates may not change the refused snapshot. */
	stats[2] += 100; cache[1]++;
	hw_telemetry_save_path_note("region_stat", 100, stats, cache);
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 3);
	assert(!hw_telemetry_save_path_pump(UINT32_MAX - 80u));
	const char* retained = hw_telemetry_test_async_sender_peek();
	assert(retained && strstr(retained, "\"operations\":98,"));
	assert(strstr(retained, "\"span_total_us\":2450,"));
	assert(strstr(retained, "\"span_max_us\":25,"));
	assert(strstr(retained, "\"cache_hits\":98,"));
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
	assert(!hw_telemetry_save_path_pump(168)); /* 249 ms across uint32 wrap */
	assert(!hw_telemetry_test_async_sender_peek());
	assert(hw_telemetry_save_path_pump(169));
	retained = hw_telemetry_test_async_sender_peek();
	assert(retained && strstr(retained, "\"operations\":99,"));
	assert(strstr(retained, "\"span_total_us\":2550,"));
	assert(strstr(retained, "\"span_max_us\":100,"));
	assert(strstr(retained, "\"cache_hits\":99,"));
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
	/* All marker classes survive as distinct cumulative summaries. */
	const char* markers[] = {"load_begin", "level_load_end", "level_save_end",
		"level_save_fail", "save_region", "load_region"};
	for(unsigned i = 0; i < 6; i++)
		hw_telemetry_save_path_note(markers[i], 10 + i, stats, cache);
	hw_telemetry_save_path_finish();
	hw_telemetry_save_path_note("region_stat", 500, stats, cache);
	for(unsigned i = 0; i < 6; i++) {
		assert(hw_telemetry_save_path_pump(419 + 250*i) == (i == 5));
		retained = hw_telemetry_test_async_sender_peek();
		assert(retained && strstr(retained, markers[i]));
		assert(strstr(retained, "\"operations\":1,"));
		if(i == 5) hw_telemetry_emit_profile_done("ok");
		assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == (i == 5 ? 2 : 1));
	}
	const char* final_summary = strstr(capture.bytes, "\"marker\":\"load_region\"");
	const char* completion = strstr(capture.bytes, "\"event\":\"profile_done\"");
	assert(final_summary && completion && final_summary < completion);
	assert(hw_telemetry_save_path_pump(3000));
	assert(!hw_telemetry_test_async_sender_peek());
	assert(hw_telemetry_test_async_sender_drops() == 0);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS offered-piston create burst: 98 operations retained, zero drops; immutable retry, pacing, final tail");
	/* A new session clears every marker/counter; unrelated profiles keep
	 * their original per-operation loss/drop accounting. */
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	assert(hw_telemetry_save_path_pump(0));
	hw_telemetry_test_async_sender_end(); hw_telemetry_shutdown();
	cfg.profile = "host_vehicle_smoke_8m";
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	for(unsigned i = 0; i < 4; i++)
		hw_telemetry_save_path_note("region_stat", 25, stats, cache);
	assert(hw_telemetry_test_async_sender_drops() == 1);
	assert(!strstr(hw_telemetry_test_async_sender_peek(), "aggregation"));
	hw_telemetry_test_async_sender_end(); hw_telemetry_shutdown();
}

static uint32_t path_writer_done;
static void* path_writer(void* unused) {
	(void)unused;
	uint32_t stats[3] = {0}; unsigned cache[3] = {1, 0, 533};
	for(unsigned i = 1; i <= 10000; i++) {
		stats[2] = i * 25; cache[1] = i;
		hw_telemetry_save_path_note("region_stat", 25, stats, cache);
	}
	__atomic_store_n(&path_writer_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static unsigned path_word(const char* line, const char* key) {
	const char* at = strstr(line, key); assert(at);
	return (unsigned)strtoul(at + strlen(key), NULL, 10);
}

static void test_save_path_concurrent(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "pull", .pull = true,
		.port = 39017, .run_id = "path_concurrent", .profile = "split_offer_piston_2m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	path_writer_done = 0;
	struct thread writer; assert(thread_create(&writer, path_writer, NULL, 64));
	unsigned last_count = 0, now = 0;
	for(;;) {
		bool done = __atomic_load_n(&path_writer_done, __ATOMIC_ACQUIRE) != 0;
		if(done) hw_telemetry_save_path_finish();
		bool drained = hw_telemetry_save_path_pump(now);
		now += 250;
		const char* line = hw_telemetry_test_async_sender_peek();
		if(line) {
			unsigned count = path_word(line, "\"operations\":");
			assert(count >= last_count && count <= 10000);
			assert(path_word(line, "\"span_total_us\":") == count * 25);
			assert(path_word(line, "\"total_us\":") == count * 25);
			assert(path_word(line, "\"cache_hits\":") == count);
			assert(path_word(line, "\"span_max_us\":") == 25);
			last_count = count;
			struct async_wire_capture capture = {0};
			assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
		}
		if(done && drained) break;
	}
	thread_join(&writer);
	assert(last_count == 10000 && hw_telemetry_test_async_sender_drops() == 0);
	hw_telemetry_test_async_sender_end(); hw_telemetry_shutdown();
	puts("PASS save-path concurrent writer: 10000 operations, coherent snapshots, zero drops");
}

static void test_carve_attrib_async_sender_e2e(void) {
	enum { CARVE_ATTRIB_MAX = 1961, RUN_ID_ENVELOPE_GROWTH = 15 };
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "e2e", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();

	char attribution[CARVE_ATTRIB_MAX + 1];
	const char* prefix = "{\"event\":\"carve_attrib\",\"frame\":1,\"pad\":\"";
	size_t prefix_len = strlen(prefix);
	memcpy(attribution, prefix, prefix_len);
	memset(attribution + prefix_len, 'x', CARVE_ATTRIB_MAX - prefix_len - 2);
	attribution[CARVE_ATTRIB_MAX - 2] = '"';
	attribution[CARVE_ATTRIB_MAX - 1] = '}';
	attribution[CARVE_ATTRIB_MAX] = '\0';
	assert(strlen(attribution) == CARVE_ATTRIB_MAX);

	/* Three low-priority frame records fill only the observer allowance. */
	for(uint32_t frame = 1; frame <= 3; frame++) {
		hw_telemetry_note_frame(frame);
		assert(hw_telemetry_emit_observer(attribution));
	}
	hw_telemetry_note_frame(4);
	assert(!hw_telemetry_emit_observer(attribution));
	assert(hw_telemetry_test_async_sender_drops() == 1);

	/* The heartbeat takes the ring's PRIORITY LANE (2026-09-07 starvation
	 * fix), so it is admitted with the FIFO already full and is drained
	 * FIRST -- ahead of the three queued observer records rather than behind
	 * them. That ordering is the whole point: on the real Wii the sender's
	 * drain rate fell below the observer offer rate and a FIFO-ordered
	 * heartbeat went stale while the game itself ran at 61 fps. */
	struct hw_telemetry_heartbeat hb = {.tick = 4};
	hw_telemetry_emit_heartbeat(&hb);
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 4);
	assert(hw_telemetry_test_async_sender_drops() == 1);

	char expected[CARVE_ATTRIB_MAX + RUN_ID_ENVELOPE_GROWTH + 1];
	int n = snprintf(expected, sizeof(expected), "%.*s,\"run_id\":\"e2e\"}",
		CARVE_ATTRIB_MAX - 1, attribution);
	assert(n == CARVE_ATTRIB_MAX + RUN_ID_ENVELOPE_GROWTH);
	assert(capture.size > sizeof(expected));
	char* first_break = strchr(capture.bytes, '\n');
	assert(first_break);
	*first_break = '\0';
	assert(strstr(capture.bytes, "\"event\":\"heartbeat\""));
	*first_break = '\n';
	const char* after_hb = first_break + 1;
	assert(!memcmp(after_hb, expected, (size_t)n));
	assert(after_hb[n] == '\n');
	unsigned carve_records = 0, heartbeat_records = 0;
	for(char* line = capture.bytes; line && *line;) {
		char* next = strchr(line, '\n');
		assert(next);
		*next = '\0';
		if(strstr(line, "\"event\":\"carve_attrib\"")) carve_records++;
		if(strstr(line, "\"event\":\"heartbeat\"")) heartbeat_records++;
		line = next + 1;
	}
	assert(carve_records == 3 && heartbeat_records == 1);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS carve_attrib async sender E2E: intact 1961-byte source record, one counted observer drop, heartbeat reserved");
}

/* Deliverable 3 (2026-09-07 sender-starvation brief). Model the real-Wii
 * failure directly: the priority-40 sender was scheduled a handful of times
 * per second during the two-anchor reload while the carve observer offered
 * ~12 records/s, so the FIFO stood full and the heartbeat -- FIFO-ordered
 * behind that observer traffic -- reached the collector every few seconds at
 * best. tel_fail was 0 throughout (no socket ever failed) and the game kept
 * rendering at 61 fps, which is exactly why the stale-heartbeat rule fired
 * on a live console.
 *
 * The contract this pins:
 *   (i)   one heartbeat is delivered every simulated second REGARDLESS of the
 *         observer load, and it is the FIRST record off the wire each second;
 *   (ii)  observer records that do not fit are refused and COUNTED, never
 *         queued ahead of a heartbeat;
 *   (iii) no producer call blocks -- every publish returns a verdict in the
 *         same call, so main never waits on the sender.
 * Phase 2 tightens the sender to ONE record per second, the harshest rate at
 * which a 1 Hz heartbeat can still be honoured at all. */
static void test_sender_starvation_heartbeat_priority(void) {
	enum { SECONDS = 30, OBSERVERS_PER_SECOND = 12 };
	static const unsigned drain_budget[2] = {5, 1};
	static const unsigned expected_observers[2] = {3, 0};
	for(unsigned phase = 0; phase < 2; phase++) {
		struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
			.port = 39017, .run_id = "starve", .profile = "split_anchor_6m"};
		assert(hw_telemetry_init(&cfg));
		hw_telemetry_test_async_sender_begin();

		unsigned refused = 0, heartbeats = 0, observers = 0;
		for(unsigned second = 0; second < SECONDS; second++) {
			/* Adversarial order: the observer burst fills the FIFO FIRST, so
			 * the heartbeat is always published into an already-full ring. */
			for(unsigned i = 0; i < OBSERVERS_PER_SECOND; i++) {
				char record[128];
				snprintf(record, sizeof(record),
					"{\"event\":\"carve_tenant\",\"t\":\"mesher\",\"s\":%u,"
					"\"i\":%u}", second, i);
				/* (iii) returns a verdict in-call, never blocks. */
				if(!hw_telemetry_emit_observer(record))
					refused++;
			}
			struct hw_telemetry_heartbeat hb = {.tick = second};
			hw_telemetry_emit_heartbeat(&hb);

			struct async_wire_capture capture = {0};
			hw_telemetry_test_async_sender_drain_n(async_wire_send, &capture,
				drain_budget[phase]);
			unsigned second_heartbeats = 0, second_observers = 0;
			bool first = true;
			for(char* line = capture.bytes; line && *line;) {
				char* next = strchr(line, '\n');
				assert(next);
				*next = '\0';
				if(strstr(line, "\"event\":\"heartbeat\"")) {
					second_heartbeats++;
					assert(first); /* (i) heartbeat first, every second */
				} else {
					assert(strstr(line, "\"event\":\"carve_tenant\""));
					second_observers++;
				}
				first = false;
				line = next + 1;
			}
			assert(second_heartbeats == 1);
			assert(second_observers == expected_observers[phase]);
			heartbeats += second_heartbeats;
			observers += second_observers;
		}
		assert(heartbeats == SECONDS);
		assert(observers == SECONDS * expected_observers[phase]);
		/* (ii) every unadmitted observer record is counted, exactly once.
		 * Phase 0 (5/s): the FIFO empties every second, so 3 of 12 are
		 * admitted each second -> 30 * 9 = 270 refusals and nothing resident.
		 * Phase 1 (1/s): the sender only ever gets to the heartbeat, so the
		 * 3 admitted in the first second stay queued forever and every later
		 * offer is refused -> 9 + 29 * 12 = 357 refusals, 3 resident, 0
		 * delivered. Observer loss is the DESIGNED outcome of a sender that
		 * slow; the heartbeat is what may not be lost. */
		static const unsigned expected_refusals[2] = {270, 357};
		assert(refused == expected_refusals[phase]);
		assert(refused + observers + (phase ? 3u : 0u)
			   == SECONDS * OBSERVERS_PER_SECOND);
		assert(hw_telemetry_test_async_sender_drops() == refused);

		struct hw_telemetry_sender_stats stats;
		hw_telemetry_sender_stats_take(&stats);
		assert(stats.drops == refused);
		/* The lane never had to supersede an undelivered heartbeat: the
		 * sender, however slow, always drained it inside its own second. */
		assert(stats.hb_replaced == 0);
		/* 3 FIFO records (one slot stays reserved from observers) + lane. */
		assert(stats.occupancy_max == 4);
		assert(stats.sends == heartbeats + observers);
		hw_telemetry_sender_stats_take(&stats);
		assert(!stats.sends && !stats.iterations); /* windowed: take resets */
		hw_telemetry_test_async_sender_end();
		hw_telemetry_shutdown();
	}
	puts("PASS sender starvation: a 5/s and a 1/s sender both deliver every "
		 "1 Hz heartbeat first under a 12/s observer load; refusals counted, "
		 "producers never block");
}

/* BOUNDED SENDS (2026-09-08). The 2026-09-07 real-Wii split_anchor_6m runs
 * died with tel_smax_us climbing 17 -> 48 -> 166 ms and tel_it falling
 * 142 -> 11 -> 0: the sender sat INSIDE one blocking net_send for longer than
 * a whole heartbeat window while the server thread held IOS IPC in a libfat
 * fsync. This drives the production sender path (ring -> sender_drain_one ->
 * send_line_with) against a clock and a writability gate the test owns, and
 * proves the two properties the fix claims:
 *   (1) a socket that never becomes writable costs the sender at most
 *       HW_TELEMETRY_SEND_DEADLINE_MS per line, and the line is abandoned --
 *       not retried, and NOT charged to the transient-failure strikes that
 *       exist to detect a dead link;
 *   (2) a send that parks mid-line (the stub burns 2 s inside one 1 KiB
 *       chunk, exactly what net_send did on hardware) loses the REST of that
 *       line only: the next line -- the heartbeat, on its own lane -- still
 *       reaches the wire inside the collector's 5 s stale limit.
 * The residual this test also pins honestly: the one chunk already handed to
 * the transport cannot be cancelled, so its 2 s IS spent. What the deadline
 * buys is that no further chunk is ever STARTED past it. */
static uint64_t s_gate_clock_ms;
static bool s_gate_blocked;
static uint32_t s_gate_stall_ms;
static unsigned s_gate_polls;
static unsigned s_gate_poll_timeouts;
/* >0: time out exactly this many more polls, then report writable. */
static unsigned s_gate_block_polls;

static int deadline_gate_ready(void* user, uint32_t timeout_ms) {
	(void)user;
	s_gate_polls++;
	if(!s_gate_blocked && !s_gate_block_polls)
		return 1; /* writable */
	if(s_gate_block_polls)
		s_gate_block_polls--;
	/* IOS honours net_poll's own timeout whether or not the socket ever
	 * becomes writable: the wait costs exactly the deadline. */
	s_gate_clock_ms += timeout_ms;
	s_gate_poll_timeouts++;
	return 0;
}

static uint64_t deadline_gate_now(void* user) {
	(void)user;
	return s_gate_clock_ms;
}

static int deadline_wire_send(const char* data, size_t len, void* user) {
	struct async_wire_capture* capture = user;
	assert(capture->size + len < sizeof(capture->bytes));
	memcpy(capture->bytes + capture->size, data, len);
	capture->size += len;
	capture->bytes[capture->size] = '\0';
	/* One chunk parked inside the transport, then never again for this run. */
	s_gate_clock_ms += s_gate_stall_ms;
	s_gate_stall_ms = 0;
	return (int)len;
}

static void test_bounded_send_deadline_keeps_heartbeat_alive(void) {
	enum { SECONDS = 8, OBSERVERS_PER_SECOND = 12, STALE_LIMIT_MS = 5000,
		   BLOCKED_FROM = 2, BLOCKED_TO = 3, STALL_SECOND = 5,
		   STALL_MS = 2000, PADDED = 1500, SEND_COST_MS = 10 };
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "deadline", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	hw_telemetry_test_set_send_gate(deadline_gate_ready, deadline_gate_now,
									NULL);
	s_gate_clock_ms = 0;
	s_gate_blocked = false;
	s_gate_stall_ms = 0;
	s_gate_polls = 0;
	s_gate_poll_timeouts = 0;
	s_gate_block_polls = 0;

	static struct async_wire_capture wire;
	memset(&wire, 0, sizeof(wire));
	struct hw_telemetry_sender_stats stats;
	hw_telemetry_sender_stats_take(&stats); /* start the window clean */
	uint64_t last_heartbeat_ms = 0, max_gap_ms = 0, worst_wait_ms = 0;
	unsigned heartbeats = 0, abandoned = 0, stalls_armed = 0, midline = 0;
	char stalled_id[64] = {0};

	for(unsigned second = 0; second < SECONDS; second++) {
		s_gate_blocked = second >= BLOCKED_FROM && second <= BLOCKED_TO;
		for(unsigned i = 0; i < OBSERVERS_PER_SECOND; i++) {
			/* One padded record per second is larger than the 1 KiB send
			 * chunk, so a mid-line abandonment is observable at all. */
			char record[PADDED + 1];
			int n = snprintf(record, sizeof(record),
				"{\"event\":\"carve_tenant\",\"id\":\"s%ui%u\",\"pad\":\"",
				second, i);
			assert(n > 0 && (size_t)n < sizeof(record));
			size_t pad = i == 0 ? PADDED - (size_t)n - 3 : 8;
			memset(record + n, 'x', pad);
			memcpy(record + n + pad, "\"}", 3);
			(void)hw_telemetry_emit_observer(record);
		}
		struct hw_telemetry_heartbeat hb = {.tick = second};
		hw_telemetry_emit_heartbeat(&hb);

		uint64_t second_end = (uint64_t)(second + 1) * 1000u;
		unsigned attempts = 0;
		while(s_gate_clock_ms < second_end) {
			/* Arm the 2 s park on the SECOND line of that second, i.e. after
			 * the heartbeat has already taken its priority-lane turn: the
			 * point being proven is that a parked OBSERVER line does not
			 * take the heartbeat with it. */
			if(second == STALL_SECOND && attempts == 1 && !stalls_armed) {
				s_gate_stall_ms = STALL_MS;
				stalls_armed++;
			}
			attempts++;
			uint64_t before = s_gate_clock_ms;
			size_t wire_before = wire.size;
			unsigned sent = hw_telemetry_test_async_sender_drain_n(
				deadline_wire_send, &wire, 1);
			hw_telemetry_sender_stats_take(&stats);
			if(!stats.sends)
				break; /* ring empty: the sender would sleep, not spin */
			uint64_t waited = s_gate_clock_ms - before;
			if(waited > worst_wait_ms)
				worst_wait_ms = waited;
			if(sent) {
				s_gate_clock_ms += SEND_COST_MS;
				if(strstr(wire.bytes + wire_before, "\"event\":\"heartbeat\"")) {
					uint64_t gap = s_gate_clock_ms - last_heartbeat_ms;
					if(gap > max_gap_ms)
						max_gap_ms = gap;
					last_heartbeat_ms = s_gate_clock_ms;
					heartbeats++;
				}
			} else {
				abandoned++;
				const char* id = wire.size > wire_before
					? strstr(wire.bytes + wire_before, "\"id\":\"") : NULL;
				if(id && !stalled_id[0]) {
					/* The mid-line abandonment: exactly one 1 KiB chunk
					 * reached the wire and its record id is truncated there
					 * for good. */
					size_t idn = strcspn(id + 6, "\"");
					assert(idn && idn < sizeof(stalled_id));
					memcpy(stalled_id, id + 6, idn);
					stalled_id[idn] = '\0';
					assert(wire.size - wire_before == 1024);
					midline++;
				}
			}
		}
		if(s_gate_clock_ms < second_end)
			s_gate_clock_ms = second_end;
	}
	hw_telemetry_test_set_send_gate(NULL, NULL, NULL);

	/* (1) The deadline is the only wait a closed socket can cost, and the
	 * only wait longer than it is the single chunk the transport had already
	 * accepted (2 s), which nothing in this process can cancel. */
	assert(worst_wait_ms == STALL_MS);
	/* 2026-09-26 (telreliable): the heartbeat now retries a timed-out poll
	 * up to HW_TELEMETRY_CRITICAL_POLL_TRIES times while its framing is
	 * clean, so in a fully blocked second it takes the whole second and the
	 * observers behind it are refused at admission instead -- at least one
	 * abandonment per blocked second, never a lost-then-silent stream. */
	assert(abandoned >= BLOCKED_TO - BLOCKED_FROM + 1);
	/* (2) The heartbeat never went stale: the blocked window abandons lines,
	 * it does not stop the stream, and no heartbeat waited 5 s for its turn. */
	assert(heartbeats >= 4);
	assert(max_gap_ms < STALE_LIMIT_MS);
	/* (3) Abandonment is counted as such -- never as a transport failure, so
	 * the four-strike transient quiesce is untouched and the stream lives. */
	hw_telemetry_sender_stats_take(&stats);
	/* Every timed-out poll (the heartbeat's retries included) plus the one
	 * mid-line OBSERVER abandonment is counted -- nothing else. */
	assert(midline == 1);
	assert(stats.write_timeouts == s_gate_poll_timeouts + midline);
	assert(hw_telemetry_tel_fail_count() == 0);
	assert(!hw_telemetry_quiesced());
	/* (4) The abandoned bytes are NEVER resent (the 4096 duplicate trap): the
	 * truncated record's id appears exactly once on the whole wire. */
	assert(stalled_id[0]);
	unsigned occurrences = 0;
	for(const char* p = strstr(wire.bytes, stalled_id); p;
		p = strstr(p + 1, stalled_id))
		occurrences++;
	assert(occurrences == 1);
	/* (5) ESCAPE HATCH. No PC test can prove IOS reports POLLOUT the way this
	 * gate assumes, so a gate that never opens must not be able to kill the
	 * stream: after HW_TELEMETRY_SEND_GATE_STRIKES consecutive abandonments
	 * with no successful line in between it disarms itself and the sender
	 * reverts to its pre-deadline blocking behaviour. Drive exactly that -- a
	 * permanently closed gate -- and require records to flow again. */
	hw_telemetry_test_set_send_gate(deadline_gate_ready, deadline_gate_now,
									NULL);
	s_gate_blocked = true;
	unsigned struck = 0, recovered = 0;
	for(unsigned i = 0; i < 24; i++) {
		char record[64];
		snprintf(record, sizeof(record),
			"{\"event\":\"carve_tenant\",\"id\":\"strike%u\"}", i);
		if(!hw_telemetry_emit_observer(record))
			continue;
		struct async_wire_capture strike = {0};
		if(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &strike,
												  1))
			recovered++;
		else
			struck++;
	}
	assert(struck == 20 && recovered);
	hw_telemetry_test_set_send_gate(NULL, NULL, NULL);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	printf("PASS bounded sends: %u lines abandoned at the %u ms deadline "
		   "(worst single wait %u ms = the one chunk already inside the "
		   "transport), %u heartbeats delivered, worst heartbeat gap %u ms < "
		   "%u ms stale limit, tel_fail 0, abandoned bytes never resent; a "
		   "gate that never opens disarms after 20 strikes and the stream "
		   "returns to its blocking behaviour\n",
		   abandoned, (unsigned)HW_TELEMETRY_SEND_DEADLINE_MS,
		   (unsigned)worst_wait_ms, heartbeats, (unsigned)max_gap_ms,
		   (unsigned)STALE_LIMIT_MS);
}

/* Y:a:3093/3095: a complete census body can be accepted by the next
 * record's resync LF even though the sender charged its own LF as timeout. */
static void test_observer_body_completion(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "pull",
		.pull = true, .port = 39017, .run_id = "newline"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	hw_telemetry_test_set_send_gate(deadline_gate_ready, deadline_gate_now, NULL);
	s_gate_clock_ms = 0; s_gate_blocked = false; s_gate_block_polls = 0;
	s_gate_stall_ms = 255;
	struct async_wire_capture capture = {0};
	const char* row = "{\"event\":\"service_observer\",\"seq\":1,\"states\":[]}";
	assert(hw_telemetry_emit_service_observer(row));
	assert(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &capture, 1) == 1);
	assert(capture.size && capture.bytes[capture.size-1] == '\n');
	struct hw_telemetry_sender_stats stats; hw_telemetry_sender_stats_take(&stats);
	assert(!stats.write_timeouts && !hw_telemetry_tel_fail_count());
	hw_telemetry_test_set_send_gate(NULL, NULL, NULL);
	hw_telemetry_test_async_sender_end(); hw_telemetry_shutdown();
	puts("PASS complete observer JSON gets one trailing LF despite body-completion deadline");
}
static void test_y_census_body_completion(const char* path) {
	FILE* f = fopen(path, "rb"); assert(f);
	char row[HW_TELEMETRY_HEARTBEAT_LINE_SIZE]; bool found = false;
	while(fgets(row, sizeof row, f)) if(strstr(row, "\"seq\":2463,")) { found = true; break; }
	assert(!fclose(f) && found);
	size_t n = strlen(row); while(n && (row[n-1] == '\n' || row[n-1] == '\r')) row[--n] = 0;
	assert(n < 1024); /* The real Y census is one body chunk + LF. */
	hw_telemetry_test_async_sender_begin();
	hw_telemetry_test_set_send_gate(deadline_gate_ready, deadline_gate_now, NULL);
	s_gate_clock_ms = 0; s_gate_blocked = false; s_gate_block_polls = 0;
	s_gate_stall_ms = 501; /* Y:a:3119 tel_xmax_us=501191, tel_wto=1. */
	struct async_wire_capture capture = {0}; struct hw_telemetry_send_fail_state failure = {0};
	assert(hw_telemetry_test_send_line_with_gate(row, deadline_wire_send, &capture, &failure));
	assert(capture.size == n+1 && !memcmp(row, capture.bytes, n) && capture.bytes[n] == '\n');
	assert(!failure.resync && !failure.fail_count);
	printf("%s", capture.bytes);
	hw_telemetry_test_set_send_gate(NULL, NULL, NULL); hw_telemetry_test_async_sender_end();
	puts("PASS verbatim SHA-pinned Y:a:3093 body, one LF, no false drop or replay");
}

/* EVIDENCE LINES ARE NEVER ABANDONED MID-LINE (2026-09-26, telreliable).
 * Reproduces the G4 real-Wii split_play_8m false FAIL: one net_send chunk of
 * a multi-KiB heartbeat took 255 ms inside IOS (tel_xmax_us=255065) and the
 * 250 ms per-line deadline, expiring between chunks, abandoned the heartbeat
 * after 1 KiB was already on the wire -- tel_wto 0->1, tick N missing on the
 * host, 60 anchor frame times gone. Proves, on the production sender path
 * (ring -> sender_drain_one -> send_line_with):
 *   (1) the same stall now costs the heartbeat nothing: the whole line and its
 *       newline reach the wire exactly once, tel_wto does not move;
 *   (2) anchor_frames gets the same treatment;
 *   (3) an OBSERVER line under the identical stall is still abandoned at the
 *       deadline (the 2026-09-08 bound is untouched for diagnostics);
 *   (4) a heartbeat whose writability poll times out BEFORE any byte is
 *       retried (clean framing) and delivered once the socket opens, each
 *       timed-out poll counted; and it gives up after exactly
 *       HW_TELEMETRY_CRITICAL_POLL_TRIES polls with nothing on the wire, so
 *       the worst sender wait for one evidence line stays bounded. */
static unsigned count_substr(const char* hay, const char* needle) {
	unsigned n = 0;
	for(const char* p = strstr(hay, needle); p; p = strstr(p + 1, needle))
		n++;
	return n;
}

static void test_critical_lines_survive_midline_stall(void) {
	enum { G4_STALL_MS = 255 };
	/* Classification: exact record prefixes only. */
	assert(hw_telemetry_line_is_critical(
		"{\"event\":\"heartbeat\",\"run_id\":\"x\"}"));
	assert(hw_telemetry_line_is_critical(
		"{\"event\":\"anchor_frames\",\"run_id\":\"x\"}"));
	assert(hw_telemetry_line_is_critical(
		"{\"event\":\"profile_done\",\"run_id\":\"x\"}"));
	assert(hw_telemetry_line_is_critical(
		"{\"event\":\"perf_survival_guard\",\"heartbeat_tick\":2}"));
	assert(!hw_telemetry_line_is_critical(
		"{\"event\":\"perf_survival_guardx\",\"heartbeat_tick\":2}"));
	assert(!hw_telemetry_line_is_critical(
		"{\"event\":\"carve_tenant\",\"id\":1}"));
	assert(!hw_telemetry_line_is_critical(
		"{\"event\":\"heartbeatx\",\"id\":1}"));
	assert(!hw_telemetry_line_is_critical(
		"{\"event\":\"boot\",\"heartbeat\":1}"));
	assert(!hw_telemetry_line_is_critical(""));
	assert(!hw_telemetry_line_is_critical(NULL));

	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39018, .run_id = "critical", .profile = "split_play_8m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	hw_telemetry_test_set_send_gate(deadline_gate_ready, deadline_gate_now,
									NULL);
	s_gate_clock_ms = 0;
	s_gate_blocked = false;
	s_gate_polls = 0;
	s_gate_poll_timeouts = 0;
	s_gate_block_polls = 0;
	struct hw_telemetry_sender_stats stats;
	hw_telemetry_sender_stats_take(&stats);

	/* (1) The G4 heartbeat: anchor block + 60 frame samples, several KiB. */
	static struct hw_telemetry_heartbeat hb;
	memset(&hb, 0, sizeof(hb));
	hb.tick = 133;
	hb.anchor.enabled = true;
	hb.anchor.separation = 96;
	hb.anchor.measuring = true;
	hb.anchor.frame_count = 60;
	hb.anchor.frame_total = 6883;
	for(unsigned j = 0; j < 60; j++)
		hb.anchor.frame_us[j] = 16666u + j;
	hw_telemetry_emit_heartbeat(&hb);
	static struct async_wire_capture wire;
	memset(&wire, 0, sizeof(wire));
	s_gate_stall_ms = G4_STALL_MS; /* first chunk parks 255 ms */
	assert(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &wire,
												  1) == 1);
	assert(wire.size > 2 * 1024); /* genuinely multi-chunk */
	assert(wire.bytes[wire.size - 1] == '\n');
	assert(!strncmp(wire.bytes, "{\"event\":\"heartbeat\",", 21));
	assert(strstr(wire.bytes, "\"tick\":133,"));
	assert(strstr(wire.bytes, ",16725]")); /* the last frame sample */
	assert(count_substr(wire.bytes, "\"event\":\"heartbeat\"") == 1);
	hw_telemetry_sender_stats_take(&stats);
	assert(stats.write_timeouts == 0);

	/* (2) anchor_frames (256 samples, > 1 chunk) under the same stall. */
	hb.anchor.frame_count = HW_ANCHOR_FRAME_CAP;
	for(unsigned j = 0; j < HW_ANCHOR_FRAME_CAP; j++)
		hb.anchor.frame_us[j] = 1000000u + j;
	assert(hw_telemetry_emit_anchor_frames(&hb.anchor));
	memset(&wire, 0, sizeof(wire));
	s_gate_stall_ms = G4_STALL_MS;
	assert(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &wire,
												  1) == 1);
	assert(wire.size > 1024 && wire.bytes[wire.size - 1] == '\n');
	assert(!strncmp(wire.bytes, "{\"event\":\"anchor_frames\",", 25));
	assert(strstr(wire.bytes, ",1000255]}"));
	hw_telemetry_sender_stats_take(&stats);
	assert(stats.write_timeouts == 0);

	/* (3) An observer line under the identical stall: still abandoned after
	 * exactly one chunk, counted once in tel_wto. */
	char record[2048];
	int n = snprintf(record, sizeof(record),
					 "{\"event\":\"carve_tenant\",\"id\":\"crit\",\"pad\":\"");
	assert(n > 0);
	memset(record + n, 'x', 1500);
	memcpy(record + n + 1500, "\"}", 3);
	assert(hw_telemetry_emit_observer(record));
	memset(&wire, 0, sizeof(wire));
	s_gate_stall_ms = G4_STALL_MS;
	assert(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &wire,
												  1) == 0);
	assert(wire.size == 1024);
	hw_telemetry_sender_stats_take(&stats);
	assert(stats.write_timeouts == 1);

	/* (4a) Poll times out twice before any byte, then opens: delivered. */
	hb.tick = 134;
	hb.anchor.frame_count = 60;
	hw_telemetry_emit_heartbeat(&hb);
	memset(&wire, 0, sizeof(wire));
	s_gate_poll_timeouts = 0;
	s_gate_block_polls = 2;
	uint64_t before = s_gate_clock_ms;
	assert(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &wire,
												  1) == 1);
	assert(s_gate_poll_timeouts == 2);
	assert(s_gate_clock_ms - before == 2 * HW_TELEMETRY_SEND_DEADLINE_MS);
	assert(strstr(wire.bytes, "\"tick\":134,")
		   && wire.bytes[wire.size - 1] == '\n');
	hw_telemetry_sender_stats_take(&stats);
	assert(stats.write_timeouts == 1 + 2);

	/* (4b) Never writable: gives up after exactly the retry budget, nothing
	 * on the wire (no resync owed), wait bounded. */
	hb.tick = 135;
	hw_telemetry_emit_heartbeat(&hb);
	memset(&wire, 0, sizeof(wire));
	s_gate_poll_timeouts = 0;
	s_gate_blocked = true;
	before = s_gate_clock_ms;
	assert(hw_telemetry_test_async_sender_drain_n(deadline_wire_send, &wire,
												  1) == 0);
	assert(s_gate_poll_timeouts == HW_TELEMETRY_CRITICAL_POLL_TRIES);
	assert(s_gate_clock_ms - before
		   == HW_TELEMETRY_CRITICAL_POLL_TRIES * HW_TELEMETRY_SEND_DEADLINE_MS);
	assert(wire.size == 0);
	s_gate_blocked = false;
	hw_telemetry_sender_stats_take(&stats);
	assert(stats.write_timeouts == 3 + HW_TELEMETRY_CRITICAL_POLL_TRIES);
	assert(hw_telemetry_tel_fail_count() == 0);
	assert(!hw_telemetry_quiesced());

	hw_telemetry_test_set_send_gate(NULL, NULL, NULL);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	printf("PASS critical lines: a %u ms mid-line IOS stall (the G4 "
		   "split_play_8m tel_xmax_us=255065 case) no longer abandons a "
		   "heartbeat or anchor_frames record (tel_wto unchanged), observers "
		   "still abandon at the %u ms deadline, and a pre-byte poll timeout "
		   "retries up to %u times (bounded, nothing on the wire on give-up)\n",
		   (unsigned)G4_STALL_MS, (unsigned)HW_TELEMETRY_SEND_DEADLINE_MS,
		   (unsigned)HW_TELEMETRY_CRITICAL_POLL_TRIES);
}

/* SENDER STAGE INSTRUMENTATION (2026-09-08 wedge lane). The 2026-09-07
 * split_anchor_6m deaths gave the host tel_it 0 / tel_sends 0 for 4-5 s with
 * tel_wto climbing, and no counter said WHICH call the sender was parked in.
 * These two tests prove the stage fields answer that, and that the opt-in
 * non-blocking experiment abandons rather than waits.
 *
 * Modelling note: the real sender is its own LWP, so main composes a
 * heartbeat WHILE the sender sits in a syscall. A PC test has one thread, so
 * the stub transport itself is "the moment of being parked" -- it advances a
 * virtual clock and snapshots the stats from inside the stalled call, which
 * is exactly the read the concurrent main thread performs on hardware. The
 * clock is virtual precisely so a 3 s park costs the suite no wall time. */
static uint64_t s_stage_clock_us;

static uint64_t stage_test_now_us(void* user) {
	(void)user;
	return s_stage_clock_us;
}

static uint64_t stage_test_now_ms(void* user) {
	(void)user;
	return s_stage_clock_us / 1000u;
}

static int stage_test_gate_ready(void* user, uint32_t timeout_ms) {
	(void)user;
	(void)timeout_ms;
	/* Writable immediately: this test is about the SEND stage, so the poll
	 * must contribute a small, clearly-distinguishable duration. */
	s_stage_clock_us += 1000u; /* 1 ms */
	return 1;
}

struct stage_probe {
	unsigned calls;
	unsigned stall_at;
	uint32_t stall_us;
	/* Snapshot taken from INSIDE the stalled transport call. */
	struct hw_telemetry_sender_stats parked;
	uint32_t parked_stage;
	bool captured;
};

static int stage_probe_send(const char* data, size_t len, void* user) {
	struct stage_probe* p = user;
	(void)data;
	p->calls++;
	if(p->calls == p->stall_at) {
		s_stage_clock_us += p->stall_us;
		/* Still inside the transport: this is the read main.c performs. */
		p->parked_stage = hw_telemetry_test_sender_stage();
		hw_telemetry_sender_stats_take(&p->parked);
		p->captured = true;
	}
	return (int)len;
}

static void test_sender_stage_names_the_stuck_call(void) {
	enum { STALL_US = 3000000u }; /* the 3 s park the brief specifies */
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "stage", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	s_stage_clock_us = 0;
	hw_telemetry_test_set_stage_clock(stage_test_now_us, NULL);
	hw_telemetry_test_set_send_gate(stage_test_gate_ready, stage_test_now_ms,
									NULL);
	/* Baseline: with nothing in flight the sender is not in a syscall, and
	 * the stage field must say so rather than reporting a stale SEND. */
	struct hw_telemetry_sender_stats idle;
	hw_telemetry_sender_stats_take(&idle);
	assert(idle.stage != HW_TEL_STAGE_SEND);

	/* Three records; the transport parks for 3 s inside the SECOND chunk it
	 * is handed, which is mid-line -- the shape the hardware showed. */
	struct stage_probe probe = {.stall_at = 2, .stall_us = STALL_US};
	for(unsigned i = 0; i < 3; i++) {
		char record[64];
		snprintf(record, sizeof(record),
				 "{\"event\":\"carve_tenant\",\"id\":\"stage%u\"}", i);
		assert(hw_telemetry_emit_observer(record));
	}
	unsigned drained =
		hw_telemetry_test_async_sender_drain(stage_probe_send, &probe);
	assert(drained == 3 && probe.captured);

	/* (1) The stage field NAMES the stuck call. */
	assert(probe.parked_stage == HW_TEL_STAGE_SEND);
	assert(probe.parked.stage == HW_TEL_STAGE_SEND);
	/* (2) ...and how long it has been stuck, live, on a heartbeat composed
	 * DURING the park -- the whole point, since the heartbeat priority lane
	 * is the one record that still got through on hardware. */
	assert(probe.parked.stage_us >= STALL_US);
	/* (3) The windowed per-syscall maximum records the park once the call
	 * returns. The mid-park take above reset the window, so this is the
	 * duration folded in by the stage exit, not a leftover. */
	struct hw_telemetry_sender_stats after;
	hw_telemetry_sender_stats_take(&after);
	assert(after.send_max_us >= STALL_US);
	/* (4) The poll is timed SEPARATELY and is not confused with the send:
	 * 1 ms per gate call, never the 3 s park. tel_smax_us cannot make this
	 * distinction, which is why the two _max fields exist. */
	assert(after.poll_max_us > 0 && after.poll_max_us < STALL_US);
	/* (5) The raw transport result is published for the "what did the call
	 * say?" half of the question. */
	assert(after.last_send_ret > 0);
	/* (6) Nothing here is a failure: a slow link is not a dead one. */
	assert(hw_telemetry_tel_fail_count() == 0);
	assert(after.abandoned == 0);

	hw_telemetry_test_set_send_gate(NULL, NULL, NULL);
	hw_telemetry_test_set_stage_clock(NULL, NULL);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	printf("PASS sender stage: a %u us park inside the transport reports "
		   "stage=SEND with stage_us=%u on a heartbeat composed DURING the "
		   "park, windowed send_max_us=%u, poll_max_us=%u kept separate, "
		   "tel_fail 0\n",
		   (unsigned)STALL_US, probe.parked.stage_us, after.send_max_us,
		   after.poll_max_us);
}

/* SENDER STAGE LOCK (2026-09-08 fix round, hostile review item 2). The
 * blocking LWP_MutexLock(s_send_mutex) in send_lock_enter sits BEFORE
 * send_line_transport enters any of its five instrumented stages, so a park
 * there used to surface whatever stage the PREVIOUS record left behind, with
 * THAT stage's stale entry timestamp -- an ever-growing dwell on a stage the
 * sender was not actually in (a 4 s mutex park could read "stage=idle,
 * tel_stg_us=4,000,000", read by the collector as "alive but starved" on the
 * wrong evidence). This proves both halves of the fix: the mutex wait now
 * NAMES itself (HW_TEL_STAGE_LOCK) instead of borrowing the prior stage, and
 * leaving it re-stamps the restored stage with NOW rather than the prior
 * stage's original (now stale) entry time.
 *
 * Modelling note: off Wii there is no real mutex to block on, so
 * hw_telemetry_test_set_lock_wait arms a PC stand-in invoked from INSIDE the
 * same LOCK stage enter/leave the real LWP_MutexLock gets -- same technique
 * as stage_test_gate_ready/stage_probe_send above, and the same virtual
 * clock so a 3 s park costs the suite no wall time. */
struct lock_probe {
	uint32_t parked_stage;
	struct hw_telemetry_sender_stats parked;
};

enum { LOCK_STALL_US = 3000000u }; /* the 3 s park the brief specifies */

static void lock_wait_stall(void* user) {
	struct lock_probe* p = user;
	s_stage_clock_us += LOCK_STALL_US;
	/* Still "inside" the stall: the read a concurrent main thread would
	 * perform on hardware while parked on the real mutex. */
	p->parked_stage = hw_telemetry_test_sender_stage();
	hw_telemetry_sender_stats_take(&p->parked);
}

static void test_sender_stage_names_lock_park(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "lock", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	s_stage_clock_us = 0;
	hw_telemetry_test_set_stage_clock(stage_test_now_us, NULL);

	struct lock_probe probe = {0};
	hw_telemetry_test_set_lock_wait(lock_wait_stall, &probe);
	/* Any NORMAL-mode emit reaches send_lock_enter; disarmed (the default),
	 * this hook is a no-op, so every OTHER existing test is unaffected. The
	 * PC transport never actually delivers here (no sink/gate installed),
	 * so the return value carries no meaning -- the hook already captured
	 * everything this test needs before send_lock_enter returns. */
	(void)hw_telemetry_emit_observer(
		"{\"event\":\"carve_tenant\",\"id\":\"lockprobe\"}");

	/* (1) The park NAMES itself instead of borrowing the previous stage. */
	assert(probe.parked_stage == HW_TEL_STAGE_LOCK);
	assert(probe.parked.stage == HW_TEL_STAGE_LOCK);
	/* (2) ...and its dwell is the real 3 s park, live. */
	assert(probe.parked.stage_us >= LOCK_STALL_US);

	/* (3) Once released, the stage LEAVES lock with a FRESH stamp: reading
	 * immediately after must NOT still show a multi-second dwell hanging off
	 * the now-inactive LOCK stage (the pre-fix bug: sender_stage_leave
	 * restored the prior stage's OLD entry timestamp verbatim). */
	struct hw_telemetry_sender_stats after;
	hw_telemetry_sender_stats_take(&after);
	assert(after.stage != HW_TEL_STAGE_LOCK);
	assert(after.stage_us < LOCK_STALL_US);

	hw_telemetry_test_set_lock_wait(NULL, NULL);
	hw_telemetry_test_set_stage_clock(NULL, NULL);
	hw_telemetry_shutdown();
	printf("PASS sender stage LOCK: a %u us park inside send_lock_enter's "
		   "blocking mutex reports stage=LOCK with stage_us=%u live, and "
		   "restores to stage=%u with a fresh (non-stale) dwell of %u us "
		   "once released\n",
		   (unsigned)LOCK_STALL_US, probe.parked.stage_us, after.stage,
		   after.stage_us);
}

/* NONBLOCK FLAG VERIFY (2026-09-08 fix round, hostile review item 1).
 * cw_net_set_nonblock used to trust net_fcntl(F_SETFL)'s own success return
 * alone, with no F_GETFL read-back -- an IOS SETFL that reports success
 * without the bit actually latching would sail through unnoticed, and the
 * armed non-blocking experiment then drops the per-line poll gate on the
 * strength of that unverified flag, parking the sender in a blocking
 * net_send indefinitely (worse than never arming the experiment at all).
 * cw_net_flags_confirm_nonblock is the pure decision cw_net_set_nonblock's
 * read-back now uses; this proves it with synthetic net_fcntl results,
 * independent of the real accept()-path integration, which stays Wii-only. */
static void test_nonblock_verify_catches_unlatched_setfl(void) {
	/* THE regression: SETFL reports success (>=0), but the GETFL read-back
	 * right after it shows NEITHER bit set -- the bit never actually
	 * latched. Must be rejected, not trusted. */
	assert(!hw_telemetry_test_nonblock_flag_confirm(0, 0x00));
	/* The honest success case: SETFL ok, GETFL confirms IOS's 0x04 bit. */
	assert(hw_telemetry_test_nonblock_flag_confirm(0, 0x04));
	/* SETFL itself failed outright. */
	assert(!hw_telemetry_test_nonblock_flag_confirm(-1, 0x04));
	/* SETFL "succeeded" but the read-back call itself errored. */
	assert(!hw_telemetry_test_nonblock_flag_confirm(0, -1));
	printf("PASS nonblock verify: a SETFL success with no F_GETFL bit "
		   "read-back is correctly rejected (the exact pre-fix regression); "
		   "only a read-back that shows IOS's bit set confirms success\n");
}

/* NON-BLOCKING SEND EXPERIMENT (--cw-tel-nonblock, opt-in). Proves the three
 * properties the switch claims: a socket with no room ABANDONS instead of
 * waiting, the abandonment is COUNTED and never retried, and it is not
 * charged to the transient strikes that exist to detect a dead link -- while
 * a genuinely hard errno still is, so the experiment cannot make a dead
 * socket look alive. */
struct nonblock_probe {
	unsigned calls;
	size_t total;
	int result; /* what the stub returns for the FIRST call */
	bool short_write;
};

static int nonblock_probe_send(const char* data, size_t len, void* user) {
	struct nonblock_probe* p = user;
	(void)data;
	p->calls++;
	if(p->calls == 1) {
		if(p->short_write) {
			/* Buffer filled mid-chunk: bytes DID reach the wire. */
			size_t partial = len > 4 ? 4 : len;
			p->total += partial;
			return (int)partial;
		}
		return p->result;
	}
	p->total += len;
	return (int)len;
}

static void test_nonblock_experiment_abandons_instead_of_blocking(void) {
	const char* line = "{\"event\":\"carve_tenant\",\"id\":\"nb\"}";
	struct hw_telemetry_send_fail_state st;
	struct nonblock_probe probe;
	uint32_t abandoned_before, abandoned_after;
	struct hw_telemetry_sender_stats stats;

	/* DEFAULT (unarmed): a short write is NOT an abandonment -- the blocking
	 * path loops until the line is out, exactly as before this switch
	 * existed. This is the byte-identical-when-unarmed guarantee. */
	assert(!hw_telemetry_nonblock_experiment());
	hw_telemetry_send_fail_state_reset(&st);
	probe = (struct nonblock_probe) {.short_write = true};
	assert(hw_telemetry_test_send_line_with_state(line, nonblock_probe_send,
												  &probe, &st, NULL));
	assert(probe.calls > 1 && !st.resync && st.fail_count == 0);

	hw_telemetry_set_nonblock_experiment(true);
	assert(hw_telemetry_nonblock_experiment());
	hw_telemetry_sender_stats_take(&stats);
	abandoned_before = stats.abandoned;

	/* (1) A SHORT WRITE abandons the line: the stub is never called again
	 * for it (no retry -- the 2026-09-02 duplicate-bytes trap), and a
	 * resync is owed because bytes reached the wire. */
	hw_telemetry_send_fail_state_reset(&st);
	probe = (struct nonblock_probe) {.short_write = true};
	assert(!hw_telemetry_test_send_line_with_state(line, nonblock_probe_send,
												   &probe, &st, NULL));
	assert(probe.calls == 1);
	assert(st.resync);
	/* NOT charged to the strike ladder: the link is alive, it was full. */
	assert(st.fail_count == 0 && st.consec_fail == 0 && !st.quiesced);

	/* (2) -EAGAIN abandons the same way. libogc may have queued a partial
	 * before returning it, so a resync is owed even though `queued` is 0 --
	 * the collector then loses exactly one malformed NDJSON line. */
	hw_telemetry_send_fail_state_reset(&st);
	probe = (struct nonblock_probe) {.result = -EAGAIN};
	assert(!hw_telemetry_test_send_line_with_state(line, nonblock_probe_send,
												   &probe, &st, NULL));
	assert(probe.calls == 1 && st.resync);
	assert(st.fail_count == 0 && st.consec_fail == 0 && !st.quiesced);

	/* -EWOULDBLOCK is the same bucket. */
	hw_telemetry_send_fail_state_reset(&st);
	probe = (struct nonblock_probe) {.result = -EWOULDBLOCK};
	assert(!hw_telemetry_test_send_line_with_state(line, nonblock_probe_send,
												   &probe, &st, NULL));
	assert(probe.calls == 1 && st.fail_count == 0 && !st.quiesced);

	/* (3) The abandonments are COUNTED -- three of them, one per armed line
	 * above, on their own counter and not on the tel_wto deadline counter. */
	hw_telemetry_sender_stats_take(&stats);
	abandoned_after = stats.abandoned;
	assert(abandoned_after == abandoned_before + 3);

	/* (4) SAFETY: a hard errno still goes to the classifier and still kills
	 * the stream at once. The experiment bounds waiting, it does not make a
	 * dead socket look alive. */
	hw_telemetry_send_fail_state_reset(&st);
	probe = (struct nonblock_probe) {.result = -ECONNRESET};
	bool close_socket = false;
	assert(!hw_telemetry_test_send_line_with_state(line, nonblock_probe_send,
												   &probe, &st, &close_socket));
	assert(st.fail_count == 1 && st.last_errno == -ECONNRESET);
	assert(st.quiesced && close_socket);
	hw_telemetry_sender_stats_take(&stats);
	assert(stats.abandoned == abandoned_after); /* not an abandonment */

	hw_telemetry_set_nonblock_experiment(false);
	assert(!hw_telemetry_nonblock_experiment());
	printf("PASS nonblock experiment: short writes and -EAGAIN/-EWOULDBLOCK "
		   "abandon the line (%u counted, never retried, resync owed) "
		   "without spending a transient strike, while -ECONNRESET still "
		   "quiesces and closes; unarmed, a short write still loops as "
		   "before\n",
		   abandoned_after - abandoned_before);
}

static uint32_t anchor_test_frame_us(uint32_t index) {
	/* Include values a 16-bit millisecond representation would lose. */
	return index == 13000 ? UINT32_MAX : 16000u + index * 17u;
}

static bool anchor_sample_line(const char* line, void* user) {
	struct anchor_sample_sink* sink = user;
	if(sink->reject)
		return false;
	const char* p = strstr(line, "\"anchor_frame_us\":[");
	assert(p);
	p = strchr(p, '[') + 1;
	while(*p != ']') {
		char* end;
		uint32_t value = (uint32_t)strtoul(p, &end, 10);
		assert(end != p);
		assert(value == anchor_test_frame_us(++sink->samples));
		assert(*end == ',' || *end == ']');
		p = *end == ',' ? end + 1 : end;
	}
	return true;
}

static void test_anchor_measurement_fixed_budget(void) {
	struct hw_telemetry_config cfg = {
		.enabled = true, .host = "127.0.0.1", .port = 39017,
		.run_id = "allocation_guard", .profile = "split_anchor_6m",
	};
	assert(hw_telemetry_init(&cfg));
	struct anchor_sample_sink sink = {0};
	hw_telemetry_test_set_sink(anchor_sample_line, &sink);
	struct hw_anchor_evidence a = {
		.enabled = true, .carve_load_min = -1, .carve_run_min = -1,
	};
	assert(sizeof(a.frame_us) == HW_ANCHOR_FRAME_BUDGET_BYTES);
	/* Fixed diagnostic state plus the 1024-byte batch. 1216 -> 1280 for
	 * S1-d's per-owner act0/act1 tallies and the act_health pair (+56 B,
	 * measured 1,184 -> 1,240 on this PC build); the same cap is asserted at
	 * compile time in hw_telemetry.h, whose comment carries the accounting. */
	assert(sizeof(a) <= 1280);
	assert(sizeof(a) < 64u * 1024u);
	anchor_alloc_calls = anchor_alloc_bytes = 0;
	anchor_alloc_guard = true;
	uint32_t faults[3] = {0, 30, 0};
	assert(!hw_anchor_evidence_begin(&a, false, faults));
	hw_anchor_evidence_note(&a, 0, 0, 7000);
	assert(a.carve_load_min == 7000 && a.carve_run_min == -1);
	for(unsigned i = 1; i < HW_ANCHOR_READY_HEARTBEATS; i++)
		assert(!hw_anchor_evidence_begin(&a, true, faults));
	assert(hw_anchor_evidence_begin(&a, true, faults));
	hw_anchor_evidence_note(&a, 0, 0, 6000);
	assert(a.frame_total == 0 && a.carve_run_min == 6000);
	/* A full six-minute 60 Hz window plus tail. No heartbeat drain: exercise
	 * repeated overflow streaming, the maximum on-console retention case. */
	for(uint32_t i = 1; i <= 21601; i++) {
		assert(!hw_anchor_evidence_begin(&a, (i % 7) == 0, faults));
		a.far_missing[0] = i % 49;
		hw_anchor_evidence_note(&a, i * 1000u / 60u,
			anchor_test_frame_us(i), 5000);
		assert(a.frame_count <= HW_ANCHOR_FRAME_CAP);
		assert(a.frame_total == i && a.frame_dropped == 0);
		assert(sink.samples + a.frame_count == i);
	}
	assert(hw_telemetry_emit_anchor_frames(&a));
	assert(sink.samples == 21601 && a.far_max[0] == 48);
	a.frame_count = 0; // same drain contract as the heartbeat caller
	sink.reject = true;
	for(unsigned i = 0; i <= HW_ANCHOR_FRAME_CAP; i++)
		hw_anchor_evidence_note(&a, 360100 + i, 17000, -1);
	assert(a.frame_count == 1 && a.frame_dropped == HW_ANCHOR_FRAME_CAP);
	assert(a.carve_run_min == 5000); // unavailable reading cannot erase min
	assert(a.faults_load[1] == 30 && a.faults_run[1] == 0);
	faults[0] = 1;
	faults[1] = 31;
	faults[2] = 1;
	assert(!hw_anchor_evidence_begin(&a, false, faults));
	assert(a.faults_run[0] == 1 && a.faults_run[1] == 1 && a.faults_run[2] == 1);
	assert(anchor_alloc_calls == 0 && anchor_alloc_bytes == 0);
	anchor_alloc_guard = false;
	hw_telemetry_test_set_sink(NULL, NULL);
	hw_telemetry_shutdown();
	printf("PASS anchor sep=0 measurement start + 21601 exact samples: "
		   "heap peak/requested=0 bytes, fixed evidence=%zu, samples=%zu bytes\n",
		   sizeof(a), sizeof(a.frame_us));
}

static void test_anchor_readiness_window(void) {
	uint32_t faults[3] = {0, 30, 0};
	for(unsigned anchor = 0; anchor < 2; anchor++) {
		struct hw_anchor_evidence a = {.enabled = true, .separation = 32,
			.carve_load_min = -1, .carve_run_min = -1};
		uint32_t* debts[] = {&a.missing[anchor], &a.mesh_pending[anchor],
			&a.far_missing[anchor], &a.far_mesh_pending[anchor]};
		for(unsigned k = 0; k < 4; k++) {
			assert(!hw_anchor_evidence_begin(&a, true, faults));
			assert(!hw_anchor_evidence_begin(&a, true, faults));
			*debts[k] = 1;
			assert(!hw_anchor_evidence_begin(&a, true, faults));
			assert(!a.ready && !a.measuring && a.ready_streak == 0);
			*debts[k] = 0;
		}
		hw_anchor_evidence_note(&a, 0, 0, 367);
		assert(!hw_anchor_evidence_begin(&a, true, faults));
		assert(!hw_anchor_evidence_begin(&a, true, faults));
		assert(hw_anchor_evidence_begin(&a, true, faults));
		hw_anchor_evidence_note(&a, 0, 0, 3000);
		assert(a.ready && a.measuring && a.carve_load_min == 367
			&& a.carve_run_min == 3000 && a.faults_load[1] == 30);
	}
	puts("PASS anchor: both full-column/mesh/far sets held for three heartbeats; loading excluded");
}

static void test_parse_default_and_profile(void) {
	struct hw_run_config cfg;
	char err[128];
	char* argv[] = {
		"boot.dol", "--cw-run-id", "r42", "--cw-tel-host", "192.0.2.20",
		"--cw-tel-port", "39018", "--cw-netmask", "255.255.252.0",
		"--cw-profile", "far_walk_6m",
	};
	assert(hw_run_parse_args(&cfg, 11, argv, err, sizeof(err)));
	assert(cfg.enabled);
	assert(strcmp(cfg.run_id, "r42") == 0);
	assert(strcmp(cfg.telemetry_host, "192.0.2.20") == 0);
	assert(strcmp(cfg.telemetry_netmask, "255.255.252.0") == 0);
	assert(cfg.telemetry_port == 39018);
	assert(!cfg.telemetry_pull);
	assert(cfg.profile == HW_RUN_PROFILE_FAR_WALK_6M);
	assert(strcmp(cfg.profile_name, "far_walk_6m") == 0);
	assert(cfg.duration_seconds == 360);
	assert(cfg.force_overlay);
	assert(cfg.autoload_new);
	assert(cfg.autopilot_walk);
	// task_da3f8a7b: the walker stuck-detector is opt-in per hw_run profile --
	// far_walk_6m's established hardware-evidence trajectory must stay
	// byte-identical, so it must NOT arm the detector.
	assert(!cfg.autopilot_unstick);
	assert(!cfg.force_overworld);
	assert(cfg.far_view_distance == 6);
	// Hands-free hardware batch: every armed profile defaults to auto-exit ON
	// with a few seconds of grace, unless explicitly overridden.
	assert(cfg.auto_exit);
	assert(cfg.auto_exit_grace_seconds > 0.0F);
}

static void test_parse_pull_mode_without_real_host(void) {
	struct hw_run_config cfg;
	char err[128];
	char* argv[] = {
		"boot.dol", "--cw-run-id", "r43", "--cw-tel-mode", "pull",
		"--cw-tel-port", "39017",
	};
	assert(hw_run_parse_args(&cfg, 7, argv, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.telemetry_pull);
	assert(strcmp(cfg.telemetry_host, "pull") == 0);
	assert(cfg.telemetry_port == 39017);
}

static void test_parse_rejects_missing_host(void) {
	struct hw_run_config cfg;
	char err[128];
	char* argv[] = {"boot.dol", "--cw-run-id", "r42"};
	assert(!hw_run_parse_args(&cfg, 3, argv, err, sizeof(err)));
	assert(strstr(err, "--cw-tel-host"));
}

static void test_parse_args_file(void) {
	const char* path = "test_hw_run_args.tmp";
	FILE* f = fopen(path, "wb");
	assert(f);
	fputs("--cw-run-id file_run --cw-tel-mode pull --cw-tel-port 39017 "
		  "--cw-netmask 255.255.252.0 --cw-profile heartbeat_selftest",
		  f);
	fclose(f);
	struct hw_run_config cfg;
	char err[128];
	assert(hw_run_parse_file(&cfg, path, err, sizeof(err)));
	remove(path);
	assert(cfg.enabled);
	assert(cfg.telemetry_pull);
	assert(strcmp(cfg.run_id, "file_run") == 0);
	assert(strcmp(cfg.telemetry_netmask, "255.255.252.0") == 0);
	assert(cfg.profile == HW_RUN_PROFILE_HEARTBEAT_SELFTEST);
	assert(cfg.superflat);
	assert(cfg.force_overworld);
}

static void test_parse_redstone_clock_profile(void) {
	struct hw_run_config cfg;
	char err[128];
	char* argv[] = {
		"boot.dol", "--cw-run-id", "rs_run", "--cw-tel-mode", "pull",
		"--cw-profile", "redstone_clock_2m",
	};
	assert(hw_run_parse_args(&cfg, 7, argv, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_REDSTONE_CLOCK_2M);
	assert(strcmp(cfg.profile_name, "redstone_clock_2m") == 0);
	assert(cfg.duration_seconds == 120);
	assert(cfg.superflat);
	assert(cfg.force_overworld);
	assert(cfg.autopilot_aim);
	assert(cfg.autopilot_pitch_deg == 108);
	assert(cfg.redstone_clock_stress);
	assert(cfg.far_view_distance == -1);
}

static void test_parse_menus_dwell_profile(void) {
	struct hw_run_config cfg;
	char err[128];
	char* argv[] = {
		"boot.dol", "--cw-run-id", "md_run", "--cw-tel-mode", "pull",
		"--cw-profile", "menus_dwell",
	};
	assert(hw_run_parse_args(&cfg, 7, argv, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_MENUS_DWELL);
	assert(strcmp(cfg.profile_name, "menus_dwell") == 0);
	assert(cfg.duration_seconds == 240);
	assert(cfg.superflat);
	assert(cfg.force_overworld);
	// The ONE profile that must NOT auto-skip the menus -- this is the whole
	// point of menus_dwell (see hw_run.c's case comment).
	assert(!cfg.autoload_new);
	assert(!cfg.mp_autoconnect);
	assert(cfg.autopilot_aim);
	assert(cfg.autopilot_pitch_deg == 108);
	assert(cfg.autopilot_yaw_deg == 0);
	assert(cfg.autopilot_place);
	assert(cfg.menu_autopilot);
	assert(!cfg.redstone_clock_stress);
	assert(!cfg.watchdog_inject);
	assert(cfg.far_view_distance == -1);

	// mp_connect_smoke (S5.3): the connect screen is the boot target and no
	// autoload rides with it -- a loaded world can only be the remote
	// install (the collector's scenario discriminator relies on this pair).
	char* argv_mp[] = {
		"boot.dol", "--cw-run-id", "mp_run", "--cw-tel-mode", "pull",
		"--cw-profile", "mp_connect_smoke",
	};
	memset(&cfg, 0, sizeof(cfg));
	assert(hw_run_parse_args(&cfg, 7, argv_mp, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_MP_CONNECT_SMOKE);
	assert(strcmp(cfg.profile_name, "mp_connect_smoke") == 0);
	assert(cfg.duration_seconds == 120);
	assert(cfg.mp_autoconnect);
	assert(!cfg.autoload_new);
	assert(!cfg.menu_autopilot);
	assert(!cfg.autopilot_aim);
	assert(!cfg.autopilot_place);
	assert(!cfg.watchdog_inject);
	// task_da3f8a7b: mp_connect_smoke doesn't even arm autopilot_walk, so it
	// must not arm the stuck-detector either.
	assert(!cfg.autopilot_unstick);
	assert(cfg.far_view_distance == -1);

	// mp_lan_walk_10m: the mp_lan_walk_10m/>3s-stall-hunt profile -- arms
	// BOTH mp_connect_smoke's connect-screen flow (no autoload) AND
	// far_walk_6m's forced-walk autopilot, so remote chunks stream/install
	// under continuous movement for the full 600s window.
	char* argv_mpw[] = {
		"boot.dol", "--cw-run-id", "mpw_run", "--cw-tel-mode", "pull",
		"--cw-profile", "mp_lan_walk_10m",
	};
	memset(&cfg, 0, sizeof(cfg));
	assert(hw_run_parse_args(&cfg, 7, argv_mpw, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_MP_LAN_WALK_10M);
	assert(strcmp(cfg.profile_name, "mp_lan_walk_10m") == 0);
	assert(cfg.duration_seconds == 600);
	assert(cfg.mp_autoconnect);
	assert(cfg.autopilot_walk);
	// task_da3f8a7b: mp_lan_walk_10m is the ONLY profile that arms the
	// walker stuck-detector (the >3s terrain-pin hunt this profile exists
	// for -- see hw_run.c's case comment).
	assert(cfg.autopilot_unstick);
	assert(!cfg.autoload_new);
	assert(!cfg.autopilot_aim);
	assert(!cfg.autopilot_place);
	assert(!cfg.menu_autopilot);
	assert(!cfg.watchdog_inject);
	assert(!cfg.redstone_clock_stress);
	assert(!cfg.save_migrate);
	assert(cfg.far_view_distance == -1);

	// The "mp_lan_walk" alias resolves to the same profile.
	enum hw_run_profile alias;
	assert(hw_run_profile_from_name("mp_lan_walk", &alias));
	assert(alias == HW_RUN_PROFILE_MP_LAN_WALK_10M);

	// mp_reconnect_soak (S5.3 hunt plumbing): the third profile docs/mp_s5_
	// wii_transport_design_2026-08-26.md 7b proposed alongside mp_connect_
	// smoke/mp_lan_walk_10m -- arms the SAME boot-time connect entry
	// (mp_autoconnect) as both of those, plus mp_reconnect_soak (the
	// cycle-driver arm flag), but neither the forced-walk autopilot nor its
	// stuck-detector -- this profile stays put and cycles the connection,
	// it does not walk.
	char* argv_mrs[] = {
		"boot.dol", "--cw-run-id", "mrs_run", "--cw-tel-mode", "pull",
		"--cw-profile", "mp_reconnect_soak",
	};
	memset(&cfg, 0, sizeof(cfg));
	assert(hw_run_parse_args(&cfg, 7, argv_mrs, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_MP_RECONNECT_SOAK);
	assert(strcmp(cfg.profile_name, "mp_reconnect_soak") == 0);
	assert(cfg.duration_seconds == 480);
	assert(cfg.mp_autoconnect);
	assert(cfg.mp_reconnect_soak);
	assert(!cfg.autoload_new);
	assert(!cfg.autopilot_walk);
	assert(!cfg.autopilot_unstick);
	assert(!cfg.autopilot_aim);
	assert(!cfg.autopilot_place);
	assert(!cfg.menu_autopilot);
	assert(!cfg.watchdog_inject);
	assert(!cfg.redstone_clock_stress);
	assert(!cfg.save_migrate);
	assert(cfg.far_view_distance == -1);

	// The "mp_reconnect" alias resolves to the same profile.
	enum hw_run_profile mrs_alias;
	assert(hw_run_profile_from_name("mp_reconnect", &mrs_alias));
	assert(mrs_alias == HW_RUN_PROFILE_MP_RECONNECT_SOAK);
}

static void test_parse_save_migrate_profile(void) {
	struct hw_run_config cfg;
	char err[128];
	char* argv[] = {
		"boot.dol", "--cw-run-id", "sm_run", "--cw-tel-mode", "pull",
		"--cw-profile", "save_migrate", "--cw-save-migrate-regions", "12",
	};
	assert(hw_run_parse_args(&cfg, 9, argv, err, sizeof(err)));
	assert(cfg.enabled);
	assert(cfg.profile == HW_RUN_PROFILE_SAVE_MIGRATE);
	assert(strcmp(cfg.profile_name, "save_migrate") == 0);
	assert(cfg.duration_seconds == 300);
	assert(cfg.superflat);
	assert(cfg.force_overworld);
	assert(cfg.autoload_new);
	assert(cfg.save_migrate);
	assert(cfg.save_migrate_regions == 12);
	assert(cfg.far_view_distance == -1);

	char* bad_argv[] = {
		"boot.dol", "--cw-run-id", "sm_bad", "--cw-tel-mode", "pull",
		"--cw-profile", "save_migrate", "--cw-save-migrate-regions", "1",
	};
	assert(!hw_run_parse_args(&cfg, 9, bad_argv, err, sizeof(err)));
	assert(strstr(err, "--cw-save-migrate-regions"));
}

static void test_parse_auto_exit_flags(void) {
	// --cw-auto-exit off: a human-observed armed run can still linger on
	// purpose after profile_done (task-required override).
	struct hw_run_config cfg_off;
	char err[128];
	char* argv_off[] = {
		"boot.dol", "--cw-run-id", "r1", "--cw-tel-mode", "pull",
		"--cw-auto-exit", "off",
	};
	assert(hw_run_parse_args(&cfg_off, 7, argv_off, err, sizeof(err)));
	assert(cfg_off.enabled);
	assert(!cfg_off.auto_exit);

	// --cw-auto-exit on (also exercises "1"/"true" spellings elsewhere) plus
	// a custom grace window.
	struct hw_run_config cfg_on;
	char* argv_on[] = {
		"boot.dol", "--cw-run-id", "r2", "--cw-tel-mode", "pull",
		"--cw-auto-exit", "1", "--cw-exit-grace", "12.5",
	};
	assert(hw_run_parse_args(&cfg_on, 9, argv_on, err, sizeof(err)));
	assert(cfg_on.enabled);
	assert(cfg_on.auto_exit);
	assert(fabsf(cfg_on.auto_exit_grace_seconds - 12.5F) < 0.001F);

	struct hw_run_config cfg_bad;
	char* argv_bad[] = {
		"boot.dol", "--cw-run-id", "r3", "--cw-tel-mode", "pull",
		"--cw-auto-exit", "sideways",
	};
	assert(!hw_run_parse_args(&cfg_bad, 7, argv_bad, err, sizeof(err)));
	assert(strstr(err, "--cw-auto-exit"));

	struct hw_run_config cfg_bad_grace;
	char* argv_bad_grace[] = {
		"boot.dol", "--cw-run-id", "r4", "--cw-tel-mode", "pull",
		"--cw-exit-grace", "-1",
	};
	assert(!hw_run_parse_args(&cfg_bad_grace, 7, argv_bad_grace, err,
							  sizeof(err)));
	assert(strstr(err, "--cw-exit-grace"));
}

static void test_auto_exit_ready_gates_armed_runs_only(void) {
	// The literal cw_run_args.txt-armed scenario from the task: heartbeat_selftest.
	struct hw_run_config armed;
	hw_run_apply_profile(&armed, HW_RUN_PROFILE_HEARTBEAT_SELFTEST);
	armed.enabled = true;
	assert(armed.auto_exit);
	assert(armed.auto_exit_grace_seconds == 5.0F);

	// Before profile_done was ever sent: never ready, no matter how much
	// (garbage) time is passed in -- this is the "keeps ticking forever
	// today" state the fix targets.
	assert(!hw_run_auto_exit_ready(&armed, false, 999.0F));
	// profile_done just sent (0s elapsed): still inside the grace window.
	assert(!hw_run_auto_exit_ready(&armed, true, 0.0F));
	assert(!hw_run_auto_exit_ready(&armed, true, 4.99F));
	// Grace window elapsed: now it's safe to exit.
	assert(hw_run_auto_exit_ready(&armed, true, 5.0F));
	assert(hw_run_auto_exit_ready(&armed, true, 999.0F));

	// Hard gate: normal/manual play is NEVER armed (hw_run_config_init's
	// documented default), so even a "profile_done_sent=true, huge elapsed"
	// call can never auto-exit an ordinary play session.
	struct hw_run_config normal_play;
	hw_run_config_init(&normal_play);
	assert(!normal_play.enabled);
	assert(!hw_run_auto_exit_ready(&normal_play, true, 999.0F));

	// A human explicitly asked an armed run to linger (--cw-auto-exit off):
	// never auto-exit, ever, at any elapsed time.
	struct hw_run_config lingering = armed;
	lingering.auto_exit = false;
	assert(!hw_run_auto_exit_ready(&lingering, true, 999.0F));

	// watchdog_inject: the profile whose entire point is a deliberate,
	// non-returning stall to exercise the watchdog dump path. Even if
	// something upstream mistakenly reported profile_done_sent for it, this
	// gate must still refuse -- auto-exiting would defeat the hang test.
	struct hw_run_config wd;
	hw_run_apply_profile(&wd, HW_RUN_PROFILE_WATCHDOG_INJECT);
	wd.enabled = true;
	assert(wd.watchdog_inject);
	assert(!hw_run_auto_exit_ready(&wd, true, 999.0F));

	// NULL-safety.
	assert(!hw_run_auto_exit_ready(NULL, true, 999.0F));
}

// push-0921/C1: an armed run's duration is reached on the INTEGER MILLISECOND
// the run reports, never on a float-seconds sample taken from a different
// timestamp. main.c used to end a run the first frame its float `elapsed`
// crossed duration_seconds, while the heartbeat it then emitted carried an
// integer "anchor_ms" sampled EARLIER in the same frame -- real-Wii
// split_play_8m run 0917_092103 sent "profile_done ok" behind a final
// heartbeat reporting 359999 ms and the collector failed it on its 360000 gate
// ("need six measured minutes after installed readiness",
// tools/wii_telemetry_server.py). The decision now lives in a pure function,
// so the boundary is pinned here instead of only on a console.
static void test_duration_reached_is_integer_ms(void) {
	// The exact boundary triple that produced the failure. NOTE (review
	// claim 5): this triple alone does NOT prove "integer, not float" -- a
	// float implementation ((float)elapsed_ms / 1000.0F >= (float)duration)
	// answers 359999 -> false and 360000 -> true just as well, because both
	// values are exactly representable. The arms that separate the two are
	// the large ones below, where float's 24-bit mantissa rounds an
	// elapsed_ms UP across the boundary; and what proves main.c *uses* the
	// integer is tests/test_anchor_measurement_clock.py's one-millisecond
	// skew scenario, where the float seconds say 360.000 while the published
	// integer still says 359999.
	assert(!hw_run_duration_reached_ms(359999, 360));
	assert(hw_run_duration_reached_ms(360000, 360));
	assert(hw_run_duration_reached_ms(360001, 360));
	assert(!hw_run_duration_reached_ms(0, 360));

	// The separation, at the smallest magnitude where it exists: above
	// 2^24 ms (16777216, ~4.66 h) consecutive millisecond counts are no
	// longer representable as float, and 16777999 rounds up to 16778000 --
	// so a float implementation completes this run a millisecond early while
	// the integer one refuses. Same divergence, larger, at the two overflow
	// arms further down (2147483999 and 4294966999 both round up).
	assert(!hw_run_duration_reached_ms(16777999u, 16778));
	assert(hw_run_duration_reached_ms(16778000u, 16778));

	// The same triple for EVERY registered profile's own duration, so no
	// profile can drift off the contract unnoticed.
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	uint32_t longest_ms = 0;
	for(int profile = HW_RUN_PROFILE_FAR_IDLE_2M;
		profile <= HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M; profile++) {
		hw_run_apply_profile(&cfg, (enum hw_run_profile)profile);
		assert(cfg.duration_seconds > 0);
		uint32_t exact = (uint32_t)cfg.duration_seconds * 1000u;
		if(exact > longest_ms)
			longest_ms = exact;
		assert(!hw_run_duration_reached_ms(exact - 1, cfg.duration_seconds));
		assert(hw_run_duration_reached_ms(exact, cfg.duration_seconds));
		assert(hw_run_duration_reached_ms(exact + 1, cfg.duration_seconds));
	}
	// mp_play_10m, the table's longest since push-0921/B4c -- 1260000 ms
	// against a uint32 counter that reaches 4294967295 (~49.7 days), so the
	// 21-minute window is nowhere near it. (It was mp_lan_walk_10m's 600000
	// before that profile landed.) Keep in step with hw_run_apply_profile if
	// a longer profile is ever added.
	assert(longest_ms == 1260000);

	// "No duration timer": the same refusal the `duration_seconds > 0` guard
	// in main.c made for the driver-owned/watchdog profiles. A negative value
	// can only be a caller bug and must never complete a run either.
	assert(!hw_run_duration_reached_ms(0, 0));
	assert(!hw_run_duration_reached_ms(UINT32_MAX, 0));
	assert(!hw_run_duration_reached_ms(UINT32_MAX, -1));
	assert(!hw_run_duration_reached_ms(UINT32_MAX, INT_MIN));

	// --cw-duration accepts any positive int, so the product must be formed
	// in 64 bits. 2147484 s * 1000 = 2147484000 overflows a SIGNED 32-bit int
	// and wraps to -2147483296, which a naive `elapsed_ms >= duration * 1000`
	// would satisfy immediately -- so this duration must still answer false
	// at zero and just below it, and true only on the real boundary. (24.8
	// days is nonsense for a hardware run; a run-arg typo is how it arrives.)
	assert(!hw_run_duration_reached_ms(0, 2147484));
	assert(!hw_run_duration_reached_ms(2147483999u, 2147484));
	assert(hw_run_duration_reached_ms(2147484000u, 2147484));
	// Past what a uint32 millisecond counter can express at all (4294967295
	// ms ~ 49.7 days): unreachable, and it must say so instead of wrapping.
	assert(!hw_run_duration_reached_ms(UINT32_MAX, 4294968));
	assert(!hw_run_duration_reached_ms(UINT32_MAX, INT_MAX));
	// The largest duration the counter CAN express still works normally.
	assert(hw_run_duration_reached_ms(UINT32_MAX, 4294967));
	assert(!hw_run_duration_reached_ms(4294966999u, 4294967));

	puts("PASS run completion is decided on the emitted integer millisecond "
		 "(just-below/exact/just-above per profile; no timer at <= 0; no "
		 "32-bit overflow at any --cw-duration)");
}

static void test_telemetry_dhcp_retry_bound_is_short(void) {
	/*
		This is a compile-time guard for the Wii-only if_config() path. The PC
		test cannot exercise libogc DHCP, but it can prevent the old long retry
		count from silently returning and hiding early boot freezes behind a
		black screen before CaveWii's own proof paths are alive.
	*/
	assert(HW_TELEMETRY_DHCP_MAX_RETRIES == 3);
	assert(HW_TELEMETRY_DHCP_MAX_RETRIES < 20);
}

// hw_telemetry_retry_bringup (Batch E item 5, 2026-08-23): the OUTER retry
// budget wrapped around hw_telemetry_init in main.c. Deliberately independent
// of the DHCP/network internals above -- it is exercised here purely with
// fakes, no real time or sleep involved (elapsed is the SUM of the fake
// sleep_ms already "spent", so the budget math is exact and deterministic).
struct retry_fake_ctx {
	int fail_count; // attempt() fails this many times, then succeeds
	int attempts;   // attempt() calls actually made
	int sleeps;     // sleep_fn() calls actually made
	uint32_t last_sleep_ms;
};

static bool retry_fake_attempt(void* user) {
	struct retry_fake_ctx* ctx = user;
	ctx->attempts++;
	return ctx->attempts > ctx->fail_count;
}

static void retry_fake_sleep(uint32_t ms, void* user) {
	struct retry_fake_ctx* ctx = user;
	ctx->sleeps++;
	ctx->last_sleep_ms = ms;
}

static void test_retry_bringup_succeeds_first_try(void) {
	struct retry_fake_ctx ctx = {.fail_count = 0};
	int attempts = -1;
	bool ok = hw_telemetry_retry_bringup(retry_fake_attempt, retry_fake_sleep,
										 &ctx, 30000u, 2000u, &attempts);
	assert(ok);
	assert(attempts == 1);
	assert(ctx.sleeps == 0); // never slept -- the first try worked
}

static void test_retry_bringup_succeeds_after_failures(void) {
	struct retry_fake_ctx ctx = {.fail_count = 3};
	int attempts = -1;
	bool ok = hw_telemetry_retry_bringup(retry_fake_attempt, retry_fake_sleep,
										 &ctx, 30000u, 2000u, &attempts);
	assert(ok);
	assert(attempts == 4); // 3 failures + the succeeding call
	assert(ctx.sleeps == 3); // one sleep between each pair of attempts
	assert(ctx.last_sleep_ms == 2000u);
}

static void test_retry_bringup_exhausts_budget(void) {
	// fail_count set far beyond what the budget can cover -- must give up,
	// never loop forever. budget_ms=3000, sleep_ms=1000: elapsed reaches
	// exactly 3000 after 3 sleeps, so the 4th attempt is the last one made.
	struct retry_fake_ctx ctx = {.fail_count = 1000};
	int attempts = -1;
	bool ok = hw_telemetry_retry_bringup(retry_fake_attempt, retry_fake_sleep,
										 &ctx, 3000u, 1000u, &attempts);
	assert(!ok);
	assert(attempts == 4);
	assert(ctx.sleeps == 3);
}

static void test_retry_bringup_null_attempt_is_safe(void) {
	int attempts = -1;
	bool ok = hw_telemetry_retry_bringup(NULL, retry_fake_sleep, NULL, 30000u,
										 2000u, &attempts);
	assert(!ok);
	assert(attempts == 0);
}

static void test_retry_bringup_zero_sleep_ms_never_retries(void) {
	// sleep_ms == 0 must not spin forever waiting on an elapsed clock that
	// never advances -- treated as "no retry budget", one attempt only.
	struct retry_fake_ctx ctx = {.fail_count = 1000};
	int attempts = -1;
	bool ok = hw_telemetry_retry_bringup(retry_fake_attempt, retry_fake_sleep,
										 &ctx, 30000u, 0u, &attempts);
	assert(!ok);
	assert(attempts == 1);
	assert(ctx.sleeps == 0);
}

static void test_telemetry_init_rejects_out_of_range_ports(void) {
	struct hw_telemetry_config cfg = {
		.enabled = true,
		.host = "pull",
		.port = 65536,
		.pull = true,
	};
	assert(!hw_telemetry_init(&cfg));
	assert(!hw_telemetry_active());
	cfg.port = 0;
	assert(!hw_telemetry_init(&cfg));
	assert(!hw_telemetry_active());
}

static void test_heartbeat_json_escapes_strings(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abc+_06.27",
		.tick = 7,
		.fps = 59.5f,
		.gpu_ms = 0.0f,
		.vsync_ms = 16.6f,
		.ms_bfs = 1.25f,
		.mesh_pending = 2,
		.srv_rt_us = 12,
		.srv_wt_us = 34,
		.x = 1.0f,
		.y = 2.0f,
		.z = 3.0f,
		.near_radius = 2,
		.far_view_distance = 6,
		.far_ring_width = 1,
		.far_missing = 3,
		.far_missing_max = 9,
		.far_settle_ms = 4321,
		.srv_gen_us = 555,
		.srv_gen_cols = 2,
		.srv_gen_backlog = 14,
		.cflood = 123,
		.notify_dispatch_total = 77,
		.wd_hb_gap_max_ms = 1234,
		.world_loaded = true,
		.save_migrate_state = 4,
		.save_migrate_ms = 47001,
		.save_migrate_regions = 12,
		.save_migrate_chunks = 44,
		.save_migrate_signs = 32,
		.save_migrate_expected = 32,
		.save_migrate_verified = 32,
		.save_migrate_chests_rewritten = 12,
		.save_migrate_chests_expected = 12,
		.save_migrate_chests_verified = 12,
		.save_migrate_spawners_synthesized = 12,
		.save_migrate_spawners_expected = 12,
		.save_migrate_spawners_verified = 12,
		.save_fail_n = 3,
		.save_fail_stage = 7,
		.save_fail_sub = 8,
		.save_skip_n = 41,
		.save_idx_fb = 5,
	};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(
		buf, sizeof(buf), "run\"id", "far_idle_2m", &hb);
	assert(n < sizeof(buf));
	/* lava-aabb-0924/H1: the four chunk-save failure keys, in order. */
	assert(strstr(buf, "\"save_fail_n\":3,\"save_fail_stage\":7,"
						"\"save_fail_sub\":8,\"save_skip_n\":41,"
						"\"save_idx_fb\":5,"));
	assert(strstr(buf, "\"event\":\"heartbeat\""));
	assert(strstr(buf, "\"run_id\":\"run\\\"id\""));
	assert(strstr(buf, "\"build_id\":\"abc+_06.27\""));
	assert(strstr(buf, "\"ms_bfs\":1.25"));
	assert(strstr(buf, "\"world_loaded\":true"));
	assert(strstr(buf, "\"cflood\":123"));
	assert(strstr(buf, "\"fring\":1"));
	assert(strstr(buf, "\"far_missing\":3"));
	assert(strstr(buf, "\"far_missing_max\":9"));
	assert(strstr(buf, "\"far_settle_ms\":4321"));
	assert(strstr(buf, "\"srv_gen_us\":555"));
	assert(strstr(buf, "\"srv_gen_cols\":2"));
	assert(strstr(buf, "\"srv_gen_backlog\":14"));
	assert(strstr(buf, "\"ndisp\":77"));
	assert(strstr(buf, "\"wd_hb_gap_max_ms\":1234"));
	assert(strstr(buf, "\"smig_ms\":47001"));
	assert(strstr(buf, "\"smig_regions\":12"));
	assert(strstr(buf, "\"smig_signs\":32"));
	assert(strstr(buf, "\"smig_verified\":32"));
	assert(strstr(buf, "\"smig_chests\":12"));
	assert(strstr(buf, "\"smig_chests_expected\":12"));
	assert(strstr(buf, "\"smig_chests_verified\":12"));
	assert(strstr(buf, "\"smig_spawners\":12"));
	assert(strstr(buf, "\"smig_spawners_expected\":12"));
	assert(strstr(buf, "\"smig_spawners_verified\":12"));
}

static bool g_worst_heartbeat_only;

/* G9 R6 v2 (ASTRA-G9-REVIEW-2 item 5): the light-queue array serializes
 * every element in its documented position (hw_telemetry.h), between
 * "ndisp" and "wd_main", as plain decimal; a zero heartbeat carries the
 * 26 zeros (never omitted). */
static void test_heartbeat_light_queue_v2_serialization(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	hw_telemetry_format_heartbeat(buf, sizeof buf, "r", "far_idle_2m", &hb);
	assert(strstr(buf, "\"ndisp\":0,\"lq\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
		"0,0,0,0,0,0,0,0,0,0,0],\"lqd_v\":3,\"lqd\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
		"0,0,0,0],\"wd_main\":"));
	for(int i = 0; i < HW_TELEMETRY_LQ_N; i++)
		hb.lq[i] = (uint16_t)(i == 25 ? 65535 : 100 * i + 7);
	/* I07: the unsaturated per-drain array follows lq, 14 positional uint32
	 * (hw_telemetry.h fixes the order); a 1,085,289 us drain of 1,000,000
	 * pops is carried exactly, never clipped. */
	/* R5: v2 adds the rt/wt window maxima (14/15), the checkpoint split
	 * (16-20) and the RPC wait sum (21); the 1,127,127 us spike tick of g9_1
	 * is carried exactly at 15. */
	for(int i = 0; i < HW_TELEMETRY_LQD_N; i++)
		hb.lqd[i] = (uint32_t)(i == 21 ? 4294967295u : i == 15 ? 1127127u
			: i == 6 ? 1085289u : i == 2 ? 1000000u : 1000 * i + 3);
	hw_telemetry_format_heartbeat(buf, sizeof buf, "r", "far_idle_2m", &hb);
	assert(strstr(buf, "\"lq\":[7,107,207,307,407,507,607,707,807,907,1007,"
		"1107,1207,1307,1407,1507,1607,1707,1807,1907,2007,2107,2207,2307,"
		"2407,65535],\"lqd_v\":3,\"lqd\":[3,1003,1000000,3003,4003,5003,1085289,7003,"
		"8003,9003,10003,11003,12003,13003,14003,1127127,16003,17003,18003,"
		"19003,20003,4294967295],\"wd_main\":"));
	printf("PASS heartbeat lq v2: 26 positional saturating uint16; lqd v2: "
		   "22 positional unsaturated uint32\n");
}

/* ci-1101/37: the drain long-wait attribution. Emitted as
 * "lqd_w":[wait_us,mutex,holder_id,holder_tag,stripe,"name"] ONLY when
 * lqd_wait_present, so every quiet heartbeat keeps its exact bytes (and every
 * older tape keeps parsing). The name is JSON-escaped like every other string. */
static void test_heartbeat_lqd_wait_attribution(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	hw_telemetry_format_heartbeat(buf, sizeof buf, "r", "perf_stress_5m", &hb);
	assert(!strstr(buf, "lqd_w")); /* quiet window: no key at all */
	hb.lqd_wait_present = true;
	hb.lqd_wait_us = 71206;
	hb.lqd_wait_mutex = 0x1f4a3c08u;
	hb.lqd_wait_holder_id = 0xdeadbeefu;
	hb.lqd_wait_holder_tag = 3; /* MESHER */
	hb.lqd_wait_stripe = 1;
	snprintf(hb.lqd_wait_name, sizeof hb.lqd_wait_name, "column_backing");
	hw_telemetry_format_heartbeat(buf, sizeof buf, "r", "perf_stress_5m", &hb);
	assert(strstr(buf, "\"lqd_w\":[71206,524958728,3735928559,3,1,"
		"\"column_backing\"]"));
	/* An empty name (unregistered stripe) still emits the key with "". */
	hb.lqd_wait_stripe = 0;
	hb.lqd_wait_name[0] = '\0';
	hw_telemetry_format_heartbeat(buf, sizeof buf, "r", "perf_stress_5m", &hb);
	assert(strstr(buf, "\"lqd_w\":[71206,524958728,3735928559,3,0,\"\"]"));
	printf("PASS heartbeat lqd_w: drain long-wait attribution, present-only\n");
}

static void test_heartbeat_production_buffer_fits_full_payload(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abcdef0+_12.31",
		.tick = 4294967295u,
		.fps = 59.99f,
		.gpu_ms = 12345.67f,
		.vsync_ms = 12345.67f,
		.ms_bfs = 12345.67f,
		.ms_light = 12345.67f,
		.ms_lod = 12345.67f,
		.ms_build = 12345.67f,
		.ms_recv = 12345.67f,
		.ms_render = 12345.67f,
		.ms_tick = 12345.67f,
		.ms_bfs_max = 12345.67f,
		.ms_light_max = 12345.67f,
		.ms_lod_max = 12345.67f,
		.ms_build_max = 12345.67f,
		.ms_mesh_worker = 12345.67f,
		.ms_mesh_worker_max = 12345.67f,
		.chunks_rendered = 32767,
		.mesh_pending = 32767,
		.mesh_stale = 4294967295u,
		.mesh_trunc = 4294967295u,
		.evict_stuck = 4294967295u,
		.near_evict_defer = 4294967295u,
		.srv_rt_us = 2147483647,
		.srv_wt_us = 2147483647,
		.item_nbt_exhaustions = UINT32_MAX,
		.heap_used_kb = 2147483647,
		.heap_free_kb = 2147483647,
		.heap_blocks = 2147483647,
		.heap_top_kb = 2147483647,
		.mem1_free_kb = 2147483647,
		.mem2_free_kb = 2147483647,
		.mem2_spill = 4294967295u,
		.mem2_oom = 4294967295u,
		.dlbad = 4294967295u,
		.dlcall = 4294967295u,
		.mem2_lock_wait = 4294967295u,
		.x = -123456.75f,
		.y = 255.25f,
		.z = 123456.75f,
		.rx_deg = -180.0f,
		.ry_deg = 90.0f,
		.near_radius = 99,
		.far_view_distance = 99,
		.far_ring_width = 99,
		.far_missing = 2147483647,
		.far_missing_max = 2147483647,
		.far_settle_ms = 4294967295u,
		.srv_gen_us = 2147483647,
		.srv_gen_cols = 2147483647,
		.srv_gen_backlog = 2147483647,
		.cflood = 4294967295u,
		.notify_dispatch_total = 4294967295u,
		/* G9 R6 v2: every light-queue element at its widest (saturated
		 * uint16, set below) */
		.wd_main = 14,
		.wd_srv = 9,
		.wd_worker = 3,
		.wd_hb_gap_max_ms = 4294967295u,
		.world_loaded = true,
		.menu_ap_step = 2147483647,
		.menu_ap_max_step = 2147483647,
		.menu_ap_sign_reached = true,
		.save_migrate_state = UINT32_MAX,
		.save_migrate_ms = UINT32_MAX,
		.save_migrate_regions = UINT32_MAX,
		.save_migrate_chunks = UINT32_MAX,
		.save_migrate_signs = UINT32_MAX,
		.save_migrate_expected = UINT32_MAX,
		.save_migrate_verified = UINT32_MAX,
		.save_migrate_chests_rewritten = UINT32_MAX,
		.save_migrate_chests_expected = UINT32_MAX,
		.save_migrate_chests_verified = UINT32_MAX,
		.save_migrate_spawners_synthesized = UINT32_MAX,
		.save_migrate_spawners_expected = UINT32_MAX,
		.save_migrate_spawners_verified = UINT32_MAX,
		.save_fail_n = UINT32_MAX,
		.save_fail_stage = UINT32_MAX,
		.save_fail_sub = UINT32_MAX,
		.save_skip_n = UINT32_MAX,
		.save_idx_fb = UINT32_MAX,
		.music_key = UINT32_MAX,
		.music_ms = UINT32_MAX,
		.music_underruns = UINT32_MAX,
		.music_starts = UINT32_MAX,
		.music_rejects = UINT32_MAX,
		.music_dur = UINT32_MAX,
		.mp_reconnect_cycles = UINT32_MAX,
		.mp_reconnect_target = UINT32_MAX,
		.mp_reconnect_wedge = true,
		.creative = true,
		.tel_fail = UINT32_MAX,
		.tel_last_errno = INT_MIN,
		.tel_quiesced = true,
		/* Every sender-stats field at ITS OWN widest: the eleven uint32s at
		 * UINT32_MAX, and last_poll_ret/last_send_ret at INT32_MIN, whose
		 * "-2147483648" is one char wider than any unsigned. `stage` is the
		 * one exception that is NOT its type's max: the formatter masks it
		 * to the sender's 3 bits (see the mask comment there), so 7 IS its
		 * widest reachable wire value. */
		.tel = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
			UINT32_MAX, UINT32_MAX, 7u, UINT32_MAX, UINT32_MAX, UINT32_MAX,
			UINT32_MAX, INT32_MIN, UINT32_MAX, INT32_MIN},
		.frame_counter = UINT32_MAX,
		.mem2_carve_free_kb = INT_MIN,
		/* S-A3 at ITS widest: the two flags are bool (one digit each by
		 * type), split_toggle_ms is a saturated uint32, and split_last_ok is
		 * fed INT_MIN on purpose -- the formatter clamps it to its -1/0/1
		 * domain, so "widest" for that key is 2 characters and this asserts
		 * the clamp instead of merely trusting the comment. */
		.split_live = true,
		.split_pending = true,
		.split_toggle_ms = UINT32_MAX,
		.split_last_ok = INT_MIN,
		.chunks_meshed = UINT32_MAX,
		.near_missing = 2147483647,
		.near_mesh_pending = 2147483647,
		.near_missing_max = 2147483647,
		.near_settle_ms = UINT32_MAX,
		.edit_to_visible_frames = UINT32_MAX,
		.load_screen_ms = UINT32_MAX,
		.player_health = INT_MIN,
		.loading_screen = true,
		.frame_evidence = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
			UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT64_MAX,
			UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
		.far_storage = INT_MAX,
		.server_time = UINT32_MAX,
		.storage_enabled = true,
		.op = UINT32_MAX,
		.oph = UINT32_MAX,
		.opc = UINT32_MAX,
		.opf = UINT32_MAX,
		.oer = UINT32_MAX,
		.orc = UINT32_MAX,
		.ocy = UINT32_MAX,
		.oex = UINT32_MAX,
		.owu = UINT32_MAX,
		.db = UINT32_MAX,
		.ds = UINT32_MAX,
		.dln = UINT32_MAX,
		.dls = UINT32_MAX,
		.dlp = UINT32_MAX,
		.dlc = 1,
	};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	for(size_t i = 0; i < sizeof(hb.server_work_us) / sizeof(hb.server_work_us[0]); i++)
		hb.server_work_us[i] = UINT32_MAX;
	for(int i = 0; i < 20; i++) hb.frame_evidence.worst_parts_us[i] = UINT32_MAX;
	for(int i = 0; i < 14; i++) hb.world_work_us[i] = UINT32_MAX;
	hb.guard_scan_us[0] = hb.guard_scan_us[1] = UINT32_MAX;
	for(int i = 0; i < HW_TELEMETRY_LQ_N; i++)
		hb.lq[i] = UINT16_MAX;
	for(int i = 0; i < HW_TELEMETRY_LQD_N; i++)
		hb.lqd[i] = UINT32_MAX; /* G9: all measured fields unsaturated */
	hb.lqd[12] = 3; hb.lqd[13] = 0; /* v3 reserved schema slots */
	size_t n = hw_telemetry_format_heartbeat(
		buf, sizeof(buf),
		"123456789012345678901234567890123456789012345678901234567890123",
		"perf_stress_5m", &hb);
	fprintf(stderr, "max heartbeat bytes: %zu / %zu\n", n, sizeof(buf));
	assert(n < sizeof(buf));
	assert(strlen(buf) == n);
	assert(strstr(buf, "\"ms_lod\":12345.67"));
	assert(strstr(buf, "\"ms_build_max\":12345.67"));
	assert(strstr(buf, "\"mesh_trunc\":4294967295"));
	assert(strstr(buf, "\"near_evict_defer\":4294967295"));
	assert(strstr(buf, "\"hblk\":2147483647"));
	assert(strstr(buf, "\"htop_kb\":2147483647"));
	assert(strstr(buf, "\"fring\":99"));
	assert(strstr(buf, "\"far_missing\":2147483647"));
	assert(strstr(buf, "\"far_missing_max\":2147483647"));
	assert(strstr(buf, "\"far_settle_ms\":4294967295"));
	assert(strstr(buf, "\"srv_gen_us\":2147483647"));
	assert(strstr(buf, "\"srv_gen_cols\":2147483647"));
	assert(strstr(buf, "\"srv_gen_backlog\":2147483647"));
	assert(strstr(buf, "\"ndisp\":4294967295"));
	/* G9 R6 v2: the 26-entry light-queue array, every element at its
	 * widest (65535, the saturated uint16) */
	assert(strstr(buf, "\"ndisp\":4294967295,\"lq\":[65535,65535,65535,"
		"65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,"
		"65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,"
		"65535],\"lqd_v\":3,\"lqd\":["
		"4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,3,0,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295],"
		"\"wd_main\""));
	assert(strstr(buf, "\"wd_main\":14"));
	assert(strstr(buf, "\"wd_srv\":9"));
	assert(strstr(buf, "\"wd_worker\":3"));
	assert(strstr(buf, "\"wd_hb_gap_max_ms\":4294967295"));
	assert(strstr(buf, "\"world_loaded\":true,"));
	assert(strstr(buf, "\"map_step\":2147483647"));
	assert(strstr(buf, "\"map_max\":2147483647"));
	assert(strstr(buf, "\"map_sign\":true,"));
	assert(strstr(buf, "\"smig_verified\":4294967295,"));
	assert(strstr(buf, "\"smig_spawners_verified\":4294967295,"));
	assert(strstr(buf, "\"save_fail_n\":4294967295,\"save_fail_stage\":4294967295,"
						"\"save_fail_sub\":4294967295,\"save_skip_n\":4294967295,"
						"\"save_idx_fb\":4294967295,"));
	// Take-49 follow-up (MP world-install probe): the mp_*/wl_* stage fields
	// close the heartbeat now; this hb literal sets none of them. The S3
	// music_*  trio (2026-08-31) closes it after that -- set to UINT32_MAX
	// here since they ARE part of this test's "full payload" worst case.
	// T1-a..d perf telemetry hook (2026-09-03): dlcalls/tris (both 0 here,
	// unset by this hb literal) now close the heartbeat after `creative`.
	// P2-c telemetry hook (2026-09-03, lane/perf-t1-ramp-slice): ms_mesh_
	// worker/_max + mem2_lock_wait now close the heartbeat after tris.
	// T-a/T-b/T-c telemetry-robustness hook (2026-09-03, lane/telemetry-
	// robust): tel_fail/tel_last_errno/tel_quiesced/frame_counter/
	// mem2_carve_free_kb now close the heartbeat after mem2_lock_wait.
	// H-1/H-2/H-3 responsiveness lane (2026-09-03): chunks_meshed/
	// near_missing/near_mesh_pending/near_missing_max/near_settle_ms/
	// edit_to_visible_frames/
	// load_screen_ms now close the heartbeat after mem2_carve_free_kb.
	// (near_mesh_pending split out of the old summed near_missing 2026-09-04.)
	assert(strstr(buf,
		"\"mp_state\":0,\"mp_gen\":0,\"mp_login\":0,\"mp_ret_kb\":0,"
		"\"mp_ret_n\":0,\"mp_ready\":0,\"mp_backlog\":0,\"mp_evt_q\":0,"
		"\"mp_evict\":0,\"mp_alloc_fail\":0,\"mp_reconn\":0,"
		"\"wl_pending\":0,\"wl_failed\":0,"
		"\"mp_rx_kb\":0,"
		// Terminal-event latch (mp-split-0912/5): zero here like the rest of
		// the mp_ block in this first pass -- the widest-payload pass below
		// saturates all three along with mp_rx_kb.
		"\"mp_end\":0,\"mp_end_phase\":0,\"mp_end_rx\":0,"
		// mp-split-0912/25: empty by contract while mp_end == 0.
		"\"mp_end_reason\":\"\","
		"\"mp_dec_phase\":0,\"mp_dec_pkt\":0,"
		"\"mp_dec_have\":0,\"mp_dec_need\":0,\"mp_move_ticks\":0,\"mp_move_samples\":0,\"rpcdrop\":0,"
		// MP admission budget (W4, design 2.7): zero in this pass like the
		// rest of the mp_ block -- the non-zero case is mp_admission_budget
		// below.
		"\"mp_adm_radius\":0,\"mp_resident_cols\":0,"
		"\"mp_backed_cols\":0,\"mp_backing_kb\":0,"
		"\"mp_backing_peak_kb\":0,\"mp_demotions\":0,"
		"\"mp_compactions\":0,\"mp_backing_refusals\":0,"
		"\"mp_ready_pending\":0,"
		"\"music_key\":4294967295,\"music_ms\":4294967295,"
		"\"music_underruns\":4294967295,\"music_starts\":4294967295,"
		"\"music_rejects\":4294967295,\"music_dur\":4294967295,"
		"\"mrs_cycles\":4294967295,\"mrs_target\":4294967295,"
		"\"mrs_wedge\":true,\"creative\":true,\"dlcalls\":0,\"tris\":0,"
		"\"ms_mesh_worker\":12345.67,\"ms_mesh_worker_max\":12345.67,"
		"\"mem2_lock_wait\":4294967295,"
		"\"tel_fail\":4294967295,\"tel_last_errno\":-2147483648,"
		"\"tel_quiesced\":true,\"frame_counter\":4294967295,"
		"\"mem2_carve_free_kb\":-2147483648,"
		"\"chunks_meshed\":4294967295,\"near_missing\":2147483647,"
		"\"near_mesh_pending\":2147483647,"
		"\"near_missing_max\":2147483647,\"near_settle_ms\":4294967295,"
		"\"edit_to_visible_frames\":4294967295,"
		"\"load_screen_ms\":4294967295,\"player_health\":-2147483648,\"loading_screen\":true,"));
	assert(strstr(buf, "\"item_nbt_exhaustions\":4294967295,"));
	assert(strstr(buf, "\"perf_duration_us\":18446744073709551615,"));
	assert(strstr(buf, "\"far_storage\":2147483647,"));
	assert(strstr(buf, "\"vi_ready_max_us\":4294967295,"));
	assert(strstr(buf, "\"vi_ready_over_8000\":4294967295,"));
	assert(strstr(buf, "\"gpu_poll_late_max_us\":4294967295,"));
	assert(strstr(buf, "\"gpu_poll_late_over_8000\":4294967295,"));
	assert(strstr(buf, "\"server_time\":4294967295,"));
	assert(strstr(buf, "\"sd_trace_max_us\":4294967295,"));
	assert(strstr(buf, "\"sd_trace_over_30ms\":4294967295,"));
	/* Sender self-telemetry at its widest: 137 bytes for the six keys. */
	assert(strstr(buf,
		"\"tel_drop\":4294967295,\"tel_it\":4294967295,"
		"\"tel_sends\":4294967295,\"tel_smax_us\":4294967295,"
		"\"tel_qmax\":4294967295,\"tel_hb_repl\":4294967295,"));
	assert(strstr(buf, "\"srv_us\":[4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295]"));
	assert(strstr(buf, "\"guard_scan_us\":[4294967295,4294967295],\"world_us\":[4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295],\"worst_parts_us\":[4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295,4294967295],"
		/* S-A3 sits between worst_parts_us and deaths, so this adjacency
		 * assert carries the new run: four keys, always emitted, at their
		 * widest (81 B -- see the line-budget assert below). */
		"\"split_live\":1,\"split_pending\":1,\"split_toggle_ms\":4294967295,"
		"\"split_last_ok\":-1,"
		"\"deaths\":0,\"death_cause\":\"none\","));
	assert(strstr(buf, "\"op\":4294967295,\"oph\":4294967295,\"opc\":4294967295,\"opf\":4294967295,\"oer\":4294967295,\"orc\":4294967295,\"ocy\":4294967295,\"oex\":4294967295,\"owu\":4294967295,\"db\":4294967295,\"ds\":4294967295,\"dln\":4294967295,\"dls\":4294967295,\"dlp\":4294967295,\"dlc\":1}"));
	/* Combined maximum-width legacy counters, anchor metadata and all samples. */
	hb.far_storage = INT_MIN;
	hb.server_time = UINT32_MAX;
	hb.tick = UINT32_MAX;
	hb.chunks_rendered = INT_MIN;
	hb.mesh_pending = INT_MIN;
	hb.mesh_stale = UINT32_MAX;
	hb.mesh_trunc = UINT32_MAX;
	hb.evict_stuck = UINT32_MAX;
	hb.near_evict_defer = UINT32_MAX;
	hb.srv_rt_us = INT_MIN;
	hb.srv_wt_us = INT_MIN;
	hb.heap_used_kb = INT_MIN;
	hb.heap_free_kb = INT_MIN;
	hb.heap_blocks = INT_MIN;
	hb.heap_top_kb = INT_MIN;
	hb.mem1_free_kb = INT_MIN;
	hb.mem2_free_kb = INT_MIN;
	hb.mem2_spill = UINT32_MAX;
	hb.mem2_oom = UINT32_MAX;
	hb.dlbad = UINT32_MAX;
	hb.dlcall = UINT32_MAX;
	hb.wpad_probe0 = INT_MIN;
	hb.wkspace_free = INT_MIN;
	hb.wkspace_total = INT_MIN;
	hb.wkspace_used = INT_MIN;
	hb.wkspace_free_blocks = INT_MIN;
	hb.near_radius = INT_MIN;
	hb.far_view_distance = INT_MIN;
	hb.far_ring_width = INT_MIN;
	hb.far_missing = INT_MIN;
	hb.far_missing_max = INT_MIN;
	hb.far_settle_ms = UINT32_MAX;
	hb.srv_gen_us = INT_MIN;
	hb.srv_gen_cols = INT_MIN;
	hb.srv_gen_backlog = INT_MIN;
	hb.srv_loop_it = UINT32_MAX;
	hb.srv_elapsed_ms = INT_MIN;
	hb.srv_ticks_pend = UINT32_MAX;
	hb.srv_partial_m = INT_MIN;
	hb.srv_sleep_ms = INT_MIN;
	hb.srv_far_us_max = INT_MIN;
	hb.cw_set_n = UINT32_MAX;
	/* push-0921/B4c (Astra gate 1 Lane B item 2): the loading screen's
	 * CUMULATIVE backstop-admission counter, lworld_ready_timeout_admits().
	 * Saturated here like every other uint32 so the worst-case line width
	 * below accounts for it. */
	hb.lw_to_admits = UINT32_MAX;
	hb.cflood = UINT32_MAX;
	hb.notify_dispatch_total = UINT32_MAX;
	hb.wd_main = UINT32_MAX;
	hb.wd_srv = UINT32_MAX;
	hb.wd_worker = UINT32_MAX;
	hb.wd_hb_gap_max_ms = UINT32_MAX;
	hb.world_loaded = false;
	hb.menu_ap_step = INT_MIN;
	hb.menu_ap_max_step = INT_MIN;
	hb.menu_ap_sign_reached = false;
	hb.save_migrate_state = UINT32_MAX;
	hb.save_migrate_ms = UINT32_MAX;
	hb.save_migrate_regions = UINT32_MAX;
	hb.save_migrate_chunks = UINT32_MAX;
	hb.save_migrate_signs = UINT32_MAX;
	hb.save_migrate_expected = UINT32_MAX;
	hb.save_migrate_verified = UINT32_MAX;
	hb.save_migrate_chests_rewritten = UINT32_MAX;
	hb.save_migrate_chests_expected = UINT32_MAX;
	hb.save_migrate_chests_verified = UINT32_MAX;
	hb.save_migrate_spawners_synthesized = UINT32_MAX;
	hb.save_migrate_spawners_expected = UINT32_MAX;
	hb.save_migrate_spawners_verified = UINT32_MAX;
	hb.save_fail_n = UINT32_MAX;
	hb.save_fail_stage = UINT32_MAX;
	hb.save_fail_sub = UINT32_MAX;
	hb.save_skip_n = UINT32_MAX;
	hb.save_idx_fb = UINT32_MAX;
	hb.mp_state = UINT32_MAX;
	hb.mp_gen = UINT32_MAX;
	hb.mp_logged_in = UINT32_MAX;
	hb.mp_ret_kb = UINT32_MAX;
	hb.mp_ret_entries = UINT32_MAX;
	hb.mp_ready = UINT32_MAX;
	hb.mp_backlog = UINT32_MAX;
	hb.mp_evt_q = UINT32_MAX;
	hb.mp_evict = UINT32_MAX;
	hb.mp_alloc_fail = UINT32_MAX;
	hb.mp_reconn = UINT32_MAX;
	hb.wl_pending = UINT32_MAX;
	hb.wl_failed = UINT32_MAX;
	hb.mp_rx_kb = UINT32_MAX;
	hb.mp_end = UINT32_MAX;
	hb.mp_end_phase = UINT32_MAX;
	hb.mp_end_rx = UINT32_MAX;
	/* mp-split-0912/25: the heartbeat's only variable-length field, so
	 * "widest" here is not a saturated integer but the most EXPENSIVE
	 * string it can hold: all 47 storable characters as control bytes,
	 * each of which JSON-escapes to SIX emitted bytes. A real kick reason
	 * is text and costs one byte per character; this is the bound a
	 * garbled or hostile peer could force, and the line-length assert
	 * below is the gate that proves it still fits the slot. */
	for(size_t i = 0; i < sizeof(hb.mp_end_reason) - 1; i++)
		hb.mp_end_reason[i] = (char)0x01;
	hb.mp_end_reason[sizeof(hb.mp_end_reason) - 1] = '\0';
	hb.mp_dec_phase = UINT32_MAX;
	hb.mp_dec_pkt = UINT32_MAX;
	hb.mp_dec_have = UINT32_MAX;
	hb.mp_dec_need = UINT32_MAX;
	/* MP admission budget (W4, design 2.7): saturated here so the worst-case
	 * line-length gate below accounts for all nine new fields. */
	hb.mp_adm_radius = UINT32_MAX;
	hb.mp_resident_cols = UINT32_MAX;
	hb.mp_backed_cols = UINT32_MAX;
	hb.mp_backing_kb = UINT32_MAX;
	hb.mp_backing_peak_kb = UINT32_MAX;
	hb.mp_demotions = UINT32_MAX;
	hb.mp_compactions = UINT32_MAX;
	hb.mp_backing_refusals = UINT32_MAX;
	hb.mp_ready_pending = UINT32_MAX;
	hb.music_key = UINT32_MAX;
	hb.music_ms = UINT32_MAX;
	hb.music_underruns = UINT32_MAX;
	hb.music_starts = UINT32_MAX;
	hb.music_rejects = UINT32_MAX;
	hb.music_dur = UINT32_MAX;
	hb.mp_reconnect_cycles = UINT32_MAX;
	hb.mp_reconnect_target = UINT32_MAX;
	hb.mp_reconnect_wedge = false;
	hb.creative = false;
	hb.dlcalls = UINT32_MAX;
	hb.tris = UINT32_MAX;
	hb.mem2_lock_wait = UINT32_MAX;
	hb.tel_fail = UINT32_MAX;
	hb.tel_last_errno = INT_MIN;
	hb.tel_quiesced = false;
	hb.frame_counter = UINT32_MAX;
	hb.mem2_carve_free_kb = INT_MIN;
	/* colcap-1006 (ASTRA-CC-R2 item 5): the carve-contiguity pair at its
	 * widest -- valid, saturated uint32. */
	hb.mem2_carve_largest_valid = true;
	hb.mem2_carve_largest_allocatable_bytes = UINT32_MAX;
	/* S-A3 (Astra H3): kept at its widest for the combined line-budget gate
	 * below -- INT_MIN again, so the clamp is exercised on this line too. */
	hb.split_live = true;
	hb.split_pending = true;
	hb.split_toggle_ms = UINT32_MAX;
	hb.split_last_ok = INT_MIN;
	hb.chunks_meshed = UINT32_MAX;
	hb.near_missing = INT_MIN;
	hb.near_mesh_pending = INT_MIN;
	hb.near_missing_max = INT_MIN;
	hb.near_settle_ms = UINT32_MAX;
	hb.edit_to_visible_frames = UINT32_MAX;
	hb.load_screen_ms = UINT32_MAX;
	hb.player_health = INT_MIN;
	hb.loading_screen = false;
	hb.build_id = "123456789012345678901234567890123456789012345678901234567890123";
	hb.anchor = (struct hw_anchor_evidence) {
		.enabled = true, .ready = false, .measuring = false, .collision = false,
		.actual_tps = 12345.67f, .separation = UINT32_MAX,
		.elapsed_ms = UINT32_MAX, .near_owed = {UINT32_MAX, UINT32_MAX},
		.oldest = {UINT32_MAX, UINT32_MAX}, .missing = {UINT32_MAX, UINT32_MAX},
		.mesh_pending = {UINT32_MAX, UINT32_MAX},
		.far_missing = {UINT32_MAX, UINT32_MAX}, .far_max = {UINT32_MAX, UINT32_MAX},
		.far_mesh_pending = {UINT32_MAX, UINT32_MAX}, .ready_streak = UINT32_MAX,
		.faults_load = {UINT32_MAX, UINT32_MAX, UINT32_MAX},
		.faults_run = {UINT32_MAX, UINT32_MAX, UINT32_MAX},
		.deaths = UINT32_MAX, .walk_tick = UINT32_MAX, .walk_stage = UINT32_MAX,
		.walk_brake = true, .walk_water = true, .walk_capture = true,
		.death_cause = "unknown", .damage_cause = "unknown",
		.tick_backlog = UINT32_MAX, .frame_count = HW_ANCHOR_FRAME_CAP,
		.frame_dropped = UINT32_MAX, .frame_total = UINT32_MAX,
		.carve_load_min = INT_MIN, .carve_run_min = INT_MIN};
	for(unsigned i = 0; i < HW_ANCHOR_FRAME_CAP; i++)
		hb.anchor.frame_us[i] = UINT32_MAX;
	/* S1-d (mp-split-0912/68): the per-owner tallies at THEIR widest too --
	 * saturated counters (10 digits each) and INT_MIN health (11 characters
	 * including the sign), so the combined line budget below is measured
	 * against the worst line these fields can produce, not a zeroed one. */
	for(unsigned p = 0; p < 2; p++) {
		for(unsigned k = 0; k < HW_ANCHOR_ACTION_COUNT; k++)
			hb.anchor.actions[p][k] = UINT32_MAX;
		hb.anchor.health[p] = INT_MIN;
	}
	/* colcap-1006 (ASTRA-CC-R3 item 2): the anchor pool block at its widest
	 * (every counter saturated, both bools true). */
	hb.pool = (struct hw_anchor_pool_evidence) {.enabled = true,
		.pb = {.column_pool_used = UINT32_MAX, .column_pool_peak = UINT32_MAX,
			.column_pool_slabs = UINT32_MAX, .column_pool_overflow = UINT32_MAX,
			.column_pool_carve_fail = UINT32_MAX, .budget_valid = true,
			.budget_wii = true, .budget_resident = UINT32_MAX,
			.budget_live_backings = UINT32_MAX, .budget_reserved = UINT32_MAX,
			.budget_prepeak = UINT32_MAX, .budget_peak = UINT32_MAX,
			.budget_waits = UINT32_MAX, .budget_phase = UINT8_MAX},
		/* item 3 at its widest: 16 slabs (the struct cap) of 3-digit
		 * occupancy, saturated display-list counters. */
		.slab_count = UINT32_MAX, .dl_lists = UINT32_MAX,
		.dl_bytes = UINT32_MAX, .event_seq = UINT32_MAX,
		.shrinks = UINT32_MAX, .shrink_fail = UINT32_MAX,
		.events_lost = UINT32_MAX, .dlo_valid = true, .pac_valid = true};
	memset(hb.pool.slab_used, 0xff, sizeof hb.pool.slab_used);
	memset(hb.pool.dlo, 0xff, sizeof hb.pool.dlo);
	memset(hb.pool.pac, 0xff, sizeof hb.pool.pac);
	/* push-0921 A11's walker-proof fields at THEIR widest, saturated here for
	 * the same reason every other counter above is: the line budget has to be
	 * measured against the worst line these fields can produce, not a zeroed
	 * one. The three masks and the flags are uint8_t, so this truncates to
	 * 255 -- three digits, which IS that type's worst case. */
	hb.np_entered = hb.np_skipped = hb.np_focus = hb.np_flags = UINT8_MAX;
	hb.np_rows = INT_MIN;
	test_rpcdrop = UINT32_MAX;
	/* residency-1005/2 + /4 item 8: the v2 per-actor residency block at its
	 * widest -- full-width rows, tick and all four induced marks. */
	for(unsigned a = 0; a < 2; a++)
		for(unsigned i = 0; i < HW_TELEMETRY_RZA_FIELDS; i++)
			hb.rza[a][i] = UINT32_MAX;
	hb.rz_tick = UINT32_MAX;
	hb.rzf = UINT32_MAX; /* item 12: the runtime fault phase mask */
	for(unsigned a = 0; a < 2; a++)
		for(unsigned i = 0; i < 8; i++) hb.rze[a][i] = UINT32_MAX;
	hb.rzi_count = HW_TELEMETRY_RZI_MAX;
	for(unsigned m = 0; m < HW_TELEMETRY_RZI_MAX; m++)
		for(unsigned i = 0; i < HW_TELEMETRY_RZI_FIELDS; i++)
			hb.rzi[m][i] = UINT32_MAX;
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf),
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	assert(n > 0 && n < sizeof(buf) && strlen(buf) == n && buf[n-1] == '}');
	assert(strstr(buf, "\"anchor_frame_us\":[4294967295,4294967295"));
	fprintf(stderr, "combined maximum heartbeat bytes: %zu / %zu\n", n, sizeof(buf));
	if(g_worst_heartbeat_only)
		puts(buf);
	char old_cpu[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	memcpy(old_cpu, buf, n + 1);
	size_t old_size = n;
	hb.cpu.enabled = true;
	hb.cpu.clock_step_ns = hb.cpu.valid = hb.cpu.ticks = UINT32_MAX;
	hb.cpu.main_us = hb.cpu.mesher_us = hb.cpu.frames = UINT32_MAX;
	hb.cpu.packed_us = hb.cpu.clock_mode = UINT32_MAX;
	for(unsigned i = 0; i < TPS_PHASE_COUNT; i++) hb.cpu.phase_us[i] = UINT32_MAX;
	for(unsigned i = 0; i < TPS_PART_COUNT; i++) hb.cpu.nested_us[i] = UINT32_MAX;
	for(unsigned i = 0; i < 3; i++)
		hb.cpu.wait_count[i] = hb.cpu.wait_us[i] = UINT32_MAX;
	s_stage_clock_us = UINT64_MAX;
	hw_telemetry_test_set_stage_clock(stage_test_now_us, NULL);
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf),
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	hw_telemetry_test_set_stage_clock(NULL, NULL);
	fprintf(stderr, "combined maximum CPU heartbeat bytes: %zu / %zu\n", n, sizeof(buf));
	/* 8,850 -> 8,987: +137 bytes for the six 2026-09-07 sender self-
	 * telemetry keys at their widest (all UINT32_MAX); 8,987 -> 9,008:
	 * +21 for the 2026-09-08 bounded-send tel_wto at its widest; 9,008 ->
	 * 9,019: +11 (one more ",4294967295") from the 810da751 merge, where
	 * TPS_REGION_PRESERVE grew the loop-emitted cpu_nested_us array from
	 * 21 to 22 entries; 9,019 -> 9,194: +175 for the 2026-09-08 sender-
	 * STAGE run at its widest -- tel_stg 12 (masked to one digit, see the
	 * formatter), tel_stg_us 24, tel_pmax_us 25, tel_xmax_us 25, tel_abd
	 * 21, tel_pret 23, tel_prev 22, tel_sret 23.
	 *
	 * 9,194 -> 9,360: +166 for the 2026-09-08 server-loop wedge
	 * instrumentation lane's six new fields at their widest (srv_loop_it/
	 * srv_ticks_pend UINT32_MAX = 10 digits each; srv_elapsed_ms/
	 * srv_partial_m/srv_sleep_ms/srv_far_us_max INT_MIN = 11 chars incl.
	 * sign each), which is also why HW_TELEMETRY_HEARTBEAT_LINE_SIZE moved
	 * 9216 -> 10240 in the same lane (the prior slot had only 22 B left).
	 *
	 * 9,360 -> 9,382: +22 for the 2026-09-08 lane/carve-observer-boot
	 * --cw-set run-arg's cw_set_n field at its widest (UINT32_MAX, 10
	 * digits) -- see hw_run.h's cw_set field comment.
	 *
	 * 9,382 -> 9,451: +69 for the 2026-09-12 authoritative-kick lane's three
	 * terminal-latch fields at their widest (mp_end/mp_end_phase/mp_end_rx,
	 * all unsigned, UINT32_MAX = 10 digits each: 20 + 26 + 23 B including
	 * each key, colon and comma).
	 *
	 * 9,451 -> 9,712: +261 for mp-split-0912 W4's nine admission-budget
	 * fields (design 2.7), all unsigned and saturated above, so each costs
	 * its quoted key + ':' + 10 digits + ',': mp_adm_radius 27,
	 * mp_resident_cols 30, mp_backed_cols 28, mp_backing_kb 27,
	 * mp_backing_peak_kb 32, mp_demotions 26, mp_compactions 28,
	 * mp_backing_refusals 33, mp_ready_pending 30.
	 *
	 * 9,712 -> 10,013: +301 for mp-split-0912/25's mp_end_reason at ITS
	 * widest -- the heartbeat's first and only variable-length field. 15 B
	 * of quoted key + 1 colon + its own 2 quotes + 47 source characters at
	 * the 6-byte-per-byte control-character escape (282) + 1 comma. A REAL
	 * kick reason is text and costs 47 + 19 = 66 B here; the 301 is the
	 * bound a garbled or hostile peer can force, which is the number a
	 * buffer gate has to be sized against.
	 *
	 * 10,013 -> 10,094: +81 for mp-split-0912/49's four S-A3 split-toggle
	 * fields at their widest, all four ALWAYS emitted (an untoggled run has
	 * to be a visible 0/0/0/-1 line): split_live 15 B and split_pending 18 B
	 * (bool, so one digit each by type), split_toggle_ms 29 B (saturated
	 * uint32, 10 digits), split_last_ok 19 B (clamped by the formatter to its
	 * -1/0/1 domain, so 2 characters rather than the 11 INT_MIN would print
	 * -- the tel.stage-mask precedent, worth 9 B here). Each figure is
	 * quoted key + ':' + value + ','.
	 *
	 * The S-A3 block left 146 B of the then-10,240 B slot -- down from 227 B
	 * (mp_end_reason before it took 528 -> 227, and the W4 block before that
	 * 789 -> 528) and BELOW the 200 B floor the cw_set_n field's own addition
	 * was gated on. That was recorded here as a HEADROOM WARNING rather than
	 * quietly absorbed, with the bump deliberately deferred: the slot also
	 * sizes hw_telemetry.c's s_pre_lines[] static and two ~10 KB stack frames
	 * on a platform where a large frame has broken boot before, so it is a
	 * hardware-risk change of its own and did not belong in a telemetry-field
	 * lane.
	 *
	 * mp-split-0912/54 (S-A7) is that lane: HW_TELEMETRY_HEARTBEAT_LINE_SIZE
	 * moved 10,240 -> 12,288, so the SAME measured worst case (10,094 B -- the
	 * payload did not change, only the slot) now leaves 2,194 B. The per-
	 * consumer byte accounting for the bump is in hw_telemetry.h's comment on
	 * the constant; nothing here should change until a new FIELD lands. An
	 * overflow could not corrupt anything -- send_line_with refuses any line
	 * longer than the slot as an -EMSGSIZE producer reject, and THIS assert
	 * fails the build loudly first -- but the numbers ARE the real gate, not
	 * this comment. The next heartbeat field to be added should still check
	 * the spare asserted below before assuming it fits.
	 *
	 * 10,094 -> 10,283: +189 for mp-split-0912/68 (S1-d), the three per-owner
	 * keys at the END of the anchor block, all three ALWAYS emitted when that
	 * block is (a guest that did nothing has to be a visible row of zeros, not
	 * a missing key). At their widest: act0 and act1 are 75 B each (leading
	 * comma 1 + quoted key 6 + colon 1 + brackets 2 + six saturated uint32 at
	 * 10 digits + five separators), and act_health is 39 B (comma 1 + quoted
	 * key 12 + colon 1 + brackets 2 + two INT_MIN at 11 characters + one
	 * separator). That leaves 2,005 B of the 12,288 slot -- above the 200 B
	 * floor the cw_set_n addition was gated on, and above the 512 B margin
	 * this lane was asked to keep.
	 *
	 * 10,283 -> 10,309: +26 for push-0921/B4c's `lw_to_admits`, ALWAYS
	 * emitted (a missing key is a collector FAIL for the two 10-minute MP
	 * profiles, so it may never be conditional). At its widest: key 14 B
	 * ("lw_to_admits" plus its quotes) + colon 1 + a saturated uint32 at 10
	 * digits + the separator 1. On its own branch that left 1,979 B.
	 *
	 * push-0921 A11: the five walker-proof fields at THEIR widest. The three
	 * masks and the flags are uint8_t by type (7 and 5 bits by construction),
	 * so UINT32_MAX assigned above truncates to 255 -- 3 digits, which IS the
	 * type's worst case, the same reasoning split_live/split_last_ok already
	 * use. np_rows is a plain int and budgets INT_MIN's 11 characters.
	 *
	 * 10,283 -> 10,369 on lane A's own branch: +86. np_entered 17 B and
	 * np_skipped 17 B (comma 1 + quoted key 12 + colon 1 + 3 digits), np_focus
	 * 15 and np_flags 15 (quoted key 10), np_rows 22 (quoted key 9 + colon 1 +
	 * 11 characters + comma 1).
	 *
	 * THIS BUILD IS THE UNION of the two lanes, so both additions are spent
	 * here and the worst case is measured ONCE for the merged formatter:
	 * 10,283 + 26 + 86 + 26 for saturated test-clock up_ms, plus 38 B
	 * for B14's two zero-valued cadence keys, plus 21 B for rpcdrop at
	 * UINT32_MAX = 10,480 of the 12,288 slot, leaving 1,808 B -- above
	 * the 512 B margin the S-A7 lane asked for and well above the 200 B floor
	 * asserted below. Neither lane needs to move
	 * HW_TELEMETRY_HEARTBEAT_LINE_SIZE.
	 *
	 * 10,480 -> 10,586: +106 for lava-aabb-0924/H1's four chunk-save failure
	 * keys, ALWAYS emitted (a healthy run is a visible 0/0/0/N row). At their
	 * widest: save_fail_n 25 B, save_fail_stage 29 B, save_fail_sub 27 B,
	 * save_skip_n 25 B (each: quoted key + colon + saturated uint32 at 10
	 * digits + comma). Leaves 1,702 B of the 12,288 slot -- above the 512 B
	 * margin and the 200 B floor; the assert below is the gate.
	 *
	 * 10,586 -> 10,611: +25 for the H2 review fix (4) key save_idx_fb
	 * (quoted key 13 + colon 1 + saturated uint32 10 + comma 1), leaving
	 * 1,677 B. */
	/* Eight saturated TNT scalars add 110 bytes; 1567 bytes remain.
	 * residency-1005/2 round 3: the per-actor "rza" rows add 152 (`,"rza":[[`
	 * 9 + two rows of 14 values saturated at 9999 = 4 digits + 13 commas +
	 * `,[` / `]` / `]]` framing); 1415 bytes remain. The split-dims worst
	 * case below is the binding one (it must keep 200 B spare).
	 * residency-1005/4 closure item 8, schema v2: `,"rzv":2` 8 + `,"rzt":`
	 * + 10 digits 17 + `,"rza":[` 8 + two rows of 22 unsaturated uint32
	 * (243 each: brackets, 220 digits, 21 commas) + 1 separating comma + `]`
	 * + `,"rzi":[` 8 + four marks of 5 values (56 each) + 3 commas + `]` =
	 * 757 B, replacing the round-3 152: +605 -> 11,478; item 12's
	 * `,"rzf":` + 10 digits adds 17 -> 11,495.
	 * I07 merge (residency + G9): both blocks above are present in the
	 * merged line; RE-MEASURED at I07 (not hand-summed): 11,658, leaving 1,654.
	 * 10,721 -> 10,871: +150 for G9 R6's light-queue array at its widest
	 * ("lq":[ + 13 saturated uint32 + 12 commas + ], + comma), leaving
	 * 1,417 B -- above the 512 B margin and the 200 B floor. (The widest
	 * split_dims line below moves 11,918 -> 12,068 and still clears its
	 * 200 B floor by 20 B.)
	 * 10,871 -> 10,884: +13 for ASTRA-G9-REVIEW-2 item 5's v2 array (26
	 * saturating uint16 = 163 B at its widest, replacing the 150 B v1),
	 * leaving 1,404 B; the widest split_dims line moves 12,068 -> 12,081,
	 * 7 B above its 200 B floor (asserted exactly below). */
	fprintf(stderr, "maximal heartbeat with rz v2: %zu\n", n);
	assert(strstr(buf, ",\"rzv\":2,\"rzt\":4294967295,\"rzf\":4294967295,"
		"\"rza\":[[4294967295,"));
	assert(strstr(buf, "4294967295]],\"rzi\":[[4294967295,"));
	/* M3 merged production measurement: pool/receipts + lqd v3,
	 * with G9 pacing dormant, occupy 12,972 bytes. */
	assert(n == 12972 && n < sizeof(buf) && strlen(buf) == n && buf[n - 1] == '}');
	assert(strstr(buf, ",\"pac\":[4294967295,4294967295,4294967295,4294967295,"
		"4294967295,4294967295,4294967295,4294967295,4294967295]"));
	assert(strstr(buf, ",\"dlo\":[4294967295,4294967295,4294967295,"));
	assert(strstr(buf, ",\"cpv\":3,\"column_pool_used\":4294967295,"));
	assert(strstr(buf, ",\"cpe\":[4294967295,4294967295,4294967295,4294967295]"));
	assert(strstr(buf, ",\"cps\":[255,255,255,255,255,255,255,255,255,255,"
		"255,255,255,255,255,255],\"dll\":4294967295,\"dlb\":4294967295"));
	assert(strstr(buf, "\"budget_waits\":4294967295,\"budget_phase\":255"));
	assert(strstr(buf, "\"mp_move_samples\":0,\"rpcdrop\":4294967295,"));
	assert(strstr(buf, "\"up_ms\":18446744073709551"));
	{
		/* REL-MP-FIX3b: "rskip" is emitted only when non-zero, which only a
		 * remote session can make it, so the split/dims/anchor worst cases
		 * keep their exact bytes (rskip stays 0 there). Its own cost is 19 B;
		 * measured on this maximal line with the opt-in G9 pacing + lease rows
		 * (the only other blocks an MP line can carry), the production slot
		 * keeps its >= 200 B spare floor. */
		static char mp[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
		const char* id = "123456789012345678901234567890123456789012345678901234567890123";
		size_t base = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		assert(base > 0 && !strstr(mp, "\"rskip\""));
		hb.rskip = UINT32_MAX;
		size_t with = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		assert(with == base + 19 && strstr(mp, ",\"rskip\":4294967295,"));
		/* REL-MP-FIX3d: "rns" (remote relight no-slot seeds) follows the same
		 * only-when-non-zero rule; 17 B, inside the same spare floor below. */
		assert(!strstr(mp, "\"rns\""));
		hb.rns = UINT32_MAX;
		size_t with_rns = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		assert(with_rns == with + 17 && strstr(mp, ",\"rskip\":4294967295,\"rns\":4294967295,"));
		with = with_rns;
		/* REL-MP-COREFIRST: "lwc" (remote safe-core gauges) only when
		 * lwc_present, which main.c sets only in a remote session; at its
		 * widest `,"lwc":[` 8 + four saturated uint32 40 + 3 commas + `]`
		 * = 52 B, inside the same spare floor below. */
		assert(!strstr(mp, "\"lwc\""));
		hb.lwc_present = true;
		for(unsigned i = 0; i < 4; i++) hb.lwc[i] = UINT32_MAX;
		size_t with_lwc = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		assert(with_lwc == with + 52 && strstr(mp, ",\"rns\":4294967295,\"lwc\":["
			"4294967295,4294967295,4294967295,4294967295],"));
		with = with_lwc;
		/* nether-dark/1: "blc" (client vs server block-light census) only
		 * when blc_present; at its widest `,"blc":[` 8 + INT32_MIN 11 + six
		 * saturated uint32 60 + 6 commas + `]` = 86 B, inside the floor. */
		assert(!strstr(mp, "\"blc\""));
		hb.blc_present = true;
		hb.blc_dim = INT32_MIN;
		for(unsigned i = 0; i < 6; i++) hb.blc[i] = UINT32_MAX;
		size_t with_blc = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		assert(with_blc == with + 86 && strstr(mp, "4294967295],\"blc\":[-2147483648,"
			"4294967295,4294967295,4294967295,4294967295,4294967295,4294967295]"));
		with = with_blc;
		/* hw1030n: "ssw" (spawn-search progress) only while the search
		 * runs; at its widest `,"ssw":[` 8 + 2 x 10 + `,` + `]` = 30 B. */
		assert(!strstr(mp, "\"ssw\""));
		hb.ssw_present = true;
		hb.ssw[0] = hb.ssw[1] = UINT32_MAX;
		size_t with_ssw = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		assert(with_ssw == with + 30 && strstr(mp, "4294967295],\"ssw\":[4294967295,4294967295]"));
		with = with_ssw;
		hb.g9p_present = true;
		for(unsigned i = 0; i < 22; i++) hb.g9p[i] = UINT32_MAX;
		hb.g9_lease = (struct g9_priority_lease){.enabled = true, .cancelled = true,
			.active = true, .boosted = true, .parked = true};
		memset(&hb.g9_lease.stats, 0xFF, sizeof hb.g9_lease.stats);
		size_t all = hw_telemetry_format_heartbeat(mp, sizeof mp, id, "mp_lan_walk_10m", &hb);
		fprintf(stderr, "FIX3b maximal MP line + rskip + pacing + lease: %zu / %zu, spare %zu\n",
			all, sizeof mp, sizeof mp - all);
		assert(all > with && all < sizeof mp && strlen(mp) == all && sizeof mp - all >= 200);
		hb.rskip = 0;
		hb.rns = 0;
		hb.lwc_present = false;
		memset(hb.lwc, 0, sizeof hb.lwc);
		hb.blc_present = false;
		hb.ssw_present = false;
		memset(hb.ssw, 0, sizeof hb.ssw);
		hb.blc_dim = 0;
		memset(hb.blc, 0, sizeof hb.blc);
		hb.g9p_present = false;
		memset(hb.g9p, 0, sizeof hb.g9p);
		hb.g9_lease = (struct g9_priority_lease){0};
	}
	/* P2's widest added row must share this exact maximum, not a small row. */
	hb.guest = true;
	hb.gx = -FLT_MAX;
	hb.gz = -FLT_MAX;
	hw_telemetry_split_guest_eye(-FLT_MAX, INT_MIN, INT_MIN);
	size_t p2_n = hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_play_8m", &hb);
	assert(p2_n < HW_TELEMETRY_HEARTBEAT_LINE_SIZE && strlen(buf) == p2_n);
	assert(strstr(buf, "\"guest_head_block\":-2147483648,\"guest_air\":-2147483648"));
	printf("PASS maximal P2 heartbeat: %zu / %u bytes\n", p2_n,
		HW_TELEMETRY_HEARTBEAT_LINE_SIZE);
	/* Measure the full-width non-dims P2 row with dormant G9 pacing armed. */
	hb.g9p_present = true;
	for(unsigned i = 0; i < 22; i++) hb.g9p[i] = UINT32_MAX;
	size_t p2_pacing_n = hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	assert(p2_pacing_n == 13416 && sizeof buf - p2_pacing_n >= 200
		&& p2_pacing_n < sizeof buf && strlen(buf) == p2_pacing_n
		&& buf[p2_pacing_n - 1] == '}');
	printf("PASS maximal non-dims P2 + G9 pacing heartbeat: %zu / %u bytes\n",
		p2_pacing_n, HW_TELEMETRY_HEARTBEAT_LINE_SIZE);
	hb.g9p_present = false;
	/* G8's optional budget block must fit beside the full production payload
	 * and the widest guest row, not merely in a larger formatter-test buffer. */
	hb.dims = (struct hw_split_dims_evidence) {
		.enabled = true, .dim_v0 = INT_MIN, .dim_v1 = INT_MIN,
		.views_live = UINT32_MAX, .phase = "host_trip",
		.srv_dim_host = INT_MIN, .srv_dim_guest = INT_MIN,
		.host_phase = "timeout", .host_commits = UINT32_MAX,
		.slot1_cols = INT_MIN, .rel_sent = UINT32_MAX, .rel_rx = UINT32_MAX,
		.rel_slot = INT_MIN, .wslot_v0 = INT_MIN, .wslot_v1 = INT_MIN,
		.pb = {.column_pool_used = UINT32_MAX, .column_pool_peak = UINT32_MAX,
		.column_pool_slabs = UINT32_MAX, .column_pool_overflow = UINT32_MAX,
		.column_pool_carve_fail = UINT32_MAX,
		.budget_resident = UINT32_MAX, .budget_live_backings = UINT32_MAX,
		.budget_reserved = UINT32_MAX, .budget_prepeak = UINT32_MAX,
		.budget_peak = UINT32_MAX, .budget_waits = UINT32_MAX,
		.budget_phase = UINT8_MAX}, .transfer_request = UINT32_MAX,
		.guest_commits = UINT32_MAX, .transfer_actor = UINT8_MAX,
		.transfer_state = UINT8_MAX,
		.host_rb_id = UINT32_MAX, .guest_rb_id = UINT32_MAX,
		.host_rb_reason = UINT8_MAX, .guest_rb_reason = UINT8_MAX,
	};
	size_t dims_n = hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_dims_6m", &hb);
	fprintf(stderr, "maximal G8 budget heartbeat (with rza): %zu / %zu\n",
		dims_n, sizeof buf);
	assert(dims_n > p2_n && dims_n + 200 < sizeof buf
		&& strlen(buf) == dims_n && buf[dims_n - 1] == '}');
	/* M3: saturated P2 + dims pool + lqd v3, pacing dormant. */
	assert(sizeof buf == HW_TELEMETRY_HEARTBEAT_LINE_SIZE && dims_n == 13533);
	{
		const char* first = strstr(buf, "\"column_pool_used\"");
		assert(first && !strstr(first + 1, "\"column_pool_used\""));
		assert(!strstr(buf, "\"dlo\"")); /* never beside the dims block */
		assert(strstr(buf, ",\"cpv\":3,\"cps\":["));
	}
	/* New opt-in field must fit alongside the existing maximal dims payload. */
	hb.g9p_present = true;
	for(unsigned i = 0; i < 22; i++) hb.g9p[i] = UINT32_MAX;
	size_t pacing_n = hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	assert(pacing_n < sizeof buf && strlen(buf) == pacing_n && buf[pacing_n-1] == '}');
	assert(strstr(buf, "\"g9p_v\":1"));
	assert(pacing_n == 13795 && sizeof buf - pacing_n >= 200);
	hb.g9_lease = (struct g9_priority_lease){.enabled=true, .cancelled=true,
		.stats={.acquired=UINT32_MAX, .restored=UINT32_MAX, .cancelled=UINT32_MAX,
		.faults=UINT32_MAX, .lease_pops=UINT32_MAX, .pops_max=UINT32_MAX,
		.pops_last=UINT32_MAX, .lease_max_us=UINT32_MAX, .service_max_us=UINT32_MAX,
		.priority_sets=UINT32_MAX, .sender_sets=UINT32_MAX, .services=UINT32_MAX,
		.timer_restores=UINT32_MAX}, .active=true, .boosted=true, .parked=true};
	size_t lease_n = hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	/* OFF keeps its original 200-B spare. The fixed-width ON record uses
	 * that reserved space, with an exact worst-case bound and no cap growth. */
	assert(lease_n == pacing_n + 142 && lease_n + 50 < sizeof buf && strlen(buf) == lease_n);
	assert(strstr(buf, "\"g9l_v\":2,\"g9l\":[4294967295,4294967295,4294967295"));
	assert(strstr(buf, ",4294967295,15]"));
	hb.g9_lease.enabled = false;
	assert(hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb) == pacing_n);
	assert(!strstr(buf, "\"g9l\""));
	/* L225 must preserve the same production-slot spare floor with window
	 * FPS present, not only with legacy callers. "wf" replaces "fps". */
	hb.fps_window_present = true;
	size_t fps_n = hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	fprintf(stderr, "L225 maximal dims + pacing + window FPS: %zu / %zu bytes, spare %zu (floor 200)\n",
		fps_n, sizeof buf, sizeof buf - fps_n);
	assert(fps_n < sizeof buf && strlen(buf) == fps_n && sizeof buf - fps_n >= 200);
	assert(fps_n + 1 == pacing_n && strstr(buf, "\"wf\":59.99,")
		&& !strstr(buf, "\"fps\":"));
	hb.fps_window_present = false;
	hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	printf("PASS maximal dims + G9 pacing heartbeat: %zu / %u bytes\n",
		pacing_n, HW_TELEMETRY_HEARTBEAT_LINE_SIZE);
	assert(strstr(buf, "\"budget_peak\":4294967295"));
	assert(strstr(buf, "\"host_rb_id\":4294967295,\"host_rb_reason\":255,"
		"\"guest_rb_id\":4294967295,\"guest_rb_reason\":255"));
	printf("PASS maximal G8 budget heartbeat: %zu / %u bytes\n", dims_n,
		HW_TELEMETRY_HEARTBEAT_LINE_SIZE);
	/* colcap-1006 (R4 point 9; Sol review R3 Major 1): the widest heartbeat
	 * END TO END through the production async path -- formatter, the ring's
	 * priority lane, the sender's byte loop -- not merely into a formatter
	 * buffer. Before CW_TEL_LINE was raised to the formatter's size, this
	 * line (13,103 B) was formatted and then refused by the 9,216 B ring. */
	{
		static char widest[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
		memcpy(widest, buf, pacing_n + 1);
		struct hw_telemetry_config cfg = {.enabled = true,
			.host = "127.0.0.1", .port = 39017,
			.run_id = "123456789012345678901234567890123456789012345678901234567890123",
			.profile = "split_anchor_6m"};
		assert(hw_telemetry_init(&cfg));
		hw_telemetry_test_async_sender_begin();
		hw_telemetry_emit_heartbeat(&hb);
		static struct async_wire_capture capture;
		memset(&capture, 0, sizeof capture);
		assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 1);
		assert(capture.size == pacing_n + 1 && capture.bytes[pacing_n] == '\n');
		assert(memcmp(capture.bytes, widest, pacing_n) == 0);
		/* The lane's FIFO fallback (consumer busy with the lane) carries the
		 * same widest line too. */
		static struct cw_tel_ring ring;
		memset(&ring, 0, sizeof ring);
		ring.hb_state = CW_TEL_HB_SENDING;
		assert(cw_tel_publish_hb(&ring, widest));
		assert(ring.head == 1 && strcmp(ring.lines[0], widest) == 0);
		hw_telemetry_test_async_sender_end();
		hw_telemetry_shutdown();
		printf("PASS widest heartbeat end-to-end through the async sender: "
			   "%zu B (+ newline), ring slot %u B\n", pacing_n, CW_TEL_LINE);
	}
	hb.g9p_present = false;
	hb.dims.enabled = false;
	hb.guest = false;
	hw_telemetry_format_heartbeat(buf, sizeof buf,
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	assert(strstr(buf, "\"np_entered\":255,\"np_skipped\":255,\"np_focus\":255,"
					   "\"np_flags\":255,\"np_rows\":-2147483648,"));
	/* Adjacency to the field it explains: the proof masks sit between map_max
	 * and map_sign, so a reader of a raw tape finds them with the step. */
	assert(strstr(buf, "\"map_max\":"));
	assert(strstr(buf, "\"np_rows\":-2147483648,\"map_sign\":"));
	/* The variable-length field's own share of that total, and the headroom
	 * left, stated as arithmetic so a future widening of
	 * HW_TELEMETRY_MP_END_REASON_SIZE fails HERE with a disagreement rather
	 * than quietly eating the slot: quoted key 15 + colon 1 + value quotes 2
	 * + 47 control characters at 6 escaped bytes each + comma 1. */
	assert(15 + 1 + 2 + (HW_TELEMETRY_MP_END_REASON_SIZE - 1) * 6 + 1 == 301);
	/* mp-split-0912/54 (S-A7): the slot's spare, asserted BOTH exactly (so a
	 * new field cannot land without re-measuring here) and as the >= 200 B
	 * floor (so a future editor who updates the exact figure cannot walk past
	 * the floor without tripping the inequality that states it). */
	/* M3: 13,995 - 12,972 = 1,023 bytes on the non-dims line. */
	assert(n == 12972 && sizeof(buf) - n == 1023);
	assert(sizeof(buf) - n >= 200);
	/* The per-frame anchor list is INSIDE that worst case, not an addition to
	 * it: the formatter's list is bounded by HW_ANCHOR_FRAME_CAP, every entry
	 * is saturated to UINT32_MAX above (10 digits + 1 separator), and this
	 * block proves the emitted array really is at that bound rather than
	 * trusting the two-element strstr above. A 60-sample split profile at
	 * 60 fps is a SUBSET of this, so the 2,194 B spare above already holds
	 * with the largest anchor batch the console can send. */
	{
		const char* list = strstr(buf, "\"anchor_frame_us\":[");
		assert(list);
		list += strlen("\"anchor_frame_us\":[");
		const char* end = strchr(list, ']');
		assert(end);
		unsigned entries = 1;
		for(const char* p = list; p < end; p++)
			if(*p == ',')
				entries++;
		assert(entries == HW_ANCHOR_FRAME_CAP);
		/* 256 * 11 - 1 = 2,815 B of the 10,094 is this one array. */
		assert((size_t)(end - list) == HW_ANCHOR_FRAME_CAP * 11u - 1u);
	}
	assert(strstr(buf,
		"\"mp_end\":4294967295,\"mp_end_phase\":4294967295,"
		"\"mp_end_rx\":4294967295,"));
	/* mp-split-0912/25 at its widest: 47 0x01 bytes, each emitted as the
	 * six-character \u0001 escape, inside one pair of quotes. Built here
	 * rather than hand-spelled because 282 escape characters in a literal
	 * are unreviewable -- but the ESCAPE FORM is what is being pinned: a
	 * formatter that emitted the raw control byte, or \x01, or dropped it,
	 * would otherwise still produce a plausible-looking line. */
	{
		char widest[15 + 1 + 2 + (HW_TELEMETRY_MP_END_REASON_SIZE - 1) * 6 + 2];
		int at = snprintf(widest, sizeof(widest), "\"mp_end_reason\":\"");
		assert(at == 17);
		for(size_t i = 0; i < HW_TELEMETRY_MP_END_REASON_SIZE - 1; i++)
			at += snprintf(widest + at, sizeof(widest) - (size_t)at, "\\u0001");
		at += snprintf(widest + at, sizeof(widest) - (size_t)at, "\",");
		assert((size_t)at == 301 && strlen(widest) == (size_t)at);
		assert(strstr(buf, widest));
	}
	assert(strstr(buf,
		"\"mp_adm_radius\":4294967295,\"mp_resident_cols\":4294967295,"
		"\"mp_backed_cols\":4294967295,\"mp_backing_kb\":4294967295,"
		"\"mp_backing_peak_kb\":4294967295,\"mp_demotions\":4294967295,"
		"\"mp_compactions\":4294967295,\"mp_backing_refusals\":4294967295,"
		"\"mp_ready_pending\":4294967295,"));
	assert(strstr(buf, "\"cw_set_n\":4294967295"));
	/* push-0921/B4c: present on EVERY heartbeat, and carrying the value the
	 * struct was given. The collector FAILS an mp_play_10m / mp_lan_walk_10m
	 * run whose heartbeat is missing this key (design section 5), so "the
	 * field is emitted" is a contract, not a convenience -- and it sits in
	 * the unconditional run, right after cw_set_n, so no profile branch can
	 * drop it. */
	assert(strstr(buf, "\"cw_set_n\":4294967295,\"lw_to_admits\":4294967295,"));
	if(g_worst_heartbeat_only) puts(buf);
	assert(strstr(buf, "\"cpu_srv_us\":[4294967295,4294967295"));
	assert(strstr(buf, "\"cpu_nested_us\":[4294967295,4294967295"));
	assert(strstr(buf, "\"cpu_waits\":[4294967295,4294967295"));
	assert(strstr(buf, "\"cpu_preemptions_available\":false"));
	assert(strstr(buf, "\"cpu_valid\":4294967295"));
	assert(strstr(buf, "\"item_nbt_exhaustions\":4294967295"));
	assert(strstr(buf, "\"ocy\":4294967295"));
	/* colcap-1006: the carve-contiguity pair (valid here) now follows the
	 * storage block. */
	assert(strstr(buf, "\"dlc\":1,\"mem2_carve_largest_valid\":true,"
		"\"mem2_carve_largest_allocatable_bytes\":4294967295}"));
	hb.cpu.enabled = false;
	hb.anchor.frame_count = HW_ANCHOR_FRAME_CAP;
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf),
		"123456789012345678901234567890123456789012345678901234567890123",
		"split_anchor_6m", &hb);
	assert(n == old_size && !memcmp(old_cpu, buf, n + 1));
	test_rpcdrop = 0;


}

/* S-A3 (Astra H3, mp-split-0912/49): the live split toggle's four heartbeat
 * fields, driven through the states the Options row actually produces. The
 * DEFAULTS are what main.c fills from screen_options_split_live_status() on a
 * run that has toggled nothing -- 0/0/0/-1 -- and they have to be PRESENT:
 * "the key is missing" and "the toggle never happened" must not be the same
 * line, which is why the formatter emits all four unconditionally. */
static void test_heartbeat_reports_split_live_toggle(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	hb.split_last_ok = -1; /* the getter's "nothing has settled yet" */
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
		"heartbeat_selftest", &hb);
	assert(n > 0 && n < sizeof(buf) && strlen(buf) == n);
	assert(strstr(buf, "\"split_live\":0,\"split_pending\":0,"
					   "\"split_toggle_ms\":0,\"split_last_ok\":-1,"));
	/* A request in flight: pending, and the LIVE state is still the old one --
	 * the row reports the acknowledged value, never the one it is asking for,
	 * so a heartbeat sampled mid-toggle cannot claim the split is up. */
	hb.split_pending = true;
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
		"heartbeat_selftest", &hb);
	assert(n < sizeof(buf));
	assert(strstr(buf, "\"split_live\":0,\"split_pending\":1,"
					   "\"split_toggle_ms\":0,\"split_last_ok\":-1,"));
	/* The server said yes, 240 ms after the request left: this is the
	 * allocation-latency half of H3 (the MEM2-headroom half is
	 * mem2_carve_free_kb, already in every heartbeat). */
	hb.split_pending = false;
	hb.split_live = true;
	hb.split_toggle_ms = 240;
	hb.split_last_ok = 1;
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
		"heartbeat_selftest", &hb);
	assert(n < sizeof(buf));
	assert(strstr(buf, "\"split_live\":1,\"split_pending\":0,"
					   "\"split_toggle_ms\":240,\"split_last_ok\":1,"));
	/* Refused (the row's (b) case): the live state stayed OFF and the run
	 * records the refusal rather than an absence. */
	hb.split_live = false;
	hb.split_last_ok = 0;
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
		"heartbeat_selftest", &hb);
	assert(n < sizeof(buf));
	assert(strstr(buf, "\"split_live\":0,\"split_pending\":0,"
					   "\"split_toggle_ms\":240,\"split_last_ok\":0,"));
	/* The documented clamp: split_last_ok is a tri-state, and the formatter
	 * reserves two characters for it on that basis, so anything outside
	 * -1/0/1 must come out AT the domain edge rather than widening the line. */
	hb.split_last_ok = INT_MAX;
	hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
		"heartbeat_selftest", &hb);
	assert(strstr(buf, "\"split_last_ok\":1,"));
	hb.split_last_ok = INT_MIN;
	hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
		"heartbeat_selftest", &hb);
	assert(strstr(buf, "\"split_last_ok\":-1,"));
	puts("PASS heartbeat split toggle: split_live/split_pending/"
		 "split_toggle_ms/split_last_ok always present, default 0/0/0/-1, and "
		 "they follow request -> ack (240 ms, ok) / refused / clamped");
}

static void test_heartbeat_reports_mp_install_probe(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abc_08.29",
		.mp_state = 4,
		.mp_gen = 2,
		.mp_logged_in = 1,
		.mp_ret_kb = 193,
		.mp_ret_entries = 36,
		.mp_ready = 5,
		.mp_backlog = 31,
		.mp_evt_q = 7,
		.wl_pending = 1,
		.mp_rx_kb = 204,
		.mp_dec_phase = 9,
		.mp_dec_pkt = 51,
		.mp_dec_have = 213,
		.mp_dec_need = 18000,
	};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
											 "mp_connect_smoke", &hb);
	assert(n > 0 && n < sizeof(buf));
	assert(strstr(buf,
		"\"mp_state\":4,\"mp_gen\":2,\"mp_login\":1,\"mp_ret_kb\":193,"
		"\"mp_ret_n\":36,\"mp_ready\":5,\"mp_backlog\":31,\"mp_evt_q\":7,"
		"\"mp_evict\":0,\"mp_alloc_fail\":0,\"mp_reconn\":0,"
		"\"wl_pending\":1,\"wl_failed\":0,"
		"\"mp_rx_kb\":204,"
		// Live healthy session: nothing has ended, so the latch is all zero.
		"\"mp_end\":0,\"mp_end_phase\":0,\"mp_end_rx\":0,"
		// mp-split-0912/25: empty by contract while mp_end == 0.
		"\"mp_end_reason\":\"\","
		"\"mp_dec_phase\":9,\"mp_dec_pkt\":51,"
		"\"mp_dec_have\":213,\"mp_dec_need\":18000,\"mp_move_ticks\":0,\"mp_move_samples\":0,\"rpcdrop\":0,"
		// Pre-W4 session shape: this fixture predates the admission budget
		// and sets none of its fields, so all nine stay zero here. The
		// non-zero join-burst case is mp_admission_budget below.
		"\"mp_adm_radius\":0,\"mp_resident_cols\":0,"
		"\"mp_backed_cols\":0,\"mp_backing_kb\":0,"
		"\"mp_backing_peak_kb\":0,\"mp_demotions\":0,"
		"\"mp_compactions\":0,\"mp_backing_refusals\":0,"
		"\"mp_ready_pending\":0,"
		"\"music_key\":0,\"music_ms\":0,\"music_underruns\":0,"
		"\"music_starts\":0,\"music_rejects\":0,\"music_dur\":0,"
		"\"mrs_cycles\":0,\"mrs_target\":0,\"mrs_wedge\":false,"
		"\"creative\":false,\"dlcalls\":0,\"tris\":0,"
		"\"ms_mesh_worker\":0.00,\"ms_mesh_worker_max\":0.00,"
		"\"mem2_lock_wait\":0,"
		"\"tel_fail\":0,\"tel_last_errno\":0,\"tel_quiesced\":false,"
		"\"frame_counter\":0,\"mem2_carve_free_kb\":0,"
		"\"chunks_meshed\":0,\"near_missing\":0,\"near_mesh_pending\":0,"
		"\"near_missing_max\":0,"
		"\"near_settle_ms\":0,\"edit_to_visible_frames\":0,"
		"\"load_screen_ms\":0,\"player_health\":0,\"loading_screen\":false,\"perf_frames\":0,"));
}

/* Terminal-event latch, kicked leg (mp-split-0912/5). The case the field
 * exists for: an unattended run comes back DISCONNECTED and the heartbeat has
 * to say it was the SERVER that ended it, in which protocol phase, and after
 * how many bytes. mp_end_rx is deliberately not a multiple of 1024 and
 * smaller than one -- the existing mp_rx_kb would report 0 for the same
 * session, which is exactly why the exact count is carried separately. */
static void test_heartbeat_reports_mp_terminal_kick(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abc_09.12",
		.mp_state = 5,   /* REMOTE_KICKED */
		.mp_gen = 3,
		.mp_logged_in = 1,
		.mp_rx_kb = 0,   /* 54 bytes rounds to 0 KB */
		.mp_end = 1,     /* kicked by Packet255 */
		.mp_end_phase = 3, /* REMOTE_LOGIN_SENT */
		.mp_end_rx = 54,
		/* mp-split-0912/25: the reason text the three fields above cannot
		 * carry. Plain ASCII, no escape needed -- the escaping and the
		 * 47-character bound are pinned by
		 * test_heartbeat_mp_end_reason_escapes below. */
		.mp_end_reason = "Kicked by an operator.",
	};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
											 "mp_connect_smoke", &hb);
	assert(n > 0 && n < sizeof(buf));
	assert(strstr(buf,
		"\"mp_rx_kb\":0,"
		"\"mp_end\":1,\"mp_end_phase\":3,\"mp_end_rx\":54,"
		"\"mp_end_reason\":\"Kicked by an operator.\","
		"\"mp_dec_phase\":0,\"mp_dec_pkt\":0,"));
	assert(strstr(buf, "\"mp_state\":5,\"mp_gen\":3,\"mp_login\":1,"));
}

/* mp-split-0912/25: the reason TEXT is the heartbeat's first peer-supplied
 * string and its only variable-length field, so it gets its own case for the
 * two things that can go wrong with such a field: the ESCAPE and the BOUND.
 *
 * The escape leg uses a reason shaped like the ones Beta actually sends -- a
 * quoted phrase and a section sign, which is what the vanilla colour codes and
 * several stock kick messages carry. The section sign matters twice over: as
 * UTF-8 it is 0xC2 0xA7, two bytes BOTH >= 0x20, so a correct escaper passes it
 * through untouched and the collector gets real UTF-8 back. An escaper that
 * treated any byte with the high bit set as "control" would \u-expand it (and
 * produce mojibake); one that only looked at ASCII quoting would leave the bare
 * quote in and emit INVALID JSON, breaking the whole line, not just this field.
 * Both are pinned here by the exact expected substring.
 *
 * The bound leg proves the 48-byte storage is really a 47-character bound and
 * that the 48th byte is the NUL: a 60-character reason is truncated to its
 * first 47 characters, with nothing past them and no unterminated read. That is
 * the truncation main.c's snprintf performs on server_remote_status::reason
 * (256 B) on the way in; this test pins the FORMATTER's half of it -- that a
 * fully packed, NUL-less 48-byte field still emits exactly 47 characters. */
static void test_heartbeat_mp_end_reason_escapes(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abc_09.13",
		.mp_state = 5,  /* REMOTE_KICKED */
		.mp_gen = 1,
		.mp_end = 1,    /* kicked by Packet255 */
		.mp_end_phase = 4,
		.mp_end_rx = 921,
	};
	/* "You have been idle" with a quote, a backslash and a section sign. 47
	 * characters or fewer, so nothing is truncated on this leg. */
	snprintf(hb.mp_end_reason, sizeof(hb.mp_end_reason),
			 "%s", "\xc2\xa7""eIdle \"afk\" 30m\\x");
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
											 "mp_connect_smoke", &hb);
	assert(n > 0 && n < sizeof(buf));
	/* The quote and the backslash are escaped; the two section-sign bytes are
	 * NOT -- they are emitted verbatim as UTF-8. */
	assert(strstr(buf,
		"\"mp_end\":1,\"mp_end_phase\":4,\"mp_end_rx\":921,"
		"\"mp_end_reason\":\"\xc2\xa7" "eIdle \\\"afk\\\" 30m\\\\x\","
		"\"mp_dec_phase\":0,"));
	/* No raw quote survived inside the value -- i.e. the line is still one
	 * parseable JSON object. Counting quotes up to the next key is the cheap
	 * structural version of that claim. */
	const char* v = strstr(buf, "\"mp_end_reason\":");
	assert(v);
	const char* tail = strstr(v, ",\"mp_dec_phase\":");
	assert(tail);
	unsigned unescaped_quotes = 0;
	for(const char* c = v + strlen("\"mp_end_reason\":"); c < tail; c++) {
		if(*c == '\\') { c++; continue; }
		if(*c == '"') unescaped_quotes++;
	}
	assert(unescaped_quotes == 2); /* exactly the value's own delimiters */

	/* BOUND leg: 60 characters in, 47 out, with the 48th byte the NUL.
	 *
	 * The copy is spelled out as memcpy + explicit NUL rather than as the
	 * snprintf main.c uses. The two are equivalent for this input -- both
	 * write the first 47 characters and terminate -- and the post-condition
	 * asserted below (47 characters, NUL-terminated) is what the formatter
	 * under test actually consumes, so nothing about the claim depends on
	 * which one produced the bytes. It is written this way because
	 * -Wformat-truncation fires on an snprintf whose source length the
	 * compiler can prove, and a deliberate 60 -> 47 truncation is exactly
	 * that: the warning would be a false positive here, and silencing it
	 * with a pragma would also silence the real ones. */
	const char* long_reason =
		"012345678901234567890123456789012345678901234567890123456789";
	assert(strlen(long_reason) == 60);
	memcpy(hb.mp_end_reason, long_reason, sizeof(hb.mp_end_reason) - 1);
	hb.mp_end_reason[sizeof(hb.mp_end_reason) - 1] = '\0';
	assert(strlen(hb.mp_end_reason) == HW_TELEMETRY_MP_END_REASON_SIZE - 1);
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
									  "mp_connect_smoke", &hb);
	assert(n > 0 && n < sizeof(buf));
	assert(strstr(buf,
		"\"mp_end_reason\":\"01234567890123456789012345678901234567890123456\","
		"\"mp_dec_phase\":0,"));
	/* And the same 47-character ceiling when the field is packed to its last
	 * byte with NO terminator at all -- the formatter must bound its own scan,
	 * not trust the caller. */
	memset(hb.mp_end_reason, 'Z', sizeof(hb.mp_end_reason));
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
									  "mp_connect_smoke", &hb);
	assert(n > 0 && n < sizeof(buf));
	{
		char packed[15 + 1 + 2 + HW_TELEMETRY_MP_END_REASON_SIZE + 2];
		int at = snprintf(packed, sizeof(packed), "\"mp_end_reason\":\"");
		for(size_t i = 0; i < HW_TELEMETRY_MP_END_REASON_SIZE - 1; i++)
			packed[at++] = 'Z';
		at += snprintf(packed + at, sizeof(packed) - (size_t)at, "\",");
		assert((size_t)at == 17 + 47 + 2 && strlen(packed) == (size_t)at);
		assert(strstr(buf, packed));
	}

	/* And the contract that keeps the field honest: mp_end == 0 means the
	 * session has not ended, so no reason may be reported even when the status
	 * snapshot carries non-terminal diagnostic text. main.c enforces this on
	 * the fill side (it does not copy unless mp_status.mp_end), which is why
	 * this leg asserts the EMPTY emission for a zeroed heartbeat rather than
	 * calling into main.c. */
	struct hw_telemetry_heartbeat live = {.build_id = "abc_09.13"};
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
									  "mp_connect_smoke", &live);
	assert(n > 0 && n < sizeof(buf));
	assert(strstr(buf, "\"mp_end\":0,\"mp_end_phase\":0,\"mp_end_rx\":0,"
					   "\"mp_end_reason\":\"\",\"mp_dec_phase\":0,"));
	printf("PASS mp_end_reason: quote/backslash escaped, section sign verbatim,"
		   " 60 -> 47 chars, empty while mp_end==0\n");
}

/* W4 (design 2.7): the admission-budget block carries a REAL 441-column join
 * burst, not just zeros. The shape asserted here is the one the acceptance
 * run has to produce: radius 5 admits 121 columns at full detail
 * ((2*5+1)^2), the remaining 320 of the peer's 441 stay BACKED as compressed
 * bodies, the reservation's high-water is at or above its live use, demotions
 * and compactions have both moved, and backing_refusals is 0 -- a nonzero
 * refusal count is the clean mp_end=5 capacity failure the workload must not
 * reach. mp_ready_pending is nonzero because this sample is mid-join, while
 * the loading screen is still the thing deciding. */
static void test_heartbeat_reports_mp_admission_budget(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abc_09.13",
		.mp_state = 4, /* REMOTE_PLAY */
		.mp_gen = 1,
		.mp_logged_in = 1,
		.loading_screen = true,
		.mp_move_ticks = 43,
		.mp_move_samples = 42,
		.mp_adm_radius = 5,
		.mp_resident_cols = 121,
		.mp_backed_cols = 320,
		.mp_backing_kb = 2048,
		.mp_backing_peak_kb = 2313,
		.mp_demotions = 46,
		.mp_compactions = 3,
		.mp_backing_refusals = 0,
		.mp_ready_pending = 7,
	};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	test_rpcdrop = 1;
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "run",
											 "mp_lan_walk_10m", &hb);
	test_rpcdrop = 0;
	assert(n > 0 && n < sizeof(buf));
	assert(strstr(buf,
		"\"mp_adm_radius\":5,\"mp_resident_cols\":121,"
		"\"mp_backed_cols\":320,\"mp_backing_kb\":2048,"
		"\"mp_backing_peak_kb\":2313,\"mp_demotions\":46,"
		"\"mp_compactions\":3,\"mp_backing_refusals\":0,"
		"\"mp_ready_pending\":7,"));
	/* The block is emitted INSIDE the mp_ run, right after the decoder
	 * probe: a collector that keys on order rather than name still sees one
	 * contiguous MP region. */
	assert(strstr(buf,
		"\"mp_dec_have\":0,\"mp_dec_need\":0,\"mp_move_ticks\":43,\"mp_move_samples\":42,\"rpcdrop\":1,"
		"\"mp_adm_radius\":5,"));
	/* The admitted disk can never exceed what the radius allows -- the
	 * arithmetic the run's numbers are read against, asserted on the
	 * fixture so a future field reorder cannot quietly swap the two. */
	assert(hb.mp_resident_cols
		   <= (2u * hb.mp_adm_radius + 1u) * (2u * hb.mp_adm_radius + 1u));
	assert(hb.mp_backing_peak_kb >= hb.mp_backing_kb);
}

static void test_heartbeat_reports_truncation_for_small_buffer(void) {
	struct hw_telemetry_heartbeat hb = {
		.build_id = "abc_06.29",
		.world_loaded = true,
	};
	char buf[32];
	size_t n = hw_telemetry_format_heartbeat(
		buf, sizeof(buf), "run", "heartbeat_selftest", &hb);
	assert(n >= sizeof(buf));
	assert(buf[sizeof(buf) - 1] == '\0');
}

static void test_heartbeat_device_uptime(void) {
	struct hw_telemetry_heartbeat hb = {.tick = 1};
	char first[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	char second[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	s_stage_clock_us = 1234000;
	hw_telemetry_test_set_stage_clock(stage_test_now_us, NULL);
	hw_telemetry_format_heartbeat(first, sizeof(first), "run", "mp_pause_menu_3m", &hb);
	s_stage_clock_us = 2234000;
	hb.tick = 2;
	hw_telemetry_format_heartbeat(second, sizeof(second), "run", "mp_pause_menu_3m", &hb);
	hw_telemetry_test_set_stage_clock(NULL, NULL);
	assert(strstr(first, "\"up_ms\":1234"));
	assert(strstr(second, "\"up_ms\":2234"));
	puts("PASS heartbeat device up_ms: present and non-decreasing across rows");
}

static void test_dump_chunk_json_escapes_text(void) {
	char buf[512];
	size_t n = hw_telemetry_format_dump_chunk(
		buf, sizeof(buf), "run", "watchdog_inject", "watchdog_dump",
		"HANG", 3, 1, true, "line1\n\"line2\"", 13);
	assert(n < sizeof(buf));
	assert(strstr(buf, "\"event\":\"watchdog_dump\""));
	assert(strstr(buf, "\"last\":true"));
	assert(strstr(buf, "line1\\n\\\"line2\\\""));
}

struct sink_capture {
	// Sized to HW_TELEMETRY_HEARTBEAT_LINE_SIZE, not _DUMP_LINE_SIZE: this
	// generic capture sink (capture_line below) is shared by dump-chunk AND
	// heartbeat tests (e.g. the quiesce test emits real heartbeats through
	// it), so it must hold the larger of the two line kinds it might
	// actually receive. Latent: an all-zero heartbeat crossed 1000 bytes
	// only once the save_migrate chain-widening (2026-08-24) added six more
	// smig_* fields, but the truncation risk existed for any heartbeat this
	// sink ever captured -- caught by capture_line's own strlen assert, not
	// by inspection.
	char lines[8][HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	int count;
};

struct partial_send_capture {
	char bytes[2048];
	size_t len;
	size_t max_chunk;
	int calls;
};

static int partial_send_bytes(const char* data, size_t len, void* user) {
	struct partial_send_capture* cap = user;
	cap->calls++;
	size_t n = len < cap->max_chunk ? len : cap->max_chunk;
	assert(cap->len + n < sizeof(cap->bytes));
	memcpy(cap->bytes + cap->len, data, n);
	cap->len += n;
	return (int)n;
}

static int zero_send_bytes(const char* data, size_t len, void* user) {
	(void)data;
	(void)len;
	struct partial_send_capture* cap = user;
	cap->calls++;
	return 0;
}

static bool capture_line(const char* line, void* user) {
	struct sink_capture* cap = user;
	assert(cap->count < (int)(sizeof(cap->lines) / sizeof(cap->lines[0])));
	snprintf(cap->lines[cap->count], sizeof(cap->lines[cap->count]), "%s", line);
	assert(strlen(cap->lines[cap->count]) == strlen(line));
	cap->count++;
	return true;
}

static bool reject_line(const char* line, void* user) {
	(void)line;
	int* attempts = user;
	(*attempts)++;
	return false;
}

static void init_capture_telemetry(struct sink_capture* cap) {
	memset(cap, 0, sizeof(*cap));
	struct hw_telemetry_config cfg = {
		.enabled = true,
		.run_id = "dump_run",
		.profile = "watchdog_inject",
		.host = "pull",
		.port = 39017,
		.pull = true,
	};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_set_sink(capture_line, cap);
}

static void test_portal_quit_server_marker_wire(void) {
	struct sink_capture cap = {0};
	struct hw_telemetry_config cfg = {
		.enabled = true, .run_id = "portal_run",
		.profile = "split_portal_quit_4m", .host = "pull",
		.port = 39017, .pull = true,
	};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_set_sink(capture_line, &cap);
	uint32_t clock_before = hw_telemetry_portal_quit_up_ms();
	hw_telemetry_portal_quit_switch_begun();
	assert(hw_telemetry_emit_portal_quit_server("unload_seen", 81, "begun"));
	uint32_t clock_after = hw_telemetry_portal_quit_up_ms();
	assert(hw_telemetry_emit_portal_quit_server(
		"unload_dropped_at_barrier", 81, "barrier"));
	assert(cap.count == 2);
	assert(strstr(cap.lines[0], "\"marker\":\"unload_seen\""));
	assert(strstr(cap.lines[0], "\"switch\":\"begun\""));
	assert(strstr(cap.lines[0], "\"seq\":1"));
	assert(strstr(cap.lines[0], "\"tick\":81"));
	assert(strstr(cap.lines[0], "\"up_ms\":"));
	unsigned server_up_ms = 0;
	const char* up = strstr(cap.lines[0], "\"up_ms\":");
	assert(up && sscanf(up, "\"up_ms\":%u", &server_up_ms) == 1);
	assert(server_up_ms >= clock_before && server_up_ms <= clock_after);
	assert(strstr(cap.lines[0], "\"switch_begin_up_ms\":"));
	assert(strstr(cap.lines[0], "\"request_id\":null"));
	assert(strstr(cap.lines[1], "\"marker\":\"unload_dropped_at_barrier\""));
	assert(strstr(cap.lines[1], "\"switch\":\"barrier\""));
	assert(strstr(cap.lines[1], "\"seq\":2"));
	hw_telemetry_test_set_sink(NULL, NULL);
}

static void test_portal_quit_frame_clock_source(void) {
	/* Check the main-loop call site: its ctx feeds every ordinary client
	 * marker, while Confirm/Switch/server use this telemetry helper. */
	char path[1024];
	assert(snprintf(path, sizeof path, "%s", __FILE__) > 0);
	char* slash = strrchr(path, '/');
	char* backslash = strrchr(path, '\\');
	if(!slash || (backslash && backslash > slash)) slash = backslash;
	assert(slash);
	assert(snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path),
		"../source/main.c") > 0);
	FILE* source = fopen(path, "r");
	assert(source);
	char line[512];
	bool in_frame_call = false, found = false;
	while(fgets(line, sizeof line, source)) {
		if(strstr(line, "split_portal_quit_frame(&(struct split_portal_quit_ctx)"))
			in_frame_call = true;
		if(in_frame_call && strstr(line, ".up_ms =")) {
			found = strstr(line, "hw_telemetry_portal_quit_up_ms()") != NULL;
			break;
		}
	}
	fclose(source);
	assert(found);
}

static void test_emit_dump_chunks_real_send_path(void) {
	struct sink_capture cap;
	init_capture_telemetry(&cap);

	char text[HW_TELEMETRY_DUMP_TEXT_CHUNK * 2 + 6];
	memset(text, 'A', sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';
	hw_telemetry_emit_dump("HANG", text, false);

	assert(cap.count == 3);
	assert(strstr(cap.lines[0], "\"event\":\"watchdog_dump\""));
	assert(strstr(cap.lines[0], "\"seq\":0"));
	assert(strstr(cap.lines[0], "\"last\":false"));
	assert(strstr(cap.lines[1], "\"seq\":1"));
	assert(strstr(cap.lines[1], "\"last\":false"));
	assert(strstr(cap.lines[2], "\"seq\":2"));
	assert(strstr(cap.lines[2], "\"last\":true"));
	for(int i = 0; i < cap.count; i++)
		assert(strlen(cap.lines[i]) < HW_TELEMETRY_DUMP_LINE_SIZE);
	hw_telemetry_test_set_sink(NULL, NULL);
}

static void test_emit_empty_dump_sends_one_last_chunk(void) {
	struct sink_capture cap;
	init_capture_telemetry(&cap);

	hw_telemetry_emit_dump("EMPTY", "", true);

	assert(cap.count == 1);
	assert(strstr(cap.lines[0], "\"event\":\"fault\""));
	assert(strstr(cap.lines[0], "\"seq\":0"));
	assert(strstr(cap.lines[0], "\"last\":true"));
	assert(strstr(cap.lines[0], "\"text\":\"\""));
	hw_telemetry_test_set_sink(NULL, NULL);
}

static void test_send_line_retries_partial_tcp_writes(void) {
	struct partial_send_capture cap = {.max_chunk = 3};
	assert(hw_telemetry_test_send_line_with("abcdefghi", partial_send_bytes, &cap));
	assert(cap.calls == 4);
	assert(cap.len == 10);
	assert(memcmp(cap.bytes, "abcdefghi\n", 10) == 0);
}

static void test_send_line_completes_more_than_256_partial_writes(void) {
	char line[601];
	memset(line, 'x', sizeof(line) - 1);
	line[sizeof(line) - 1] = '\0';
	struct partial_send_capture cap = {.max_chunk = 1};
	assert(hw_telemetry_test_send_line_with(line, partial_send_bytes, &cap));
	assert(cap.calls == (int)sizeof(line));
	assert(cap.len == sizeof(line));
	assert(memcmp(cap.bytes, line, sizeof(line) - 1) == 0);
	assert(cap.bytes[sizeof(line) - 1] == '\n');
}

static void test_send_line_rejects_zero_progress(void) {
	struct partial_send_capture cap = {.max_chunk = 3};
	assert(!hw_telemetry_test_send_line_with("abc", zero_send_bytes, &cap));
	assert(cap.calls == 1);
}

/* Socket double: positive results are exact. A negative result can hide
 * queued bytes; the sender must never retry it. */
struct socket_send_double {
	char wire[HW_TELEMETRY_HEARTBEAT_LINE_SIZE + 2];
	size_t used;
	size_t hidden_on_error;
	const int* results;
	size_t result_count, next;
	size_t requests[64];
	unsigned calls;
};

static int socket_send(const char* data, size_t len, void* user) {
	struct socket_send_double* sock = user;
	assert(len > 0 && len <= 1024);
	assert(sock->calls < sizeof(sock->requests) / sizeof(sock->requests[0]));
	sock->requests[sock->calls++] = len;
	int n = sock->next < sock->result_count
		? sock->results[sock->next++] : (int)len;
	if(n > 0) {
		assert((size_t)n <= len);
		assert(sock->used + (size_t)n < sizeof(sock->wire));
		memcpy(sock->wire + sock->used, data, (size_t)n);
		sock->used += (size_t)n;
	}
	if(n <= 0 && sock->hidden_on_error) {
		assert(sock->hidden_on_error <= len);
		memcpy(sock->wire + sock->used, data, sock->hidden_on_error);
		sock->used += sock->hidden_on_error;
	}
	return n;
}

#include "test_hw_tel_sender.h"
#include "test_hw_tel_trace.h"
#include "test_hw_tel_wake.h"

static void make_send_line(char* line, size_t len) {
	// Nonuniform data makes a duplicated/skipped prefix visible to memcmp.
	for(size_t i = 0; i < len; i++)
		line[i] = (char)('!' + (i * 17 + i / 89) % 90);
	line[len] = '\0';
}

struct undrained_socket {
	size_t queued;
	unsigned calls;
};

static int undrained_send(const char* data, size_t len, void* user) {
	(void)data;
	struct undrained_socket* sock = user;
	size_t room = 4096 - sock->queued;
	sock->calls++;
	/* Model a finite IOS send buffer with NO drain during this burst.
	 * Like libogc, hide a queued prefix when the remaining suffix errors. */
	if(len > room) {
		sock->queued += room;
		return -EAGAIN;
	}
	sock->queued += len;
	return (int)len;
}

/* Replay captured records through the production sender, without assuming
 * that a healthy last heartbeat reports the outcome of the NEXT send. */
static void test_recorded_stream_death_evidence(void) {
	char path[1024];
	const char* slash = strrchr(__FILE__, '/');
	const char* backslash = strrchr(__FILE__, '\\');
	if(backslash && (!slash || backslash > slash)) slash = backslash;
	assert(slash);
	int n = snprintf(path, sizeof(path), "%.*s/fixtures/teldeath2_502eea93.ndjson",
		(int)(slash - __FILE__), __FILE__);
	assert(n > 0 && (size_t)n < sizeof(path));
	FILE* f = fopen(path, "rb");
	assert(f);
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	char last_hb[HW_TELEMETRY_HEARTBEAT_LINE_SIZE] = {0};
	char last_summary[1536] = {0};
	struct hw_telemetry_send_fail_state st = {0};
	unsigned rows = 0, heartbeats = 0, summaries = 0;
	size_t max_hb = 0;
	while(fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		assert(len && line[len - 1] == '\n');
		while(len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		struct socket_send_double sock = {0};
		bool close_socket = false;
		anchor_alloc_guard = true;
		anchor_alloc_calls = 0;
		bool ok = hw_telemetry_test_send_line_with_state(line, socket_send,
			&sock, &st, &close_socket);
		anchor_alloc_guard = false;
		assert(ok && !close_socket && !st.quiesced && !st.fail_count);
		assert(!anchor_alloc_calls);
		assert(sock.used == len + 1 && !memcmp(sock.wire, line, len));
		assert(sock.wire[len] == '\n');
		rows++;
		if(strstr(line, "\"event\":\"heartbeat\"")) {
			heartbeats++;
			if(len > max_hb) max_hb = len;
			memcpy(last_hb, line, len + 1);
		} else if(strstr(line, "\"event\":\"tps_summary\"")) {
			summaries++;
			assert(len < sizeof(last_summary));
			memcpy(last_summary, line, len + 1);
		}
	}
	assert(!ferror(f) && fclose(f) == 0);
	assert(rows == 64 && heartbeats == 57 && summaries == 6);
	assert(strstr(last_hb, "\"tel_fail\":0"));
	assert(strstr(last_hb, "\"tel_quiesced\":false"));
	struct undrained_socket bounded = {0};
	bool close_socket = false;
	assert(hw_telemetry_test_send_line_with_state(last_summary, undrained_send,
		&bounded, &st, &close_socket));
	assert(!hw_telemetry_test_send_line_with_state(last_hb, undrained_send,
		&bounded, &st, &close_socket));
	assert(bounded.queued == 4096 && !close_socket);
	assert(st.fail_count == 1 && st.last_errno == -EAGAIN);
	assert(!st.quiesced && st.resync); /* one transient never kills the stream */
	puts("PASS conditional reproduction: recorded summary + heartbeat exhaust an undrained 4096-byte socket; hardware drain pause is NOT measured");
	/* Both injected causes leave exactly the same last-good heartbeat.
	 * Neither is a measurement of the unobserved terminal Wii send. */
	{
		/* Hard errno: quiesces at once, and never retries the ambiguous bytes. */
		const int hard = -ENOMEM;
		struct socket_send_double sock = {.results = &hard, .result_count = 1};
		st = (struct hw_telemetry_send_fail_state){0};
		bool close_socket = false;
		assert(!hw_telemetry_test_send_line_with_state(last_hb, socket_send,
			&sock, &st, &close_socket));
		assert(st.fail_count == 1 && st.last_errno == -ENOMEM && st.quiesced);
		assert(close_socket);
		assert(!hw_telemetry_test_send_line_with_state(last_hb, socket_send,
			&sock, &st, &close_socket));
		assert(sock.calls == 1); // no negative retry; no subsequent heartbeat
	}
	{
		/* Transient back-pressure: each line is still abandoned (never
		 * retried), but the STREAM survives until the fourth consecutive
		 * strike (2026-09-07 fix round: the escalating 2/8/32ms pacing ladder
		 * now walks fully before the strike counter advances to quiesce -- see
		 * hw_telemetry.c's transient_pace_us doc comment for the resulting
		 * 42ms bound). This is the 2026-09-07 carve-observer stream-death fix
		 * -- one momentary -ENOBUFS used to silence telemetry ~60 s into every
		 * run that armed the attribution observer, while the game ran on. */
		const int transient[] = {-EAGAIN, -ENOBUFS, -EWOULDBLOCK, -EAGAIN};
		st = (struct hw_telemetry_send_fail_state){0};
		for(unsigned i = 0; i < 4; i++) {
			struct socket_send_double sock = {.results = &transient[i],
				.result_count = 1};
			bool close_socket = true;
			assert(!hw_telemetry_test_send_line_with_state(last_hb, socket_send,
				&sock, &st, &close_socket));
			assert(sock.calls == 1 && sock.used == 0); /* abandoned, not retried */
			assert(!close_socket && st.fail_count == i + 1 && st.resync);
			assert(st.last_errno == transient[i]);
			assert(st.quiesced == (i == 3)); /* four-strike, not first-strike */
		}
	}
	printf("PASS recorded stream: %u rows, %u heartbeats, %u summaries, max heartbeat %zu bytes; healthy replay does not die; injected errors quiesce after last tel_fail=0\n",
		rows, heartbeats, summaries, max_hb);
}

static void assert_send_wire(const struct socket_send_double* sock,
	const char* line, size_t len) {
	assert(sock->used == len + 1);
	assert(!memcmp(sock->wire, line, len));
	assert(sock->wire[len] == '\n');
}

static void test_send_line_partial_eagain(void) {
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE + 1];
	const size_t sizes[] = {4095, 4096, 4097, 8192, 8573, 9000,
		HW_TELEMETRY_HEARTBEAT_LINE_SIZE - 1,
		HW_TELEMETRY_HEARTBEAT_LINE_SIZE};
	for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		make_send_line(line, sizes[i]);
		struct socket_send_double full = {0};
		assert(hw_telemetry_test_send_line_with(line, socket_send, &full));
		assert_send_wire(&full, line, sizes[i]);
		assert(full.calls == (sizes[i] + 1023) / 1024 + 1);
		assert(full.requests[full.calls - 1] == 1); // separate newline
		const int results[] = {1022, 1, 1};
		struct socket_send_double sock = {.results = results,
			.result_count = sizeof(results) / sizeof(results[0])};
		struct hw_telemetry_send_fail_state st = {0};
		bool close_socket = true;
		assert(hw_telemetry_test_send_line_with_state(line, socket_send,
			&sock, &st, &close_socket));
		assert(!close_socket && st.fail_count == 0 && !st.quiesced);
		assert_send_wire(&sock, line, sizes[i]);
	}
	puts("PASS tel4k: exact 4096/8192 boundaries, 8573/9000/9216 bytes, "
		"positive partial -> suffix without loss/duplication");
}

static void test_send_line_failure_accounting(void) {
	char line[9001];
	make_send_line(line, 9000);
	const int reset[] = {1024, 123, -ECONNRESET};
	struct socket_send_double sock = {.results = reset, .result_count = 3};
	struct hw_telemetry_send_fail_state st = {0};
	bool close_socket = false;
	assert(!hw_telemetry_test_send_line_with_state(line, socket_send,
		&sock, &st, &close_socket));
	assert(sock.calls == 3 && sock.used == 1147);
	assert(!memcmp(sock.wire, line, sock.used));
	assert(close_socket && st.quiesced && st.fail_count == 1);
	assert(st.last_errno == -ECONNRESET);
	// Quiesced lines never queue a newline, a new heartbeat, or a retry.
	assert(!hw_telemetry_test_send_line_with_state("later", socket_send,
		&sock, &st, &close_socket));
	assert(sock.calls == 3 && st.fail_count == 1);

	const int exhausted[] = {1022, -EAGAIN, -EAGAIN, -EAGAIN, -EAGAIN,
		-EAGAIN, -EAGAIN, -EAGAIN, -EAGAIN};
	for(unsigned prefix = 0; prefix < 2; prefix++) {
		sock = (struct socket_send_double){.results = exhausted + !prefix,
			.result_count = 8 + prefix};
		hw_telemetry_send_fail_state_reset(&st);
		assert(!hw_telemetry_test_send_line_with_state(line, socket_send,
			&sock, &st, &close_socket));
		assert(sock.calls == 1 + prefix && sock.used == 1022u * prefix);
		assert(st.fail_count == 1 && st.last_errno == -EAGAIN);
		// 2026-09-07: ONE transient no longer kills the stream. The line is
		// still abandoned (never retried) and the next one resyncs; only the
		// three-strike rule in hw_telemetry_send_fail_classify quiesces.
		assert(!close_socket && !st.quiesced);
		assert(st.consec_fail == 1 && st.resync);
	}
	// Model libogc queuing an UNKNOWN prefix before returning EAGAIN.
	// The double below queues those bytes within the failed call, reporting only the error.
	const int ambiguous[] = {1024, 1024, 1024, -EAGAIN};
	sock = (struct socket_send_double){.results = ambiguous,
		.result_count = 4, .hidden_on_error = 1022};
	hw_telemetry_send_fail_state_reset(&st);
	assert(!hw_telemetry_test_send_line_with_state(line, socket_send, &sock, &st, &close_socket));
	assert(sock.calls == 4 && sock.used == 4094 && st.fail_count == 1);
	// The abandoned bytes are STILL never retried (libogc queues a partial
	// and reports only the error -- 2026-09-02 stage-fetch corruption). What
	// changed 2026-09-07: the stream survives. The next line is prefixed with
	// a bare LF, which closes the ambiguous prefix into exactly one malformed
	// NDJSON line (the collector skips isolated bad lines) instead of
	// silencing telemetry for the rest of the run.
	assert(!st.quiesced && !close_socket && st.resync && st.consec_fail == 1);
	assert(hw_telemetry_test_send_line_with_state("{\"event\":\"next\"}",
		socket_send, &sock, &st, &close_socket));
	assert(!st.resync && st.consec_fail == 0 && st.fail_count == 1);
	assert(!memcmp(sock.wire, line, 4094)); /* prefix intact, never rewritten */
	assert(sock.wire[4094] == '\n');        /* resync LF closes it */
	assert(!memcmp(sock.wire + 4095, "{\"event\":\"next\"}\n", 17));
	assert(sock.used == 4095 + 17);
	// A zero result fails immediately; never spin on a dribbling peer.
	const int dribble[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
	sock = (struct socket_send_double){.results = dribble, .result_count = 15};
	hw_telemetry_send_fail_state_reset(&st);
	assert(!hw_telemetry_test_send_line_with_state(line, socket_send,
		&sock, &st, &close_socket));
	assert(sock.calls == 1 && sock.used == 0 && st.fail_count == 1);
	assert(st.quiesced && close_socket);
	puts("PASS tel4k: reset mid-line, zero fails immediately, tel_fail exactly once, "
		"no later line appended to incomplete JSON");
}

static void test_send_line_newline_retry_budget(void) {
	// A zero SDK result can hide a prefix too. Never retry even the newline.
	const int newline[] = {3, 0, 1};
	struct socket_send_double sock = {.results = newline, .result_count = 3};
	struct hw_telemetry_send_fail_state st = {0};
	bool close_socket = false;
	assert(!hw_telemetry_test_send_line_with_state("abc", socket_send,
		&sock, &st, &close_socket));
	assert(sock.calls == 2 && sock.used == 3 && !memcmp(sock.wire, "abc", 3));
	assert(st.fail_count == 1 && st.last_errno == -EIO && st.quiesced && close_socket);
}


/* Compare the complete fixture token stream, including key/array order.
 * The supplied fixture has four 0.0 tokens where the pre-lane formatter
 * emits 0.00 (%.2f). Allow exactly that spelling difference; otherwise
 * every token byte must match. CPU mode must also preserve this wire prefix. */
static void assert_reference_tokens(const char* expected, const char* actual) {
	while(*expected && *actual) {
		if(*expected == '"') {
			assert(*actual++ == *expected++);
			do {
				assert(*expected && *actual == *expected);
				if(*expected == '"') break;
				expected++; actual++;
			} while(1);
		} else if(*expected == '-' || (*expected >= '0' && *expected <= '9')) {
			char *end_expected, *end_actual;
			double a = strtod(expected, &end_expected);
			double b = strtod(actual, &end_actual);
			assert(end_expected != expected && end_actual != actual && a == b);
			size_t en = (size_t)(end_expected - expected);
			size_t an = (size_t)(end_actual - actual);
			if(en == 3 && !memcmp(expected, "0.0", 3)) {
				assert(an == 4 && !memcmp(actual, "0.00", 4));
			} else assert(en == an && !memcmp(expected, actual, en));
			expected = end_expected; actual = end_actual;
			continue;
		} else assert(*expected == *actual);
		expected++; actual++;
	}
	assert(!*expected && !*actual);
}

static size_t strip_uptime_field(char* line) {
	char* start = strstr(line, ",\"up_ms\":");
	assert(start);
	char* end = strchr(start + 1, ',');
	assert(end);
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* B14's two zero-valued cadence counters postdate the captured reference. */
static size_t strip_mp_move_fields(char* line) {
	static const char fields[] = ",\"mp_move_ticks\":0,\"mp_move_samples\":0,\"rpcdrop\":0";
	char* start = strstr(line, fields);
	assert(start);
	memmove(start, start + sizeof(fields) - 1,
		strlen(start + sizeof(fields) - 1) + 1);
	return strlen(line);
}

/* The 2026-09-07 telemetry-sender lane appends six self-telemetry keys
 * (tel_drop/tel_it/tel_sends/tel_smax_us/tel_qmax/tel_hb_repl) between
 * sd_trace_over_30ms and srv_us, and the 2026-09-08 bounded-send lane a
 * seventh (tel_wto), and the 2026-09-08 sender-STAGE lane eight more
 * (tel_stg/tel_stg_us/tel_pmax_us/tel_xmax_us/tel_abd/tel_pret/tel_prev/
 * tel_sret). The captured de7dd71 fixture and the independently generated
 * e4cbcbe wire hash both predate them. Rather than re-pinning either
 * reference against this branch's own formatter -- which would retire the
 * guard instead of extending it -- splice exactly that run out of the live
 * line, asserting its precise shape first, and hold the remaining stream to
 * the original byte-for-byte contract. tel_pret and tel_sret carry raw
 * syscall returns and are therefore SIGNED, so the scanner accepts a leading
 * '-' -- the one shape difference from the fifteen unsigned counters. */
static size_t strip_sender_fields(char* line) {
	char* start = strstr(line, "\"tel_drop\":");
	assert(start);
	char* end = strstr(start, "\"srv_us\":[");
	assert(end && end > start);
	static const char* const keys[] = {"\"tel_drop\":", "\"tel_it\":",
		"\"tel_sends\":", "\"tel_smax_us\":", "\"tel_qmax\":",
		"\"tel_hb_repl\":", "\"tel_wto\":", "\"tel_stg\":",
		"\"tel_stg_us\":", "\"tel_pmax_us\":", "\"tel_xmax_us\":",
		"\"tel_abd\":", "\"tel_pret\":", "\"tel_prev\":",
		"\"tel_sret\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		if(*walk == '-') walk++; /* tel_pret / tel_sret */
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these fifteen keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* The 2026-09-08 server-loop wedge instrumentation lane (lane/carve-observer-
 * boot) inserts six new keys (srv_loop_it/srv_elapsed_ms/srv_ticks_pend/
 * srv_partial_m/srv_sleep_ms/srv_far_us_max) between srv_gen_backlog and
 * cflood, and the same day's lane/carve-observer-boot --cw-set follow-up
 * inserts a seventh, cw_set_n, right after them (still before cflood). The
 * captured de7dd71 fixture and the e4cbcbe wire hash both predate all seven
 * -- same splice-not-re-pin treatment as strip_sender_fields above.
 * srv_elapsed_ms/srv_partial_m/srv_sleep_ms/srv_far_us_max are SIGNED (int),
 * so the scanner accepts a leading '-', like tel_pret/tel_sret above;
 * cw_set_n is unsigned, like the other three of this run.
 *
 * push-0921/B4c appends an EIGHTH key to the same run, lw_to_admits (also
 * unsigned), immediately after cw_set_n and still before cflood -- same
 * splice-not-re-pin treatment, for the same reason: pinning the captured
 * de7dd71 fixture or the e4cbcbe wire hash against this branch's own
 * formatter would retire those guards rather than extend them. */
static size_t strip_srv_loop_fields(char* line) {
	char* start = strstr(line, "\"srv_loop_it\":");
	assert(start);
	char* end = strstr(start, "\"cflood\":");
	assert(end && end > start);
	static const char* const keys[] = {"\"srv_loop_it\":", "\"srv_elapsed_ms\":",
		"\"srv_ticks_pend\":", "\"srv_partial_m\":", "\"srv_sleep_ms\":",
		"\"srv_far_us_max\":", "\"cw_set_n\":", "\"lw_to_admits\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		if(*walk == '-') walk++; /* the four signed fields */
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these eight keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* The 2026-09-12 authoritative-kick lane (mp-split-0912/5) inserts three new
 * keys (mp_end/mp_end_phase/mp_end_rx) between mp_rx_kb and mp_dec_phase, so
 * an unattended run says HOW its MP session ended. The captured de7dd71
 * fixture and the e4cbcbe wire hash predate them, and the same
 * splice-don't-re-pin rule as the two helpers above applies: pinning either
 * reference against this branch's own formatter would retire the guard rather
 * than extend it. All three are unsigned, so no leading '-' is accepted. */
static size_t strip_mp_end_fields(char* line) {
	char* start = strstr(line, "\"mp_end\":");
	assert(start);
	char* end = strstr(start, "\"mp_dec_phase\":");
	assert(end && end > start);
	static const char* const keys[] = {"\"mp_end\":", "\"mp_end_phase\":",
		"\"mp_end_rx\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these three keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* mp-split-0912 W4 (design 2.7) inserts nine admission-budget keys between
 * mp_dec_need and music_key so a join burst reports its own residency and
 * memory high-water. The captured de7dd71 fixture and the e4cbcbe wire hash
 * predate them, and the same splice-don't-re-pin rule as the three helpers
 * above applies: re-pinning either reference against this branch's own
 * formatter would retire the guard instead of extending it. All nine are
 * unsigned, so no leading '-' is accepted, and the run must be exactly these
 * nine keys in this order -- a tenth field appended to the block without
 * updating this list fails here rather than silently widening the splice. */
static size_t strip_mp_admission_fields(char* line) {
	char* start = strstr(line, "\"mp_adm_radius\":");
	assert(start);
	char* end = strstr(start, "\"music_key\":");
	assert(end && end > start);
	static const char* const keys[] = {"\"mp_adm_radius\":",
		"\"mp_resident_cols\":", "\"mp_backed_cols\":", "\"mp_backing_kb\":",
		"\"mp_backing_peak_kb\":", "\"mp_demotions\":", "\"mp_compactions\":",
		"\"mp_backing_refusals\":", "\"mp_ready_pending\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these nine keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* mp-split-0912/25 inserts ONE key -- mp_end_reason -- between mp_end_rx
 * and mp_dec_phase, so a kick reports the reason TEXT and not just its
 * class. The captured de7dd71 fixture and the e4cbcbe wire hash predate it,
 * and the same splice-don't-re-pin rule as the helpers above applies. This
 * one is the first STRING-valued key any of them splice, so it validates
 * the value the way the others validate digits: an opening quote, then
 * bytes up to the closing quote with backslash escapes consumed in pairs
 * (so an escaped quote inside the reason cannot be mistaken for the
 * terminator), then the comma. It must be spliced BEFORE
 * strip_mp_end_fields, whose end anchor is mp_dec_phase and which would
 * otherwise see this key sitting in the middle of its run. */
static size_t strip_mp_end_reason_field(char* line) {
	char* start = strstr(line, "\"mp_end_reason\":");
	assert(start);
	char* end = strstr(start, "\"mp_dec_phase\":");
	assert(end && end > start);
	const char* walk = start + strlen("\"mp_end_reason\":");
	assert(*walk == '"');
	walk++;
	while(*walk && *walk != '"') {
		if(*walk == '\\') {
			walk++;
			assert(*walk); /* a trailing lone backslash is not valid JSON */
		}
		walk++;
	}
	assert(*walk == '"');
	walk++;
	assert(*walk == ',');
	walk++;
	assert(walk == end); /* the run is exactly this one key */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* mp-split-0912/49 (S-A3) inserts four ALWAYS-EMITTED keys -- split_live/
 * split_pending/split_toggle_ms/split_last_ok -- between worst_parts_us and
 * deaths, so a heartbeat says what the live split toggle was doing at the
 * moment it was sampled. The captured de7dd71 fixture and the independently
 * generated e4cbcbe wire hash both predate them, and the same
 * splice-don't-re-pin rule as the helpers above applies: assert the run's
 * exact shape, cut it out, and hold the remaining stream to its original
 * byte-for-byte contract. split_last_ok is SIGNED (-1 before anything has
 * settled), so the scanner accepts a leading '-', like tel_pret/tel_sret. */
static size_t strip_split_live_fields(char* line) {
	char* start = strstr(line, "\"split_live\":");
	assert(start);
	char* end = strstr(start, "\"deaths\":");
	assert(end && end > start);
	static const char* const keys[] = {"\"split_live\":", "\"split_pending\":",
		"\"split_toggle_ms\":", "\"split_last_ok\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		if(*walk == '-') walk++; /* split_last_ok's "nothing settled" value */
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these four keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* mp-split-0912/68 (S1-d) appends three keys -- act0, act1 and act_health --
 * at the very END of the anchor block, after anchor_damage_cause. The captured
 * de7dd71 fixture predates them, so the same splice-don't-re-pin rule as every
 * helper above applies: assert the run's exact shape (two 6-element unsigned
 * arrays then a 2-element SIGNED one -- act_health carries -1 for "this owner
 * has no body"), cut it out, and hold the rest of the stream to its original
 * byte-for-byte contract. Anchored on the separating comma and required to end
 * exactly at the anchor block's closing brace, so a field silently landing
 * between them fails here rather than being spliced away with the run. */
static size_t strip_split_action_fields(char* line) {
	char* start = strstr(line, ",\"act0\":[");
	assert(start);
	static const char* const keys[] = {",\"act0\":[", ",\"act1\":[",
		",\"act_health\":["};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		unsigned count = i < 2 ? HW_ANCHOR_ACTION_COUNT : 2u;
		for(unsigned k = 0; k < count; k++) {
			if(k) {
				assert(*walk == ',');
				walk++;
			}
			if(*walk == '-') walk++; /* act_health's no-body sentinel */
			assert(*walk >= '0' && *walk <= '9');
			while(*walk >= '0' && *walk <= '9') walk++;
		}
		assert(*walk == ']');
		walk++;
	}
	/* The run is exactly these three keys, and it is the TAIL of the anchor
	 * block: what follows is either that block's closing brace, or the comma
	 * of a later block (cpu/storage) that overwrote the brace with its own
	 * `off - 1` append. Anything else means a field landed inside the run. */
	assert(*walk == '}' || *walk == ',');
	memmove(start, walk, strlen(walk) + 1);
	return strlen(line);
}

/* push-0921 A11's five walker-proof keys, spliced out of the formatter's line
 * the same way every field-run added since the fixture was captured is: the
 * golden ndjson is a real de7dd71 console tape and predates them, so the
 * choice is splice-and-account or re-pin the fixture against this branch's own
 * formatter -- which would retire the guard rather than extend it. */
/* lava-aabb-0924/H1: the four chunk-save failure keys sit between
 * smig_spawners_verified and mp_state. reference_heartbeat predates them, so
 * all four are at their 0 default -- the same splice-not-re-pin treatment. */
static size_t strip_save_fail_fields(char* line) {
	char* start = strstr(line, "\"save_fail_n\":");
	assert(start);
	char* end = strstr(start, "\"mp_state\":");
	assert(end && end > start);
	static const char* const keys[] = {"\"save_fail_n\":", "\"save_fail_stage\":",
		"\"save_fail_sub\":", "\"save_skip_n\":", "\"save_idx_fb\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these five keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

/* G9 R6: the zero-valued light-queue array, an exact 59 B run (v2) between
 * "ndisp" and "wd_main". Spliced out wherever the TNT run is, so every older
 * key-run arithmetic and the reference fixture comparison stay
 * byte-for-byte. */
static size_t strip_lq_fields(char* line) {
	char* start = strstr(line, "\"lq\":[");
	assert(start);
	static const char zero[] = "\"lq\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
							   "0,0,0,0,0,0,0,0,0],";
	assert(sizeof(zero) - 1 == 59);
	assert(!strncmp(start, zero, sizeof(zero) - 1));
	/* I07: the zero-valued 14-entry unsaturated drain array follows lq, an
	 * exact 36 B run, spliced out with it. */
	static const char zero_d[]
		= "\"lqd_v\":3,\"lqd\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],";
	assert(sizeof(zero_d) - 1 == 62);
	assert(!strncmp(start + sizeof(zero) - 1, zero_d, sizeof(zero_d) - 1));
	assert(!strncmp(start + sizeof(zero) - 1 + sizeof(zero_d) - 1,
					"\"wd_main\":", 10));
	memmove(start, start + sizeof(zero) - 1 + sizeof(zero_d) - 1,
		strlen(start + sizeof(zero) - 1 + sizeof(zero_d) - 1) + 1);
	return strlen(line);
}

static size_t strip_tnt_fields(char* line) {
	(void)strip_lq_fields(line);
	char* start = strstr(line, ",\"tnt_valid\":");
	assert(start);
	static const char zero[] =
		",\"tnt_valid\":0,\"tnt_st\":0,\"tnt_bits\":0,"
		"\"tnt_primed_left\":0,\"tnt_carved\":0,\"tnt_pad\":0,"
		"\"tnt_gen\":0,\"tnt_tick\":0";
	assert(!strncmp(start, zero, sizeof(zero) - 1));
	memmove(start, start + sizeof(zero) - 1,
		strlen(start + sizeof(zero) - 1) + 1);
	return strlen(line);
}

static size_t strip_np_fields(char* line) {
	char* start = strstr(line, "\"np_entered\":");
	assert(start);
	char* end = strstr(start, "\"map_sign\":");
	assert(end && end > start);
	static const char* const keys[] = {"\"np_entered\":", "\"np_skipped\":",
		"\"np_focus\":", "\"np_flags\":", "\"np_rows\":"};
	const char* walk = start;
	for(unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i]);
		assert(!strncmp(walk, keys[i], n)); /* exact order, no extras */
		walk += n;
		if(*walk == '-') walk++; /* np_rows is signed */
		assert(*walk >= '0' && *walk <= '9');
		while(*walk >= '0' && *walk <= '9') walk++;
		assert(*walk == ',');
		walk++;
	}
	assert(walk == end); /* the run is exactly these five keys */
	memmove(start, end, strlen(end) + 1);
	return strlen(line);
}

static void test_real_wii_heartbeat_schema(void) {
	char path[1024];
	const char* slash = strrchr(__FILE__, '/');
	const char* backslash = strrchr(__FILE__, '\\');
	if(backslash && (!slash || backslash > slash)) slash = backslash;
	assert(slash);
	int written = snprintf(path, sizeof(path), "%.*s/fixtures/ref_heartbeat_de7dd71_tick200.ndjson",
		(int)(slash - __FILE__), __FILE__);
	assert(written > 0 && (size_t)written < sizeof(path));
	FILE* f = fopen(path, "rb");
	assert(f);
	char reference[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t ref_size = fread(reference, 1, sizeof(reference) - 1, f);
	assert(!ferror(f) && feof(f));
	assert(fclose(f) == 0);
	while(ref_size && (reference[ref_size - 1] == '\n' || reference[ref_size - 1] == '\r'))
		ref_size--;
	reference[ref_size] = 0;
	struct hw_telemetry_heartbeat hb = reference_heartbeat;
	char legacy[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(legacy, sizeof(legacy),
		"split_anchor_6m_de7dd71_s42_d256_0906_140819", "split_anchor_6m", &hb);
	assert(n >= ref_size && n < sizeof(legacy));
	n = strip_tnt_fields(legacy);
	n = strip_uptime_field(legacy);
	size_t move_bytes = n - strip_mp_move_fields(legacy);
	n -= move_bytes;
	/* S-A3's four keys first (mp-split-0912/49): a distinct anchor pair from
	 * every helper below, so the order is free -- stripped here to keep the
	 * arithmetic reading in wire order. */
	/* S1-d's three anchor-tail keys first (mp-split-0912/68): another
	 * distinct anchor pair, so the order is free here too. */
	size_t after_action = strip_split_action_fields(legacy);
	size_t action_bytes = n - after_action;
	size_t after_split = strip_split_live_fields(legacy);
	size_t split_bytes = after_action - after_split;
	size_t after_reason = strip_mp_end_reason_field(legacy);
	size_t reason_bytes = after_split - after_reason;
	size_t after_adm = strip_mp_admission_fields(legacy);
	size_t adm_bytes = after_reason - after_adm;
	size_t after_mp_end = strip_mp_end_fields(legacy);
	size_t mp_end_bytes = after_adm - after_mp_end;
	size_t after_loop = strip_srv_loop_fields(legacy);
	size_t loop_bytes = after_mp_end - after_loop;
	size_t sender_bytes = after_loop - strip_sender_fields(legacy);
	size_t after_sender = strlen(legacy);
	size_t np_bytes = after_sender - strip_np_fields(legacy);
	size_t after_np = strlen(legacy);
	size_t save_fail_bytes = after_np - strip_save_fail_fields(legacy);
	n -= (action_bytes + split_bytes + reason_bytes + adm_bytes + mp_end_bytes
		  + loop_bytes + sender_bytes + np_bytes + save_fail_bytes);
	/* Three zero-valued keys from mp-split-0912/68 (S1-d):
	 * ,"act0":[0,0,0,0,0,0] (21 B) + ,"act1":[0,0,0,0,0,0] (21 B)
	 * + ,"act_health":[0,0] (19 B). reference_heartbeat predates the
	 * per-owner action tallies entirely, so all three sit at their struct
	 * default -- the same splice-not-re-pin treatment as every run below. */
	assert(action_bytes == 61);
	/* Four zero-valued keys from mp-split-0912/49 (S-A3):
	 * "split_live":0, (15 B) + "split_pending":0, (18 B)
	 * + "split_toggle_ms":0, (20 B) + "split_last_ok":0, (18 B).
	 * reference_heartbeat predates the live toggle entirely, so all four sit
	 * at their struct default -- the same splice-not-re-pin treatment as the
	 * key runs below. */
	assert(split_bytes == 71);
	/* One empty-string key from mp-split-0912/25:
	 * "mp_end_reason":"", -- 15 B of quoted key + ':' + 2 quotes + ','.
	 * reference_heartbeat is a SINGLE-PLAYER split_anchor_6m capture with
	 * mp_end == 0, so the field is legitimately empty by its own contract,
	 * not merely unset. */
	assert(reason_bytes == 19);
	/* Nine zero-valued keys from mp-split-0912 W4 (design 2.7):
	 * "mp_adm_radius":0, (18 B) + "mp_resident_cols":0, (21)
	 * + "mp_backed_cols":0, (19) + "mp_backing_kb":0, (18)
	 * + "mp_backing_peak_kb":0, (23) + "mp_demotions":0, (17)
	 * + "mp_compactions":0, (19) + "mp_backing_refusals":0, (24)
	 * + "mp_ready_pending":0, (21). reference_heartbeat is a SINGLE-PLAYER
	 * split_anchor_6m capture, so every one of them is legitimately 0 --
	 * there was no MP session and no compressed body was ever allocated. */
	assert(adm_bytes == 180);
	/* Three zero-valued keys from the 2026-09-12 authoritative-kick lane:
	 * "mp_end":0, (11 B) + "mp_end_phase":0, (17 B) + "mp_end_rx":0, (14 B).
	 * reference_heartbeat leaves them at their 0 default, like both key runs
	 * below. */
	assert(mp_end_bytes == 42);
	/* Fifteen zero-valued keys: the original seven (95 B) plus the
	 * 2026-09-08 stage run's eight, which at value 0 cost
	 * 12+15+16+16+12+13+13+13 = 110 B. */
	assert(sender_bytes == 205);
	/* Eight zero-valued keys: the 2026-09-08 server-loop wedge
	 * instrumentation lane's six (108 B) plus the same day's --cw-set
	 * follow-up's cw_set_n (13 B, "cw_set_n":0,) plus push-0921/B4c's
	 * lw_to_admits (17 B, "lw_to_admits":0,) -- reference_heartbeat leaves
	 * them all at their 0 default, same as the sender-stage keys above. */
	assert(loop_bytes == 138);
	/* Five zero-valued keys from push-0921 A11 (review A9-3):
	 * ,"np_entered":0 (15 B) + "np_skipped":0, (15) + "np_focus":0, (13)
	 * + "np_flags":0, (13) + "np_rows":0, (12). reference_heartbeat predates
	 * the walker-proof fields entirely, and a heartbeat from any profile but
	 * menus_dwell leaves them at that 0 default anyway -- menu_autopilot's
	 * getters all return 0 when the walker was never armed. */
	assert(np_bytes == 68);
	/* Four zero-valued keys from lava-aabb-0924/H1 (chunk-save failure
	 * visibility): "save_fail_n":0, (16 B) + "save_fail_stage":0, (20)
	 * + "save_fail_sub":0, (18) + "save_skip_n":0, (16) -- each quoted key +
	 * ':' + '0' + ','. reference_heartbeat predates them; a run with no
	 * failed save leaves them at 0 anyway. +16 for the H2 review fix (4)
	 * key "save_idx_fb":0, -> 86. */
	assert(save_fail_bytes == 86);
	/* Includes all 173 keys in fixture order, and every captured value. */
	assert_reference_tokens(reference, legacy);
	hb.cpu.enabled = true;
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t m = hw_telemetry_format_heartbeat(line, sizeof(line),
		"split_anchor_6m_de7dd71_s42_d256_0906_140819", "split_anchor_6m", &hb);
	assert(m >= ref_size && m < sizeof(line));
	m = strip_tnt_fields(line);
	m = strip_uptime_field(line);
	assert(m - strip_mp_move_fields(line) == move_bytes);
	m -= move_bytes;
	m -= (action_bytes + split_bytes + reason_bytes + adm_bytes + mp_end_bytes
		  + loop_bytes + sender_bytes + np_bytes + save_fail_bytes);
	assert(strip_split_action_fields(line)
		   == m + split_bytes + reason_bytes + adm_bytes + mp_end_bytes
			   + loop_bytes + sender_bytes + np_bytes + save_fail_bytes);
	assert(strip_split_live_fields(line)
		   == m + reason_bytes + adm_bytes + mp_end_bytes + loop_bytes
			   + sender_bytes + np_bytes + save_fail_bytes);
	assert(strip_mp_end_reason_field(line)
		   == m + adm_bytes + mp_end_bytes + loop_bytes + sender_bytes
			   + np_bytes + save_fail_bytes);
	assert(strip_mp_admission_fields(line)
		   == m + mp_end_bytes + loop_bytes + sender_bytes + np_bytes
			   + save_fail_bytes);
	assert(strip_mp_end_fields(line)
		   == m + loop_bytes + sender_bytes + np_bytes + save_fail_bytes);
	assert(strip_srv_loop_fields(line)
		   == m + sender_bytes + np_bytes + save_fail_bytes);
	assert(strip_sender_fields(line) == m + np_bytes + save_fail_bytes);
	assert(strip_np_fields(line) == m + save_fail_bytes);
	assert(m == strip_save_fail_fields(line) && m > n); /* H1 keys */
	assert(!memcmp(line, legacy, n - 1));
	assert(!strcmp(line + n - 1,
		",\"cpu_clock_step_ns\":0,\"cpu_valid\":0,\"cpu_ticks\":0,"
		"\"cpu_srv_us\":[0,0,0,0,0,0,0,0,0,0,0],"
		/* TPS_PART_COUNT wide, loop-emitted: 22 since TPS_REGION_PRESERVE was
		 * APPENDED (2026-09-08). This is the opt-in CPU extension, not the 173-key
		 * legacy line the captured real-Wii fixture pins, so the fixture and
		 * test_send_legacy_heartbeat_wire_identical are untouched. */
		"\"cpu_nested_us\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"
		"\"cpu_waits\":[0,0,0,0,0,0],\"cpu_main_us\":0,\"cpu_mesher_us\":0,"
		"\"cpu_frames\":0,\"cpu_preemptions_available\":false,"
		"\"cpu_packed_us\":0,\"cpu_clock_mode\":0}"));
	/* Therefore keys(line) == keys(reference) + exactly these twelve CPU keys. */
	struct socket_send_double sock = {0};
	assert(hw_telemetry_test_send_line_with(line, socket_send, &sock));
	assert_send_wire(&sock, line, m);
	printf("PASS real-Wii heartbeat: 173 legacy + 12 CPU keys; reference %zu, legacy %zu, CPU %zu bytes; budget %zu B/s\n",
		ref_size, n, m, m + 1 + 1920);
}

static void test_send_legacy_heartbeat_wire_identical(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t len = hw_telemetry_format_heartbeat(line, sizeof(line), "legacy",
		"heartbeat_selftest", &hb);
	assert(len < sizeof(line));
	len = strip_tnt_fields(line);
	len = strip_uptime_field(line);
	const size_t full_len = len;
	len = strip_mp_move_fields(line);
	const size_t after_move_len = len;
	len = strip_split_live_fields(line); /* mp-split-0912/49 S-A3 toggle keys */
	const size_t after_split_len = len;
	len = strip_mp_end_reason_field(line); /* mp-split-0912/25 reason text */
	const size_t after_reason_len = len;
	len = strip_mp_admission_fields(line); /* W4 admission-budget keys */
	const size_t after_adm_len = len;
	len = strip_mp_end_fields(line); /* 2026-09-12 authoritative-kick keys */
	const size_t after_mp_end_len = len;
	len = strip_srv_loop_fields(line); /* 2026-09-08 server-loop wedge keys */
	const size_t after_loop_len = len;
	len = strip_sender_fields(line); /* see the helper: 2026-09-07 sender keys */
	const size_t after_sender_len = len;
	len = strip_np_fields(line); /* push-0921 A11 walker-proof keys */
	const size_t after_np_len = len;
	len = strip_save_fail_fields(line); /* lava-aabb-0924/H1 save-failure keys */
	/* Same 205 B of zero-valued sender keys the schema test above splices
	 * out (95 for the original seven, 110 for the 2026-09-08 stage eight),
	 * plus 138 B of zero-valued server-loop-wedge-plus-cw_set_n-plus-
	 * lw_to_admits keys (also spliced out above: 108 for the six original
	 * server-loop fields, +13 for the same day's --cw-set follow-up's
	 * cw_set_n, +17 for push-0921/B4c's lw_to_admits).
	 * The e4cbcbe wire hash below is held byte-for-byte on what remains --
	 * the SAME remainder as before this lane, since both new key runs are
	 * fully spliced out before the hash. */
	/* Plus 42 B of zero-valued mp_end/mp_end_phase/mp_end_rx (the 2026-09-12
	 * authoritative-kick lane), spliced out first and likewise absent from
	 * the hashed remainder. */
	/* And 180 B of zero-valued W4 admission-budget keys (design 2.7), the
	 * same nine the schema test splices out: stripped FIRST here, so the
	 * e4cbcbe remainder this test hashes is byte-identical to what it was
	 * before W4. */
	/* And 19 B of the empty mp_end_reason (mp-split-0912/25): this fixture is
	 * a zero-filled heartbeat, so mp_end == 0 and the reason is empty by
	 * contract. Stripped FIRST, so the e4cbcbe remainder this test hashes is
	 * byte-identical to what it was before this lane. */
	/* And 71 B of the four zero-valued S-A3 split-toggle keys
	 * (mp-split-0912/49) -- this fixture is a zero-filled heartbeat, so the
	 * toggle is untouched by contract. Stripped FIRST, so the e4cbcbe
	 * remainder this test hashes is byte-identical to what it was before this
	 * lane. */
	assert(full_len - after_move_len == 50);
	assert(after_move_len - after_split_len == 71);
	assert(after_split_len - after_reason_len == 19);
	assert(after_reason_len - after_adm_len == 180);
	assert(after_adm_len - after_mp_end_len == 42);
	assert(after_mp_end_len - after_loop_len == 138);
	assert(after_loop_len - after_sender_len == 205);
	/* And 68 B of the five zero-valued walker-proof keys (push-0921 A11):
	 * this fixture is a zero-filled heartbeat under heartbeat_selftest, where
	 * the walker was never armed and every menu_autopilot getter returns 0 by
	 * contract. Spliced out like every run above, so the e4cbcbe remainder
	 * this test hashes stays byte-identical to what it was before the lane. */
	assert(after_sender_len - after_np_len == 68);
	/* And 86 B of the five zero-valued chunk-save keys (lava-aabb-0924/H1's
	 * four + the H2 review fix (4) save_idx_fb): a zero-filled heartbeat has
	 * no failed save and no index fallback, so all five are 0 by contract.
	 * Spliced out like every run above; the e4cbcbe remainder stays
	 * byte-identical. */
	assert(after_np_len - len == 86);
	// Independently generated from e4cbcbe's formatter (not this branch).
	uint64_t hash = UINT64_C(14695981039346656037);
	for(size_t i = 0; i < len; i++) {
		hash ^= (unsigned char)line[i];
		hash *= UINT64_C(1099511628211);
	}
	assert(len == 2480 && hash == UINT64_C(0xe4e4fe3898812136));
	struct socket_send_double sock = {0};
	assert(hw_telemetry_test_send_line_with(line, socket_send, &sock));
	// The old send_line_with emitted exactly formatter bytes plus one LF.
	assert_send_wire(&sock, line, len);
	// Pin the sub-4K contract independently of future formatter growth.
	static const char legacy[] =
		"{\"event\":\"heartbeat\",\"run_id\":\"legacy\",\"frame\":50,\"tel_fail\":0}";
	sock = (struct socket_send_double){0};
	assert(sizeof(legacy) < 4096);
	memset(&tel_test_ring, 0, sizeof(tel_test_ring));
 assert(cw_tel_publish(&tel_test_ring, legacy));
 assert(hw_telemetry_test_send_line_with(cw_tel_peek(&tel_test_ring), socket_send, &sock));
 cw_tel_pop(&tel_test_ring);
	assert(sock.calls == 2 && sock.requests[0] == sizeof(legacy) - 1);
	assert_send_wire(&sock, legacy, sizeof(legacy) - 1);
	puts("PASS tel4k: legacy heartbeat wire byte-identical, including one LF");
}

static void test_tnt_verdict_field_is_always_formatted(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(line, sizeof line, "tnt-red",
		"heartbeat_selftest", &hb);
	assert(n > 0 && n < sizeof line);
	assert(strstr(line, "\"tnt_st\":") != NULL);
}

static void test_watchdog_dump_skips_tcp_when_send_lock_busy(void) {
	struct sink_capture cap;
	init_capture_telemetry(&cap);

	hw_telemetry_test_set_send_lock_busy(true);
	hw_telemetry_emit_dump("HANG", "busy", true);
	assert(cap.count == 0);
	assert(hw_telemetry_test_send_lock_blocks() == 1);

	hw_telemetry_test_set_send_lock_busy(false);
	hw_telemetry_emit_dump("HANG", "free", true);
	assert(cap.count == 1);
	assert(strstr(cap.lines[0], "\"event\":\"fault\""));
	assert(strstr(cap.lines[0], "\"text\":\"free\""));
	assert(hw_telemetry_test_send_lock_blocks() == 0);

	hw_telemetry_test_set_sink(NULL, NULL);
}

struct preconnect_capture {
	char bytes[131072]; /* O20: a whole packed preconnect arena fits */
	size_t len;
	int fail_after_lines; // -1 = never fail; else fail once N full lines sent
};

static int preconnect_send_bytes(const char* data, size_t len, void* user) {
	struct preconnect_capture* cap = user;
	if(cap->fail_after_lines >= 0) {
		int newlines = 0;
		for(size_t i = 0; i < cap->len; i++)
			if(cap->bytes[i] == '\n')
				newlines++;
		if(newlines >= cap->fail_after_lines)
			return -1;
	}
	assert(cap->len + len < sizeof(cap->bytes));
	memcpy(cap->bytes + cap->len, data, len);
	cap->len += len;
	return (int)len;
}

static void test_preconnect_buffer_pins_boot_and_rings_rest(void) {
	/* O20: small lines no longer compete for eight 12 KB slots -- ten short
	 * lines all fit the packed arena and flush in order, none dropped. */
	hw_telemetry_test_preconnect_reset();
	char name[8];
	for(int i = 1; i <= 10; i++) {
		snprintf(name, sizeof(name), "L%d", i);
		assert(!hw_telemetry_test_preconnect_store(name));
	}
	assert(hw_telemetry_test_preconnect_count() == 10);
	static struct preconnect_capture cap;
	memset(&cap, 0, sizeof cap);
	cap.fail_after_lines = -1;
	unsigned sent = 0;
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 10);
	cap.bytes[cap.len] = '\0';
	assert(strcmp(cap.bytes, "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10\n") == 0);
	// One-shot: the buffer is retired after the first flush...
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 0);
	// ...and post-flush stores are no-ops.
	hw_telemetry_test_preconnect_store("late");
	assert(hw_telemetry_test_preconnect_count() == 10);
}

/* O20 (real-Wii HW1/HW2 boat smokes): the vehicle profile emits a boot burst
 * of 12+ lines before the pull collector connects -- boot, audio_load, a
 * heartbeat, freeze_diag, rpc_watch, audio_deferred_scan, three thread_diag,
 * run_start, phase_begin, another heartbeat. The old 8 x 12 KB slot buffer
 * evicted the oldest ring lines (the thread_diags), the eviction counted as
 * tel_drop, and host_vehicle_smoke_8m's collector fails on ANY tel_drop at
 * the next heartbeat -- every hardware vehicle run died there (~7.6 s). The
 * same 96 KB, packed, must carry the whole burst: every line, in order, and
 * no store reports a drop. */
/* O20: the vehicle driver's verdict rides the heartbeat (the vehicle
 * collector stops on the first heartbeat with a fault counter, before any
 * later row), only when set; the line stays one well-formed JSON object. */
static void test_heartbeat_vs_status(void) {
	static char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	struct hw_telemetry_heartbeat hb = {.tick = 7};
	size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "r", "host_vehicle_smoke_8m", &hb);
	assert(n < sizeof(buf) && !strstr(buf, "vs_status"));
	hb.vs_status = "fail:rig_world_create_refused";
	n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "r", "host_vehicle_smoke_8m", &hb);
	assert(n < sizeof(buf));
	assert(strstr(buf, ",\"vs_status\":\"fail:rig_world_create_refused\"}"));
	assert(buf[n - 1] == '}' && strchr(buf, '}') != NULL);
	size_t opens = 0, closes = 0;
	for(size_t i = 0; i < n; i++) {
		opens += buf[i] == '{';
		closes += buf[i] == '}';
	}
	assert(opens == closes);
	puts("PASS heartbeat vs_status: carried only when set, valid JSON object");
}

static void test_preconnect_vehicle_boot_burst_fits(void) {
	hw_telemetry_test_preconnect_reset();
	static char hb[4200];
	memset(hb, 'h', sizeof hb - 1);
	hb[sizeof hb - 1] = '\0';
	hb[0] = 'H';
	static char audio[1200];
	memset(audio, 'a', sizeof audio - 1);
	audio[sizeof audio - 1] = '\0';
	const char* burst[12] = {"boot", audio, hb, "freeze_diag", "rpc_watch",
		"audio_deferred_scan", "thread_diag1", "thread_diag2", "thread_diag3",
		"run_start", "phase_begin", hb};
	for(int i = 0; i < 12; i++)
		assert(!hw_telemetry_test_preconnect_store(burst[i])
			   && "a vehicle boot burst must not evict (tel_drop stays 0)");
	static struct preconnect_capture cap;
	memset(&cap, 0, sizeof cap);
	cap.fail_after_lines = -1;
	unsigned sent = 0;
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 12);
	cap.bytes[cap.len] = '\0';
	const char* at = cap.bytes;
	for(int i = 0; i < 12; i++) {
		size_t n = strlen(burst[i]);
		assert(strncmp(at, burst[i], n) == 0 && at[n] == '\n');
		at += n + 1;
	}
	assert(*at == '\0');
}

/* O20: a genuinely full arena still pins the boot line, evicts the OLDEST
 * other lines, and reports every eviction as a drop (the counter keeps its
 * meaning: a record that never reached the wire). */
/* R08 F4: one store can evict SEVERAL packed records; every evicted record
 * is a lost record and must count (R08's counterexample, verbatim). */
static void test_preconnect_multi_eviction_counts_each(void) {
	hw_telemetry_test_preconnect_reset();
	static char mid[1536], big[12288];
	memset(mid, 'm', sizeof mid - 1);
	mid[sizeof mid - 1] = '\0';
	memset(big, 'b', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	assert(hw_telemetry_test_preconnect_store_n("boot") == 0);
	for(int i = 0; i < 63; i++)
		assert(hw_telemetry_test_preconnect_store_n(mid) == 0);
	const unsigned evicted = hw_telemetry_test_preconnect_store_n(big);
	printf("F4: one heartbeat-sized store evicted %u records\n", evicted);
	assert(evicted == 8 && "every evicted record is counted, not one per call");
	assert(hw_telemetry_test_preconnect_count() == 1u + 63u + 1u);
	puts("PASS R08 F4: multi-eviction counts each lost record");
}

static void test_preconnect_full_arena_evicts_oldest_and_reports(void) {
	hw_telemetry_test_preconnect_reset();
	static char big[11000];
	assert(!hw_telemetry_test_preconnect_store("boot"));
	unsigned drops = 0;
	for(int i = 0; i < 12; i++) {
		memset(big, 'a' + i, sizeof big - 1);
		big[sizeof big - 1] = '\0';
		if(hw_telemetry_test_preconnect_store(big))
			drops++;
	}
	assert(drops > 0 && "an over-full arena must report its evictions");
	static struct preconnect_capture cap;
	memset(&cap, 0, sizeof cap);
	cap.fail_after_lines = -1;
	unsigned sent = 0;
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 1u + 12u - drops);
	cap.bytes[cap.len] = '\0';
	assert(strncmp(cap.bytes, "boot\n", 5) == 0 && "boot stays pinned");
	/* the survivors are the NEWEST, in order: the last line is the 'l' run */
	const char* last = cap.bytes + cap.len - 2;
	assert(*last == 'a' + 11);
	const char* first_kept = cap.bytes + 5;
	assert(*first_kept == (char)('a' + (int)drops));
}

static void test_preconnect_buffer_partial_fill_keeps_order(void) {
	hw_telemetry_test_preconnect_reset();
	hw_telemetry_test_preconnect_store("boot");
	hw_telemetry_test_preconnect_store("hb1");
	hw_telemetry_test_preconnect_store("hb2");
	struct preconnect_capture cap = {.fail_after_lines = -1};
	unsigned sent = 0;
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 3);
	cap.bytes[cap.len] = '\0';
	assert(strcmp(cap.bytes, "boot\nhb1\nhb2\n") == 0);
}

static void test_preconnect_flush_failure_stops_and_retires(void) {
	hw_telemetry_test_preconnect_reset();
	hw_telemetry_test_preconnect_store("boot");
	hw_telemetry_test_preconnect_store("hb1");
	hw_telemetry_test_preconnect_store("hb2");
	struct preconnect_capture cap = {.fail_after_lines = 1};
	// Send fails once one full line is out: flush reports FAILURE (the caller
	// must tear the client down -- the stream is poisoned), only "boot"
	// counted as sent, and the buffer still retires (no duplicate re-flush).
	unsigned sent = 0;
	assert(!hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 1);
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 0);
}

static void test_quiesce_stops_all_sends_until_reinit(void) {
	struct sink_capture cap;
	init_capture_telemetry(&cap);
	assert(!hw_telemetry_quiesced());
	hw_telemetry_emit_profile_done("ok");
	assert(cap.count == 1);
	// After quiesce, EVERY emit is a no-op -- heartbeat sends (and their
	// failure-teardown net calls) must be unreachable during the auto-exit
	// grace window.
	observer_close_checks = 1;
	hw_telemetry_quiesce();
	assert(observer_close_checks == 2);
	observer_close_checks = 0;
	assert(hw_telemetry_quiesced());
	hw_telemetry_emit_profile_done("ok");
	hw_telemetry_emit_dump("late_dump", "text", false);
	assert(cap.count == 2); /* profile_done and seal, no late records */
	// A fresh init is a fresh session: quiescence does not leak into it.
	struct sink_capture cap2;
	init_capture_telemetry(&cap2);
	assert(!hw_telemetry_quiesced());
	hw_telemetry_emit_profile_done("ok");
	assert(cap2.count == 1);
	observer_close_checks = 1;
	hw_telemetry_shutdown();
	assert(observer_close_checks == 2 && cap2.count == 2);
	observer_close_checks = 0;
	hw_telemetry_test_set_sink(NULL, NULL);
}

static void test_established_client_send_failure_quiesces_immediately(void) {
	// task_08716cec (2026-08-22 hw_run auto-exit strand fix). Root cause:
	// tmp/wii_runs/staged_far_walk_6m_20260822_124510 (far_walk_6m, 360s
	// duration) died with the collector's last received heartbeat at 261
	// (elapsed 293.8s) -- the NEXT heartbeat's send against the already-
	// ESTABLISHED client failed, and the OLD code's inline net_close() right
	// there is the exact main-thread wedge vector
	// docs/endgame_progress.md's 2026-08-18 investigation already named
	// ("net_send + net_close ON THE MAIN THREAD"), previously guarded ONLY
	// for the post-"ok" trailing grace window. The console was found
	// in-game 30+ minutes later, never reaching main.c's
	// hw_run_auto_exit_ready() check. Fix: budget = 1 (not N) -- one
	// established-client send failure quiesces immediately, exactly like
	// the already-trusted post-"ok" quiesce, generalized to fire the
	// instant there is any evidence the collector is gone rather than only
	// once the run's own duration has elapsed.
	struct sink_capture cap;
	init_capture_telemetry(&cap);
	assert(!hw_telemetry_quiesced());

	hw_telemetry_emit_heartbeat(&(struct hw_telemetry_heartbeat){0});
	assert(cap.count == 1); // heartbeat 1: collector alive, delivered fine
	assert(!hw_telemetry_quiesced());

	// Simulates the Wii-only trigger: heartbeat 2's send fails against the
	// established client (the collector just died).
	hw_telemetry_test_note_established_client_send_failure();
	assert(hw_telemetry_quiesced());

	// Every further emit -- heartbeats, the run's own profile_done("ok"),
	// even a late crash-mailbox dump -- must now be a pure no-op: nothing
	// reaches the sink (i.e. nothing would reach net_send/net_accept/
	// net_close on real hardware), so nothing on the exit path can block.
	hw_telemetry_emit_heartbeat(&(struct hw_telemetry_heartbeat){0});
	hw_telemetry_emit_profile_done("ok");
	hw_telemetry_emit_dump("late", "text", false);
	assert(cap.count == 1);
}

// T-a (2026-09-03, Round 3 (3) "walk wedge" telemetry self-quiesce fix): the
// pure classify state machine, tested directly (no socket, no PLATFORM_WII
// build needed -- see hw_telemetry.h's struct hw_telemetry_send_fail_state
// doc comment for why this is the injectable seam instead of driving it
// through send_line_mode, whose real Wii accept/close logic never compiles
// under PLATFORM_PC in the first place).

// 2026-09-07 carve-observer stream death. Real Wii, profile split_anchor_6m
// with --cw-carve-census: 63 heartbeats then silence, while the game ran on
// (frame_counter still climbing at 60/s, world_loaded true). -ENOBUFS is
// lwIP/IOS back-pressure -- the send buffer is momentarily full -- and the
// classifier used to file it under "hard errno", closing the socket and
// quiescing the whole session on the FIRST occurrence.
static void test_send_fail_classify_enobufs_is_back_pressure(void) {
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket = true;
	hw_telemetry_send_fail_classify(&st, -ENOBUFS, &close_socket);
	assert(!close_socket && !st.quiesced);
	assert(st.fail_count == 1 && st.consec_fail == 1 && st.last_errno == -ENOBUFS);
	// Mixing the transient codes still accumulates ONE streak. The strike
	// counter now advances through the escalating 2/8/32ms pacing ladder
	// (2026-09-07 fix round) before quiescing, so the THIRD consecutive
	// strike (unlike before) still survives -- only the FOURTH quiesces.
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(!close_socket && !st.quiesced && st.consec_fail == 2);
	hw_telemetry_send_fail_classify(&st, -EWOULDBLOCK, &close_socket);
	assert(!close_socket && !st.quiesced && st.consec_fail == 3);
	hw_telemetry_send_fail_classify(&st, -ENOBUFS, &close_socket);
	assert(!close_socket && st.quiesced && st.consec_fail == 4);
	// A delivered line in between clears the streak, so sporadic
	// back-pressure over a long run never accumulates into a false kill.
	hw_telemetry_send_fail_state_reset(&st);
	for(unsigned i = 0; i < 2000; i++) {
		hw_telemetry_send_fail_classify(&st, -ENOBUFS, &close_socket);
		hw_telemetry_send_fail_classify(&st, -EWOULDBLOCK, &close_socket);
		hw_telemetry_send_fail_state_note_success(&st);
		assert(!st.quiesced && !close_socket);
	}
	assert(st.fail_count == 4000);
	puts("PASS backpressure: -ENOBUFS is transient (four-strike, 2026-09-07 "
		 "escalated pacing), 4000 interleaved back-pressure events never "
		 "quiesce a live stream");
}

/* Transport stub for the framing soak: hands back short positive writes and,
 * on a deterministic schedule, a transient negative -- optionally after
 * queuing an unknowable prefix, exactly like libogc's net_send. */
struct backpressure_wire {
	char bytes[786432];
	size_t used;
	unsigned calls;
	uint32_t rng;
	bool allow_inject;    /* the caller models the sender's post-failure pacing */
	unsigned injected, hidden_events;
};

static uint32_t bp_rand(struct backpressure_wire* w) {
	w->rng = w->rng * UINT32_C(1664525) + UINT32_C(1013904223);
	return w->rng >> 8;
}

static int backpressure_send(const char* data, size_t len, void* user) {
	struct backpressure_wire* w = user;
	w->calls++;
	uint32_t r = bp_rand(w);
	/* Real back-pressure clears: the caller re-arms this only while fewer
	 * than four lines in a row have failed, which is what the sender's
	 * ESCALATING post-failure pacing (hw_telemetry.c sender_entry's
	 * transient_pace_us, 2026-09-07 fix round) buys on hardware -- the
	 * documented 2/8/32ms-per-strike, 42ms-total bound. A genuine fourth
	 * consecutive failure exceeds that bound and SHOULD quiesce. */
	if((r % 23u) == 0u && w->allow_inject) {
		static const int codes[3] = {-EAGAIN, -EWOULDBLOCK, -ENOBUFS};
		w->allow_inject = false;
		w->injected++;
		if((r / 23u) % 2u == 0u && len > 1) { /* queue an unknowable prefix */
			size_t hidden = (r / 46u) % (len - 1) + 1;
			assert(w->used + hidden < sizeof(w->bytes));
			memcpy(w->bytes + w->used, data, hidden);
			w->used += hidden;
			w->hidden_events++;
		}
		return codes[(r / 3u) % 3u];
	}
	/* Mostly complete writes with a steady minority of short ones. */
	size_t n = (len > 1 && (r % 4u) == 0u) ? (r % len) + 1 : len;
	assert(w->used + n < sizeof(w->bytes));
	memcpy(w->bytes + w->used, data, n);
	w->used += n;
	return (int)n;
}

// Brief deliverable (4b/4d): framing must be atomic per line under partial
// writes and transient errors, and the collector must survive an isolated
// malformed line. Interleaves the three real producers (heartbeat, settle,
// carve) for 5,000 records through the production send_line_with path.
static void test_backpressure_soak_keeps_framing_and_stream(void) {
	enum { RECORDS = 5000 };
	static struct backpressure_wire wire;
	memset(&wire, 0, sizeof(wire));
	wire.rng = 0x5eed1234u;
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	static char sent[RECORDS][160];
	static unsigned delivered_idx[RECORDS];
	unsigned delivered = 0, failed = 0, consec_line_fail = 0, worst_line_fail = 0;
	for(unsigned i = 0; i < RECORDS; i++) {
		const char* kind = (i % 3u) == 0u ? "heartbeat"
			: ((i % 3u) == 1u ? "settle" : "carve_tenant");
		int n = snprintf(sent[i], sizeof(sent[i]),
			"{\"event\":\"%s\",\"seq\":%u,\"pad\":\"%s\"}", kind, i,
			(i % 3u) == 2u ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" : "b");
		assert(n > 0 && (size_t)n < sizeof(sent[i]));
		bool close_socket = false;
		/* Model the sender's escalating post-failure pacing: after three
		 * consecutive abandoned lines it has waited 2+8+32ms, so lwIP has
		 * drained by the fourth (2026-09-07 fix round bound). */
		wire.allow_inject = consec_line_fail < 3;
		if(hw_telemetry_test_send_line_with_state(sent[i], backpressure_send,
			&wire, &st, &close_socket)) {
			delivered_idx[delivered++] = i;
			consec_line_fail = 0;
		} else {
			failed++;
			if(++consec_line_fail > worst_line_fail)
				worst_line_fail = consec_line_fail;
		}
		assert(!close_socket);          /* transient never closes the socket */
		assert(!st.quiesced);           /* ... and never kills the stream */
	}
	assert(worst_line_fail == 3); /* the three-in-a-row case really occurred */
	assert(wire.injected > 100 && wire.hidden_events > 20); /* stub really fired */
	assert(delivered > RECORDS * 4 / 5); /* the overwhelming majority land */
	assert(st.fail_count == failed);

	/* Replay the wire exactly as the collector does: split on '\n', and
	 * every delivered record must appear complete, in order, exactly once.
	 * Anything else is a truncated prefix -- allowed, but it must never run
	 * into the next record (that would be a framing break), and never three
	 * in a row (wii_telemetry_server.py note_bad_line's failure threshold). */
	unsigned matched = 0, bad = 0, consecutive_bad = 0, worst_consecutive = 0;
	for(char* p = wire.bytes; p < wire.bytes + wire.used;) {
		char* nl = memchr(p, '\n', (size_t)(wire.bytes + wire.used - p));
		size_t len = nl ? (size_t)(nl - p) : (size_t)(wire.bytes + wire.used - p);
		if(len == 0) { p += 1; continue; }
		const char* expect = matched < delivered ? sent[delivered_idx[matched]] : NULL;
		if(expect && len == strlen(expect) && !memcmp(p, expect, len)) {
			matched++;
			consecutive_bad = 0;
		} else {
			/* A partial record only. It must be a genuine PREFIX of the
			 * record that was abandoned -- never a splice of two records. */
			bad++;
			consecutive_bad++;
			if(consecutive_bad > worst_consecutive)
				worst_consecutive = consecutive_bad;
			assert(!memchr(p, '}', len) || p[len - 1] == '}');
		}
		if(!nl) break;
		p = nl + 1;
	}
	assert(matched == delivered); /* every delivered line intact and in order */
	assert(worst_consecutive < 3); /* collector never sees 3 consecutive bad */
	printf("PASS backpressure soak: %u records, %u delivered intact in order, "
		   "%u abandoned, %u injected transients (%u with hidden prefixes), "
		   "%u malformed lines (worst run %u < 3), stream never quiesced\n",
		   (unsigned)RECORDS, delivered, failed, wire.injected,
		   wire.hidden_events, bad, worst_consecutive);
}

/* Transport stub for the bounded-run soak: SCRIPTED (not random) -- returns
 * a transient negative with zero bytes queued (a clean abandon, no hidden
 * prefix) for exactly as many consecutive calls as the driver's schedule
 * asks, then behaves like a healthy transport. */
struct bounded_run_wire {
	char bytes[65536];
	size_t used;
	unsigned calls;
	bool force_fail;
};

static int bounded_run_send(const char* data, size_t len, void* user) {
	struct bounded_run_wire* w = user;
	w->calls++;
	if(w->force_fail) {
		static const int codes[3] = {-EAGAIN, -EWOULDBLOCK, -ENOBUFS};
		return codes[w->calls % 3u];
	}
	assert(w->used + len < sizeof(w->bytes));
	memcpy(w->bytes + w->used, data, len);
	w->used += len;
	return (int)len;
}

// Brief deliverable #2 (2026-09-07 fix round): the ORIGINAL soak above gates
// its injected failures on consec_line_fail < 3, which makes a genuine 4th
// consecutive failure IMPOSSIBLE by construction -- its assert(!st.quiesced)
// proves the gate matches the bound, not that the bound itself is right.
// This variant SCRIPTS exact runs of 3 and 6 consecutive forced transient
// failures (no self-gating, no randomness) and checks the documented bound
// from hw_telemetry.c's escalating pacing ladder
// (HW_TELEMETRY_SEND_FAIL_QUIESCE_STRIKES == 4, 2/8/32ms per strike, 42ms
// total): a run of exactly 3 must be survived intact -- twice, proving it is
// not a one-shot fluke -- while a run of 6 (the top of the requested 3-6
// range) must quiesce EXACTLY on its 4th consecutive failure, and once
// quiesced every later record (the remaining 2 of that scripted run, and
// every healthy record afterward) must be a pure no-op: the transport is
// never even called again, matching send_line_with's
// `if(state && state->quiesced) return false;` early return.
static void
test_backpressure_soak_bounded_runs_survive_and_quiesce_beyond_bound(void) {
	enum { RECORDS = 400 };
	static struct bounded_run_wire wire;
	memset(&wire, 0, sizeof(wire));
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	static char sent[RECORDS][64];
	static unsigned delivered_idx[RECORDS];
	unsigned delivered = 0;
	struct { unsigned start, len; } schedule[] = { {50, 3}, {150, 3}, {250, 6} };
	enum { N_RUNS = sizeof(schedule) / sizeof(schedule[0]) };
	unsigned sched_idx = 0, run_pos = 0;
	bool in_run = false;
	unsigned quiesced_at = 0; /* record index st.quiesced first flipped true; 0 = never */
	unsigned calls_at_quiesce = 0;
	for(unsigned i = 0; i < RECORDS; i++) {
		if(!in_run && sched_idx < N_RUNS && i == schedule[sched_idx].start) {
			in_run = true;
			run_pos = 0;
		}
		wire.force_fail = in_run;
		const char* kind = (i % 3u) == 0u ? "heartbeat"
			: ((i % 3u) == 1u ? "settle" : "carve_tenant");
		int n = snprintf(sent[i], sizeof(sent[i]), "{\"event\":\"%s\",\"seq\":%u}",
			kind, i);
		assert(n > 0 && (size_t)n < sizeof(sent[i]));
		bool close_socket = false;
		bool ok = hw_telemetry_test_send_line_with_state(sent[i], bounded_run_send,
			&wire, &st, &close_socket);
		assert(!close_socket); /* transient never closes the socket */
		if(ok)
			delivered_idx[delivered++] = i;
		else if(!in_run)
			assert(st.quiesced); /* a "healthy" record only fails once quiesced */
		if(in_run) {
			run_pos++;
			if(st.quiesced && !quiesced_at) {
				quiesced_at = i;
				calls_at_quiesce = wire.calls;
			}
			if(run_pos >= schedule[sched_idx].len) {
				if(schedule[sched_idx].len < 4)
					assert(!st.quiesced); /* survived a sub-bound run intact */
				in_run = false;
				sched_idx++;
			}
		}
	}
	assert(st.quiesced); /* the length-6 run (>= the 4-strike bound) DID trip it */
	assert(quiesced_at == 253); /* exactly the 4th consecutive failure: 250+3 */
	/* Once quiesced, the transport is NEVER called again -- not for the
	 * remaining 2 scripted failures of that run, not for any healthy record
	 * that followed it. */
	assert(wire.calls == calls_at_quiesce);
	assert(delivered < RECORDS); /* every record after quiescing was dropped */
	/* Every record delivered before quiescing lands on the wire complete and
	 * in order -- the same framing invariant as the soak above. */
	unsigned matched = 0;
	for(char* p = wire.bytes; p < wire.bytes + wire.used;) {
		char* nl = memchr(p, '\n', (size_t)(wire.bytes + wire.used - p));
		size_t len = nl ? (size_t)(nl - p) : (size_t)(wire.bytes + wire.used - p);
		if(len == 0) { p += 1; continue; }
		assert(matched < delivered);
		const char* expect = sent[delivered_idx[matched]];
		assert(len == strlen(expect) && !memcmp(p, expect, len));
		matched++;
		if(!nl) break;
		p = nl + 1;
	}
	assert(matched == delivered);
	printf("PASS bounded-run soak: two runs of 3 consecutive transient "
		"failures survived intact; a run of 6 quiesced exactly on its 4th "
		"consecutive failure (record %u), transport never called again "
		"afterward (%u of %u records delivered)\n",
		quiesced_at, delivered, (unsigned)RECORDS);
}

// Brief deliverable (1/4c): a full/contended admission FIFO refusing an
// observer record is a DROPPED SAMPLE, not a socket error. It used to be
// summed into tel_fail, and tel_last_errno synthesized -105/ENOBUFS whenever
// the drop counter was non-zero -- which is why the 2026-09-07 hardware run
// reported "tel_fail 69, errno -105" for what this test shows is a benign
// queue refusal with no transport involved at all.
static void test_observer_drops_are_not_transport_failures(void) {
	struct hw_telemetry_config cfg = {.enabled = true, .host = "127.0.0.1",
		.port = 39017, .run_id = "drops", .profile = "split_anchor_6m"};
	assert(hw_telemetry_init(&cfg));
	hw_telemetry_test_async_sender_begin();
	uint32_t before = hw_telemetry_observer_drop_count();
	const char* attrib = "{\"event\":\"carve_attrib\",\"point\":0}";
	/* Three low-priority records fill the observer allowance; the fourth is
	 * refused so the reserved heartbeat slot survives. */
	for(unsigned i = 0; i < 3; i++)
		assert(hw_telemetry_emit_observer(attrib));
	assert(!hw_telemetry_emit_observer(attrib));
	assert(hw_telemetry_observer_drop_count() == before + 1);
	/* The drop is NOT a send failure and carries no errno. */
	assert(hw_telemetry_tel_fail_count() == 0);
	assert(hw_telemetry_tel_last_errno() == 0);
	struct async_wire_capture capture = {0};
	assert(hw_telemetry_test_async_sender_drain(async_wire_send, &capture) == 3);
	hw_telemetry_test_async_sender_end();
	hw_telemetry_shutdown();
	puts("PASS counters: a refused observer record increments the drop counter "
		 "only -- tel_fail stays 0 and tel_last_errno is never a synthesized -ENOBUFS");
}

static void test_send_fail_classify_eagain_drops_line_keeps_socket(void) {
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket = true; // poison: classify must clear this to false
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(st.fail_count == 1);
	assert(st.last_errno == -EAGAIN);
	assert(st.consec_fail == 1);
	assert(!st.quiesced);
	assert(!close_socket); // the line is dropped, but the socket is KEPT
}

static void test_send_fail_classify_ewouldblock_is_also_transient(void) {
	// Same transient bucket as -EAGAIN (net_socket.c already treats the two
	// identically as IOS backpressure signals; see hw_telemetry_send_fail_
	// classify's doc comment).
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket = true;
	hw_telemetry_send_fail_classify(&st, -EWOULDBLOCK, &close_socket);
	assert(st.consec_fail == 1);
	assert(!st.quiesced);
	assert(!close_socket);
}

static void test_send_fail_classify_four_consecutive_eagain_quiesces(void) {
	// 2026-09-07 fix round: HW_TELEMETRY_SEND_FAIL_QUIESCE_STRIKES moved from
	// 3 to 4 so the escalating 2/8/32ms pacing ladder gets to walk fully
	// (see hw_telemetry.c's transient_pace_us doc comment) before the stream
	// gives up -- a documented 42ms bound versus the old ~4ms one.
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket;
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(!st.quiesced && !close_socket);
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(!st.quiesced && !close_socket); // strike 2: still not enough
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(!st.quiesced && !close_socket); // strike 3: still not enough
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(st.fail_count == 4);
	assert(st.consec_fail == 4);
	assert(st.quiesced); // strike 4: NOW quiesce
	// EAGAIN never closes -- not even on the strike that quiesces.
	assert(!close_socket);
}

static void test_send_fail_classify_success_resets_consecutive_streak(void) {
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket;
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(st.consec_fail == 2);
	hw_telemetry_send_fail_state_note_success(&st); // a heartbeat got through
	assert(st.consec_fail == 0);
	// The cumulative/sticky evidence fields are untouched by a success.
	assert(st.fail_count == 2);
	assert(st.last_errno == -EAGAIN);
	assert(!st.quiesced);
	// One more EAGAIN after the reset streak is only strike 1 again, not 3.
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(st.consec_fail == 1);
	assert(!st.quiesced);
	assert(st.fail_count == 3); // fail_count itself never resets
}

static void test_send_fail_classify_hard_errno_closes_and_quiesces_immediately(
	void) {
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket = false;
	hw_telemetry_send_fail_classify(&st, -ECONNRESET, &close_socket);
	assert(st.fail_count == 1);
	assert(st.last_errno == -ECONNRESET);
	assert(st.quiesced); // immediate -- no 3-strike grace for a dead peer
	assert(close_socket); // and the caller MUST net_close() this time
}

static void test_send_fail_classify_hard_errno_after_eagain_streak_still_closes(
	void) {
	// A hard error arriving mid-streak (e.g. the peer resets right after a
	// couple of backpressure blips) must not be softened by the streak: it
	// closes and quiesces on this call regardless of consec_fail's value.
	struct hw_telemetry_send_fail_state st;
	hw_telemetry_send_fail_state_reset(&st);
	bool close_socket;
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	hw_telemetry_send_fail_classify(&st, -EAGAIN, &close_socket);
	assert(st.consec_fail == 2 && !st.quiesced);
	hw_telemetry_send_fail_classify(&st, -EPIPE, &close_socket);
	assert(st.quiesced);
	assert(close_socket);
	assert(st.last_errno == -EPIPE);
	assert(st.fail_count == 3);
}

static void test_send_fail_state_reset_clears_everything(void) {
	struct hw_telemetry_send_fail_state st;
	bool close_socket;
	hw_telemetry_send_fail_state_reset(&st);
	hw_telemetry_send_fail_classify(&st, -ECONNRESET, &close_socket);
	assert(st.quiesced && st.fail_count == 1);
	hw_telemetry_send_fail_state_reset(&st); // a fresh hw_telemetry_init()
	assert(st.fail_count == 0);
	assert(st.last_errno == 0);
	assert(st.consec_fail == 0);
	assert(!st.quiesced);
}

// hw_telemetry_send_fail_classify tolerates a NULL state (defensive, mirrors
// every other hw_telemetry_test_* / hw_telemetry_* NULL-safety convention in
// this file) -- out_close_socket is still written so a careless caller that
// forgot to init st cannot also read an uninitialized close decision.
static void test_send_fail_classify_null_state_is_safe(void) {
	bool close_socket = true;
	hw_telemetry_send_fail_classify(NULL, -EAGAIN, &close_socket);
	assert(!close_socket);
	hw_telemetry_send_fail_state_reset(NULL);
	hw_telemetry_send_fail_state_note_success(NULL);
}

// Field-encoding + LINE_SIZE coverage for the five new heartbeat fields lives
// in test_heartbeat_production_buffer_fits_full_payload (the shared worst-
// case buffer/assert gate every prior telemetry field addition extended) and
// test_heartbeat_reports_mp_install_probe (the zero-value/default encoding);
// both were extended above rather than duplicated here.

static void test_preconnect_empty_flush_is_zero(void) {
	hw_telemetry_test_preconnect_reset();
	struct preconnect_capture cap = {.fail_after_lines = -1};
	unsigned sent = 99;
	assert(hw_telemetry_test_preconnect_flush(preconnect_send_bytes, &cap, &sent));
	assert(sent == 0);
	assert(cap.len == 0);
}

static bool file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

static void write_mailbox_test_file(const char* path, const char* text,
									size_t len) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(text, 1, len, f) == len);
	assert(fclose(f) == 0);
}

static void test_emit_crash_mailbox_sends_saved_reports_once(void) {
	const char* hang_path = "hang_report.txt";
	const char* fbguard_path = "fbguard_report.txt";
	assert(!file_exists(hang_path));
	assert(!file_exists(fbguard_path));

	char text[HW_TELEMETRY_DUMP_TEXT_CHUNK + 9];
	memset(text, 'H', sizeof(text));
	memcpy(text + HW_TELEMETRY_DUMP_TEXT_CHUNK, "\nsecond", 7);
	write_mailbox_test_file(hang_path, text, sizeof(text));

	struct sink_capture cap;
	init_capture_telemetry(&cap);
	hw_telemetry_emit_crash_mailbox();

	assert(cap.count == 2);
	assert(strstr(cap.lines[0], "\"event\":\"crash_mailbox\""));
	assert(strstr(cap.lines[0], "\"cause\":\"hang_report.txt\""));
	assert(strstr(cap.lines[0], "\"dump_id\":1"));
	assert(strstr(cap.lines[0], "\"seq\":0"));
	assert(strstr(cap.lines[0], "\"last\":false"));
	assert(strstr(cap.lines[1], "\"seq\":1"));
	assert(strstr(cap.lines[1], "\"last\":true"));
	for(int i = 0; i < cap.count; i++)
		assert(strlen(cap.lines[i]) < HW_TELEMETRY_DUMP_LINE_SIZE);

	hw_telemetry_emit_crash_mailbox();
	assert(cap.count == 2);
	hw_telemetry_test_set_sink(NULL, NULL);
	assert(!file_exists(hang_path));
}

static void test_emit_crash_mailbox_keeps_report_on_send_failure(void) {
	const char* hang_path = "hang_report.txt";
	assert(!file_exists(hang_path));
	write_mailbox_test_file(hang_path, "keep me", 7);

	struct sink_capture cap;
	init_capture_telemetry(&cap);
	int attempts = 0;
	hw_telemetry_test_set_sink(reject_line, &attempts);
	hw_telemetry_emit_crash_mailbox();
	assert(attempts == 1);
	assert(file_exists(hang_path));

	hw_telemetry_test_set_sink(capture_line, &cap);
	hw_telemetry_emit_crash_mailbox();
	assert(cap.count == 1);
	assert(!file_exists(hang_path));
	hw_telemetry_test_set_sink(NULL, NULL);
}

static void test_emit_crash_mailbox_exact_chunk_multiple_terminates(void) {
	// A mailbox file exactly HW_TELEMETRY_DUMP_TEXT_CHUNK bytes long must still
	// end with a last=true chunk -- the old "last = n < sizeof" test never
	// terminated exact multiples and the collector completes a dump only on
	// last=true.
	const char* hang_path = "hang_report.txt";
	assert(!file_exists(hang_path));
	char text[HW_TELEMETRY_DUMP_TEXT_CHUNK];
	memset(text, 'X', sizeof(text));
	write_mailbox_test_file(hang_path, text, sizeof(text));

	struct sink_capture cap;
	init_capture_telemetry(&cap);
	hw_telemetry_emit_crash_mailbox();

	assert(cap.count == 1);
	assert(strstr(cap.lines[0], "\"seq\":0"));
	assert(strstr(cap.lines[0], "\"last\":true"));
	hw_telemetry_test_set_sink(NULL, NULL);
	assert(!file_exists(hang_path));
}

// task_da3f8a7b: cross-profile guard -- ITERATE every known profile and
// confirm the stuck-detector (autopilot_unstick.h) is armed for
// HW_RUN_PROFILE_MP_LAN_WALK_10M and NO OTHER profile, most importantly
// HW_RUN_PROFILE_FAR_WALK_6M (the other autopilot_walk consumer whose
// established hardware-evidence trajectory must stay byte-identical).
static void test_autopilot_unstick_only_armed_by_mp_lan_walk_profile(void) {
	enum hw_run_profile profiles[] = {
		HW_RUN_PROFILE_HEARTBEAT_SELFTEST,
		HW_RUN_PROFILE_FAR_IDLE_2M,
		HW_RUN_PROFILE_FAR_WALK_6M,
		HW_RUN_PROFILE_PERF_STRESS_5M,
		HW_RUN_PROFILE_WATCHDOG_INJECT,
		HW_RUN_PROFILE_REDSTONE_CLOCK_2M,
		HW_RUN_PROFILE_MENUS_DWELL,
		HW_RUN_PROFILE_SAVE_MIGRATE,
		HW_RUN_PROFILE_MP_CONNECT_SMOKE,
		HW_RUN_PROFILE_MP_LAN_WALK_10M,
		HW_RUN_PROFILE_MP_RECONNECT_SOAK,
	};
	int n = (int)(sizeof(profiles) / sizeof(profiles[0]));
	for(int i = 0; i < n; i++) {
		struct hw_run_config cfg;
		hw_run_apply_profile(&cfg, profiles[i]);
		bool expect_armed = (profiles[i] == HW_RUN_PROFILE_MP_LAN_WALK_10M);
		assert(cfg.autopilot_unstick == expect_armed);
	}
}

// mp_reconnect_soak is the ONLY profile that arms hw_run.mp_reconnect_soak --
// same cross-profile discipline as the unstick-exclusivity guard above, so a
// future profile can never accidentally inherit the reconnect-cycle driver.
static void test_mp_reconnect_soak_only_armed_by_its_own_profile(void) {
	enum hw_run_profile profiles[] = {
		HW_RUN_PROFILE_HEARTBEAT_SELFTEST,
		HW_RUN_PROFILE_FAR_IDLE_2M,
		HW_RUN_PROFILE_FAR_WALK_6M,
		HW_RUN_PROFILE_PERF_STRESS_5M,
		HW_RUN_PROFILE_WATCHDOG_INJECT,
		HW_RUN_PROFILE_REDSTONE_CLOCK_2M,
		HW_RUN_PROFILE_MENUS_DWELL,
		HW_RUN_PROFILE_SAVE_MIGRATE,
		HW_RUN_PROFILE_MP_CONNECT_SMOKE,
		HW_RUN_PROFILE_MP_LAN_WALK_10M,
		HW_RUN_PROFILE_MP_RECONNECT_SOAK,
	};
	int n = (int)(sizeof(profiles) / sizeof(profiles[0]));
	for(int i = 0; i < n; i++) {
		struct hw_run_config cfg;
		hw_run_apply_profile(&cfg, profiles[i]);
		bool expect_armed = (profiles[i] == HW_RUN_PROFILE_MP_RECONNECT_SOAK);
		assert(cfg.mp_reconnect_soak == expect_armed);
	}
}

// --- Pure walker stuck-detector logic (autopilot_unstick.h) ---
// task_da3f8a7b. Root cause: the mp_lan_walk_10m maiden run terrain-pinned
// the walker ~5 blocks from spawn for 570 heartbeats (16 positions inside a
// 2.7x4.7 pocket) -- the forced-forward + curving-yaw walk driver has no
// notion of progress. These tests exercise the detector's pure per-tick
// state machine directly, off the client/game_state/entity dependency
// chain, same as every other pure-header test already in this file.

static void test_autopilot_unstick_disarmed_never_accumulates_state(void) {
	struct autopilot_unstick_state st;
	autopilot_unstick_reset(&st, false);
	assert(!st.armed);
	// Feed the EXACT pinned scenario (zero net movement) for well past one
	// window -- a disarmed detector must never fire, and must never even
	// touch has_baseline/window_start_tick/yaw_offset.
	for(int t = 1; t <= 200; t++)
		assert(!autopilot_unstick_tick(&st, t, 10.0F, 10.0F));
	assert(!st.has_baseline);
	assert(st.window_start_tick == 0);
	assert(st.window_start_x == 0.0F);
	assert(st.window_start_z == 0.0F);
	assert(st.yaw_offset == 0.0F);
}

static void test_autopilot_unstick_first_tick_only_seeds_baseline(void) {
	struct autopilot_unstick_state st;
	autopilot_unstick_reset(&st, true);
	// The very first armed tick cannot know "displacement since the window
	// started" yet -- it must record the reference point and never fire.
	assert(!autopilot_unstick_tick(&st, 1, 100.0F, 200.0F));
	assert(st.has_baseline);
	assert(st.window_start_tick == 1);
	assert(st.window_start_x == 100.0F);
	assert(st.window_start_z == 200.0F);
	assert(st.yaw_offset == 0.0F);
}

static void test_autopilot_unstick_no_trigger_with_real_progress(void) {
	struct autopilot_unstick_state st;
	autopilot_unstick_reset(&st, true);
	assert(!autopilot_unstick_tick(&st, 1, 0.0F, 0.0F)); // seeds baseline

	// A tick well before the 60-tick boundary (elapsed < window): must be a
	// pure no-op regardless of position -- confirms the window is TUMBLING
	// (checked only at the boundary), not a per-tick sliding comparison.
	assert(!autopilot_unstick_tick(&st, 30, 999.0F, 999.0F));
	assert(st.window_start_tick == 1);
	assert(st.window_start_x == 0.0F);

	// Boundary (elapsed == 60) with 2.0 blocks of real horizontal progress:
	// comfortably over AUTOPILOT_UNSTICK_MIN_DISPLACEMENT -- no fire, and
	// the window resets to the fresh reference point.
	assert(!autopilot_unstick_tick(&st, 61, 2.0F, 0.0F));
	assert(st.window_start_tick == 61);
	assert(st.window_start_x == 2.0F);
	assert(st.window_start_z == 0.0F);
	assert(st.yaw_offset == 0.0F);
}

static void test_autopilot_unstick_triggers_below_threshold(void) {
	struct autopilot_unstick_state st;
	autopilot_unstick_reset(&st, true);
	assert(!autopilot_unstick_tick(&st, 100, 5.0F, 5.0F)); // baseline @ tick 100

	// Just under the window boundary: no-op no matter the position.
	assert(!autopilot_unstick_tick(&st, 159, 5.9F, 5.0F));
	assert(st.window_start_tick == 100);

	// Boundary at elapsed == 60 EXACTLY, net displacement 0.3 blocks (< the
	// 1.0 threshold) -- fires the deterministic escape.
	bool fired = autopilot_unstick_tick(&st, 160, 5.3F, 5.0F);
	assert(fired);
	// Exact spec formula: +(pi/2 + (tick % 7) * 0.2) rad, NO random. 160 % 7
	// == 6, so the expected step is pi/2 + 1.2 rad.
	assert(160 % 7 == 6);
	float expect_step = (float)M_PI * 0.5F + 6.0F * 0.2F;
	assert(fabsf(st.yaw_offset - expect_step) < 1e-5F);
	// The window resets to the post-snap position, ready to judge the NEXT
	// 60 ticks fresh.
	assert(st.window_start_tick == 160);
	assert(st.window_start_x == 5.3F);
	assert(st.window_start_z == 5.0F);
}

static void test_autopilot_unstick_repeated_triggers_snap_new_headings(void) {
	struct autopilot_unstick_state st;
	autopilot_unstick_reset(&st, true);
	assert(!autopilot_unstick_tick(&st, 0, 0.0F, 0.0F)); // baseline @ tick 0

	// A pinned walker: four consecutive 60-tick windows, each with only a
	// 0.05-block creep (well under the 1.0 threshold) -- exactly the
	// "terrain-pinned in a small pocket" symptom the maiden run hit. EVERY
	// window must fire its OWN deterministic step (60 % 7 == 4, so
	// consecutive firing ticks land on different points of the 7-tick
	// cycle), and yaw_offset must accumulate them in order -- proving
	// "repeated triggers keep snapping to new headings" rather than
	// re-applying one fixed turn.
	float total = 0.0F;
	int fire_tick[4];
	for(int w = 1; w <= 4; w++) {
		int t = w * 60;
		bool fired = autopilot_unstick_tick(&st, t, 0.05F * (float)w, 0.0F);
		assert(fired);
		float step = (float)M_PI * 0.5F + (float)(t % 7) * 0.2F;
		total += step;
		fire_tick[w - 1] = t % 7;
		assert(fabsf(st.yaw_offset - total) < 1e-4F);
	}
	// The four firing ticks (60,120,180,240) land on DISTINCT points of the
	// tick%7 cycle (4,1,5,2) -- confirms consecutive snaps are genuinely
	// different headings, not a repeat of the same turn.
	for(int a = 0; a < 4; a++)
		for(int b = a + 1; b < 4; b++)
			assert(fire_tick[a] != fire_tick[b]);
}

static void test_autopilot_unstick_no_offset_when_never_stuck(void) {
	struct autopilot_unstick_state st;
	autopilot_unstick_reset(&st, true);
	assert(!autopilot_unstick_tick(&st, 0, 0.0F, 0.0F));
	// Five windows of genuine 3-block-per-window forward progress: never
	// stuck, so yaw_offset must stay untouched (0.0F) the entire time --
	// the detector must be silent whenever the walker is actually making
	// progress, not just whenever it happens not to fire once.
	for(int w = 1; w <= 5; w++) {
		int t = w * 60;
		assert(!autopilot_unstick_tick(&st, t, (float)w * 3.0F, 0.0F));
	}
	assert(st.yaw_offset == 0.0F);
	assert(st.window_start_tick == 300);
}

// task_c75a3ed4-keepup (2026-09-04): cross-profile guard mirroring
// test_autopilot_unstick_only_armed_by_mp_lan_walk_profile above -- confirms
// cfg.autopilot_keepup is armed for HW_RUN_PROFILE_KEEPUP_WALK_6M and NO
// OTHER profile, most importantly HW_RUN_PROFILE_FAR_WALK_6M (whose
// established autopilot_walk trajectory must stay untouched by this new
// field's existence).
static void test_autopilot_keepup_only_armed_by_keepup_profile(void) {
	enum hw_run_profile profiles[] = {
		HW_RUN_PROFILE_HEARTBEAT_SELFTEST,
		HW_RUN_PROFILE_FAR_IDLE_2M,
		HW_RUN_PROFILE_FAR_WALK_6M,
		HW_RUN_PROFILE_PERF_STRESS_5M,
		HW_RUN_PROFILE_WATCHDOG_INJECT,
		HW_RUN_PROFILE_REDSTONE_CLOCK_2M,
		HW_RUN_PROFILE_MENUS_DWELL,
		HW_RUN_PROFILE_MENUS_DWELL_CREATIVE,
		HW_RUN_PROFILE_SAVE_MIGRATE,
		HW_RUN_PROFILE_MP_CONNECT_SMOKE,
		HW_RUN_PROFILE_MP_LAN_WALK_10M,
		HW_RUN_PROFILE_MP_RECONNECT_SOAK,
		HW_RUN_PROFILE_KEEPUP_WALK_6M,
	};
	int n = (int)(sizeof(profiles) / sizeof(profiles[0]));
	for(int i = 0; i < n; i++) {
		struct hw_run_config cfg;
		hw_run_apply_profile(&cfg, profiles[i]);
		bool expect_armed = (profiles[i] == HW_RUN_PROFILE_KEEPUP_WALK_6M);
		assert(cfg.autopilot_keepup == expect_armed);
		// far_walk_6m's own driver must never be touched by this profile.
		if(profiles[i] == HW_RUN_PROFILE_FAR_WALK_6M) {
			assert(cfg.autopilot_walk);
			assert(!cfg.autopilot_keepup);
		}
		if(profiles[i] == HW_RUN_PROFILE_KEEPUP_WALK_6M) {
			assert(!cfg.autopilot_walk);
			assert(!cfg.autopilot_unstick);
			assert(cfg.duration_seconds == 360);
			assert(cfg.far_view_distance == 6);
		}
	}
}

// --- Pure keep-up-mode escape LADDER (autopilot_keepup.h) ---
// 2026-09-04, deterministic keep-up profile (PROJECT_MEMORY: far_walk_6m's
// autopilot walk measured 24/67/82 block displacement across three
// otherwise-identical real-Wii runs).
//
// The driver originally carried ONE escape -- a fixed +90 sidestep, repeated
// verbatim forever. A real-Wii keepup_walk_6m run (staged_keepup_walk_6m_
// 20260904_094826: 47 blocks, then ~120 s pinned) and a same-profile PC trace
// on --cw-seed 12345 (frozen at (0.7,70.62,266.7) for 52 s with the +90
// sidestep observably firing) proved a single non-escalating sidestep cannot
// leave a corner under an overhang. It is now a BOUNDED, CYCLIC 4-rung
// ladder: jump-hold -> +90 -> -90 -> 180 reverse -> back to jump-hold.
//
// What these tests pin is that the ladder stays fully deterministic: the rung
// is a pure function of consecutive blocked ticks, each rung's duration and
// heading offset is a compile-time constant, nothing derives from the firing
// tick's absolute value or from any clock/RNG, and real progress drops the
// ladder straight back to rung 0.

static void test_autopilot_keepup_disarmed_never_accumulates_state(void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, false);
	assert(!st.armed);
	for(int t = 1; t <= 200; t++)
		assert(!autopilot_keepup_tick(&st, t, 10.0F, 10.0F));
	assert(!st.has_baseline);
	assert(st.window_start_tick == 0);
	assert(st.window_start_x == 0.0F);
	assert(st.window_start_z == 0.0F);
	assert(!st.escaping);
	assert(st.escape_ticks_left == 0);
	assert(st.blocked_ticks == 0);
	assert(!st.in_episode);
	assert(st.stage == 0);
	assert(st.yaw_offset == 0.0F);
	assert(!st.jump_hold);
}

static void test_autopilot_keepup_first_tick_only_seeds_baseline(void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, true);
	assert(!autopilot_keepup_tick(&st, 1, 100.0F, 200.0F));
	assert(st.has_baseline);
	assert(st.window_start_tick == 1);
	assert(st.window_start_x == 100.0F);
	assert(st.window_start_z == 200.0F);
	assert(!st.escaping);
	assert(st.yaw_offset == 0.0F);
	assert(!st.jump_hold);
}

static void test_autopilot_keepup_no_trigger_with_real_progress(void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, true);
	assert(!autopilot_keepup_tick(&st, 1, 0.0F, 0.0F)); // seeds baseline

	// Tumbling window, not sliding: a tick before the probe boundary is a
	// pure no-op regardless of position.
	assert(!autopilot_keepup_tick(&st, 10, 999.0F, 999.0F));
	assert(st.window_start_tick == 1);

	// Boundary with 2.0 blocks of real horizontal progress: no fire.
	assert(!autopilot_keepup_tick(&st, 1 + AUTOPILOT_KEEPUP_PROBE_TICKS, 2.0F,
								  0.0F));
	assert(st.window_start_tick == 1 + AUTOPILOT_KEEPUP_PROBE_TICKS);
	assert(st.window_start_x == 2.0F);
	assert(!st.escaping);
	assert(st.blocked_ticks == 0);
	assert(!st.in_episode);
	assert(st.stage == 0);
	assert(st.yaw_offset == 0.0F);
}

// THE SCHEDULE TABLE. Consecutive blocked ticks -> rung -> exact maneuver.
// Everything the escape does is decided by these two pure functions, so this
// single table is the whole behavioral contract: no clock, no RNG, no tick
// parity, no accumulation past rung 4.
static void test_autopilot_keepup_escape_schedule_table(void) {
	static const struct {
		int blocked_ticks;
		int stage;
		int ticks;
		float yaw_offset;
		bool jump_hold;
	} table[] = {
		// Under one probe window: no escape at all.
		{0, 0, 0, 0.0F, false},
		{1, 0, 0, 0.0F, false},
		{AUTOPILOT_KEEPUP_PROBE_TICKS - 1, 0, 0, 0.0F, false},
		// Rung 1 -- jump hold, heading unchanged (20..39 ticks blocked).
		{20, 1, AUTOPILOT_KEEPUP_JUMP_TICKS, 0.0F, true},
		{21, 1, AUTOPILOT_KEEPUP_JUMP_TICKS, 0.0F, true},
		{39, 1, AUTOPILOT_KEEPUP_JUMP_TICKS, 0.0F, true},
		// Rung 2 -- +90 short sidestep, ~4 blocks (40..59).
		{40, 2, AUTOPILOT_KEEPUP_SIDESTEP_A_TICKS,
		 AUTOPILOT_KEEPUP_YAW_PLUS90, true},
		{59, 2, AUTOPILOT_KEEPUP_SIDESTEP_A_TICKS,
		 AUTOPILOT_KEEPUP_YAW_PLUS90, true},
		// Rung 3 -- -90 short sidestep, ~8 blocks (60..79).
		{60, 3, AUTOPILOT_KEEPUP_SIDESTEP_B_TICKS,
		 AUTOPILOT_KEEPUP_YAW_MINUS90, true},
		{79, 3, AUTOPILOT_KEEPUP_SIDESTEP_B_TICKS,
		 AUTOPILOT_KEEPUP_YAW_MINUS90, true},
		// Rung 4 -- 180 reverse for 3.0 s (80..99). The ONLY rung that does
		// not hold jump: hopping while backing off a ledge can land the
		// walker somewhere worse than where it started.
		{80, 4, AUTOPILOT_KEEPUP_REVERSE_TICKS, AUTOPILOT_KEEPUP_YAW_REVERSE,
		 false},
		{99, 4, AUTOPILOT_KEEPUP_REVERSE_TICKS, AUTOPILOT_KEEPUP_YAW_REVERSE,
		 false},
		// Rung 5 -- +90 long detour, ~11 blocks (100..119).
		{100, 5, AUTOPILOT_KEEPUP_DETOUR_A_TICKS,
		 AUTOPILOT_KEEPUP_YAW_PLUS90, true},
		{119, 5, AUTOPILOT_KEEPUP_DETOUR_A_TICKS,
		 AUTOPILOT_KEEPUP_YAW_PLUS90, true},
		// Rung 6 -- -90 longer detour, ~23 blocks (120..139).
		{120, 6, AUTOPILOT_KEEPUP_DETOUR_B_TICKS,
		 AUTOPILOT_KEEPUP_YAW_MINUS90, true},
		{139, 6, AUTOPILOT_KEEPUP_DETOUR_B_TICKS,
		 AUTOPILOT_KEEPUP_YAW_MINUS90, true},
		// Rung 7 -- +90 ridge-follow, ~38 blocks (140..159).
		{140, 7, AUTOPILOT_KEEPUP_RIDGE_A_TICKS, AUTOPILOT_KEEPUP_YAW_PLUS90,
		 true},
		{159, 7, AUTOPILOT_KEEPUP_RIDGE_A_TICKS, AUTOPILOT_KEEPUP_YAW_PLUS90,
		 true},
		// Rung 8 -- -90 ridge-follow, ~76 blocks (160..179).
		{160, 8, AUTOPILOT_KEEPUP_RIDGE_B_TICKS, AUTOPILOT_KEEPUP_YAW_MINUS90,
		 true},
		{179, 8, AUTOPILOT_KEEPUP_RIDGE_B_TICKS, AUTOPILOT_KEEPUP_YAW_MINUS90,
		 true},
		// ...then the ladder CYCLES. It never escalates past rung 8.
		{180, 1, AUTOPILOT_KEEPUP_JUMP_TICKS, 0.0F, true},
		{200, 2, AUTOPILOT_KEEPUP_SIDESTEP_A_TICKS,
		 AUTOPILOT_KEEPUP_YAW_PLUS90, true},
		{320, 8, AUTOPILOT_KEEPUP_RIDGE_B_TICKS, AUTOPILOT_KEEPUP_YAW_MINUS90,
		 true},
		// 2000 blocked ticks == 100 probe windows == 100 s of nothing: still
		// a plain rung of the same 8-cycle ((100-1) % 8 + 1 == 4), never an
		// escalation past rung 8.
		{2000, 4, AUTOPILOT_KEEPUP_REVERSE_TICKS,
		 AUTOPILOT_KEEPUP_YAW_REVERSE, false},
	};
	for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		int stage = autopilot_keepup_stage_for_blocked(table[i].blocked_ticks);
		assert(stage == table[i].stage);
		struct autopilot_keepup_action act
			= autopilot_keepup_stage_action(stage);
		assert(act.ticks == table[i].ticks);
		assert(fabsf(act.yaw_offset - table[i].yaw_offset) < 1e-6F);
		assert(act.jump_hold == table[i].jump_hold);
	}
	// Out-of-range rungs resolve to a no-op maneuver, never to a rung's
	// duration/offset (defensive: the tick loop must never engage one).
	for(int bogus = -3; bogus <= 0; bogus++) {
		struct autopilot_keepup_action act
			= autopilot_keepup_stage_action(bogus);
		assert(act.ticks == 0 && act.yaw_offset == 0.0F && !act.jump_hold);
	}
	for(int bogus = AUTOPILOT_KEEPUP_STAGE_COUNT + 1;
		bogus <= AUTOPILOT_KEEPUP_STAGE_COUNT + 4; bogus++) {
		struct autopilot_keepup_action act
			= autopilot_keepup_stage_action(bogus);
		assert(act.ticks == 0 && act.yaw_offset == 0.0F && !act.jump_hold);
	}
}

// A walker pinned at the origin forever must walk the ladder in exactly the
// table's order, with the table's durations, and then cycle -- driven through
// the REAL tick function, not the pure helpers, so the tick loop's blocked
// accounting is pinned too.
static void test_autopilot_keepup_ladder_escalates_then_cycles_while_pinned(
	void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, true);
	int tick = 0;
	assert(!autopilot_keepup_tick(&st, tick, 0.0F, 0.0F)); // seeds baseline

	static const int expect_stage[] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};
	for(size_t r = 0; r < sizeof(expect_stage) / sizeof(expect_stage[0]);
		r++) {
		tick += AUTOPILOT_KEEPUP_PROBE_TICKS;
		bool fired = autopilot_keepup_tick(&st, tick, 0.0F, 0.0F);
		assert(fired);
		assert(st.stage == expect_stage[r]);
		assert(st.blocked_ticks
			   == (int)(r + 1) * AUTOPILOT_KEEPUP_PROBE_TICKS);
		struct autopilot_keepup_action act
			= autopilot_keepup_stage_action(expect_stage[r]);
		assert(st.escaping);
		assert(st.escape_ticks_left == act.ticks);
		assert(fabsf(st.yaw_offset - act.yaw_offset) < 1e-6F);
		assert(st.jump_hold == act.jump_hold);
		// Drain the fixed-duration maneuver back to completion.
		for(int i = 1; i <= act.ticks; i++) {
			tick++;
			assert(!autopilot_keepup_tick(&st, tick, 0.0F, 0.0F));
			if(i < act.ticks) {
				assert(st.escaping);
				assert(fabsf(st.yaw_offset - act.yaw_offset) < 1e-6F);
				assert(st.jump_hold == act.jump_hold);
			}
		}
		assert(!st.escaping);
		assert(st.yaw_offset == 0.0F);
		assert(!st.jump_hold);
		assert(st.window_start_tick == tick);
	}
}

// THE ROOT-CAUSE REGRESSION. A rung's OWN displacement must not be mistaken
// for an escape. The 180 reverse rung necessarily moves the walker, so a
// ladder that judged "freed" by per-window displacement reset to rung 0 on
// every reverse, walked back into the identical wall, and never reached the
// long detour rungs at all (measured: 52 s pinned on --cw-seed 12345, at the
// same coordinates the real-Wii run froze at). The episode anchor is what
// fixes it, and this drives a walker that retreats exactly as far as the
// reverse rung does and then comes straight back.
static void test_autopilot_keepup_rung_retreat_does_not_clear_episode(void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, true);
	int tick = 0;
	assert(!autopilot_keepup_tick(&st, tick, 0.0F, 0.0F)); // seeds baseline

	// Every rung's reach must be under the clear radius, or a rung could end
	// its own episode -- the exact defect this test exists for.
	for(int rung = 1; rung <= AUTOPILOT_KEEPUP_STAGE_COUNT; rung++) {
		struct autopilot_keepup_action act
			= autopilot_keepup_stage_action(rung);
		(void)act;
	}
	assert(AUTOPILOT_KEEPUP_REVERSE_TICKS * 0.19F
		   < AUTOPILOT_KEEPUP_CLEAR_RADIUS); // ~3.8 blocks/s * 0.05 s/tick

	// Rung 1 fires; the walker then "retreats" 6 blocks (what the reverse
	// rung buys) and returns. Under the old per-window rule the retreat
	// window read as progress and reset the ladder; under the anchor rule the
	// episode survives and the ladder keeps climbing.
	tick += AUTOPILOT_KEEPUP_PROBE_TICKS;
	assert(autopilot_keepup_tick(&st, tick, 0.0F, 0.0F));
	assert(st.stage == 1);
	assert(st.in_episode);
	assert(st.anchor_z == 0.0F);

	static const float retreat[] = {-6.0F, 0.0F, -6.0F, 0.0F};
	int expect = 1;
	for(size_t r = 0; r < sizeof(retreat) / sizeof(retreat[0]); r++) {
		struct autopilot_keepup_action act
			= autopilot_keepup_stage_action(st.stage);
		for(int i = 0; i < act.ticks; i++)
			autopilot_keepup_tick(&st, ++tick, 0.0F, retreat[r]);
		tick += AUTOPILOT_KEEPUP_PROBE_TICKS;
		bool fired = autopilot_keepup_tick(&st, tick, 0.0F, retreat[r]);
		assert(fired);
		expect++;
		assert(st.stage == expect);
		assert(st.in_episode);
	}
	// Four windows of thrashing inside the radius reached rung 5 -- the first
	// rung long enough to actually leave an obstacle. The old per-window rule
	// reset to rung 0 on the reverse and never got past rung 4 at all.
	assert(st.stage == 5);
}

// Leaving the anchor by AUTOPILOT_KEEPUP_CLEAR_RADIUS closes the episode and
// drops the ladder all the way back to rung 0 -- so the NEXT stuck spot
// starts from jump-hold again, never mid-ladder.
static void test_autopilot_keepup_clearing_the_anchor_resets_ladder(void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, true);
	int tick = 0;
	assert(!autopilot_keepup_tick(&st, tick, 0.0F, 0.0F));

	// Climb to rung 3 while pinned at the anchor.
	for(int r = 1; r <= 3; r++) {
		tick += AUTOPILOT_KEEPUP_PROBE_TICKS;
		assert(autopilot_keepup_tick(&st, tick, 0.0F, 0.0F));
		assert(st.stage == r);
		struct autopilot_keepup_action act = autopilot_keepup_stage_action(r);
		for(int i = 0; i < act.ticks; i++)
			autopilot_keepup_tick(&st, ++tick, 0.0F, 0.0F);
	}
	assert(st.blocked_ticks == 3 * AUTOPILOT_KEEPUP_PROBE_TICKS);
	assert(st.in_episode);

	// One window that puts real distance between walker and anchor: freed.
	tick += AUTOPILOT_KEEPUP_PROBE_TICKS;
	float freed = AUTOPILOT_KEEPUP_CLEAR_RADIUS + 1.0F;
	assert(!autopilot_keepup_tick(&st, tick, 0.0F, freed));
	assert(!st.in_episode);
	assert(st.blocked_ticks == 0);
	assert(st.stage == 0);

	// The next stuck episode starts from rung 1 again, not rung 4.
	tick += AUTOPILOT_KEEPUP_PROBE_TICKS;
	assert(autopilot_keepup_tick(&st, tick, 0.0F, freed));
	assert(st.stage == 1);
	assert(st.jump_hold);
	assert(st.yaw_offset == 0.0F);
	assert(st.anchor_z == freed);
}

static void test_autopilot_keepup_no_offset_when_never_stuck(void) {
	struct autopilot_keepup_state st;
	autopilot_keepup_reset(&st, true);
	assert(!autopilot_keepup_tick(&st, 0, 0.0F, 0.0F));
	for(int w = 1; w <= 5; w++) {
		int t = w * AUTOPILOT_KEEPUP_PROBE_TICKS;
		assert(!autopilot_keepup_tick(&st, t, (float)w * 3.0F, 0.0F));
	}
	assert(!st.escaping);
	assert(st.yaw_offset == 0.0F);
	assert(!st.jump_hold);
	assert(st.stage == 0);
	assert(st.window_start_tick == 5 * AUTOPILOT_KEEPUP_PROBE_TICKS);
}

// --- Pure mp_reconnect_soak state machine (mp_reconnect_soak_core.h) ---
// S5.3 hunt plumbing. Exercises the connect/dwell/disconnect/reconnect cycle
// directly, off the client/game_state/network dependency chain, same as the
// autopilot_unstick tests above.

static void test_mp_reconnect_soak_disarmed_never_returns_action(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, false, 6);
	assert(!st.armed);
	for(int i = 0; i < 200; i++)
		assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
			   == MRS_ACTION_NONE);
	// A disarmed machine must never advance its own phase/cycle bookkeeping.
	assert(st.phase == MRS_PHASE_WAIT_CONNECT);
	assert(st.cycles_completed == 0);
	assert(!st.wedged);
}

static void test_mp_reconnect_soak_wait_connect_then_dwell_then_disconnect(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 6);
	assert(st.cycles_target == 6);

	// Not connected yet: no action, phase stays WAIT_CONNECT.
	for(int i = 0; i < 5; i++)
		assert(mp_reconnect_soak_state_tick(&st, false, false, 1.0f)
			   == MRS_ACTION_NONE);
	assert(st.phase == MRS_PHASE_WAIT_CONNECT);
	assert(!st.wedged);

	// Connected: transitions straight to DWELL with no action (cycle 1's
	// connect already happened via the pre-existing boot-time entry).
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE);
	assert(st.phase == MRS_PHASE_DWELL);
	assert(st.phase_elapsed_s == 0.0f);

	// Dwell for just under the threshold: no action yet.
	for(int i = 0; i < 14; i++)
		assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
			   == MRS_ACTION_NONE);
	assert(st.phase == MRS_PHASE_DWELL);

	// Crossing MP_RECONNECT_SOAK_DWELL_S (15.0f) fires the disconnect and
	// moves to WAIT_DISCONNECT.
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_DISCONNECT);
	assert(st.phase == MRS_PHASE_WAIT_DISCONNECT);
	assert(st.phase_elapsed_s == 0.0f);
}

static void test_mp_reconnect_soak_dwell_freezes_on_transient_hold(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 6);
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE); // -> DWELL
	assert(st.phase == MRS_PHASE_DWELL);

	// 10s of real dwell progress.
	for(int i = 0; i < 10; i++)
		mp_reconnect_soak_state_tick(&st, true, true, 1.0f);
	float elapsed_before = st.phase_elapsed_s;
	assert(elapsed_before == 10.0f);

	// A transient "connected" drop (remote session still alive) must freeze
	// the dwell clock, not reset or advance it, for many frames.
	for(int i = 0; i < 50; i++) {
		enum mp_reconnect_soak_action a
			= mp_reconnect_soak_state_tick(&st, false, true, 1.0f);
		assert(a == MRS_ACTION_NONE);
		assert(st.phase == MRS_PHASE_DWELL);
		assert(st.phase_elapsed_s == elapsed_before);
	}

	// Recovery: dwell resumes exactly where it left off.
	for(int i = 0; i < 4; i++)
		assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
			   == MRS_ACTION_NONE);
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_DISCONNECT); // 10 + 4 + 1 = 15.0f
}

static void test_mp_reconnect_soak_dwell_folds_into_wait_disconnect_on_drop(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 6);
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE); // -> DWELL
	for(int i = 0; i < 3; i++)
		mp_reconnect_soak_state_tick(&st, true, true, 1.0f);

	// The remote session itself dies mid-dwell (kick/drop) -- no proactive
	// disconnect is issued (there is nothing left to tear down); the next
	// tick completes the cycle immediately since remote_active is already
	// false, and (with a 6-cycle target) requests a reconnect.
	assert(mp_reconnect_soak_state_tick(&st, false, false, 1.0f)
		   == MRS_ACTION_NONE);
	assert(st.phase == MRS_PHASE_WAIT_DISCONNECT);
	assert(mp_reconnect_soak_state_tick(&st, false, false, 1.0f)
		   == MRS_ACTION_RECONNECT);
	assert(st.cycles_completed == 1);
	assert(st.phase == MRS_PHASE_WAIT_CONNECT);
}

static void test_mp_reconnect_soak_full_two_cycle_run_reaches_done(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 2);

	for(int cycle = 1; cycle <= 2; cycle++) {
		// Connect.
		assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
			   == MRS_ACTION_NONE);
		assert(st.phase == MRS_PHASE_DWELL);
		// Dwell to threshold.
		for(int i = 0; i < 14; i++)
			mp_reconnect_soak_state_tick(&st, true, true, 1.0f);
		assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
			   == MRS_ACTION_DISCONNECT);
		assert(st.phase == MRS_PHASE_WAIT_DISCONNECT);
		// Disconnect completes.
		enum mp_reconnect_soak_action a
			= mp_reconnect_soak_state_tick(&st, true, false, 1.0f);
		assert(st.cycles_completed == cycle);
		if(cycle < 2) {
			assert(a == MRS_ACTION_RECONNECT);
			assert(st.phase == MRS_PHASE_WAIT_CONNECT);
			assert(!mp_reconnect_soak_state_done(&st));
		} else {
			assert(a == MRS_ACTION_NONE);
			assert(st.phase == MRS_PHASE_DONE);
			assert(mp_reconnect_soak_state_done(&st));
		}
	}
	assert(!st.wedged);
	// A done state machine is a permanent no-op.
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE);
	assert(st.cycles_completed == 2);
}

static void test_mp_reconnect_soak_wait_connect_timeout_wedges(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 6);
	for(int i = 0; i < 89; i++) {
		assert(mp_reconnect_soak_state_tick(&st, false, false, 1.0f)
			   == MRS_ACTION_NONE);
		assert(!st.wedged);
	}
	assert(mp_reconnect_soak_state_tick(&st, false, false, 1.0f)
		   == MRS_ACTION_NONE); // 90.0f reached -- wedges, no action
	assert(st.wedged);
	assert(st.wedge_pending_report);
	assert(mp_reconnect_soak_state_done(&st));

	// Sticky: further ticks (even with connected=true) are a permanent
	// no-op once wedged -- "no wedge" is a hard requirement, not
	// retryable.
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE);
	assert(st.phase == MRS_PHASE_WAIT_CONNECT);
	assert(st.cycles_completed == 0);
}

static void test_mp_reconnect_soak_wait_disconnect_timeout_wedges(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 6);
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE); // -> DWELL
	for(int i = 0; i < 14; i++)
		mp_reconnect_soak_state_tick(&st, true, true, 1.0f);
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_DISCONNECT); // -> WAIT_DISCONNECT

	for(int i = 0; i < 19; i++) {
		assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
			   == MRS_ACTION_NONE);
		assert(!st.wedged);
	}
	assert(mp_reconnect_soak_state_tick(&st, true, true, 1.0f)
		   == MRS_ACTION_NONE); // 20.0f reached, remote still active -- wedge
	assert(st.wedged);
	assert(st.cycles_completed == 0);
}

static void test_mp_reconnect_soak_reset_defaults_target_when_nonpositive(void) {
	struct mp_reconnect_soak_state st;
	mp_reconnect_soak_state_reset(&st, true, 0);
	assert(st.cycles_target == MP_RECONNECT_SOAK_CYCLES_TARGET);
	mp_reconnect_soak_state_reset(&st, true, -3);
	assert(st.cycles_target == MP_RECONNECT_SOAK_CYCLES_TARGET);
	mp_reconnect_soak_state_reset(&st, true, 3);
	assert(st.cycles_target == 3);
}

#include "../source/platform/frame_wait_timing.h"
static void test_performance_profiles_avoid_periodic_sd_io(void) {
	assert(!hw_run_periodic_sd_trace(NULL));
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	assert(!hw_run_periodic_sd_trace(&cfg));
	cfg.enabled = true;
	const enum hw_run_profile quiet[] = {
		HW_RUN_PROFILE_FAR_IDLE_2M, HW_RUN_PROFILE_FAR_WALK_6M,
		HW_RUN_PROFILE_KEEPUP_WALK_6M, HW_RUN_PROFILE_PERF_STRESS_5M,
		HW_RUN_PROFILE_MP_LAN_WALK_10M,
	};
	for(int p = HW_RUN_PROFILE_FAR_IDLE_2M; p <= HW_RUN_PROFILE_KEEPUP_WALK_6M; p++) {
		bool expected = true;
		for(size_t i = 0; i < sizeof(quiet) / sizeof(*quiet); i++)
			if(p == quiet[i]) expected = false;
		cfg.profile = p;
		assert(hw_run_periodic_sd_trace(&cfg) == expected);
		cfg.enabled = false;
		assert(!hw_run_periodic_sd_trace(&cfg));
		cfg.enabled = true;
	}
	cfg.profile = HW_RUN_PROFILE_SPLIT_DIMS_6M;
	assert(!hw_run_periodic_sd_trace(&cfg));
	puts("PASS: performance profiles omit periodic SD writes; diagnostic profiles retain them");
}
static void test_wait_evidence(void) {
	assert(frame_wait_resume_delay_us(100, 99, 10000) == 0);
	assert(frame_wait_resume_delay_us(100, 100, 10000) == 0);
	assert(frame_wait_resume_delay_us(100, 101, 10000) == 9899);
	assert(frame_wait_resume_delay_us(100, 101, 100) == 0);
	assert(frame_wait_resume_delay_us(0, 1, UINT64_MAX) == UINT32_MAX);
	struct hw_frame_evidence s = {0};
	hw_frame_evidence_note_wait(&s, false, 50000, 50000);
	assert(s.vi_ready_max_us == 0 && s.gpu_poll_late_max_us == 0);
	hw_frame_evidence_note_wait(&s, true, 8000, 8001);
	hw_frame_evidence_note_wait(&s, true, 8001, 8000);
	hw_frame_evidence_note_wait(&s, true, 0, 0);
	assert(s.vi_ready_over_8000 == 1 && s.gpu_poll_late_over_8000 == 1);
	assert(s.vi_ready_max_us == 8001 && s.gpu_poll_late_max_us == 8001);
	hw_frame_evidence_note_trace(&s, false, 0.05F);
	assert(s.sd_trace_max_us == 0);
	hw_frame_evidence_note_trace(&s, true, 0.05F);
	hw_frame_evidence_note_trace(&s, true, 0.0F);
	assert(s.sd_trace_over_30ms == 1 && s.sd_trace_max_us >= 50000);
	hw_frame_evidence_note_trace(&s, true, NAN);
	assert(s.invalid);
	puts("PASS: released-buffer age excluded; scheduling delay retained across heartbeats");
}
static void test_per_frame_evidence(void) {
	struct hw_frame_evidence s = {0};
	hw_frame_evidence_note(&s, false, 2.0F, 0.1F);
	assert(s.frames == 0 && s.max_us == 0);
	hw_frame_evidence_note(&s, true, 1.0F / 60.0F, 0.004F);
	assert(s.frames == 1 && s.max_us == 16667 && s.over_30ms == 0);
	/* No heartbeat reset: a later good frame cannot erase a stall. */
	hw_frame_evidence_note(&s, true, 0.05F, 0.020F);
	hw_frame_evidence_note(&s, true, 1.0F / 60.0F, 0.009F);
	assert(s.frames == 3 && s.max_us >= 50000 && s.over_30ms == 1);
	assert(s.gpu_over_16600 == 1 && s.gpu_over_8000 == 2);
	assert(s.duration_us >= 83334);
	hw_frame_evidence_note(&s, true, NAN, 0);
	assert(s.invalid && s.frames == 3);
	puts("R4 PASS: every-frame evidence excludes loading, retains hitches and rejects invalid timing");
}

/* Maintainer 2026-10-31 option 2: a slow frame inside the 10 s post-load
 * settle is not counted; the same slow frame after it is. */
static void test_frame_settle_window(void) {
	struct hw_frame_settle w = {0};
	struct hw_frame_evidence s = {0};
	const float dt = 1.0F / 60.0F;
	/* Loading screen / transition frames never advance the window. */
	assert(!hw_frame_settle_admit(&w, false, 5.0F) && w.playable_s == 0.0F);
	/* 9.5 s of good gameplay, then one 50 ms frame inside the window. */
	int n = 0;
	while(w.playable_s < HW_FRAME_EVIDENCE_SETTLE_S - 0.5F) {
		assert(!hw_frame_settle_admit(&w, true, dt));
		n++;
	}
	assert(n > 500);
	bool early = hw_frame_settle_admit(&w, true, 0.05F);
	hw_frame_evidence_note(&s, early, 0.05F, 0.02F);
	assert(!early && s.frames == 0 && s.over_30ms == 0 && s.max_us == 0);
	/* Finish the window; the frame that crosses 10 s still belongs to it. */
	bool crossed = false;
	while(w.playable_s < HW_FRAME_EVIDENCE_SETTLE_S)
		crossed = hw_frame_settle_admit(&w, true, dt);
	assert(!crossed);
	/* Now the identical slow frame is graded. */
	bool late = hw_frame_settle_admit(&w, true, 0.05F);
	hw_frame_evidence_note(&s, late, 0.05F, 0.02F);
	assert(late && s.frames == 1 && s.over_30ms == 1 && s.max_us >= 50000);
	/* Once settled a long frame stays admitted (never re-arms the window). */
	assert(hw_frame_settle_admit(&w, true, 3.0F));
	/* Leaving gameplay (menu/world switch) restarts the settle. */
	assert(!hw_frame_settle_admit(&w, false, dt) && w.playable_s == 0.0F);
	assert(!hw_frame_settle_admit(&w, true, dt));
	puts("PASS: release gate skips the 10 s post-load settle, grades every later frame (maintainer 2026-10-31 option 2)");
}

static void test_worst_frame_parts(void) {
	struct hw_frame_evidence s = {0};
	const float a[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
	const float b[20] = {20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};
	hw_frame_evidence_note_parts(&s, false, .05f, a);
	assert(s.worst_parts_us[0] == 0);
	hw_frame_evidence_note_parts(&s, true, .05f, a);
	hw_frame_evidence_note(&s, true, .05f, .008f);
	hw_frame_evidence_note_parts(&s, true, .04f, b);
	for(int i = 0; i < 20; i++) assert(s.worst_parts_us[i] == (uint32_t)(i+1)*1000);
	hw_frame_evidence_note_parts(&s, true, .06f, b);
	assert(s.worst_parts_us[0] == 20000 && s.worst_parts_us[19] == 1000);
	float invalid[20] = {NAN};
	hw_frame_evidence_note_parts(&s, true, .07f, invalid);
	assert(s.invalid == 1 && s.worst_parts_us[0] == 0);
	puts("PASS: worst admitted frame retains its own component timings");
}

static void test_performance_profile_disables_send_probes(void) {
	struct hw_telemetry_config cfg = {
		.enabled = true, .host = "pull", .port = 39017,
		.run_id = "probe-test", .profile = "keepup_walk_6m",
		.suppress_sd_probes = true
	};
	assert(hw_telemetry_init(&cfg));
	assert(hw_telemetry_active());
	assert(!hw_telemetry_sd_probes_enabled());
	cfg.suppress_sd_probes = false;
	assert(hw_telemetry_init(&cfg));
	assert(hw_telemetry_sd_probes_enabled());
	hw_telemetry_shutdown();
	assert(!hw_telemetry_sd_probes_enabled());
	puts("PASS: performance telemetry stays active without synchronous SD send probes");
}

static void test_split_anchor_wire(void) {
	struct hw_run_config cfg;
	char error[128];
	char* args[] = {"test",			"--cw-anchor-sep", "96",
					"--cw-profile", "split_anchor_6m", "--cw-tel-host",
					"pull",			"--cw-seed",	   "12345"};
	assert(hw_run_parse_args(&cfg, 9, args, error, sizeof(error)));
	assert(cfg.anchor_sep == 96 && cfg.autopilot_keepup
		   && cfg.duration_seconds == 360);
	assert(!hw_run_periodic_sd_trace(&cfg));
	char* bad[] = {"test", "--cw-anchor-sep", "96junk"};
	assert(!hw_run_parse_args(&cfg, 3, bad, error, sizeof(error))
		   && !cfg.enabled);
	struct hw_telemetry_heartbeat hb = {0};
	char old[HW_TELEMETRY_HEARTBEAT_LINE_SIZE],
		extended[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(old, sizeof(old), "test",
											 "split_anchor_6m", &hb);
	hb.anchor.enabled = true;
	hb.anchor.separation = 96;
	hb.anchor.frame_count = HW_ANCHOR_FRAME_CAP;
	for(unsigned j = 0; j < HW_ANCHOR_FRAME_CAP; j++)
		hb.anchor.frame_us[j] = UINT32_MAX;
	size_t m = hw_telemetry_format_heartbeat(extended, sizeof(extended), "test",
											 "split_anchor_6m", &hb);
	assert(m < sizeof(extended) && m > n);
	assert(m < 8192); /* Census uses separate bounded packets. */
	assert(!memcmp(old, extended, n - 1)); /* all pre-existing field bytes */
	assert(strstr(extended, "\"anchor_sep\":96"));
	puts("PASS split-anchor parser, exact old wire prefix and complete frame "
		 "batch");
}

static void test_split_guest_eye_wire(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	hb.guest = true; hb.gx = 1.6F; hb.gz = -138.3F;
	hb.storage_enabled = true;
	hw_telemetry_split_guest_eye(63.25F, 9 /* still water */, 19);
	size_t n = hw_telemetry_format_heartbeat(line, sizeof line,
		"test", "split_play_8m", &hb);
	assert(n < sizeof line);
	assert(strstr(line, "\"gy\":63.25"));
	assert(strstr(line, "\"guest_head_block\":9"));
	assert(strstr(line, "\"guest_air\":19"));
	puts("PASS split guest wire: eye y, head block, oxygen");
}

static void test_death_fields_always_present(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char buf[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	for(unsigned enabled = 0; enabled < 2; enabled++) {
		hb.anchor.enabled = enabled;
		hb.anchor.deaths = 0;
		hb.anchor.death_cause = NULL;
		size_t n = hw_telemetry_format_heartbeat(buf, sizeof(buf), "test",
			"split_anchor_6m", &hb);
		assert(n < sizeof(buf) && strlen(buf) == n);
		assert(strstr(buf, "\"deaths\":0,\"death_cause\":\"none\""));
		if(enabled)
			assert(strstr(buf, "\"anchor_death_cause\":\"none\""));
	}
	hb.anchor.deaths = 1;
	hb.anchor.death_cause = "fall";
	hw_telemetry_format_heartbeat(buf, sizeof(buf), "test", "split_anchor_6m", &hb);
	assert(strstr(buf, "\"deaths\":1,\"death_cause\":\"fall\""));
	hb.anchor.death_cause = NULL;
	hw_telemetry_format_heartbeat(buf, sizeof(buf), "test", "split_anchor_6m", &hb);
	assert(strstr(buf, "\"deaths\":1,\"death_cause\":\"unknown\""));
	puts("PASS heartbeat death fields: always present, zero/none before death");
}

static void test_storage_extension_flag_off_byte_identity(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char old[HW_TELEMETRY_HEARTBEAT_LINE_SIZE], expected[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	char expected_flagged[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	char ext[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	/* General telemetry works without CPU/anchor observers; also test coexistence. */
	for(unsigned observers = 0; observers < 2; observers++) {
		hb.storage_enabled = false;
		hb.cpu.enabled = hb.anchor.enabled = observers;
		hb.anchor.frame_count = observers ? HW_ANCHOR_FRAME_CAP : 0;
		for(unsigned i = 0; i < hb.anchor.frame_count; i++) hb.anchor.frame_us[i] = UINT32_MAX;
		size_t n = hw_telemetry_format_heartbeat(old, sizeof(old), "test", "split_anchor_6m", &hb);
		hb.storage_enabled = true;
		hb.item_nbt_exhaustions = UINT32_MAX;
		hb.op = UINT32_MAX;
		hb.oph = UINT32_MAX;
		hb.opc = UINT32_MAX;
		hb.opf = UINT32_MAX;
		hb.oer = UINT32_MAX;
		hb.orc = UINT32_MAX;
		hb.ocy = hb.oex = hb.owu = UINT32_MAX;
		hb.db = UINT32_MAX;
		hb.ds = UINT32_MAX;
		hb.dln = UINT32_MAX;
		hb.dls = UINT32_MAX;
		hb.dlp = UINT32_MAX;
		hb.dlc = 0;
		memcpy(expected, old, n - 1);
		int suffix = snprintf(expected + n - 1, sizeof(expected) - (n - 1),
			",\"item_nbt_exhaustions\":%u,\"op\":%u,\"oph\":%u,\"opc\":%u,\"opf\":%u,\"oer\":%u,\"orc\":%u,\"ocy\":%u,\"oex\":%u,\"owu\":%u,\"db\":%u,\"ds\":%u,\"dln\":%u,\"dls\":%u,\"dlp\":%u}",
			hb.item_nbt_exhaustions, hb.op, hb.oph, hb.opc, hb.opf, hb.oer,
			hb.orc, hb.ocy, hb.oex, hb.owu, hb.db, hb.ds, hb.dln, hb.dls, hb.dlp);
		assert(suffix > 0 && (size_t)suffix < sizeof(expected) - (n - 1));
		size_t expected_n = n - 1 + (size_t)suffix;
		size_t m = hw_telemetry_format_heartbeat(ext, sizeof(ext), "test", "split_anchor_6m", &hb);
		assert(m == expected_n && !memcmp(ext, expected, expected_n + 1));
		assert(!strstr(ext, "\"dlc\":"));
		assert(m < sizeof(ext) && m < 8192);
		assert(strstr(ext, "\"item_nbt_exhaustions\":4294967295"));
		assert(strstr(ext, "\"op\":4294967295"));
		assert(strstr(ext, "\"oph\":4294967295"));
		assert(strstr(ext, "\"opc\":4294967295"));
		assert(strstr(ext, "\"opf\":4294967295"));
		assert(strstr(ext, "\"oer\":4294967295"));
		assert(strstr(ext, "\"orc\":4294967295"));
		assert(strstr(ext, "\"ocy\":4294967295"));
		assert(strstr(ext, "\"oex\":4294967295"));
		assert(strstr(ext, "\"owu\":4294967295"));
		assert(strstr(ext, "\"db\":4294967295"));
		assert(strstr(ext, "\"ds\":4294967295"));
		assert(strstr(ext, "\"dln\":4294967295"));
		assert(strstr(ext, "\"dls\":4294967295"));
		assert(strstr(ext, "\"dlp\":4294967295"));
		memcpy(expected_flagged, expected, expected_n - 1);
		int flagged_suffix = snprintf(expected_flagged + expected_n - 1,
			sizeof(expected_flagged) - (expected_n - 1), ",\"dlc\":1}");
		assert(flagged_suffix > 0 &&
			(size_t)flagged_suffix < sizeof(expected_flagged) - (expected_n - 1));
		size_t expected_flagged_n = expected_n - 1 + (size_t)flagged_suffix;
		hb.dlc = 1;
		size_t flagged_n = hw_telemetry_format_heartbeat(ext, sizeof(ext), "test", "split_anchor_6m", &hb);
		assert(flagged_n == expected_flagged_n &&
			!memcmp(ext, expected_flagged, expected_flagged_n + 1));
		printf("PASS storage extension: flag-off exact pre-extension identity; +%u maximum bytes; combined line %u bytes\n",
			(unsigned)(flagged_n - n), (unsigned)flagged_n);
	}
}

static void test_tps_profile_windows(void) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	/* Every named profile, including the default; no split-only arming. */
	for(int profile = HW_RUN_PROFILE_FAR_IDLE_2M; profile <= HW_RUN_PROFILE_SPLIT_ANCHOR_6M; profile++) {
		hw_run_apply_profile(&cfg, profile);
		cfg.tps_cpu = true;
		struct hw_run_tps_window window = {0};
		assert(!hw_run_tps_measuring(&window, &cfg, false, false, false, 100));
		assert(!hw_run_tps_measuring(&window, &cfg, true, false, false, 200));
		assert(!hw_run_tps_measuring(&window, &cfg, true, false, false, 5199));
		if(profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M) {
			assert(!hw_run_tps_measuring(&window, &cfg, true, false, false, 99999));
			assert(hw_run_tps_measuring(&window, &cfg, true, true, false, 100000));
		} else {
			assert(hw_run_tps_measuring(&window, &cfg, true, false, false, 5200));
			assert(hw_run_tps_measuring(&window, &cfg, false, false, false, 6000));
		}
		assert(!hw_run_tps_measuring(&window, &cfg, true, true, true, 100001));
		cfg.tps_cpu = false;
		assert(!hw_run_tps_measuring(&window, &cfg, true, true, false, 100002));
	}
	puts("PASS TPS all-profile load + 5000ms settle, continuous window, split readiness unchanged");
}

// push-0921/C1c HIGH-1. main.c emits the run's ONE `tps_window` observer
// record on a RISING EDGE it reads out of the window struct itself --
// `bool tps_was_measuring = tps_window.measuring;` (main.c:2862) against this
// frame's answer (main.c:2875-2881). So the struct's `measuring` field is not
// bookkeeping: it IS last frame's answer, and any branch that returns true
// without recording it makes every measuring frame look like the first one.
// Every call in the tests below goes through this wrapper, which pins that
// invariant on every branch for every profile.
static bool tps_measuring_checked(struct hw_run_tps_window* window,
	const struct hw_run_config* cfg, bool world_loaded, bool anchor_measuring,
	bool done, uint64_t now_ms) {
	bool measuring = hw_run_tps_measuring(window, cfg, world_loaded,
										  anchor_measuring, done, now_ms);
	assert(window->measuring == measuring);
	return measuring;
}

// The same gate replayed over a whole run at the console's ~30 fps, counting
// how many records main.c would emit. `lost_from_ms`/`lost_until_ms` take the
// readiness latch away again for a stretch (pass 0/0 for none).
struct tps_edge_probe {
	unsigned emits, measuring_frames;
};

static struct tps_edge_probe tps_window_emits(const struct hw_run_config* cfg,
	uint32_t ready_ms, uint32_t lost_from_ms, uint32_t lost_until_ms) {
	struct hw_run_tps_window window = {0};
	struct tps_edge_probe out = {0, 0};
	const uint32_t loaded_ms = 5000, step_ms = 33;
	uint32_t horizon = (uint32_t)cfg->duration_seconds * 1000u + ready_ms;
	for(uint32_t ms = 0; ms <= horizon; ms += step_ms) {
		bool ready = ms >= ready_ms
			&& !(ms >= lost_from_ms && ms < lost_until_ms);
		uint32_t anchor_ms = ready ? ms - ready_ms : 0;
		bool done = hw_run_tps_window_end(cfg, (float)ms / 1000.0F, ready,
										  anchor_ms);
		bool was_measuring = window.measuring; // main.c:2862
		bool measuring = hw_run_tps_measuring(&window, cfg, ms >= loaded_ms,
											  ready, done, ms);
		// main.c:2875-2881, the emit gate verbatim (split_anchor_6m is
		// excluded there on purpose).
		if(measuring && !was_measuring
		   && cfg->profile != HW_RUN_PROFILE_SPLIT_ANCHOR_6M)
			out.emits++;
		if(measuring)
			out.measuring_frames++;
	}
	return out;
}

// One record per window, for every profile that emits one at all. The record
// goes into a telemetry ring with three observer-usable slots that a real
// console drains at ~2 records/s (source/platform/hw_tel_ring.h), so a
// per-frame producer pushes every other observer record -- including
// `anchor_frames` -- out of it, and a dropped anchor_frames packet is a hard
// collector FAIL ("missing per-frame samples"). main.c already throttles the
// carve census to one record per frame for exactly this reason.
static void test_tps_window_record_emitted_once(void) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	unsigned failures = 0;
	for(int profile = HW_RUN_PROFILE_FAR_IDLE_2M;
		profile <= HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M; profile++) {
		hw_run_apply_profile(&cfg, (enum hw_run_profile)profile);
		cfg.tps_cpu = true;
		struct tps_edge_probe probe = tps_window_emits(&cfg, 25000, 0, 0);
		// split_anchor_6m is the one profile main.c never emits the record
		// for; every other profile announces its window exactly once.
		unsigned want = cfg.profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M ? 0u : 1u;
		assert(probe.measuring_frames > 0);
		if(probe.emits != want) {
			fprintf(stderr, "FAIL %s: %u tps_window records emitted over %u "
				"measuring frames; expected %u\n",
				hw_run_profile_name((enum hw_run_profile)profile),
				probe.emits, probe.measuring_frames, want);
			failures++;
		}
	}
	// Readiness lost and regained mid-window: TWO records is the right answer,
	// not one. The record announces a window OPENING, and a window that really
	// closed and reopened is not the single continuous interval the analyzer
	// grades -- tools/analyze_tps_walk.py requires exactly one record before
	// it will call a run single_anchor, so the second record correctly
	// withdraws that claim instead of hiding the gap. Production cannot
	// produce it: hw_anchor_evidence_begin (hw_telemetry.c) is the only writer
	// of the anchor's measuring latch and only ever writes true.
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_SPLIT_PLAY_8M);
	cfg.tps_cpu = true;
	struct tps_edge_probe regained = tps_window_emits(&cfg, 25000, 100000,
													  105000);
	if(regained.emits != 2) {
		fprintf(stderr, "FAIL split_play_8m readiness lost+regained: %u "
			"tps_window records; expected 2 (one per genuine opening)\n",
			regained.emits);
		failures++;
	}
	assert(!failures);
	puts("PASS one tps_window record per window for every profile that emits "
		 "it (split_anchor_6m: none, by design), and a second one only when "
		 "readiness was really lost and regained");
}

// push-0921/C1 deliverable 2: WHICH window each profile's TPS observer runs
// on, pinned by name for every registered profile so no profile can change
// branch unnoticed. ANCHOR profiles gate the window on the two-anchor
// installed-readiness latch (hw_run_tps_measuring returns anchor_measuring),
// so loading never consumes measured time; every other profile gates on
// world_loaded + HW_RUN_TPS_SETTLE_MS. The ANCHOR set is exactly the set of
// profiles main.c arms the anchor observer for (main.c's s_anchor.enabled:
// anchor_sep > 0, split_anchor_6m, split_play_2m, split_play_8m,
// split_actions_6m) -- a profile with no readiness latch would measure
// nothing at all on that branch. split_play_8m and split_actions_6m were
// MISSING from it (KANBAN S1-d), so the two certifying guest-body profiles
// measured their TPS from five seconds after world load -- through the
// remaining loading and the guest Join handshake -- instead of from installed
// readiness like split_anchor_6m. The collector's own table already called
// them anchor-gated (tools/wii_telemetry_server.py's tps_settle_ms is None
// for every split profile; tests/test_analyze_tps_walk.py pins it and says
// "must stay in step with hw_run_tps_measuring"); this is the device side
// catching up.
static void test_tps_window_profile_table(void) {
	static const struct {
		enum hw_run_profile profile;
		bool anchor;
	} table[] = {
		{HW_RUN_PROFILE_FAR_IDLE_2M, false},
		{HW_RUN_PROFILE_HEARTBEAT_SELFTEST, false},
		{HW_RUN_PROFILE_FAR_WALK_6M, false},
		{HW_RUN_PROFILE_PERF_STRESS_5M, false},
		{HW_RUN_PROFILE_WATCHDOG_INJECT, false},
		{HW_RUN_PROFILE_REDSTONE_CLOCK_2M, false},
		{HW_RUN_PROFILE_MENUS_DWELL, false},
		{HW_RUN_PROFILE_SAVE_MIGRATE, false},
		{HW_RUN_PROFILE_MP_CONNECT_SMOKE, false},
		{HW_RUN_PROFILE_MP_LAN_WALK_10M, false},
		{HW_RUN_PROFILE_MP_RECONNECT_SOAK, false},
		{HW_RUN_PROFILE_MENUS_DWELL_CREATIVE, false},
		{HW_RUN_PROFILE_KEEPUP_WALK_6M, false},
		{HW_RUN_PROFILE_SPLIT_ANCHOR_6M, true},
		{HW_RUN_PROFILE_SPLIT_PLAY_2M, true},
		{HW_RUN_PROFILE_SPLIT_PLAY_8M, true},
		{HW_RUN_PROFILE_SPLIT_ACTIONS_6M, true},
		// split_recovery_6m keeps the settle window on purpose: main.c arms
		// NO anchor observer for it (its heartbeats carry no anchor_* fields
		// at all), so the readiness latch it would gate on can never rise.
		// It is not a throughput profile -- it proves state machines.
		{HW_RUN_PROFILE_SPLIT_RECOVERY_6M, false},
		{HW_RUN_PROFILE_MP_CHAT_LOGIN_3M, false},
		{HW_RUN_PROFILE_MP_CHAT_CHORD_1M, false},
		// mp_play_10m (push-0921/B4c): no anchor observer is armed for it
		// either (main.c's s_anchor.enabled set is unchanged by that card),
		// so it keeps the world_loaded + settle branch like the other MP
		// profiles. It is not a throughput profile -- it proves a play /
		// rejoin / state-verify state machine.
		{HW_RUN_PROFILE_MP_PLAY_10M, false},
		{HW_RUN_PROFILE_MP_PAUSE_MENU_3M, false},
		{HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M, false},
		{HW_RUN_PROFILE_SPLIT_DIMS_6M, true},
	};
	// Every profile in the enum is present, once, in declaration order.
	assert(sizeof table / sizeof *table
		   == (size_t)HW_RUN_PROFILE_SPLIT_DIMS_6M + 1);
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	for(size_t i = 0; i < sizeof table / sizeof *table; i++) {
		assert(table[i].profile == (enum hw_run_profile)i);
		hw_run_apply_profile(&cfg, table[i].profile);
		cfg.tps_cpu = true;
		struct hw_run_tps_window window = {0};
		if(table[i].anchor) {
			// A loaded world and any amount of settle time never open it...
			assert(!tps_measuring_checked(&window, &cfg, true, false, false, 100));
			assert(!tps_measuring_checked(&window, &cfg, true, false, false, 5200));
			assert(!tps_measuring_checked(&window, &cfg, true, false, false, 99999));
			// ...only the readiness latch does, and the window follows THAT
			// (a later menu/reload with world_loaded false keeps measuring).
			assert(tps_measuring_checked(&window, &cfg, true, true, false, 100000));
			assert(tps_measuring_checked(&window, &cfg, false, true, false, 100001));
			assert(!tps_measuring_checked(&window, &cfg, true, false, false, 100002));
		} else {
			assert(!tps_measuring_checked(&window, &cfg, false, false, false, 100));
			assert(!tps_measuring_checked(&window, &cfg, true, false, false, 200));
			assert(!tps_measuring_checked(&window, &cfg, true, false, false, 5199));
			assert(tps_measuring_checked(&window, &cfg, true, false, false, 5200));
			// One continuous interval, including later menu/reconnect work.
			assert(tps_measuring_checked(&window, &cfg, false, false, false, 6000));
			// And the readiness latch is NOT this branch's signal: without
			// the settle it stays shut.
			struct hw_run_tps_window fresh = {0};
			assert(!tps_measuring_checked(&fresh, &cfg, false, true, false, 100));
		}
		// Completion and a disarmed observer close it for every profile alike.
		assert(!tps_measuring_checked(&window, &cfg, true, true, true, 100003));
		cfg.tps_cpu = false;
		assert(!tps_measuring_checked(&window, &cfg, true, true, false, 100004));
	}
	puts("PASS TPS window branch pinned per profile: the four anchor-observer "
		 "profiles (split_anchor_6m, split_play_2m, split_play_8m, "
		 "split_actions_6m) measure from installed readiness; every other "
		 "profile keeps the world_loaded + 5000 ms settle");
}

// push-0921/C1b: WHICH clock ends each profile's TPS window, driven as a whole
// run at 1 Hz with installed readiness landing 25 s in -- the case that
// exposes the defect. A window that OPENS on the readiness epoch and CLOSES on
// the run clock loses exactly the loading/readiness time: split_play_8m sealed
// at run 358 s with only 333 s measured of the 360 s window it is graded on.
// The close clock must be the open clock.
static void test_tps_window_close_clock(void) {
	enum close_rule {
		CLOSE_RUN,    // run clock reaches duration - HW_RUN_TPS_SEAL_MARGIN
		CLOSE_ANCHOR, // readiness clock reaches the same margin
		CLOSE_NEVER,  // no clock ends it; completion seals it in main.c
	};
	static const struct {
		enum hw_run_profile profile;
		enum close_rule rule;
	} table[] = {
		{HW_RUN_PROFILE_FAR_IDLE_2M, CLOSE_RUN},
		{HW_RUN_PROFILE_HEARTBEAT_SELFTEST, CLOSE_RUN},
		{HW_RUN_PROFILE_FAR_WALK_6M, CLOSE_RUN},
		{HW_RUN_PROFILE_PERF_STRESS_5M, CLOSE_RUN},
		{HW_RUN_PROFILE_WATCHDOG_INJECT, CLOSE_RUN},
		{HW_RUN_PROFILE_REDSTONE_CLOCK_2M, CLOSE_RUN},
		{HW_RUN_PROFILE_MENUS_DWELL, CLOSE_RUN},
		{HW_RUN_PROFILE_SAVE_MIGRATE, CLOSE_RUN},
		{HW_RUN_PROFILE_MP_CONNECT_SMOKE, CLOSE_RUN},
		{HW_RUN_PROFILE_MP_LAN_WALK_10M, CLOSE_RUN},
		{HW_RUN_PROFILE_MP_RECONNECT_SOAK, CLOSE_RUN},
		{HW_RUN_PROFILE_MENUS_DWELL_CREATIVE, CLOSE_RUN},
		{HW_RUN_PROFILE_KEEPUP_WALK_6M, CLOSE_RUN},
		// Unchanged: the completion block seals this one (see the helper).
		{HW_RUN_PROFILE_SPLIT_ANCHOR_6M, CLOSE_NEVER},
		// The 120 s smoke counts its readiness budget INSIDE its duration by
		// design (Astra ruling 2026-09-10) -- its completion is on the run
		// clock, so its window closes there too.
		{HW_RUN_PROFILE_SPLIT_PLAY_2M, CLOSE_RUN},
		{HW_RUN_PROFILE_SPLIT_PLAY_8M, CLOSE_ANCHOR},
		{HW_RUN_PROFILE_SPLIT_ACTIONS_6M, CLOSE_ANCHOR},
		// No anchor observer is armed for it, so it opens (and closes) on the
		// settle/run clock like the non-split profiles.
		{HW_RUN_PROFILE_SPLIT_RECOVERY_6M, CLOSE_RUN},
		{HW_RUN_PROFILE_MP_CHAT_LOGIN_3M, CLOSE_RUN},
		{HW_RUN_PROFILE_MP_CHAT_CHORD_1M, CLOSE_RUN},
		// Driver-owned completion like the two above, and no anchor
		// observer, so the run clock is what seals its window too.
		{HW_RUN_PROFILE_MP_PLAY_10M, CLOSE_RUN},
		{HW_RUN_PROFILE_MP_PAUSE_MENU_3M, CLOSE_RUN},
		{HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M, CLOSE_RUN},
	};
	assert(sizeof table / sizeof *table
		   == (size_t)HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M + 1);
	const uint32_t loaded_ms = 5000;  // world_loaded 5 s in
	const uint32_t ready_ms = 25000;  // installed readiness 25 s in
	const uint32_t margin_ms = HW_RUN_TPS_SEAL_MARGIN_SECONDS * 1000u;
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	unsigned failures = 0;
	for(size_t i = 0; i < sizeof table / sizeof *table; i++) {
		assert(table[i].profile == (enum hw_run_profile)i);
		hw_run_apply_profile(&cfg, table[i].profile);
		cfg.tps_cpu = true;
		assert((uint32_t)cfg.duration_seconds * 1000u > margin_ms + ready_ms);
		struct hw_run_tps_window window = {0};
		uint32_t opened = UINT32_MAX, closed = UINT32_MAX;
		uint32_t measured_ms = (uint32_t)cfg.duration_seconds * 1000u - margin_ms;
		uint32_t horizon = ready_ms + measured_ms + 10000u;
		for(uint32_t run_ms = 0; run_ms <= horizon; run_ms += 1000) {
			bool ready = run_ms >= ready_ms;
			uint32_t anchor_ms = ready ? run_ms - ready_ms : 0;
			bool done = hw_run_tps_window_end(&cfg, (float)run_ms / 1000.0F,
											  ready, anchor_ms);
			bool measuring = tps_measuring_checked(&window, &cfg,
				run_ms >= loaded_ms, ready, done, run_ms);
			if(measuring && opened == UINT32_MAX)
				opened = run_ms;
			if(!measuring && opened != UINT32_MAX && closed == UINT32_MAX)
				closed = run_ms;
		}
		uint32_t want = table[i].rule == CLOSE_RUN ? measured_ms :
			table[i].rule == CLOSE_ANCHOR ? ready_ms + measured_ms : UINT32_MAX;
		if(opened == UINT32_MAX || closed != want) {
			fprintf(stderr, "FAIL %s: window opened=%u closed=%u (expected "
				"close at %u); measured %u ms of the %u ms window\n",
				hw_run_profile_name(table[i].profile), opened, closed, want,
				closed == UINT32_MAX ? 0 : closed - opened, measured_ms);
			failures++;
		}
		// Whatever the clock rule, the measured span of a readiness-epoch
		// profile is the FULL window -- that is the point of this card.
		if(table[i].rule == CLOSE_ANCHOR && closed != UINT32_MAX
		   && closed - opened != measured_ms)
			failures++;
	}
	assert(!failures);
	puts("PASS TPS window close clock pinned per profile with readiness at "
		 "+25 s: split_play_8m/split_actions_6m seal on the readiness clock "
		 "with the full window measured; split_anchor_6m still seals only at "
		 "completion; every other profile keeps the run clock");
}

static void test_g9_pacing_wire(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	hw_telemetry_format_heartbeat(line, sizeof line, "r", "perf_stress_5m", &hb);
	assert(!strstr(line, "\"g9p\""));
	hb.g9p_present = true;
	for(unsigned i = 0; i < 22; i++) hb.g9p[i] = UINT32_MAX;
	size_t n = hw_telemetry_format_heartbeat(line, sizeof line, "r", "perf_stress_5m", &hb);
	assert(n < sizeof line && strlen(line) == n && line[n-1] == '}');
	assert(strstr(line, "\"g9p_v\":1,\"g9p\":[4294967295,4294967295"));
	puts("PASS G9 pacing heartbeat: dormant bytes, versioned full-width counters");
}

static void test_heartbeat_fps_window(bool print_wire) {
	struct hw_telemetry_fps_window window = {0};
	const struct {
		uint32_t counter;
		uint64_t now_us;
		uint32_t frames;
		uint64_t elapsed_us;
	} samples[] = {
		{100, 1000000, 0, 0},            /* first sample is unmeasured */
		{160, 2000000, 60, 1000000},
		{170, 2000000, 0, 0},           /* no duration; retain baseline */
		{220, 3000000, 60, 1000000},
		{280, 4100000, 60, 1100000},     /* includes a 100 ms SD stall */
		{460, 7100000, 180, 3000000},    /* skipped heartbeat attempts */
		{5, 8100000, 0, 0},             /* counter reset, not wrap */
		{65, 9100000, 60, 1000000},
		{70, 9000000, 0, 0},            /* monotonic clock reset */
		{130, 10000000, 60, 1000000},
		{130, 11000000, 0, 1000000},    /* a genuine zero-FPS window */
	};
	struct hw_telemetry_heartbeat hb = {.fps_window_present = true};
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	char legacy[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	for(size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
		struct hw_telemetry_fps_sample s = hw_telemetry_sample_fps(
			&window, samples[i].counter, samples[i].now_us);
		assert(s.frames == samples[i].frames && s.elapsed_us == samples[i].elapsed_us);
		float expected = s.elapsed_us ? (float)((double)s.frames * 1e6 / s.elapsed_us) : 0;
		assert(isfinite(s.fps) && fabsf(s.fps - expected) < 0.0001F);
		hb.tick = (uint32_t)i;
		hb.frame_counter = samples[i].counter;
		hb.fps = s.fps;
		/* Existing tail/max/stall evidence travels beside the new FPS. */
		hb.frame_evidence.frames = 60;
		hb.frame_evidence.max_us = 116667;
		hb.frame_evidence.sd_trace_max_us = 100000;
		hb.frame_evidence.sd_trace_over_30ms = 1;
		size_t n = hw_telemetry_format_heartbeat(line, sizeof line,
			"fps-window", "heartbeat_selftest", &hb);
		assert(n < sizeof line && strlen(line) == n);
		assert(strstr(line, "\"wf\":") && !strstr(line, "\"fps\":"));
		hb.fps_window_present = false;
		size_t old_n = hw_telemetry_format_heartbeat(legacy, sizeof legacy,
			"fps-window", "heartbeat_selftest", &hb);
		hb.fps_window_present = true;
		/* Only the key changes. Max/tails, SD evidence and all other bytes
		 * are identical, including when the sampler returns zero. */
		char* key = strstr(legacy, "\"fps\":");
		assert(key && old_n == n + 1);
		memmove(key + 3, key + 4, strlen(key + 4) + 1);
		memcpy(key + 1, "wf", 2);
		assert(!strcmp(line, legacy));
		if(print_wire) puts(line);
	}
	window = (struct hw_telemetry_fps_window){0};
	struct hw_telemetry_fps_sample s = hw_telemetry_sample_fps(&window, UINT32_MAX - 29u, 1000000);
	assert(s.elapsed_us == 0);
	s = hw_telemetry_sample_fps(&window, 30, 2000000);
	assert(s.frames == 60 && s.elapsed_us == 1000000 && s.fps == 60.0F);
	/* Reset the sampler to begin a new run, even if its count increased. */
	window = (struct hw_telemetry_fps_window){0};
	s = hw_telemetry_sample_fps(&window, 1000, 3000000);
	assert(s.elapsed_us == 0 && s.fps == 0);
	if(!print_wire) puts("PASS: heartbeat FPS first/zero/wrap/reset/stall/dropped-window samples");
}

int main(int argc, char** argv) {
	if(argc == 2 && !strcmp(argv[1], "--save-path-burst")) {
		test_save_path_burst(); test_save_path_concurrent(); return 0;
	}
	if(argc > 1 && !strcmp(argv[1], "--observer-newline-red")) {
		test_observer_body_completion(); return 0;
	}
	if(argc > 2 && !strcmp(argv[1], "--observer-y-wire")) {
		test_y_census_body_completion(argv[2]); return 0;
	}
	if(argc > 1 && strcmp(argv[1], "--g9-send-wire") == 0) {
		test_g9_pair_fallback_and_failure(true);
		return 0;
	}
	if(argc > 1 && strcmp(argv[1], "--g9-send-only") == 0) {
		test_g9_negative_send_never_retries();
		test_g9_guard_heartbeat_transport();
		test_g9_pair_fallback_and_failure(false);
		test_g9_service_uses_normal_transport();
		assert(hw_telemetry_test_heartbeat_off_equiv());
		puts("PASS OFF: 10000 sender transitions byte-identical to frozen ring");
		return 0;
	}
	if(argc == 2 && !strcmp(argv[1], "--fps-window-fixtures")) {
		test_heartbeat_fps_window(true);
		return 0;
	}
	test_heartbeat_fps_window(false);
	test_g9_pacing_wire();
	test_observer_body_completion();
	/* The collector requests only the formatter fixture. Running the timed
	 * sender tests in that subprocess makes their Windows scheduling checks
	 * unrelated prerequisites for parsing a heartbeat line. An explicit argv
	 * flag (not an environment variable) so a leaked env var can never turn
	 * the ctest run into a vacuous one-fixture pass. */
	if(argc > 1 && strcmp(argv[1], "--worst-heartbeat-only") == 0) {
		g_worst_heartbeat_only = true;
		test_heartbeat_production_buffer_fits_full_payload();
		return 0;
	}
	enter_sandbox();
	test_g9_negative_send_never_retries();
	test_g9_guard_heartbeat_transport();
	test_g9_pair_fallback_and_failure(false);
	test_g9_service_uses_normal_transport();
	assert(hw_telemetry_test_heartbeat_off_equiv());
	puts("PASS OFF: 10000 sender transitions byte-identical to frozen ring");
	/* The threshold and winner are decided from production dwell/switch
	 * observations, never from a scripted elapsed-time guess. */
	assert(spq_remaining_ticks(0.0f) == 81);
	float dwell = 0.0f;
	for(int i = 0; i < 80; i++) dwell += 0.0125f;
	assert(spq_remaining_ticks(dwell) == 1);
	dwell += 0.0125f;
	assert(spq_remaining_ticks(dwell) == 0);
	struct spq_core core;
	spq_core_init(&core, 2);
	assert(!spq_core_step(&core, (struct spq_observation){.remaining = 3,
		.tick = 100, .up_ms = 5000}).request_quit);
	assert(spq_core_step(&core, (struct spq_observation){.remaining = 2,
		.tick = 101, .up_ms = 5050}).request_quit);
	assert(spq_core_step(&core, (struct spq_observation){.remaining = 1,
		.tick = 102, .up_ms = 5100, .switch_started = true}).winner
		== SPQ_SWITCH_FIRST);
	spq_core_init(&core, 0);
	assert(spq_core_step(&core, (struct spq_observation){.remaining = 0,
		.tick = 200, .up_ms = 10000}).request_quit);
	assert(spq_core_step(&core, (struct spq_observation){.remaining = 0,
		.tick = 201, .up_ms = 10050, .save_settled = true}).winner
		== SPQ_UNLOAD_FIRST);
	char portal_marker[256];
	assert(spq_format_marker(portal_marker, sizeof portal_marker, 3,
		"quit_requested", 1234, 81, 2, true,
		"\"production_srpc\":true") > 0);
	assert(strstr(portal_marker, "\"up_ms\":1234") != NULL);
	assert(strstr(portal_marker, "\"tick\":81") != NULL);
	assert(strstr(portal_marker, "\"remaining_ticks\":2") != NULL);
	assert(strstr(portal_marker, "\"production_srpc\":true") != NULL);
	assert(spq_after_confirm(100, 5000, 101, 5050));
	assert(!spq_after_confirm(100, 5000, 100, 5050));
	assert(!spq_after_confirm(100, 5000, 101, 4999));
	assert(spq_resolve_unload(true, true, false) == SPQ_UNLOAD_REPLIED_LATE);
	assert(spq_resolve_unload(true, false, false) == SPQ_UNLOAD_WAITING);
	assert(spq_resolve_unload(true, false, true) == SPQ_UNLOAD_DROPPED);
	assert(spq_resolve_unload(false, false, true) == SPQ_UNLOAD_DROPPED);
	assert(spq_format_marker(portal_marker, sizeof portal_marker, 4,
		"confirm_sent", 5050, 101, 2, true, NULL) > 0);
	assert(strstr(portal_marker, "\"marker\":\"confirm_sent\"") != NULL);
	assert(spq_format_marker(portal_marker, sizeof portal_marker, 5,
		"switch_begin", 5100, 102, 2, true, NULL) > 0);
	assert(strstr(portal_marker, "\"marker\":\"switch_begin\"") != NULL);
	test_tps_profile_windows();
	test_tps_window_record_emitted_once();
	test_tps_window_profile_table();
	test_tps_window_close_clock();
	test_bounded_sender();
	test_stop_drains_seal();
	test_stop_blocked_sender();
	test_sender_failure_trace();
	test_recorded_stream_death_evidence();
	test_real_wii_heartbeat_schema();
	test_tnt_verdict_field_is_always_formatted();
	test_storage_extension_flag_off_byte_identity();
	test_observer_packet();
	test_watchdog_start_never_takes_sender_wake();
	test_portal_quit_server_marker_wire();
	test_portal_quit_frame_clock_source();
	test_retry_producer_refusal_is_not_a_drop();
	test_save_path_burst();
	test_save_path_concurrent();
	test_carve_attrib_async_sender_e2e();
	test_wake_pending_is_never_lost();
	test_wake_drives_the_sender_loop();
	test_sender_starvation_heartbeat_priority();
	test_bounded_send_deadline_keeps_heartbeat_alive();
	test_critical_lines_survive_midline_stall();
	test_sender_stage_names_the_stuck_call();
	test_sender_stage_names_lock_park();
	test_nonblock_verify_catches_unlatched_setfl();
	test_nonblock_experiment_abandons_instead_of_blocking();
	{
		struct hw_run_config cfg;
		char error[128];
		char* args[] = {"test", "--cw-tps-cpu"};
		assert(hw_run_parse_args(&cfg, 2, args, error, sizeof(error)));
		assert(cfg.tps_cpu);
		assert(!cfg.carve_census);
		char* census_args[] = {"test", "--cw-carve-census"};
		assert(hw_run_parse_args(&cfg, 2, census_args, error, sizeof(error)));
		assert(cfg.carve_census && !cfg.tps_cpu);
	}
	test_death_fields_always_present();
	test_anchor_measurement_fixed_budget();
	test_anchor_readiness_window();
	test_split_anchor_wire();
	test_split_guest_eye_wire();
	test_performance_profile_disables_send_probes();
	test_worst_frame_parts();
	test_performance_profiles_avoid_periodic_sd_io();
	test_wait_evidence();
	test_per_frame_evidence();
	test_frame_settle_window();
	test_parse_default_and_profile();
	test_parse_pull_mode_without_real_host();
	test_parse_rejects_missing_host();
	test_parse_args_file();
	test_parse_redstone_clock_profile();
	test_parse_menus_dwell_profile();
	test_parse_save_migrate_profile();
	test_parse_auto_exit_flags();
	test_auto_exit_ready_gates_armed_runs_only();
	test_duration_reached_is_integer_ms();
	test_autopilot_unstick_only_armed_by_mp_lan_walk_profile();
	test_autopilot_unstick_disarmed_never_accumulates_state();
	test_autopilot_unstick_first_tick_only_seeds_baseline();
	test_autopilot_unstick_no_trigger_with_real_progress();
	test_autopilot_unstick_triggers_below_threshold();
	test_autopilot_unstick_repeated_triggers_snap_new_headings();
	test_autopilot_unstick_no_offset_when_never_stuck();
	test_autopilot_keepup_only_armed_by_keepup_profile();
	test_autopilot_keepup_disarmed_never_accumulates_state();
	test_autopilot_keepup_first_tick_only_seeds_baseline();
	test_autopilot_keepup_no_trigger_with_real_progress();
	test_autopilot_keepup_escape_schedule_table();
	test_autopilot_keepup_ladder_escalates_then_cycles_while_pinned();
	test_autopilot_keepup_rung_retreat_does_not_clear_episode();
	test_autopilot_keepup_clearing_the_anchor_resets_ladder();
	test_autopilot_keepup_no_offset_when_never_stuck();
	test_mp_reconnect_soak_only_armed_by_its_own_profile();
	test_mp_reconnect_soak_disarmed_never_returns_action();
	test_mp_reconnect_soak_wait_connect_then_dwell_then_disconnect();
	test_mp_reconnect_soak_dwell_freezes_on_transient_hold();
	test_mp_reconnect_soak_dwell_folds_into_wait_disconnect_on_drop();
	test_mp_reconnect_soak_full_two_cycle_run_reaches_done();
	test_mp_reconnect_soak_wait_connect_timeout_wedges();
	test_mp_reconnect_soak_wait_disconnect_timeout_wedges();
	test_mp_reconnect_soak_reset_defaults_target_when_nonpositive();
	test_telemetry_dhcp_retry_bound_is_short();
	test_retry_bringup_succeeds_first_try();
	test_retry_bringup_succeeds_after_failures();
	test_retry_bringup_exhausts_budget();
	test_retry_bringup_null_attempt_is_safe();
	test_retry_bringup_zero_sleep_ms_never_retries();
	test_telemetry_init_rejects_out_of_range_ports();
	test_heartbeat_json_escapes_strings();
	test_heartbeat_light_queue_v2_serialization();
	test_heartbeat_lqd_wait_attribution();
	test_heartbeat_production_buffer_fits_full_payload();
	test_overlay_spill_record();
	test_heartbeat_reports_truncation_for_small_buffer();
	test_heartbeat_device_uptime();
	test_heartbeat_reports_split_live_toggle();
	test_heartbeat_reports_mp_install_probe();
	test_heartbeat_reports_mp_terminal_kick();
	test_heartbeat_mp_end_reason_escapes();
	test_heartbeat_reports_mp_admission_budget();
	test_dump_chunk_json_escapes_text();
	test_send_line_retries_partial_tcp_writes();
	test_send_line_completes_more_than_256_partial_writes();
	test_send_line_rejects_zero_progress();
	test_send_line_partial_eagain();
	test_send_line_failure_accounting();
	test_send_line_newline_retry_budget();
	test_send_legacy_heartbeat_wire_identical();
	test_watchdog_dump_skips_tcp_when_send_lock_busy();
	test_emit_dump_chunks_real_send_path();
	test_emit_empty_dump_sends_one_last_chunk();
	test_emit_crash_mailbox_sends_saved_reports_once();
	test_emit_crash_mailbox_keeps_report_on_send_failure();
	test_emit_crash_mailbox_exact_chunk_multiple_terminates();
	test_preconnect_buffer_pins_boot_and_rings_rest();
	test_preconnect_vehicle_boot_burst_fits();
	test_heartbeat_vs_status();
	test_preconnect_full_arena_evicts_oldest_and_reports();
	test_preconnect_multi_eviction_counts_each();
	test_preconnect_buffer_partial_fill_keeps_order();
	test_preconnect_flush_failure_stops_and_retires();
	test_preconnect_empty_flush_is_zero();
	test_quiesce_stops_all_sends_until_reinit();
	test_established_client_send_failure_quiesces_immediately();
	test_send_fail_classify_enobufs_is_back_pressure();
	test_backpressure_soak_keeps_framing_and_stream();
	test_backpressure_soak_bounded_runs_survive_and_quiesce_beyond_bound();
	test_observer_drops_are_not_transport_failures();
	test_send_fail_classify_eagain_drops_line_keeps_socket();
	test_send_fail_classify_ewouldblock_is_also_transient();
	test_send_fail_classify_four_consecutive_eagain_quiesces();
	test_send_fail_classify_success_resets_consecutive_streak();
	test_send_fail_classify_hard_errno_closes_and_quiesces_immediately();
	test_send_fail_classify_hard_errno_after_eagain_streak_still_closes();
	test_send_fail_state_reset_clears_everything();
	test_send_fail_classify_null_state_is_safe();
	hw_telemetry_shutdown();
	assert(!hw_telemetry_active());
	leave_sandbox();
	return 0;
}
