/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).
*/

#ifndef PLATFORM_WATCHDOG_H
#define PLATFORM_WATCHDOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
	Hang/fault watchdog (Wii). A high-priority LWP thread (prio 100 > the prio-64 main
	and server threads) detects that the main render loop has STALLED (no heartbeat for
	~2s) and -- even though the main thread is wedged -- emits the hang LOCATION: the
	current coarse PHASE tag of the main and server threads, the last chunk coord, and the
	diagnostic counters. It writes them to hang_report.txt on the SD card AND paints a
	PANIC band on the frozen framebuffer. So a no-dump freeze finally NAMES where it hung.

	It also overrides libogc's exception panic fn so a REAL CPU fault (DSI/ISI/program)
	dumps SRR0/registers the same way -- and is distinguishable from a hang (libogc's
	default panic fn prints to a dead UART then spins, which is why a fault has looked
	identical to a hang).

	On PC every function is a no-op (defined in the PLATFORM_PC branch of watchdog.c).
*/

enum wd_phase_main { // MAIN render thread (prio 64)
	WDM_IDLE = 0, WDM_CLIN_UPDATE, WDM_CLIENT_TICK, WDM_PRERENDER, WDM_LIGHTING,
	WDM_LOD, WDM_BUILD_CHUNKS, WDM_SCREEN_UPDATE, WDM_GFX_FLIP, WDM_MESH_RECV,
	WDM_RENDER, WDM_GFX_FINISH,
	// gfx_flip_buffers sub-phases (which of its steps is the GP/flip hang).
	// FBPUSH/SEMWAIT replaced MQSEND/MQRECV when the MQ framebuffer handoff
	// (libogc v3.0.4 MQ_Send silently drops on a full queue -- the audited
	// cause of the 2026-06-07 run2 + 2026-06-11b flip hangs) was replaced by
	// the SPSC-ring + semaphore handoff in wii/gfx.c.
	WDM_GFX_DRAWDONE, WDM_GFX_FBPUSH, WDM_GFX_SEMWAIT,
	// SHUTDOWN phases (Batch E item 1, 2026-08-23 HBC-direct RCA): main.c's
	// post-loop teardown (server_local_shutdown -> audio_shutdown ->
	// hw_telemetry_shutdown -> return 0) used to run with ZERO watchdog
	// coverage -- no phase update and no heartbeat after the loop's last
	// WDM_GFX_FINISH stamp, so a slow-but-alive shutdown step painted a
	// stale "hung in GFX_FINISH" panic once the fire-once 3s timer elapsed
	// (the RCA's finding (d): the reported "GFX_FINISH hang" was a label
	// artifact, not a real GPU wedge). APPENDED, never reordered/inserted --
	// same rule as WDS_SLOT_WAIT/WDS_RPC_SEND above and WDB_WEATHER in
	// watchdog_draw_site: an existing phase's numeric value is baked into
	// decoded hang reports.
	WDM_SHUTDOWN_SERVER, WDM_SHUTDOWN_AUDIO, WDM_SHUTDOWN_TELEMETRY,
	WDM_SHUTDOWN_WPAD,
	// C1 (candidate, hardware-unconfirmed, 2026-09-02): watchdog_shutdown()
	// itself is now the LAST step before the pre-loader-return breadcrumb --
	// stopping the detector thread that would otherwise stay armed through
	// the loader handoff. Tagged so a stall INSIDE that join (the one shutdown
	// step where the watchdog is stopping ITSELF) still reports under its own
	// honest label instead of silently inheriting WDM_SHUTDOWN_WPAD's. APPENDED,
	// never inserted -- same rule as the other WDM_SHUTDOWN_* values above.
	WDM_SHUTDOWN_WATCHDOG,
	// T-b (2026-09-03, Round 3 (3) instrumentation-gap fix): main.c's
	// emit_hw_heartbeat call site (~1940) previously stamped NOTHING of its
	// own, so the heartbeat's own wd_main field -- and any hang report a
	// stall inside heartbeat-building/sending produced -- always read
	// whichever phase was last stamped earlier in the frame (WDM_CLIENT_TICK,
	// 1874), a label that carried zero information about where the main
	// thread actually was. Stamped immediately before the emit_hw_heartbeat
	// call, so a hang while building/sending the heartbeat (mallinfo, the
	// mem2/mesher stat reads, the JSON format, the non-blocking net_send) is
	// now correctly attributed instead of misreported as CLIENT_TICK.
	// APPENDED, never inserted -- same rule as every other WDM_* value above.
	WDM_HEARTBEAT,
	// 2026-09-11 (pack-switch HANG reports seq 1/2 on 5f2f5af5: 3198 / 4663 ms in
	// SCRN_UPD): the texture-pack apply reloads every texture plus the sound
	// and music registries synchronously on the main thread. Stamped around
	// that reload so a report inside it names the operation instead of the
	// generic screen update. APPENDED, never inserted.
	WDM_PACK_RELOAD, WDM_MAX
};
enum wd_phase_srv { // SERVER thread (prio 64) -- the chunk gen/stream/evict pipeline
	WDS_IDLE = 0, WDS_RPC, WDS_SPAWN, WDS_RANDOM_TICK, WDS_WORLD_TICK,
	WDS_EVICT_NEAR, WDS_EVICT_FAR, WDS_NEAR_SCAN, WDS_ENSURE_POPULATED,
	WDS_CHUNK_SEND, // assembling + sending ONE chunk payload. Shared by the near send
					// (fill the pooled slot: 5 memcpys ~80 KB, the piston-moving
					// snapshot, worldgen_climate) and the far batch (generate + extract
					// the ~1 KB skin). Tell them apart by the report's chunk COORD --
					// a far column is outside MAX_VIEW_DISTANCE of the player, a near
					// one is not.
	// APPENDED, never reordered: hang reports are decoded from the RAW cell counts,
	// so an existing phase's number is part of the diagnostic record (the 2026-07-27
	// dump read srv=8 == WDS_ENSURE_POPULATED). These two split the near-send span
	// that previously carried the ENSURE_POPULATED tag all the way through the
	// blocking slot acquire and the blocking CRPC_CHUNK send -- the stale label that
	// misdirected that investigation. Each brackets its ONE blocking call and nothing
	// else: a tag that outlives the call it names is the same defect with a more
	// convincing label, so the span between them (and after them) is CHUNK_SEND /
	// NEAR_SCAN, never a leftover.
	WDS_SLOT_WAIT,  // parked in clin_chunk_slot_acquire (client not draining chunks)
	WDS_RPC_SEND,   // parked in clin_rpc_send (client not draining the token pool)
	// APPENDED 2026-09-08 (server-loop wedge instrumentation, lane/carve-observer-boot):
	// real-Wii split_anchor_6m evidence (see server_local.c's server_local_thread) could
	// not tell "sleeping in thread_msleep" from "looping with 0 ticks" from "blocked
	// inside FAR" -- wd_srv froze at WDS_IDLE(0) for 400+ heartbeats after a single
	// 109.6s FAR-phase stretch. These five split that ambiguity. Never reordered: an
	// existing phase's number is part of the decoded hang-report record (see above).
	WDS_IDLE_SAMPLE, // about to sample elapsed time / call server_tick_clock_advance --
					  // pure compute, the "looping with 0 ticks" case if seen repeatedly
	WDS_IDLE_SLEEP,   // parked inside thread_msleep (both the ticks_pending==0 wait and
					  // the end-of-tick inter-tick sleep) -- the "sleeping forever" case
	WDS_FAR_EXTRACT,  // inside server_world_extract_far_skin (cheap terrain-only far skin,
					  // far_lod_terrain_only path) -- compute, no disk/lock touched
	WDS_FAR_POPULATE, // inside server_world_ensure_populated_far (faithful full populate
					  // cascade + tree/snow decoration) -- compute, may itself enter
					  // WDS_REGION_IO below via server_world_load_chunk
	WDS_REGION_IO,    // inside server_world_load_chunk_impl's disk read (region file
					  // stat/open + region_archive_get_chunk fread) -- the one real
					  // blocking I/O reachable from either WDS_ENSURE_POPULATED (near)
					  // or WDS_FAR_POPULATE (far); shared code, not far-exclusive.
					  // KNOWN GAP (documented, not silent): only the entry/exit of the
					  // disk-read block restore the caller's prior phase: a handful of
					  // rare mid-block failure returns (corrupt region / OOM / stale
					  // piston sidecar) leave the tag at WDS_REGION_IO until the next
					  // watchdog_phase_srv call rather than restoring immediately. Those
					  // paths return promptly (no further blocking under the stale
					  // label), so this does not hide a freeze -- it can only mislabel
					  // an already-fast error exit for at most one loop iteration.
	WDS_SAVE,         // the autosave sweep: the serialise/save half (server_world_save_chunk)
	// APPENDED 2026-09-11 (Astra round 5 C / round 9 X: measure selection vs
	// I/O before optimising): the sweep's dict WALK to the next eligible column,
	// and the near evictor's furthest-column SCAN (the loop condition).
	// WDS_SAVE / WDS_EVICT_NEAR keep their meaning as the save/free halves.
	WDS_SAVE_SELECT,
	WDS_EVICT_SELECT,
	// APPENDED 2026-09-24 (lava-aabb-0924/H1, far_walk_6m autosave-window hang
	// probe): the dump that motivated this read srv=CHUNK_SEND chunk=(6,6) while
	// the far batch had made ZERO picks -- server_local.c stamped CHUNK_SEND on
	// far-batch ADMISSION, and everything after the batch (the hazard/portal/
	// inventory tail incl. a BLOCKING clin_rpc_send, then the loop epilogue's
	// carve-observer dict walk) ran under that stale label. Three honest
	// phases, each stamped only where the named work really starts:
	WDS_FAR_SELECT, // far batch: template rebuild + backlog scan + the pick walk
					// (no column chosen yet). CHUNK_SEND now means a column WAS
					// picked and is being generated/assembled/sent.
	WDS_TAIL,       // server_local_update's post-stream tail: player hazard/fluid
					// contact, portal dwell, invalidated-inventory close. Its one
					// blocking call (clin_rpc_send CRPC_RETURN_INGAME) is bracketed
					// by WDS_RPC_SEND like every other blocking send.
	WDS_EPILOGUE,   // server_local_thread after server_local_update returns:
					// the carve-observer boundary hook (walks s->world.chunks),
					// the TPS boundary hook, tick timing -- until IDLE_SLEEP.
	WDS_MAX
	// HEADROOM (H2 review fix (9)): WDS_MAX is 23 and the heartbeat's
	// per-phase profiler array is hw_heartbeat.srv_ph[24] (hw_telemetry.h);
	// ONE more phase fits, the next after that must widen srv_ph and the
	// "srv_ph" wire array together (and the collector's reader of it).
	// NOTE: WDS_MAX is both the cell-bar CAP and the SRV_NM bound in
	// wii/watchdog.c -- a new enumerator needs its label appended there too.
};
enum wd_phase_worker { WDW_IDLE = 0, WDW_RECV, WDW_BUILD, WDW_SEND, WDW_MAX };

