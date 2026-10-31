/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include "../service_observer.h"
#include <gccore.h>
#ifndef CAVEWII_RETIRE_GFX_TEST
#include <ogc/lwp_watchdog.h>
#include "../frame_wait_timing.h"
#include "../guard_scan.h"
#include <malloc.h>
#include <math.h>
#include <string.h>

#include "../../graphics/texture_atlas.h"
#include "../../light_brightness.h"
#include "../../util.h"
#include "../gfx.h"
#include "../displaylist.h"
#include "../texture.h"
#include "../watchdog.h"
#endif

int usleep(useconds_t); // newlib hides it under -std=c99 (thread.c declares it too)

#define GP_WEDGE_RECOVER_MS 500 // bounded draw-done deadline; >> any legit frame

static lwp_t s_gpu_main_thread;
static void gfx_wait_token_bounded(uint32_t token);
static bool gfx_gpu_is_main(void) {
	return LWP_GetSelf() == s_gpu_main_thread;
}
static uint16_t gfx_gpu_completed(void) { return GX_GetDrawSync(); }
static void gfx_gpu_seal(uint16_t token) { GX_SetDrawSync(token); }

#ifndef CAVEWII_RETIRE_GFX_TEST
// 2MB (2026-06-13): the OLD 256KB FIFO was overflowed ~5578x in a 10-min soak
// (ovc) and deadlocked main in GX_WaitDrawDone with the GP IDLE. The MECHANISM
// (verified in libogc): FIFO overflow runs __GXOverflowHandler, which
// LWP_SuspendThread's the writer (main) at the high watermark and arms an
// underflow IRQ to resume it. Under chronic saturation that suspend/resume
// races -- main is left suspended (or the CP pointer accounting is left
// inconsistent: GP idle yet FIFO "full"), so main never finishes the frame and
// parks in the draw-done wait forever. (NOT a "lost finish token" -- a lost IRQ
// would leave the FIFO EMPTY, not full.) The dominant producer is gutil_clouds:
// a 33x33 cell grid, each opaque cell a 24-vertex cube emitted TWICE = up to
// ~52k immediate-mode verts (~0.7-0.9MB) per frame, far past 256KB. 2MB keeps
// the worst-case frame UNDER the high watermark so __GXOverflowHandler never
// fires -> no suspend race -> no deadlock. A genuine CAPACITY fix for a
// saturation deadlock with the GP keeping up -- NOT the "hide a malformed
// command by growing the FIFO" anti-pattern (that was the resolved cmdIdle=0
// face). The bounded draw-done wait (gfx_wait_drawdone_bounded) is the
// robustness floor for ANY future trigger; reducing the cloud vertex volume is
// a separate perf follow-up. MEM1 has ~14MB free.
#define FIFO_SIZE (2 * 1024 * 1024)

// The watchdog's wedge-pass bracketing needs the REAL ring size; it used to
// hardcode the pre-2026-06-13 256K, which corrupted the single-wrap membership
// math and lap accounting in every hang report taken since the 2MB grow.
uint32_t gfx_fifo_size(void) { return FIFO_SIZE; }
#define FIFO_GUARD 1024         // 0xA5 band each side of the FIFO (32-aligned)
#define FIFO_GUARD_BYTE 0xA5u   // matches the mem2 red-zone fill
#define FB_GUARD (16 * 1024)    // trailing 0xA5 band per framebuffer (absorb+name)

static GXRModeObj* screenMode; // TODO: rename
static void* frameBuffer[3];
static void* frame = NULL;
static void* fifoBuffer = NULL;
static uint8_t* fifoAlloc = NULL; // guard | fifoBuffer | guard (uncached K1)
static void* fbAllocBase[3];      // each XFB's base (uncached K1), for the guard scan
static uint32_t fbDataSize[3];    // VIDEO_GetFrameBufferSize per XFB

// First corrupted byte (1-based) in a FIFO guard band, else 0; *leading tells
// which band. Read by the watchdog on a hang -- a non-zero result NAMES a stray
// MEM1 writer overrunning into the GX FIFO (the GP-wedge root-cause suspect that
// the mem2 canary cannot see). Cheap linear scan, only on the fire path.
uint32_t gfx_fifo_guard_check(int* leading) {
	if(!fifoAlloc)
		return 0;
	for(uint32_t i = 0; i < FIFO_GUARD; i++)
		if(fifoAlloc[i] != FIFO_GUARD_BYTE) {
			if(leading)
				*leading = 1;
			return i + 1;
		}
	uint8_t* trail = fifoAlloc + FIFO_GUARD + FIFO_SIZE;
	for(uint32_t i = 0; i < FIFO_GUARD; i++)
		if(trail[i] != FIFO_GUARD_BYTE) {
			if(leading)
				*leading = 0;
			return i + 1;
		}
	return 0;
}

// Extent + first value of leading-guard corruption, for the report. *first/*last
// = 1-based byte range of corruption (0 = none); *val = the byte AT *first. The
// VALUE discriminates the writer: a dlmalloc free-list pointer high byte
// (0x80..0x9f) confirms the stale-cache-writeback artifact (harmless, below the
// GP's buf_start); float/vertex bytes (e.g. 0x3f/0xb9) = a live stray store.
void gfx_fifo_guard_extent(uint32_t* first, uint32_t* last, uint8_t* val) {
	*first = 0;
	*last = 0;
	*val = 0;
	if(!fifoAlloc)
		return;
	for(uint32_t i = 0; i < FIFO_GUARD; i++)
		if(fifoAlloc[i] != FIFO_GUARD_BYTE) {
			if(!*first) {
				*first = i + 1;
				*val = fifoAlloc[i];
			}
			*last = i + 1;
		}
}

// fifoAlloc base for the watchdog's hex window of the corruption + the 32B gap
// below it (uncached K1: a direct RAM read of what the writer left).
const uint8_t* gfx_fifo_alloc_base(void) {
	return fifoAlloc;
}

// Scan the trailing guard of each framebuffer; returns 1 + sets *which (0..2) /
// *off (1-based byte into that guard) on the first corrupt band, else 0. A hit
// NAMES which XFB was overrun past its end -- frameBuffer[2] is the FIFO's
// immediate neighbor, so a fb[2] hit pins the GP-wedge writer. Off the render
// hot path (watchdog 250ms tick + on a hang dump).
uint32_t gfx_fb_guard_check(int* which, uint32_t* off) {
	for(int i = 0; i < 3; i++) {
		if(!fbAllocBase[i])
			continue;
		uint8_t* g = (uint8_t*)fbAllocBase[i] + fbDataSize[i];
		uint32_t hit = (uint32_t)guard_scan_a5(g, FB_GUARD);
		if(hit) {
			if(which) *which = i;
			if(off) *off = hit;
			return 1;
		}
	}
	return 0;
}

/* Frozen byte-scan oracle, used only by the opt-in startup comparison. */
static uint32_t gfx_fb_guard_reference(void) {
	for(int i = 0; i < 3; i++) {
		if(!fbAllocBase[i]) continue;
		const uint8_t* g = (const uint8_t*)fbAllocBase[i] + fbDataSize[i];
		for(uint32_t k = 0; k < FB_GUARD; k++)
			if(g[k] != FIFO_GUARD_BYTE) return 1;
	}
	return 0;
}
void gfx_guard_scan_benchmark(uint32_t out_us[2]) {
	uint64_t elapsed[2] = {0,0};
	volatile uint32_t result = 0;
	for(int n = 0; n < 16; n++) {
		uint64_t start = gettime();
		result = gfx_fb_guard_reference();
		elapsed[0] += gettime()-start;
		start = gettime();
		result = gfx_fb_guard_check(NULL,NULL);
		elapsed[1] += gettime()-start;
	}
	(void)result;
	for(int i = 0; i < 2; i++) out_us[i] = ticks_to_microsecs(elapsed[i])/16;
}

// Physical bases/sizes of the three XFBs + the FIFO alloc, for the hang report's
// neighbor table -- shows adjacency (fb[2]_phys + size == fifo_phys confirms
// frameBuffer[2] sits immediately below the FIFO).
void gfx_neighbor_info(uint32_t fb_phys[3], uint32_t fb_size[3],
					   uint32_t* fifo_phys) {
	for(int i = 0; i < 3; i++) {
		fb_phys[i] = (uint32_t)fbAllocBase[i] & 0x1FFFFFFFu;
		fb_size[i] = fbDataSize[i];
	}
	*fifo_phys = (uint32_t)fifoAlloc & 0x1FFFFFFFu;
}
static uint8_t colors[256 * 3] ATTRIBUTE_ALIGN(32);

