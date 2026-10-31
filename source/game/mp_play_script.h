/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* mp_play_10m -- the IMPURE half of the unattended LAN play/rejoin driver.
 *
 * CONTRACT: docs/mp_play_profile.md (draft: tmp/jobs/push-0921/
 * design-mp-play-10m.md). That file, not this header, is the authority on the
 * phases, the marker names/keys/order and the collector's PASS rule.
 *
 * THE SPLIT. source/game/mp_play_core.h is the whole phase machine, PURE: it
 * takes a struct mpp_obs and returns a struct mpp_out and never touches a
 * global. This file is the other half -- ONE call per rendered frame that
 *   (a) fills struct mpp_obs from OBSERVED production state,
 *   (b) calls mpp_tick(),
 *   (c) performs what struct mpp_out asks: button ACTIONS through the login
 *       driver's virtual pad -> the production binding table -> the owner
 *       latch; `walk` / `aim` through autopilot.c's runtime seam; and the
 *       queued markers onto the telemetry wire.
 * source/game/mp_play_map.h holds every translation between the two, PURE and
 * unit-tested on its own.
 *
 * OBSERVED, NEVER ASSUMED. Every field of struct mpp_obs comes from state the
 * game already computes for its own reasons -- the local player entity, the
 * crosshair the block highlight already raycast, the client's own player
 * inventory, the pause/inventory focus observers, and struct
 * server_remote_play_obs's observation rings. Nothing here tells the core that
 * something happened; it only reports what it can see, and the core decides.
 * An observation that cannot be made is reported as "not observed" and the
 * core then fails by name on a budget rather than proceeding on a guess.
 *
 * INERTNESS. mp_play_frame() returns on its first statement unless
 * mp_play_arm() was called. Unarmed -- normal play and every other profile --
 * nothing is filled, nothing is ticked, the virtual pad is never asked for,
 * autopilot's seam is never asserted and not one marker is emitted.
 */

#ifndef GAME_MP_PLAY_SCRIPT_H
#define GAME_MP_PLAY_SCRIPT_H

#include <stdbool.h>
#include <stdint.h>

#include "mp_play_core.h" /* enum mpp_focus / mpp_screen / mpp_phase */

struct chat_login_ctx;
struct chat_ring_state;
struct screen;
struct server_remote_play_obs;

/* Everything this engine-pool file cannot reach itself, or that another lane
 * owns. Each one is a seam a test replaces with a recorder, which is what
 * makes the driver drivable off-console -- the struct chat_login_hooks
 * precedent, deliberately the same shape. */
struct mp_play_hooks {
	/* hw_telemetry_emit_observer(line). May REFUSE (ring full / quiesced); the
	 * driver then retries the SAME line, with the same seq, next frame. */
	bool (*emit)(void* user, const char* line);
	/* hw_telemetry_emit_profile_done(status). Called exactly once per run. */
	void (*profile_done)(void* user, const char* status);
	/* server_remote_get_play_obs() for the live session -- the three
	 * observation rings, the world-install generation and the watched cell.
	 * source/network is an APP/MP-pool translation unit this file may not
	 * reference, so it arrives as a hook exactly like chat_login's
	 * chat_lines_sent. FALSE means there is no remote provider at all: the
	 * driver then reports nothing confirmed and the core fails on
	 * MPP_READY_BUDGET_MS by name.
	 *
	 * THE ONLY remote-observation hook (push-0921/B19 item 0). It used to sit
	 * beside a `remote_status` hook carrying struct server_remote_status, but
	 * once B18 moved the rings out of that struct the driver read NOTHING out
	 * of it -- the call survived only as an "is there a remote provider" gate,
	 * and this hook answers false under exactly the same condition (both hook
	 * bodies guard on `!remote || !remote->impl`). Keeping it cost the driver
	 * a second mutex acquisition and a 696 B copy per frame for a struct it
	 * never read, so the gate collapsed onto this one. */
	bool (*remote_play_obs)(void* user, struct server_remote_play_obs* out);
	/* server_remote_play_watch() -- NAME THE ONE CELL the core is waiting on,
	 * every frame, so the network layer keeps that cell's echo out of the
	 * 16-entry ring's way (push-0921/B19). Called from the core's own
	 * mpp_out.watch BEFORE the frame's buttons are applied, so the watch is
	 * in place before the press that will produce the echo.
	 *
	 * A NULL hook is the inert case and costs the core nothing: obs.watch_echo
	 * then stays serial 0 and mpp_echo_for falls back to the ring alone, which
	 * is exactly the behaviour before this seam existed. */
	void (*remote_watch)(void* user, bool on, int32_t x, int32_t y, int32_t z);
	/* Focus on the TITLE and MULTIPLAYER screens, which another lane owns.
	 * A NULL hook reports MPP_FOCUS_UNKNOWN -- "not observed" -- and the core
	 * keeps stepping focus until its own menu budget fails the run by name,
	 * which is the honest outcome of nobody having wired it yet. The PAUSE
	 * screen is NOT asked here: this file reads it directly through
	 * screen_pause_focus_is_leave(). */
	enum mpp_focus (*menu_focus)(void* user);
	/* Has the Beta blockExists gate released the local player's body?
	 *
	 * THIS IS A HOOK ONLY BECAUSE THE PRODUCTION PREDICATE IS UNREACHABLE.
	 * The gate is local_player_column_present() in
	 * source/entity/entity_local_player.c (a file static, no header), called
	 * as the first thing in that file's entity_tick. It must NOT be
	 * re-derived here -- two copies of the Beta chunkExists gate is exactly
	 * the drift the F8b ruling closed, and a second copy that disagreed would
	 * make the driver report a body ticking that is frozen, or the reverse.
	 * Exporting it is a one-line change in a file this card does not own, so
	 * until that lands the observation arrives through here. A NULL hook
	 * reports false = "not observed"; the core then waits in MPP_PH_READY and
	 * fails with "world never became ready" rather than playing into a frozen
	 * body. */
	bool (*body_ticking)(void* user);
	/* Pass-through to the login driver's own hook of the same name
	 * (mp_session_chat_lines_sent), so main.c wires ONE hook set. */
	bool (*chat_lines_sent)(void* user, uint32_t* out);
	void* user;
};

