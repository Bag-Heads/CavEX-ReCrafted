/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX. GPLv3 (inherited).

	Autonomous test autopilot. Drives the REAL local player -- it forces the movement
	buttons (via input_held) and authors the camera yaw/pitch (via camera_attach) so the
	genuine, byte-faithful movement/collision physics walk the character with NO human input
	and NO teleport. Gated by debug.autopilot=on (default off); used by the headless PC test
	harness to survey the world (esp. the distant LOD skin) and capture screenshots unaided.
*/

#ifndef AUTOPILOT_H
#define AUTOPILOT_H

#include <stdbool.h>
#include <stdint.h>

#include "../platform/input.h"

struct entity;
struct world;

/* Read-only split_dims diagnostic: a 5x5 terrain sample centered on the
 * client body, at floor/feet/head height in the body's owning world. '?' is
 * an uninstalled column; P portal, O obsidian, '.' air, '#' other solid.
 * This never drives input or alters the world. */
struct autopilot_portal_geometry {
	int x, z, feet_y;
	char cells[3][5][6];
	char portal_axis;
	bool body_collision;
};
bool autopilot_portal_geometry_sample(const struct entity* body,
	struct autopilot_portal_geometry* out);

/* split_dims proof input only: remember the actual arrival pane in the body's
 * owning world, then verify a short, level, supported corridor back to it.
 * A failed probe must leave the driver stopped; it never edits terrain/pose. */
struct autopilot_portal_return_anchor {
	struct world* world;
	int x[2], z[2], feet_y;
};
struct autopilot_portal_return_step {
	int heading_deg;
	float target_x, target_z;
	bool inside;
};
bool autopilot_portal_return_anchor(const struct entity* body,
	struct autopilot_portal_return_anchor* out);
bool autopilot_portal_return_step(const struct entity* body,
	const struct autopilot_portal_return_anchor* anchor,
	struct autopilot_portal_return_step* out);
bool autopilot_portal_return_rearmed(uint32_t observed_tick,
	uint32_t current_tick);
/* Input seam for that checked corridor only. Rechecks immediately before each
 * physics tick; a blocked path forces no forward/jump/sneak input. */
void autopilot_script_portal_walk(bool on,
	const struct autopilot_portal_return_anchor* anchor);

void autopilot_script_portal_patrol(const struct autopilot_portal_return_anchor* anchor,
	unsigned endpoint);

// Advance once per 20Hz tick, world loaded. The terrain-safe route runs BEFORE
// player physics and publishes both its heading and forced-button state there.
void autopilot_tick(void);

// True if this button is autopilot-forced this tick; *out = the forced held-state.
// Returns false for buttons the autopilot does not drive (so input_held falls through).
bool autopilot_force_button(enum input_button b, bool* out);

/* Death-screen Respawn script: the RETRY PERIOD, in 20 Hz autopilot ticks.
 *
 * The script cannot simply release once and then hold forever. screen_death.c
 * keeps its gate shut for the whole entry grace (DEATH_GRACE_S = 0.4 s,
 * screen_death.c:109) and seeds the hold only from a FRESH press edge
 * (`gate_open && input_pressed(IB_GUI_CLICK)`), resetting hold_s to 0 on every
 * frame that finds no press and no progress. A single edge ~100 ms after entry
 * (death_screen_ticks 1 -> 2) therefore lands INSIDE the grace, is discarded,
 * and the continuous level behind it can never start a hold -- the real-Wii
 * lan_walk_0912 FAIL ("mp autopilot walk displaced 0.0 blocks"). So the script
 * re-presses: one released tick every P ticks, producing a fresh edge P ticks
 * after the last one.
 *
 * Derivation of P (at the autopilot's 20 Hz tick = 50 ms/tick):
 *   P * 50 ms  >  DEATH_GRACE_S (400 ms)
 *                 + gui.death_hold_respawn_ms default (750 ms, main.c:1690)
 *                 + 250 ms margin
 *              =  1400 ms   ->   P > 28   ->   P >= 29.
 * P = 30 (1.500 s) is the round period just above that floor (100 ms slack).
 * Two consequences both hold with room to spare:
 *   - the edge at tick 1+P = 31 -> 32 lands 1.55 s into the screen, i.e. well
 *     after the 0.4 s grace has expired, so the gate is open when it arrives
 *     (and 1+2P = 61 -> 62 is a further retry if a frame ever swallows it);
 *   - the uninterrupted hold that edge seeds lasts P-1 = 29 ticks = 1450 ms,
 *     nearly 2x the 750 ms the screen needs, so the next released tick never
 *     cuts a hold short before it commits.
 * Raising gui.death_hold_respawn_ms above P-1 ticks (1450 ms) would break the
 * second property; that is a debug-harness config, left to the operator.
 */
