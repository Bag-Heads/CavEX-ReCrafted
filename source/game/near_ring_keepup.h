/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_NEAR_RING_KEEPUP_H
#define GAME_NEAR_RING_KEEPUP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
	Pure "does the near ring keep up" bookkeeping -- the near-ring mirror of
	far_ring_keepup.h (2026-09-03 maintainer ruling: "loading screen must not
	end until the near ring is installed AND meshed", "add near_settle_ms from
	spawn/teleport/dimension-switch"). Same file shape as far_ring_keepup.h
	(autopilot_unstick.h / mp_reconnect_soak_core.h precedent): a pure header,
	zero gstate/world/network dependency, so it links into any test target
	with no new manifest entry.

	The impure half (main.c's emit_hw_heartbeat AND screen_load_world.c's
	loading-screen gate) computes near_missing itself each call
	(world_near_ring_status, world.c/.h -- a bounded disk scan over Chebyshev
	<= MAX_VIEW_DISTANCE, see that function's doc comment for the cost bound)
	as missing-sections + mesh-pending-chunks (one combined count, matching
	far_missing's single-number shape: BOTH must be zero for the near ring to
	be truly ready, and treating them as one number is exactly what the
	loading-screen gate's "near_missing == 0" condition means) and the
	player's current chunk coordinates, then feeds both plus a monotonic
	millisecond clock into near_ring_keepup_update below. This module only
	tracks:

	  - missing_max: the running high-water mark of near_missing, so a brief
	    spike (a spawn, a teleport, a dimension switch, or just a fast walk)
	    isn't lost between samples.
	  - settle_ms_max: how long, after the LAST detected chunk-crossing,
	    near_missing took to fall back to 0 -- the direct answer to "does the
	    client's own build_chunks/mesher pipeline keep up with a moving near
	    ring", tracked as a running max across every crossing this run.

	A "crossing" here is "the caller's (cx, cz) differs from the previous
	call's" -- this fires on ordinary walking too, not just spawn/teleport/
	dimension-switch, which is a superset of what the maintainer asked for
	(those three are all ALSO chunk-coordinate changes) and, like far ring's
	same choice, is the simpler and more complete signal: every settle event
	the maintainer named is captured, plus every other one worth knowing
	about.
*/
struct near_ring_keepup_state {
	bool have_center;
	bool crossing_pending; // true from a crossing until near_missing hits 0
	int32_t last_cx, last_cz;
	uint32_t last_crossing_ms;
	int32_t missing_max;
	uint32_t settle_ms_max;
};

static inline void near_ring_keepup_reset(struct near_ring_keepup_state* st) {
	memset(st, 0, sizeof(*st));
}

// Call once per sample (main.c's emit_hw_heartbeat, or any other cadence a
// caller chooses -- unlike far ring this is also driven at loading-screen
// update cadence, which runs every frame) with the player's current chunk
// coordinates, the freshly (re)computed near_missing count (missing sections
// + mesh-pending chunks, see the header comment above), and a monotonic
// millisecond clock (any epoch, as long as it only ever increases across
// calls to the SAME state). Safe to call even when no world is loaded/no
// near ring is active as long as the caller passes near_missing == 0 and
// resets the state (near_ring_keepup_reset) on world unload so a later
// reload starts clean rather than carrying a stale crossing/max.
static inline void near_ring_keepup_update(struct near_ring_keepup_state* st,
											int32_t cx, int32_t cz,
											int32_t near_missing,
											uint32_t now_ms) {
	if(near_missing > st->missing_max)
		st->missing_max = near_missing;

	bool crossed
		= st->have_center && (cx != st->last_cx || cz != st->last_cz);
	if(!st->have_center || crossed) {
		st->have_center = true;
		st->last_cx = cx;
		st->last_cz = cz;
		st->last_crossing_ms = now_ms;
		st->crossing_pending = true;
	}

	if(st->crossing_pending && near_missing == 0) {
		uint32_t settle
			= now_ms >= st->last_crossing_ms ? now_ms - st->last_crossing_ms : 0;
		if(settle > st->settle_ms_max)
			st->settle_ms_max = settle;
		st->crossing_pending = false;
	}
}

static inline int32_t
near_ring_keepup_missing_max(const struct near_ring_keepup_state* st) {
	return st->missing_max;
}

static inline uint32_t
near_ring_keepup_settle_ms_max(const struct near_ring_keepup_state* st) {
	return st->settle_ms_max;
}

#endif
