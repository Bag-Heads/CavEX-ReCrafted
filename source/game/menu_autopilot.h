/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/*
	menus_dwell hardware profile: the engine-facing driver around the pure
	menu_autopilot_script state machine (see that header for the design).
	This is the impure half -- it reads gstate.players[0].current_screen and gfx_width/
	height, so (unlike menu_autopilot_script.c) it is NOT unit tested here;
	it is thin, deterministic glue, the same shape as autopilot.c/
	agent_input.c already are for their own input rigs.

	Gated entirely by menu_autopilot_arm(): a normal run (any other profile,
	or no hw_run at all) never calls it, so input_pressed()/input_pointer()
	are byte-identical to before this file existed. See main.c (hw_run wiring)
	and platform/input.c (the two hook call sites).
*/

#ifndef GAME_MENU_AUTOPILOT_H
#define GAME_MENU_AUTOPILOT_H

#include <stdbool.h>

#include "../platform/input.h"

/* Arm the script (call once at boot, only when hw_run.enabled &&
 * hw_run.menu_autopilot). A no-op if called again before the script
 * finishes; harmless if never called (every hook below then always returns
 * "not owned"/"not intercepted"). `target_creative` is read straight from
 * hw_run.cworld_mode (source/game/hw_run.h) and forwarded to menu_ap_
 * script_reset() -- false (Survival) reproduces the original behavior
 * byte-for-byte; true is the --cw-cworld-mode creative / menus_dwell_
 * creative knob. */
void menu_autopilot_arm(bool target_creative);

/* Advance one frame (call exactly once per frame, from main.c's main loop,
 * BEFORE gstate.players[0].current_screen->update() runs this frame -- mirrors
 * agent_input_poll()'s documented contract). No-op unless armed. */
void menu_autopilot_frame_tick(void);

/* True while armed and the script has not finished (or abandoned). */
bool menu_autopilot_active(void);
/* critique-1028 glyph-proof hook: the controller FAMILY the walker presents
 * (prompt glyphs only -- input, owners and notices are untouched), from the
 * rig key debug.autopilot_device = nunchuk (default) | classic | gamecube,
 * read at menu_autopilot_arm. Lets the real Wii photograph the GameCube /
 * Classic glyph cells with no human; it proves the GLYPHS, never pad input. */
enum input_category menu_autopilot_device_parse(const char* value);
enum input_category menu_autopilot_device_family(void);
/* 1 + the step ordinal the walker abandoned at (menu_autopilot_script.h
 * map_fail), or 0. Also sent once as a {"event":"menu_ap_fail"} line. */
int menu_autopilot_map_fail(void);

/* input_pressed() hook: mirrors autopilot_force_button's contract. Returns
 * true and writes *out IFF this frame's script output presses `b`; false
 * otherwise (including when not armed), letting the caller fall through to
 * real input. */
bool menu_autopilot_force_button(enum input_button b, bool* out);

/* input_pointer() hook. Returns true whenever this module is actively
 * OWNING the pointer query this frame (armed and not yet finished) --
 * suppressing the real IR/mouse read for the whole scripted run, not only
 * the one tick it actually asserts a position, so a coincidentally-valid
 * stray Wiimote IR reading during an unattended run can never land an
 * unintended click (docs/deviations.md). When it returns true, *available
 * is this frame's pointer-presence answer, and *x, *y, *angle are valid
 * iff *available is true.
 *
 * This is THE seam the no-pointer leg asserts through (push-0921 A7 part 1a):
 * while menu_ap_output.pointer_suppressed is set it answers *available =
 * false BEFORE consulting pointer_target, so the suppression DOMINATES and
 * the screens read exactly what input_pointer() gives them on a console with
 * the Remote pointed away. There is no second query and no screen change. */
bool menu_autopilot_force_pointer(float* x, float* y, float* angle,
								   bool* available);

