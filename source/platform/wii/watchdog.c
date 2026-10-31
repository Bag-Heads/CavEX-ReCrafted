/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	Compiled via the flat dispatcher source/platform/watchdog.c under PLATFORM_WII (the
	Wii Makefile globs source/platform but not source/platform/wii -- same pattern as
	wii/gfx.c). See watchdog.h for the design.
*/

#include <gccore.h>           // LWP_*, VIDEO_*, SYS_*, GXRModeObj
#include "../g9_probe.h"
#include <ogc/irq.h>
#include <ogc/lwp_watchdog.h> // gettime(), ticks_to_millisecs()
#include <sys/types.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h> // fopen/fwrite/remove (report write), fprintf (stderr dump)
#include <string.h> // strcmp (FBGUARD vs HANG report routing)

#include "wd_internal.h"
#include "../watchdog.h"
#include "../hw_telemetry.h"
#include "../mem2.h"
#include "../../stack.h" // stack_oom_drops(): realloc-OOM dropped pushes (the far-ring flood crash)
#include "../../lighting.h" // lighting_flood_ceiling_hits(): aborted pathological relights
#include "../../profile.h" // WD_PROFILE_TAG: which budget profile this .dol compiled
#include "../../network/region_archive.h" // region_archive_io_busy(): M1 libfat priority inversion

// The 2026-06-12 audit found FCAVEX_PROFILE_WII_LOW had NEVER been compiled into any
// .dol (the Makefile lacked the -D) -- hardware silently ran the fast-core budgets.
// Stamp the compiled profile into every hang report so that class of drift is visible.
#ifdef FCAVEX_PROFILE_WII_LOW
#define WD_PROFILE_TAG "low"
#else
#define WD_PROFILE_TAG "fast"
#endif
#include "../../chunk_mesher.h" // chunk_mesher_pending(), mesh_stale_discards, g_mesh_truncations

int usleep(useconds_t); // newlib hides it under -std=c99; thread.c forward-declares it too
extern uint32_t g_evict_stuck; // server_local.c (extern-declared here to avoid pulling its deps)
extern uint32_t g_near_evict_deferred; // server_local.c: near unload RPC deferrals
extern uint32_t gfx_vi_enter_count(void); // wii/gfx.c: VI pre-retrace IRQ enter/exit counters
extern uint32_t gfx_vi_exit_count(void);
extern uint32_t gfx_fb_ring_over_count(void); // wii/gfx.c: refused framebuffer-ring pushes
extern uint32_t tchannel_full_count(void); // platform/thread.c: blocking sends that found a full channel
extern uint32_t g_displaylist_malformed; // wii/displaylist.c: lists whose vtxcnt != bytes written
extern uint32_t g_displaylist_badcall;   // wii/displaylist.c: CALLs refused (corrupt list struct)
extern uint32_t gfx_fifo_guard_check(int* leading); // wii/gfx.c: FIFO guard-band scan
extern uint32_t gfx_fb_guard_check(int* which, uint32_t* off); // wii/gfx.c: XFB guard scan
extern void gfx_neighbor_info(uint32_t fb_phys[3], uint32_t fb_size[3], uint32_t* fifo_phys);
extern void gfx_fifo_guard_extent(uint32_t* first, uint32_t* last, uint8_t* val);
extern const uint8_t* gfx_fifo_alloc_base(void); // wii/gfx.c: for the corruption hex window
extern uint32_t gfx_gp_reset_count(void);     // wii/gfx.c: GP resets performed (bounded wait)
// world.c: chunk-list walks abandoned on the bounded-walk guard. Extern-declared rather
// than #include "../../world.h" for the same reason as g_evict_stuck above -- the header
// drags in the whole chunk/dict stack for one counter. size_t, not uint32_t: it is the
// getter's real type and a mismatched declaration would be UB, not a warning.
extern size_t world_ilist_overrun_count(void);
// server_local.c / client_interface.c: the far-slot pool's starvation dials. starve = the
// CURRENT run of consecutive far-batch bails (>= FAR_SLOT_LEAK_BAILS means a slot the
// client never handed back, i.e. far streaming is dead, not merely backpressured); busy =
// the pool's own cumulative refused-acquire count.
extern uint32_t server_local_far_slot_starve(void);
extern uint32_t clin_far_slot_busy_count(void);

// The build stamp the F3 overlay prints (CMakeLists / Makefile bake it in). Reused here so
// a report on the card names the .dol that wrote it -- see wd_dump_identity in watchdog.h.
#ifndef CW_BUILD_ID
#define CW_BUILD_ID "unstamped"
#endif

// VI-callback liveness sampled by the watchdog when a hang fires (two samples ~150ms apart).
static volatile uint32_t g_wd_vi_e0, g_wd_vi_e1, g_wd_vi_x0, g_wd_vi_x1;

// GP-wedge FIFO write-offset ring (see watchdog.h). Written by watchdog_draw_breadcrumb
// on the render thread; read here in wd_emit_dump only after that thread is wedged (hang)
// or stopped (fault), so -- like the volatiles below -- no lock is taken. NOT static: the
// header's inline writes these from main.c / entity.c, so they must have external linkage.
volatile struct wd_piwr_ent g_wd_piwr_ring[WD_PIWR_RING];
volatile uint32_t g_wd_piwr_head = 0;

// Watchdog only READS these; the wedged thread WROTE them before it hung. The phase
// values are naturally atomic 32-bit stores. The 64-bit heartbeat is protected from
// high-priority watchdog preemption by a tiny IRQ-disabled store in watchdog_heartbeat.
static volatile uint64_t g_wd_heartbeat = 0;
static volatile uint32_t g_wd_phase_main = WDM_IDLE;
static volatile uint32_t g_wd_phase_srv = WDS_IDLE;
static volatile uint32_t g_wd_phase_worker = WDW_IDLE;
static volatile uint32_t g_wd_ms_bfs_x10 = 0;
static volatile uint32_t g_wd_ms_light_x10 = 0;
static volatile uint32_t g_wd_ms_lod_x10 = 0;
static volatile uint32_t g_wd_ms_build_x10 = 0;
static volatile uint32_t g_wd_ms_bfs_max_x10 = 0;
static volatile uint32_t g_wd_ms_light_max_x10 = 0;
static volatile uint32_t g_wd_ms_lod_max_x10 = 0;
static volatile uint32_t g_wd_ms_build_max_x10 = 0;
static volatile uint32_t g_wd_hb_gap_max_ms = 0;
// Declared long-operation grace (watchdog_expect_stall_ms): added to the 3000 ms
// HANG threshold until the next heartbeat clears it. Written by the main thread
// under IRQ_Disable like the heartbeat itself; read by the detector thread.
static volatile uint32_t g_wd_stall_grace_ms = 0;
static volatile int32_t g_wd_cx = 0, g_wd_cz = 0;
static volatile int g_wd_fired = 0; // fire-once guard (MANDATORY: else re-fopen churn every 250ms)
static volatile int g_wd_guard_reported = 0; // one-shot: XFB-guard hit reported (no-wedge path)
// C1 (candidate, hardware-unconfirmed): shutdown request, checked by wd_thread_entry
// immediately before and after each of its two unchanged usleep() waits (250ms and
// 150ms) so a shutdown request breaks the loop without altering the wake cadence a
// live game session runs under. Plain volatile int, same treatment as
// g_wd_fired/g_wd_guard_reported above -- single writer (watchdog_shutdown, which
// then blocks in LWP_JoinThread until the thread has observed it and exited), single
// reader (this thread), no ordering requirement beyond "eventually visible".
static volatile int g_wd_stop = 0;
static uint32_t g_wd_loop_seq = 0;
static uint32_t g_wd_main_census = 0;
uint32_t watchdog_loop_seq(void) {
	return __atomic_load_n(&g_wd_loop_seq, __ATOMIC_RELAXED);
}
void watchdog_main_census_phase(uint32_t phase) {
	__atomic_store_n(&g_wd_main_census, phase, __ATOMIC_RELAXED);
}
uint32_t watchdog_main_census_current(void) {
	return __atomic_load_n(&g_wd_main_census, __ATOMIC_RELAXED);
}
// Outcome of the LAST report write (enum wd_sd_status, watchdog.h), painted as its own
// cell-bar row. Written by the emit sequencer's status callback below.
static volatile uint32_t g_wd_sd_status = WD_SD_NONE;
// Per-BOOT nonce for the report's identity line (watchdog.h wd_dump_identity). Seeded in
// watchdog_init, but computed lazily as well: wii/panic.c can fault before init returns.
//
// MECHANISM CORRECTED 2026-07-29 -- the conclusion survives and is stronger. This used to
// justify itself with "the timebase reading at init differs by tens of millions of ticks
// between runs (HBC menu time, SD spin-up, DHCP)", i.e. it assumed the timebase measures
// time since power-on. It does not: libogc seeds it from the RTC, so it is a WALL CLOCK
// (see wd_uptime_ticks). The real argument: the timebase advances at 60,750,000 ticks/s, so
// the low 32 bits taken here wrap every 70.70 s, making the nonce a ~16.5 ns-resolution
// sample of wall-clock time mod 70.7 s. Two boots collide only if their start times differ
// by an exact multiple of 70.70 s to within ~16 ns. Verdict: adequate. Degraded case: on a
// console whose RTC read fails, __SYS_SetBootTime returns early and the timebase really is
// uptime-at-init -- which still varies by tens of millions of ticks between runs, so the
// nonce holds either way.
static volatile uint32_t g_wd_boot_id = 0;
// Timebase reading at watchdog_init, the base for the report's `up=` field. 0 = not seeded
// (a fault before init returned), which wd_uptime_ticks turns into an honest up=0. This is
// NOT redundant with g_wd_boot_id: the nonce only has to DIFFER between boots, the base has
// to be subtractable, and the nonce is deliberately truncated to 32 bits.
static volatile uint64_t g_wd_boot_tb = 0;
static lwp_t g_wd_thread = LWP_THREAD_NULL;
static uint8_t g_wd_stack[64 * 1024] __attribute__((aligned(32))); // static: no alloc post-wedge