// ---- hang-report ASSEMBLY + emit ORDER (shared PC/Wii, so both are TESTABLE off-device) --
// The dump buffer, its append/seal helpers and the emit SEQUENCER live outside the
// PLATFORM_WII branch on purpose: every rule they encode (the trailer that makes a
// truncated file self-identifying, paint-before-SD, the status/artifact agreement) only
// ever manifests on a wedged console, where the only instrument is a photograph. Keeping
// them platform-neutral is what lets tests/test_watchdog_phase.c drive the whole contract
// with fakes -- including the outcome that cannot be provoked at all on hardware, an SD
// call that NEVER RETURNS. Wii-only state (XFB paint, libfat, GX registers) stays in
// wii/watchdog.c and reaches this code through the callbacks below.

#define WD_DUMP_CAP 2048 // s_wd_dump size: body + the reserved trailer tail
// END-OF-REPORT trailer. Written into a RESERVED tail (see wd_dump_seal), so a report
// that reached the card in full ALWAYS ends with it and one that was cut short NEVER
// does. That is what lets a reader holding only the file decide whether it is complete,
// independently of what the on-screen status row claims.
#define WD_DUMP_TRAILER "==== END CAVEWII REPORT ====\n"
// Body limit: everything except the reserved trailer tail (sizeof() counts the NUL, which
// is exactly the byte the sealed string needs).
#define WD_DUMP_BODY_MAX (WD_DUMP_CAP - sizeof(WD_DUMP_TRAILER))

