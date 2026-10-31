/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_MP_RECONNECT_SOAK_CORE_H
#define GAME_MP_RECONNECT_SOAK_CORE_H

#include <stdbool.h>

/*
	Pure connect/dwell/disconnect/reconnect cycle state machine for
	HW_RUN_PROFILE_MP_RECONNECT_SOAK (S5.3 hunt plumbing, docs/mp_s5_wii_
	transport_design_2026-08-26.md 7b.3's "reconnect-cycle soak" -- the
	design doc's own "100 connect/disconnect cycles" S4 gate, ported to
	hardware at a smaller count first). Catches the LWP-thread-bookkeeping
	accumulation risk (design doc 3b) and the MEM2-margin exhaustion risk
	(design doc 4b) that only surface ACROSS repeated reconnects, never a
	single session -- mp_connect_smoke and mp_lan_walk_10m each only ever
	connect once.

	Kept as a pure header (autopilot_unstick.h precedent, same file-comment
	shape): zero gstate/screen/network dependency, so it links into ANY test
	target with no new manifest entry and no new CMake target, and every
	transition is directly unit-testable off the engine.

	The impure half (source/game/mp_reconnect_soak.h/.c, autopilot.c's
	relationship to autopilot_unstick.h precedent exactly) reads
	game_state_client_entities_active(&gstate) / gstate.remote_session each
	frame, feeds them into mp_reconnect_soak_state_tick() below, and performs
	the ONE action (if any) it returns: MRS_ACTION_DISCONNECT ->
	clin_send_unload_world() (the same SRPC_UNLOAD_WORLD ->
	server_remote_disconnect() path screen_pause.c's "Save and quit to
	title" and screen_multiplayer_connect.c's mid-connect Cancel already use
	-- server_remote_protocol.c's remote_process_srpc handles it FIRST,
	unconditionally, so no pause-toggle or in-screen state is required to
	send it headlessly); MRS_ACTION_RECONNECT -> screen_set(screen_
	multiplayer_connect) (the EXACT call main.c's boot-time mp_autoconnect
	entry already makes -- mpc_reset()'s own doc comment: "Re-request is
	idempotent", so repeated visits are safe).

	Design note on "quit to title": Beta's GuiIngameMenu "Save and quit to
	title" toggles pause first (screen_pause.c screen_pause_process_edges),
	which only matters for the PAUSED-menu UI path -- the actual session
	teardown is the SAME unconditional SRPC_UNLOAD_WORLD send this driver
	issues directly, so driving it headlessly (never opening screen_pause at
	all) reaches the identical server_remote_disconnect() call with no
	synthesized button presses. Documented deviation: real players go
	through the pause screen; this soak profile calls the underlying RPC
	the pause screen's button would have sent, the same "boundary-local RPC"
	screen_multiplayer_connect.c's own mid-connect Cancel already reuses for
	an equivalent reason (see that file's mpc_update comment).
*/

// Target successful connect/disconnect round-trips per run. Smaller than the
// design doc's PC-loopback "100 cycles" S4 gate on purpose -- this is a
// hardware soak (each connect costs real wall time), not a stress multiplier;
// see the duration comment in hw_run.c's HW_RUN_PROFILE_MP_RECONNECT_SOAK
// case for the budget math this target must fit inside.
#define MP_RECONNECT_SOAK_CYCLES_TARGET 6

// Seconds spent connected-and-idle before requesting the next disconnect.
// Long enough to accumulate several real heartbeats (proving the session is
// genuinely stable, not just momentarily up) before tearing it down again.
#define MP_RECONNECT_SOAK_DWELL_S 15.0f

// Bound on waiting for a connect (or reconnect) to land. Generous: cycle 1
// may pay the ~25-30s measured Wii DHCP/net_bringup settle (mp_connect_
// smoke's own figure); net_bringup_request() is idempotent and a no-op once
// NET_BRINGUP_READY latches (net_bringup.c), so later cycles are expected to
// be far faster in practice -- this timeout exists to catch a genuine wedge,
// not to be routinely approached.
#define MP_RECONNECT_SOAK_CONNECT_TIMEOUT_S 90.0f

// Bound on waiting for a requested disconnect to complete (gstate.
// remote_session clearing). main.c's own provider-swap deadline
// (MP_SWAP_DEADLINE_MS) is 15s; this sits comfortably above it with margin
// for the graceful-close belt to run its course.
#define MP_RECONNECT_SOAK_DISCONNECT_TIMEOUT_S 20.0f

enum mp_reconnect_soak_phase {
	MRS_PHASE_WAIT_CONNECT = 0,
	MRS_PHASE_DWELL,
	MRS_PHASE_WAIT_DISCONNECT,
	MRS_PHASE_DONE,
};

// Returned by mp_reconnect_soak_state_tick(): the ONE side effect (if any)
// the impure driver must perform this frame. Never more than one action per
// call -- every transition either returns an action or advances silently.
enum mp_reconnect_soak_action {
	MRS_ACTION_NONE = 0,
	MRS_ACTION_DISCONNECT, // caller must clin_send_unload_world()
	MRS_ACTION_RECONNECT,  // caller must screen_set(screen_multiplayer_connect)
};