// --- cheap per-frame setters (one volatile store each) ---------------------------------
void watchdog_heartbeat(void) {
	uint64_t now = gettime();
	u32 level = IRQ_Disable();
	g_wd_heartbeat = now;
	g_wd_stall_grace_ms = 0; // a declared long operation ends with the frame that ran it
	IRQ_Restore(level);
}

void watchdog_expect_stall_ms(uint32_t ms) {
	u32 level = IRQ_Disable();
	g_wd_stall_grace_ms = ms;
	IRQ_Restore(level);
}
static uint64_t wd_heartbeat_read(void) {
	u32 level = IRQ_Disable();
	uint64_t heartbeat = g_wd_heartbeat;
	IRQ_Restore(level);
	return heartbeat;
}
void watchdog_phase_main(enum wd_phase_main p) {
	g_wd_phase_main = (uint32_t)p;
	thread_stripe_phase(THREAD_STRIPE_MAIN, (uint32_t)p);
}
void watchdog_phase_srv(enum wd_phase_srv p) {
	wds_account((uint32_t)p);
	g_wd_phase_srv = (uint32_t)p;
	thread_stripe_phase(THREAD_STRIPE_SERVER, (uint32_t)p);
}
void watchdog_phase_worker(enum wd_phase_worker p) {
	g_wd_phase_worker = (uint32_t)p;
	thread_stripe_phase(THREAD_STRIPE_MESHER, (uint32_t)p);
}
uint32_t watchdog_phase_main_current(void) { return g_wd_phase_main; }
uint32_t watchdog_phase_srv_current(void) { return g_wd_phase_srv; }
uint32_t watchdog_phase_worker_current(void) { return g_wd_phase_worker; }
uint32_t watchdog_heartbeat_gap_max_ms(void) { return g_wd_hb_gap_max_ms; }
void watchdog_main_timings_x10(uint32_t bfs, uint32_t light, uint32_t lod,
							   uint32_t build, uint32_t bfs_max,
							   uint32_t light_max, uint32_t lod_max,
							   uint32_t build_max) {
	g_wd_ms_bfs_x10 = bfs;
	g_wd_ms_light_x10 = light;
	g_wd_ms_lod_x10 = lod;
	g_wd_ms_build_x10 = build;
	g_wd_ms_bfs_max_x10 = bfs_max;
	g_wd_ms_light_max_x10 = light_max;
	g_wd_ms_lod_max_x10 = lod_max;
	g_wd_ms_build_max_x10 = build_max;
}
void watchdog_coord(int32_t cx, int32_t cz) {
	g_wd_cx = cx;
	g_wd_cz = cz;
}

// Never 0: 0 is the "not seeded yet" sentinel, and a report reading boot=00000000 would
// say nothing at all.
static uint32_t wd_boot_id(void) {
	if(g_wd_boot_id == 0) {
		uint32_t seed = (uint32_t)gettime();
		g_wd_boot_id = seed ? seed : 1u;
	}
	return g_wd_boot_id;
}

static void wd_record_heartbeat_gap(uint64_t now, uint64_t hb) {
	if(now <= hb)
		return;
	uint32_t gap_ms = (uint32_t)ticks_to_millisecs(now - hb);
	if(gap_ms > g_wd_hb_gap_max_ms)
		g_wd_hb_gap_max_ms = gap_ms;
}

static const char* MAIN_NM[] = {"IDLE", "CLIN", "CTICK", "PRERENDER", "LIGHTING", "LOD",
	"BUILD_CHUNKS", "SCRN_UPD", "GFX_FLIP", "MESH_RECV", "RENDER", "GFX_FINISH",
	"GFX_DRAWDONE", "GFX_FBPUSH", "GFX_SEMWAIT",
	// Batch E item 1: mirrors WDM_SHUTDOWN_SERVER/AUDIO/TELEMETRY (watchdog.h).
	// C1 (2026-09-02): SHUT_WD mirrors the appended WDM_SHUTDOWN_WATCHDOG.
	// T-b (2026-09-03): HEARTBEAT mirrors the appended WDM_HEARTBEAT.
	"SHUT_SRV", "SHUT_AUD", "SHUT_TEL", "SHUT_WPAD", "SHUT_WD", "HEARTBEAT",
	// 2026-09-11: mirrors the appended WDM_PACK_RELOAD (watchdog.h).
	"PACK_RELOAD"};
static const char* SRV_NM[] = {"IDLE", "RPC", "SPAWN", "RANDOM_TICK", "WORLD_TICK",
	"EVICT_NEAR", "EVICT_FAR", "NEAR_SCAN", "ENSURE_POP", "CHUNK_SEND",
	"SLOT_WAIT", "RPC_SEND",
	// APPENDED 2026-09-08 (server-loop wedge instrumentation): mirrors the
	// WDS_IDLE_SAMPLE/WDS_IDLE_SLEEP/WDS_FAR_EXTRACT/WDS_FAR_POPULATE/
	// WDS_REGION_IO appended to enum wd_phase_srv (watchdog.h).
	"IDLE_SAMPLE", "IDLE_SLEEP", "FAR_EXTRACT", "FAR_POPULATE", "REGION_IO",
	// APPENDED 2026-09-11: WDS_SAVE (the autosave sweep).
	"SAVE",
	// APPENDED 2026-09-11: the selection halves of the sweep and the near evictor.
	"SAVE_SELECT", "EVICT_SELECT",
	// APPENDED 2026-09-24 (lava-aabb-0924/H1): honest far-select / tail /
	// epilogue phases (watchdog.h).
	"FAR_SELECT", "TAIL", "EPILOGUE"};
static const char* WRK_NM[] = {"IDLE", "RECV", "BUILD", "SEND"};
// watchdog_draw_site (watchdog.h): index = enum value (0=NONE..9=WEATHER).
static const char* SITE_NM[] = {"none", "sky", "world", "screen3D", "particles",
	"entity", "world_t", "clouds", "gui", "weather"};
static const char* nm(const char** t, uint32_t n, uint32_t v) { return v < n ? t[v] : "?"; }
// nm() bounds every lookup by the enum's _MAX, so a table SHORTER than its enum
// reads past the array instead of printing "?" -- a new phase would corrupt the
// one report that names the hang. Pin the two together at compile time.
_Static_assert(sizeof(MAIN_NM) / sizeof(MAIN_NM[0]) == WDM_MAX, "MAIN_NM vs wd_phase_main");
_Static_assert(sizeof(SRV_NM) / sizeof(SRV_NM[0]) == WDS_MAX, "SRV_NM vs wd_phase_srv");
// CLAMP: SITE_NM was one entry short of WDB_WEATHER (missing here until now),
// so nm()'s own bounds check silently rejected the in-range value 9 and every
// weather hang breadcrumb printed "?". Same compile-time pin as MAIN_NM/SRV_NM
// above -- a future WDB_* addition without a matching label now fails the
// build instead of shipping another silent "?".
_Static_assert(sizeof(SITE_NM) / sizeof(SITE_NM[0]) == WDB_MAX, "SITE_NM vs watchdog_draw_site");
_Static_assert(sizeof(WRK_NM) / sizeof(WRK_NM[0]) == WDW_MAX, "WRK_NM vs wd_phase_worker");

// The dump buffer (WD_DUMP_CAP: header + FIFO@rd hex + wedge-pass + fifoguard lines,
// 2026-06-13) and its append/seal helpers now live in the SHARED half of
// platform/watchdog.c: the END-trailer contract that tells a truncated report from a
// whole one only ever fires on a wedged console, so it is kept platform-neutral and
// tested off-device. This file only fills the buffer in (wd_dump_appendf) and seals it.

// Paint a bright PANIC band into the LIVE (frozen) XFB. The frozen image is the XFB VI is
// scanning out (VIDEO_GetCurrentFramebuffer). XFBs are uncached MEM_K0_TO_K1, so writes
// hit RAM immediately (no DCFlush) and VI shows the band within one field even with the
// main thread dead. YUYV 4:2:2: each u32 = two pixels [Y0 U Y1 V]. NO GX, no VI reconfig.
static void wd_paint_banner(void) {
	GXRModeObj* m = VIDEO_GetPreferredMode(NULL);
	uint32_t* fb = (uint32_t*)VIDEO_GetCurrentFramebuffer();
	if(!fb || !m)
		return;
	int fbw = m->fbWidth, fbh = m->xfbHeight, bandH = 72;
	if(bandH > fbh)
		bandH = fbh;
	for(int y = 0; y < bandH; y++)
		for(int x = 0; x < fbw / 2; x++)
			fb[y * (fbw / 2) + x] = 0xC050C0E0u; // bright pink band (Y=C0 U=50 Y=C0 V=E0)
}