char* wd_dump_buf(void);   // the single static assembly buffer (no alloc post-wedge)
void wd_dump_reset(void);  // empty it before assembling a report
// Append to the buffer at `used`, returning the new length. vsnprintf returns the bytes
// it WANTED to write, not the bytes that fit, so the running offset is clamped to
// WD_DUMP_BODY_MAX - 1: a truncated field can never turn the next append's size into an
// underflow, and never eats the reserved trailer tail.
size_t wd_dump_appendf(size_t used, const char* fmt, ...);
// Terminate the report with WD_DUMP_TRAILER. Always fits (the tail is reserved) and is
// the LAST thing appended, so the sealed length is what every sink -- stderr, SD,
// telemetry -- must write in full.
size_t wd_dump_seal(size_t used);
// True when `text[0..n)` ends with the trailer, i.e. this is a WHOLE report. A reader's
// (and the test's) completeness check; the crash-mailbox replay can use the same rule.
bool wd_dump_is_sealed(const char* text, size_t n);

// BOOT/BUILD IDENTITY line, stamped into every report (2026-07-29). WD_WRITE_NONE and the
// FAILED / FALLBACK statuses all mean "no file was written THIS boot" -- but a COMPLETE,
// sealed report from an EARLIER boot can still be sitting at sd:/hang_report.txt, so the
// on-screen status row and the artifact on the card contradict each other and the reader
// cannot tell which is which. The dump carried no boot or build identity at all, so a
// stale report read exactly like this one's. This line carries what differs between boots:
//   build -- CW_BUILD_ID, the same stamp the F3 overlay prints, so a report left by an
//            EARLIER .dol is obvious even when the two runs are minutes apart
//   boot  -- a per-boot nonce (the Wii seeds it from the timebase in watchdog_init), so
//            two runs of the SAME .dol are still told apart
//   up    -- ms since WATCHDOG INIT when the report fired (2026-07-29: this used to say
//            "since power-on" and the code implemented exactly that claim -- it passed the
//            raw timebase through. The Wii timebase is NOT zero at power-on; libogc seeds
//            it from the RTC, so gettime()/60750000 is seconds since 2000-01-01 and a
//            40-minute session reported up=1149429345ms = 13 days. The base is now captured
//            in watchdog_init and the field carries the delta -- see wd_uptime_ticks in
//            watchdog.c. A fault before that base is set honestly reports up=0.)
//   seq   -- the n-th report of this boot, which separates the non-fatal FBGUARD report
//            from the HANG report that may follow it
// It is stamped as the SECOND line, so even a file cut short after a few hundred bytes
// still identifies its boot. Shared rather than Wii-only for the same reason the rest of
// the assembly is: the format is then exercised off-device. `seq` is owned here and
// advances once per call.
size_t wd_dump_identity(size_t used, const char* build_id, uint32_t boot_id,
						uint32_t uptime_ms);
