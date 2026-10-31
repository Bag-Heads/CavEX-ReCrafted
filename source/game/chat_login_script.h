/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* mp_chat_login_3m -- the scripted MP chat/OSK command-delivery phase driver.
 *
 * CONTRACT: docs/chat_login_profile.md. That file, not this header, is the
 * authority on the phases, the marker key names/types/order, the timeouts and
 * the failure diagnostics; tools/wii_telemetry_server.py is written against it.
 *
 * WHAT THIS IS. One profile (HW_RUN_PROFILE_MP_CHAT_LOGIN_3M) drives, with no
 * human at the console, the whole chat path nobody has ever proved unattended:
 * open the composer with the real D-pad-Up opener (chord gate included), walk
 * the on-screen keyboard to spell `/login hunter22`, submit it, and observe the
 * server's reply land in gstate.chat_ring. Every assertion is made from
 * OBSERVED production state; a marker never says "the command was issued"
 * (Astra ruling 2026-09-17 item 5, carried over from split_recovery).
 *
 * WHY A DRIVER AND NOT MAIN.C CODE. On an unattended console NOBODY owns
 * player 0 (input.c binds only on an intentional A press), so both the opener
 * (input_chat_open_edge) and the composer (screen_chat's owner frame) are dead.
 * SEAM S1 therefore gives the driver a VIRTUAL PAD bound as player 0, seeded
 * UPSTREAM of edge derivation: the driver supplies a button-code mask only, and
 * production still runs input_binding_levels (chords included), input_glyph_
 * frame and -- unchanged -- input_owner_latch, which is where pressed/down/
 * released and the attach/reconnect block are really derived.
 *
 * INERTNESS. Nothing here runs unless chat_login_arm() was handed an ARMED
 * hw_run config whose profile is mp_chat_login_3m. Unarmed -- which is normal
 * play and every other profile -- chat_login_pad_for() and
 * chat_login_pad_native_key() return false on their first statement, so
 * input.c's two seam sites keep today's reader, today's frame and today's
 * native answer; chat_login_frame() returns immediately; nothing binds a
 * device and not one marker is emitted.
 *
 * WITHDRAWAL. The virtual pad is unbound on EVERY exit path (ok, failed,
 * timeout) -- split_recovery's "synthetic device withdrawn when the run ends".
 */

#ifndef GAME_CHAT_LOGIN_SCRIPT_H
#define GAME_CHAT_LOGIN_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../platform/input_inventory.h" /* struct input_identity */
#include "../platform/input_owner.h" /* struct input_device, IB_COUNT */

struct hw_run_config;
struct chat_ring_state;

/* The command this profile delivers, and the fixture reply it waits for.
 * Both are contract constants: the collector and mc173-diag's `/login`
 * handler are written against these exact bytes. */
#define CHAT_LOGIN_COMMAND "/login hunter22"
#define CHAT_LOGIN_COMMAND_LEN 15u
#define CHAT_LOGIN_ACK_LINE "CaveWii fixture: login OK"

/* chat_login_ctx::screen_id -- main.c maps players[0].current_screen to one of
 * these. Anything else is CHAT_LOGIN_SCREEN_OTHER; the point is to name the
 * handful a scripted MP run can legitimately sit on. */
#define CHAT_LOGIN_SCREEN_OTHER 0u
#define CHAT_LOGIN_SCREEN_INGAME 1u
#define CHAT_LOGIN_SCREEN_CHAT 2u
#define CHAT_LOGIN_SCREEN_LOAD_WORLD 3u
#define CHAT_LOGIN_SCREEN_TITLE 4u
#define CHAT_LOGIN_SCREEN_MP_CONNECT 5u
#define CHAT_LOGIN_SCREEN_PAUSE 6u
#define CHAT_LOGIN_SCREEN_DEATH 7u

/* The virtual pad's identity (contract section 1.4). On the Wii it is
 * {INPUT_DEVICE_WPAD, channel 3} -- the last WPAD channel -- and the driver
 * additionally REFUSES TO ARM when ANY WPAD channel reports connected (lead
 * ruling 2), so it can never shadow a real pad. INPUT_CAT_NUNCHUK is the floor
 * family and the only WPAD family input_owner_latch does not force-disconnect;
 * the presented CATEGORY is INPUT_CAT_NUNCHUK on both platforms.
 *
 * On PC the DEVICE KIND has to be the keyboard: input_player_bind refuses
 * anything else there, and input_owner_bind refuses a keyboard on a channel
 * other than 0. The category, the codes, the route and every marker are the
 * same -- only the kind/channel the run reports differ, which is why
 * p1.pad_bound carries the channel it really bound instead of a constant. */
#ifdef PLATFORM_PC
#define CHAT_LOGIN_PAD_DEVICE_KIND INPUT_DEVICE_KEYBOARD
#define CHAT_LOGIN_PAD_CHANNEL 0u
/* input_chat_open_edge's opener gate is compiled for the Wii only. Where the
 * gate does not exist the refusal leg legitimately OPENS the composer; the
 * driver records refused:false and closes it again. A Wii run -- the only one
 * a hardware PASS is ever claimed from -- always records true.
 * NAME NOTE (b1-chat-opener-0919): the macro still says CHORD because that is
 * what the leg asked for under marker contract 1 (D-pad Up + Left). Under
 * contract 2 the same flag gates the OVER-LONG-HOLD leg -- the opener held
 * past its 500 ms tap window -- and nothing else about it changed. */
#define CHAT_LOGIN_CHORD_GATED false
/* p0.session's `platform`: the collector requires the P1 refusal marker's
 * `refused` == true on "wii" and accepts false on "pc" (contract section 10
 * ruling 2). That marker is p1.chord_refused under contract 1 and
 * p1.hold_refused under contract 2 -- see docs/chat_login_profile.md
 * section 0; p0.session carries the `contract` version key. */
#define CHAT_LOGIN_PLATFORM "pc"
#else
#define CHAT_LOGIN_PAD_DEVICE_KIND INPUT_DEVICE_WPAD
#define CHAT_LOGIN_PAD_CHANNEL 3u
#define CHAT_LOGIN_CHORD_GATED true
#define CHAT_LOGIN_PLATFORM "wii"
#endif

/* Per-phase and total bounds (contract section 3). The per-phase budgets sum
 * to 195 s against a 180 s total, so the TOTAL is the binding cap and a
 * pathological run reports "run:total timeout" rather than a phase timeout --
 * the split_recovery precedent. Every run still reports. */
#define CHAT_LOGIN_P0_MS 60000u
#define CHAT_LOGIN_P1_MS 15000u
#define CHAT_LOGIN_P2_MS 25000u
#define CHAT_LOGIN_P3_MS 15000u
#define CHAT_LOGIN_P4_MS 45000u
#define CHAT_LOGIN_P5_MS 10000u
#define CHAT_LOGIN_P6_MS 25000u
#define CHAT_LOGIN_TOTAL_MS 180000u

enum chat_login_phase {
	CHAT_LOGIN_IDLE = 0,
	CHAT_LOGIN_P0,
	CHAT_LOGIN_P1,
	CHAT_LOGIN_P2,
	CHAT_LOGIN_P3,
	CHAT_LOGIN_P4,
	CHAT_LOGIN_P5,
	CHAT_LOGIN_P6,
	CHAT_LOGIN_OK,
	CHAT_LOGIN_FAILED,
	/* PLAY MODE ONLY (chat_login_arm_play). The login reached OK, run.ok was
	 * emitted, and the driver is holding the virtual pad open for the caller
	 * instead of unbinding and reporting. Appended, never inserted, so no
	 * existing value moves. */
	CHAT_LOGIN_PARKED,
};

/* The three things the driver cannot do itself, supplied by main.c. Each one
 * is a seam a test replaces with a recorder, which is what makes the whole
 * phase machine drivable off-console. */
struct chat_login_hooks {
	/* hw_telemetry_emit_observer(line). May refuse (ring full / quiesced);
	 * the driver then retries the SAME line with the SAME seq next frame. */
	bool (*emit)(void* user, const char* line);
	/* hw_telemetry_emit_profile_done(status). Called exactly once per run. */
	void (*profile_done)(void* user, const char* status);
	/* mp_session_chat_lines_sent(): the existing production snapshot
	 * (server_remote_get_status().chat_lines_sent). A hook, not a direct call,
	 * because mp_session.c is an APP-only translation unit -- this file lives
	 * in the shared engine pool and may not reference it. False when no remote
	 * provider exists (the driver then waits and finally fails honestly). */
	bool (*chat_lines_sent)(void* user, uint32_t* out);
	void* user;
};

/* Everything the driver observes that is not already public through
 * screen_chat_observe(). All of it is client-visible main-thread state. */
struct chat_login_ctx {
	bool remote_session;
	bool world_loaded;
	bool world_switch_pending;
	bool local_player;		/* players[0].local_player != NULL */
	bool screen_is_ingame;	/* players[0].current_screen == &screen_ingame */
	bool screen_is_chat;	/* players[0].current_screen == &screen_chat   */
	/* A compact id for players[0].current_screen, so a TIMEOUT can say what
	 * was on screen instead of leaving a console run unexplainable. The first
	 * real-Wii run spent P1's whole budget on screen_load_world and its diag
	 * said only "phase timeout". See CHAT_LOGIN_SCREEN_*. */
	unsigned screen_id;
	/* main.c's own `recovery_modal` for this frame -- screen_controller_
	 * recovery(), evaluated ONCE per frame there, never re-stepped here. With
	 * the inventory seam in place it must stay false for the whole run; P1
	 * publishes it and any appearance outside P3's sanctioned disconnect
	 * window fails the run with diag "recovery modal". */
	bool recovery_modal;
	float x, y, z;			/* the local player's position, for p0.session */
	uint32_t epoch;			/* clin_input_epoch() */
	uint32_t now_ms;		/* any monotonic ms source; rebased by the driver */
	const struct chat_ring_state* ring; /* &gstate.chat_ring */
};

/* ---- SEAM S1 (contract section 1.4) ------------------------------------- */

/* The virtual pad handed to input_players_poll. `codes` is the set of BUTTON
 * CODES the driver is asserting this poll -- resolved from the LIVE binding
 * table, so the driver names ACTIONS and the config still decides which
 * physical code carries them on this platform. Production then derives levels,
 * chords, glyphs, pressed/held/released, the attach block and the neutral
 * re-arm from those codes exactly as it does for a real pad. */
#define CHAT_LOGIN_PAD_CODES 48
struct chat_login_pad {
	bool connected;
	unsigned count;
	int codes[CHAT_LOGIN_PAD_CODES];
};

/* input.c, inside input_players_poll: substitute the frame seed AND the key
 * reader for `player` when this returns true. `map`/`lengths` are the binding
 * table input_poll just refreshed from the config. Returns FALSE on its first
 * statement unless the driver is armed AND owns `player`, which is what keeps
 * every other profile and normal play byte-identical. */
bool chat_login_pad_for(unsigned player,
					   int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
					   const size_t* lengths, struct chat_login_pad* out);

/* The input_key_reader for that substituted frame (`user` is the pad above). */
bool chat_login_pad_key(void* user, int key);

/* input.c, first statement of input_native_key_down (both platforms): answers
 * ONLY codes in the virtual pad's vocabulary -- the codes bound to the twelve
 * chat actions -- and only while armed. Everything else, and everything while
 * disarmed, falls through to the hardware read unchanged. On Wii this reaches
 * only input_chat_open_edge's chord gate (the merged tail is unreachable after
 * input_poll's early return); on PC it is also the merged mask, which is what
 * makes the opener reachable there at all. */
bool chat_login_pad_native_key(int key, bool* value);

/* input.c, inside input_devices_poll (Wii), at the per-channel
 * input_inventory_poll(...) call: while armed AND BOUND, the virtual pad's
 * channel is published to the device inventory as a connected, fresh, neutral
 * -- and therefore ARMED -- Nunchuk, so the pad is indistinguishable from a
 * real one to every consumer that reads the inventory rather than the owner
 * frame. The one that matters is screen_controller_recovery(), which decides
 * "is the owner present" from `published.connected`: without this the recovery
 * modal would own the whole update from the moment the driver bound and no Wii
 * run could reach P1 (contract section 10 ruling 1).
 *
 * Gated on BOUND, not merely armed, for a reason: the pre-bind refusal check
 * (lead ruling 2) reads the inventory's `observed.connected` for every WPAD
 * channel, and a pad that published itself before binding would refuse to arm
 * against its own reflection.
 *
 * `neutral` is reported true even while the driver is asserting buttons: the
 * inventory's neutral flag is the ARMING gate (it is what keeps `armed` true),
 * not a claim about the physical sticks, and the owner frame -- which is where
 * every press really comes from -- is seeded by the seam above. Stated, not
 * hidden. False when disarmed or unbound, so the real WPAD_Probe answer stands
 * exactly as it does today. */
bool chat_login_pad_inventory(struct input_device device,
							  struct input_identity* identity, bool* connected,
							  bool* fresh, bool* neutral);

/* ---- lifecycle ---------------------------------------------------------- */

void chat_login_arm(const struct hw_run_config* cfg,
					const struct chat_login_hooks* hooks);
bool chat_login_armed(void);

/* ---- PLAY MODE (push-0921 B4a) ------------------------------------------
 *
 * WHY. mp_play_10m (docs/mp_play_profile.md, source/game/mp_play_core.h) needs
 * this login machine TWICE in one run -- once per connection generation -- and
 * needs the virtual pad for everything in between, because on an unattended
 * console nobody owns player 0 and gameplay, menus and the composer are all
 * equally dead without it. Astra ruling 1 answer B3 ruled that it CALLS this
 * implementation rather than copying it: "Preserve contract-2 markers,
 * ordering, timing semantics and input behavior." So P0..P6 are not touched --
 * play mode only changes what happens AFTER the login has succeeded, and only
 * when it was armed through chat_login_arm_play.
 *
 * INERTNESS IS UNCHANGED. Nothing below has any effect unless play mode was
 * armed: the two existing profiles keep today's seam, today's vocabulary and
 * today's markers, byte for byte (proved by case_trace_equivalence's golden in
 * tests/test_chat_login_script.c).
 *
 * Arms the machine for one mp_play_10m run. Unlike chat_login_arm this takes
 * no hw_run config and checks no profile: the CALLER decides when a login is
 * due, so `username` (the run nonce echoed in p0.session) is passed directly. */
void chat_login_arm_play(const struct chat_login_hooks* hooks,
						 const char* username);

/* ACTION-ONLY play mode (REL-14b-0). Vanilla Beta has no `/login`: it logs
 * the unknown command and replies with nothing (CFR NetServerHandler.java:
 * 471-474), so the typed login above can never reach p6.ack there. This arm
 * acquires the SAME pad through the SAME P0 readiness and P1 refusal/publish/
 * bind/settle steps, emits p1.actions_ready instead of typing anything, and
 * PARKS -- no hold leg, no composer, no run.ok. A chat_login_play_restart
 * (reconnect) leaves PARKED at once and re-enters P0, so actions are refused
 * until the new generation's session and binding are observed afresh
 * (p0.session + p1.pad_bound again; the bind itself is kept). Readiness lost
 * between the bind and the grant fails as p1.actions_ready "session lost".
 * Requests, faults, release and play_end are the play-mode ones above. */
void chat_login_arm_play_actions(const struct chat_login_hooks* hooks,
								 const char* username);

/* How many ACTIONS one play request may hold at once. mp_play_core.h's whole
 * vocabulary is eleven (attack, use, open-inventory, close, pause, the four
 * GUI directions, click, hotbar-next) and it never asks for all of them
 * together; twelve is headroom. A longer request is REFUSED rather than
 * silently truncated -- a dropped action would be a press the caller believes
 * it made. */
#define CHAT_LOGIN_PLAY_ACTIONS 12

/* While CHAT_LOGIN_PARKED: make the virtual pad assert the codes bound to
 * `actions`, resolved through the LIVE binding table on the same path
 * cl_request/chat_login_pad_for already use -- so the caller names ACTIONS and
 * the staged config still decides which physical code carries them, and a
 * CHORD encoding is never asserted as a composite (its halves are resolved
 * through this same reader). This is the ONLY place the pad's action
 * vocabulary reaches past the twelve chat roles, and it is reachable only in
 * play mode: chat_login_pad_native_key answers exactly today's twelve-role
 * vocabulary for mp_chat_login_3m and mp_chat_chord_1m.
 *
 * The request PERSISTS until the next call replaces it; `count == 0` releases
 * everything. Returns false -- and changes nothing -- when the driver is not
 * parked, so a caller can never drive buttons through a login in flight. */
bool chat_login_play_request(const enum input_button* actions, unsigned count);

/* From CHAT_LOGIN_PARKED only: run a SECOND login on the still-bound pad.
 *
 * It re-enters at the first P1 step after the bind work, because the pad is
 * already bound AND already published to the device inventory -- P1 step 0's
 * real-pad refusal reads that inventory and would refuse against the driver's
 * own reflection. A restarted login therefore OMITS exactly two markers that a
 * first login emits: `p0.session` (the session observation P0 makes) and
 * `p1.pad_bound` (the bind it does not repeat). Every later marker --
 * p1.hold_refused, p1.chat_open, all of P2..P6 and run.ok -- is emitted
 * unchanged, in the same order.
 *
 * `seq` CONTINUES (it is the stream's own gapless counter, not a per-login
 * one) and so does the driver's ms clock; only the PER-LOGIN budget restarts,
 * so CHAT_LOGIN_TOTAL_MS is measured from this call and the parked time in
 * between is not charged to either login. Any pending play request is
 * released: a gameplay button still held would reach the composer.
 *
 * Returns false -- and changes nothing -- when the driver is not parked. */
bool chat_login_play_restart(void);

/* Withdraw the virtual pad and disarm, WITHOUT calling hooks.profile_done:
 * in play mode the mp_play driver owns the run's verdict. Safe to call in any
 * state; a no-op when play mode was never armed. */
void chat_login_play_end(void);

/* The two run-ending rules that stay live while PARKED -- a real pad appearing
 * on the driver's own channel, and the production controller-recovery modal --
 * have no run to end there, because the play driver owns completion. They
 * latch instead, and this is where the caller reads them: true once either
 * fired, with `*diag` set to the same wording cl_fail_at would have used.
 * First fault wins; a restart does NOT clear it. */
bool chat_login_play_fault(const char** diag);
/* One call per rendered frame, AFTER player 0's screen update: the driver
 * observes the frame the screen just consumed and posts the request the next
 * input_poll will turn into an owner frame. Inert when unarmed. */
void chat_login_frame(const struct chat_login_ctx* ctx);

#ifdef CAVEWII_TESTING
/* ---- the MODELLED console input layer (brief 12 D-1) --------------------
 * A PC build binds no F3 chord and compiles no order gate
 * (input_chord_order_apply is PLATFORM_WII-only), so without this a PC run can
 * never reach the state the real-Wii run mp_chat_chord_1m_db01e56_0919_173900
 * reached: ONE legal toggle spent and the debug overlay left ON. Arming the
 * model makes the driver treat this device as one that binds the chord and
 * applies the ORDER GATE -- with production's own pure machine
 * (input_chord_order_step) over the owner frame's two chord halves. The test's
 * config supplies the real chord binding; the test's main loop supplies
 * main.c's F3 hunk, reading chat_login_test_chord_edge() for the frame the
 * modelled layer fired on. See source/game/chat_login_script.c. */
/* Modes for chat_login_test_chord_model. */
#define CHAT_LOGIN_CHORD_MODEL_OFF 0
/* BOTH modes model a console: cl_chord_required() is true and the ORDER GATE
 * is applied, because that is the wiring a PC build lacks. They differ ONLY in
 * what they answer about the staged config -- and UNBOUND answers nothing at
 * all, deferring to the live cl_chord_scan, which is what makes it the mode
 * that can prove the scan (hostile review 17, H-1). */
#define CHAT_LOGIN_CHORD_MODEL_BOUND 1	 /* short-circuits "the config binds
										  * it", so it can never buy the scan */
#define CHAT_LOGIN_CHORD_MODEL_UNBOUND 2 /* answers NOTHING: the live scan
										  * decides, and p1.chord_bound
										  * refuses by name if it says no */
void chat_login_test_chord_model(int mode);
bool chat_login_test_chord_edge(void);
#endif

/* Observation for tests and diagnostics. */
enum chat_login_phase chat_login_phase_now(void);
uint32_t chat_login_seq_now(void);
unsigned chat_login_step_now(void);
/* Full teardown of the file-static state. Production never calls this (a run
 * is one process); tests call it between cases. */
void chat_login_reset(void);

#endif
