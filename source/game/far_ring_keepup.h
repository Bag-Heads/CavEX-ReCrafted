/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_FAR_RING_KEEPUP_H
#define GAME_FAR_RING_KEEPUP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
	Pure "does worldgen keep up while walking" bookkeeping for the far-ring
	reach sweep (maintainer gate 4, 2026-09-03). Kept as a pure header
	(autopilot_unstick.h / mp_reconnect_soak_core.h precedent, same file-
	comment shape): zero gstate/world/network dependency, so it links into
	ANY test target with no new manifest entry and links against nothing
	heavier than <stdint.h>.

	The impure half (source/main.c's emit_hw_heartbeat) computes far_missing
	itself each heartbeat (world_far_missing_count, world.c/.h -- a bounded
	scan of the far annulus throttled to the heartbeat's own <=1 Hz cadence,
	see that function's doc comment for why a receive/evict-only incremental
	counter was rejected) and the player's current chunk coordinates, then
	feeds both plus a monotonic millisecond clock into far_ring_keepup_update
	below every heartbeat. This module only tracks:

	  - far_missing_max: the running high-water mark of far_missing, so a
	    brief spike during a fast walk isn't lost between 1 Hz samples.
	  - far_settle_ms: how long, after the LAST detected chunk-crossing,
	    far_missing took to fall back to 0 -- the direct answer to "does
	    worldgen keep up while walking", tracked as a running max across
	    every crossing this run (one slow catch-up mid-run must not be
	    hidden by a fast one before or after it).

	A "chunk-crossing" here is simply "the caller's (cx, cz) differs from
	the previous call's" -- sampled at heartbeat cadence, so a crossing that
	both happens and re-settles within under a second can be missed. That is
	the same instrument-caveat class as every other single-frame-sampled
	heartbeat field (docs/perf_frame_drops_rootcause_2026-09-03.md) and is
	acceptable here: the reach-sweep gate cares about SUSTAINED walking
	(far_walk_6m runs for minutes), not sub-second dips.
*/
struct far_ring_keepup_state {
	bool have_center;
	bool crossing_pending; // true from a crossing until far_missing hits 0
	int32_t last_cx, last_cz;
	uint32_t last_crossing_ms;
	int32_t missing_max;
	uint32_t settle_ms_max;
};

static inline void far_ring_keepup_reset(struct far_ring_keepup_state* st) {
	memset(st, 0, sizeof(*st));
}

// Call once per heartbeat sample (main.c's emit_hw_heartbeat) with the
// player's current chunk coordinates, the freshly (re)computed far_missing
// count, and a monotonic millisecond clock (any epoch, as long as it only
// ever increases across calls to the SAME state -- main.c uses its own
// first-heartbeat epoch). Safe to call even when no world is loaded/no far
// ring is active as long as the caller passes far_missing == 0 and resets
// the state (far_ring_keepup_reset) on world unload so a later reload starts
// clean rather than carrying a stale crossing/max from a previous session.
static inline void far_ring_keepup_update(struct far_ring_keepup_state* st,
										   int32_t cx, int32_t cz,
										   int32_t far_missing,
										   uint32_t now_ms) {
	if(far_missing > st->missing_max)
		st->missing_max = far_missing;

	bool crossed
		= st->have_center && (cx != st->last_cx || cz != st->last_cz);
	if(!st->have_center || crossed) {
		st->have_center = true;
		st->last_cx = cx;
		st->last_cz = cz;
		st->last_crossing_ms = now_ms;
		st->crossing_pending = true;
	}

	if(st->crossing_pending && far_missing == 0) {
		// now_ms is monotonic per the contract above, so this never
		// underflows for a well-behaved caller; guard it anyway rather than
		// let a caller bug wrap to a huge uint32 "settle time".
		uint32_t settle
			= now_ms >= st->last_crossing_ms ? now_ms - st->last_crossing_ms : 0;
		if(settle > st->settle_ms_max)
			st->settle_ms_max = settle;
		st->crossing_pending = false;
	}
}

static inline int32_t
far_ring_keepup_missing_max(const struct far_ring_keepup_state* st) {
	return st->missing_max;
}

static inline uint32_t
far_ring_keepup_settle_ms_max(const struct far_ring_keepup_state* st) {
	return st->settle_ms_max;
}

#endif