/* Telemetry getters (source/main.c wires these into the hw_telemetry
 * heartbeat). All return 0/false when not armed. */
int menu_autopilot_step(void);
int menu_autopilot_screen(void); /* last classified menu_ap_screen, -1 when not armed (boot-trace diagnostics) */
int menu_autopilot_max_step(void);
int menu_autopilot_step_count(void);
int menu_autopilot_step_required_for_pass(void);
bool menu_autopilot_sign_reached(void);
/* True iff STEP_WAIT_SIGN's soft wait ever timed out this run (one-shot
 * latch, mirrors sign_reached). main.c watches this for a false->true edge
 * to log which sign gate failed -- see menu_autopilot_script.h's design
 * note. */
bool menu_autopilot_sign_wait_timed_out(void);

/* ---- the NO-POINTER leg (push-0921 A7 part 1) -------------------------- */
/* Which no-pointer-leg step's bounded wait ran out, or 0 if none has. One
 * terminal step ordinal cannot name WHICH wait failed, and "the leg failed"
 * without that is exactly the kind of un-diagnosable hardware result this
 * profile exists to avoid -- so the ordinal says "the leg", this says which
 * step. */
int menu_autopilot_np_failed_step(void);
/* The leg completed: Select World, Texture Packs and Multiplayer were each
 * reached from the title by direction + activate and closed by the Close
 * role, with the pointer asserted UNAVAILABLE throughout. This is the
 * profile's PASS ordinal in boolean form. */
bool menu_autopilot_np_leg_done(void);
/* This platform's keyboard owns the Close role on OSK-backed screens
 * (PLATFORM_PC), so the walker cannot enter and leave them -- the coverage is
 * BOUNDED and said out loud rather than faked. False on the Wii, where
 * input_text_active() is a hard false and the console run is the real proof
 * of those screens. */
bool menu_autopilot_np_osk_legs_skipped(void);
/* Is the walker asserting "no pointer" to the screens THIS frame? The live
 * value behind menu_autopilot_force_pointer's suppression branch, exposed so
 * a heartbeat can show the leg really ran with the pointer down instead of
 * the claim resting on the step ordinal alone. */
bool menu_autopilot_pointer_suppressed(void);

/* ---- part 2: the observation-driven sub-legs --------------------------- */
/* Which sub-leg (enum menu_ap_sub) the shared walk is on. The walk's step
 * ordinals are re-entered per sub-leg, so a heartbeat needs this as well as
 * menu_autopilot_step() to say where a run actually is. */
int menu_autopilot_sub(void);
/* The sub-leg that died, valid iff menu_autopilot_np_failed_step() != 0. */
int menu_autopilot_np_failed_sub(void);
/* Bitmasks over (1 << enum menu_ap_sub). Deliberately three separate
 * answers: the ring REACHED the control, the control was ENTERED and left,
 * and the sub-leg was SKIPPED by name (an empty list disabling Rename /
 * Delete / Edit, or an OSK-backed screen whose Close this platform's keyboard
 * owns). "Focus reached but not entered" is a real, different claim from
 * "never got near it" and the two must never be blurred into one number. */
unsigned menu_autopilot_np_focus_reached(void);
unsigned menu_autopilot_np_entered(void);
unsigned menu_autopilot_np_skipped(void);
/* Register the Multiplayer screen's read-only observers. MUST be called from
 * a build that links source/game/gui/screen_multiplayer_setup.c (it is in the
 * MP source pool, not the engine core this file compiles into -- see the .c
 * for why this is a hook and not a direct call):
 *   menu_autopilot_set_mps_observers(screen_mps_focus_target, screen_mps_view,
 *                                     screen_mps_server_count);
 * Unregistered is SAFE, not guessed: the walker reports the four Multiplayer
 * sub-legs as named skips and never presses at a screen it cannot see. */
void menu_autopilot_set_mps_observers(int (*focus)(void), int (*view)(void),
									  int (*rows)(void));
