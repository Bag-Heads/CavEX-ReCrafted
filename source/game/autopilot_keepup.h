/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).
*/

#ifndef GAME_AUTOPILOT_KEEPUP_H
#define GAME_AUTOPILOT_KEEPUP_H

#include <stdbool.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846 // strict C99 (glibc/devkitPPC) hides it
#endif

/*
	Deterministic keep-up-load walk driver for HW_RUN_PROFILE_KEEPUP_WALK_6M
	(source/game/hw_run.c), added because far_walk_6m's existing walk driver
	(autopilot.c's mode==0, "gstate.autopilot.yaw = (float)t * 0.005F") is
	*itself* fully deterministic given the tick counter, but its trajectory
	is a NEVER-RESETTING heading drift: at 0.005 rad/tick * 20 ticks/s the
	walker follows a constant-curvature path (radius == forward_speed/omega,
	~43 blocks on flat ground) that necessarily loops back through the
	spawn neighborhood roughly every ~63s (2*pi/omega) -- PROJECT_MEMORY's
	"both autopilots walk out then wander back near spawn" observation is
	this circle, not a bug in the tick math. Because far_walk_6m's harness
	boots via the "+new" autoload sentinel (screen_select_world.c's
	screen_sworld_create_new: "a fresh level.dat, RANDOM seed"), every run
	rides that circle over a completely different, randomly seeded terrain
	-- hills/water/cliffs/ravines deflect the real collision-driven path off
	the nominal circle by a different, terrain-dependent amount each run,
	and an unlucky fall/drown can kill the walker outright, at which point
	autopilot.c's death-recovery presses Respawn and the position snaps back
	to spawn (see tests/test_autopilot_death_recovery.c). The measured
	displacement (tools/wii_telemetry_server.py's walk_min/max bounding box)
	is therefore a function of which random world a run happened to draw,
	not of the (already-deterministic) input script -- exactly the 24/67/82
	block spread PROJECT_MEMORY records across three otherwise-identical
	far_walk_6m runs.

	This driver removes the SCRIPT's own contribution to that variance:
	  - the heading is a FIXED constant (never drifts with tick), so the
		walker cannot re-enter its own past neighborhood by construction --
		there is no lap to complete, unlike the circle above;
	  - the stuck-escape is a bounded, non-escalating, non-wobbling deviation
		(see below) instead of autopilot_unstick.h's accumulating "tick % 7"
		heading wobble (which far_walk_6m never armed anyway -- only
		HW_RUN_PROFILE_MP_LAN_WALK_10M does, hw_run.c's own comment -- but
		this driver is a clean, separate mode specifically so no consumer of
		the legacy walk driver's established trajectory is touched).
	Residual, DOCUMENTED limitation: this driver controls the SCRIPT only.
	It does not pin the world seed, so two runs still generate different
	terrain under the same fixed heading -- a real fall/drown death is still
	possible on unlucky terrain and still triggers the existing Respawn
	recovery, which still resets position. What this driver removes is the
	self-inflicted, terrain-independent source of variance (the endless
	heading drift and the unbounded escape wobble); it does not (and, short
	of pinning a seed, cannot) make identical input produce an identical
	final position across two different randomly generated worlds.

	Kept as a pure header for the same reasons as autopilot_unstick.h
	(zero game_state/entity/cglm/gfx dependency -- links into any test
	target with no new manifest entry).
*/