// ON-SCREEN diagnostic for a BARE Wii (no USB Gecko, frozen SD): paint the key dump values
// as rows of COUNTABLE white cell-bars directly into the frozen XFB, BELOW the panic banner.
// No GX, no font, no allocation -- uncached XFB writes that VI scans out within one field
// even with the main thread dead (same mechanism as wd_paint_banner). The reader PHOTOGRAPHS
// the frozen TV and COUNTS the cells per row; every 5th cell is a darker MARKER so the eye
// counts in groups of 5. ROW ORDER (top->bottom) and meaning:
//   row 1 = MAIN-loop phase    (0..14; the subsystem the heartbeat died in -- see enum
//                               wd_phase_main: 4=LIGHTING 6=BUILD_CHUNKS 9=MESH_RECV
//                               10=RENDER 12=GFX_DRAWDONE ...). THE key value.
//   row 2 = SERVER-thread phase (0..11; enum wd_phase_srv -- 8=ENSURE_POP 9=CHUNK_SEND
//                               10=SLOT_WAIT 11=RPC_SEND)
//   row 3 = WORKER(mesher) phase (0..3; enum wd_phase_worker)
//   row 4 = MEM2 red-zone CANARY hits (0 = no bounded overrun caught -> double-free class;
//                                      >0 = a stray-writer overrun, the writer is named in
//                                      the report's offset)
//   row 5 = display-list malformed + bad-call count (GX-stream corruption)
//   row 6 = GP reset count (GX GP wedges recovered)
//   row 7 = MEM2 carve SPILL count   (mem2_stat_carve_spill: allocs the 16MB carve could
//                                     not satisfy, served from MEM1 instead)
//   row 8 = MEM2 OOM count           (mem2_stat_oom: allocs that returned NULL outright)
//   row 9 = mesh TRUNCATION count    (g_mesh_truncations: chunk meshes dropped because an
//                                     OOM cut the emit short -- the partial-mesh/GP-wedge
//                                     precursor). Rows 7-9 are the F3 "spill/oom/trunc"
//                                     memory-pressure row; they were text-dump-only until
//                                     2026-07-27, and the panic paint overwrites the live
//                                     F3 overlay, so a photograph alone had lost them.
//   row 10 = SD REPORT status        (enum wd_sd_status, watchdog.h: EMPTY=not attempted,
//                                     1=attempt IN FLIGHT -- seeing this means the SD call
//                                     never returned, so at most a trailer-less partial
//                                     file exists; 2=complete report at the primary path,
//                                     3=complete report at the app-dir fallback,
//                                     4=RED, no complete report anywhere but an INCOMPLETE
//                                     file survives, 5=RED, nothing was created at all --
//                                     this photo is the only evidence)
//   row 11 = chunk-list WALK OVERRUNS (RED; world_ilist_overrun_count(): a render/gpu-busy
//                                     intrusive chain was abandoned mid-walk -- the guard
//                                     that replaced the 2026-07-27 world_build_chunks
//                                     spin. Non-zero is not a warning: each bail
//                                     DELIBERATELY leaks that frame's chunk references
//                                     rather than double-free them, so a rising count is
//                                     the leak. Empty row = the guard never fired and the
//                                     hang is somewhere else)
// APPEND new rows at the BOTTOM, never renumber: a hang report is decoded from the RAW
// cell counts on a photograph, so an existing row's position is part of the record (the
// same rule enum wd_phase_srv carries in watchdog.h).
// Rows 4-9 and 11 are CAPPED at 20 cells: a full 20-cell row means ">= 20", read the text
// dump for the exact figure. Rows 1-3 are capped at their enum's _MAX and never saturate.
// A row with ZERO cells = that value is 0 (e.g. an empty row 4 = canary 0 = the double-free
// class, which steers the fix per the synthesis Part B).
// FITS 480: the last row's strip ends at y = WD_DIAG_Y0 + 10*WD_DIAG_DY + cellH + 2 = 388,
// inside the 480-line xfbHeight of every standard mode (wd_fill clips, but a clipped row
// carries no information -- assert the budget when adding rows).
#define WD_DIAG_Y0 84 // first row's top, clear of the 72-line panic banner
#define WD_DIAG_DY 28
#define WD_DIAG_ROWS 11 // keep in step with the wd_bar calls in wd_paint_diag
#define WD_DIAG_CELL_H 22
// wd_fill CLIPS, so an off-screen row costs nothing -- and shows nothing. Enforce the
// budget instead of asserting it in prose: adding rows past this must move the layout,
// not silently paint into the void.
_Static_assert(WD_DIAG_Y0 + (WD_DIAG_ROWS - 1) * WD_DIAG_DY + WD_DIAG_CELL_H + 2 <= 480,
			   "watchdog diag rows run off a 480-line framebuffer");
static void wd_fill(uint32_t* fb, int w2, int fbh, int ux, int uy, int uw, int uh,
					uint32_t c) {
	for(int y = uy; y < uy + uh && y < fbh && y >= 0; y++)
		for(int x = ux; x < ux + uw && x < w2 && x >= 0; x++)
			fb[y * w2 + x] = c;
}
static void wd_bar_c(uint32_t* fb, int w2, int fbh, int uy, unsigned value,
					 unsigned cap, uint32_t cell, uint32_t mark) {
	const uint32_t BG = 0x10801080u;
	const int cellW = 7, gap = 2, x0 = 4; // u32 cols (each = 2 screen px)
	if(value > cap)
		value = cap;
	wd_fill(fb, w2, fbh, 0, uy - 2, w2, WD_DIAG_CELL_H + 4, BG); // dark strip for contrast
	for(unsigned i = 0; i < value; i++)
		wd_fill(fb, w2, fbh, x0 + (int)i * (cellW + gap), uy, cellW,
				WD_DIAG_CELL_H, ((i + 1) % 5 == 0) ? mark : cell);
}
static void wd_bar(uint32_t* fb, int w2, int fbh, int uy, unsigned value, unsigned cap) {
	wd_bar_c(fb, w2, fbh, uy, value, cap, 0xEB80EB80u, 0x9F809F80u); // white / grey marker
}
// RED cells: for a row whose non-zero value is itself a FAILURE, so a photograph
// reads the alert before anyone counts cells.
static void wd_bar_alert(uint32_t* fb, int w2, int fbh, int uy, unsigned value,
						 unsigned cap) {
	wd_bar_c(fb, w2, fbh, uy, value, cap, 0x515A51F0u, 0x3F5A3FD0u);
}
/* The SD write can stall after the bars are painted. Preserve the fault's
 * registers on the XFB too: no font texture, heap, FILE lock or GX calls.
 * Three hexadecimal columns, read left to right:
 *   y=396: cause (1 hang, 2 CPU fault, 3 guard), PC, LR
 *   y=418: VI enter before, VI enter after, framebuffer ring overruns
 *   y=440: VI exit before, VI exit after, SD report status
 * Existing countable rows keep their positions and meanings. */
static uint32_t wd_screen_cause, wd_screen_pc, wd_screen_lr;
static void wd_hex_word(uint32_t* fb, int w2, int fbh, int x, int y,
	uint32_t word) {
	static const uint16_t glyph[16] = {
		0x7b6f, 0x2492, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249,
		0x7bef, 0x7bcf, 0x7bed, 0x6bae, 0x7927, 0x6b6e, 0x79e7, 0x79e4};
	for(unsigned digit = 0; digit < 8; digit++) {
		uint16_t bits = glyph[(word >> (28 - 4 * digit)) & 15];
		for(unsigned row = 0; row < 5; row++)
			for(unsigned col = 0; col < 3; col++)
				if(bits & (1u << (14 - 3 * row - col)))
					wd_fill(fb, w2, fbh, x + digit * 8 + col * 2,
						y + row * 3, 2, 3, 0xEB80EB80u);
	}
}