struct mp_reconnect_soak_state {
	// Set ONCE at reset time from hw_run.mp_reconnect_soak; every other
	// field here is runtime-accumulated by mp_reconnect_soak_state_tick.
	bool armed;
	enum mp_reconnect_soak_phase phase;
	int cycles_completed;
	int cycles_target;
	float phase_elapsed_s;
	// Sticky once true: a phase exceeded its timeout, so the soak is
	// considered failed and mp_reconnect_soak_state_tick becomes a
	// permanent no-op (returns MRS_ACTION_NONE without touching any other
	// field) for the rest of the run -- "no wedge" is a hard PASS
	// requirement (design doc 7b.3), not something a later recovery could
	// paper over.
	bool wedged;
	// One-shot edge for the impure driver to log the wedge exactly once
	// (mp_reconnect_soak_state_tick sets this true on the SAME tick it sets
	// wedged; the driver clears it after reading).
	bool wedge_pending_report;
};

// Zero the state machine and (re)apply `armed`/`cycles_target`. Call once at
// arm time (main.c, hw_run.mp_reconnect_soak), NOT per-cycle.
// cycles_target <= 0 falls back to MP_RECONNECT_SOAK_CYCLES_TARGET.
static inline void mp_reconnect_soak_state_reset(
		struct mp_reconnect_soak_state* st, bool armed, int cycles_target) {
	if(!st)
		return;
	st->armed = armed;
	st->phase = MRS_PHASE_WAIT_CONNECT;
	st->cycles_completed = 0;
	st->cycles_target = cycles_target > 0 ? cycles_target
										   : MP_RECONNECT_SOAK_CYCLES_TARGET;
	st->phase_elapsed_s = 0.0f;
	st->wedged = false;
	st->wedge_pending_report = false;
}

// Call once per frame (mp_reconnect_soak.c's frame_tick, never per client
// tick -- this is wall-clock-timed, not simulation-timed) with:
//   connected     = game_state_client_entities_active(&gstate) this frame
//   remote_active = gstate.remote_session this frame
//   dt            = frame delta seconds (gstate.stats.dt)
// Disarmed / already wedged / already done: returns MRS_ACTION_NONE
// immediately and touches no other field, exactly like autopilot_unstick_
// tick's disarmed early-out.
//
// Cycle 1's WAIT_CONNECT phase is satisfied by the PRE-EXISTING boot-time
// mp_autoconnect entry (main.c screen_set(screen_multiplayer_connect) before
// the main loop even starts) -- this function issues NO action while merely
// waiting for `connected`, so it never redundantly re-requests the very
// first connect.
static inline enum mp_reconnect_soak_action mp_reconnect_soak_state_tick(
		struct mp_reconnect_soak_state* st, bool connected, bool remote_active,
		float dt) {
	if(!st || !st->armed || st->wedged || st->phase == MRS_PHASE_DONE)
		return MRS_ACTION_NONE;

	switch(st->phase) {
	case MRS_PHASE_WAIT_CONNECT:
		if(connected) {
			st->phase = MRS_PHASE_DWELL;
			st->phase_elapsed_s = 0.0f;
			return MRS_ACTION_NONE;
		}
		st->phase_elapsed_s += dt;
		if(st->phase_elapsed_s >= MP_RECONNECT_SOAK_CONNECT_TIMEOUT_S) {
			st->wedged = true;
			st->wedge_pending_report = true;
		}
		return MRS_ACTION_NONE;

	case MRS_PHASE_DWELL:
		if(!remote_active) {
			// The session is already gone (kick/drop/network loss) before we
			// asked to leave -- nothing to proactively disconnect. Fold into
			// WAIT_DISCONNECT, whose own gate (`!remote_active`) is already
			// true, so this completes the cycle on the very next tick rather
			// than wedging on a guess about why the session ended.
			st->phase = MRS_PHASE_WAIT_DISCONNECT;
			st->phase_elapsed_s = 0.0f;
			return MRS_ACTION_NONE;
		}
		if(!connected) {
			// Remote session still alive but entity physics is momentarily
			// inactive (e.g. a same-dim terrain hold, game_state.h's
			// remote_terrain_hold) -- freeze the dwell clock rather than
			// either counting stalled time as "dwelt" or disconnecting
			// early. Does not touch phase_elapsed_s: dwell resumes exactly
			// where it left off once connected is true again.
			return MRS_ACTION_NONE;
		}
		st->phase_elapsed_s += dt;
		if(st->phase_elapsed_s >= MP_RECONNECT_SOAK_DWELL_S) {
			st->phase = MRS_PHASE_WAIT_DISCONNECT;
			st->phase_elapsed_s = 0.0f;
			return MRS_ACTION_DISCONNECT;
		}
		return MRS_ACTION_NONE;

	case MRS_PHASE_WAIT_DISCONNECT:
		if(!remote_active) {
			st->cycles_completed++;
			if(st->cycles_completed >= st->cycles_target) {
				st->phase = MRS_PHASE_DONE;
				return MRS_ACTION_NONE;
			}
			st->phase = MRS_PHASE_WAIT_CONNECT;
			st->phase_elapsed_s = 0.0f;
			return MRS_ACTION_RECONNECT;
		}
		st->phase_elapsed_s += dt;
		if(st->phase_elapsed_s >= MP_RECONNECT_SOAK_DISCONNECT_TIMEOUT_S) {
			st->wedged = true;
			st->wedge_pending_report = true;
		}
		return MRS_ACTION_NONE;

	default:
		return MRS_ACTION_NONE;
	}
}

// True once the target cycle count has been reached OR the soak wedged --
// either way, mp_reconnect_soak_state_tick has nothing left to do.
static inline bool mp_reconnect_soak_state_done(
		const struct mp_reconnect_soak_state* st) {
	return st && (st->phase == MRS_PHASE_DONE || st->wedged);
}

#endif