/* The per-frame app state main.c already has. Deliberately the same fields as
 * struct chat_login_ctx, in the same order, so main.c fills ONE struct per
 * frame and mp_play_login_ctx() below maps it onto the other -- the two can
 * then never drift apart. */
struct mp_play_ctx {
	bool remote_session;
	bool world_loaded;
	bool world_switch_pending;
	bool local_player; /* players[0].local_player != NULL */
	/* players[0].current_screen. Mapped to enum mpp_screen by
	 * mp_play_screen_of() below rather than by main.c, so the screen-identity
	 * table lives next to the core that branches on it. */
	const struct screen* current_screen;
	/* main.c's own single evaluation for this frame -- screen_controller_
	 * recovery(), never re-stepped here. While it is up main.c skips the
	 * screen's own update, so the driver requests NO actions that frame. */
	bool recovery_modal;
	uint32_t epoch;  /* clin_input_epoch(); carried for mp_play_login_ctx */
	uint32_t now_ms; /* any monotonic ms source; rebased by the core */
	const struct chat_ring_state* ring; /* &gstate.chat_ring; ditto */
};

/* ---- lifecycle ----------------------------------------------------------- */

/* `username` is the run nonce (the login username the fixture counts logins
 * for, and the `nonce` the session marker carries); it is copied.
 *
 * `run_id` names the run this driver was armed for and is NOT copied or
 * emitted: the transport owns the run identity and stamps it onto every line
 * the emit hook takes (hw_telemetry_emit_observer,
 * source/platform/hw_telemetry.c:2743-2747). It stays in the signature so the
 * call site still states which run it armed -- writing it into the marker too
 * is what put a duplicate `run_id` on every line until 2026-09-21. */
void mp_play_arm(const struct mp_play_hooks* hooks, const char* username,
				 const char* run_id);
bool mp_play_armed(void);
/* PC-ONLY debugging lever: cut measured play to `ms` and waive the floors so
 * the rejoin half can be exercised quickly (mp_play_core.h, short_play_ms).
 * Call AFTER mp_play_arm (arming resets it). The tape says so in its session
 * marker and the collector refuses it; main.c compiles the call out on the
 * Wii. */
void mp_play_set_short_play(uint32_t ms);

/* ONE call per rendered frame, at the same point main.c already calls
 * chat_login_frame(): AFTER player 0's screen update and BEFORE the
 * input_poll at the bottom of the loop -- the only point where the driver can
 * both observe the owner frame the screen just consumed and post the request
 * the next poll turns into the following frame's owner frame.
 *
 * It does NOT call chat_login_frame(): main.c's existing hunk already does
 * that once per frame whenever chat_login_armed(), which play mode makes true,
 * and stepping it twice would halve every login budget. */
void mp_play_frame(const struct mp_play_ctx* ctx);

/* Full teardown of the file-static state. Production never calls this (a run
 * is one process); tests call it between cases. */
void mp_play_reset(void);

/* ---- pure mappings main.c and tests share -------------------------------- */

enum mpp_screen mp_play_screen_of(const struct screen* s);

/* Fill the login driver's ctx from this one. main.c builds a single
 * struct mp_play_ctx per frame and hands the login its view through this, so
 * the two observations are the SAME observation. */
void mp_play_login_ctx(const struct mp_play_ctx* in, struct chat_login_ctx* out);

/* ---- observation (tests and diagnostics) --------------------------------- */

enum mpp_phase mp_play_phase_now(void);
uint32_t mp_play_seq_now(void);
unsigned mp_play_markers_pending(void);
bool mp_play_done_now(void);
/* Action requests the map refused because the core asked for something outside
 * the vocabulary. Counted, never silent: a nonzero value is a core/vocabulary
 * defect, and the run still fails by name on the step whose press never
 * happened. */
unsigned mp_play_refused_actions(void);

#endif