static void wd_paint_diag(void) {
	GXRModeObj* m = VIDEO_GetPreferredMode(NULL);
	uint32_t* fb = (uint32_t*)VIDEO_GetCurrentFramebuffer();
	if(!fb || !m)
		return;
	int w2 = m->fbWidth / 2, fbh = m->xfbHeight;
	void* cp;
	size_t cs, co;
	(void)cp;
	(void)cs;
	(void)co;
	mem2_canary_first(&cp, &cs, &co);
	int y = WD_DIAG_Y0, dy = WD_DIAG_DY;
	wd_bar(fb, w2, fbh, y, g_wd_phase_main, WDM_MAX);
	y += dy;
	wd_bar(fb, w2, fbh, y, g_wd_phase_srv, WDS_MAX);
	y += dy;
	wd_bar(fb, w2, fbh, y, g_wd_phase_worker, WDW_MAX);
	y += dy;
	wd_bar(fb, w2, fbh, y, (unsigned)mem2_canary_hits(), 20);
	y += dy;
	wd_bar(fb, w2, fbh, y,
		   (unsigned)(g_displaylist_malformed + g_displaylist_badcall), 20);
	y += dy;
	wd_bar(fb, w2, fbh, y, (unsigned)gfx_gp_reset_count(), 20);
	y += dy;
	wd_bar(fb, w2, fbh, y, (unsigned)mem2_stat_carve_spill(), 20);
	y += dy;
	wd_bar(fb, w2, fbh, y, (unsigned)mem2_stat_oom(), 20);
	y += dy;
	// CR-08: cross-thread read (mesher-worker writer) of a plain uint32_t --
	// __atomic_load_n, safe here too: it lowers to a single ordinary load on
	// PPC32, so it works from the panic/fault path the same as a bare read
	// would have, just with defined semantics instead of a data race.
	wd_bar(fb, w2, fbh, y,
		   (unsigned)__atomic_load_n(&g_mesh_truncations, __ATOMIC_RELAXED), 20);
	y += dy;
	// RED once no COMPLETE report exists anywhere (PARTIAL / FAILED); a written report
	// paints the ordinary white 2 (primary) / 3 (fallback) cells, and the 1-cell TRYING
	// state left by a writer that never returned stays white because at that moment
	// nothing has failed yet -- it just never finished.
	if(g_wd_sd_status >= WD_SD_PARTIAL)
		wd_bar_alert(fb, w2, fbh, y, g_wd_sd_status, WD_SD_FAILED);
	else
		wd_bar(fb, w2, fbh, y, g_wd_sd_status, WD_SD_FAILED);
	y += dy;
	// ALERT colour: any non-zero value here is a chunk-reference leak the guard took on
	// purpose in place of a double free, so a photograph should read it before anyone
	// starts counting cells.
	wd_bar_alert(fb, w2, fbh, y, (unsigned)world_ilist_overrun_count(), 20);
	const uint32_t words[3][3] = {
		{wd_screen_cause, wd_screen_pc, wd_screen_lr},
		{g_wd_vi_e0, g_wd_vi_e1, gfx_fb_ring_over_count()},
		{g_wd_vi_x0, g_wd_vi_x1, g_wd_sd_status}};
	for(unsigned row = 0; row < 3; row++) {
		int top = 396 + 22 * row;
		wd_fill(fb, w2, fbh, 0, top - 2, w2, 19, 0x10801080u);
		for(unsigned col = 0; col < 3; col++)
			wd_hex_word(fb, w2, fbh, 8 + col * 100, top, words[row][col]);
	}
}

// FALLBACK report paths (2026-07-27): the app directory on the same card. A root
// open can fail on its own -- a full FAT root directory, or a root-only write error --
// while a subdirectory open still succeeds, and the stage server already fetches from
// this directory (it is where the .dol and cw_run_args.txt are staged). The strings
// themselves live in hw_telemetry.h beside the primaries so the writer here, the crash-
// mailbox replay and the stage-server fetch cannot drift apart -- a fallback that only
// the writer knows about is a file nobody ever collects.
#define WD_HANG_REPORT_FALLBACK_PATH HW_TELEMETRY_HANG_REPORT_FALLBACK_PATH
#define WD_FBGUARD_REPORT_FALLBACK_PATH HW_TELEMETRY_FBGUARD_REPORT_FALLBACK_PATH
#define WD_SD_ATTEMPTS 3 // per path: rides out a transient libfat error, still bounded
// M1 (round-5 fix round): PRIORITY INVERSION ON THE LIBFAT PARTITION LOCK.
// This thread runs at prio 100 -- above main/server (64) and above the region
// I/O helper (48) -- but libogc mutexes do NOT implement priority inheritance,
// so a fopen() issued while the helper is inside a publication queues behind a
// prio-48 thread blocked in IOS and inherits its wait. The OBSERVED hardware
// failure this file already documents IS a wedged fopen (wd_write_report /
// wd_emit_dump), i.e. exactly this shape, and a fault report that never
// returns is a fault report that is lost.
//
// So the write is BOUNDED: while the helper is publishing, wait in 10 ms
// usleep() slices (which, at prio 100, is what actually lets the prio-48
// helper reach the CPU and finish) for at most WD_REGION_IO_DEFER_MS, then
// SKIP the SD write entirely rather than enter a lock we cannot leave. One
// publication is a handful of SD writes plus three fsyncs, so 300 ms is a
// generous multiple of the measured worst case and still under the 3 s
// hang threshold this thread itself enforces. The dump SAYS which happened:
// wd_emit_dump stamps a `regionio=` line before sealing.
#define WD_REGION_IO_DEFER_MS 300u

// Write the sealed dump[0..n) to `path`. A short fwrite or a failing fflush/fclose leaves
// a TRUNCATED file, which reads as a report and is worse than none, so it counts as a
// failure and retries (the "w" reopen truncates, so a retry never appends to the partial
// attempt). What the caller gets back is what is ON THE CARD at `path` afterwards, which
// is what the status row has to agree with:
//   WD_WRITE_OK      the whole sealed report, END trailer and all
//   WD_WRITE_NONE    nothing -- either fopen never succeeded, or the partial file left by
//                    a failed attempt was successfully removed
//   WD_WRITE_PARTIAL an incomplete file survives (remove() refused too). It cannot carry
//                    the trailer, so a reader can still tell, but the status row must say
//                    a file is there.
// The remove() is why the OK/NONE cases can be trusted: fopen("w") destroys any earlier
// report the moment it succeeds, so "the write failed" would otherwise silently mean
// "there is now a stub where a good report used to be".
static enum wd_write_result wd_write_report(const char* path, size_t n) {
	bool created = false;
	// M1: never call fopen() into a partition lock the prio-48 region I/O
	// helper is holding. Bounded wait, then give up on the SD sink -- the
	// telemetry stream (emitted BEFORE this, see wd_emit_dump) and the cell
	// bars are what carry the report in that case.
	for(unsigned waited = 0;
		region_archive_io_busy() && waited < WD_REGION_IO_DEFER_MS;
		waited += 10)
		usleep(10 * 1000);
	if(region_archive_io_busy())
		return WD_WRITE_NONE;
	for(int attempt = 0; attempt < WD_SD_ATTEMPTS; attempt++) {
		FILE* f = fopen(path, "w");
		if(!f)
			continue;
		created = true; // "w" truncated/created it: something is on the card from here on
		size_t wrote = fwrite(wd_dump_buf(), 1, n, f);
		int flushed = fflush(f);
		int closed = fclose(f);
		if(wrote == n && flushed == 0 && closed == 0)
			return WD_WRITE_OK;
	}
	if(!created) {
		// RETIRE A STALE REPORT (2026-07-29). fopen never succeeded, so nothing was
		// written THIS boot -- but a complete, sealed report from an EARLIER boot can
		// still be sitting at `path`, and WD_WRITE_NONE (and the FAILED status row it
		// feeds) claims the path is empty. Try to make that claim true. remove() failing
		// with ENOENT means there was nothing there to begin with, which is the same
		// outcome; if the remove itself is refused the stale file survives, and the
		// report's build/boot identity line is then the only thing that tells it from
		// this boot's -- which is exactly why wd_emit_dump stamps one.
		int removed = remove(path);
		int remove_error = errno;
		return wd_write_cleanup_result(removed, remove_error);
	}
	int removed = remove(path);
	int remove_error = errno;
	return wd_write_cleanup_result(removed, remove_error);
}

// --- emit hooks: the Wii half of the shared wd_emit_sequence contract ------------------
struct wd_emit_ctx {
	int fatal; // selects the HANG vs FBGUARD path pair (the sequencer's own `fatal`
			   // decides only whether to paint)
	// This report's per-boot sequence, for the seq-suffixed HANG paths (watchdog.h
	// wd_report_path_for_seq). Captured in wd_emit_dump AFTER wd_dump_identity has
	// advanced it, so it is THIS report's number, not the previous one's.
	uint32_t seq;
	// Scratch for the built path. Lives in the ctx rather than a static because the ctx
	// is already the per-emit state; either is safe (single writer -- the watchdog thread
	// on the hang path, the faulting thread on the panic path, never both), but a member
	// keeps the lifetime obvious. Primary and fallback each get their own so a fallback
	// build cannot clobber the primary path mid-sequence.
	char primary_path[WD_REPORT_PATH_MAX];
	char fallback_path[WD_REPORT_PATH_MAX];
};
static void wd_emit_status_cb(void* ctx, enum wd_sd_status s) {
	(void)ctx;
	g_wd_sd_status = (uint32_t)s;
}
static void wd_emit_paint_cb(void* ctx) {
	(void)ctx;
	wd_paint_banner();
	wd_paint_diag(); // ON-SCREEN cell-bar diagnostic (bare Wii, no Gecko: photograph + count)
}
static void wd_emit_repaint_cb(void* ctx) {
	(void)ctx;
	// Diag ONLY: the banner is already up and wd_paint_diag is idempotent -- it rewrites
	// its own strips, so re-running it just replaces the TRYING row with the outcome.
	wd_paint_diag();
}
// Only the HANG paths are sequenced; the FBGUARD report is one-shot per boot
// (g_wd_guard_reported) so it cannot overwrite itself and keeps its fixed name. See the
// wd_report_path_for_seq note in watchdog.h.
static enum wd_write_result wd_emit_write_primary_cb(void* ctx, size_t n) {
	struct wd_emit_ctx* c = (struct wd_emit_ctx*)ctx;
	if(!c->fatal)
		return wd_write_report(HW_TELEMETRY_FBGUARD_REPORT_PATH, n);
	return wd_write_report(wd_report_path_for_seq(c->primary_path,
												  sizeof(c->primary_path),
												  HW_TELEMETRY_HANG_REPORT_PATH, c->seq),
						   n);
}
static enum wd_write_result wd_emit_write_fallback_cb(void* ctx, size_t n) {
	struct wd_emit_ctx* c = (struct wd_emit_ctx*)ctx;
	if(!c->fatal)
		return wd_write_report(WD_FBGUARD_REPORT_FALLBACK_PATH, n);
	return wd_write_report(wd_report_path_for_seq(c->fallback_path,
												  sizeof(c->fallback_path),
												  WD_HANG_REPORT_FALLBACK_PATH, c->seq),
						   n);
}