/*
	ESCAPE LADDER (2026-09-04, cand10/walker-dev -- measured, not guessed).

	The first version of this driver carried ONE escape: a fixed +90 degree
	sidestep for 10 ticks, repeated verbatim every time the 60-tick window
	closed with no progress. Both a real-Wii run (tmp/wii_runs/
	staged_keepup_walk_6m_20260904_094826: 47 blocks in 40 s, then ~120 s
	pinned at one spot) and a PC trace of the identical profile on a fixed
	seed (--cw-seed 12345, CAVEWII_TRACE=1 state export) show that single
	sidestep is not enough. The PC trace pins the mechanism exactly:

	  seed 12345, t=70.8s .. 122.8s: pos frozen at (0.7, 70.62, 266.7),
	  on_ground=true, in_water=false, look hits a solid block at
	  (0,70,267) -- a wall dead ahead on the fixed +Z heading. The JUMP
	  pulse fires (y ticks 70.62 -> 70.81 and straight back) but never
	  clears 0.2 blocks: there is a CEILING one block over the player's
	  head (the 5x5 height window reads -3 at dz=-1 and +1 at dz=+2 --
	  the walker had dropped into an overhung notch). The +90 sidestep
	  fires too (orient yaw is observed at 90.0 during the freeze) and
	  buys 0.2 blocks of x before that side blocks as well.

	So the failing geometry is a CORNER under an overhang: forward blocked,
	up blocked, and ONE lateral direction blocked. A single non-escalating
	sidestep can never leave it, by construction -- which is why the old
	driver sat there for the rest of the run.

	The fix is a bounded, strictly deterministic LADDER: each time a probe
	window closes with no progress the driver escalates to the next rung,
	and any window that DOES make progress drops it straight back to rung 0.
	The rungs are, in order, jump-hold -> +90 short sidestep -> -90 short
	sidestep -> 180 reverse -> +90 long detour -> -90 longer detour -> +90
	ridge-follow -> -90 longer ridge-follow; after the eighth the ladder
	cycles back to the first, so the deviation stays
	bounded (there is no rung that turns further, walks longer, or
	accumulates anything the way autopilot_unstick.h's "tick % 7" wobble
	does). Every rung's duration and heading offset is a compile-time
	constant: no RNG, no wall-clock, no dependence on the tick counter's
	absolute value -- only on how many consecutive probe windows have failed.
	Cheap rungs first: a plain 1-block step costs 10 ticks of jump, not a
	120-tick detour. The reverse sits in the middle -- after both cheap
	laterals, before the two expensive detours -- because it is the only
	rung that gives back forward progress.
*/

// Progress probe window: 20 ticks == 1.0 s at the client's fixed 20 Hz tick
// rate. Shorter than the old 60-tick window on purpose -- the ladder needs
// several rungs inside the time a human would call "stuck", and the ladder's
// own thresholds are expressed in whole probe windows below.
#define AUTOPILOT_KEEPUP_PROBE_TICKS 20

// "Not making progress" bar for ONE probe window. Beta's normal walk is
// ~4.3 blocks/s and this driver measures ~3.8 blocks/s on flat ground, so a
// walker that is genuinely moving clears 1.0 block in 1.0 s with a wide
// margin, while one grinding against a wall does not.
#define AUTOPILOT_KEEPUP_MIN_DISPLACEMENT 1.0F

// How far from where it FIRST got blocked the walker must be before the
// driver calls the episode over and drops the ladder back to rung 0.
//
// MEASURED ROOT CAUSE (PC trace, --cw-seed 12345; the same corner the real
// Wii run staged_keepup_walk_6m_20260904_104842 froze in, at the same
// coordinates -- see the top comment). The first ladder judged "freed" by
// per-window displacement alone, and that is exactly wrong for a ladder whose
// rungs MOVE THE WALKER: the 180 reverse rung always produces displacement,
// so every episode ran 1 -> 2 -> 3 -> 4 and then reset to rung 0 on the
// reverse's own retreat, walked back into the identical wall, and started
// over. Rungs 5 and 6 -- the only ones long enough to leave an obstacle of
// terrain scale -- were therefore UNREACHABLE, and the walker ground through
// that 4-rung loop for the rest of the run (52 s pinned; adding rungs 5/6
// alone changed nothing, which is what exposed the real defect).
//
// Anchoring the "am I free yet?" test to the position where the episode
// STARTED fixes it: a rung's own retreat cannot clear the episode, because
// every rung's reach is smaller than this radius. 12 blocks is ~3 s of free
// walking at the measured ~3.8 blocks/s -- comfortably past the reverse
// rung's ~6 blocks, and short enough that a genuinely freed walker ends the
// episode within a couple of probe windows.
#define AUTOPILOT_KEEPUP_CLEAR_RADIUS 12.0F