static bool gfx_matrix_texture_prev = false;
static bool gfx_fog_prev = false;
static bool gfx_tint_prev = false;
// audit #17f: tracks whether the emissive entity-tint 2nd TEV stage is CURRENTLY
// applied, so gfx_entity_tint() can early-out and touch ZERO GX registers on the
// common untinted path (amount==0 when nothing was applied) -- the same _prev
// dirty-guard pattern gfx_fog/gfx_tint use. Without this, every untinted mob paid
// ~5 redundant GP register writes per frame on the mandatory reset call.
static bool gfx_entity_tint_active = false;

static uint32_t gfx_depth_last = GX_TRUE;
static uint32_t gfx_depth_test_last = GX_TRUE;
static uint32_t gfx_depth_func_last = GX_LEQUAL;

static int gfx_screen_width = 802;

/*
	FRAMEBUFFER HANDOFF (main thread <-> VI pre-retrace IRQ callback).

	This used to be a pair of libogc message queues (frame_draw/frame_empty;
	the design dates from when the flip ran in a vsync THREAD). Twice on
	hardware (2026-06-07 "run2", 2026-06-11b) the main thread sat blocked in
	MQ_Receive(frame_empty) forever while the VI callback demonstrably kept
	running (enter/exit counters advancing) and the GP was idle. The 2026-06-11
	libogc source/binary audit found why the MQ can do that: in libogc v3.0.4's
	tuxedo compat shim (lwpcompat/lwpc_mq.c), blocking MQ_Send NO LONGER EXISTS
	-- an `if (true)` forces every send through KMailboxTrySend, which on a
	full queue returns false and SILENTLY DROPS the message (verified in the
	linked libogc.a: MQ_Send never reads its flags argument). A dropped
	framebuffer message is a buffer leaked out of circulation forever -> the
	receiver starves exactly as captured. So the handoff now uses primitives
	whose contracts the audit verified against the installed kernel:

	- two single-producer/single-consumer rings (fb_to_vi: main pushes drawn
	  frames, the callback pops; fb_from_vi: the callback pushes released
	  buffers, main pops). Each index is written by exactly ONE side, slots
	  are written before the index bump (volatile keeps the order), and on
	  this single-core in-order CPU an IRQ and a thread strictly interleave
	  -- so the rings need no locks at all.
	- one LWP semaphore counting released buffers: the callback posts AFTER
	  its ring push, main waits then pops. In v3.0.4 (lwpcompat/lwpc_sem.c)
	  wait's decrement-and-block and post's increment-and-wake each run as one
	  EE-off section -- no check-then-block window, ISR-safe by construction,
	  and exact debtor accounting (one post wakes exactly one wait, so a
	  successful wait ALWAYS finds a pushed buffer in the ring).

	Capacity 4 > the 3 framebuffers that can ever be in flight, so the rings
	cannot fill and pushes never block. Because this file ships NDEBUG (assert
	is a no-op on the only platform that runs it), the push still carries an
	explicit one-compare full-check: a full ring REFUSES the push (never
	overwrites a live slot) and bumps g_gfx_fb_ring_over, which the watchdog
	prints in the hang report ("fbover=") -- so a future token-accounting
	regression names itself instead of silently corrupting the ring.
*/

#define FB_RING_SIZE 4 /* power of two, > nr of framebuffers (3) */

static bool s_wait_timing_enabled;
static struct gfx_wait_timing s_wait_timing;
struct gfx_wait_timing gfx_frame_wait_timing(bool enabled) {
	__atomic_store_n(&s_wait_timing_enabled, enabled, __ATOMIC_RELAXED);
	return s_wait_timing;
}

struct fb_ring {
	/* Published before tail, read before head advances, just like slot. */
	volatile uint64_t released_us[FB_RING_SIZE];
	void* volatile slot[FB_RING_SIZE];
	volatile uint32_t tail; /* producer-owned: next write position */
	volatile uint32_t head; /* consumer-owned: next read position */
};

static struct fb_ring fb_to_vi;   /* main -> VI cb: frames ready to display */
static struct fb_ring fb_from_vi; /* VI cb -> main: buffers free to draw into */
static sem_t fb_free_sem;         /* counts buffers available in fb_from_vi */

/* Unreachable while the 3 buffer tokens are conserved (see header comment);
 * counted, not asserted, because the Wii build is NDEBUG. Printed in the
 * watchdog hang report as "fbover=". */
volatile uint32_t g_gfx_fb_ring_over = 0;
uint32_t gfx_fb_ring_over_count(void) { return g_gfx_fb_ring_over; }

static void fb_ring_push(struct fb_ring* r, void* fb) {
	if(r->tail - r->head >= FB_RING_SIZE) { /* full: refuse, never overwrite */
		g_gfx_fb_ring_over++;
		return;
	}

	r->released_us[r->tail % FB_RING_SIZE]
		= r == &fb_from_vi
		  && __atomic_load_n(&s_wait_timing_enabled, __ATOMIC_RELAXED)
			? ticks_to_microsecs(gettime()) : 0;
	r->slot[r->tail % FB_RING_SIZE] = fb; /* slot BEFORE index bump */
	r->tail = r->tail + 1;
}

static void* fb_ring_pop(struct fb_ring* r) {
	if(r->head == r->tail)
		return NULL;

	void* fb = r->slot[r->head % FB_RING_SIZE];
	r->head = r->head + 1;
	return fb;
}

static void* volatile current_frame = NULL;

// Watchdog diagnostics: g_gfx_vi_enter bumps every VI pre-retrace IRQ (is the IRQ alive?);
// g_gfx_vi_exit bumps after the callback body completes. Since the SPSC-ring+semaphore
// rework the callback has NO blocking call, so at a flip hang: enter advances + exit
// frozen should be IMPOSSIBLE (if seen, something inside copy_buffers faulted/looped);
// enter frozen -> the VI IRQ stopped entirely; both advancing -> the IRQ is fine, so
// suspect the handoff accounting -- check the "fbover=" counter in the same report.
volatile uint32_t g_gfx_vi_enter = 0;
volatile uint32_t g_gfx_vi_exit = 0;
uint32_t gfx_vi_enter_count(void) { return g_gfx_vi_enter; }
uint32_t gfx_vi_exit_count(void) { return g_gfx_vi_exit; }

static void copy_buffers(u32 cnt) {
	g_gfx_vi_enter++;
	void* input_frame = fb_ring_pop(&fb_to_vi);

	if(input_frame) {
		VIDEO_SetNextFramebuffer(input_frame);
		VIDEO_Flush();

		if(current_frame) {
			fb_ring_push(&fb_from_vi, current_frame);
			LWP_SemPost(fb_free_sem); /* post AFTER the push */
		}

		current_frame = input_frame;
	}
	g_gfx_vi_exit++;
}

int gfx_width() {
	return gfx_screen_width;
}

int gfx_height() {
	return 480;
}

// Allocate a framebuffer with a trailing 0xA5 guard band (2026-06-13 GP-wedge):
// the leading-FIFO-guard hit proved a CPU FORWARD OVERRUN from the buffer just
// below the FIFO, and frameBuffer[2] (the live draw XFB, written every frame by
// GX_CopyDisp, plain MEM1 / no canary) is that neighbor by construction. A
// trailing guard on each XFB (a) trips at the OFFENDING fb's boundary -> names
// the writer, and (b) a generous FB_GUARD absorbs a bounded (few-row) overrun
// before it reaches the FIFO, turning the hard freeze into a caught+logged
// event. Same size as SYS_AllocateFramebuffer (VIDEO_GetFrameBufferSize) + guard.
static void* gfx_alloc_fb_guarded(int idx) {
	uint32_t sz = VIDEO_GetFrameBufferSize(screenMode);
	void* allocation = memalign(32, sz + FB_GUARD);
	if(!allocation) {
		fprintf(stderr, "[gfx] framebuffer %d allocation failed (%u bytes)\n", idx,
				(unsigned)(sz + FB_GUARD));
		return NULL;
	}
	uint8_t* base = (uint8_t*)MEM_K0_TO_K1(allocation);
	// Drop the stale dlmalloc-written cached lines (see the FIFO alloc) before
	// touching the K1 alias, so no writeback corrupts the XFB or its guard.
	DCInvalidateRange(MEM_K1_TO_K0(base), sz + FB_GUARD);
	memset(base + sz, FIFO_GUARD_BYTE, FB_GUARD);
	fbAllocBase[idx] = base;
	fbDataSize[idx] = sz;
	return base;
}

