/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	Flat platform dispatcher (mirrors source/platform/gfx.c). On Wii it pulls in the real
	implementation; on PC every watchdog_* is a no-op so the shared TUs (main.c,
	server_local.c) still build + link.
*/

#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "watchdog.h"
#include "service_observer.h"
#include "time.h"
#include "thread.h"
#include "g9_worst_tick.h"

// GP-wedge draw breadcrumb storage (see watchdog.h): written by the render
// path on BOTH platforms (PC writes are harmless), read by the Wii dump.
volatile uint32_t g_wd_draw_site = 0;
volatile uint32_t g_wd_draw_id = 0;
volatile uint32_t g_wd_draw_aux = 0;

enum wd_write_result wd_write_cleanup_result(int remove_result,
										  int remove_error) {
	return remove_result == 0 || remove_error == ENOENT ? WD_WRITE_NONE
														 : WD_WRITE_PARTIAL;
}

// --- report ASSEMBLY (shared PC/Wii -- see watchdog.h) ---------------------------------
// STATIC: the assembly runs after the heap is suspect (or from exception context), so it
// must not allocate. Single writer -- the watchdog thread on the hang path, the faulting
// thread on the panic path, and neither can be in the other.
static char s_wd_dump[WD_DUMP_CAP];

char* wd_dump_buf(void) { return s_wd_dump; }
void wd_dump_reset(void) { s_wd_dump[0] = '\0'; }

size_t wd_dump_appendf(size_t used, const char* fmt, ...) {
	if(used >= WD_DUMP_BODY_MAX - 1)
		return WD_DUMP_BODY_MAX - 1;
	va_list ap;
	va_start(ap, fmt);
	int written = vsnprintf(s_wd_dump + used, WD_DUMP_BODY_MAX - used, fmt, ap);
	va_end(ap);
	if(written < 0) {
		s_wd_dump[used] = '\0';
		return used;
	}
	if((size_t)written >= WD_DUMP_BODY_MAX - used)
		return WD_DUMP_BODY_MAX - 1;
	return used + (size_t)written;
}

size_t wd_dump_seal(size_t used) {
	// wd_dump_appendf clamps the BODY at WD_DUMP_BODY_MAX - 1, so [WD_DUMP_BODY_MAX,
	// WD_DUMP_CAP) is reserved and this copy cannot be squeezed out by an overlong
	// dump. That reservation is the whole point: if the trailer could be truncated
	// away, a complete write of an overlong report would read as a truncated file and
	// the on-card artifact would contradict the status row that says it is whole.
	if(used > WD_DUMP_BODY_MAX - 1)
		used = WD_DUMP_BODY_MAX - 1;
	memcpy(s_wd_dump + used, WD_DUMP_TRAILER, sizeof(WD_DUMP_TRAILER));
	return used + sizeof(WD_DUMP_TRAILER) - 1;
}

bool wd_dump_is_sealed(const char* text, size_t n) {
	const size_t len = sizeof(WD_DUMP_TRAILER) - 1;
	return text && n >= len && memcmp(text + n - len, WD_DUMP_TRAILER, len) == 0;
}

// Per-boot report sequence (see wd_dump_identity in watchdog.h). Same single-writer
// argument as the dump buffer itself: the watchdog thread on the hang path, the faulting
// thread on the panic path, and neither can be inside the other.
static uint32_t s_wd_report_seq = 0;

uint32_t wd_dump_report_seq(void) { return s_wd_report_seq; }

size_t wd_dump_identity(size_t used, const char* build_id, uint32_t boot_id,
						uint32_t uptime_ms) {
	// Increment FIRST so the number in the file is the sequence of the report being
	// written (the first report of a boot reads seq=1, not seq=0 -- 0 is "none written").
	s_wd_report_seq++;
	// Fallback literal deliberately NOT "unstamped": that exact token is the
	// build-ID gate's stale-stamp sentinel (build-verify.sh), and in non-LTO
	// dev builds this runtime string survives into the .dol (LTO folds it
	// away in release), false-tripping the mixed-ID check. build_id is never
	// NULL in a stamped build; this only labels a hand-rolled caller.
	return wd_dump_appendf(used, "id build=%s boot=%08x up=%ums seq=%u\n",
						   build_id ? build_id : "no_build_id", (unsigned)boot_id,
						   (unsigned)uptime_ms, (unsigned)s_wd_report_seq);
}