// Ladder rung count. After the last rung the ladder wraps to rung 1, so the
// escape is a fixed 8-element cycle and never escalates past it.
//
// SIZED BY MEASUREMENT, not taste (PC trace, --cw-seed 12345, the corner at
// (0.5,70.6,266.7) described above). A first cut of this ladder used four
// rungs with 2- and 4-block laterals; it DID free the walker where the old
// single sidestep never did (52 s pinned -> 32.7 s), but 32.7 s is still ~3.6
// full ladder cycles, because a 2..4 block lateral is not enough to round the
// obstacle: the walker slides back onto the fixed heading still inside the
// same notch. The two long lateral rungs below (~11 and ~23 blocks, opposite
// sides) are what actually clear an obstacle of terrain scale, and they are
// deliberately LAST -- the cheap rungs are still tried first, so a plain
// 1-block step costs 10 ticks, not 120.
#define AUTOPILOT_KEEPUP_STAGE_COUNT 8

/* Anchor-only local route memory. A recently visited cell loses to an
 * unvisited safe neighbor, so retreating from a ledge does not loop back
 * into the same dead end. Fixed storage; no search tree or world edits. */
#define AUTOPILOT_ROUTE_HISTORY 64
struct autopilot_route_state {
	bool has_target;
	float x, y, z;
	unsigned ticks, count, next;
	int32_t recent_x[AUTOPILOT_ROUTE_HISTORY], recent_z[AUTOPILOT_ROUTE_HISTORY];
};

// Rung 1 -- jump hold, heading unchanged (clears a 1-block step / a lip the
// base driver's every-12th-tick pulse can miss the phase of).
#define AUTOPILOT_KEEPUP_JUMP_TICKS 10
// Rung 2 -- +90 sidestep, ~4 blocks (20 ticks * ~3.8 blocks/s * 0.05 s).
#define AUTOPILOT_KEEPUP_SIDESTEP_A_TICKS 20
// Rung 3 -- -90 sidestep, ~8 blocks: twice rung 2, and the OPPOSITE side, so
// a corner that blocks +90 is still escapable.
#define AUTOPILOT_KEEPUP_SIDESTEP_B_TICKS 40
// Rung 4 -- 180 reverse for 1.5 s (~6 blocks): backs out of a dead end. The
// one rung that gives back forward progress, so it is tried after both cheap
// laterals and before the two expensive ones -- and kept SHORTER than the
// episode clear radius below, so backing up can never look like an escape.
#define AUTOPILOT_KEEPUP_REVERSE_TICKS 30
// Rung 5 -- +90 detour, ~11 blocks: a lateral long enough to leave the
// obstacle's footprint before the fixed heading resumes.
#define AUTOPILOT_KEEPUP_DETOUR_A_TICKS 60
// Rung 6 -- -90 detour, ~23 blocks: the same, twice as long, other side.
#define AUTOPILOT_KEEPUP_DETOUR_B_TICKS 120
// Rungs 7 and 8 -- RIDGE FOLLOWING, ~38 and ~76 blocks of pure lateral.
//
// Measured need (PC trace, --cw-seed 12345, 360 s): after the ladder frees
// the walker from the (0.7,266.7) corner it runs clean to z=436, where it
// meets a rising slope -- and there BOTH short laterals and both detours are
// obstructed too, so the walker never gets the 12 blocks from its anchor that
// would close the episode, and it grinds for the remaining 150 s. A wall of
// terrain scale needs a lateral of terrain scale: pressing sideways for
// 10-20 s finds the gap or the shoulder. For a keep-up-LOAD profile that is
// not a compromise -- walking along a ridge streams exactly as many new
// chunks as walking through it would have.
#define AUTOPILOT_KEEPUP_RIDGE_A_TICKS 200
#define AUTOPILOT_KEEPUP_RIDGE_B_TICKS 400

// Heading offsets, in the same radian space as gstate.autopilot.yaw.
#define AUTOPILOT_KEEPUP_YAW_PLUS90 ((float)M_PI * 0.5F)
#define AUTOPILOT_KEEPUP_YAW_MINUS90 (-(float)M_PI * 0.5F)
#define AUTOPILOT_KEEPUP_YAW_REVERSE ((float)M_PI)

// One ladder rung, resolved from the rung index alone. Pure: no state, no
// clock, no RNG -- this is the table test_hw_telemetry.c pins byte-for-byte.
struct autopilot_keepup_action {
	int ticks;			// how long the maneuver is held
	float yaw_offset;	// added to the driver's fixed base heading
	bool jump_hold;		// force IB_JUMP held for the whole maneuver
};