static void service_observer_vi(u32 count) {
	(void)count;
	SERVICE_OBSERVER(service_observer_display((uintptr_t)VIDEO_GetCurrentFramebuffer()));
}

bool gfx_setup() {
	// A chainloader (e.g. the CWRUN1 stage server) can jump here with VI still
	// enabled in its own configuration; libogc's VIDEO_Init() performs its full
	// __VIInit hardware reset ONLY when VI is disabled, so an enabled VI seeds
	// the mode/timing shadows from half-inherited state (banded/interleaved
	// picture on real Wii, 2026-08-18). Disable VI (DCR=0) first so every
	// loader path takes the proven cold-boot reset.
	*(vu16*)0xCC002002 = 0;
	VIDEO_Init();
	screenMode = VIDEO_GetPreferredMode(NULL);
	if(!screenMode) {
		fprintf(stderr, "[gfx] VIDEO_GetPreferredMode failed\n");
		return false;
	}
	// T1-a (2026-09-03 far-ring frame-drop root-cause investigation): the doc's
	// mechanism B describes a TWO-framebuffer XFB starving the render pipeline
	// of any spare buffer. Re-reading this function against that claim found
	// triple buffering ALREADY landed here -- three XFBs allocated (below),
	// FB_RING_SIZE sized "> nr of framebuffers (3)" (see the fb_ring comment
	// block), and the semaphore below already started at count=2/max=3 (i.e.
	// n_bufs-1 of n_bufs start free) since M1's SPSC-ring rework (92b52e3,
	// long before this root-cause doc). tests/test_fb_ring_equiv.c proves the
	// CAPACITY consequence directly: plain double buffering (2 XFBs) already
	// gives gfx_flip_buffers one free flip of render-ahead headroom (the
	// textbook minimum for double buffering to mean anything -- draw into
	// the spare while the other buffer is on screen); this 3-buffer
	// configuration buys a SECOND free flip on top of that -- a full extra
	// frame of slack -- before the semaphore can ever force a real
	// VI-callback wait. So T1-a's fix is already in place; what was missing
	// was the pinning test + this note, so a future "cleanup" can't silently
	// regress the buffer count back to 2 (losing that extra frame) without a
	// test failing first. The measured vsync-class dips in the doc's
	// telemetry (t=169 etc.) therefore do NOT stem from a missing spare
	// buffer; the field-time-cost mechanism (a vsync miss reads as 2 VI
	// fields, ~33ms) is inherent to how a MISSED deadline is observed on
	// interlaced/field-rendered video, not a buffering-depth defect -- worth
	// a follow-up note in the root-cause doc.
	_Static_assert(sizeof(frameBuffer) / sizeof(frameBuffer[0]) == 3,
				  "T1-a: gfx_setup's initial fb_from_vi seeding + the "
				  "LWP_SemInit(2,3) below assume exactly 3 XFBs");

	frameBuffer[0] = gfx_alloc_fb_guarded(0);
	frameBuffer[1] = gfx_alloc_fb_guarded(1);
	frameBuffer[2] = gfx_alloc_fb_guarded(2);
	if(!frameBuffer[0] || !frameBuffer[1] || !frameBuffer[2])
		return false;

	// Same initial ownership as the old MQ design: fb[0]+fb[1] start free
	// (fb[0] is also the boot scanout target -- at worst one torn frame at
	// startup, before current_frame tracking begins), main draws into fb[2].
	fb_ring_push(&fb_from_vi, frameBuffer[0]);
	fb_ring_push(&fb_from_vi, frameBuffer[1]);
	s32 sem_res = LWP_SemInit(&fb_free_sem, 2, 3);
	if(sem_res != 0) {
		fprintf(stderr, "[gfx] framebuffer semaphore init failed: %ld\n",
				(long)sem_res);
		return false;
	}
	frame = frameBuffer[2];

	if(CONF_GetAspectRatio() == CONF_ASPECT_16_9) {
		screenMode->viWidth = 678;
		gfx_screen_width = 802;
	} else {
		screenMode->viWidth = 672;
		gfx_screen_width = 640;
	}

	if(VIDEO_GetCurrentTvMode() == VI_PAL
	   || VIDEO_GetCurrentTvMode() == VI_MPAL) {
		screenMode->viXOrigin = (VI_MAX_WIDTH_PAL - screenMode->viWidth) / 2;
	} else {
		screenMode->viXOrigin = (VI_MAX_WIDTH_NTSC - screenMode->viWidth) / 2;
	}

	s8 hoffset = 0;
	CONF_GetDisplayOffsetH(&hoffset);
	screenMode->viXOrigin += hoffset;

	VIDEO_Configure(screenMode);
	VIDEO_SetNextFramebuffer(frameBuffer[0]);
	VIDEO_SetPreRetraceCallback(copy_buffers);
	SERVICE_OBSERVER(for(unsigned so_fb = 0; so_fb < 3; so_fb++) service_observer_submit((uintptr_t)frameBuffer[so_fb]));
	SERVICE_OBSERVER(VIDEO_SetPostRetraceCallback(service_observer_vi));
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	// VI SETTLE FRAMES (Batch E item 4, 2026-08-23 HBC-direct RCA fix
	// candidate 3 -- vertical-band corruption on HBC-direct launches even
	// with the DCR=0 full-VI-reset above). Transcribed VERBATIM from the
	// chainload-proven pattern in tools/wii_stage_server/source/main.c:58-60
	// (video_init(): ...VIDEO_Flush(); VIDEO_WaitVSync(); if(rmode->viTVMode
	// & VI_NON_INTERLACE) VIDEO_WaitVSync();) -- every staged/chainloaded run
	// goes through that exact sequence and boots clean; the game's own
	// gfx_setup never had it. One retrace to let VIDEO_Configure's new
	// timing/mode actually latch before anything scans out, plus a second
	// for non-interlaced modes (double strobe), mirroring the stage
	// server's own conditional exactly rather than assuming its cause.
	VIDEO_WaitVSync();
	if(screenMode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	// FIFO GUARD BANDS (2026-06-13 GP-wedge root cause): the wedge is a stray
	// MEM1 write smashing a live GX_CallDispList operand INSIDE this buffer (the
	// captured CALL size was non-32-aligned, which displaylist_render's /32*32
	// formula provably cannot emit -> the FIFO bytes were overwritten in place,
	// not mis-enqueued). The buffer is a raw memalign, NOT mem2_alloc, so the
	// mem2 red-zone canary is structurally blind to it (canary=0 in every
	// report). Bracket it with 0xA5 guard bands: a forward heap overrun from a
	// MEM1 neighbor into the FIFO region trips the leading guard (and a backward
	// overrun the trailing one) BEFORE/while reaching the command stream, so the
	// watchdog can NAME that this writer class is real + how far it reached.
	void* fifo_allocation = memalign(32, FIFO_GUARD + FIFO_SIZE + FIFO_GUARD);
	if(!fifo_allocation) {
		fprintf(stderr, "[gfx] GX FIFO allocation failed (%u bytes)\n",
				(unsigned)(FIFO_GUARD + FIFO_SIZE + FIFO_GUARD));
		return false;
	}
	fifoAlloc = (uint8_t*)MEM_K0_TO_K1(fifo_allocation);
	// CACHE-COHERENCY (2026-06-13 root cause of the fifoguard=1 hit): memalign
	// returns a CACHED (K0) pointer over memory dlmalloc previously wrote
	// FREE-LIST POINTERS into (cached) while the chunk was free. We use the
	// UNCACHED (K1) alias and memset the guards/body through it -- but those
	// stale dirty K0 lines (the first line, at offset 0) are NOT invalidated, so
	// a later eviction writes the stale dlmalloc pointers back over fifoAlloc[0..]
	// = the "pinned to offset 0, layout-independent" leading-guard corruption.
	// Drop the stale cached lines before the K1 memsets so nothing evicts over
	// them. Discard (not flush): the dlmalloc free-list bytes are unwanted -- we
	// own this memory now and never free it. Same latent hazard on the XFBs
	// (masked there because GX_CopyDisp rewrites their offset 0 every frame).
	DCInvalidateRange(MEM_K1_TO_K0(fifoAlloc),
					  FIFO_GUARD + FIFO_SIZE + FIFO_GUARD);
	memset(fifoAlloc, FIFO_GUARD_BYTE, FIFO_GUARD);
	memset(fifoAlloc + FIFO_GUARD + FIFO_SIZE, FIFO_GUARD_BYTE, FIFO_GUARD);
	fifoBuffer = fifoAlloc + FIFO_GUARD;
	memset(fifoBuffer, 0, FIFO_SIZE);

	GX_Init(fifoBuffer, FIFO_SIZE);
	s_gpu_main_thread = LWP_GetSelf();
	const struct displaylist_gpu_hooks gpu_hooks = {
		gfx_gpu_completed, gfx_gpu_seal, gfx_wait_token_bounded, gfx_gpu_is_main
	};
	displaylist_gpu_configure(&gpu_hooks, GX_GetDrawSync());
	gfx_clear_buffers(255, 255, 255);
	gfx_set_viewport(0, 0, gfx_width(), gfx_height());
	GX_SetDispCopyYScale(
		GX_GetYScaleFactor(screenMode->efbHeight, screenMode->xfbHeight));
	GX_SetScissor(0, 0, screenMode->fbWidth, screenMode->efbHeight);
	GX_SetDispCopySrc(0, 0, screenMode->fbWidth, screenMode->efbHeight);
	GX_SetDispCopyDst(screenMode->fbWidth, screenMode->xfbHeight);
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	GX_SetFieldMode(screenMode->field_rendering,
					((screenMode->viHeight == 2 * screenMode->xfbHeight) ?
						 GX_ENABLE :
						 GX_DISABLE));

	GX_SetCullMode(GX_CULL_BACK);
	GX_CopyDisp(frameBuffer[0], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	GX_InvalidateTexAll();
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

	// blocks
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGB, GX_RGB8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_U8, 8);

	// entities, particles
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	// gui, font drawing
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_U16, 8);

	// blocks etc with direct color
	GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
	GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_U8, 8);

	// biome-tinted blocks: indexed light (CLR0, like VTXFMT0) plus a second
	// direct-RGB colour channel (CLR1) carrying the per-vertex Beta biome tint.
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGB, GX_RGB8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR1, GX_CLR_RGB, GX_RGB8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_TEX0, GX_TEX_ST, GX_U8, 8);

	GX_SetArray(GX_VA_CLR0, colors, 3 * sizeof(uint8_t));
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetNumTevStages(1);
	gfx_texture(true);
	gfx_alpha_test(true);

	if(!tex_init()) {
		fprintf(stderr, "[gfx] mandatory texture initialization failed\n");
		return false;
	}
	gfx_bind_texture(&texture_terrain);
	tex_gfx_bind(&texture_fog, GX_TEXMAP1);

	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

	GX_SetLineWidth(12, GX_TO_ZERO);

	GX_DrawDone();
	return true;
}