/* BOOT-CREDITS-1028 T5: true while the boot credit cards own the front end
 * (main.c, every frame, before frame_tick). The walker holds -- no press,
 * no clock -- until the title is admitted. */
void menu_autopilot_set_front_end_blocked(bool blocked);
/* True once the walker skipped its Multiplayer sub-legs for want of those
 * observers. A run with this set has NOT proven the Multiplayer half of the
 * no-pointer leg, whatever else it reports. */
bool menu_autopilot_np_mps_unobserved(void);
/* The heartbeat's np_flags: MENU_AP_NP_FLAG_* in menu_autopilot_script.h.
 * Every bit means "this run did not prove what a clean run proves", which is
 * why the console verdict requires it to be zero. */
unsigned menu_autopilot_np_flags(void);
/* The Select World row count the leg latched, 0 if it never took one. The
 * before/after comparison around the delete confirm and the rename editor is
 * made inside the walker; this is the value it compared against, carried so a
 * console PASS can be re-read rather than trusted. */
int menu_autopilot_np_rows(void);

/* ---- ui-build-1026/T16a: the world_copy_convert route ------------------ */
/* The production read-only observers (and the ONE fault seam) the route
 * reads. They live in screen_select_world.c / screen_convert_backup.c, which
 * this lane may not edit, so they arrive through this hook exactly like the
 * Multiplayer observers above; main.c registers them when it arms the
 * profile (the owed hunks: tmp/jobs/ui-build-1026/T16a-RIG.md). While ANY is
 * NULL the route fails at its first tick by name (MENU_AP_COPY_STEP_
 * UNOBSERVED) and never presses. */
#include <stdint.h>
struct menu_autopilot_copy_hooks {
	/* Select World */
	bool (*sworld_settled)(void);
	int (*sworld_selected_row)(void);
	int (*sworld_row_of_directory)(const char* directory); /* -1 absent */
	bool (*sworld_row_needs_convert)(int row);
	bool (*sworld_partial_copy_visible)(void);
	/* screen_convert_backup (enum values: menu_autopilot_script.h) */
	int (*cb_state)(void);
	int (*cb_focused_action)(void);
	int (*cb_fail)(void);
	int (*cb_copy_error)(void);
	int (*cb_percent)(void);
	bool (*cb_volume)(uint64_t* free_bytes, uint32_t* cluster_bytes,
					  uint64_t* required_bytes);
	int (*cb_convert_outcome)(void);
	bool (*cb_backup_published)(void);
	/* INTERRUPT variant only: abandon the running copy WITHOUT cleanup (the
	 * on-disk shape a power cut leaves) and park the screen doing no SD
	 * work. Never called by any other variant. */
	void (*cb_interrupt)(void);
};
/* Arm the route (instead of menu_autopilot_arm, never together): `variant`
 * is enum menu_ap_copy_variant, `fixture` the fixture DIRECTORY name,
 * `expected_root` the isolated per-run worlds root main.c just set, `hooks`
 * the observers (copied). The configured "paths.worlds" is read BACK: unless
 * it is exactly that isolated root (menu_ap_copy_root_isolated), the route is
 * REFUSED by name -- no presses, no listing, no disk writes, only
 * wcc_begin/wcc_card(root_ok=false)/wcc_end(failed_step=ISOLATION) -- and
 * this returns false, so the caller arms no fault seam either. The route
 * state is heap-allocated here and freed once its markers are admitted. */
bool menu_autopilot_arm_copy_route(int variant, const char* fixture,
								   const char* expected_root,
								   const struct menu_autopilot_copy_hooks* hooks);
int menu_autopilot_copy_step(void);
int menu_autopilot_copy_failed_step(void);
bool menu_autopilot_copy_finished(void);
bool menu_autopilot_copy_ok(void);
/* Every route marker line was admitted by the telemetry ring. */
bool menu_autopilot_copy_markers_flushed(void);

#endif
