/*
	Regression guard for the watchdog PHASE instrument -- and for the hang-report
	EMIT CONTRACT (assembly trailer + paint/SD ordering + status-vs-artifact agreement).

	The 2026-07-27 real-Wii list-walk hang was diagnosed ONLY from a photograph of
	the frozen on-screen cell bars, and two of those rows LIED:

	  - watchdog_phase_worker() had ZERO call sites tree-wide, so the mesher row read
	    0 forever. A reader decodes 0 as WDW_IDLE ("the mesher was idle"), when the
	    truth was "this instrument is not wired up" -- it actively misdirected the
	    investigation.
	  - WDM_SCREEN_UPDATE existed in the enum but was never stamped, so the phase-6
	    (WDM_BUILD_CHUNKS) tag silently spanned world_build_chunks AND the entire
	    screen update -- two unrelated suspects under one number.

	Neither defect was catchable off-device, because the PC watchdog branch DISCARDED
	every phase and its getters returned a constant. That branch now stores them, so
	the instrument is testable here.

	WHAT THIS ASSERTS:
	  (1) STORAGE -- the PC branch round-trips every value of all three phase enums.
	      This is what makes any transition observable off-device at all.
	  (2) CALL SITES -- every enumerator of wd_phase_main and wd_phase_worker is
	      actually stamped somewhere under source/. This is the exact defect class
	      above: an enum value nobody writes is a permanently-zero row that reads as
	      real data. Scanned from the tree so a NEW phase cannot be added without one.
	  (3) DECODING -- WDM_CLIENT_TICK, WDM_BUILD_CHUNKS and WDM_SCREEN_UPDATE are
	      three DISTINCT in-range values, so the client-tick, chunk-build and
	      screen-update spans of the main loop decode to three different cell counts
	      on a hang photograph rather than collapsing into one.
	  (4) THE MESHER, LIVE -- the REAL chunk_mesher worker thread (spawned by
	      chunk_mesher_init, fed through the production chunk_mesher_send on a real
	      world chunk) reports WDW_RECV while parked on its request channel, WDW_BUILD
	      while a build runs, and WDW_RECV again once the queue drains. No hand copy of
	      the loop: a stamp deleted from chunk_mesher.c fails this.

	  (5) REPORT ASSEMBLY -- wd_dump_appendf clamps to the body limit and wd_dump_seal
	      ALWAYS appends the END trailer out of a reserved tail, so a whole report is
	      self-identifying and a short write provably is not. Without the reservation an
	      overlong dump would lose its trailer and a COMPLETE write would read as
	      truncated -- the artifact would contradict the status row.
	  (6) EMIT ORDER + STATUS/ARTIFACT AGREEMENT -- wd_emit_sequence drives fakes, so the
	      rules that only ever fire on a wedged console are checked here: the banner and
	      cell bars are painted BEFORE the first SD call (the observed hardware failure is
	      a wedged fopen -- libfat allocating from a smashed heap and never returning --
	      and a write-first order turns that into a plain black freeze, destroying the one
	      instrument that produced the 2026-07-27 evidence), the diag is RE-painted after
	      the outcome so the resolved status reaches the screen, and every outcome
	      (complete / fell back / truncated remnant / nothing written / never returned)
	      maps to a distinct status that matches what is actually on the card.

	  (7) BOOT/BUILD IDENTITY -- every report is stamped with the build id, a per-boot
	      nonce, the uptime and a per-boot sequence, so a COMPLETE report left on the card
	      by an EARLIER boot can be told from this one's. Without it "no file was written
	      this boot" (WD_WRITE_NONE / FAILED / FALLBACK) and a whole report sitting at
	      sd:/hang_report.txt are two facts a reader cannot reconcile.
	  (8) SINKS THAT SURVIVE TO HARDWARE -- world_ilist_overrun_count() and the far-slot
	      starvation run must reach the F3 overlay and the watchdog report (text + cell
	      bars). This is (2)'s defect class again: a counter whose only reader is
	      fprintf(stderr) is write-only on the shipping .dol, because SYS_STDIO_Report is
	      #ifndef NDEBUG-gated and the Wii Makefile compiles -DNDEBUG.
	  (9) PAINT BEFORE STDERR -- the ~2KB stderr dump in wd_emit_dump sits outside the
	      sequencer (6) drives, and must not precede it; fprintf can block in newlib/
	      devoptab from exception context, and yields nothing on the shipping build.
	      Asserted on the source text, since wii/watchdog.c cannot be linked on PC.
	 (10) FALLBACK REPORTS ARE COLLECTED -- the app-dir pair the watchdog writes when the
	      sd:/ root open fails is named by the crash-mailbox replay, the stage-server
	      manifest and the staged hardware runner. A fallback nobody fetches is not one.

	NOT COVERED: main()'s loop is a single non-callable function, so the ORDER of its
	stamps is not exercised here -- (2) proves each tag is written and (3) proves they
	decode apart. wd_phase_srv is deliberately left out of (2): WDS_SPAWN has no call
	site today (server_local.c), so scanning it would assert a known, unrelated gap.
	The Wii-only halves that (6) stands in for -- wd_paint_diag's XFB writes and
	wd_write_report's libfat calls -- are unreachable off-device; what is shared, and
	therefore checked here, is the sequencing and the status they feed.
*/

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "block/blocks.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "network/client_interface.h"
#include "platform/hw_telemetry.h" // the report paths every collector has to agree on
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/watchdog.h"
#include "world.h"

// Generous: the worker only has to be SAMPLED once in each state, and every wait
// below is on a state the worker reaches unconditionally. The deadline exists so a
// deleted stamp FAILS instead of wedging CI.
#define PHASE_TIMEOUT_MS 5000
#define YF 64 // stone-floor plane, well inside chunk cy=4

// --- (1) storage ------------------------------------------------------------
static void check_phase_roundtrip(void) {
	for(int p = WDM_IDLE; p < WDM_MAX; p++) {
		watchdog_phase_main((enum wd_phase_main)p);
		assert(watchdog_phase_main_current() == (uint32_t)p
			   && "PC watchdog dropped a main phase");
	}
	for(int p = WDS_IDLE; p < WDS_MAX; p++) {
		watchdog_phase_srv((enum wd_phase_srv)p);
		assert(watchdog_phase_srv_current() == (uint32_t)p
			   && "PC watchdog dropped a server phase");
	}
	for(int p = WDW_IDLE; p < WDW_MAX; p++) {
		watchdog_phase_worker((enum wd_phase_worker)p);
		assert(watchdog_phase_worker_current() == (uint32_t)p
			   && "PC watchdog dropped a worker phase");
	}
	// Setting one family must not disturb another (the hang report reads all three
	// from one frozen snapshot).
	watchdog_phase_main(WDM_LIGHTING);
	watchdog_phase_srv(WDS_WORLD_TICK);
	watchdog_phase_worker(WDW_BUILD);
	assert(watchdog_phase_main_current() == WDM_LIGHTING);
	assert(watchdog_phase_srv_current() == WDS_WORLD_TICK);
	assert(watchdog_phase_worker_current() == WDW_BUILD);
	printf("  [1] PC watchdog stores + reports every phase of all three enums\n");
	// Leave the worker tag where the mesher check below expects a clean start.
	watchdog_phase_worker(WDW_IDLE);
	watchdog_phase_main(WDM_IDLE);
	watchdog_phase_srv(WDS_IDLE);
}