void gfx_update_light(float daytime, float rain_strength, float thunder_strength,
					  const float* light_lookup) {
	assert(daytime > -GLM_FLT_EPSILON && daytime < 1.0F + GLM_FLT_EPSILON
		   && light_lookup);
	assert(rain_strength >= 0.0F && rain_strength <= 1.0F);
	assert(thunder_strength >= 0.0F && thunder_strength <= 1.0F);

	// Beta-faithful lightmap, built by the SAME shared code as pc/gfx.c (no drift) and
	// pinned by tests/test_daylight_curve.c: integer sky-subtract + max(blockLight,
	// skyLight - subtract) + single lightBrightnessTable lookup. Rain/thunder add to
	// the sky-subtract, so a storm dims the whole world (Beta
	// calculateSkylightSubtracted). Quantize the resulting brightness to the u8
	// grayscale GX vertex-colour the colours[] LUT holds.
	float lightmap[256];
	gfx_build_lightmap(lightmap, daytime, rain_strength, thunder_strength,
					   light_lookup);
	for(int i = 0; i < 256; i++) {
		uint8_t gray = roundf(lightmap[i] * 255.0F);
		colors[i * 3 + 0] = gray;
		colors[i * 3 + 1] = gray;
		colors[i * 3 + 2] = gray;
	}

	DCStoreRange(colors, sizeof(colors));
	GX_InvVtxCache();
}

float gfx_lookup_light(uint8_t light) {
	return (float)colors[light * 3] / 255.0F;
}

void gfx_clear_buffers(uint8_t r, uint8_t g, uint8_t b) {
	GX_SetCopyClear((GXColor) {r, g, b, 255}, GX_MAX_Z24);
}

// Per-frame PE draw-sync token, advanced after the EFB->XFB copy. The bounded
// wait polls GX_GetDrawSync()==this to know the copy has RETIRED. Unlike the CP
// idle bits (which go idle as soon as the CP dispatches the copy's BP writes,
// while the Pixel Engine is still streaming pixels into `frame`), this token is
// a PE-level fence -- it travels through the FIFO behind the copy, so the PE
// updates _peReg[7] only after the copy is fully written. (2026-06-13 review.)
/* Last frame's seal, not a second counter: retirement owns token allocation. */
static uint32_t s_frame_token;

void gfx_finish(bool vsync) {
	assert(frame);

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	/* Observer snapshots are MAIN-owned until fb_to_vi publication. The
	 * three conserved XFB tokens keep this buffer distinct from current_frame;
	 * copy_buffers returns the previous token before MAIN can draw it again.
	 * Post-retrace runs in the same VI IRQ, so MAIN cannot interleave that
	 * return with the display hook. See service_observer_submit's fence. */
	SERVICE_OBSERVER(service_observer_submit_safe((uintptr_t)frame, (uintptr_t)current_frame));
	GX_CopyDisp(frame, GX_TRUE);
	GX_SetDrawDone();
	s_frame_token = displaylist_gpu_seal(); // same allocator as DL retirement

	if(!vsync) {
		GX_WaitDrawDone();
		VIDEO_SetNextFramebuffer(frame);
		VIDEO_Flush();
	}
}

// GP-wedge recovery is now OWNED by gfx_wait_drawdone_bounded (main thread). The
// earlier watchdog-thread gfx_gp_recover() was REMOVED (2026-06-13 review Q3):
// once main self-recovers within GP_WEDGE_RECOVER_MS it never stalls in
// WDM_GFX_DRAWDONE long enough for the watchdog's 3s timer to fire, so that path
// was both dead AND a hazard -- two threads in GX_AbortFrame/__GX_CleanGPFifo at
// once race the CP/PI pointer accounting, the exact corruption class this whole
// effort fought. Single GX caller on the recovery path = no race.
volatile uint32_t g_gp_reset_count = 0;
uint32_t gfx_gp_reset_count(void) { return g_gp_reset_count; }

// Wait for the GP to finish the frame, BOUNDED -- the robustness floor
// (2026-06-13): an unbounded GX_WaitDrawDone deadlocks main FOREVER if the
// draw-done finish IRQ never arrives (the FIFO-overflow / GP-idle freeze class).
// The 2MB FIFO prevents the known cloud-saturation trigger, but this guarantees
// that NO future trigger can ever permanently freeze the console. Poll the PE
// draw-sync token (GX_GetDrawSync == the value gfx_finish set after the copy):
// it is a PIXEL-ENGINE fence that retires AFTER the EFB->XFB copy fully lands in
// `frame`, so flipping is safe (unlike the CP idle bits, which go true while the
// copy is still streaming -- the 2026-06-13 review caught that). Past the
// deadline (no legit frame is 500ms) the GP is wedged: reset it (GX_AbortFrame)
// and drop the frame rather than freeze, counting it (gpreset=). Normal frames
// fall through in 1-2 polls; the usleep yields to the server/mesher threads.
// NOTE: main is the sole GX caller on this path -- the watchdog's GP recovery
// was removed precisely so its GX_AbortFrame can't race this one (review Q3).
#endif /* unrelated rendering/VI code is excluded by the host lifetime test */
static void gfx_wait_token_bounded(uint32_t token) {
	assert(gfx_gpu_is_main());
	/* Push the token out of the write-gather pipe before reading the PE. */
	GX_Flush();
	ptime_t t0 = time_get();
	for(;;) {
		displaylist_gpu_reap();
		if(displaylist_gpu_token_complete(token)) return;
		if(time_diff_s(t0, time_get()) * 1000.0F > GP_WEDGE_RECOVER_MS) {
			g_gp_reset_count++;
			displaylist_gpu_abort_begin();
			GX_AbortFrame(); // wedged: reset the GP, drop the frame, survive
			/* libogc GX_AbortFrame synchronously waits for the PE abort,
			 * resets the GP and cleans its FIFO. Only THIS return witnesses
			 * quiescence; a timeout or missing token alone frees nothing. */
			displaylist_gpu_abort_complete();
			return;
		}
		bool timing = __atomic_load_n(&s_wait_timing_enabled, __ATOMIC_RELAXED);
		uint64_t deadline = timing ? ticks_to_microsecs(gettime()) + 200 : 0;
		usleep(200);
		if(timing) {
			uint32_t late = frame_wait_resume_delay_us(0, deadline,
				ticks_to_microsecs(gettime()));
			if(late > s_wait_timing.gpu_poll_late_us)
				s_wait_timing.gpu_poll_late_us = late;
		}
	}
}