uint32_t wd_dump_report_seq(void); // reports stamped so far this boot

// Guarded uptime delta for the `up=` field above, in TIMEBASE TICKS (the caller applies the
// platform's ticks->ms conversion). base_ticks == 0 means "base not seeded yet" and yields 0;
// a non-monotonic now_ticks <= base_ticks yields 0 rather than underflowing into a ~9,700-year
// uptime. Shared so it is testable off-device -- see the long note at its definition for why
// the raw timebase is a wall clock and why time_reset()/settime(0) is the WRONG fix.
uint64_t wd_uptime_ticks(uint64_t base_ticks, uint64_t now_ticks);

// SEQUENCED HANG-REPORT PATHS (2026-07-29). wd_write_report opens its path with
// fopen(path, "w"), which TRUNCATES -- so with one fixed path, report N destroyed report
// N-1 and only the LAST stall of a session was ever readable. The 2026-07-29 card report
// read seq=6 with fbguard=intact: six separate >3s main-loop stalls that boot, of which
// five were unrecoverable. (The transient "crash screen that goes away" the maintainer saw
// is the fire-once-then-re-arm detector working as designed -- it paints into the live XFB
// and the next gfx_flip erases it.)
//
// Report 1 keeps the canonical HW_TELEMETRY_HANG_REPORT_PATH so every existing collector
// -- the crash mailbox, the stage manifest, the staged-hardware runner, and a human
// looking at the card -- keeps working unchanged. Reports 2..WD_REPORT_SEQ_MAX get
// `_<seq>` inserted before the extension.
//
// BOUNDED so a stall that repeats every 250ms cannot fill the card: past the bound the
// sequence CLAMPS, so slot WD_REPORT_SEQ_MAX always holds the most recent report while
// 1..MAX-1 preserve the earliest ones. That keeps both ends of a session -- the first
// stall (often the boot-time one) and the latest -- which is what a diagnosis needs.
//
// Only the HANG pair is sequenced. The FBGUARD report is one-shot per boot
// (g_wd_guard_reported), so it cannot overwrite itself and keeps its fixed name. The two
// share the seq counter, so an FBGUARD at seq 2 leaves a GAP in the hang numbering -- that
// gap is information (it says a guard report happened), and the seq= in each file's own
// identity line remains authoritative.
#define WD_REPORT_SEQ_MAX 8
#define WD_REPORT_PATH_MAX 64
// Build the seq-suffixed variant of `base_path` into `buf`. Returns `buf`, or `base_path`
// itself when seq <= 1, or when the result would not fit -- degrading to the historical
// overwrite rather than writing to a truncated path. NEVER allocates: this runs after the
// heap is suspect, and from exception context.
const char* wd_report_path_for_seq(char* buf, size_t cap, const char* base_path,
								   uint32_t seq);