static inline struct autopilot_keepup_action
autopilot_keepup_stage_action(int stage) {
	struct autopilot_keepup_action a;
	a.ticks = 0;
	a.yaw_offset = 0.0F;
	a.jump_hold = false;
	switch(stage) {
		case 1:
			a.ticks = AUTOPILOT_KEEPUP_JUMP_TICKS;
			a.jump_hold = true;
			break;
		case 2:
			a.ticks = AUTOPILOT_KEEPUP_SIDESTEP_A_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_PLUS90;
			a.jump_hold = true;
			break;
		case 3:
			a.ticks = AUTOPILOT_KEEPUP_SIDESTEP_B_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_MINUS90;
			a.jump_hold = true;
			break;
		case 4:
			// No jump on the reverse: backing out of a pit or off a ledge is
			// the one maneuver where hopping can put the walker somewhere
			// worse than where it started.
			a.ticks = AUTOPILOT_KEEPUP_REVERSE_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_REVERSE;
			break;
		case 5:
			a.ticks = AUTOPILOT_KEEPUP_DETOUR_A_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_PLUS90;
			a.jump_hold = true;
			break;
		case 6:
			a.ticks = AUTOPILOT_KEEPUP_DETOUR_B_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_MINUS90;
			a.jump_hold = true;
			break;
		case 7:
			a.ticks = AUTOPILOT_KEEPUP_RIDGE_A_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_PLUS90;
			a.jump_hold = true;
			break;
		case 8:
			a.ticks = AUTOPILOT_KEEPUP_RIDGE_B_TICKS;
			a.yaw_offset = AUTOPILOT_KEEPUP_YAW_MINUS90;
			a.jump_hold = true;
			break;
		default: break; // rung 0 / out of range: no maneuver
	}
	return a;
}

// The whole escape SCHEDULE as a pure function of consecutive blocked ticks:
// under one probe window -> rung 0 (no escape); 20..39 -> rung 1; 40..59 ->
// rung 2; 60..79 -> rung 3; 80..99 -> rung 4; 100..119 -> rung 5; 120..139 ->
// rung 6; 140..159 -> rung 7; 160..179 -> rung 8; 180..199 -> rung 1 again,
// and so on. Bounded and cyclic, never escalating past
// AUTOPILOT_KEEPUP_STAGE_COUNT.
static inline int autopilot_keepup_stage_for_blocked(int blocked_ticks) {
	if(blocked_ticks < AUTOPILOT_KEEPUP_PROBE_TICKS)
		return 0;
	int n = blocked_ticks / AUTOPILOT_KEEPUP_PROBE_TICKS; // 1, 2, 3, ...
	return ((n - 1) % AUTOPILOT_KEEPUP_STAGE_COUNT) + 1;
}

struct autopilot_keepup_state {
	// Set ONCE at arm time (main.c), mirroring autopilot_unstick_state.armed.
	bool armed;
	bool has_baseline;
	int window_start_tick;
	float window_start_x, window_start_z; // horizontal plane only
	// Consecutive ticks of the current stuck EPISODE, in whole probe windows.
	// Zeroed when the walker gets AUTOPILOT_KEEPUP_CLEAR_RADIUS away from
	// where the episode began.
	int blocked_ticks;
	// Where the walker was standing when the current episode started. Only
	// meaningful while blocked_ticks > 0.
	bool in_episode;
	float anchor_x, anchor_z;
	// Rung most recently engaged (0 = none). Diagnostic + test handle.
	int stage;
	// True for exactly the current rung's `ticks` ticks after it fires.
	bool escaping;
	int escape_ticks_left;
	float yaw_offset; // rung's offset while escaping, else 0.0F
	bool jump_hold;	  // rung's jump flag while escaping, else false
};

static inline void
autopilot_keepup_reset(struct autopilot_keepup_state* st, bool armed) {
	if(!st)
		return;
	st->armed = armed;
	st->has_baseline = false;
	st->window_start_tick = 0;
	st->window_start_x = 0.0F;
	st->window_start_z = 0.0F;
	st->blocked_ticks = 0;
	st->in_episode = false;
	st->anchor_x = 0.0F;
	st->anchor_z = 0.0F;
	st->stage = 0;
	st->escaping = false;
	st->escape_ticks_left = 0;
	st->yaw_offset = 0.0F;
	st->jump_hold = false;
}