#ifndef CAVEWII_RETIRE_GFX_TEST
static void gfx_wait_drawdone_bounded(void) {
	gfx_wait_token_bounded(s_frame_token);
}

void gfx_wait_idle(void) {
	/* Includes unsealed submits during teardown/provider replacement. */
	gfx_wait_token_bounded(displaylist_gpu_seal());
}

bool gfx_can_present(void) {
	return frame != NULL;
}

void gfx_wait_vsync(void) {
	VIDEO_WaitVSync();
}

void gfx_flip_buffers(float* gpu_wait, float* vsync_wait) {
	assert(frame);

	s_wait_timing = (struct gfx_wait_timing){0};
	ptime_t gpu_wait_start = time_get();
	watchdog_phase_main(WDM_GFX_DRAWDONE); // hang here = GP wedged (now bounded)
	gfx_wait_drawdone_bounded();
	ptime_t gpu_wait_end = time_get();

	watchdog_phase_main(WDM_GFX_FBPUSH); // non-blocking; a hang here is impossible by design
	fb_ring_push(&fb_to_vi, frame);
	watchdog_phase_main(WDM_GFX_SEMWAIT); // hang here = VI cb stopped releasing buffers
	bool timing = __atomic_load_n(&s_wait_timing_enabled, __ATOMIC_RELAXED);
	uint64_t started = timing ? ticks_to_microsecs(gettime()) : 0;
	LWP_SemWait(fb_free_sem);
	if(timing) {
		uint64_t resumed = ticks_to_microsecs(gettime());
		uint64_t released = fb_from_vi.released_us[fb_from_vi.head % FB_RING_SIZE];
		s_wait_timing.vi_ready_us = frame_wait_resume_delay_us(started, released, resumed);
	}
	frame = fb_ring_pop(&fb_from_vi);
	assert(frame); // a successful wait guarantees a pushed buffer (post follows push)

	if(vsync_wait)
		*vsync_wait = time_diff_s(gpu_wait_end, time_get());
	if(gpu_wait)
		*gpu_wait = time_diff_s(gpu_wait_start, gpu_wait_end);
}

void gfx_bind_texture(struct tex_gfx* tex) {
	assert(tex);
	tex_gfx_bind(tex, GX_TEXMAP0);
}

void gfx_copy_framebuffer(uint8_t* dest, size_t* width, size_t* height) {
	assert(width && height);

	*width = screenMode->fbWidth;
	*height = screenMode->efbHeight;

	if(!dest)
		return;

	size_t length
		= GX_GetTexBufferSize(*width, *height, GX_TF_RGBA8, GX_FALSE, 0);
	uint8_t* buffer = memalign(32, length);

	if(!buffer)
		return;

	GX_SetTexCopySrc(0, 0, *width, *height);
	GX_SetTexCopyDst(*width, *height, GX_TF_RGBA8, GX_FALSE);
	GX_CopyTex(buffer, GX_FALSE);
	GX_PixModeSync();
	GX_SetDrawDone();
	s_frame_token = displaylist_gpu_seal(); // same fence allocator as gfx_finish
	// bounded wait below actually waits on THIS copy's retirement rather than
	// a stale token left over from the last gfx_finish() call.
	DCInvalidateRange(buffer, length);
	// CLAMP (hardware-risk): a raw GX_WaitDrawDone() here had no timeout -- the
	// same GP-wedge freeze class gfx_flip_buffers already guards against
	// (2026-06-13 review). Reuse the bounded wait + GX_AbortFrame recovery
	// instead of duplicating it; on a wedge this drops the screenshot (buffer
	// may be incomplete) rather than freezing the console forever.
	gfx_wait_drawdone_bounded();

	uint8_t* src = buffer;

	// CLAMP (2026-06-13): the GX tile copy is 4x4-blocked, so the loops step in
	// 4s; when *width/*height are not multiples of 4 the edge tiles address
	// (x+bx)/(y+by) PAST the dest bounds (dest is *width x *height x 4). The src
	// (GX tex buffer) still carries the full padded tile, so advance src for
	// every block element but only WRITE dest in-bounds -- the old code wrote
	// out of bounds, a real MEM1 heap overrun (screenshot-gated; landed to
	// narrow the GP-wedge stray-writer field).
	for(size_t y = 0; y < *height; y += 4) {
		for(size_t x = 0; x < *width; x += 4) {
			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 4; bx++) {
					uint8_t a = *(src++), r = *(src++);
					if(x + bx < *width && y + by < *height) {
						uint8_t* col = dest + (x + bx + (y + by) * (*width)) * 4;
						col[3] = a;
						col[0] = r;
					}
				}
			}

			for(size_t by = 0; by < 4; by++) {
				for(size_t bx = 0; bx < 4; bx++) {
					uint8_t g = *(src++), b = *(src++);
					if(x + bx < *width && y + by < *height) {
						uint8_t* col = dest + (x + bx + (y + by) * (*width)) * 4;
						col[1] = g;
						col[2] = b;
					}
				}
			}
		}
	}

	free(buffer);
}

/* Debug capture (2026-09-11): stream the EFB to a raw RGBA file in 32-row
 * bands through ONE static tile buffer (~80 KB) plus a 4-row line buffer,
 * instead of gfx_copy_framebuffer's two transient 1.2 MB heap buffers (+
 * lodepng's tables): the free heap during a console capture run was ~1.3 MB
 * and the first such run (menus_dwell, 142 PNGs) died at 145 s mid-write.
 * File format: "CWFB", u32 width, u32 height (big-endian), then
 * width*height RGBA8 pixels top-down; tools/wii_rgba_to_png.py converts.
 * Same PE fence + bounded wait per band as the full copy above. */