// Outcome of ONE report write to ONE path. NONE means nothing is left at that path --
// either it was never created, or an incomplete attempt was successfully removed -- so
// the status row derived from it never claims a file that is not there.
enum wd_write_result {
	WD_WRITE_NONE = 0, // no file exists at this path
	WD_WRITE_PARTIAL,  // an unusable/incomplete or stale file could not be removed
	WD_WRITE_OK        // the whole sealed report is on the card
};

/* Classify the result of retiring a failed write's on-card artifact. ENOENT is
 * the only failed remove that proves the desired NONE state; every other error
 * leaves an untrusted path behind and must be surfaced as PARTIAL. */
enum wd_write_result wd_write_cleanup_result(int remove_result,
										  int remove_error);

// Outcome of the LAST report write, painted as its own cell-bar row. Until 2026-07-27
// the fopen result was DISCARDED (the if(f) had no else), so a reader who found no
// hang_report.txt could not tell a failed write from an incident that never fired --
// the write is best-effort by design, so its failure must be part of the diagnostic.
// The values are CELL COUNTS on a photograph, so each one states exactly what is on the
// card; nothing here may over-claim.
enum wd_sd_status {
	WD_SD_NONE = 0,  // 0 cells: no write attempted (the emit never reached the SD step)
	WD_SD_TRYING,    // 1: attempt IN FLIGHT. Seeing 1 on the photograph means the SD call
					 //    NEVER RETURNED (the libfat-on-a-smashed-heap wedge) -- at most a
					 //    partial file exists, and it will lack the END trailer.
	WD_SD_PRIMARY,   // 2: the COMPLETE report is at HW_TELEMETRY_*_REPORT_PATH
	WD_SD_FALLBACK,  // 3: the COMPLETE report is at the app-dir fallback; the primary path
					 //    holds no usable file (absent, or a trailer-less remnant)
	WD_SD_PARTIAL,   // 4 (red): NO complete report anywhere; an INCOMPLETE file survives on
					 //    the card -- it lacks the trailer, do not read it as the whole story
	WD_SD_FAILED,    // 5 (red): nothing was created at all; this photograph is the evidence
	WD_SD_MAX
};