// Call once per client tick (autopilot.c's keepup-mode branch) with the
// current tick counter and the player's current horizontal position.
// Disarmed: returns false immediately and touches NO field of *st.
//
// Armed: while a maneuver is in progress, counts it down to completion and
// otherwise does nothing (no window bookkeeping while escaping -- the probe
// window resumes fresh the instant the maneuver ends, so displacement
// achieved DURING the escape neither retroactively cancels it nor carries
// stale distance into the next window). While not escaping, tracks a
// tumbling AUTOPILOT_KEEPUP_PROBE_TICKS window.
//
// OUTSIDE an episode, a window that clears AUTOPILOT_KEEPUP_MIN_DISPLACEMENT
// is a walker walking normally and does nothing; a window that does not OPENS
// an episode anchored at the current position and engages rung 1.
//
// INSIDE an episode the per-window displacement is IGNORED and the anchor
// decides: once the walker is AUTOPILOT_KEEPUP_CLEAR_RADIUS from where the
// episode began it is genuinely free, so the episode closes and the ladder
// drops to rung 0; otherwise the window's ticks are added to blocked_ticks
// and the next rung fires. Returns true exactly on the tick a rung is
// engaged.
static inline bool
autopilot_keepup_tick(struct autopilot_keepup_state* st, int tick, float x,
					   float z) {
	if(!st || !st->armed)
		return false;

	if(st->escaping) {
		st->escape_ticks_left--;
		if(st->escape_ticks_left <= 0) {
			st->escaping = false;
			st->yaw_offset = 0.0F;
			st->jump_hold = false;
			// Fresh baseline the instant the maneuver ends, so the next
			// window judges progress made AFTER the escape, not before it.
			st->has_baseline = true;
			st->window_start_tick = tick;
			st->window_start_x = x;
			st->window_start_z = z;
		}
		return false;
	}

	if(!st->has_baseline) {
		st->has_baseline = true;
		st->window_start_tick = tick;
		st->window_start_x = x;
		st->window_start_z = z;
		return false;
	}

	int elapsed = tick - st->window_start_tick;
	if(elapsed < AUTOPILOT_KEEPUP_PROBE_TICKS)
		return false;

	// Reset the window's reference point at this boundary regardless of the
	// outcome below (tumbling, not sliding).
	float wx = st->window_start_x, wz = st->window_start_z;
	st->window_start_tick = tick;
	st->window_start_x = x;
	st->window_start_z = z;

	if(st->in_episode) {
		// Already escaping an obstacle: "am I free yet?" is measured from the
		// episode ANCHOR, never from the last window -- see the
		// AUTOPILOT_KEEPUP_CLEAR_RADIUS comment for the run this fixes.
		float adx = x - st->anchor_x;
		float adz = z - st->anchor_z;
		if(sqrtf(adx * adx + adz * adz) >= AUTOPILOT_KEEPUP_CLEAR_RADIUS) {
			st->in_episode = false;
			st->blocked_ticks = 0;
			st->stage = 0;
			return false;
		}
	} else {
		float dx = x - wx;
		float dz = z - wz;
		if(sqrtf(dx * dx + dz * dz) >= AUTOPILOT_KEEPUP_MIN_DISPLACEMENT)
			return false; // walking normally -- nothing to escape
		// First failing window: open an episode anchored right here.
		st->in_episode = true;
		st->anchor_x = x;
		st->anchor_z = z;
		st->blocked_ticks = 0;
	}

	st->blocked_ticks += elapsed;
	st->stage = autopilot_keepup_stage_for_blocked(st->blocked_ticks);
	struct autopilot_keepup_action act
		= autopilot_keepup_stage_action(st->stage);
	if(act.ticks <= 0)
		return false; // unreachable for a real rung; never engage a no-op
	st->escaping = true;
	st->escape_ticks_left = act.ticks;
	st->yaw_offset = act.yaw_offset;
	st->jump_hold = act.jump_hold;
	return true;
}

#endif