bool gfx_write_framebuffer_raw(const char* path) {
	enum { BAND_ROWS = 32 };
	size_t w = screenMode->fbWidth, h = screenMode->efbHeight;
	static uint8_t* tile_buf;
	static size_t tile_len;
	static uint8_t* line_buf; /* 4 linear rows */
	if(!tile_buf) {
		tile_len = GX_GetTexBufferSize(w, BAND_ROWS, GX_TF_RGBA8, GX_FALSE, 0);
		tile_buf = memalign(32, tile_len);
		line_buf = malloc(w * 4 * 4);
		if(!tile_buf || !line_buf) {
			free(tile_buf);
			free(line_buf);
			tile_buf = NULL;
			line_buf = NULL;
			return false;
		}
	}
	FILE* fp = fopen(path, "wb");
	if(!fp)
		return false;
	uint8_t hdr[12] = {'C', 'W', 'F', 'B', w >> 24, w >> 16, w >> 8, w,
					   h >> 24, h >> 16, h >> 8, h};
	bool ok = fwrite(hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
	for(size_t y0 = 0; ok && y0 < h; y0 += BAND_ROWS) {
		size_t rows = h - y0 < BAND_ROWS ? h - y0 : BAND_ROWS;
		GX_SetTexCopySrc(0, y0, w, rows);
		GX_SetTexCopyDst(w, rows, GX_TF_RGBA8, GX_FALSE);
		GX_CopyTex(tile_buf, GX_FALSE);
		GX_PixModeSync();
		GX_SetDrawDone();
		s_frame_token = displaylist_gpu_seal();
		DCInvalidateRange(tile_buf, tile_len);
		gfx_wait_drawdone_bounded();
		const uint8_t* src = tile_buf;
		for(size_t ty = 0; ok && ty < rows; ty += 4) {
			/* one 4-row tile strip -> 4 linear rows (AR pairs, then GB) */
			for(size_t x = 0; x < w; x += 4) {
				for(size_t by = 0; by < 4; by++)
					for(size_t bx = 0; bx < 4; bx++) {
						uint8_t a = *(src++), r = *(src++);
						if(x + bx < w) {
							uint8_t* col = line_buf + (x + bx + by * w) * 4;
							col[0] = r;
							col[3] = a;
						}
					}
				for(size_t by = 0; by < 4; by++)
					for(size_t bx = 0; bx < 4; bx++) {
						uint8_t g = *(src++), b = *(src++);
						if(x + bx < w) {
							uint8_t* col = line_buf + (x + bx + by * w) * 4;
							col[1] = g;
							col[2] = b;
						}
					}
			}
			size_t lines = rows - ty < 4 ? rows - ty : 4;
			ok = fwrite(line_buf, 1, w * 4 * lines, fp) == w * 4 * lines;
		}
	}
	fclose(fp);
	return ok;
}

/* BG-BAKE-1028 capture (gfx.h): same banded EFB->texture copy and bounded PE
 * fence as gfx_write_framebuffer_raw above, but into a caller sink, with
 * per-call scratch released before return, and every band copied WITH clear
 * (GX_CopyTex clear flag: the copied EFB rect is reset to the current
 * GX_SetCopyClear colour + Z, i.e. what main's gfx_clear_buffers set for this
 * frame) -- so the rest of the frame draws on a clean EFB exactly as after a
 * flip. A cancel or sink failure stops delivery but still clears the
 * remaining bands. */
bool gfx_capture_frame_rows(gfx_capture_rows_fn sink, void* user) {
	enum { BAND_ROWS = 32 };
	size_t w = screenMode->fbWidth, h = screenMode->efbHeight;
	size_t tile_len = GX_GetTexBufferSize(w, BAND_ROWS, GX_TF_RGBA8, GX_FALSE, 0);
	uint8_t* tile_buf = memalign(32, tile_len);
	uint8_t* band = malloc(w * 4 * BAND_ROWS);
	bool ok = sink && tile_buf && band;
	if(!tile_buf) {
		/* No scratch to copy into: the EFB cannot be cleared through a copy.
		 * Report failure; the caller's menu frame overdraws the colour, and
		 * GUI passes draw without a depth test. */
		free(band);
		return false;
	}
	for(size_t y0 = 0; y0 < h; y0 += BAND_ROWS) {
		size_t rows = h - y0 < BAND_ROWS ? h - y0 : BAND_ROWS;
		GX_SetTexCopySrc(0, y0, w, rows);
		GX_SetTexCopyDst(w, rows, GX_TF_RGBA8, GX_FALSE);
		GX_CopyTex(tile_buf, GX_TRUE);
		GX_PixModeSync();
		GX_SetDrawDone();
		s_frame_token = displaylist_gpu_seal();
		DCInvalidateRange(tile_buf, tile_len);
		uint32_t resets = g_gp_reset_count;
		gfx_wait_drawdone_bounded();
		/* A wedge recovery (GX_AbortFrame) dropped this copy: never hand a
		 * possibly incomplete band to the sink. */
		if(g_gp_reset_count != resets)
			ok = false;
		if(!ok)
			continue; /* keep clearing; deliver nothing more */
		const uint8_t* src = tile_buf;
		/* 4x4 RGBA8 tiles: 16 AR pairs then 16 GB pairs per tile. */
		for(size_t ty = 0; ty < rows; ty += 4) {
			for(size_t x = 0; x < w; x += 4) {
				for(size_t by = 0; by < 4; by++)
					for(size_t bx = 0; bx < 4; bx++) {
						uint8_t a = *(src++), r = *(src++);
						if(x + bx < w && ty + by < rows) {
							uint8_t* col = band + (x + bx + (ty + by) * w) * 4;
							col[0] = r;
							col[3] = a;
						}
					}
				for(size_t by = 0; by < 4; by++)
					for(size_t bx = 0; bx < 4; bx++) {
						uint8_t g = *(src++), b = *(src++);
						if(x + bx < w && ty + by < rows) {
							uint8_t* col = band + (x + bx + (ty + by) * w) * 4;
							col[1] = g;
							col[2] = b;
						}
					}
			}
		}
		ok = sink(user, band, y0, rows, w, h);
	}
	free(band);
	free(tile_buf);
	return ok;
}

void gfx_mode_world() {
	gfx_write_buffers(true, true, true);
}

void gfx_mode_gui() {
	gfx_mode_gui_viewport(gfx_width(), gfx_height());
}

void gfx_mode_gui_viewport(int width, int height) {
	gfx_fog(false);

	Mtx44 projection;

	guOrtho(projection, 0, height, 0, width, -256, 256);
	GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);

	gfx_lighting(false);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(true);
	gfx_write_buffers(true, false, false);
}

void gfx_matrix_projection(mat4 proj, bool is_perspective) {
	assert(proj);

	mat4 convert;
	glm_translate_make(convert, (vec3) {0, 0, -1});
	convert[3][3] = 2.0F;
	glm_mat4_mul(convert, proj, convert);
	glm_mat4_transpose(convert);

	GX_LoadProjectionMtx(convert,
						 is_perspective ? GX_PERSPECTIVE : GX_ORTHOGRAPHIC);
}

void gfx_matrix_modelview(mat4 mv) {
	assert(mv);

	mat4 convert;
	glm_mat4_transpose_to(mv, convert);
	GX_LoadPosMtxImm(convert, GX_PNMTX0);
}

void gfx_matrix_texture(bool enable, mat4 tex) {
	if(enable) {
		assert(tex);

		mat4 convert;
		glm_mat4_transpose_to(tex, convert);
		GX_LoadTexMtxImm(convert, GX_TEXMTX0, GX_MTX2x4);
	}

	if(enable != gfx_matrix_texture_prev)
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0,
						  enable ? GX_TEXMTX0 : GX_IDENTITY);
	gfx_matrix_texture_prev = enable;
}

void gfx_fog_color(uint8_t r, uint8_t g, uint8_t b) {
	GX_SetTevColor(GX_TEVREG0, (GXColor) {r, g, b, 255});
}

void gfx_fog_pos(float dx, float dz, float distance) {
	assert(distance > 0);

	if(gfx_fog_prev) {
		float dist = 1.0F / (distance * 2.0F);
		GX_LoadTexMtxImm((mat4) {{dist, 0, 0, dx * dist + 0.5F},
								 {0, 0, dist, dz * dist + 0.5F},
								 {0, 0, 0, 0},
								 {0, 0, 0, 1}},
						 GX_TEXMTX1, GX_MTX2x4);
	}
}

// Lay out the TEV pipeline from the current fog + tint state. The biome-tint
// stage (when active) is stage 1 (tex*light -> *tint); fog, when on, is the
// LAST stage so it blends the finished colour, which means it shifts to stage 2
// while tinting. Stage 0 (tex*light, set by gfx_texture) is always present.
static void gfx_apply_tev(void) {
	int base = gfx_tint_prev ? 2 : 1; // stages before the optional fog stage
	GX_SetNumTexGens(gfx_fog_prev ? 2 : 1);
	GX_SetNumTevStages(base + (gfx_fog_prev ? 1 : 0));

	if(gfx_tint_prev) {
		// prev (tex*light) * CLR1 (per-vertex biome tint); alpha passes through.
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL,
					   GX_COLOR1A1);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC,
						 GX_CC_ZERO);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
						 GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
						 GX_CA_APREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
						 GX_TRUE, GX_TEVPREV);
	}

	if(gfx_fog_prev) {
		u8 fog_stage = gfx_tint_prev ? GX_TEVSTAGE2 : GX_TEVSTAGE1;
		GX_SetTevOp(fog_stage, GX_DECAL);
		GX_SetTevOrder(fog_stage, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
		GX_SetTevColorIn(fog_stage, GX_CC_CPREV, GX_CC_C0, GX_CC_TEXA,
						 GX_CC_ZERO);
	}
}

void gfx_fog(bool enable) {
	if(enable != gfx_fog_prev) {
		gfx_fog_prev = enable;
		gfx_apply_tev();
	}
}

// Enable/disable the per-vertex biome tint channel for the chunk render pass.
// Wraps only chunk meshes (GX_VTXFMT4 lists); other geometry stays single-
// channel. No-op-safe to bracket the world render with.
void gfx_tint(bool enable) {
	if(enable == gfx_tint_prev)
		return;
	gfx_tint_prev = enable;

	if(enable) {
		GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
		GX_SetNumChans(2);
		// Channel 1 is a straight pass-through of the vertex CLR1 (no GX light).
		GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0,
					   GX_DF_NONE, GX_AF_NONE);
	} else {
		GX_SetVtxDesc(GX_VA_CLR1, GX_NONE);
		GX_SetNumChans(1);
	}

	gfx_apply_tev();
}