// Platform hooks for wd_emit_sequence. Each is called EXACTLY where the ordering contract
// says, which is the whole point of routing them through a callback: on hardware the
// paint is the only instrument that survives a wedged writer, so it cannot be sequenced
// after one.
struct wd_emit_ops {
	void* ctx;
	void (*status)(void* ctx, enum wd_sd_status s); // publish the cell-bar row value
	void (*paint)(void* ctx);   // banner + cell bars; runs BEFORE any SD call
	void (*repaint)(void* ctx);  // cell bars only; runs AFTER the outcome is known
	enum wd_write_result (*write_primary)(void* ctx, size_t n);
	enum wd_write_result (*write_fallback)(void* ctx, size_t n);
};

// Drive the emit ORDER and the status/artifact agreement, and return the final status.
//   paint -> write primary -> (write fallback) -> publish status -> repaint
// PAINT FIRST is load-bearing: the documented real-hardware failure is that the fopen
// path WEDGES (libfat allocating from a smashed heap, never returning -- the historical
// "banner without hang_report.txt" symptom, see hw_telemetry.c). Sequencing the write
// first means such a wedge costs the banner AND the cell bars, leaving a console
// indistinguishable from a plain freeze; it is worse still from wii/panic.c, which calls
// the emit from EXCEPTION context and then spins to hold the banner up. The repaint is
// what still gets the RESOLVED status onto the screen -- wd_paint_diag is idempotent and
// only touches its own strips.
// `fatal` is false only for the non-fatal FBGUARD report, which must not paint a panic
// banner over a live game.
enum wd_sd_status wd_emit_sequence(const struct wd_emit_ops* ops, bool fatal, size_t n);

bool watchdog_init(void);                  // create the detector thread (after gfx + SD)
#ifdef PLATFORM_WII
/* Temporary freeze observer. Callback runs on the high-priority watchdog;
 * it must remain bounded and never wait, allocate, or perform I/O. Main must
 * clear it before destroying any observed thread. */
void watchdog_context_observer_set(void (*observer)(void));
#endif
// C1 (candidate, hardware-unconfirmed): stop the detector thread and JOIN it --
// returns only once the thread has exited. Idempotent (a second call, or a call
// when watchdog_init() never ran/failed, is a harmless no-op). Does NOT change
// the 3s fire-once hang semantics, the prio-100 preemption reasoning, or the
// UNCHANGED 250ms/150ms usleep() wait durations at watchdog_init's definition --
// this only adds an exit check immediately before and after each of those two
// waits, on top of the loop that already exists (an unmeasured change to the
// wake cadence itself would be a T3 perf change, not this fix's scope). A
// shutdown join can therefore take up to ~400ms (the sum of both waits), which
// is accepted so the certified hot-path behaviour stays byte-identical.
// Call this LAST in the shutdown sequence, immediately before the process
// returns to the loader, so every earlier shutdown phase stays covered by a
// live watchdog. PC: no-op (there is no detector thread to stop).
void watchdog_shutdown(void);
void watchdog_install_fault_handler(void); // override the exception panic fn (early)
void watchdog_heartbeat(void);             // call ONCE per main-loop frame
// Declare a KNOWN synchronous main-thread operation of up to `ms` before
// running it (the texture-pack reload): the detector fires a HANG report only
// past 3000 ms + this grace, so a legitimate multi-second reload no longer
// paints the panic banner and burns a hang_report slot, while a real freeze
// inside the operation still reports once the grace is exhausted. Consumed
// by the next watchdog_heartbeat (the frame after the operation), so it can
// never linger past the frame that declared it. PC: no-op.
void watchdog_expect_stall_ms(uint32_t ms);
void watchdog_phase_main(enum wd_phase_main p);
void watchdog_phase_srv(enum wd_phase_srv p);
void watchdog_phase_worker(enum wd_phase_worker p);
uint32_t watchdog_phase_main_current(void);
uint32_t watchdog_phase_srv_current(void);
/* Server-phase profiler (2026-09-10, Astra TPS diagnosis): microseconds the
 * server thread spent in each wd_phase_srv since the last take. Accumulated
 * at every watchdog_phase_srv transition (server thread only); taken and
 * zeroed by the main thread once per heartbeat. WDS_MAX slots. */