// --- (3) decoding -----------------------------------------------------------
static void check_main_tags_distinct(void) {
	assert(WDM_CLIENT_TICK != WDM_BUILD_CHUNKS);
	assert(WDM_BUILD_CHUNKS != WDM_SCREEN_UPDATE);
	assert(WDM_CLIENT_TICK != WDM_SCREEN_UPDATE);
	// In range of the cell-bar cap (wd_paint_diag paints value capped at WDM_MAX)
	// and of the report's name table, else the photo/report decodes to "?".
	assert(WDM_CLIENT_TICK < WDM_MAX && WDM_BUILD_CHUNKS < WDM_MAX
		   && WDM_SCREEN_UPDATE < WDM_MAX);
	printf("  [3] CLIENT_TICK=%d BUILD_CHUNKS=%d SCREEN_UPDATE=%d: distinct, "
		   "in range (< %d)\n",
		   (int)WDM_CLIENT_TICK, (int)WDM_BUILD_CHUNKS, (int)WDM_SCREEN_UPDATE,
		   (int)WDM_MAX);
}

// --- (5) report assembly: the END trailer is what makes a file self-identifying ------
static void check_dump_seal(void) {
	const size_t tlen = strlen(WD_DUMP_TRAILER);
	const char* buf = wd_dump_buf();

	// Ordinary report: sealed, NUL-terminated, inside the buffer.
	wd_dump_reset();
	size_t n = wd_dump_appendf(0, "CAVEWII PANIC: %s main=%s\n", "HANG", "BUILD_CHUNKS");
	size_t body = n;
	n = wd_dump_seal(n);
	assert(n == body + tlen && "seal must append exactly the trailer");
	assert(n < WD_DUMP_CAP && buf[n] == '\0');
	assert(wd_dump_is_sealed(buf, n) && "a sealed report must end with the trailer");
	// A SHORT WRITE -- the truncated-file case wd_write_report retries and then reports
	// as PARTIAL. One byte missing is enough to make the artifact identify itself.
	assert(!wd_dump_is_sealed(buf, n - 1));
	assert(!wd_dump_is_sealed(buf, body) && "an unsealed body must not read as whole");
	assert(!wd_dump_is_sealed(buf, 0));

	// OVERLONG dump: the body clamps at the reserved tail and the trailer still lands,
	// so a complete write of a report that overflowed the buffer is NOT mistaken for a
	// truncated file. Sealing through the ordinary append path would drop the trailer
	// here and put the card and the status row into direct contradiction.
	wd_dump_reset();
	n = 0;
	for(int i = 0; i < 200; i++)
		n = wd_dump_appendf(n, "%03d 0123456789abcdef0123456789abcdef\n", i);
	assert(n == WD_DUMP_BODY_MAX - 1 && "body must clamp at the reserved tail");
	n = wd_dump_seal(n);
	assert(n == WD_DUMP_BODY_MAX - 1 + tlen);
	assert(n < WD_DUMP_CAP && buf[n] == '\0' && "seal ran past the buffer");
	assert(wd_dump_is_sealed(buf, n)
		   && "an overlong report lost its trailer -- a complete write would read as "
			  "truncated");
	printf("  [5] dump seals with %u-byte END trailer; short + overlong cases agree\n",
		   (unsigned)tlen);
}

// --- (6) emit ORDER + status/artifact agreement --------------------------------------
// Fakes for wd_emit_sequence's platform hooks. Every call is appended to an event log
// with the status the screen would be showing at that moment, so the ORDER -- not just
// the outcome -- is assertable. This is the only way to exercise the wedge case at all:
// on hardware "the SD call never returns" cannot be provoked on demand, so a writer that
// longjmps out models it exactly.
enum fake_ev { EV_STATUS = 1, EV_PAINT, EV_REPAINT, EV_WRITE_PRIMARY, EV_WRITE_FALLBACK };
struct fake_emit {
	int ev[16];                     // event kinds, in call order
	enum wd_sd_status shown[16];    // status the row carried when the event happened
	size_t bytes[16];               // n handed to each writer
	int count;
	enum wd_write_result primary, fallback;
	int wedge_ev;                   // event kind whose callback never returns (0 = none)
	jmp_buf wedge_out;
	enum wd_sd_status live;         // last status published
};
static struct fake_emit g_fk;

static void fk_push(int ev, size_t bytes) {
	assert(g_fk.count < (int)(sizeof(g_fk.ev) / sizeof(g_fk.ev[0])));
	g_fk.ev[g_fk.count] = ev;
	g_fk.shown[g_fk.count] = g_fk.live;
	g_fk.bytes[g_fk.count] = bytes;
	g_fk.count++;
	if(g_fk.wedge_ev == ev)
		longjmp(g_fk.wedge_out, 1); // "never returned"
}
static void fk_status(void* ctx, enum wd_sd_status s) {
	(void)ctx;
	g_fk.live = s; // published BEFORE the log entry: the row already reads s
	fk_push(EV_STATUS, 0);
}
static void fk_paint(void* ctx) {
	(void)ctx;
	fk_push(EV_PAINT, 0);
}
static void fk_repaint(void* ctx) {
	(void)ctx;
	fk_push(EV_REPAINT, 0);
}
static enum wd_write_result fk_write_primary(void* ctx, size_t n) {
	(void)ctx;
	fk_push(EV_WRITE_PRIMARY, n);
	return g_fk.primary;
}
static enum wd_write_result fk_write_fallback(void* ctx, size_t n) {
	(void)ctx;
	fk_push(EV_WRITE_FALLBACK, n);
	return g_fk.fallback;
}
static const struct wd_emit_ops FK_OPS = {
	.ctx = NULL,
	.status = fk_status,
	.paint = fk_paint,
	.repaint = fk_repaint,
	.write_primary = fk_write_primary,
	.write_fallback = fk_write_fallback,
};

static void fk_reset(enum wd_write_result primary, enum wd_write_result fallback) {
	memset(&g_fk, 0, sizeof(g_fk));
	g_fk.primary = primary;
	g_fk.fallback = fallback;
	g_fk.live = WD_SD_NONE;
}
// Index of the first event of kind `ev`, or -1.
static int fk_first(int ev) {
	for(int i = 0; i < g_fk.count; i++)
		if(g_fk.ev[i] == ev)
			return i;
	return -1;
}
static int fk_count_of(int ev) {
	int c = 0;
	for(int i = 0; i < g_fk.count; i++)
		if(g_fk.ev[i] == ev)
			c++;
	return c;
}

#define FK_N 1234u // stand-in sealed length; the writers must be handed exactly this