// audit #17f: emissive colour-tint overlay for textured ENTITY draws (creeper white
// swell flash + mob red hurt flash). FIFO-SAFETY is the whole point of this design --
// see CLAUDE.md's hang history (a render path that left the CLR0 vertex descriptor in
// the wrong mode desynced the GX FIFO and HUNG the Wii). So this function changes NO
// vertex descriptor, NO vertex attribute format, and NO colour-channel count. The mob
// keeps drawing as GX_VTXFMT0 (POS s16 / CLR0 INDEX8 light / TEX0 u8) byte-for-byte.
// The ONLY new GX state is a second TEV stage + two TEV colour registers, all of
// which are reset when amount==0 (the caller brackets every tinted mob with a final
// gfx_entity_tint(.,.,.,0) so the next draw sees the exact single-stage state it
// expects). amount==0 early-outs to the current state and adds nothing.
//
// Wiring (the unambiguous register-colour variant -- every input is a known source,
// so the lerp algebra is trivially checkable and never reads the wrong channel):
//   GX colour op computes  out = d + (1-c)*a + c*b
//   a = CPREV   (stage-0 result = texture * day/night light)
//   b = C1      (GX_TEVREG1 = the tint colour r,g,b)
//   c = KONST   (the colour KONST input; we point its per-stage KColorSel at K0's
//               ALPHA replicated into rgb = (t,t,t), so c == the lerp amount t)
//   d = ZERO
//   => out = (1 - t)*CPREV + t*tint     -- exact lerp toward the tint colour.
// t=0 => out = CPREV (model identical to today). Alpha passes through unchanged
// (APREV) so the alpha-test (GX_GEQUAL 16) still culls the mob's transparent texels.
//
// SUBTLE but CRITICAL: a TEV colour input GX_CC_KONST reads the per-stage KColorSel
// (set by GX_SetTevKColorSel), NOT the alpha selector. So to feed the scalar amount
// t into input c we point KColorSel at GX_TEV_KCSEL_K0_A, which replicates K0.alpha
// across rgb. (KAlphaSel would only matter if a GX_CA_KONST alpha input were used --
// it isn't here, alpha just passes APREV through.)
//
// NOTE GX_TEVREG0 is owned by gfx_fog_color() -- we use GX_TEVREG1 so fog is never
// clobbered. Fog is off in the entity pass anyway, but REG1 keeps it clean regardless.
void gfx_entity_tint(float r, float g, float b, float amount) {
	if(amount < 0.0f)
		amount = 0.0f;
	if(amount > 1.0f)
		amount = 1.0f;

	if(amount <= 0.0f) {
		// GRACEFUL-DEGRADE / RESTORE PATH. Return the TEV pipeline to the single-stage
		// tex*light default the rest of the frame (and every other mob/world/item draw)
		// expects. This MUST fully undo everything the amount>0 branch set, because a
		// stale 2-stage config would run an undefined stage 1 on the next draw -> wrong
		// colours or a hang. This branch is also the "no tint" state, so a tint amount
		// of 0 is byte-identical to today.
		//
		// DIRTY-GUARD: if the 2nd stage was never raised (the common case -- an untinted
		// mob, or the mandatory reset call after an already-untinted draw), there is
		// NOTHING to undo, so early-out and touch ZERO GX registers. This makes the
		// untinted path a true no-op instead of paying ~5 redundant GP register writes
		// per mob per frame (matches the gfx_fog/gfx_tint _prev-guard house pattern).
		if(!gfx_entity_tint_active)
			return;
		gfx_entity_tint_active = false;
		// IMPORTANT BASELINE ASSUMPTION: entities are drawn with fog AND the biome tint
		// OFF (main.c brackets the entity pass with gfx_fog(false), and gfx_tint is the
		// chunk-pass-only channel), so single-stage tex*light IS the correct pipeline to
		// restore here -- the state the ENTITY PASS expects. Right after the pass main.c
		// calls gfx_fog(true), which re-runs gfx_apply_tev() and rebuilds the multi-stage
		// fog/tint layout from scratch, self-healing any state this hardcoded restore
		// bypassed. (If an entity were ever drawn with fog/tint ON, this restore would be
		// wrong and should instead delegate to gfx_apply_tev() -- not the case today.)
		GX_SetNumTevStages(1);                  // back to one stage (MOST important)
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE); // == gfx_texture(true): tex * light
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0,
					   GX_COLOR0A0);            // == the gfx_setup stage-0 order
		// Belt-and-braces: reset the konst selectors we touch so any later code that
		// raises the stage count (e.g. gfx_apply_tev's biome/fog stages) starts from a
		// benign konst selection rather than our K0-alpha pick.
		GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_1);
		GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_1);
		// NOTE: STAGE1's ColorIn/AlphaIn/Op/Order are deliberately left RESIDENT (only the
		// stage COUNT drops to 1, so stage 1 simply isn't executed). This is safe because
		// every later consumer that bumps NumTevStages>=2 -- gfx_apply_tev's biome+fog
		// stages -- fully RE-SPECIFIES stage 1 before use. Any FUTURE multi-stage consumer
		// MUST likewise fully configure STAGE1; it must not inherit this tint's leftover
		// lerp config.
		// The vertex descriptor was NEVER changed here, so there is nothing descriptor-
		// side to undo (this is the key FIFO-safety property). gfx_lighting(true) at the
		// end of each render_entity_* fn still guarantees CLR0=INDEX8 for the next draw.
		return;
	}

	// PRECONDITION (held by the call-site ordering): the entity pass always BEGINS with
	// the pipeline single-stage -- gfx_setup leaves NumTevStages==1, and each frame
	// gfx_mode_gui()'s gfx_fog(false) (+ the line-353 gfx_fog(false) before entities)
	// re-runs gfx_apply_tev() back to single-stage before any mob is drawn. So stage 0
	// is already the tex*light MODULATE this stage-1 lerp reads as CPREV; we only ADD
	// stage 1 and never need to (re)assert stage 0. The dirty-guard reset above restores
	// that single-stage baseline, keeping the precondition true for the next mob.
	uint8_t tr = (uint8_t)(r * 255.0f + 0.5f);
	uint8_t tg = (uint8_t)(g * 255.0f + 0.5f);
	uint8_t tb = (uint8_t)(b * 255.0f + 0.5f);
	uint8_t t8 = (uint8_t)(amount * 255.0f + 0.5f); // lerp factor as a byte

	// Tint colour -> register C1 (input b). Lerp amount t -> konst K0 alpha (input c).
	GX_SetTevColor(GX_TEVREG1, (GXColor) {tr, tg, tb, 255});
	GX_SetTevKColor(GX_KCOLOR0, (GXColor) {0, 0, 0, t8});

	// Stage 0 is UNCHANGED (tex*light from gfx_texture's GX_MODULATE) -- do not touch
	// it. Configure stage 1 = lerp(CPREV, C1, t):
	//   The GX_CC_KONST 'c' colour input reads this stage's KColorSel; point it at
	//   K0's ALPHA replicated across rgb (GX_TEV_KCSEL_K0_A) so c == (t,t,t).
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0_A);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_C1, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
					 GX_TEVPREV);
	// Alpha MUST pass through unchanged (APREV) so alpha-test still culls transparent
	// texels -- otherwise you get opaque black boxes around the model.
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
					 GX_TEVPREV);
	// Stage 1 has no texture/colour of its own (c comes from KONST, b from C1); point
	// it at the null coord/map. The raster channel is irrelevant here; reuse COLOR0A0.
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);

	GX_SetNumTevStages(2); // enable the 2nd stage for THIS tinted draw only
	gfx_entity_tint_active = true; // arm the dirty-guard so the matching reset undoes it
}