// UPTIME BASE (2026-07-29). The `up=` field used to pass ticks_to_millisecs(gettime())
// straight through, on the assumption that the PowerPC timebase reads zero at power-on. It
// does not: libogc's SYS_PreMain calls __SYS_SetBootTime -> __SYS_SetTime, which seeds the
// timebase from the RTC plus the CONF counter bias, and libogc's own gettod computes
// ticks_to_secs(gettime()) + 946684800 (1970->2000) -- so gettime()/60750000 is SECONDS
// SINCE 2000-01-01, not since boot. A report written during a sub-hour session therefore
// read up=1149429345ms, i.e. 13 days, and the field was worse than useless because it
// looked plausible. Every OTHER timing in the codebase uses DIFFERENCES (time_diff_ms,
// wd_record_heartbeat_gap, the 3s threshold) and was never affected.
//
// Shared rather than Wii-only for the same reason as the rest of the assembly: the
// arithmetic is then exercised off-device. It returns TICKS, not ms, because the
// ticks->ms divisor is a platform constant -- the Wii converts with libogc's own
// ticks_to_millisecs, whose exactness was verified independently (__PPCMulDiv(ticks, 1000,
// PPC_TIMER_CLOCK) with PPC_TIMER_CLOCK = 243000000/4, i.e. ticks/60750: no overflow, no
// scale error). The defect was never the conversion, it was the missing base, so what is
// pinned here is the base handling and the two ways it can lie.
//
// DO NOT "fix" the epoch by calling time_reset()/settime(0) at startup instead. That zeroes
// libogc's wall clock, breaking time(), the level.dat LastPlayed tag and the strftime column
// in screen_select_world.c, and makes any in-flight unsigned ptime_t difference underflow.
// time_reset() has zero call sites tree-wide and must keep them.
uint64_t wd_uptime_ticks(uint64_t base_ticks, uint64_t now_ticks) {
	// base 0 is the "not seeded yet" sentinel: wii/panic.c can fault before watchdog_init
	// returns. Report an honest up=0 rather than a wall-clock number that reads like a
	// multi-day uptime.
	if(base_ticks == 0)
		return 0;
	// The timebase is monotonic so now < base cannot happen -- but an unsigned underflow
	// here would print a ~9,700-year uptime, which is exactly the class of plausible-looking
	// lie this function exists to end. Refuse rather than wrap.
	if(now_ticks <= base_ticks)
		return 0;
	return now_ticks - base_ticks;
}

// Seq-suffixed report paths -- see the long note in watchdog.h for WHY (fopen("w")
// truncates, so one fixed path meant report N destroyed report N-1 and five of the
// 2026-07-29 session's six stalls were unrecoverable). Shared, so the string surgery is
// exercised off-device; pure, so it can be table-tested.
const char* wd_report_path_for_seq(char* buf, size_t cap, const char* base_path,
								   uint32_t seq) {
	if(!base_path)
		return base_path;
	// Report 1 keeps the canonical name: every existing collector, and every human who
	// knows to look at sd:/hang_report.txt, keeps working with no change.
	if(seq <= 1 || !buf || cap == 0)
		return base_path;
	// CLAMP rather than grow without bound: a stall that re-fires every 250ms would
	// otherwise fill the card. Slot MAX therefore holds the most RECENT report while
	// 1..MAX-1 hold the earliest.
	if(seq > WD_REPORT_SEQ_MAX)
		seq = WD_REPORT_SEQ_MAX;

	// Insert before the extension, so "sd:/hang_report.txt" -> "sd:/hang_report_2.txt"
	// and the files still open as text. Only a '.' AFTER the last '/' counts as an
	// extension -- otherwise a path like "./reports/hang_report" would split at the
	// leading "./" and produce garbage.
	size_t len = strlen(base_path);
	size_t stem = len; // no extension found -> suffix goes on the end
	for(size_t i = len; i > 0; i--) {
		char c = base_path[i - 1];
		if(c == '/' || c == '\\')
			break;
		if(c == '.') {
			stem = i - 1;
			break;
		}
	}

	int written = snprintf(buf, cap, "%.*s_%u%s", (int)stem, base_path, (unsigned)seq,
						   base_path + stem);
	// A truncated path would name the WRONG file (or an unopenable one), which is worse
	// than the overwrite this function exists to prevent. Fall back to the base path.
	if(written < 0 || (size_t)written >= cap)
		return base_path;
	return buf;
}