void wd_emit_dump(const char* cause, uint32_t srr0, uint32_t srr1, uint32_t lr,
				  const uint32_t* gpr) {
	/* HANG is emitted from the watchdog LWP, never exception context. Publish
	 * this tiny best-effort breadcrumb before any dump helper or SD access. */
	if(!gpr && strcmp(cause, "HANG") == 0) {
		char line[160];
		snprintf(line, sizeof(line),
			"{\"event\":\"watchdog_start\",\"build_id\":\"%s\",\"main\":%u,\"srv\":%u,\"worker\":%u}",
			CW_BUILD_ID, (unsigned)g_wd_phase_main,
			(unsigned)g_wd_phase_srv, (unsigned)g_wd_phase_worker);
		(void)hw_telemetry_emit_watchdog_start(line);
	}
	wd_screen_cause = gpr ? 2 : strcmp(cause, "HANG") == 0 ? 1 : 3;
	wd_screen_pc = srr0;
	wd_screen_lr = lr;
	size_t n = 0;
	wd_dump_reset();
	n = wd_dump_appendf(n, "CAVEWII PANIC: %s prof=%s\n", cause, WD_PROFILE_TAG);
	// SECOND LINE, before anything that can be cut short: which .dol and which BOOT wrote
	// this file. Without it a complete report left at sd:/hang_report.txt by an earlier
	// boot is byte-for-byte as convincing as the one this boot did NOT manage to write --
	// the status row says FAILED, the card holds a whole report, and the reader has no way
	// to reconcile them. See wd_dump_identity in watchdog.h.
	n = wd_dump_identity(
		n, CW_BUILD_ID, wd_boot_id(),
		(uint32_t)ticks_to_millisecs(wd_uptime_ticks(g_wd_boot_tb, gettime())));
	{
		struct thread_stripe_breadcrumb stripe;
		if(thread_stripe_breadcrumb_get(&stripe)) {
			/* Correlation for H1 only: a holder and server phase do not prove
			 * which thread caused the stall. A transition/unstable snapshot is
			 * explicitly uncertain rather than an ownerless assertion. */
			const char* holder = stripe.holder_tag == THREAD_STRIPE_MAIN ? "main"
				: stripe.holder_tag == THREAD_STRIPE_SERVER ? "server"
				: stripe.holder_tag == THREAD_STRIPE_MESHER ? "mesher" : "other";
			const char* phase = stripe.holder_tag == THREAD_STRIPE_MAIN
				? nm(MAIN_NM, WDM_MAX, stripe.holder_phase)
				: stripe.holder_tag == THREAD_STRIPE_SERVER
				? nm(SRV_NM, WDS_MAX, stripe.holder_phase)
				: stripe.holder_tag == THREAD_STRIPE_MESHER
				? nm(WRK_NM, WDW_MAX, stripe.holder_phase) : "?";
			const char* state = stripe.holder_state == THREAD_STRIPE_ACQUIRING ? "acquiring"
				: stripe.holder_state == THREAD_STRIPE_HELD ? "held"
				: stripe.holder_state == THREAD_STRIPE_RELEASING ? "releasing" : "idle";
			n = wd_dump_appendf(n,
				"stripe_wait_hint stripe=%u holder=%s holder_id=%08x phase_hint=%s(%u) state=%s wait_us=%u active=%u uncertain=%u (H1 support only; causality unproven)\n",
				(unsigned)stripe.stripe, holder, (unsigned)stripe.holder_id,
				phase, (unsigned)stripe.holder_phase, state,
				(unsigned)stripe.wait_us, (unsigned)stripe.active,
				(unsigned)stripe.uncertain);
		}
	}
	// POLARITY IS MIXED ON THIS LINE -- read this before treating any number as a symptom.
	//   FAULT dials (0 = healthy): trunc, estk, spill, oom, dlbad, gpreset, soom, lfceil,
	//     ilist, farbail.
	//   THROUGHPUT dials (non-zero EXPECTED): stale, nevdef.
	// The 2026-07-29 report's stale=245 / nevdef=10 were read as symptoms; both were
	// healthy. Judging them needs the polarity, so it lives here beside the only sink.
	//
	// stale = mesh results discarded because the chunk was re-dirtied while its mesh was in
	//   flight. Nothing leaks -- the display lists are destroyed, the chunk re-meshes next
	//   frame. Judge as a RATIO against the build counts, never as an absolute (see the note
	//   at mesh_stale_discards in chunk_mesher.c). A long piston/TNT/redstone session
	//   produces it by design.
	// nevdef = near-evict UNLOAD RPCs deferred into the server's pending queue because the
	//   client's token pool was momentarily empty, i.e. the server took the NON-BLOCKING
	//   path. It is an EDGE counter, not a level, and that inverts how it reads:
	//   the eviction loop only runs when the pending queue already DRAINED
	//   (near_unloads_flushed) and BREAKS on the first deferral, so a CHRONIC client-inbox
	//   stall makes nevdef FREEZE, not climb. **A FLAT nevdef DOES NOT MEAN HEALTHY.** The
	//   level indicator is pending_near_unload_count, which is surfaced nowhere today.
	//
	// ilist = bounded chunk-list walks ABANDONED (world.c's corrupt-chain guard, the
	// replacement for the 2026-07-27 world_build_chunks spin); each one deliberately
	// leaked that frame's chunk references instead of double-freeing them, so non-zero
	// names a real leak. farbail/farbusy = the far-slot pool's consecutive-bail run and
	// its cumulative refused acquires: a run at or past FAR_SLOT_LEAK_BAILS means the
	// client stopped releasing far slots and far streaming is dead, not backpressured.
	n = wd_dump_appendf(n,
		"main=%s srv=%s worker=%s chunk=(%ld,%ld)\n"
		"q=%d stale=%u trunc=%u estk=%u nevdef=%u spill=%u oom=%u dlbad=%u dlcall=%u gpreset=%u soom=%u lfceil=%u ilist=%u farbail=%u farbusy=%u\nMEM1=%dK MEM2=%dK\n",
		nm(MAIN_NM, WDM_MAX, g_wd_phase_main), nm(SRV_NM, WDS_MAX, g_wd_phase_srv),
		nm(WRK_NM, WDW_MAX, g_wd_phase_worker), (long)g_wd_cx, (long)g_wd_cz,
		// CR-08: all four are plain uint32_t written on another thread
		// (server or mesher worker) -- __atomic_load_n at this reader, safe
		// on the panic/fault path for the same reason noted at the
		// g_mesh_truncations cell-bar read above.
		chunk_mesher_pending(),
		(unsigned)__atomic_load_n(&mesh_stale_discards, __ATOMIC_RELAXED),
		(unsigned)__atomic_load_n(&g_mesh_truncations, __ATOMIC_RELAXED),
		(unsigned)__atomic_load_n(&g_evict_stuck, __ATOMIC_RELAXED),
		(unsigned)__atomic_load_n(&g_near_evict_deferred, __ATOMIC_RELAXED),
		(unsigned)mem2_stat_carve_spill(), (unsigned)mem2_stat_oom(),
		(unsigned)g_displaylist_malformed, (unsigned)g_displaylist_badcall,
		(unsigned)gfx_gp_reset_count(), (unsigned)stack_oom_drops(),
		(unsigned)lighting_flood_ceiling_hits(),
		(unsigned)world_ilist_overrun_count(),
		(unsigned)server_local_far_slot_starve(),
		(unsigned)clin_far_slot_busy_count(),
		(int)(SYS_GetArena1Size() / 1024), (int)(SYS_GetArena2Size() / 1024));
	n = wd_dump_appendf(n,
		"main_ms_x10 bf=%u li=%u lo=%u bd=%u maxbf=%u maxli=%u maxlo=%u maxbd=%u\n",
		(unsigned)g_wd_ms_bfs_x10, (unsigned)g_wd_ms_light_x10,
		(unsigned)g_wd_ms_lod_x10, (unsigned)g_wd_ms_build_x10,
		(unsigned)g_wd_ms_bfs_max_x10, (unsigned)g_wd_ms_light_max_x10,
		(unsigned)g_wd_ms_lod_max_x10, (unsigned)g_wd_ms_build_max_x10);
	n = wd_dump_appendf(n,
		"heartbeat_gap_max_ms=%u\n", (unsigned)g_wd_hb_gap_max_ms);
	// mem2 red-zone CANARY: non-zero NAMES the stray-writer heap-corruption class
	// at the hang -- first offender's block base + alloc size + corrupt guard
	// offset (a fixed offset across runs fingerprints the writer's stride).
	{
		void* cp;
		size_t cs, co;
		mem2_canary_first(&cp, &cs, &co);
		n = wd_dump_appendf(n,
			"canary hits=%u first=%p size=%u off=%u\n",
			(unsigned)mem2_canary_hits(), cp, (unsigned)cs, (unsigned)co);
	}
	// VI-callback liveness across the ~150ms sample: enter advances + exit advances =>
	// IRQ fine, so a flip hang is in the handoff accounting (since the SPSC-ring+semaphore
	// rework the callback has NO blocking call, so enter-advances+exit-frozen should be
	// impossible); both frozen => VI IRQ stopped. (0->0 on the fault path: not sampled.)
	// fbover/tokfull read directly at emit time (plain volatile u32 reads -- valid
	// on the fault path too, where the 150ms VIcb sampling never ran): nonzero
	// fbover = the framebuffer handoff ring refused a push (wii/gfx.c); nonzero
	// tokfull = a "blocking" tchannel_send found its channel full, i.e. a
	// token-conservation regression in the mesher/clin/svin channels -- the send
	// then spin-waited instead of dropping (platform/thread.c), which can present
	// as exactly this kind of hang.
	n = wd_dump_appendf(n,
		"VIcb enter %u->%u exit %u->%u fbover=%u tokfull=%u\n", (unsigned)g_wd_vi_e0,
		(unsigned)g_wd_vi_e1, (unsigned)g_wd_vi_x0, (unsigned)g_wd_vi_x1,
		(unsigned)gfx_fb_ring_over_count(), (unsigned)tchannel_full_count());
	// GP wedge introspection (meaningful when main=GFX_DRAWDONE): cmdIdle=0 => GP still
	// chewing a command = STUCK on a bad draw (corrupt display list / descriptor desync);
	// cmdIdle=1+rdIdle=1 => GP idle so the wedge is the drawdone token, not the GP.
	// over=1 ALONE IS AMBIGUOUS (2026-06-12 audit): it cannot distinguish a live >=hi-
	// watermark backlog (CPU overflow-suspended) from a latch left by a past, serviced
	// overflow -- the CP FIFO line below disambiguates. Read-only register peek.
	{
		u8 gp_over = 0, gp_under = 0, gp_rdidle = 0, gp_cmdidle = 0, gp_brk = 0;
		GX_GetGPStatus(&gp_over, &gp_under, &gp_rdidle, &gp_cmdidle, &gp_brk);
		n = wd_dump_appendf(n,
			"GP over=%d under=%d rdIdle=%d cmdIdle=%d brk=%d\n", gp_over, gp_under,
			gp_rdidle, gp_cmdidle, gp_brk);
	}
	// CP/PI FIFO registers -- the over=1 disambiguator AND the breadcrumb-trust gate:
	// dist ~0  => stream fully drained; over was a stale latch; the LAST breadcrumb
	//             site really is the wedge neighborhood.
	// dist big => a backlog is queued BEHIND the wedge; the wedging command sits at the
	//             GP READ pointer (rd), up to FIFO-size bytes EARLIER in the frame than
	//             the breadcrumb site (which only tracks the CPU's write side, piwr).
	// libogc keeps _cpReg/_piReg static, so address the hardware directly. CP regs are
	// u16 halves (lo at the lower index), PI write pointer is u32 (top bits = wrap
	// flag, masked); ovc = count of hi-watermark overflow IRQs serviced this session.
	// On the HANG path the pair reads are stable (the wedged GP freezes rd/dist and
	// main -- the only FIFO writer -- is parked). On the FAULT path the GP may still
	// be DRAINING (a CPU exception does not halt GX DMA) and a >64KB FIFO means the
	// hi half is live, so compose each pair with a BOUNDED read-until-stable instead
	// of trusting one read (a straddled 64K boundary would mis-report by 65536).
	{
		volatile u16* cp = (volatile u16*)0xCC000000;
		volatile u32* pi = (volatile u32*)0xCC003000;
		u32 cp_dist = 0, cp_rd = 0;
		for(int t = 0; t < 4; t++) {
			u32 d0 = ((u32)cp[25] << 16) | cp[24]; // FIFO rd-wt distance (0x30/0x32)
			u32 r0 = ((u32)cp[29] << 16) | cp[28]; // GP read pointer    (0x38/0x3A)
			cp_dist = ((u32)cp[25] << 16) | cp[24];
			cp_rd = ((u32)cp[29] << 16) | cp[28];
			if(d0 == cp_dist && r0 == cp_rd)
				break;
		}
		u32 pi_wr = pi[5]; // CPU write pointer (0xCC003014): single u32, atomic
		n = wd_dump_appendf(n,
			"CP dist=%u rd=0x%08x piwr=0x%08x ovc=%u\n", (unsigned)cp_dist,
			(unsigned)cp_rd, (unsigned)(pi_wr & 0x1FFFFFFF),
			(unsigned)GX_GetOverflowCount());

		// FIFO WINDOW AT rd (2026-06-13 GP-wedge instrument): on a cmdIdle=0
		// wedge the byte the GP choked on is AT rd, so a hex window straddling
		// it NAMES the wedging opcode -- the decisive datum the (stale, CPU-side)
		// breadcrumb cannot give at full-FIFO depth. Read from the GP's own
		// UNCACHED (K1) view; rd is a physical address into the fifoBuffer
		// (gfx_fifo_size() bytes, 2MB since 2026-06-13),
		// mapped to K1 by OR-ing 0xC0000000. Stable on the hang path (the wedged
		// GP froze rd; main is parked). The opcode at the '>' marker decides the
		// branch: 0x40 = GX_CallDispList -> the wedge is INSIDE a called display
		// list (far-ring LOD = rank-1 suspect); 0x80|vtxfmt(0..4) = an immediate
		// GX_Begin draw -> a descriptor/vtxfmt desync (rank 2). Plausibility-
		// gated so a garbage rd never faults the dump.
		if(cp_rd >= 0x100 && cp_rd < 0x02000000) {
			volatile uint8_t* fb
				= (volatile uint8_t*)(0xC0000000u | (cp_rd & 0x1FFFFFFFu));
			n = wd_dump_appendf(n, "FIFO@rd[-8..+23]:");
			for(int i = -8; i < 24 && n < WD_DUMP_BODY_MAX - 8; i++)
				n = wd_dump_appendf(n, "%s%02x",
							  (i == 0) ? " >" : " ", (unsigned)fb[i]);
			n = wd_dump_appendf(n, "\n");
		}

		// WEDGE-PASS bracketing (2026-06-13): map the frozen GP read pointer (rd)
		// back to the draw SITE that wrote the bytes now AT rd. Each breadcrumb
		// snapshotted the FIFO write offset (piwr) at site entry into g_wd_piwr_ring;
		// consecutive entries delimit a pass's [piwr_start, piwr_end) byte-range (the
		// newest pass ends at the current write head, pi_wr). rd and piwr share the
		// fifoBuffer's physical coordinate space (gx.c:364/373), so the pass whose
		// range brackets rd NAMES the wedging draw -- the datum the CPU-side (write-
		// head) site breadcrumb alone cannot give once the FIFO has lapped past it.
		// MULTI-LAP CAVEAT (the 2026-06-13 hang: dist=full FIFO, ovc large): under chronic
		// FIFO saturation a single frame writes more than one full lap, so the SAME offset
		// is written by several passes and more than one range legitimately brackets
		// rd. We do NOT assert a unique pass -- we report EVERY match and flag the
		// ambiguity (>1 match, a single pass that itself spanned >=1 lap, or full FIFO).
		{
			extern uint32_t gfx_fifo_size(void); // wii/gfx.c: the LIVE FIFO_SIZE
			const uint32_t FIFO_SZ = gfx_fifo_size(); // was hardcoded 256K after the 2MB grow --
													  // every wrap-span/lap figure below was wrong
			uint32_t rd_off = cp_rd & 0x1FFFFFE0u;  // into the ring's masked space
			uint32_t pi_off = pi_wr & 0x1FFFFFE0u;  // newest pass end = write head now
			uint32_t head = g_wd_piwr_head;         // writer parked => stable; read once
			uint32_t cnt = head < WD_PIWR_RING ? head : WD_PIWR_RING;
			uint32_t total_adv = 0; // bytes written across the recorded window; >=1 lap
									// => an offset is reused => no pass is unique
			int matches = 0;
			n = wd_dump_appendf(n, "wedge pass=");
			for(uint32_t k = 0; k < cnt && n < WD_DUMP_BODY_MAX - 24; k++) {
				uint32_t idx = (head - cnt + k) & (WD_PIWR_RING - 1);
				uint32_t a = g_wd_piwr_ring[idx].piwr;
				uint32_t b = (k + 1 < cnt)
					? g_wd_piwr_ring[(head - cnt + k + 1) & (WD_PIWR_RING - 1)].piwr
					: pi_off; // last recorded pass runs up to the current write head
				// [a,b) membership with ONE FIFO wrap (b<a => the range crossed the top).
				// Both a and b are offsets within the one FIFO_SZ ring, so this single-wrap
				// span is always < FIFO_SZ -- a SINGLE pass that itself lapped >1x cannot
				// be seen from its two endpoints (the residual blind spot below).
				int hit = (a <= b) ? (rd_off >= a && rd_off < b)
								   : (rd_off >= a || rd_off < b);
				total_adv += (a <= b) ? (b - a) : (b + FIFO_SZ - a);
				if(hit) {
					n = wd_dump_appendf(n, "%s%s",
						matches ? "," : "", nm(SITE_NM, WDB_MAX, g_wd_piwr_ring[idx].site));
					matches++;
				}
			}
			if(matches == 0)
				n = wd_dump_appendf(n,
					cnt ? "?(rd predates ring)" : "?(empty)");
			// AMBIGUOUS if >1 pass brackets rd, OR the recorded window itself wrote >=1
			// full lap, OR the FIFO is at/over full -- in each case the same physical
			// offset was (re)written across laps, so no single pass is authoritative.
			// (A pass that lapped invisibly BETWEEN two breadcrumbs is the residual blind
			// spot two endpoints cannot reveal -- the fundamental limit, not a bug.)
			if(matches > 1 || total_adv >= FIFO_SZ || cp_dist >= FIFO_SZ)
				n = wd_dump_appendf(n, " [multi-lap:ambiguous]");
			n = wd_dump_appendf(n,
				" (rd_off=0x%05x head=%u n=%u wrote=%uK)\n", (unsigned)rd_off,
				(unsigned)head, (unsigned)cnt, (unsigned)(total_adv >> 10));
		}
	}
	// DRAW BREADCRUMB (the 2026-06-11 wedge instrument): the last draw SITE the
	// render thread entered this frame (watchdog.h enum: 1=sky 2=world 3=screen3D
	// 4=particles 5=entity 6=world-translucent 7=clouds 8=gui), with the entity
	// id + (type<<8|monster) for the entity pass -- on a cmdIdle=0 hang this NAMES
	// the draw neighborhood that wedged the GP.
	n = wd_dump_appendf(n,
		"last draw site=%u id=%u aux=0x%x\n", (unsigned)g_wd_draw_site,
		(unsigned)g_wd_draw_id, (unsigned)g_wd_draw_aux);
	// FIFO GUARD BANDS (2026-06-13): non-zero = a stray MEM1 write overran a
	// neighbor INTO the GX FIFO region (the canary-blind GP-wedge suspect). off
	// = first corrupt byte (1-based) into the named band; lead=1 leading band
	// (forward overrun from before the FIFO), 0 trailing.
	{
		int lead = 0;
		uint32_t goff = gfx_fifo_guard_check(&lead);
		n = wd_dump_appendf(n, "fifoguard=%u band=%s\n",
			(unsigned)goff, goff ? (lead ? "leading" : "trailing") : "intact");
		// VALUE + EXTENT of leading-guard corruption (2026-06-13): names the
		// writer. val high byte 0x80..0x9f + range ~[1..16] = stale dlmalloc
		// free-list pointers written back from a dirty cache line (the cache-
		// coherency artifact, harmless -- below buf_start); float/vertex bytes or
		// a long run = a live stray store. Hex windows: glead = the corrupt
		// values at fifoAlloc[0..16); ggap = fifoAlloc[-16..0) (what's below).
		uint32_t gf = 0, gl = 0;
		uint8_t gv = 0;
		gfx_fifo_guard_extent(&gf, &gl, &gv);
		const uint8_t* fb = gfx_fifo_alloc_base();
		n = wd_dump_appendf(n,
			"  gext=[%u..%u] gval=0x%02x", (unsigned)gf, (unsigned)gl,
			(unsigned)gv);
		if(fb) {
			n = wd_dump_appendf(n, " glead:");
			for(int i = 0; i < 16 && n < WD_DUMP_BODY_MAX - 8; i++)
				n = wd_dump_appendf(n, "%02x", fb[i]);
			n = wd_dump_appendf(n, " ggap:");
			for(int i = -16; i < 0 && n < WD_DUMP_BODY_MAX - 8; i++)
				n = wd_dump_appendf(n, "%02x", fb[i]);
		}
		n = wd_dump_appendf(n, "\n");
	}
	// XFB GUARD + NEIGHBOR TABLE (2026-06-13): names the forward-overrun writer.
	// fbguard = the XFB whose trailing guard was smashed; the table shows
	// physical adjacency (fb2_base + size == fifo_base confirms frameBuffer[2]
	// sits immediately below the FIFO, so a fb2 hit pins the writer).
	{
		int fbw = -1;
		uint32_t fboff = 0;
		uint32_t hit = gfx_fb_guard_check(&fbw, &fboff);
		uint32_t fbp[3], fbs[3], fifop = 0;
		gfx_neighbor_info(fbp, fbs, &fifop);
		n = wd_dump_appendf(n,
			"fbguard=%s off=%u\n"
			"fb0=0x%07x+%u fb1=0x%07x+%u fb2=0x%07x+%u fifo=0x%07x\n",
			hit ? (fbw == 2 ? "fb2" : fbw == 1 ? "fb1" : "fb0") : "intact",
			(unsigned)fboff, (unsigned)fbp[0], (unsigned)fbs[0],
			(unsigned)fbp[1], (unsigned)fbs[1], (unsigned)fbp[2],
			(unsigned)fbs[2], (unsigned)fifop);
	}
	if(gpr) { // FAULT path only: real registers
		n = wd_dump_appendf(n, "PC=%08lx MSR=%08lx LR=%08lx\n",
			(unsigned long)srr0, (unsigned long)srr1, (unsigned long)lr);
		for(int i = 0; i < 13; i++)
			n = wd_dump_appendf(n, "r%-2d=%08lx%s", i,
				(unsigned long)gpr[i], (i % 4 == 3) ? "\n" : " ");
		n = wd_dump_appendf(n, "\n");
	}
	// M1: record whether the region I/O helper is inside a publication RIGHT
	// NOW, i.e. whether the SD sink below is about to meet a held libfat
	// partition lock. Sampled before the seal so the note is in every copy of
	// the dump, including the one that goes out when the SD write is skipped.
	n = wd_dump_appendf(n, "regionio=%s\n",
		region_archive_io_busy() ? "BUSY(sd write may be skipped)" : "idle");
	n = wd_dump_appendf(n, "mem2op=%u thread=%u bad=%u scan=%u/%uus wdseq=%u census=%u\n",
		(unsigned)mem2_diag_op(), (unsigned)mem2_diag_thread(),
		(unsigned)mem2_diag_bad(), (unsigned)mem2_diag_scan_count(),
		(unsigned)mem2_diag_scan_max_us(), (unsigned)watchdog_loop_seq(),
		(unsigned)watchdog_main_census_current());
	// SEAL: the END trailer is the last thing in the buffer, so every sink below writes
	// the same complete text and a file that lacks it is provably truncated.
	n = wd_dump_seal(n);
	// M1: TELEMETRY BEFORE LIBFAT. The sealed text goes to the network sink
	// FIRST, because that sink cannot block: on the Wii send_line_mode
	// publishes into the lock-free sender ring (cw_tel_publish, a CAS that
	// drops rather than waits) for a non-fault dump, and for a FAULT dump it
	// returns false immediately without touching IOS at all -- the deliberate
	// "never enter IOS or a producer critical section from an exception
	// handler" rule, which also means the ordering costs the fault path
	// nothing. The SD sink below is the one that can wedge on the libfat
	// partition lock (see WD_REGION_IO_DEFER_MS), so it can no longer take
	// the report down with it. This REPLACES the old "SD before TELEMETRY"
	// ordering, whose stated reason -- "the network send can wedge" -- was
	// true of a synchronous send and is not true of a ring publish.
	/* Emitted above, before the SD write (M1). */
	// "FBGUARD" is the NON-FATAL absorbed-overrun report: the engine is still
	// running (the XFB guard caught a bounded overrun before it reached the
	// FIFO), so do NOT paint the panic banner over a live game, and write a
	// SEPARATE file so it can't be mistaken for -- or clobber -- a real hang.
	int fatal = strcmp(cause, "FBGUARD") != 0;
	// PAINT BEFORE THE SD WRITE, then RE-PAINT the diag once the outcome is known --
	// wd_emit_sequence owns that order and the status/artifact agreement (watchdog.h).
	// The 2026-07-27 revision that wrote FIRST (to spare the live F3 overlay one more
	// frame) was wrong: the OBSERVED hardware failure is a wedged fopen -- libfat
	// allocating from the smashed heap and never returning, the historical "banner
	// without hang_report.txt" symptom named in hw_telemetry.c -- and with the write
	// first that wedge costs the banner AND the cell bars, leaving a console
	// indistinguishable from a plain freeze. The overlay argument does not hold either:
	// the F3 overlay is OFF unless debug.overlay is set (main.c), so a shipping run has
	// no overlay to preserve. wii/panic.c calls this from EXCEPTION context and then
	// spins to hold the banner up, which only works if the banner is painted first.
	// PAINT BEFORE STDERR TOO (2026-07-29). The survive-the-frozen-SD stderr dump below
	// used to run here, ahead of the paint. It has to be the other way round on two counts.
	// (1) It can BLOCK: fprintf takes newlib's FILE lock and enters libogc's devoptab
	// write, and wii/panic.c calls this whole function from EXCEPTION context -- the same
	// class of wedge the paint exists to survive. (2) On the SHIPPING build it yields
	// nothing at all to weigh against that risk: SYS_STDIO_Report(true) is #ifndef
	// NDEBUG-gated in main.c and the Wii Makefile compiles -DNDEBUG, so the .dol that
	// actually runs on hardware has NO stderr channel. Sequencing a sink that produces
	// zero evidence ahead of the only sink a bare console has is backwards.
	// SD AFTER TELEMETRY (M1, 2026-09-08): see the emit above. The SD report is still
	// what the crash mailbox replays on the next boot, so it is still attempted --
	// it simply no longer gates the one sink that cannot block.
	// ABSOLUTE sd:/ paths (HW_TELEMETRY_*_PATH): the old CWD-relative names only landed at
	// sd:/ because the HBC-launch CWD happens to be the device root -- the "CWD =
	// sd:/apps/CavEX" comment that used to be here was FALSE. The crash-mailbox reader
	// and the stage-server fetch use these same paths.
	// wd_dump_identity above already advanced the sequence, so this is THIS report's
	// number -- report 1 lands on the canonical path, 2..8 get the `_<seq>` suffix, and
	// nothing overwrites its predecessor. See wd_report_path_for_seq (watchdog.h).
	struct wd_emit_ctx ectx = {.fatal = fatal, .seq = wd_dump_report_seq()};
	struct wd_emit_ops ops = {
		.ctx = &ectx,
		.status = wd_emit_status_cb,
		.paint = wd_emit_paint_cb,
		.repaint = wd_emit_repaint_cb,
		.write_primary = wd_emit_write_primary_cb,
		.write_fallback = wd_emit_write_fallback_cb,
	};
	if(wd_emit_sequence(&ops, fatal != 0, n) >= WD_SD_PARTIAL)
		fprintf(stderr, "[watchdog] SD report INCOMPLETE on both paths (status=%u)\n",
				(unsigned)g_wd_sd_status);
	// SURVIVE-THE-FROZEN-SD diagnostic (2026-06-20, resequenced 2026-07-29): the assembled
	// dump to stderr, AFTER the paint + SD attempt above and BEFORE the network send below.
	// stderr is UNBUFFERED so this never mallocs -- it survives the exact lwp_heap
	// free-list corruption that hangs the fopen() in wd_write_report, which is why it is
	// still emitted even when that write failed. CAVEAT: it only REACHES a reader where
	// stdio is routed -- Dolphin's log window, or a USB Gecko on a build that called
	// SYS_STDIO_Report(true), which main.c does only under !NDEBUG. The shipping -DNDEBUG
	// .dol has NO stderr channel, so on hardware the cell-bar paint is the only bare-
	// console instrument, which is why it carries the memory-pressure and walk-overrun
	// rows. Read the canary first-offender (cnry/size/off) + the phase tags to name the
	// writer, and the "id build=... boot=..." line to confirm which boot wrote it.
	fprintf(stderr, "\n==== CAVEWII WATCHDOG DUMP (post-paint, survives frozen heap) ====\n%s"
					"==== END WATCHDOG DUMP ====\n",
			wd_dump_buf());
	hw_telemetry_emit_dump(cause, wd_dump_buf(), gpr != NULL);
}