void watchdog_srv_phase_take(uint32_t out_us[]);
uint32_t watchdog_phase_worker_current(void);
/* Temporary cold-stall liveness probe: increments at the existing 250ms wake. */
uint32_t watchdog_loop_seq(void);
/* Temporary main-thread heartbeat census breadcrumb (0=outside). */
void watchdog_main_census_phase(uint32_t phase);
uint32_t watchdog_main_census_current(void);
#if defined(PLATFORM_PC) && defined(CAVEWII_FAR_SLOT_DEADLOCK_TEST)
/* Synchronous PC-test observation of the exact phase stores. The callback runs
 * on the setter's thread after the current phase has been published. */
typedef void (*watchdog_test_srv_phase_observer)(enum wd_phase_srv phase,
											 void* user);
void watchdog_test_set_srv_phase_observer(
	watchdog_test_srv_phase_observer observer, void* user);
#endif
uint32_t watchdog_heartbeat_gap_max_ms(void);
void watchdog_main_timings_x10(uint32_t bfs, uint32_t light, uint32_t lod,
							   uint32_t build, uint32_t bfs_max,
							   uint32_t light_max, uint32_t lod_max,
							   uint32_t build_max);
void watchdog_coord(int32_t cx, int32_t cz); // last chunk coord being processed
/* H2 review fix (9): the "no chunk in hand" coordinate. Stamped with WDS_TAIL
 * so a TAIL / EPILOGUE report does not carry the last far column as if it
 * were being worked on; decodes as chunk=(-2147483648,-2147483648). */
#define WD_COORD_NONE INT32_MIN

// GP-wedge DRAW BREADCRUMB (2026-06-11 console wedge: main=GFX_DRAWDONE,
// cmdIdle=0, canary CLEAN -- a pure GX command-stream desync, channel still
// unnamed). The render thread stamps which draw SITE it most recently entered
// (plus the entity identity for the entity pass), so the NEXT hang report
// NAMES the last draw the GP was fed instead of leaving the whole frame as
// the suspect. Three volatile u32 stores per site -- effectively free. The
// breadcrumb marks the site ENTERED, not completed: if the GP wedges, the
// dump's site is either the wedging draw or the one immediately after the
// wedging bytes entered the FIFO -- both name a neighborhood of a few calls.
enum watchdog_draw_site {
	WDB_NONE = 0,
	WDB_SKY = 1,       // gutil_sky_box
	WDB_WORLD = 2,     // world_render opaque pass
	WDB_SCREEN3D = 3,  // current_screen->render3D
	WDB_PARTICLES = 4, // particle_render
	WDB_ENTITY = 5,    // entities_client_render (id/aux identify WHICH entity)
	WDB_WORLD_T = 6,   // world_render translucent pass (fancy liquids)
	WDB_CLOUDS = 7,    // gutil_clouds
	WDB_GUI = 8,       // gfx_mode_gui onward (HUD/overlay/screens)
	// render-slice (RENDER_DISPATCH.md): render_rain_snow, between the
	// translucent world pass and clouds (Beta's own order). Appended rather
	// than inserted so existing numeric IDs (any external hang-report
	// tooling keyed on them) stay stable.
	WDB_WEATHER = 9,
	// CLAMP: SITE_NM in wii/watchdog.c was one entry short of WDB_WEATHER, so
	// nm()'s bounds check (v < n) rejected the in-range value 9 and every
	// weather hang breadcrumb printed "?" instead of "weather". WDB_MAX pins
	// the table length to the enum the same way WDM_MAX/WDS_MAX already do,
	// via the _Static_assert alongside SITE_NM.
	WDB_MAX
};
extern volatile uint32_t g_wd_draw_site; // enum watchdog_draw_site
extern volatile uint32_t g_wd_draw_id;   // entity id at WDB_ENTITY, else 0
extern volatile uint32_t g_wd_draw_aux;  // (entity type << 8) | monster id