static void check_emit_order(void) {
	// THE ORDERING CONTRACT. A paint must precede the first SD call, and a repaint must
	// follow the final status, or a photograph shows either nothing at all (wedged
	// writer) or a stale row.
	fk_reset(WD_WRITE_OK, WD_WRITE_NONE);
	assert(wd_emit_sequence(&FK_OPS, true, FK_N) == WD_SD_PRIMARY);
	int paint = fk_first(EV_PAINT), write = fk_first(EV_WRITE_PRIMARY),
		repaint = fk_first(EV_REPAINT);
	assert(paint >= 0 && write >= 0 && repaint >= 0);
	assert(paint < write
		   && "the panic banner + cell bars must be painted BEFORE the SD write: a "
			  "wedged fopen would otherwise leave a console indistinguishable from a "
			  "plain freeze");
	assert(repaint > write && "the resolved SD status never reached the screen");
	assert(g_fk.shown[paint] == WD_SD_TRYING
		   && "the pre-write paint must already show the in-flight status");
	assert(g_fk.shown[repaint] == WD_SD_PRIMARY
		   && "the repaint must show the RESOLVED status");
	assert(g_fk.ev[g_fk.count - 1] == EV_REPAINT && "repaint must be the last step");
	assert(g_fk.bytes[write] == FK_N && "writer got a different length than the caller");
	// A complete primary write must not touch the fallback path (an extra fopen on a
	// wedge-prone card buys nothing once the report is already down).
	assert(fk_first(EV_WRITE_FALLBACK) < 0);
	printf("  [6] paint -> write -> status -> repaint; pre-write row reads TRYING\n");

	// WEDGED WRITER: the call never returns. Everything the reader will ever get must
	// already be on screen, and the row must read TRYING -- "in flight", which no
	// resolved outcome uses -- so a photograph is never read as "no incident".
	fk_reset(WD_WRITE_OK, WD_WRITE_OK);
	g_fk.wedge_ev = EV_WRITE_PRIMARY;
	if(setjmp(g_fk.wedge_out) == 0)
		(void)wd_emit_sequence(&FK_OPS, true, FK_N);
	assert(fk_first(EV_PAINT) >= 0
		   && "a writer that never returns left NOTHING on screen");
	assert(fk_first(EV_PAINT) < fk_first(EV_WRITE_PRIMARY));
	assert(g_fk.live == WD_SD_TRYING
		   && "a wedged write must leave the in-flight status, not a resolved one");
	printf("  [6] a writer that never returns still leaves banner + bars, row = TRYING\n");

	// NON-FATAL (FBGUARD): no banner over a live game, but the write still happens and
	// the status is still published for the next report.
	fk_reset(WD_WRITE_OK, WD_WRITE_NONE);
	assert(wd_emit_sequence(&FK_OPS, false, FK_N) == WD_SD_PRIMARY);
	assert(fk_count_of(EV_PAINT) == 0 && fk_count_of(EV_REPAINT) == 0
		   && "FBGUARD must not paint a panic banner over a running game");
	assert(fk_first(EV_WRITE_PRIMARY) >= 0);
	printf("  [6] FBGUARD writes its report without painting\n");
}

// Every write outcome maps to a status that describes what is ACTUALLY on the card.
static void check_emit_status_matches_card(void) {
	struct {
		enum wd_write_result primary, fallback;
		enum wd_sd_status want;
		const char* card;
	} cases[] = {
		{WD_WRITE_OK, WD_WRITE_NONE, WD_SD_PRIMARY, "whole report at the primary path"},
		{WD_WRITE_NONE, WD_WRITE_OK, WD_SD_FALLBACK, "whole report at the fallback"},
		// A truncated remnant at the primary must NOT downgrade a good fallback: the
		// remnant lacks the trailer, so the complete file is the one to name.
		{WD_WRITE_PARTIAL, WD_WRITE_OK, WD_SD_FALLBACK, "whole report at the fallback"},
		// ...but with no complete file anywhere, a surviving remnant must be announced.
		// Reporting FAILED here is the defect: a reader who then FINDS a file on the
		// card would read a truncated report as the whole story.
		{WD_WRITE_PARTIAL, WD_WRITE_NONE, WD_SD_PARTIAL, "an INCOMPLETE file only"},
		{WD_WRITE_NONE, WD_WRITE_PARTIAL, WD_SD_PARTIAL, "an INCOMPLETE file only"},
		{WD_WRITE_PARTIAL, WD_WRITE_PARTIAL, WD_SD_PARTIAL, "an INCOMPLETE file only"},
		{WD_WRITE_NONE, WD_WRITE_NONE, WD_SD_FAILED, "nothing at all"},
	};
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fk_reset(cases[i].primary, cases[i].fallback);
		enum wd_sd_status got = wd_emit_sequence(&FK_OPS, true, FK_N);
		assert(got == cases[i].want && "status row does not match what is on the card");
		assert(g_fk.live == got && "the painted row and the return value disagree");
		// Anything short of a complete primary must have TRIED the fallback -- the
		// fallback exists because a root-directory open can fail on its own.
		assert((cases[i].primary == WD_WRITE_OK)
			   == (fk_first(EV_WRITE_FALLBACK) < 0));
		assert(g_fk.bytes[fk_first(EV_WRITE_PRIMARY)] == FK_N);
	}
	// The two states that mean "no complete report exists" are the two the paint draws
	// in red, and they are the top of the enum -- wd_paint_diag tests `>= WD_SD_PARTIAL`.
	assert(WD_SD_PARTIAL < WD_SD_FAILED && WD_SD_FAILED < WD_SD_MAX);
	assert(WD_SD_TRYING < WD_SD_PRIMARY && WD_SD_PRIMARY < WD_SD_FALLBACK
		   && WD_SD_FALLBACK < WD_SD_PARTIAL);
	printf("  [6] all 7 write outcomes map to a status that matches the card\n");
}

// --- (2) call sites ---------------------------------------------------------
// Recursive source scan. dirent's d_type is not portable (the same reason
// screen_select_world.c uses stat + S_ISDIR), so directories are identified by stat.
struct needle {
	const char* text; // the exact call the stamp site must contain
	bool found;
};