#define AUTOPILOT_DEATH_CLICK_RETRY_TICKS 30

/* The scripted IB_GUI_CLICK LEVEL for a given death_screen_ticks: RELEASED
 * (false) for exactly one tick at tick 1 and at every multiple of the retry
 * period after it, HELD (true) on every other tick. Tick 0 (a frame that runs
 * before the first autopilot tick on the screen) is released too, so the very
 * first thing screen_death ever sees is the button UP -- that is what arms its
 * fresh-press latch. Shared by autopilot.c and the screen tests that double
 * input_pressed/input_held, so one formula backs both. */
static inline bool autopilot_death_click_held(int death_screen_ticks) {
	return death_screen_ticks > 0
		&& ((death_screen_ticks - 1) % AUTOPILOT_DEATH_CLICK_RETRY_TICKS) != 0;
}

/* Rising edge OUT of autopilot_force_button's held LEVEL.
 *
 * input_held() can force a button down, but every screen that confirms through
 * input_pressed() needs a genuine once-only press edge out of that same
 * forcing. screen_death's Respawn hold is the case that bit us: the forced
 * IB_GUI_CLICK was held forever, input_pressed() had no autopilot hook at all,
 * so the hold was never seeded and a walker that died never respawned
 * (real-Wii lan_walk_0912, "displaced 0.0 blocks").
 *
 * The rule is exactly the real merged mask's: forced-now && !forced-at-the-
 * previous-poll. The latch array belongs to the CALLER -- input.c owns the
 * production one and advances it once per input_poll(), next to the real
 * input_action_prev advance -- so nothing hides in this header, and the screen
 * tests that double input_pressed/input_held (they cannot link
 * platform/input.c) drive the identical algorithm.
 */
static inline bool autopilot_force_edge_pressed(const bool* forced_prev,
												enum input_button b,
												bool forced) {
	return forced && b >= 0 && b < IB_COUNT && !forced_prev[b];
}

/* Latch one button's forced level for the next frame's edge. Call for every
 * button exactly once per input frame, wherever the real edges are advanced. */
static inline void autopilot_force_edge_advance(bool* forced_prev,
												enum input_button b,
												bool forced) {
	if(b >= 0 && b < IB_COUNT)
		forced_prev[b] = forced;
}

// True (consuming one pulse) when the aim-autopilot wants to inject a block-place this frame.
// Lets the game place blocks deterministically with no input (debug.autopilot_place=on).
bool autopilot_take_place_pulse(void);

// Aim mode only (debug.autopilot=aim): re-point the held look at runtime, in the
// camera's degrees (yaw 0 = +Z, positive toward +X; pitch 90 level, larger = down).
// The menus_dwell walker uses it to face each demo container (menu_dwell_demo.h).
void autopilot_aim_set_deg(int yaw_deg, int pitch_deg);

// Aim mode only: latch ONE place pulse for screen_ingame to consume as an
// empty-hand right-click on whatever the crosshair holds (same cap as the
// scheduled pulse: never accumulates).
void autopilot_request_place(void);

/* ---- the scripted-driver runtime seam (push-0921/B4b) --------------------
 *
 * mp_play_10m (source/game/mp_play_script.c) needs the EXISTING walker and the
 * EXISTING aim lock to run during ONE unattended run, alternating many times
 * per run -- and it cannot get them the way every other profile does. The
 * config key debug.autopilot is read ONCE at startup (main.c) and picks a
 * single mode for the whole process; hw_run.c's profile table is the same
 * decision made earlier. A run that walks for 40 s, then aims straight down to
 * dig, then walks again, cannot be expressed by either.
 *
 * So these two are RUNTIME assertions of the same state those config paths
 * set, and nothing else: no new walker, no second aim path, no new call site.
 * Everything downstream -- autopilot_tick's mode 0 route/yaw code, the unstick
 * detector, autopilot_force_button's FORWARD/JUMP forcing, camera.c's
 * "autopilot owns the look while active" branch -- runs exactly as it does for
 * debug.autopilot=walk / =aim.
 *
 * INERTNESS. With neither ever called, this file behaves byte-identically:
 * the seam's own state is file-static and starts cleared, the two functions
 * below are its only writers, and no existing function body changed. The
 * first assertion SAVES gstate.autopilot's active/mode/unstick-armed and the
 * aim angles; the release restores them, so a config-driven autopilot that was
 * already running is handed back exactly as it was found.
 *
 * WALK WINS over AIM when both are asserted in the same frame. They are
 * genuinely exclusive downstream -- mode 0 rewrites yaw/pitch from its own
 * route every tick -- and the core only ever asks for both during its
 * re-centring bursts, where MOVING is the thing that cannot be done any other
 * way and the aim is re-asserted the moment the burst ends. Stated, not
 * hidden.
 */