// Per-channel emissive MULTIPLY for entity draws (Beta fleece tint = wool*colour per channel).
// Same FIFO-safe 2-stage design as gfx_entity_tint -- it changes NO vertex descriptor/format,
// only adds a register-colour TEV stage 1 -- but stage 1 here is a MULTIPLY (CPREV * C1) instead
// of a lerp, so it reproduces Beta's per-channel src*colour EXACTLY for every fleece colour (the
// lerp can only match the neutral R==G==B subset). Shares gfx_entity_tint_active + the SAME reset
// path: gfx_entity_modulate(1,1,1) -- or gfx_entity_tint(.,.,.,0) -- drops back to single stage.
//
// Wiring: GX colour op out = d + (1-c)*a + c*b. With a=ZERO, b=CPREV (stage-0 tex*light),
// c=C1 (GX_TEVREG1 = the fleece colour), d=ZERO  =>  out = C1 * CPREV  (per-channel multiply).
// Alpha passes APREV through so the alpha-test still culls transparent texels.
void gfx_entity_modulate(float r, float g, float b) {
	if(r < 0.0f) r = 0.0f;
	if(r > 1.0f) r = 1.0f;
	if(g < 0.0f) g = 0.0f;
	if(g > 1.0f) g = 1.0f;
	if(b < 0.0f) b = 0.0f;
	if(b > 1.0f) b = 1.0f;

	// Identity multiply (white fleece, or the reset) -> return to the single-stage tex*light
	// baseline. Reuse gfx_entity_tint's reset (it drops NumTevStages to 1 and clears the guard),
	// which is exactly what an identity modulate needs. Dirty-guarded inside (no-op if inactive).
	if(r >= 0.999f && g >= 0.999f && b >= 0.999f) {
		gfx_entity_tint(0.0f, 0.0f, 0.0f, 0.0f);
		return;
	}

	uint8_t cr = (uint8_t)(r * 255.0f + 0.5f);
	uint8_t cg = (uint8_t)(g * 255.0f + 0.5f);
	uint8_t cb = (uint8_t)(b * 255.0f + 0.5f);

	// Fleece colour -> register C1 (the 'c' multiplier input). Stage 0 (tex*light) is UNCHANGED.
	GX_SetTevColor(GX_TEVREG1, (GXColor) {cr, cg, cb, 255});
	// Stage 1 = CPREV * C1 (per-channel multiply): a=ZERO, b=CPREV, c=C1, d=ZERO.
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_C1, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
					 GX_TEVPREV);
	// Alpha passes through unchanged (APREV) so alpha-test still culls transparent texels.
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
					 GX_TEVPREV);
	// Reset the konst selector we share with the tint stage to a benign constant (the modulate
	// reads no konst); keeps the reset path's GX_TEV_KCSEL_1 assumption consistent.
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_1);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);

	GX_SetNumTevStages(2); // enable the 2nd stage for THIS modulated draw only
	gfx_entity_tint_active = true; // arm the dirty-guard so gfx_entity_tint(.,.,.,0) undoes it
}

void gfx_blending(enum gfx_blend mode) {
	switch(mode) {
		case MODE_BLEND:
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA,
							GX_LO_NOOP);
			break;
		case MODE_BLEND2:
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
			break;
		case MODE_BLEND3:
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_SRCCLR,
							GX_LO_NOOP);
			break;
		case MODE_INVERT:
			GX_SetBlendMode(GX_BM_LOGIC, GX_BL_ZERO, GX_BL_ZERO, GX_LO_INV);
			break;
		case MODE_OFF:
			GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
			break;
	}
}

void gfx_alpha_test(bool enable) {
	if(enable) {
		GX_SetAlphaCompare(GX_GEQUAL, 16, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_FALSE);
	} else {
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
}

void gfx_alpha_test_ref(bool enable, float ref) {
	if(enable) {
		// Beta's GL_GREATER (strict "greater than", 516) -- distinct from
		// gfx_alpha_test's own fixed GX_GEQUAL/16 threshold; see gfx.h's
		// citation. GX_SetAlphaCompare's reference is an 8-bit [0,255] byte;
		// ref is already clamped to [0,1] by every caller (glAlphaFunc's own
		// domain), so the round-trip cannot go out of range.
		uint8_t byte_ref = (uint8_t)roundf(ref * 255.0f);
		GX_SetAlphaCompare(GX_GREATER, byte_ref, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_FALSE);
	} else {
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
	}
}

void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	GX_SetColorUpdate(color ? GX_TRUE : GX_FALSE);

	gfx_depth_last = depth ? GX_TRUE : GX_FALSE;
	gfx_depth_test_last = depth_test ? GX_TRUE : GX_FALSE;

	GX_SetZMode(gfx_depth_test_last, gfx_depth_func_last, gfx_depth_last);
}

// Current viewport rect, cached so gfx_depth_range re-applies the ACTIVE
// viewport instead of silently resetting to full-screen. Found via the
// lberwa/CavEX fork analysis: their splitscreen hit exactly this landmine --
// any future second viewport (splitscreen, PiP, map render) would have its
// rect destroyed by the first held-item depth-range call. Full-screen default
// matches the gfx_setup init.
static float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;

void gfx_set_viewport(float x, float y, float w, float h) {
	/* Callers use logical UI/camera coordinates (802 wide in widescreen).
	 * GX addresses the physical EFB, normally 640 pixels wide. Cache the
	 * converted rectangle so held-item depth changes do not convert twice. */
	float sx = (float)screenMode->fbWidth / gfx_width();
	float sy = (float)screenMode->efbHeight / gfx_height();
	vp_x = x * sx;
	vp_y = y * sy;
	vp_w = w * sx;
	vp_h = h * sy;
	GX_SetViewport(vp_x, vp_y, vp_w, vp_h, 0, 1);
}

void gfx_depth_range(float near, float far) {
	GX_SetViewport(vp_x, vp_y, vp_w, vp_h, near, far);
}

void gfx_depth_func(enum depth_func func) {
	switch(func) {
		default:
		case MODE_LEQUAL: gfx_depth_func_last = GX_LEQUAL; break;
		case MODE_EQUAL: gfx_depth_func_last = GX_EQUAL; break;
	}

	GX_SetZMode(gfx_depth_test_last, gfx_depth_func_last, gfx_depth_last);
}

void gfx_texture(bool enable) {
	GX_SetTevOp(GX_TEVSTAGE0, enable ? GX_MODULATE : GX_PASSCLR);
}

void gfx_lighting(bool enable) {
	GX_SetVtxDesc(GX_VA_CLR0, enable ? GX_INDEX8 : GX_DIRECT);
}

void gfx_cull_func(enum cull_func func) {
	switch(func) {
		case MODE_NONE: GX_SetCullMode(GX_CULL_NONE); break;
		case MODE_BACK: GX_SetCullMode(GX_CULL_BACK); break;
		case MODE_FRONT: GX_SetCullMode(GX_CULL_FRONT); break;
	}
}

void gfx_scissor(bool enable, uint32_t x, uint32_t y, uint32_t width,
				 uint32_t height) {
	if(enable) {
		/* Convert shared edges, rather than independently rounding widths:
		 * adjacent split rectangles must cover the EFB without a gap. */
		uint32_t left = (uint64_t)x * screenMode->fbWidth / gfx_width();
		uint32_t top = (uint64_t)y * screenMode->efbHeight / gfx_height();
		uint32_t right = ((uint64_t)x + width) * screenMode->fbWidth / gfx_width();
		uint32_t bottom = ((uint64_t)y + height) * screenMode->efbHeight / gfx_height();
		GX_SetScissor(left, top, right - left, bottom - top);
	} else {
		GX_SetScissor(0, 0, 0x3FF, 0x3FF);
	}
}

void gfx_draw_lines(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors) {
	assert(vertices && colors);

	GX_Begin(GX_LINES, GX_VTXFMT3, vertex_count);

	for(size_t k = 0; k < vertex_count; k++) {
		GX_Position3s16(vertices[k * 3 + 0], vertices[k * 3 + 1],
						vertices[k * 3 + 2]);
		GX_Color4u8(colors[k * 4 + 0], colors[k * 4 + 1], colors[k * 4 + 2],
					colors[k * 4 + 3]);
		GX_TexCoord2u8(0, 0);
	}

	GX_End();
}

void gfx_draw_quads(size_t vertex_count, const int16_t* vertices,
					const uint8_t* colors, const uint16_t* texcoords) {
	assert(vertices && colors && texcoords);

	GX_Begin(GX_QUADS, GX_VTXFMT2, vertex_count);

	for(size_t k = 0; k < vertex_count; k++) {
		GX_Position3s16(vertices[k * 3 + 0], vertices[k * 3 + 1],
						vertices[k * 3 + 2]);
		GX_Color4u8(colors[k * 4 + 0], colors[k * 4 + 1], colors[k * 4 + 2],
					colors[k * 4 + 3]);
		GX_TexCoord2u16(texcoords[k * 2 + 0], texcoords[k * 2 + 1]);
	}

	GX_End();
}

void gfx_draw_quads_flt(size_t vertex_count, const float* vertices,
						const uint8_t* colors, const float* texcoords) {
	assert(vertices && colors && texcoords);

	GX_Begin(GX_QUADS, GX_VTXFMT1, vertex_count);

	for(size_t k = 0; k < vertex_count; k++) {
		GX_Position3f32(vertices[k * 3 + 0], vertices[k * 3 + 1],
						vertices[k * 3 + 2]);
		GX_Color4u8(colors[k * 4 + 0], colors[k * 4 + 1], colors[k * 4 + 2],
					colors[k * 4 + 3]);
		GX_TexCoord2f32(texcoords[k * 2 + 0], texcoords[k * 2 + 1]);
	}

	GX_End();
}
#endif /* !CAVEWII_RETIRE_GFX_TEST */