// --- emit ORDER + status/artifact agreement (shared PC/Wii -- see watchdog.h) ----------
enum wd_sd_status wd_emit_sequence(const struct wd_emit_ops* ops, bool fatal, size_t n) {
	if(!ops || !ops->status || !ops->write_primary || !ops->write_fallback)
		return WD_SD_NONE;
	// PUBLISH BEFORE PAINTING: the row the first paint puts on screen must already read
	// "attempt in flight", because that is the only frame a writer which never returns
	// will ever leave behind.
	ops->status(ops->ctx, WD_SD_TRYING);
	if(fatal && ops->paint)
		ops->paint(ops->ctx);
	enum wd_sd_status status;
	enum wd_write_result primary = ops->write_primary(ops->ctx, n);
	if(primary == WD_WRITE_OK) {
		status = WD_SD_PRIMARY; // fallback deliberately NOT attempted: one write is the
								// cheapest path off a wedged console
	} else {
		enum wd_write_result fallback = ops->write_fallback(ops->ctx, n);
		if(fallback == WD_WRITE_OK)
			status = WD_SD_FALLBACK;
		else if(primary == WD_WRITE_PARTIAL || fallback == WD_WRITE_PARTIAL)
			status = WD_SD_PARTIAL; // a trailer-less remnant survives -- say so, rather
									// than claim FAILED next to a file that exists
		else
			status = WD_SD_FAILED;  // nothing on the card at either path
	}
	ops->status(ops->ctx, status);
	if(fatal && ops->repaint)
		ops->repaint(ops->ctx); // the RESOLVED status still has to reach the screen
	return status;
}

/* Server-phase profiler, shared by both platform setters below. Only the
 * server thread calls watchdog_phase_srv, so the accumulation is single-
 * writer; the main thread takes each slot with an atomic exchange. */
static uint32_t s_wds_us[WDS_MAX];
static ptime_t s_wds_last;
static uint32_t s_wds_prev;
static bool s_wds_started;

_Static_assert(G9WT_PHASES == (unsigned)WDS_MAX,
			   "g9_worst_tick phase arrays must cover every wd_phase_srv");
_Static_assert(SO_PHASES == (unsigned)WDS_MAX, "observer must cover every server phase");
static void wds_account(uint32_t p) {
	/* debug.g9_worst_tick_int (R20 Q3): one relaxed load while off. */
	g9_worst_tick_phase(p);
	ptime_t now = time_get();
	SERVICE_OBSERVER(service_observer_phase(p, service_observer_at(now)));
	if(s_wds_started) {
		float us = time_diff_s(s_wds_last, now) * 1.0e6f;
		if(us > 0 && s_wds_prev < (uint32_t)WDS_MAX)
			__atomic_fetch_add(&s_wds_us[s_wds_prev], (uint32_t)us,
							   __ATOMIC_RELAXED);
	}
	s_wds_started = true;
	s_wds_last = now;
	s_wds_prev = p;
}

void watchdog_srv_phase_take(uint32_t out_us[]) {
	for(unsigned k = 0; k < (unsigned)WDS_MAX; k++)
		out_us[k] = __atomic_exchange_n(&s_wds_us[k], 0u, __ATOMIC_RELAXED);
}