static void (*s_context_observer)(void);
void watchdog_context_observer_set(void (*observer)(void)) {
	__atomic_store_n(&s_context_observer, observer, __ATOMIC_RELEASE);
}

static void* wd_thread_entry(void* arg) {
	(void)arg;
	g9_probe_role(5);
	uint64_t last_hb = wd_heartbeat_read(); // last heartbeat we sampled
	unsigned context_divider = 0;
	for(;;) {
		// C1 (candidate, hardware-unconfirmed): checked immediately before AND
		// after the UNCHANGED 250ms wait below so watchdog_shutdown() can break
		// the loop without altering the wake cadence a live game session runs
		// under -- an unmeasured cadence change here (e.g. polling in smaller
		// chunks) is T3 per the project's perf rules. A shutdown join can
		// therefore take up to ~400ms (the sum of the two waits below); that
		// latency is accepted so the certified hot-path behaviour stays
		// byte-identical.
		if(g_wd_stop)
			break;
		usleep(250 * 1000); // 250ms; the wake IRQ preempts the wedged prio-64 main thread
		__atomic_fetch_add(&g_wd_loop_seq, 1, __ATOMIC_RELAXED);
		if(g_wd_stop)
			break;
		if(++context_divider == 4) {
			context_divider = 0;
			void (*observer)(void) = __atomic_load_n(&s_context_observer,
				__ATOMIC_ACQUIRE);
			if(observer) observer();
		}

		// XFB-overrun watch (2026-06-13): if a forward overrun is ABSORBED by an
		// XFB trailing guard (a bounded overrun that never reaches the FIFO -> no
		// wedge, the engine keeps running), it would otherwise go unreported.
		// Catch it here off the render path: on the first hit, write a one-shot
		// FBGUARD report so the writer is NAMED even without a freeze. Write-once
		// (a repeating per-frame overrun must not re-fopen every 250ms).
		if(!g_wd_guard_reported && gfx_fb_guard_check(NULL, NULL)) {
			g_wd_guard_reported = 1;
			wd_emit_dump("FBGUARD", 0, 0, 0, NULL);
		}

		uint64_t now = gettime(), hb = wd_heartbeat_read();
		wd_record_heartbeat_gap(now, hb);
		if(hb != last_hb) {
			// Main loop advanced since the last check -> alive -> RE-ARM. This is what
			// makes a transient heavy frame (a one-off >threshold stall) NOT permanently
			// disarm the watchdog: it can fire once on a transient and then re-arm when
			// the heartbeat resumes, so only a PERMANENT stall leaves a final report.
			last_hb = hb;
			g_wd_fired = 0;
			continue;
		}
		// Heartbeat unchanged since the previous 250ms sample = no main-loop progress.
		if(!g_wd_fired && now > hb
		   && ticks_to_millisecs(now - hb) > 3000ull + g_wd_stall_grace_ms) {
			g_wd_fired = 1; // fire ONCE per stall; a later fire overwrites the report
			// Sample the VI pre-retrace IRQ liveness across ~150ms: does it still fire,
			// and does its callback body still complete, while the main loop is wedged?
			g_wd_vi_e0 = gfx_vi_enter_count();
			g_wd_vi_x0 = gfx_vi_exit_count();
			// C1: same before/after check around the UNCHANGED 150ms wait -- see
			// the note at the top of the loop. Skipping the dump on this exit
			// path is correct: there is nothing to report, we are shutting down
			// cleanly, not diagnosing a wedge.
			if(g_wd_stop)
				break;
			usleep(150 * 1000);
			if(g_wd_stop)
				break;
			g_wd_vi_e1 = gfx_vi_enter_count();
			g_wd_vi_x1 = gfx_vi_exit_count();
			wd_emit_dump("HANG", 0, 0, 0, NULL); // TIER-0: phase tags + counters
			// GP-wedge recovery lives in gfx_wait_drawdone_bounded (main thread),
			// which self-recovers within GP_WEDGE_RECOVER_MS -- so main never
			// reaches the 3s WDM_GFX_DRAWDONE stall here, and a watchdog-thread
			// GX_AbortFrame would only RACE main's (review Q3). No recovery here.
		}
	}
	return NULL;
}

