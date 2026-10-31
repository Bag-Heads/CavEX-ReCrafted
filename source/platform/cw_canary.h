/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef CW_CANARY_H
#define CW_CANARY_H

/* S5.3 hunt PROBE (temporary, ROUND 6): round 5's "how far did execution get"
 * markers dead-ended -- take 21 proved the crash SITE wanders when unrelated
 * diagnostic code changes elsewhere in the binary (addendum 4's finding,
 * reproduced), which is the signature of memory corruption whose victim is
 * layout-dependent, not a fixed-instruction hang. Progress markers answer
 * "how far did the thread get"; they cannot answer "who wrote here". This is
 * a corruption-DETECTION primitive instead: paint a known pattern into a
 * region we don't expect touched, then cheaply re-check it at every existing
 * marker point. A mismatch triangulates the writer by (a) which canary broke
 * -- ties it to a specific suspect structure/region -- and (b) which marker
 * first saw it broken -- brackets the writer's timing the same way the
 * round-5 progress markers bracketed pump/tick/render.
 *
 * Header-only (no new build-manifest entry) so it drops into any TU that
 * needs it: main.c (gstate guards + the main-stack watermark), game_state.c
 * (gstate guards), client_interface.c (clin_inbox/clin_empty_msg guards),
 * server_remote_transport.c (impl read_* scratch guard + the write/read
 * thread stack floors). All-PLATFORM_WII-gated inside the header itself, so
 * an unguarded #include is inert on PC.
 *
 * Zero I/O on the clean path: cw_canary_scan only opens/writes the caller's
 * trace file when a clobber is actually found, so arming this everywhere
 * costs a handful of u32 compares per call site, not new file I/O -- avoids
 * repeating round 5's own risk (multiple threads doing per-marker SD writes
 * every frame). Each call site passes ITS OWN thread's trace file (single-
 * writer-per-file discipline preserved: main thread -> cw_boot_trace.txt,
 * write thread -> cw_wt_trace.txt, read thread -> its own file), so a
 * clobber report from one thread can never race a normal-path write from
 * another. */