static void scan_file(const char* path, struct needle* needles, size_t count) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return;
	if(fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	long size = ftell(f);
	if(size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return;
	}
	char* buf = malloc((size_t)size + 1);
	if(!buf) {
		fclose(f);
		return;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	for(size_t k = 0; k < count; k++)
		if(!needles[k].found && strstr(buf, needles[k].text))
			needles[k].found = true;
	free(buf);
}

static void scan_dir(const char* dir, struct needle* needles, size_t count) {
	DIR* d = opendir(dir);
	if(!d)
		return;
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct stat st;
		if(stat(path, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
			scan_dir(path, needles, count);
		else {
			size_t len = strlen(e->d_name);
			if(len > 2 && !strcmp(e->d_name + len - 2, ".c"))
				scan_file(path, needles, count);
		}
	}
	closedir(d);
}

static void check_every_phase_is_stamped(void) {
	// IDLE is the initial value of each tag, not a site; _MAX is the bound. Every
	// other enumerator MUST have a writer, or its row is permanently zero and reads
	// as data. Keep in step with enum wd_phase_main / wd_phase_worker.
	struct needle needles[] = {
		{"watchdog_phase_main(WDM_CLIN_UPDATE)", false},
		{"watchdog_phase_main(WDM_CLIENT_TICK)", false},
		{"watchdog_phase_main(WDM_PRERENDER)", false},
		{"watchdog_phase_main(WDM_LIGHTING)", false},
		{"watchdog_phase_main(WDM_LOD)", false},
		{"watchdog_phase_main(WDM_BUILD_CHUNKS)", false},
		{"watchdog_phase_main(WDM_SCREEN_UPDATE)", false},
		{"watchdog_phase_main(WDM_GFX_FLIP)", false},
		{"watchdog_phase_main(WDM_MESH_RECV)", false},
		{"watchdog_phase_main(WDM_RENDER)", false},
		{"watchdog_phase_main(WDM_GFX_FINISH)", false},
		{"watchdog_phase_main(WDM_GFX_DRAWDONE)", false},
		{"watchdog_phase_main(WDM_GFX_FBPUSH)", false},
		{"watchdog_phase_main(WDM_GFX_SEMWAIT)", false},
		// Batch E item 1 (2026-08-23): shutdown-phase coverage. main.c's
		// post-loop teardown (server_local_shutdown -> audio_shutdown ->
		// hw_telemetry_shutdown) previously ran with the phase tag frozen
		// at whatever the last loop iteration left it (WDM_GFX_FINISH),
		// painting a stale hang label on a merely-slow shutdown.
		{"watchdog_phase_main(WDM_SHUTDOWN_SERVER)", false},
		{"watchdog_phase_main(WDM_SHUTDOWN_AUDIO)", false},
		{"watchdog_phase_main(WDM_SHUTDOWN_TELEMETRY)", false},
		// Exit-wedge candidate (2026-08-24): WPAD/BT teardown before the
		// loader return -- see main.c WDM_SHUTDOWN_WPAD site.
		{"watchdog_phase_main(WDM_SHUTDOWN_WPAD)", false},
		// C1 (candidate, hardware-unconfirmed, 2026-09-02): watchdog_shutdown()
		// is now the LAST shutdown step, tagged so a stall inside its own join
		// reports honestly instead of inheriting WDM_SHUTDOWN_WPAD's label --
		// see main.c WDM_SHUTDOWN_WATCHDOG site.
		{"watchdog_phase_main(WDM_SHUTDOWN_WATCHDOG)", false},
		// T-b (2026-09-03, Round 3 (3) instrumentation-gap fix): stamped right
		// before the emit_hw_heartbeat call site -- see main.c and this
		// enumerator's doc comment in watchdog.h.
		{"watchdog_phase_main(WDM_HEARTBEAT)", false},
		// 2026-09-11: the texture-pack reload is stamped WDM_PACK_RELOAD around
		// texturepacks_apply (screen_texturepacks.c) with a declared watchdog
		// grace -- a hang report inside it names the operation, and a
		// legitimate 3-5 s reload no longer paints the panic banner.
		{"watchdog_phase_main(WDM_PACK_RELOAD)", false},
		{"watchdog_phase_worker(WDW_RECV)", false},
		{"watchdog_phase_worker(WDW_BUILD)", false},
		{"watchdog_phase_worker(WDW_SEND)", false},
	};
	const size_t count = sizeof(needles) / sizeof(needles[0]);
	// WDM_IDLE + the 16 stamped main values == WDM_MAX; same for the worker. A new
	// enumerator without a needle would slip through the scan, so pin the counts.
	assert(count == (size_t)(WDM_MAX - 1) + (size_t)(WDW_MAX - 1)
		   && "a phase enumerator was added without a call-site needle");

	// WORKING_DIRECTORY is the source root (see CMakeLists) -- same convention as
	// test_audio_env's asset load.
	scan_dir("source", needles, count);
	int missing = 0;
	for(size_t k = 0; k < count; k++)
		if(!needles[k].found) {
			fprintf(stderr,
					"test_watchdog_phase: NO call site for %s -- its watchdog row "
					"is permanently 0 and will read as real data on a hang report\n",
					needles[k].text);
			missing++;
		}
	assert(missing == 0 && "a watchdog phase has no call site");
	printf("  [2] all %d main/worker phase tags have a call site under source/\n",
		   (int)count);
}

// --- (7) boot/build IDENTITY on every report ---------------------------------
// A stale, COMPLETE report from an EARLIER boot sitting at sd:/hang_report.txt used to be
// byte-for-byte as convincing as one this boot wrote -- and WD_WRITE_NONE / FAILED /
// FALLBACK all mean "nothing was written THIS boot", so the status row and the card
// contradicted each other with no way to reconcile them. Three things have to differ
// across boots for a reader to tell them apart, and the per-boot sequence has to advance.
static void check_dump_identity(void) {
	const char* buf = wd_dump_buf();

	uint32_t seq0 = wd_dump_report_seq();
	wd_dump_reset();
	size_t n = wd_dump_identity(0, "abc1234_07.29", 0xDEADBEEFu, 12345u);
	assert(n > 0 && n < WD_DUMP_BODY_MAX);
	assert(wd_dump_report_seq() == seq0 + 1 && "the report sequence did not advance");
	// Each field has to SURVIVE into the text: a stamp that drops the build id cannot
	// tell this .dol's report from the previous one's.
	assert(strstr(buf, "abc1234_07.29") && "the build id is missing from the report");
	assert(strstr(buf, "deadbeef") && "the boot nonce is missing from the report");
	assert(strstr(buf, "up=12345ms") && "the uptime is missing from the report");
	char want_seq[32];
	snprintf(want_seq, sizeof(want_seq), "seq=%u", (unsigned)(seq0 + 1));
	assert(strstr(buf, want_seq) && "the per-boot report sequence is missing");
	// One line, so it costs a truncated report almost nothing to carry it.
	assert(strchr(buf, '\n') == buf + n - 1 && "the identity must be exactly one line");

	// THE POINT: two boots of the SAME build produce DIFFERENT text. Without the nonce
	// the two reports are indistinguishable, which is the defect.
	char first[WD_DUMP_CAP];
	memcpy(first, buf, n + 1);
	wd_dump_reset();
	size_t n2 = wd_dump_identity(0, "abc1234_07.29", 0x12345678u, 12345u);
	assert(strcmp(first, buf) != 0
		   && "two different boots stamped an identical identity line");
	assert(n2 > 0);
	// A build with no stamp must still produce a readable line, not a NULL
	// deref. The fallback literal is "no_build_id", deliberately NOT
	// "unstamped" -- that token is build-verify.sh's stale-stamp sentinel and
	// survives into non-LTO dev .dols (see wd_dump_identity's comment).
	wd_dump_reset();
	(void)wd_dump_identity(0, NULL, 1u, 0u);
	assert(strstr(buf, "no_build_id"));

	wd_dump_reset();
	printf("  [7] every report carries build + per-boot nonce + uptime + sequence\n");
}

// --- (8) the counters reach a sink that EXISTS on the shipping build ---------
// The defect class this whole test file was written for: an instrument nobody reads.
// watchdog_phase_worker had zero call sites and its cell-bar row decoded as a meaningful
// 0. A counter whose only sink is fprintf(stderr) repeats it exactly -- main.c calls
// SYS_STDIO_Report(true) only under !NDEBUG and the Wii Makefile compiles -DNDEBUG, so
// the shipping .dol has NO stderr channel at all. The sinks that survive to hardware are
// the F3 overlay and the watchdog's text dump + XFB cell bars, and nothing else.
static char* read_source(const char* path) {
	FILE* f = fopen(path, "rb");
	assert(f && "source file not found -- is the test's WORKING_DIRECTORY the repo root?");
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	char* buf = malloc((size_t)size + 1);
	assert(buf);
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

static void expect_in(const char* text, const char* path, const char* needle,
					  const char* why) {
	if(strstr(text, needle))
		return;
	fprintf(stderr, "test_watchdog_phase: %s does not contain \"%s\" -- %s\n", path,
			needle, why);
	assert(0 && "a diagnostic counter has no sink on the shipping build");
}

#define F3_OVERLAY "source/game/gui/screen_ingame.c"
#define WII_WATCHDOG "source/platform/wii/watchdog.c"

static void check_counters_reach_a_shipping_sink(void) {
	char* f3 = read_source(F3_OVERLAY);
	char* wd = read_source(WII_WATCHDOG);

	// world_ilist_overrun_count(): the corrupt-chunk-chain guard DELIBERATELY leaks that
	// frame's chunk references on a bail and justifies it by "a rising count is visible",
	// which is only true while something reads the count.
	expect_in(f3, F3_OVERLAY, "world_ilist_overrun_count()",
			  "the chunk-list walk over-run counter is invisible in the running game");
	expect_in(wd, WII_WATCHDOG, "world_ilist_overrun_count()",
			  "the chunk-list walk over-run counter is absent from the hang report");
	// ...in BOTH watchdog sinks: the text dump for a card that can be read, and the XFB
	// cell bars for a bare frozen console where the photograph is the only evidence.
	expect_in(wd, WII_WATCHDOG, "ilist=%u",
			  "the hang report's counter line does not carry the walk over-run count");
	expect_in(wd, WII_WATCHDOG, "wd_bar_alert(fb, w2, fbh, y, (unsigned)world_ilist_overrun_count()",
			  "the walk over-run count has no cell-bar row on the panic paint");

	// The far-slot pool's starvation run: a non-blocking bail is silent by construction,
	// so a slot the client never releases stops far streaming with no symptom at all.
	expect_in(f3, F3_OVERLAY, "server_local_far_slot_starve()",
			  "the far-slot starvation run is invisible in the running game");
	expect_in(wd, WII_WATCHDOG, "server_local_far_slot_starve()",
			  "the far-slot starvation run is absent from the hang report");
	expect_in(wd, WII_WATCHDOG, "farbail=%u",
			  "the hang report's counter line does not carry the far-slot bail run");

	// POLARITY. The counter line mixes fault dials (0 = healthy) with throughput dials
	// (non-zero expected), and the 2026-07-29 report's stale=245 / nevdef=10 were both read
	// as symptoms when both were healthy. A counter whose polarity is undocumented at its
	// only sink is as misleading as one nobody reads at all -- which is what this whole
	// check exists to prevent -- so the legend is pinned like any other sink.
	expect_in(wd, WII_WATCHDOG, "THROUGHPUT dials",
			  "the hang report's mixed-polarity counter line has no legend, so a reader "
			  "cannot tell stale/nevdef from the fault dials printed beside them");
	expect_in(wd, WII_WATCHDOG, "EDGE counter",
			  "nevdef FREEZES rather than climbing under a chronic client-inbox stall (the "
			  "eviction loop only runs once the queue drained, and breaks on the first "
			  "deferral), and nothing at its only sink says so -- a flat nevdef reads as "
			  "healthy when it is the opposite");

	const char* mesher = "source/chunk_mesher.c";
	char* ms = read_source(mesher);
	expect_in(ms, mesher, "THROUGHPUT dial",
			  "mesh_stale_discards has no polarity note at its definition -- a reader of "
			  "the report's stale= cannot tell it from the fault dials beside it");
	free(ms);

	free(wd);
	free(f3);
	printf("  [8] ilist over-run + far-slot starvation reach F3 and the watchdog report; "
		   "counter POLARITY is documented at both sinks\n");
}

// --- (9) the panic PAINT is sequenced ahead of everything that can block ------
// wd_emit_sequence's own ordering is pinned by (6) with fakes, but the stderr dump in
// wd_emit_dump sits OUTSIDE the sequencer, and it used to run first. fprintf takes
// newlib's FILE lock and enters libogc's devoptab write -- from EXCEPTION context, since
// wii/panic.c calls the emit from its fault handler -- so it is exactly the kind of call
// the paint exists to survive; and on the shipping -DNDEBUG build it yields nothing at
// all, because there is no stderr channel. wii/watchdog.c is Wii-only and cannot be
// linked here, so the order is asserted where it lives, in the source text.
static void check_paint_precedes_stderr(void) {
	char* wd = read_source(WII_WATCHDOG);

	const char* emit = strstr(wd, "wd_emit_sequence(&ops");
	const char* dump = strstr(wd, "==== CAVEWII WATCHDOG DUMP");
	assert(emit && "wd_emit_dump no longer drives wd_emit_sequence");
	assert(dump && "the survive-the-frozen-SD stderr dump is gone");
	assert(emit < dump
		   && "the ~2KB stderr dump is sequenced BEFORE wd_emit_sequence, so a wedge in "
			  "newlib/devoptab costs the panic banner AND the cell bars -- the only "
			  "instrument a shipping -DNDEBUG .dol has, since it routes stderr nowhere");

	// The stale-report retirement (R4's other half): a failed write must not leave an
	// EARLIER boot's complete report sitting at the path the status row calls empty.
	const char* nocreate = strstr(wd, "if(!created) {");
	assert(nocreate && "wd_write_report's never-created branch is gone");
	const char* retire = strstr(nocreate, "remove(path);");
	const char* classify = strstr(nocreate, "wd_write_cleanup_result");
	assert(classify && retire && retire < classify
		   && "a boot that could not write must first try to retire the stale report it "
			  "is about to claim is not there");
	assert(wd_write_cleanup_result(0, EACCES) == WD_WRITE_NONE);
	assert(wd_write_cleanup_result(-1, ENOENT) == WD_WRITE_NONE);
	assert(wd_write_cleanup_result(-1, EACCES) == WD_WRITE_PARTIAL
		   && "a refused stale-file removal must not claim the report path is empty");

	free(wd);
	printf("  [9] wd_emit_dump paints + writes SD before it touches stderr\n");
}

// --- (9b) the `up=` field is an UPTIME, not a wall clock ---------------------------
// The 2026-07-28 hang report read up=1149429345ms -- 13 days -- on a sub-hour session,
// because wii/watchdog.c passed ticks_to_millisecs(gettime()) straight to wd_dump_identity
// on the assumption that the PowerPC timebase is zero at power-on. libogc seeds it from the
// RTC (SYS_PreMain -> __SYS_SetBootTime), and its own gettod does ticks_to_secs(gettime()) +
// 946684800, so gettime()/60750000 is seconds since 2000-01-01. Decoding the reported value
// confirms it exactly: 838,598,400 s past 2000-01-01 is 2026-07-29, and 838,598,400,000 ms
// mod 2^32 = 1,079,777,280, which is the reported number minus 69,652,065 ms = 19h20m -- an
// ordinary evening on the reported play date.
//
// The field is now a delta through wd_uptime_ticks. This pins the two ways that delta can
// still lie, neither of which is reachable on hardware to test.
static void check_uptime_is_a_delta_not_a_wall_clock(void) {
	// Wii timebase rate, for readable fixtures: PPC_TIMER_CLOCK = 243000000/4 = 60,750,000
	// ticks/s, so ticks_to_millisecs is ticks/60750.
	const uint64_t TICKS_PER_MS = 60750;
	const uint64_t base = 1000000000ull; // an arbitrary mid-wall-clock base

	assert(wd_uptime_ticks(base, base) == 0 && "no time has passed -> 0");
	assert(wd_uptime_ticks(base, base + 60750 * 1234) == 60750ull * 1234
		   && "an exact tick delta must survive unscaled");
	assert(wd_uptime_ticks(base, base + 60750 * 1234) / TICKS_PER_MS == 1234
		   && "1234 ms of ticks must read as 1234 ms");

	// THE REGRESSION THAT SHIPPED: an unseeded base must not leak the wall clock. Before the
	// fix this position returned the raw `now`, which is what printed 13 days.
	assert(wd_uptime_ticks(0, base) == 0
		   && "base 0 is the not-seeded sentinel (a fault before watchdog_init returned) -- "
			  "it must report up=0, never the raw wall-clock timebase");

	// A non-monotonic sample must refuse rather than underflow: base-1 would otherwise wrap
	// to ~1.8e19 ticks = ~9,700 years, which is the same class of plausible-looking lie.
	assert(wd_uptime_ticks(base, base - 1) == 0 && "now < base must not underflow");
	assert(wd_uptime_ticks(base, 0) == 0 && "a zeroed now must not underflow");

	// The identity line must actually carry the value it is handed. 4051 ms is the real
	// stall the maintainer timed on the save/quit path, so this doubles as a readable
	// fixture for what the field SHOULD have printed.
	const char* buf = wd_dump_buf();
	wd_dump_reset();
	size_t n = wd_dump_identity(0, "testbuild", 0xabcd1234u,
								(uint32_t)(wd_uptime_ticks(base, base + 60750ull * 4051)
										   / TICKS_PER_MS));
	assert(n > 0 && buf);
	assert(strstr(buf, "up=4051ms") && "the identity line dropped the uptime it was given");

	printf("  [9b] up= is a guarded delta (0 when unseeded, never underflows)\n");
}

// --- (9c) source scans: the wrong fixes must stay un-taken -------------------------
// Two of them are tempting and both are worse than the bug.
static void check_uptime_wrong_fixes_stay_untaken(void) {
	char* wd = read_source(WII_WATCHDOG);

	// (i) the original defect: the raw timebase handed straight to the identity line. Scan
	// the actual identity call -- from "wd_dump_identity(" to its terminating ';' -- rather
	// than matching an exact formatting of the old line, which any reindent would defeat.
	const char* id = strstr(wd, "wd_dump_identity(");
	assert(id && "wii/watchdog.c no longer stamps an identity line");
	const char* line_end = strchr(id, ';');
	assert(line_end && "the identity call is unterminated");
	size_t call_len = (size_t)(line_end - id);
	char* call = malloc(call_len + 1);
	assert(call);
	memcpy(call, id, call_len);
	call[call_len] = '\0';
	assert(strstr(call, "wd_uptime_ticks")
		   && "the identity line must go through wd_uptime_ticks, not the raw timebase");
	assert(!strstr(call, "ticks_to_millisecs(gettime())")
		   && "the raw-wall-clock uptime argument is back -- this printed up=13 days");
	free(call);
	free(wd);

	// (ii) DO NOT zero libogc's clock. settime(0) at startup WOULD make gettime() an uptime,
	// which is exactly why it is tempting -- and it breaks time(), the level.dat LastPlayed
	// tag, the strftime column in screen_select_world.c, and makes any in-flight unsigned
	// ptime_t difference underflow. time_reset() is defined in platform/time.c and has never
	// been called; this asserts it still is not.
	// Match the CALL form "time_reset();" rather than the bare identifier: the whole point
	// of this scan is that the wrong fix must stay documented as wrong, and the doc comment
	// beside wd_uptime_ticks necessarily NAMES time_reset()/settime(0). A bare-identifier
	// scan fails on its own warning label -- it did, on the first run of this test.
	const char* callers[] = {"source/main.c", "source/platform/wii/watchdog.c",
							 "source/platform/watchdog.c"};
	for(size_t i = 0; i < sizeof(callers) / sizeof(callers[0]); i++) {
		char* text = read_source(callers[i]);
		assert(!strstr(text, "time_reset();")
			   && "time_reset()/settime(0) is the WRONG fix for the up= epoch -- it zeroes "
				  "libogc's wall clock and breaks time(), LastPlayed and the world-list "
				  "date column");
		assert(!strstr(text, "settime(0);")
			   && "settime(0) zeroes libogc's wall clock -- see wd_uptime_ticks");
		free(text);
	}
	// And the invariant the above depends on: time_reset's ONLY definition still lives in
	// platform/time.c (PC no-op + the Wii settime(0) body). If someone inlines a second copy
	// elsewhere, the call-site scan above stops meaning anything.
	char* timec = read_source("source/platform/time.c");
	assert(strstr(timec, "settime(0);")
		   && "platform/time.c no longer holds the settime(0) body -- has time_reset moved?");
	free(timec);

	printf("  [9c] the raw-timebase and settime(0) fixes both stay un-taken\n");
}

// --- (9d) one hang report per boot must not destroy the previous one ---------------
// wd_write_report opens its path with fopen(path, "w"), which TRUNCATES. With a single
// fixed path, report N clobbered report N-1 and only the LAST stall of a session was ever
// readable. The 2026-07-29 card report read seq=6 with fbguard=intact -- six separate >3s
// main-loop stalls, five of them unrecoverable, which is why that session could only ever
// diagnose one. This pins the path split that fixes it.
static void check_hang_reports_do_not_overwrite_each_other(void) {
	char buf[WD_REPORT_PATH_MAX];
	const char* base = HW_TELEMETRY_HANG_REPORT_PATH;

	// Report 1 keeps the CANONICAL name -- the whole compatibility argument. Every
	// existing collector, and every human who knows sd:/hang_report.txt, keeps working.
	assert(wd_report_path_for_seq(buf, sizeof(buf), base, 0) == base);
	assert(wd_report_path_for_seq(buf, sizeof(buf), base, 1) == base);

	// 2..MAX insert the suffix BEFORE the extension, so the files still open as text.
	const char* p2 = wd_report_path_for_seq(buf, sizeof(buf), base, 2);
	assert(strstr(p2, "_2") && "report 2 did not get a sequence suffix");
	assert(strstr(p2, ".txt") && "the suffix ate the extension");
	assert(strcmp(p2, base) != 0 && "report 2 still lands on report 1's path");

	// THE INVARIANT, stated directly: no two reports of a boot share a path.
	char seen[WD_REPORT_SEQ_MAX + 1][WD_REPORT_PATH_MAX];
	for(uint32_t s = 1; s <= WD_REPORT_SEQ_MAX; s++) {
		const char* p = wd_report_path_for_seq(buf, sizeof(buf), base, s);
		snprintf(seen[s], sizeof(seen[s]), "%s", p);
		for(uint32_t t = 1; t < s; t++)
			assert(strcmp(seen[s], seen[t]) != 0
				   && "two reports of the same boot write to the same path -- the later "
					  "one truncates the earlier");
	}

	// PAST the bound the sequence CLAMPS, so a stall re-firing every 250ms cannot fill the
	// card. Slot MAX therefore holds the most RECENT report; 1..MAX-1 keep the earliest.
	const char* pmax = wd_report_path_for_seq(buf, sizeof(buf), base, WD_REPORT_SEQ_MAX);
	char keep[WD_REPORT_PATH_MAX];
	snprintf(keep, sizeof(keep), "%s", pmax);
	assert(strcmp(wd_report_path_for_seq(buf, sizeof(buf), base, WD_REPORT_SEQ_MAX + 1),
				  keep)
		   == 0);
	assert(strcmp(wd_report_path_for_seq(buf, sizeof(buf), base, 100000u), keep) == 0
		   && "an unbounded sequence would fill the SD card with reports");

	// A path with no extension appends at the end rather than mangling the name.
	const char* noext = wd_report_path_for_seq(buf, sizeof(buf), "reports/hang_report", 3);
	assert(strcmp(noext, "reports/hang_report_3") == 0);
	// ...and a '.' that belongs to a DIRECTORY component is not an extension. Splitting on
	// the last '.' unconditionally would turn "./hang_report" into "._3/hang_report".
	const char* dotdir = wd_report_path_for_seq(buf, sizeof(buf), "./hang_report", 3);
	assert(strcmp(dotdir, "./hang_report_3") == 0);

	// A buffer that cannot hold the result degrades to the BASE path (the historical
	// overwrite) rather than writing a truncated path that names the wrong file.
	char tiny[4];
	assert(wd_report_path_for_seq(tiny, sizeof(tiny), base, 2) == base);
	assert(wd_report_path_for_seq(NULL, 0, base, 2) == base);

	// The real fallback path must sequence too: the watchdog falls back PER REPORT, so a
	// boot whose root open fails repeatedly would otherwise overwrite in the app dir.
	const char* fb = wd_report_path_for_seq(buf, sizeof(buf),
										   HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH, 2);
	assert(strcmp(fb, HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH) != 0);
	assert(sizeof(buf) > strlen(HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH) + 4
		   && "WD_REPORT_PATH_MAX is too small for the longest report path + suffix");

	printf("  [9d] reports 1..%d take %d distinct paths, then clamp\n", WD_REPORT_SEQ_MAX,
		   WD_REPORT_SEQ_MAX);
}

// --- (9e) the sequenced reports are actually COLLECTED -----------------------------
// Same rule as the fallbacks in (10): a report path no collector names is a file nobody
// ever fetches. Five reports were destroyed by the truncation; the fix must not replace
// that with five reports that survive on the card and are never pulled off it.
static void check_sequenced_reports_are_collected(void) {
	struct {
		const char* path;
		const char* needle;
		const char* why;
	} sinks[] = {
		{"source/platform/hw_telemetry.c", "wd_report_path_for_seq",
		 "the crash mailbox replays only report 1, so stalls 2..8 are never sent"},
		{"tools/wii_stage_server/source/stage_manifest_paths.h", "sd:/hang_report_8.txt",
		 "the stage server does not serve the sequenced reports, so they cannot be "
		 "fetched"},
		{"tools/run_staged_wii_hardware.ps1", "hang_report_",
		 "the staged hardware runner does not pull the sequenced reports into the run "
		 "directory"},
	};
	for(size_t i = 0; i < sizeof(sinks) / sizeof(sinks[0]); i++) {
		char* text = read_source(sinks[i].path);
		if(!strstr(text, sinks[i].needle)) {
			fprintf(stderr,
					"test_watchdog_phase: %s omits the sequenced hang reports -- %s\n",
					sinks[i].path, sinks[i].why);
			assert(0 && "a sequenced hang report is never collected");
		}
		free(text);
	}

	// And the CALL SITE, which (9d) cannot see: a pure path builder that the writer never
	// calls is exactly the "orphan instrument" defect this file exists for. Both write
	// callbacks must route the HANG path through it, and the ctx must carry the sequence.
	char* wd = read_source(WII_WATCHDOG);
	const char* prim = strstr(wd, "wd_emit_write_primary_cb");
	const char* fb = strstr(wd, "wd_emit_write_fallback_cb");
	assert(prim && fb && "the write callbacks are gone");
	const char* prim_end = strstr(prim, "\n}");
	const char* fb_end = strstr(fb, "\n}");
	assert(prim_end && fb_end);
	assert((size_t)(prim_end - prim) < 600 && (size_t)(fb_end - fb) < 600);
	char body[600];
	snprintf(body, sizeof(body), "%.*s", (int)(prim_end - prim), prim);
	assert(strstr(body, "wd_report_path_for_seq")
		   && "the primary writer went back to a fixed hang-report path -- report N will "
			  "truncate report N-1 again");
	snprintf(body, sizeof(body), "%.*s", (int)(fb_end - fb), fb);
	assert(strstr(body, "wd_report_path_for_seq")
		   && "the fallback writer went back to a fixed hang-report path");
	assert(strstr(wd, ".seq = wd_dump_report_seq()")
		   && "the emit ctx no longer carries this report's sequence, so every report "
			  "would build the same path");
	free(wd);

	printf("  [9e] reports 2..%d are built by the writer AND reach the mailbox, the stage "
		   "manifest and the runner\n",
		   WD_REPORT_SEQ_MAX);
}

// --- (10) the SD fallback reports are actually COLLECTED ---------------------
// A fallback nobody fetches is not a fallback. The watchdog writes the app-dir pair when
// the sd:/ root open fails -- precisely the boot on which the root paths hold nothing --
// so every collection path has to name them: the on-device crash-mailbox replay, the
// stage server's manifest, and the staged-hardware runner that pulls the mailbox.
static void check_fallback_reports_are_collected(void) {
	// Compile-time: the strings themselves have ONE owner, so the four consumers cannot
	// drift apart into paths that merely look alike.
	assert(strcmp(HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH,
				  HW_TELEMETRY_HANG_REPORT_PATH)
		   != 0);
	assert(strcmp(HW_TELEMETRY_FBGUARD_REPORT_FALLBACK_PATH,
				  HW_TELEMETRY_FBGUARD_REPORT_PATH)
		   != 0);

	struct {
		const char* path;
		const char* why;
	} sinks[] = {
		{"source/platform/hw_telemetry.c",
		 "the crash mailbox replays only the primary reports, so a boot that fell back "
		 "sends nothing"},
		{"tools/wii_stage_server/source/stage_manifest_paths.h",
		 "the stage server does not serve the fallback, so it can never be fetched"},
		{"tools/run_staged_wii_hardware.ps1",
		 "the staged hardware runner does not pull the fallback into the run directory"},
	};
	for(size_t i = 0; i < sizeof(sinks) / sizeof(sinks[0]); i++) {
		char* text = read_source(sinks[i].path);
		// hw_telemetry.c names them through the macros; the tools spell the literal path.
		bool ok = strstr(text, "HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH")
			|| strstr(text, "sd:/apps/CavEX/hang_report.txt");
		bool ok2 = strstr(text, "HW_TELEMETRY_FBGUARD_REPORT_FALLBACK_PATH")
			|| strstr(text, "sd:/apps/CavEX/fbguard_report.txt");
		if(!ok || !ok2) {
			fprintf(stderr, "test_watchdog_phase: %s omits a fallback report path -- %s\n",
					sinks[i].path, sinks[i].why);
			assert(0 && "an SD fallback report path is never collected");
		}
		free(text);
	}
	printf("  [10] both app-dir fallback reports reach the mailbox, the stage manifest "
		   "and the runner\n");
}

// --- (4) the live mesher ----------------------------------------------------
// RPC drain thread: the full client links clin_*; keep its inbox empty so no path
// blocks. The client world below is independent of it. (Shape from
// test_piston_chunk_mesh_env.c.)
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static struct world w;

static void set(w_coord_t x, w_coord_t y, w_coord_t z, int type) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = 0,
										 .sky_light = 15,
										 .torch_light = 0},
					false /* no light flood: sky_light is set explicitly */);
}