/* Assert/release the walker at a FIXED heading (+ the keep-up escape ladder,
 * which is what frees a body wedged against terrain during a 10-minute walk).
 *
 * `heading_deg` is in this ENGINE's yaw convention, degrees, the same one
 * autopilot_script_aim takes and the same one autopilot_aim_set_deg writes --
 * the caller converts (mp_play_map.h's mpp_map_engine_yaw_deg has the
 * derivation). It is read on EVERY call, not only on the rising edge, so a
 * driver that changes heading mid-assertion is obeyed; the escape ladder is
 * re-seeded only on the rising edge, because a re-seed inside a leg would
 * forget the progress that leg has already made.
 *
 * MODE 2, not mode 0 (push-0921/B7). Mode 0's heading is `tick * 0.005`, a
 * constant-curvature circle that cannot hold a requested heading at all -- the
 * first two PC runs of mp_play_10m curved into water and a pit and reached
 * 58.9 blocks of the 64-block displacement floor. Mode 2 is the EXISTING fixed-
 * heading driver (autopilot_keepup.h, written for keepup_walk_6m for the same
 * variance reason) with its bounded, non-escalating sidestep ladder. Nothing
 * new is written here; only which existing mode the seam asserts.
 *
 * `terrain_safe` route probing IS turned on by this seam, and restored on
 * release. It was left off in the first version of this function because the
 * probe's candidate table (autopilot.c's `dirs`) has a hard-coded +Z
 * preference and would have overridden the heading -- but a fixed heading with
 * no hazard avoidance walks straight off whatever is in front of it, and the
 * PC run that proved it (pc_mp_play_20260921_124707) DIED of fall damage 12.6 s
 * into measured play. Both are now true at once: the probe still only proposes
 * a landing it has verified, and ap_script_route_rank makes it propose them in
 * the order the requested heading asks for, so nothing silently ignores the
 * caller. Off the seam that ranking is the identity, so every config consumer
 * of the probe keeps its exact trajectory. */
void autopilot_script_walk(bool on, int heading_deg);

/* Assert/release the aim lock at a heading. `pitch_down_deg` is the CALLER's
 * convention -- positive is DOWN from level, 0 level, 90 straight down -- and
 * is converted here to the camera's polar degrees (90 + down). camera.c clamps
 * the result to [0.5, 179.5], so a request for straight down lands a hair off
 * vertical, which still targets the cell under the feet. */
void autopilot_script_aim(bool on, int yaw_deg, int pitch_down_deg);

/* host_vehicle_smoke_8m (vehicle-smoke-1004, CONTRACT-v1 BUTTON): the
 * profile-scoped, button-LEVEL override. While armed, autopilot_force_button
 * answers IB_ACTION1 / IB_ACTION2 / IB_JUMP / IB_FORWARD / IB_GUI_CLICK from
 * `levels` BEFORE the aim mode's early return, so the real host input path
 * (input_held / input_pressed's forced-edge latch) carries them to the actual
 * consumers. Every other button keeps its normal resolution. The caller
 * changes a level only after the preceding input_poll latch and leaves one
 * neutral frame between edges; `armed=false` releases all owned buttons (neutral)
 * and restores the pre-override behaviour exactly. REL-11-seam adds
 * IB_OPEN_INVENTORY / IB_INVENTORY / IB_GUI_QUICK_MOVE to the owned set;
 * unarmed they stay unclaimed. Only the vehicle smoke
 * script calls this; unarmed it is one false branch. */
void autopilot_script_buttons(bool armed, const bool levels[IB_COUNT]);
bool autopilot_script_buttons_armed(void);
/* The same script's float look (radians, engine convention: yaw =
 * atan2(dx, dz), pitch = polar angle from +Y, 90deg = level) for exact
 * entity aiming; rides the aim mode like autopilot_script_aim. */
void autopilot_script_look(bool on, float yaw_rad, float pitch_polar_rad);

#endif