#ifdef PLATFORM_WII
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* S5.3 hunt PROBE (temporary, ROUND 10 cycle 2, coordinator/Sol plan item 2):
 * global kill switch for EVERY SD-writing tracer in the S5.3 hunt probe rig
 * (hw_boot_trace_write, cw_dispatch_trace*, cw_pump_lock_trace, cw_decode_
 * trace, cw_lock_trace, cw_write_trace, cw_world_reset_trace, cw_tel_trace*,
 * cw_canary_scan/cw_read_canary_scan's clobber-report path, main.c's inline
 * mp_state_changed + stack_watermark_hit writes) -- round 10 cycle 1 proved
 * removing ONE multi-writer SD collision (thread.c's cw_cond_trace) moved
 * the mp_connect_smoke wall a full frame later (4->5), so the remaining
 * wall may be another SD collision this round's audit hasn't found yet.
 * Set true right after the provider swap (main.c) so early-boot tracing is
 * unaffected (useful baseline) but everything in the post-swap window this
 * hunt cares about goes fully silent -- the decisive test for whether
 * eliminating SD I/O entirely lets the run progress further/succeed.
 * Declared here (not a new file) so every TU that already includes this
 * header sees it without a new build-manifest entry; defined once in
 * cw_canary.c-equivalent -- actually a plain extern + single definition in
 * main.c avoids needing a new translation unit at all. */
extern volatile bool g_cw_sd_trace_off;
#define CW_SD_TRACE_OFF() (g_cw_sd_trace_off)

#define CW_CANARY_MAGIC 0x43414E31u /* ASCII 'CAN1' -- distinct from
									  * CW_STACK_PAINT (0x5AFE5AFE) so a hex
									  * dump tells struct-guard clobbers apart
									  * from stack-watermark clobbers. */
#define CW_CANARY_WORDS 4

struct cw_canary {
	uint32_t words[CW_CANARY_WORDS];
};

static inline void cw_canary_paint(struct cw_canary* c) {
	for(int i = 0; i < CW_CANARY_WORDS; i++)
		c->words[i] = CW_CANARY_MAGIC;
}

/* -1 if every word still matches; else the index of the first mismatch. */
static inline int cw_canary_check(const struct cw_canary* c) {
	for(int i = 0; i < CW_CANARY_WORDS; i++)
		if(c->words[i] != CW_CANARY_MAGIC)
			return i;
	return -1;
}

/* One-shot budget per canary SITE (not global) -- a caller declares its own
 * `static unsigned budget` and passes a pointer so a clobber that repeats
 * every frame doesn't flood the SD once it starts (mirrors every other S5.3
 * hunt tracer's own small per-site budget). Pass NULL to log unconditionally
 * (rare; only for sites expected to fire at most once, like thread-entry
 * paint confirmations). */
static inline int cw_canary_scan(const char* path, const char* label,
								 const struct cw_canary* c,
								 unsigned* budget) {
	int idx = cw_canary_check(c);
	if(idx < 0)
		return -1;
	if(CW_SD_TRACE_OFF())
		return idx;
	if(budget) {
		if(*budget == 0)
			return idx;
		(*budget)--;
	}
	FILE* f = fopen(path, "a");
	if(f) {
		fprintf(f, "CANARY_CLOBBER label=%s idx=%d addr=%p got=%08lx\n",
				label, idx, (const void*)&c->words[idx],
				(unsigned long)c->words[idx]);
		fflush(f);
		fclose(f);
	}
	return idx;
}

/* Stack-watermark variant (generalizes the existing single-shot
 * cw_stack_paint/cw_stack_scan pair in server_remote_protocol.c, which
 * painted once around the now-proven-irrelevant CRPC_WORLD_RESET call):
 * paints from `floor` up to (current SP - margin), and a later scan reports
 * how many bytes from `floor` are still untouched. Re-arming this every
 * frame (paint at frame-top from the SHALLOWEST point in the call chain, so
 * the painted region covers every deeper call that frame) turns it from a
 * one-shot "how deep did THIS ONE call go" probe into a per-frame ceiling
 * measurement. stack_left shrinking toward 0 over the run, or dropping
 * sharply on the frame that dies, would be direct stack-overflow evidence;
 * round 5's numbers (server_remote_pump 8280B against a 128KB main budget)
 * argued against this, but a live measurement beats the static argument. */
static inline uint32_t* cw_stack_watermark_paint(uintptr_t floor,
												  uintptr_t sp,
												  uintptr_t margin) {
	uintptr_t lo = (floor + 3u) & ~(uintptr_t)3u;
	uintptr_t hi = (sp - margin) & ~(uintptr_t)3u;
	if(hi <= lo)
		return NULL;
	for(uint32_t* p = (uint32_t*)lo; p < (uint32_t*)hi; p++)
		*p = 0x5AFE5AFEu;
	return (uint32_t*)lo;
}

/* ROUND 7 FIX (Codex static review + a second ambiguity caught while fixing
 * it): the original version returned a plain byte count with no way to tell
 * "the deepest call reached exactly this far above the floor" (a real,
 * meaningful reading) apart from "nothing ever wrote anywhere near the floor
 * at all, the scan just walked cleanly out to sp" (which returns the SAME
 * KIND of largeish number, region-size-dependent, carrying no depth
 * information whatsoever) -- round 6's misreading of a single such number as
 * "93.7% of the stack used" came from exactly this conflation. Returns TRUE
 * only when a real mismatch was found (something demonstrably wrote between
 * floor and sp since painting) and sets *depth_from_floor to how far above
 * the floor that write reached -- SMALLER is WORSE (closer to the floor,
 * closer to the 128KB ceiling this floor sits just above). Returns FALSE
 * (region fully intact, *depth_from_floor unset) when nothing in [floor, sp)
 * was touched -- the unambiguous healthy case, regardless of the region's
 * raw size. */
static inline bool cw_stack_watermark_scan(uint32_t* paint_floor,
										   uintptr_t sp,
										   unsigned long* depth_from_floor) {
	if(!paint_floor)
		return false;
	uint32_t* p = paint_floor;
	while((uintptr_t)p < sp && *p == 0x5AFE5AFEu)
		p++;
	if((uintptr_t)p >= sp)
		return false; /* clean: the call chain never reached this deep */
	if(depth_from_floor)
		*depth_from_floor
			= (unsigned long)((uintptr_t)p - (uintptr_t)paint_floor);
	return true;
}

#endif /* PLATFORM_WII */
#endif /* CW_CANARY_H */