// A full stone floor plus a scattered layer above it, so each build emits a
// realistic number of faces instead of returning almost immediately.
static struct chunk* build_scene(void) {
	for(w_coord_t x = 0; x < CHUNK_SIZE; x++)
		for(w_coord_t z = 0; z < CHUNK_SIZE; z++) {
			set(x, YF, z, BLOCK_STONE);
			if(((x + z) & 1) == 0)
				set(x, YF + 1, z, BLOCK_STONE);
		}
	return world_find_chunk(&w, 8, YF, 8);
}

static bool wait_worker_phase(uint32_t want) {
	ptime_t t0 = time_get();
	while(watchdog_phase_worker_current() != want)
		if(time_diff_ms(t0, time_get()) > PHASE_TIMEOUT_MS)
			return false;
	return true;
}

// Keep the request queue saturated and sample the worker tag until WDW_BUILD shows.
// With work always queued the worker is inside chunk_mesher_build for effectively all
// of its runtime -- its receive never blocks, and its send cannot block (the results
// channel's capacity equals the whole token pool, and the worker holds a token) -- so
// a multi-second sample cannot plausibly miss the build window.
static bool observe_worker_build(struct chunk* c) {
	ptime_t t0 = time_get();
	bool seen_build = false, seen_send = false;
	while(!seen_build && time_diff_ms(t0, time_get()) <= PHASE_TIMEOUT_MS) {
		while(chunk_mesher_send(c)) // claim every free token
			;
		uint32_t p = watchdog_phase_worker_current();
		if(p == WDW_BUILD)
			seen_build = true;
		else if(p == WDW_SEND)
			seen_send = true;
		chunk_mesher_receive(); // recycle tokens so the queue stays fed
	}
	// Drain what is still in flight so the chunk's refcount returns to the world's
	// baseline before teardown.
	ptime_t t1 = time_get();
	while(chunk_mesher_pending() > 0
		  && time_diff_ms(t1, time_get()) <= PHASE_TIMEOUT_MS) {
		chunk_mesher_receive();
		thread_msleep(1);
	}
	// WDW_SEND is a microseconds-wide window between two non-blocking calls, so it is
	// reported, never asserted -- a deleted WDW_SEND stamp is caught by the source
	// scan in (2), which is why that scan exists.
	printf("  [4] worker sampled in BUILD=%s SEND=%s (SEND is informational)\n",
		   seen_build ? "yes" : "no", seen_send ? "yes" : "no");
	return seen_build;
}