#ifdef PLATFORM_WII
#include "wii/watchdog.c"
#else

#include "watchdog.h"
#include "time.h"

// The phase tags are STORED here, not discarded. There is still no detector
// thread, no SD report and no paint on PC -- one scalar store per setter is the
// whole implementation, and the setters have no other effect -- but storing them
// makes the getters report REAL transitions, so the stamping of the main loop and
// the mesher worker is testable off-device (tests/test_watchdog_phase.c). With the
// no-op getters below, a missing call site (watchdog_phase_worker had ZERO of them
// until 2026-07-27) was unfalsifiable on PC and only surfaced as a permanently-zero
// row on a real-Wii hang photograph. volatile: the mesher worker writes the worker
// phase while another thread reads it, exactly as on Wii.
static volatile uint32_t s_wd_phase_main = WDM_IDLE;
static volatile uint32_t s_wd_phase_srv = WDS_IDLE;
static volatile uint32_t s_wd_phase_worker = WDW_IDLE;
#ifdef CAVEWII_FAR_SLOT_DEADLOCK_TEST
static watchdog_test_srv_phase_observer s_test_srv_phase_observer = NULL;
static void* s_test_srv_phase_observer_user = NULL;

void watchdog_test_set_srv_phase_observer(
	watchdog_test_srv_phase_observer observer, void* user) {
	s_test_srv_phase_observer = observer;
	s_test_srv_phase_observer_user = user;
}
#endif

bool watchdog_init(void) { return true; }
// C1 (candidate, hardware-unconfirmed): no-op -- PC has no detector thread to
// stop (see watchdog_init above). Kept as a real call so main.c's shutdown
// sequence needs no PLATFORM_WII guard around it.
void watchdog_shutdown(void) {}
void watchdog_install_fault_handler(void) {}
void watchdog_heartbeat(void) {}
void watchdog_expect_stall_ms(uint32_t ms) { (void)ms; }
void watchdog_phase_main(enum wd_phase_main p) {
	s_wd_phase_main = (uint32_t)p;
	thread_stripe_phase(THREAD_STRIPE_MAIN, (uint32_t)p);
}
void watchdog_phase_srv(enum wd_phase_srv p) {
	wds_account((uint32_t)p);
	s_wd_phase_srv = (uint32_t)p;
	thread_stripe_phase(THREAD_STRIPE_SERVER, (uint32_t)p);
#ifdef CAVEWII_FAR_SLOT_DEADLOCK_TEST
	if(s_test_srv_phase_observer)
		s_test_srv_phase_observer(p, s_test_srv_phase_observer_user);
#endif
}
void watchdog_phase_worker(enum wd_phase_worker p) {
	s_wd_phase_worker = (uint32_t)p;
	thread_stripe_phase(THREAD_STRIPE_MESHER, (uint32_t)p);
}
uint32_t watchdog_phase_main_current(void) { return s_wd_phase_main; }
uint32_t watchdog_phase_srv_current(void) { return s_wd_phase_srv; }
uint32_t watchdog_phase_worker_current(void) { return s_wd_phase_worker; }
uint32_t watchdog_loop_seq(void) { return 0; }
void watchdog_main_census_phase(uint32_t phase) { (void)phase; }
uint32_t watchdog_main_census_current(void) { return 0; }
uint32_t watchdog_heartbeat_gap_max_ms(void) { return 0; }
void watchdog_main_timings_x10(uint32_t bfs, uint32_t light, uint32_t lod,
							   uint32_t build, uint32_t bfs_max,
							   uint32_t light_max, uint32_t lod_max,
							   uint32_t build_max) {
	(void)bfs;
	(void)light;
	(void)lod;
	(void)build;
	(void)bfs_max;
	(void)light_max;
	(void)lod_max;
	(void)build_max;
}
void watchdog_coord(int32_t cx, int32_t cz) {
	(void)cx;
	(void)cz;
}

#endif