bool watchdog_init(void) {
	watchdog_heartbeat();
	g_wd_hb_gap_max_ms = 0;
	g_wd_boot_tb = gettime(); // base for the report's `up=` delta (wd_uptime_ticks)
	(void)wd_boot_id();       // seed the per-boot report nonce
	g_wd_stop = 0; // defensive: a hypothetical re-init after a prior shutdown
					// must not start the new thread pre-armed to exit.
	// prio 100 > main/server (64) > mesher (32): when the 250ms usleep expires, the timer
	// ISR makes this thread READY and libogc preempts the wedged prio-64 main thread on
	// IRQ return BECAUSE 100 > 64. (Equal prio would NEVER preempt -- it MUST be > 64.)
	s32 result = LWP_CreateThread(&g_wd_thread, wd_thread_entry, NULL, g_wd_stack,
							  sizeof(g_wd_stack), 100);
	if(result != 0) {
		g_wd_thread = LWP_THREAD_NULL;
		fprintf(stderr, "[watchdog] LWP_CreateThread failed: %ld\n", (long)result);
		return false;
	}
	watchdog_install_fault_handler(); // wii/panic.c
	return true;
}

// C1 (candidate, hardware-unconfirmed, 2026-09-02): stop + join the detector
// thread. Idempotent -- g_wd_thread is reset to LWP_THREAD_NULL once joined, so
// a second call (or a call after watchdog_init was never invoked, or failed)
// sees the sentinel and returns immediately without touching g_wd_stop or
// blocking. Does not free g_wd_stack: it is a static array, not an allocation.
void watchdog_shutdown(void) {
	if(g_wd_thread == LWP_THREAD_NULL)
		return;
	g_wd_stop = 1;
	void* unused;
	LWP_JoinThread(g_wd_thread, &unused); // returns once wd_thread_entry exits
	g_wd_thread = LWP_THREAD_NULL;
}
