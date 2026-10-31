/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).
*/

#ifndef GAME_AUTOPILOT_UNSTICK_H
#define GAME_AUTOPILOT_UNSTICK_H

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846 // strict C99 (glibc/devkitPPC) hides it
#endif

/*
	Opt-in walker stuck-detector for autopilot.c's scripted-walk mode
	(gstate.autopilot.mode == 0). Root cause this addresses: the maiden
	mp_lan_walk_10m run (tmp/wii_runs/staged_mp_lan_walk_10m_maiden_20260831)
	terrain-pinned the walker ~5 blocks from the mc173 MP spawn for 570
	heartbeats -- the walk driver is forced-forward + a slowly curving yaw
	with no notion of whether the player is actually making progress, so a
	pocket that happens to curve the walker back into the same obstruction
	never escapes on its own.

	Kept as a pure header (camera_bob.h precedent) for two reasons:
	  1. It has zero game_state/entity/cglm/gfx dependency, so it links into
		 ANY test target -- including the focused, no-engine-pool
		 test_hw_telemetry executable -- with no new manifest entry and no
		 new CMake target.
	  2. The whole detector is pure per-tick state transformation (position
		 in, yaw delta out); there is nothing here that benefits from being a
		 translation unit instead of an inline function.

	OPT-IN, and NOT wired through any "debug.*" config key: the ONLY writer
	of struct autopilot_unstick_state.armed is source/main.c reading
	hw_run.autopilot_unstick (source/game/hw_run.c), which only
	HW_RUN_PROFILE_MP_LAN_WALK_10M's hw_run_apply_profile case sets true.
	Every other profile (far_walk_6m included) and every hand-edited/capture
	config leaves armed == false forever, and every function below is a
	guaranteed no-op -- not just "does nothing observable", but does not even
	touch st's fields -- when armed is false. far_walk_6m's established
	hardware-evidence trajectories must stay byte-identical; see
	autopilot_unstick_tick's early-out below and autopilot.c's call site.
*/

// One stuck-detection window: 60 ticks at the client's fixed 20 Hz tick rate
// == 3.0 s, matching the task's "net displacement over the last 60 ticks
// (3s)" spec.
#define AUTOPILOT_UNSTICK_WINDOW_TICKS 60

// Minimum horizontal (X/Z) net displacement, in world blocks, required over
// one window to NOT be considered stuck. Vertical (Y) motion (jumping in
// place) deliberately does not count as progress -- that is exactly the
// "pinned in a pocket, jumping every 12 ticks" symptom this exists to catch.
#define AUTOPILOT_UNSTICK_MIN_DISPLACEMENT 1.0F

struct autopilot_unstick_state {
	// Set ONCE at arm time (main.c) from hw_run.autopilot_unstick; every
	// other field here is runtime-accumulated by autopilot_unstick_tick.
	bool armed;
	// False until the first tick call establishes a reference point -- the
	// detector cannot know "displacement since the window started" before it
	// has seen one real position, so the very first armed tick only records
	// the baseline and never fires.
	bool has_baseline;
	int window_start_tick;
	float window_start_x, window_start_z; // horizontal plane only
	// Cumulative escape heading correction (radians), ADDED to the walk
	// mode's own t*0.005F curve every tick (autopilot.c). Persistent by
	// design: the base curve is recomputed from scratch every tick, so a
	// one-shot yaw write would be overwritten on the very next tick and the
	// escape would never actually happen.
	float yaw_offset;
};

// Zero the detector and (re)apply `armed`. Call once at autopilot arm time
// (main.c, alongside the rest of gstate.autopilot's reset), NOT per-world or
// per-tick.
static inline void autopilot_unstick_reset(struct autopilot_unstick_state* st,
											bool armed) {
	if(!st)
		return;
	st->armed = armed;
	st->has_baseline = false;
	st->window_start_tick = 0;
	st->window_start_x = 0.0F;
	st->window_start_z = 0.0F;
	st->yaw_offset = 0.0F;
}

// Call once per client tick (autopilot.c's walk-mode branch only) with the
// current tick counter and the player's current horizontal position.
// Disarmed (st->armed == false): returns false immediately and touches NO
// field of *st -- the detector accumulates no state at all unless enabled,
// so a disarmed caller's `*st` stays exactly as autopilot_unstick_reset left
// it for the entire run.
//
// Armed: tracks a TUMBLING (non-overlapping) 60-tick window. Every
// AUTOPILOT_UNSTICK_WINDOW_TICKS ticks it compares the current position to
// the window's starting position; if the net horizontal distance is under
// AUTOPILOT_UNSTICK_MIN_DISPLACEMENT, it fires an escape: st->yaw_offset
// accumulates a large deterministic heading step derived from the tick
// counter (NO random source), so autopilot.c's next yaw computation
// (base-curve + yaw_offset) picks up the new heading starting the very next
// tick. The window always resets to a fresh reference point at the 60-tick
// boundary, whether or not it fired -- so a walker that finally frees itself
// keeps being judged against fresh progress, not a stale escape-time anchor.
// Returns true exactly on a tick that fired an escape.
static inline bool autopilot_unstick_tick(struct autopilot_unstick_state* st,
										  int tick, float x, float z) {
	if(!st || !st->armed)
		return false;

	if(!st->has_baseline) {
		st->has_baseline = true;
		st->window_start_tick = tick;
		st->window_start_x = x;
		st->window_start_z = z;
		return false;
	}

	int elapsed = tick - st->window_start_tick;
	if(elapsed < AUTOPILOT_UNSTICK_WINDOW_TICKS)
		return false;

	float dx = x - st->window_start_x;
	float dz = z - st->window_start_z;
	float dist = sqrtf(dx * dx + dz * dz);

	// Reset the window's reference point at this boundary regardless of the
	// outcome below (tumbling, not sliding -- see the function comment).
	st->window_start_tick = tick;
	st->window_start_x = x;
	st->window_start_z = z;

	if(dist >= AUTOPILOT_UNSTICK_MIN_DISPLACEMENT)
		return false; // real progress this window -- no escape needed

	// Deterministic escape heading (task spec): a hard ~90 degree turn plus
	// a tick-seeded 0..1.2 rad wobble (tick % 7 cycles every 7 ticks, so
	// consecutive 60-tick-apart firings land on DIFFERENT members of the
	// 7-step cycle) -- repeated triggers keep sweeping new headings instead
	// of all picking the identical quarter turn, so a pinned walker
	// eventually sweeps a heading that frees it.
	st->yaw_offset
		+= (float)M_PI * 0.5F + (float)(tick % 7) * 0.2F;
	return true;
}

#endif