// GP-wedge FIFO write-offset RING (2026-06-13). The site breadcrumb above names the
// CPU's WRITE side only; on a full-FIFO wedge the GP READ pointer (rd, _cpReg[28/29])
// sits up to a whole 256KB lap EARLIER than the last write, so the site alone cannot
// say which pass's bytes the GP actually choked on. Each breadcrumb ALSO snapshots the
// GX FIFO CPU write pointer (_piReg[5], physical, 32B-masked) at site ENTRY into this
// ring; wd_emit_dump then finds the pass whose [piwr_start, piwr_end) brackets the
// frozen rd and prints "wedge pass=<site>". rd and piwr are BOTH physical addresses of
// the same fifoBuffer (verified vs the libogc tuxedo fork gx.c:364-373; libogc itself
// masks _piReg[5] with 0x1FFFFFE0), so they share one coordinate space. The ring's
// STORAGE is free; the only added hot-path cost is one uncached MMIO read per
// breadcrumb -- so that read is WII-ONLY and PC builds stay no-ops (no MMIO at all).
#define WD_PIWR_RING 32 // power-of-2; ~1 frame of sites. Bigger reaches further back
						// but risks straddling multiple frames/FIFO laps (the multi-lap
						// ambiguity wd_emit_dump annotates); per-call cost is fixed.
struct wd_piwr_ent {
	uint32_t site; // enum watchdog_draw_site recorded at entry
	uint32_t piwr; // _piReg[5] & 0x1FFFFFE0: physical FIFO write offset at entry
};
extern volatile struct wd_piwr_ent g_wd_piwr_ring[WD_PIWR_RING];
extern volatile uint32_t g_wd_piwr_head; // free-running push count; mask to index

static inline void watchdog_draw_breadcrumb(uint32_t site, uint32_t id,
											uint32_t aux) {
	g_wd_draw_site = site;
	g_wd_draw_id = id;
	g_wd_draw_aux = aux;
#ifdef PLATFORM_WII
	// WII-ONLY: the single added MMIO read on the hot render path (~6+/frame).
	// 0xCC003014 = _piReg[5] (GP FIFO CPU write pointer); mask 0x1FFFFFE0 strips the
	// wrap/flag bits and 32B-aligns, exactly as libogc writes it (gx.c:373). Single
	// writer (the render thread); the watchdog reads the ring only AFTER that thread is
	// wedged/parked, so plain volatile stores are correct (aligned scalars don't tear,
	// same argument as the breadcrumb stores above) -- no lock, no barrier.
	uint32_t piwr = (*(volatile uint32_t*)0xCC003014u) & 0x1FFFFFE0u;
	uint32_t h = g_wd_piwr_head;
	g_wd_piwr_ring[h & (WD_PIWR_RING - 1)].site = site;
	g_wd_piwr_ring[h & (WD_PIWR_RING - 1)].piwr = piwr;
	g_wd_piwr_head = h + 1;
#endif
}

#endif