static void check_mesher_reports_build(void) {
	assert(chunk_mesher_init() && "mesher worker thread did not start");
	// The worker's first act is to stamp WDW_RECV and park on its request channel.
	assert(wait_worker_phase(WDW_RECV)
		   && "mesher worker never reported WDW_RECV while parked on its queue");
	printf("  [4] worker parked on an empty queue reports WDW_RECV\n");

	world_create(&w);
	struct chunk* c = build_scene();
	assert(c);
	assert(chunk_get_block(c, 8, YF & 15, 8).type == BLOCK_STONE);

	assert(observe_worker_build(c)
		   && "mesher worker never reported WDW_BUILD during a real chunk build");

	// Back to RECV once the queue drains: the stamp is per iteration, not once at
	// thread start (a stamp hoisted above the loop would pass the check above and
	// fail here).
	assert(wait_worker_phase(WDW_RECV)
		   && "mesher worker did not return to WDW_RECV after the queue drained");
	printf("  [4] worker returns to WDW_RECV after the queue drains\n");
}

int main(void) {
	printf("test_watchdog_phase:\n");
	check_phase_roundtrip();
	check_every_phase_is_stamped();
	check_main_tags_distinct();
	check_dump_seal();
	check_emit_order();
	check_emit_status_matches_card();
	check_dump_identity();
	check_counters_reach_a_shipping_sink();
	check_paint_precedes_stderr();
	check_uptime_is_a_delta_not_a_wall_clock();
	check_uptime_wrong_fixes_stay_untaken();
	check_hang_reports_do_not_overwrite_each_other();
	check_sequenced_reports_are_collected();
	check_fallback_reports_are_collected();

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();

	check_mesher_reports_build();

	g_drain_run = 0;
	printf("test_watchdog_phase: PASS\n");
	return 0;
}
