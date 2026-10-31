/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_MP_PLAY_CORE_H
#define GAME_MP_PLAY_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

/*
	Pure phase machine for HW_RUN_PROFILE_MP_PLAY_10M -- the unattended LAN
	proof of release blocker D1 (Astra gate 1, 2026-09-21: join, typed login
	inside the server deadline, >= 600 s of MEASURED play with server-matched
	digs / places / inventory transfers, a clean disconnect through the pause
	menu, a rejoin through the menus, server-owned state verified).

	CONTRACT: docs/mp_play_profile.md. That file is the authority on marker
	names, keys, order and the collector's PASS rule; this header implements
	the device half of it.

	Kept as a pure header (mp_reconnect_soak_core.h / autopilot_unstick.h
	precedent): zero gstate / screen / network dependency, so every transition
	is unit-testable off the engine. The impure half (mp_play_script.c) fills
	struct mpp_obs from OBSERVED production state once per rendered frame,
	calls mpp_tick(), and performs what struct mpp_out asks for: button
	ACTIONS go through the virtual pad -> production binding table -> owner
	latch; `walk` arms the existing walker + unstick; `aim` uses the existing
	harness aim lock (a stick cannot be synthesised -- stated, not hidden).

	WHAT THE CORE NEVER DOES: it never counts an action on client prediction.
	A dig or a place is counted only when the SERVER's block-change echo for
	that exact cell arrives; an inventory move only when the server accepted
	both clicks AND the resulting client slot state is the intended one. It
	never respawns, teleports or restores anything: death, a lost session or
	a fault ends the run at once, by name.
*/

/* ---- contract constants -------------------------------------------------- */

#define MPP_CONTRACT 1u
#define MPP_MEASURED_PLAY_MS 600000u
#define MPP_SETTLE_MS 3000u
/* 20 s (was 40, then 30; push-0921/B16). Both passing PC runs of 2026-09-21
 * met the place floor with EXACTLY 8: terrain refused 4 build legs each time
 * and shore brakes cost 8 stuck legs in the second. 20 s legs give about 21
 * cycles in the measured 600 s, so NINE refusals still leave the floors met
 * (tests/test_mp_play_core.c case_skip_margin) -- and more server-matched
 * actions per run is a stronger proof, not a weaker one. One leg still covers
 * the whole 64-block displacement floor (~4.3 blocks / s). */
#define MPP_WALK_LEG_MS 20000u
#define MPP_SHORT_LEG_MS 6000u /* retry leg after a skipped build */
/* Cycles are NOT counted: walk -> build -> (odd cycles) inventory repeats until
 * the measured clock reaches the floor, so the digs / places spread over ALL
 * of measured play (contract section 5) and one skipped build leg does not
 * leave the floors with zero margin (about ten cycles fit in 600 s). */
#define MPP_FLOOR_DIGS 8u
#define MPP_FLOOR_PLACES 8u
#define MPP_FLOOR_INV_MOVES 4u
/* The collector's displacement floor (MP_PLAY_MIN_DISPLACEMENT): the maximum
 * horizontal distance from the first gen-1 in-world position, teleports
 * removed. The device tracks its own copy only to decide walk_done_stuck. */
#define MPP_FLOOR_DISPLACEMENT 64.0f
#define MPP_LOGIN_DEADLINE_MS 60000u /* the server's own login deadline */
#define MPP_REJOIN_BUDGET_MS 90000u
#define MPP_STAND_MS 30000u
#define MPP_TOTAL_CAP_MS 1200000u
#define MPP_TELEPORT_BLOCKS 8.0f
#define MPP_POS_TOL_H 1.0f
#define MPP_POS_TOL_V 1.5f

/* Per-step budgets. A step that outlives its budget fails the run with the
 * step's name -- except the three BUILD sub-steps marked skippable, which
 * abandon the leg and count `build_skipped` (terrain the script cannot use is
 * not a defect; a floor missed because of it still fails at p2_end). */
#define MPP_READY_BUDGET_MS 120000u
#define MPP_LOGIN_BUDGET_MS 90000u
#define MPP_REST_BUDGET_MS 8000u	/* to come to rest after the walker stops */
#define MPP_CENTRE_BUDGET_MS 12000u /* to nudge into one column, after rest   */
#define MPP_NUDGE_MS 50u			/* one tick of FORWARD from rest ~ 0.2 block */
#define MPP_NUDGE_SETTLE_MS 450u	/* friction brings the body back to rest   */
#define MPP_NUDGE_MAX 14u
#define MPP_FACE_BUDGET_MS 1500u	/* the aim must face the centre by then    */
/* The body is 0.6 wide: inside [0.3, 0.7] it fits one column exactly, but a
 * body FLUSH against a wall rests at 0.7000008 and then still overlaps the
 * neighbour by an ulp -- it would not fall into the dug cell. So the window
 * keeps a real margin. */
#define MPP_CENTRE_LO 0.32f
#define MPP_CENTRE_HI 0.68f
#define MPP_LEG_MIN_PROGRESS 8.0f /* a walk leg that moved less turns 90 deg */
/* THE FIRST HEADING IS PART OF THE SCOUTED FIXTURE, like the spawn itself
 * (mc173-diag: seed 9999, MC173_SPAWN_X / _Z = -64 / -896, scouted flat and dry
 * in push-0921/B8). North = 180 in this header's convention (0 = +Z, 180 = -Z).
 * 120 blocks SOUTH of that spawn a cliff drops into pockets the walker can enter
 * -- the route probe accepts two-block drops -- and never leave -- it climbs one
 * block: PC runs pc_mp_play_20260921_182523 and _185536 both ended there with
 * "walker made no progress on four headings in a row", the second without the
 * script having dug anything nearby. Run _184003 walked NORTH for its whole ten
 * minutes. The world is the same on the console, so the choice carries over;
 * the 90-degree turns still apply from here. */
#define MPP_START_HEADING_DEG 180
/* A STUCK LEG NEVER BUILDS (push-0921/B16; PC run pc_mp_play_20260921_171336).
 * That run could not leave the hole it had just dug; the no-progress rule
 * turned the heading and the core built ANYWAY -- in the same column, five
 * times, straight down to stone, 600 s gone, "action floors not met". A leg
 * that went nowhere is now followed by ANOTHER LEG on the new heading; a body
 * that has not moved one block in MPP_STUCK_CHECK_MS (5 s: one second of
 * walker start-up, two to climb out of a hole, two to spare) is called stuck without
 * waiting the whole leg out; and MPP_STUCK_LEGS_MAX such legs in a row (every
 * heading tried) fail the run BY NAME instead of burning the measured clock. */
#define MPP_STUCK_CHECK_MS 5000u
#define MPP_STUCK_MIN_PROGRESS 1.0f
#define MPP_STUCK_LEGS_MAX 4u
/* FLOORS MET, THEN STUCK (real Wii hw1021f mp_play_10m). A walker that can no
 * longer move after every walk-phase floor is already met has nothing left to
 * prove by walking: instead of failing "no progress on four headings" (or
 * trying the pillar escape, which mc173 can refuse at the feet cell), it ends
 * the walk as SUCCESS with a `walk_done_stuck` marker naming the stuck
 * position, and stands until measured play reaches its floor; p2_end and its
 * action-floor check then run exactly as before. "Met" is the collector's
 * whole walk-phase set, judged on the device's own counts: digs / places /
 * inventory moves at their floors, one dig and one place in EACH third and
 * inventory moves in two thirds (the spread rule), and the displacement floor.
 * Anything short of that still fails exactly as before, and the collector
 * re-grades every floor from the server's receipts regardless. */
/* THE PILLAR ESCAPE (REL-14b-walker-escape; real Wii hw1020b fell 7 blocks into
 * a 2x3 shaft, no heading can leave it). Before the fourth stuck leg fails the
 * run, the walker stands on its own block: jump, put the held block under the
 * feet at the top of the jump, land one higher, walk again. One block per
 * step, MPP_ESCAPE_STEP_MS to gain it, MPP_ESCAPE_BLOCKS_MAX per episode (the
 * count restarts when a walk leg completes). A step that gains no height ends
 * the run with the SAME stuck reason: a sealed box still fails honestly. */
#define MPP_ESCAPE_STEP_MS 6000u
#define MPP_ESCAPE_BLOCKS_MAX 24u
#define MPP_ESCAPE_RISE 0.9f	/* landed at least this much higher = a block */
#define MPP_ESCAPE_PLACE_RISE 1.05f /* feet this high over the base: the cell
									 * under them is free of the body */
/* ONE REFUSED PLACE IS NOT A SEALED BOX (real Wii hw1021e mp_play_10m vs
 * mc173, seed 12345). The walker fell ~5 blocks into a pocket at
 * (-182.45, 86, -640.33), jumped, pressed USE at feet 87.17 -- and mc173 said
 * "use_block_stack: can_place refuse id=3 pos=[-183,86,-641]": its server-side
 * player entity is ticked with gravity between position packets, so its box
 * had already sunk back into the cell when the place arrived. The old step
 * pressed USE ONCE and then waited its whole 6 s out: "no height gained,
 * blocks=0". Now a landing without the rise is one failed ATTEMPT: the walker
 * jumps again within the same step, and each retry presses USE a little later
 * in the jump (0 / 50 / 100 ms after the feet clear the cell) so the place
 * meets a different server tick. Vanilla takes the first one.
 * AND THE POCKET NEEDS MORE BLOCKS THAN THE WALKER CARRIES: that run held ONE
 * dirt (mc173: "use_stack: index=7 stack_id=3 size=1") for a ~5-block climb.
 * With no placeable block in the hotbar the walker digs a hand-diggable wall
 * cell next to it (feet or head level, a horizontal neighbour: dig distance^2
 * ~2, far inside 36), waits for the drop, and pillars with it. Neither the
 * wall digs nor the pillar blocks count toward the action floors. */
#define MPP_ESCAPE_USE_STAGGER_MS 50u
#define MPP_ESCAPE_MINE_PITCH 20 /* crosshair on a neighbour's side face */
#define MPP_ESCAPE_AIM_SETTLE_MS 250u /* a scan cardinal's look settles first:
									   * the crosshair then belongs to it */
#define MPP_DIG_BUDGET_MS 12000u
#define MPP_PICKUP_BUDGET_MS 6000u
#define MPP_PLACE_AIM_BUDGET_MS 6000u /* all four cardinals together */
#define MPP_PLACE_BUDGET_MS 4000u
#define MPP_DROP_BUDGET_MS 4000u   /* to land in the dug cell before aiming   */
#define MPP_AIM_DIR_MS 1200u	   /* one cardinal of the placing pass         */
#define MPP_SCAN_DIR_MS 700u	   /* one cardinal of the scan, when not usable */
/* LOOK BEFORE YOU DIG (push-0921/B16; PC run pc_mp_play_20260921_182523). The
 * dig ITSELF can build the trap: at (-79,82,-774) the ground stood 83 to the W
 * and 84 to the N -- one step up, walkable -- and digging one down made both
 * two-high walls, with a dead-end pocket to the E and a four-block drop to the
 * S. From the surface, before the dig, the body looks at each cardinal at
 * MPP_SURVEY_PITCH: a neighbour LEVEL with the column shows its top face one
 * below the feet (or the plant standing on it); only those will be one step up
 * -- an exit, and a place target -- once the body stands one lower. Fewer than
 * MPP_SURVEY_MIN_LEVEL of them: no dig. */
#define MPP_SURVEY_PITCH 60
/* REVIEW B17 (first hostile review of this lane, 2026-09-21).
 * M1: a dig is credited only if ATTACK had really been held on the cell for at
 *     least MPP_DIG_MIN_HOLD_MS when the server said "air" (wall time, not
 *     frames: the Wii renders fewer of them; Beta's fastest hand dig of a
 *     block this script accepts is 750 ms) -- a cell emptied by falling sand or
 *     another agent is not this script's dig.
 * M4: one USE press the server never echoes is RETRIED once (after the face is
 *     read again); a leg that loses both is skipped by name; the run fails with
 *     the old diag only on the MPP_UNECHOED_LEGS_MAX-th such leg.
 * M6: the observation ring holds MPP_ECHO_RING echoes; when more than that many
 *     arrived since the baseline, "my echo is not there" proves nothing, and the
 *     leg is skipped by name instead of failing as a disagreement. */
#define MPP_DIG_MIN_HOLD_MS 300u
#define MPP_DIG_HOLD_GAP_MS 250u /* a longer gap off the cell breaks the hold  */
/* hw1030r: snapshot_pre was taken ~0.5 s before the server's collect of a real
 * dig's drop reached the client inventory, and the rejoin verify then saw an
 * extra item. The snapshot waits for MPP_SETTLE_QUIET_MS of quiet on BOTH the
 * client inventory and the last dig cell's air echo; a wait past
 * MPP_SETTLE_BUDGET_MS in the phase is an explicit failure, never a hang. */
#define MPP_SETTLE_QUIET_MS 1000u
#define MPP_SETTLE_BUDGET_MS 10000u
#define MPP_UNECHOED_LEGS_MAX 3u
#define MPP_ECHO_RING 16u
#define MPP_SURVEY_MIN_LEVEL 2u
#define MPP_CLEAR_BUDGET_MS 3000u  /* to break a plant standing on the target  */
#define MPP_PLACES_PER_CYCLE 2u	   /* a cycle may catch up ONE missed place    */
#define MPP_INV_STEP_BUDGET_MS 6000u
#define MPP_MENU_STEP_BUDGET_MS 10000u
#define MPP_TITLE_BUDGET_MS 30000u

#define MPP_PRESS_MS 120u /* a button press is held this long ...          */
#define MPP_GAP_MS 220u	  /* ... and the next press waits this long after it */

/* ---- vocabulary ---------------------------------------------------------- */

/* Abstract actions; mp_play_script.c maps each to its enum input_button. */
#define MPP_ACT_ATTACK (1u << 0)	   /* IB_ACTION1 (break)            */
#define MPP_ACT_USE (1u << 1)		   /* IB_ACTION2 (place)            */
#define MPP_ACT_OPEN_INVENTORY (1u << 2) /* IB_OPEN_INVENTORY           */
#define MPP_ACT_CLOSE (1u << 3)		   /* IB_INVENTORY (Close role)     */
#define MPP_ACT_PAUSE (1u << 4)		   /* IB_HOME                       */
#define MPP_ACT_GUI_UP (1u << 5)
#define MPP_ACT_GUI_DOWN (1u << 6)
#define MPP_ACT_GUI_LEFT (1u << 7)
#define MPP_ACT_GUI_RIGHT (1u << 8)
#define MPP_ACT_GUI_CLICK (1u << 9)
#define MPP_ACT_HOTBAR_NEXT (1u << 10) /* IB_SCROLL_RIGHT               */
#define MPP_ACT_FORWARD (1u << 11)	   /* IB_FORWARD (centring nudges)  */
#define MPP_ACT_JUMP (1u << 12)		   /* IB_JUMP (pillar escape; the map
										* must carry it before the Wii can)*/

enum mpp_screen {
	MPP_SCREEN_OTHER = 0,
	MPP_SCREEN_INGAME,
	MPP_SCREEN_INVENTORY,
	MPP_SCREEN_PAUSE,
	MPP_SCREEN_TITLE,
	MPP_SCREEN_MP_SETUP,
	MPP_SCREEN_MP_CONNECT,
	MPP_SCREEN_LOAD_WORLD,
	MPP_SCREEN_CHAT,
	MPP_SCREEN_DEATH,
};

/* What a menu screen reports as focused, so the core can walk focus by
 * direction without knowing any screen's layout. */
enum mpp_focus {
	MPP_FOCUS_UNKNOWN = 0,
	MPP_FOCUS_PAUSE_DISCONNECT, /* the pause row that leaves a remote session */
	MPP_FOCUS_TITLE_MULTIPLAYER,
	MPP_FOCUS_MPS_JOIN, /* Multiplayer: the configured server row + Join */
	MPP_FOCUS_OTHER,
};

enum mpp_login {
	MPP_LOGIN_IDLE = 0,
	MPP_LOGIN_RUNNING,
	MPP_LOGIN_OK,
	MPP_LOGIN_FAILED,
};

#define MPP_INV_SLOTS 36 /* main inventory incl. hotbar, window-0 numbering
							is the impure half's business: index 0..8 = hotbar */
#define MPP_FACE_TOP 1

struct mpp_item {
	int16_t item; /* 0 = empty */
	uint8_t count;
	int16_t damage;
};

struct mpp_echo {
	int32_t x, y, z;
	uint8_t type, metadata;
	uint32_t serial;
};

struct mpp_txn {
	int8_t window;
	int16_t action_id;
	bool accepted;
	uint32_t serial;
};

struct mpp_obs {
	uint32_t now_ms; /* any monotonic ms; rebased by the core */
	/* session */
	bool remote_session;
	bool world_loaded;
	bool body_ticking; /* the Beta blockExists gate has released the body */
	enum mpp_screen screen;
	enum mpp_focus focus;
	uint32_t install_gen;	 /* remote world installs in this process, 1-based */
	uint32_t timeout_admits; /* lworld_ready_timeout_admits() */
	bool fault; /* the login machine's two run-ending latches (a real pad on its
				 * channel, the controller-recovery modal). A watchdog or an
				 * exception cannot be observed by a script running in the
				 * process that died: the collector's heartbeat sweep owns
				 * those (review B17 M3). */
	/* body */
	float x, y, z;
	bool at_rest; /* on ground and no velocity */
	bool in_water; /* the body is in a fluid: no build leg here */
	/* Unit vector, on the horizontal plane, of where FORWARD would move the
	 * body RIGHT NOW (from the camera yaw actually in force). The core never
	 * trusts its own yaw convention: it asks for a yaw, then only presses
	 * FORWARD once this vector really points where it wants to go. */
	float fwd_x, fwd_z;
	int dimension;
	int health;
	/* crosshair */
	bool target_valid;
	int32_t tx, ty, tz;
	int tface;
	bool target_top_clear; /* the cell ABOVE the targeted cell is air */
	uint8_t target_type;   /* block id of the targeted cell (0 when none) */
	uint8_t under_feet_type; /* block id of the cell under the feet */
	/* The block one below the dig target must catch a one-block drop.
	 * Coordinates bind this observation to the intended dig, not a later
	 * body position after the support above has been removed. */
	int32_t support_x, support_y, support_z;
	bool support_known, support_safe;
	/* server confirmations (copied out of struct server_remote_status) */
	uint32_t echo_serial;
	struct mpp_echo echoes[16];
	/* THE WATCHED CELL (push-0921/B16): the newest server echo for the one
	 * cell mpp_out.watch named, kept by the network layer OUTSIDE the ring so a
	 * burst of unrelated block changes cannot evict it. serial 0 = none (also
	 * what a glue without the seam reports: the ring is then all there is). */
	struct mpp_echo watch_echo;
	/* hw1030g: EVERY echo stored for the watched cell since the watch was
	 * armed (newest-only masked a dig's air under a later record for the same
	 * cell), plus the network layer's own counters, reported on a miss. */
	struct mpp_echo watch_hist[4];
	uint32_t watch_hist_n; /* records stored for the watched cell since armed */
	uint32_t watch_bodies; /* Packet51 bodies that covered it since armed */
	uint32_t watch_clears; /* cumulative arm/move/off that dropped a record */
	bool watch_supported; /* the glue HAS the watched-cell seam (hook present) */
	uint32_t txn_serial;
	struct mpp_txn txns[8];
	/* client view of the player inventory */
	struct mpp_item inv[MPP_INV_SLOTS];
	struct mpp_item cursor;
	int held_hotbar;	/* 0..8 */
	int inv_focus_slot; /* index into inv[] the container focus is on, -1 = none */
	int16_t last_click_action_id; /* action id of the last click this client sent */
	enum mpp_login login;
};

#define MPP_MARKER_KEY 24
#define MPP_MARKER_BODY 420
#define MPP_MARKER_QUEUE 10 /* the LAST slot is reserved for `fail` */
#define MPP_INV_PARTS 3u	/* a snapshot's inventory travels in 3 x 12 slots */

struct mpp_marker {
	uint32_t seq;
	char key[MPP_MARKER_KEY];
	char body[MPP_MARKER_BODY]; /* JSON members WITHOUT the braces, may be "" */
};

struct mpp_out {
	unsigned actions; /* MPP_ACT_* held this frame */
	bool walk;
	int walk_heading_deg; /* fixed heading for the walker (same yaw convention
						   * as yaw_deg); the core turns it 90 deg after a leg
						   * that made no progress */
	bool aim;
	int yaw_deg, pitch_deg; /* pitch: positive = DOWN; the impure half maps
							 * this onto the aim lock's own sign convention */
	bool login_start; /* kick the shared login machine (edge, one frame) */
	/* THE ONE CELL THE SCRIPT IS WAITING ON (dig cell, place cell, the plant
	 * being cleared). Published every frame of the wait, from its FIRST frame,
	 * so the network layer is watching before the server can answer. */
	bool watch;
	int32_t watch_x, watch_y, watch_z;
};

enum mpp_phase {
	MPP_PH_IDLE = 0,
	MPP_PH_READY,	/* wait for the remote install + released body        */
	MPP_PH_LOGIN,	/* the shared login machine runs                      */
	MPP_PH_SETTLE,
	MPP_PH_WALK,
	MPP_PH_ESCAPE, /* pillar up out of a pit before the stuck run fails */
	MPP_PH_BUILD_REST,
	MPP_PH_BUILD_SURVEY, /* four looks from the surface before any dig */
	MPP_PH_BUILD_DIG,
	MPP_PH_BUILD_PICKUP,
	MPP_PH_BUILD_AIM,
	MPP_PH_BUILD_PLACE,
	MPP_PH_INV_OPEN,
	MPP_PH_INV_SRC,
	MPP_PH_INV_PICK,
	MPP_PH_INV_DST,
	MPP_PH_INV_PUT,
	MPP_PH_INV_CLOSE,
	MPP_PH_PAUSE_REST, /* the walker has let go; wait for the body to land */
	MPP_PH_PAUSE_OPEN,
	MPP_PH_PAUSE_FOCUS,
	MPP_PH_PAUSE_LEAVE,
	MPP_PH_TITLE,	/* wait for the title screen                           */
	MPP_PH_REJOIN_TITLE,
	MPP_PH_REJOIN_SETUP,
	MPP_PH_VERIFY,
	MPP_PH_STAND,
	MPP_PH_OK,
	MPP_PH_FAILED,
};

struct mpp_state {
	bool clock_set;
	uint32_t base_ms, now_ms, phase_ms;
	enum mpp_phase phase;
	unsigned gen; /* 1 or 2 */
	uint32_t seq;
	/* measured play */
	bool measuring;
	uint32_t p2_t0_ms, measured_ms;
	unsigned cycle, digs, picks, places, inv_moves, build_skipped;
	/* install / login timing */
	uint32_t install_ms[3];
	uint32_t rejoin_t0_ms;
	/* REL-14b: this LOGIN phase has seen the machine NOT report OK. An OK
	 * before that is the previous generation's PARKED, never this one's. */
	bool login_left;
	/* press sequencing */
	uint32_t press_until_ms, next_press_ms;
	unsigned press_act;
	bool once_done; /* this phase's single press has been issued */
	bool snap_done; /* snapshot_pre has been taken (pause open, body at rest) */
	bool inv_this_cycle; /* this cycle's inventory leg has run (or was skipped) */
	/* build leg */
	int32_t dig_x, dig_y, dig_z;
	uint8_t dig_old;
	uint32_t echo_seen, txn_seen;
	uint32_t wclr_seen; /* hw1030g: obs.watch_clears at the wait's baseline */
	int16_t pick_item;
	uint8_t pick_count_before;
	int place_dir; /* 0..3 */
	int32_t place_x, place_y, place_z;
	/* inventory leg */
	int inv_src, inv_dst;
	struct mpp_item inv_moving;
	int16_t inv_action_pick, inv_action_put, click_base;
	bool have_pick_id, have_put_id;
	/* teleport latch */
	bool have_last_pos;
	float last_x, last_y, last_z;
	/* walk legs + centring */
	int heading_deg;
	float leg_x, leg_z;
	bool leg_started; /* this walk leg's start position has been latched */
	unsigned stuck_legs; /* walk legs in a row that went nowhere */
	/* walk_done_stuck: thirds (bit 0..2) holding a counted dig / place /
	 * inventory move, and the device's own displacement record */
	unsigned dig_thirds, place_thirds, inv_thirds;
	bool disp_origin;
	float disp_x0, disp_z0, disp_off_x, disp_off_z, disp_max2;
	bool walk_stuck_done; /* the walk ended as success; stand until p2_end */
	unsigned inv_last_third; /* 1 + third of the last inventory move, 0 = none */
	bool short_leg;		 /* after a build skip: retry on a short walk leg */
	unsigned esc_blocks; /* pillar blocks this stuck episode */
	bool esc_based;		 /* the escape step has latched its base height */
	bool esc_gap;
	float esc_base_y;
	/* one pillar ATTEMPT inside an escape step (a step may take several) */
	bool esc_air;		   /* the body left the ground on this attempt     */
	bool esc_used;		   /* USE was pressed on this attempt              */
	uint32_t esc_above_ms; /* when the feet first cleared the cell (0 = not) */
	unsigned esc_tries;	   /* attempts of this step that landed unraised   */
	/* the crosshair at the latest frame of this attempt's USE window (feet
	 * clear of the cell), for the escape_try marker */
	bool esc_t_seen, esc_t_valid;
	int32_t esc_t_x, esc_t_y, esc_t_z;
	/* digging a wall cell for a block to pillar with */
	bool esc_scan_on, esc_mining;
	uint32_t esc_scan_t0_ms, esc_mine_t0_ms, esc_mine_wait_ms;
	int esc_mine_yaw;
	int32_t esc_mx, esc_my, esc_mz;
	uint8_t esc_mine_old;
	unsigned esc_mines; /* wall cells dug this stuck episode */
	/* PC-ONLY SHORT PLAY (push-0921/B16). 0 = the contract. Non-zero cuts
	 * measured play to this many ms and WAIVES the action floors, so the rejoin
	 * half (pause, leave, title, rejoin, login, verify) can meet the real app in
	 * three minutes instead of twenty-one. It can never pass a gate: the
	 * session marker states the floor really used, and the collector refuses
	 * anything but the contract's ("built against a different contract"). Set
	 * only by mp_play_set_short_play(), which main.c calls only off the Wii. */
	uint32_t short_play_ms;
	bool have_dig;		 /* dig_x / dig_z name a column that WAS dug */
	bool rested; /* BUILD_REST has seen the body at rest once */
	uint32_t rest_ms, nudge_until_ms, nudge_next_ms, face_since_ms;
	unsigned nudges;
	uint8_t under_type; /* last observed block id under the feet (diagnostics) */
	unsigned dig_on_frames; /* frames this dig leg held ATTACK on the dig cell */
	/* place sweep */
	bool aim_ready;			/* the body has landed in the dug cell, at rest */
	uint32_t aim_t0_ms;		/* when the sweep (not the phase) started       */
	uint32_t aim_dir_t0_ms; /* when the current cardinal started            */
	bool clearing;			/* holding ATTACK on a plant over the target    */
	int32_t clear_x, clear_y, clear_z;
	uint32_t clear_t0_ms, clear_echo;
	unsigned plants_cleared, places_this_cycle, pickups_missed;
	/* KEEP A WAY OUT: all four cardinals are read before any place */
	bool scan_done;
	unsigned scan_dir;		 /* the cardinal being read, 0..3                */
	unsigned usable_mask;	 /* bit d: place_dir d has a usable top face     */
	unsigned places_allowed; /* this cycle: usable cardinals - 1, capped     */
	unsigned survey_dir;	 /* the cardinal being surveyed, 0..3            */
	int16_t place_item;		 /* the block in the hand when USE was pressed   */
	bool dig_on_seen;		 /* ATTACK has been held on the dig cell this leg */
	uint32_t dig_on_t0_ms;	 /* ... since then (the current CONTINUOUS hold)  */
	uint32_t dig_last_on_ms; /* the last frame the crosshair was on the cell  */
	/* hw1030r: the hold's shape, for the build_skipped marker -- the widest
	 * gap between two on-cell frames, and between any two frames, this leg */
	uint32_t dig_prev_frame_ms, dig_on_gap_max_ms, dig_frame_gap_max_ms;
	/* hw1030r SETTLE (rejoin verify race): snapshot_pre waits until the client
	 * inventory and the last dig cell's air echo have both been quiet for
	 * MPP_SETTLE_QUIET_MS. Tracked every in-world frame. */
	bool inv_prev_set;
	struct mpp_item inv_prev[MPP_INV_SLOTS];
	uint32_t inv_change_ms; /* the last frame o->inv differed from inv_prev */
	bool dcell_armed;		/* dcell_* names the last watched dig cell      */
	int32_t dcell_x, dcell_y, dcell_z;
	uint32_t dcell_serial; /* echoes up to this serial are already counted */
	bool dig_air_seen;
	uint32_t dig_air_ms; /* the last frame an air echo for dcell arrived    */
	bool place_retried;		 /* this place already used its one retry        */
	unsigned unechoed_legs;	 /* place legs lost to an unechoed USE, this run */
	unsigned level_mask;	 /* bit d: that neighbour is level with the column */
	unsigned teleports;
	/* snapshots */
	float pre_x, pre_y, pre_z;
	int pre_dim;
	struct mpp_item pre_inv[MPP_INV_SLOTS];	 /* snapshot_pre: what VERIFY compares */
	struct mpp_item pick_inv[MPP_INV_SLOTS]; /* the pickup baseline of THIS dig --
											  * its own storage since review B17
											  * L4: the run's verdict must not
											  * share a buffer with a scratch */
	/* verdict */
	char fail_step[24];
	char fail_diag[96];
	/* markers */
	struct mpp_marker queue[MPP_MARKER_QUEUE];
	unsigned q_head, q_count;
	unsigned markers_dropped; /* queue overflow: fails the run */
};

/* ---- markers ------------------------------------------------------------- */

static inline void mpp_mark(struct mpp_state* s, const char* key,
							const char* body) {
	/* The last slot is kept for `fail`: a run whose queue is stuck must still
	 * be able to say so on the wire ("a stuck run always reports"). */
	unsigned cap = strcmp(key, "fail") == 0 ? MPP_MARKER_QUEUE
											  : MPP_MARKER_QUEUE - 1u;
	if(s->q_count >= cap) {
		s->markers_dropped++;
		return;
	}
	struct mpp_marker* m
		= &s->queue[(s->q_head + s->q_count) % MPP_MARKER_QUEUE];
	m->seq = ++s->seq;
	snprintf(m->key, sizeof(m->key), "%s", key);
	snprintf(m->body, sizeof(m->body), "%s", body ? body : "");
	s->q_count++;
}

/* The impure half: peek, try to emit, pop ONLY when the sender took it (the
 * same line with the same seq is retried next frame otherwise). */
static inline const struct mpp_marker* mpp_marker_peek(
	const struct mpp_state* s) {
	return s->q_count ? &s->queue[s->q_head] : NULL;
}

static inline void mpp_marker_pop(struct mpp_state* s) {
	if(!s->q_count)
		return;
	s->q_head = (s->q_head + 1) % MPP_MARKER_QUEUE;
	s->q_count--;
}

/* ---- helpers ------------------------------------------------------------- */

static inline const char* mpp_phase_name(enum mpp_phase p) {
	switch(p) {
		case MPP_PH_READY: return "ready";
		case MPP_PH_LOGIN: return "login";
		case MPP_PH_SETTLE: return "settle";
		case MPP_PH_WALK: return "walk";
		case MPP_PH_ESCAPE: return "escape";
		case MPP_PH_BUILD_REST: return "build_rest";
		case MPP_PH_BUILD_SURVEY: return "build_survey";
		case MPP_PH_BUILD_DIG: return "build_dig";
		case MPP_PH_BUILD_PICKUP: return "build_pickup";
		case MPP_PH_BUILD_AIM: return "build_aim";
		case MPP_PH_BUILD_PLACE: return "build_place";
		case MPP_PH_INV_OPEN: return "inv_open";
		case MPP_PH_INV_SRC: return "inv_src";
		case MPP_PH_INV_PICK: return "inv_pick";
		case MPP_PH_INV_DST: return "inv_dst";
		case MPP_PH_INV_PUT: return "inv_put";
		case MPP_PH_INV_CLOSE: return "inv_close";
		case MPP_PH_PAUSE_REST: return "pause_rest";
		case MPP_PH_PAUSE_OPEN: return "pause_open";
		case MPP_PH_PAUSE_FOCUS: return "pause_focus";
		case MPP_PH_PAUSE_LEAVE: return "pause_leave";
		case MPP_PH_TITLE: return "title";
		case MPP_PH_REJOIN_TITLE: return "rejoin_title";
		case MPP_PH_REJOIN_SETUP: return "rejoin_setup";
		case MPP_PH_VERIFY: return "verify";
		case MPP_PH_STAND: return "stand";
		case MPP_PH_OK: return "ok";
		case MPP_PH_FAILED: return "failed";
		default: return "idle";
	}
}

static inline void mpp_enter(struct mpp_state* s, enum mpp_phase p) {
	s->phase = p;
	s->phase_ms = s->now_ms;
	s->press_until_ms = 0;
	s->press_act = 0;
	s->once_done = false;
}

static inline void mpp_fail(struct mpp_state* s, const char* diag) {
	char body[MPP_MARKER_BODY];
	snprintf(s->fail_step, sizeof(s->fail_step), "%s", mpp_phase_name(s->phase));
	snprintf(s->fail_diag, sizeof(s->fail_diag), "%s", diag);
	snprintf(body, sizeof(body),
			 "\"phase\":\"%s\",\"gen\":%u,\"diag\":\"%s\",\"measured_ms\":%u",
			 s->fail_step, s->gen, s->fail_diag, (unsigned)s->measured_ms);
	s->phase = MPP_PH_FAILED;
	mpp_mark(s, "fail", body);
}

static inline uint32_t mpp_in_phase(const struct mpp_state* s) {
	return s->now_ms - s->phase_ms;
}

/* A fresh pillar attempt: jump from rest, USE once the feet clear the cell. */
static inline void mpp_escape_attempt_reset(struct mpp_state* s) {
	s->esc_air = s->esc_used = s->esc_gap = false;
	s->esc_above_ms = 0;
	s->esc_t_seen = s->esc_t_valid = false;
	s->esc_t_x = s->esc_t_y = s->esc_t_z = 0;
}

/* A fresh escape step: re-latch the base height, no attempt, no wall dig. */
static inline void mpp_escape_step_reset(struct mpp_state* s) {
	mpp_escape_attempt_reset(s);
	s->esc_based = false;
	s->esc_tries = 0;
	s->esc_scan_on = s->esc_mining = false;
	s->esc_mine_wait_ms = 0;
	/* the one-column centring before the base latches (BUILD_REST's taps) */
	s->nudges = 0;
	s->nudge_until_ms = s->nudge_next_ms = s->face_since_ms = 0;
}

/* One clean press: hold `act` for MPP_PRESS_MS, then a MPP_GAP_MS neutral gap
 * (production needs a released frame to derive the next pressed edge).
 * Returns true on the frame the press STARTS. */
static inline bool mpp_press(struct mpp_state* s, struct mpp_out* out,
							 unsigned act) {
	if(s->now_ms < s->press_until_ms) {
		out->actions |= s->press_act;
		return false;
	}
	if(s->now_ms < s->next_press_ms)
		return false;
	s->press_act = act;
	s->press_until_ms = s->now_ms + MPP_PRESS_MS;
	s->next_press_ms = s->press_until_ms + MPP_GAP_MS;
	out->actions |= act;
	return true;
}

/* Exactly ONE press for the current phase, however long the phase lasts. */
static inline void mpp_press_once(struct mpp_state* s, struct mpp_out* out,
								  unsigned act) {
	if(s->once_done) {
		if(s->now_ms < s->press_until_ms)
			out->actions |= s->press_act;
		return;
	}
	if(mpp_press(s, out, act))
		s->once_done = true;
}

static inline bool mpp_pressing(const struct mpp_state* s) {
	return s->now_ms < s->next_press_ms;
}

static inline bool mpp_item_eq(const struct mpp_item* a,
							   const struct mpp_item* b) {
	if(a->item <= 0 && b->item <= 0)
		return true;
	return a->item == b->item && a->count == b->count
		&& a->damage == b->damage;
}

static inline const struct mpp_echo* mpp_echo_for(const struct mpp_obs* o,
												  uint32_t after_serial,
												  int32_t x, int32_t y,
												  int32_t z) {
	const struct mpp_echo* best = NULL;
	for(size_t k = 0; k < 16; k++) {
		const struct mpp_echo* e = &o->echoes[k];
		if(e->serial > after_serial && e->x == x && e->y == y && e->z == z
		   && (!best || e->serial > best->serial))
			best = e;
	}
	/* the watch slot: same rules, never evicted */
	const struct mpp_echo* we = &o->watch_echo;
	if(we->serial > after_serial && we->x == x && we->y == y && we->z == z
	   && (!best || we->serial > best->serial))
		best = we;
	for(size_t k = 0; k < 4; k++) { /* hw1030g: the cell's history, same rules */
		const struct mpp_echo* h = &o->watch_hist[k];
		if(h->serial > after_serial && h->x == x && h->y == y && h->z == z
		   && (!best || h->serial > best->serial))
			best = h;
	}
	return best;
}

static inline const struct mpp_txn* mpp_txn_for(const struct mpp_obs* o,
												uint32_t after_serial,
												int16_t action_id) {
	for(size_t k = 0; k < 8; k++)
		if(o->txns[k].serial > after_serial
		   && o->txns[k].action_id == action_id && o->txns[k].window == 0)
			return &o->txns[k];
	return NULL;
}

static inline int32_t mpp_floor(float v) {
	int32_t i = (int32_t)v;
	return (float)i > v ? i - 1 : i;
}

static inline float mpp_frac(float v) {
	return v - (float)mpp_floor(v);
}

/* THE ONE-COLUMN CENTRING, shared by BUILD_REST and the pillar escape (which
 * must not jump while the 0.6-wide body straddles two columns: real Wii
 * hw1023c rested at z -802.16 over a 2-lower neighbour column, the downward
 * crosshair met nothing inside reach, and nine jumps placed nothing). */
static inline bool mpp_centred(const struct mpp_obs* o) {
	float fx = mpp_frac(o->x), fz = mpp_frac(o->z);
	return fx >= MPP_CENTRE_LO && fx <= MPP_CENTRE_HI && fz >= MPP_CENTRE_LO
		&& fz <= MPP_CENTRE_HI;
}

/* The yaw that faces the column centre. Yaw convention of THIS header
 * (Beta's): 0 = +Z, 90 = -X, i.e. forward = (-sin yaw, cos yaw). The impure
 * half maps it onto the aim lock; fwd_x / fwd_z report what really came out. */
static inline int mpp_centre_yaw(const struct mpp_obs* o) {
	float cx = 0.5f - mpp_frac(o->x), cz = 0.5f - mpp_frac(o->z);
	if(sqrtf(cx * cx + cz * cz) <= 0.0001f)
		return 0;
	float deg = atan2f(-cx, cz) * (180.0f / 3.14159265f);
	return ((int)(deg + (deg < 0.0f ? -0.5f : 0.5f)) + 360) % 360;
}

/* One single-tick FORWARD tap toward the column centre, issued only while the
 * OBSERVED facing vector points at it. The caller has checked: at rest, not
 * centred, tap budget left. Returns a failure reason, or NULL (tapped, or
 * still turning to face the centre). */
static inline const char* mpp_centre_tap(struct mpp_state* s,
										 const struct mpp_obs* o,
										 struct mpp_out* out) {
	float cx = 0.5f - mpp_frac(o->x), cz = 0.5f - mpp_frac(o->z);
	float cl = sqrtf(cx * cx + cz * cz);
	float dot = cl > 0.0001f ? (o->fwd_x * cx + o->fwd_z * cz) / cl : 1.0f;
	if(dot < 0.95f) {
		if(s->face_since_ms == 0u)
			s->face_since_ms = s->now_ms;
		else if(s->now_ms - s->face_since_ms >= MPP_FACE_BUDGET_MS)
			return "aim never faced the column centre";
		return NULL;
	}
	s->face_since_ms = 0u;
	s->nudges++;
	s->nudge_until_ms = s->now_ms + MPP_NUDGE_MS;
	s->nudge_next_ms = s->nudge_until_ms + MPP_NUDGE_SETTLE_MS;
	out->actions |= MPP_ACT_FORWARD;
	return NULL;
}

static inline int mpp_find_stack(const struct mpp_obs* o) {
	for(int k = 0; k < MPP_INV_SLOTS; k++)
		if(o->inv[k].item > 0 && o->inv[k].count > 0)
			return k;
	return -1;
}

static inline int mpp_find_empty(const struct mpp_obs* o, int src) {
	/* Stay in the region of the SOURCE slot. Beta merges a pickup into an
	 * existing partial stack first (InventoryPlayer.storeItemStack), so a dirt
	 * stack moved out of the hotbar would swallow every later pickup and the
	 * place leg would never hold a block again. Hotbar -> hotbar keeps it
	 * holdable; BUILD_AIM scrolls the held slot to wherever the stack sits. */
	/* THE LAST EMPTY SLOT OF THE REGION, NOT THE FIRST (PC run
	 * pc_mp_play_20260921_191831). Pickups do not wait for the inventory
	 * screen: with the stack on the cursor a new item goes to the FIRST empty
	 * slot -- which was exactly the destination this function used to pick, so
	 * the put landed on three freshly collected items, made five, and "exactly
	 * what I moved" never came true. Nothing lands first in the last empty
	 * slot while an earlier one is free. */
	int lo = src < 9 ? 0 : 9, hi = src < 9 ? 9 : MPP_INV_SLOTS;
	/* ...and with ONE empty slot in the region it is also the first, so a
	 * pickup between the two clicks can take it (review B21 M3): then there is
	 * no safe destination, and the caller skips the move by name. */
	int empties = 0;
	for(int k = lo; k < hi; k++)
		if(k != src && o->inv[k].item <= 0)
			empties++;
	if(empties < 2)
		return -1;
	for(int k = hi - 1; k >= lo; k--)
		if(k != src && o->inv[k].item <= 0)
			return k;
	for(int k = MPP_INV_SLOTS - 1; k >= 0; k--)
		if(k != src && o->inv[k].item <= 0)
			return k;
	return -1;
}

/* Blocks a bare hand digs quickly AND that drop a placeable block: grass (2,
 * drops dirt), dirt (3), sand (12). Stone-class blocks drop nothing by hand
 * and gravel (13) can drop flint, which cannot be placed. */
/* Plants that may stand on the block we want to place against, all broken
 * by hand in one tick (hardness 0) except the snow layer (0.1): sapling 6,
 * tall grass 31, dead bush 32, flowers 37 / 38, mushrooms 39 / 40, snow layer
 * 78. On a grass plain tall grass covers a large share of the cells -- the
 * first full run lost 4 of 13 place legs to it. Breaking one is what a player
 * does; whatever it drops (seeds) is not placeable and changes nothing. */
static inline bool mpp_is_plant(uint8_t type) {
	return type == 6 || type == 31 || type == 32 || type == 37 || type == 38
		|| type == 39 || type == 40 || type == 78;
}

static inline unsigned mpp_bits4(unsigned m) {
	return (m & 1u) + ((m >> 1) & 1u) + ((m >> 2) & 1u) + ((m >> 3) & 1u);
}

/* WHICH usable cardinal gets the next block -- and which one never does.
 *
 * The body stands in the cell it dug. A neighbour whose ground is level with
 * the old surface stands exactly ONE above the floor of that cell: it is the
 * only kind of neighbour the body can climb, and it is exactly the kind whose
 * top face this leg can place on. So every usable cardinal is a way out, and a
 * block on it closes it. Read from the saved worlds of PC runs 171336 and
 * 172431: both holes had two such neighbours (the other two were natural ground
 * one higher -- walls), the catch-up put a block on both, and the walker never
 * left; the older core then dug the same column down to stone.
 *
 * ONE usable cardinal is therefore always kept: the one on the walk heading if
 * that is usable (the walker tries it first), else the highest bit. Returns the
 * cardinal to place on, or -1 when only the kept one is left. */
static inline int mpp_pick_place_dir(unsigned usable_mask, int heading_deg) {
	int keep = -1;
	/* ANY heading (review B17 L1): C's % keeps the sign of the dividend, and a
	 * shift by a negative count is undefined. */
	int h = ((heading_deg % 360) + 360) % 360;
	if(mpp_bits4(usable_mask) < 2u)
		return -1;
	if(h % 90 == 0 && ((usable_mask >> (h / 90)) & 1u))
		keep = h / 90;
	else
		for(int d = 3; d >= 0; d--)
			if((usable_mask >> d) & 1u) {
				keep = d;
				break;
			}
	for(int d = 0; d < 4; d++)
		if(((usable_mask >> d) & 1u) && d != keep)
			return d;
	return -1;
}

static inline uint32_t mpp_measured_play_ms(const struct mpp_state* s) {
	return s->short_play_ms ? s->short_play_ms : MPP_MEASURED_PLAY_MS;
}

/* The newest echo for that cell, after the baseline, that says THIS block. */
static inline const struct mpp_echo* mpp_echo_for_type(const struct mpp_obs* o,
													   uint32_t after_serial,
													   int32_t x, int32_t y,
													   int32_t z, int16_t type) {
	const struct mpp_echo* best = NULL;
	for(size_t k = 0; k < 16; k++) {
		const struct mpp_echo* e = &o->echoes[k];
		if(e->serial > after_serial && e->x == x && e->y == y && e->z == z
		   && (int16_t)e->type == type && (!best || e->serial > best->serial))
			best = e;
	}
	const struct mpp_echo* we = &o->watch_echo;
	if(we->serial > after_serial && we->x == x && we->y == y && we->z == z
	   && (int16_t)we->type == type && (!best || we->serial > best->serial))
		best = we;
	/* hw1030g: the cell's history -- a later record for the same cell (water
	 * into a dug hole) must not hide the one that says THIS block. */
	for(size_t k = 0; k < 4; k++) {
		const struct mpp_echo* h = &o->watch_hist[k];
		if(h->serial > after_serial && h->x == x && h->y == y && h->z == z
		   && (int16_t)h->type == type && (!best || h->serial > best->serial))
			best = h;
	}
	return best;
}

/* hw1030g: a "never echoed" verdict carries what the core actually saw, so a
 * future miss is explained by the marker alone: the echo counter and the
 * baseline, the watched slot, the network layer's history/body/clear counters
 * (clears are counted since this wait's baseline), the hold, and then as many
 * of the newest ring records within one column of the cell (dx,dy,dz
 * relative, type, serial; up to 4) as still fit -- the body is always valid
 * JSON members and never truncated mid-token. Falls back to the plain body
 * only if even the fixed part would not fit (it does: see the suite's
 * worst-case check). */
static inline void mpp_fail_echo(struct mpp_state* s, const struct mpp_obs* o,
								 const char* diag, int32_t x, int32_t y,
								 int32_t z) {
	char body[MPP_MARKER_BODY];
	snprintf(s->fail_step, sizeof(s->fail_step), "%s", mpp_phase_name(s->phase));
	snprintf(s->fail_diag, sizeof(s->fail_diag), "%s", diag);
	int n = snprintf(
		body, sizeof(body),
		"\"phase\":\"%s\",\"gen\":%u,\"diag\":\"%s\",\"measured_ms\":%u,"
		"\"cell\":[%d,%d,%d],\"echo_serial\":%u,\"echo_seen\":%u,"
		"\"watch_echo\":[%d,%d,%d,%u,%u],\"watch_n\":%u,\"watch_bodies\":%u,"
		"\"watch_clears\":%u,\"hold_frames\":%u",
		s->fail_step, s->gen, s->fail_diag, (unsigned)s->measured_ms, (int)x,
		(int)y, (int)z, (unsigned)o->echo_serial, (unsigned)s->echo_seen,
		(int)o->watch_echo.x, (int)o->watch_echo.y, (int)o->watch_echo.z,
		(unsigned)o->watch_echo.type, (unsigned)o->watch_echo.serial,
		(unsigned)o->watch_hist_n, (unsigned)o->watch_bodies,
		(unsigned)(o->watch_clears - s->wclr_seen), (unsigned)s->dig_on_frames);
	/* `,"near":[` + `]` must fit after the fixed part */
	if(n < 0 || (size_t)n + 11u >= sizeof(body))
		snprintf(body, sizeof(body),
				 "\"phase\":\"%s\",\"gen\":%u,\"diag\":\"%s\","
				 "\"measured_ms\":%u",
				 s->fail_step, s->gen, s->fail_diag, (unsigned)s->measured_ms);
	else {
		size_t len = (size_t)n;
		unsigned shown = 0;
		len += (size_t)snprintf(body + len, sizeof(body) - len, ",\"near\":[");
		for(size_t k = 0; k < 16 && shown < 4u; k++) {
			const struct mpp_echo* e = &o->echoes[k]; /* newest first */
			int32_t dx = e->x - x, dy = e->y - y, dz = e->z - z;
			char one[64];
			if(!e->serial || dx < -1 || dx > 1 || dz < -1 || dz > 1 || dy < -2
			   || dy > 2)
				continue;
			int m = snprintf(one, sizeof(one), "%s[%d,%d,%d,%u,%u]",
							 shown ? "," : "", (int)dx, (int)dy, (int)dz,
							 (unsigned)e->type, (unsigned)e->serial);
			/* keep room for the closing bracket and the terminator */
			if(m < 0 || len + (size_t)m + 2u > sizeof(body))
				break;
			memcpy(body + len, one, (size_t)m);
			len += (size_t)m;
			shown++;
		}
		body[len++] = ']';
		body[len] = 0;
	}
	s->phase = MPP_PH_FAILED;
	mpp_mark(s, "fail", body);
}

static inline bool mpp_hand_diggable(uint8_t type) {
	/* Gravel (13) is in: it digs by hand in 0.9 s and drops itself 9 times in
	 * 10 (a flint drop is harmless -- it is not placeable, so that cycle just
	 * keeps its reserve). The first real runs parked on non-diggable ground
	 * in 10 of 13 build legs; every skip marker now names the block id. */
	return type == 2 || type == 3 || type == 12 || type == 13;
}

/* Core inv[] index -> protocol window-0 slot number (what the fixture's
 * receipts print): hotbar 0..8 -> 36..44, main grid 9..35 unchanged. */
static inline int mpp_proto_slot(int k) {
	return k < 9 ? 36 + k : k;
}

/* A snapshot = one head marker + MPP_INV_PARTS `inv` markers of 12 core slots
 * each, so no body is ever truncated (36 occupied slots do not fit one line).
 * Empty slots are omitted; the collector compares the union with the
 * fixture's snapshot receipt slot by slot (item, count, damage). */
static inline void mpp_snapshot(struct mpp_state* s, const char* key,
								const struct mpp_obs* o) {
	char body[MPP_MARKER_BODY];
	snprintf(body, sizeof(body),
			 "\"gen\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"dim\":%d,"
			 "\"parts\":%u",
			 s->gen, (double)o->x, (double)o->y, (double)o->z, o->dimension,
			 MPP_INV_PARTS);
	mpp_mark(s, key, body);
	for(unsigned part = 0; part < MPP_INV_PARTS; part++) {
		size_t n = (size_t)snprintf(body, sizeof(body),
									"\"of\":\"%s\",\"gen\":%u,\"part\":%u,"
									"\"slots\":[",
									key, s->gen, part);
		bool first = true;
		for(int k = (int)part * 12; k < (int)part * 12 + 12; k++) {
			if(o->inv[k].item <= 0)
				continue;
			/* snprintf returns what it WOULD have written (review B17 M5): an
			 * entry that does not fit whole is dropped and the list closed,
			 * never an n past the buffer. Worst case today is 46 + 12 x 23 =
			 * 322 of MPP_MARKER_BODY bytes. */
			int w = snprintf(body + n, sizeof(body) - n, "%s[%d,%d,%u,%d]",
							 first ? "" : ",", mpp_proto_slot(k),
							 (int)o->inv[k].item, (unsigned)o->inv[k].count,
							 (int)o->inv[k].damage);
			if(w < 0 || (size_t)w >= sizeof(body) - n - 1u) {
				body[n] = 0;
				break;
			}
			n += (size_t)w;
			first = false;
		}
		snprintf(body + n, sizeof(body) - n, "]");
		mpp_mark(s, "inv", body);
	}
}

/* ---- lifecycle ----------------------------------------------------------- */

static inline void mpp_reset(struct mpp_state* s) {
	memset(s, 0, sizeof(*s));
}

static inline bool mpp_done(const struct mpp_state* s) {
	return s->phase == MPP_PH_OK || s->phase == MPP_PH_FAILED;
}

/* The unattended pad owns menu pointer queries only during its disconnect /
 * rejoin walk. Other phases, a completed profile, and an unarmed driver all
 * leave the native mouse/IR alone. Shared with the impure input seam. */
static inline bool mpp_pointer_script_owns(bool armed,
										const struct mpp_state* s) {
	return armed && !mpp_done(s)
		&& s->phase >= MPP_PH_PAUSE_OPEN
		&& s->phase <= MPP_PH_REJOIN_SETUP;
}

/* The status string for hw_telemetry_emit_profile_done. */
static inline const char* mpp_status(const struct mpp_state* s, char* buf,
									 size_t cap) {
	if(s->phase == MPP_PH_OK)
		return "ok";
	snprintf(buf, cap, "failed:%s", s->fail_step[0] ? s->fail_step : "run");
	return buf;
}

/* Third (0..2) of measured play now: the collector's spread rule needs the
 * inventory moves in at least two of them. */
static inline unsigned mpp_play_third(const struct mpp_state* s) {
	uint32_t t = (uint32_t)(((uint64_t)s->measured_ms * 3u)
							/ mpp_measured_play_ms(s));
	return t > 2u ? 2u : t;
}

/* A cycle is WALK -> BUILD (rest, dig, pickup, [inventory], aim, place).
 * The inventory leg sits BETWEEN the pickup and the place on odd cycles: that
 * is the one moment a fresh player is guaranteed to hold a stack (a fresh
 * world gives an empty inventory, and each build places what it dug). */
static inline void mpp_cycle_done(struct mpp_state* s) {
	s->cycle++;
	s->inv_this_cycle = false;
	s->places_this_cycle = 0;
	mpp_enter(s, MPP_PH_WALK);
}

static inline void mpp_after_pickup(struct mpp_state* s) {
	s->aim_ready = false;
	/* Odd cycles as before; once terrain has refused a build leg, EVERY
	 * successful cycle may move a stack until the floor is met, but only in a
	 * different third of measured play than the last move (spread rule). */
	if(((s->cycle & 1u) == 1u
	    || (s->build_skipped > 0
		&& mpp_play_third(s) + 1u != s->inv_last_third))
	   && !s->inv_this_cycle
	   && s->inv_moves < MPP_FLOOR_INV_MOVES)
		mpp_enter(s, MPP_PH_INV_OPEN);
	else
		mpp_enter(s, MPP_PH_BUILD_AIM);
}

/* Every walk-phase floor met on the device's own counts (see
 * MPP_STUCK_LEGS_MAX's FLOORS MET note). Never true for a run short of any. */
static inline bool mpp_walk_floors_met(const struct mpp_state* s) {
	unsigned inv_thirds = (s->inv_thirds & 1u) + ((s->inv_thirds >> 1) & 1u)
		+ ((s->inv_thirds >> 2) & 1u);
	return s->digs >= MPP_FLOOR_DIGS && s->places >= MPP_FLOOR_PLACES
		&& s->inv_moves >= MPP_FLOOR_INV_MOVES && s->dig_thirds == 7u
		&& s->place_thirds == 7u && inv_thirds >= 2u && s->disp_origin
		&& s->disp_max2 >= MPP_FLOOR_DISPLACEMENT * MPP_FLOOR_DISPLACEMENT;
}

/* End the walk as success at the stuck position: no more walking, building
 * or escaping; the WALK phase only waits for measured play to end. */
static inline void mpp_walk_done_stuck(struct mpp_state* s,
									   const struct mpp_obs* o) {
	char body[MPP_MARKER_BODY];
	s->walk_stuck_done = true;
	s->leg_started = false;
	snprintf(body, sizeof(body),
			 "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"measured_ms\":%u,"
			 "\"heading_deg\":%d,\"stuck\":%u,\"digs\":%u,\"places\":%u,"
			 "\"inv_moves\":%u,\"disp\":%.1f",
			 (double)o->x, (double)o->y, (double)o->z,
			 (unsigned)s->measured_ms, s->heading_deg, s->stuck_legs, s->digs,
			 s->places, s->inv_moves, (double)sqrtf(s->disp_max2));
	mpp_mark(s, "walk_done_stuck", body);
	mpp_enter(s, MPP_PH_WALK);
}

static inline void mpp_skip_build_x(struct mpp_state* s, const char* why,
									const char* extra) {
	char body[MPP_MARKER_BODY];
	s->build_skipped++;
	snprintf(body, sizeof(body),
			 "\"cycle\":%u,\"why\":\"%s\",\"under\":%u,\"measured_ms\":%u%s",
			 s->cycle, why, (unsigned)s->under_type, (unsigned)s->measured_ms,
			 extra);
	mpp_mark(s, "build_skipped", body);
	s->short_leg = true;
	mpp_cycle_done(s);
}

static inline void mpp_skip_build(struct mpp_state* s, const char* why) {
	mpp_skip_build_x(s, why, "");
}

static inline bool mpp_landing_support_matches(const struct mpp_obs* o,
											 int32_t dig_x, int32_t dig_y, int32_t dig_z) {
	return o->support_known && o->support_safe && o->support_x == dig_x
		&& o->support_y == dig_y - 1 && o->support_z == dig_z;
}

static inline void mpp_tick(struct mpp_state* s, const struct mpp_obs* o,
							struct mpp_out* out) {
	char body[MPP_MARKER_BODY];
	memset(out, 0, sizeof(*out));
	if(!s->clock_set) {
		s->clock_set = true;
		s->base_ms = o->now_ms;
	}
	s->now_ms = o->now_ms - s->base_ms; /* unsigned: wrap-safe */
	if(mpp_done(s))
		return;
	if(s->phase == MPP_PH_IDLE) {
		s->gen = 1;
		s->heading_deg = MPP_START_HEADING_DEG;
		snprintf(body, sizeof(body),
				 "\"contract\":%u,\"measured_floor_ms\":%u,\"total_cap_ms\":%u",
				 MPP_CONTRACT, (unsigned)mpp_measured_play_ms(s),
				 MPP_TOTAL_CAP_MS);
		mpp_mark(s, "session", body);
		mpp_enter(s, MPP_PH_READY);
	}

	/* -- continuous latches: every frame, not per heartbeat (Astra B-5) ---- */
	if(s->markers_dropped) {
		mpp_fail(s, "marker queue overflow");
		return;
	}
	if(s->now_ms >= MPP_TOTAL_CAP_MS) {
		mpp_fail(s, "total timeout");
		return;
	}
	if(o->fault) {
		mpp_fail(s, "fault latch");
		return;
	}
	bool in_world = s->phase >= MPP_PH_LOGIN && s->phase <= MPP_PH_PAUSE_LEAVE;
	bool in_world2 = s->phase == MPP_PH_VERIFY || s->phase == MPP_PH_STAND;
	/* AN ABSENT BODY IS NOT A DEAD ONE (review B17 H3). The glue cannot read a
	 * health it has no body for -- the field stays 0 -- and the leave takes the
	 * body away in the same breath as the session. A real death shows the death
	 * screen, and a live body reports body_ticking; either one is required. */
	if((in_world || in_world2)
	   && ((o->body_ticking && o->health <= 0)
		   || o->screen == MPP_SCREEN_DEATH)) {
		mpp_fail(s, "died");
		return;
	}
	if(((in_world && s->phase != MPP_PH_PAUSE_LEAVE) || in_world2)
	   && !o->remote_session) {
		mpp_fail(s, "session lost");
		return;
	}
	/* The leave tears the local body down with the session (it then reads
	 * (0, -1.62, 0)): that is the END of the position stream, not a teleport.
	 * The latch is off from the moment the leave is pressed; VERIFY re-arms it
	 * on the rejoined body. */
	if((in_world && s->phase != MPP_PH_PAUSE_LEAVE) || in_world2) {
		if(s->have_last_pos) {
			float dx = o->x - s->last_x, dy = o->y - s->last_y,
				  dz = o->z - s->last_z;
			if(dx * dx + dy * dy + dz * dz
			   > MPP_TELEPORT_BLOCKS * MPP_TELEPORT_BLOCKS) {
				s->teleports++;
				/* removed from the walked path, as the collector does */
				s->disp_off_x += o->x - s->last_x;
				s->disp_off_z += o->z - s->last_z;
				snprintf(body, sizeof(body),
						 "\"n\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f",
						 s->teleports, (double)o->x, (double)o->y, (double)o->z);
				mpp_mark(s, "teleport", body);
			}
		}
		s->have_last_pos = true;
		s->last_x = o->x;
		s->last_y = o->y;
		s->last_z = o->z;
		if(s->gen == 1u && s->phase >= MPP_PH_SETTLE) {
			float wx = o->x - s->disp_off_x, wz = o->z - s->disp_off_z;
			if(!s->disp_origin) {
				s->disp_origin = true;
				s->disp_x0 = wx;
				s->disp_z0 = wz;
			} else {
				float ddx = wx - s->disp_x0, ddz = wz - s->disp_z0;
				if(ddx * ddx + ddz * ddz > s->disp_max2)
					s->disp_max2 = ddx * ddx + ddz * ddz;
			}
		}
		/* hw1030r SETTLE tracking (snapshot_pre waits on it) */
		/* field by field: struct mpp_item has padding a glue need not zero */
		bool inv_moved = !s->inv_prev_set;
		for(int k = 0; k < MPP_INV_SLOTS && !inv_moved; k++)
			inv_moved = s->inv_prev[k].item != o->inv[k].item
				|| s->inv_prev[k].count != o->inv[k].count
				|| s->inv_prev[k].damage != o->inv[k].damage;
		if(inv_moved) {
			s->inv_prev_set = true;
			for(int k = 0; k < MPP_INV_SLOTS; k++)
				s->inv_prev[k] = o->inv[k];
			s->inv_change_ms = s->now_ms;
		}
		if(s->dcell_armed) {
			const struct mpp_echo* de = mpp_echo_for_type(
				o, s->dcell_serial, s->dcell_x, s->dcell_y, s->dcell_z, 0);
			if(de) {
				s->dcell_serial = de->serial;
				s->dig_air_seen = true;
				s->dig_air_ms = s->now_ms;
			}
		}
	} else
		s->have_last_pos = false;
	if(s->measuring)
		s->measured_ms = s->now_ms - s->p2_t0_ms;

	switch(s->phase) {
		case MPP_PH_READY:
			if(o->remote_session && o->world_loaded && o->body_ticking
			   && o->screen == MPP_SCREEN_INGAME && o->install_gen == s->gen) {
				s->install_ms[s->gen] = s->now_ms;
				snprintf(body, sizeof(body),
						 "\"gen\":%u,\"timeout_admits\":%u,\"x\":%.2f,"
						 "\"y\":%.2f,\"z\":%.2f,\"dim\":%d,\"health\":%d",
						 s->gen, (unsigned)o->timeout_admits, (double)o->x,
						 (double)o->y, (double)o->z, o->dimension, o->health);
				mpp_mark(s, "ready", body);
				if(o->timeout_admits != 0) {
					mpp_fail(s, "load released by the 30 s backstop");
					return;
				}
				snprintf(body, sizeof(body), "\"gen\":%u", s->gen);
				mpp_mark(s, "login_begin", body);
				out->login_start = true;
				s->login_left = false;
				mpp_enter(s, MPP_PH_LOGIN);
			} else if(mpp_in_phase(s) >= MPP_READY_BUDGET_MS)
				mpp_fail(s, "world never became ready");
			break;

		case MPP_PH_LOGIN:
			if(o->login != MPP_LOGIN_OK)
				s->login_left = true;
			else if(!s->login_left) {
				mpp_fail(s, "stale login generation");
				return;
			}
			if(o->login == MPP_LOGIN_OK) {
				uint32_t since = s->now_ms - s->install_ms[s->gen];
				snprintf(body, sizeof(body),
						 "\"gen\":%u,\"since_connect_ms\":%u", s->gen,
						 (unsigned)since);
				mpp_mark(s, "login_ok", body);
				if(since >= MPP_LOGIN_DEADLINE_MS) {
					mpp_fail(s, "login past the server deadline");
					return;
				}
				mpp_enter(s, s->gen == 1 ? MPP_PH_SETTLE : MPP_PH_VERIFY);
			} else if(o->login == MPP_LOGIN_FAILED)
				mpp_fail(s, "login machine failed");
			else if(mpp_in_phase(s) >= MPP_LOGIN_BUDGET_MS)
				mpp_fail(s, "login timeout");
			break;

		case MPP_PH_SETTLE:
			if(mpp_in_phase(s) >= MPP_SETTLE_MS) {
				s->measuring = true;
				s->p2_t0_ms = s->now_ms;
				s->measured_ms = 0;
				snprintf(body, sizeof(body), "\"t0_ms\":%u",
						 (unsigned)s->now_ms);
				mpp_mark(s, "p2_begin", body);
				mpp_enter(s, MPP_PH_WALK);
			}
			break;

		case MPP_PH_WALK:
			out->walk = !s->walk_stuck_done;
			out->walk_heading_deg = s->heading_deg;
			if(!s->leg_started) {
				s->leg_started = true;
				s->leg_x = o->x;
				s->leg_z = o->z;
			}
			if(s->measured_ms >= mpp_measured_play_ms(s)) {
				s->measuring = false;
				snprintf(body, sizeof(body),
						 "\"measured_ms\":%u,\"cycles\":%u,\"digs\":%u,"
						 "\"picks\":%u,\"places\":%u,\"inv_moves\":%u,"
						 "\"build_skipped\":%u,\"teleports\":%u",
						 (unsigned)s->measured_ms, s->cycle, s->digs, s->picks,
						 s->places, s->inv_moves, s->build_skipped,
						 s->teleports);
				mpp_mark(s, "p2_end", body);
				if(!s->short_play_ms
				   && (s->digs < MPP_FLOOR_DIGS || s->places < MPP_FLOOR_PLACES
					   || s->inv_moves < MPP_FLOOR_INV_MOVES)) {
					mpp_fail(s, "action floors not met");
					return;
				}
				s->snap_done = false;
				mpp_enter(s, MPP_PH_PAUSE_REST);
			} else if(s->walk_stuck_done) {
				/* the walk ended as success: stand until measured play ends */
			} else {
				/* A leg that went nowhere (a pit, a shore, a wall) turns the
				 * NEXT leg by 90 degrees: displacement is a contract floor, and
				 * a fixed heading into an obstacle would never meet it. It does
				 * NOT build: see MPP_STUCK_CHECK_MS. */
				float dx = o->x - s->leg_x, dz = o->z - s->leg_z;
				float d2 = dx * dx + dz * dz;
				bool leg_over = mpp_in_phase(s)
					>= (s->short_leg ? MPP_SHORT_LEG_MS : MPP_WALK_LEG_MS);
				bool stuck = leg_over
					? d2 < MPP_LEG_MIN_PROGRESS * MPP_LEG_MIN_PROGRESS
					: (mpp_in_phase(s) >= MPP_STUCK_CHECK_MS
					   && d2 < MPP_STUCK_MIN_PROGRESS * MPP_STUCK_MIN_PROGRESS);
				if(stuck) {
					s->stuck_legs++;
					if(s->stuck_legs >= MPP_STUCK_LEGS_MAX) {
						if(mpp_walk_floors_met(s)) {
							mpp_walk_done_stuck(s, o);
							break;
						}
						if(s->esc_blocks < MPP_ESCAPE_BLOCKS_MAX) {
							mpp_escape_step_reset(s);
							mpp_enter(s, MPP_PH_ESCAPE);
							break;
						}
						mpp_fail(s, "walker made no progress on four headings "
									"in a row");
						return;
					}
					s->heading_deg = (s->heading_deg + 90) % 360;
					snprintf(body, sizeof(body),
							 "\"cycle\":%u,\"heading_deg\":%d,\"stuck\":%u",
							 s->cycle, s->heading_deg, s->stuck_legs);
					mpp_mark(s, "heading_turned", body);
					s->leg_started = false;
					mpp_enter(s, MPP_PH_WALK); /* a fresh leg, no build */
					break;
				}
				if(!leg_over)
					break;
				s->stuck_legs = 0;
				s->short_leg = false;
				s->esc_blocks = 0;
				s->esc_mines = 0;
				s->leg_started = false;
				s->rested = false;
				s->nudges = 0;
				s->nudge_until_ms = 0;
				s->nudge_next_ms = 0;
				s->face_since_ms = 0;
				mpp_enter(s, MPP_PH_BUILD_REST);
			}
			break;

		case MPP_PH_ESCAPE: {
			/* Looking straight down, no walking. Latch the rest height, hold
			 * JUMP, and press USE once the feet clear the cell under them;
			 * landing a block higher is the step. Any failure to gain height
			 * ends the run as a WALK failure with the stuck reason (the step
			 * name stays "walk"; the detail is in the escape_failed marker). */
			const char* why = NULL;
			bool held_ok = o->inv[o->held_hotbar].item > 0
				&& o->inv[o->held_hotbar].item <= 255;
			bool any = false;
			for(int k = 0; k < 9; k++)
				any |= o->inv[k].item > 0 && o->inv[k].item <= 255;
			out->aim = true;
			out->yaw_deg = 0;
			out->pitch_deg = 90;
			if(mpp_in_phase(s) >= MPP_ESCAPE_STEP_MS)
				why = s->esc_mining ? "wall dig never echoed"
					: !s->esc_based ? "never centred in one column"
									: "no height gained";
			else if(!s->esc_based) {
				/* ONE COLUMN FIRST (real Wii hw1023c mp_play_10m). That body
				 * rested at z -802.16 -- on (57,78,-802), but straight down
				 * from its centre is column -803, two lower: the crosshair met
				 * no block inside the 4.5 pick reach and nine jumps placed
				 * nothing. Centre in floor(x), floor(z) with BUILD_REST's
				 * taps (a body that drops into a lower column on the way just
				 * latches its base there), then latch the base. */
				bool centred = mpp_centred(o);
				if(!centred)
					out->yaw_deg = mpp_centre_yaw(o);
				if(s->now_ms < s->nudge_until_ms)
					out->actions |= MPP_ACT_FORWARD; /* the tap in progress */
				else if(o->at_rest && s->now_ms >= s->nudge_next_ms) {
					if(centred) {
						s->esc_based = true;
						s->esc_base_y = o->y;
						if(s->nudges) {
							snprintf(body, sizeof(body),
									 "\"nudges\":%u,\"x\":%.2f,\"y\":%.2f,"
									 "\"z\":%.2f",
									 s->nudges, (double)o->x, (double)o->y,
									 (double)o->z);
							mpp_mark(s, "escape_centred", body);
							s->phase_ms = s->now_ms; /* the jumps get a full
													  * step clock */
						}
					} else if(s->nudges >= MPP_NUDGE_MAX)
						why = "never centred in one column";
					else
						why = mpp_centre_tap(s, o, out);
				}
			} else if(o->at_rest && o->y >= s->esc_base_y + MPP_ESCAPE_RISE) {
				s->esc_blocks++;
				snprintf(body, sizeof(body),
						 "\"n\":%u,\"y\":%.2f,\"tries\":%u,\"mines\":%u",
						 s->esc_blocks, (double)o->y, s->esc_tries + 1u,
						 s->esc_mines);
				mpp_mark(s, "escape_step", body);
				mpp_escape_step_reset(s);
				s->stuck_legs = MPP_STUCK_LEGS_MAX - 1u; /* one more stuck leg
														  * = the next step */
				s->leg_started = false;
				mpp_enter(s, MPP_PH_WALK);
				break;
			} else if(s->esc_mining) {
				/* DIG THE WALL CELL for a block. Held on the latched cell,
				 * credited only on the server's own "air" echo for it. */
				out->yaw_deg = s->esc_mine_yaw;
				out->pitch_deg = MPP_ESCAPE_MINE_PITCH;
				out->watch = true;
				out->watch_x = s->esc_mx;
				out->watch_y = s->esc_my;
				out->watch_z = s->esc_mz;
				if(o->target_valid && o->tx == s->esc_mx && o->ty == s->esc_my
				   && o->tz == s->esc_mz)
					out->actions |= MPP_ACT_ATTACK; /* a dig is a hold */
				const struct mpp_echo* e = mpp_echo_for(
					o, s->echo_seen, s->esc_mx, s->esc_my, s->esc_mz);
				if(e && e->type == 0) {
					s->esc_mines++;
					s->esc_mining = false;
					snprintf(body, sizeof(body),
							 "\"n\":%u,\"x\":%d,\"y\":%d,\"z\":%d,"
							 "\"old_id\":%u",
							 s->esc_mines, (int)s->esc_mx, (int)s->esc_my,
							 (int)s->esc_mz, (unsigned)s->esc_mine_old);
					mpp_mark(s, "escape_mine", body);
					/* the drop has to arrive, then be placed: a fresh step
					 * clock for that (bounded by MPP_ESCAPE_BLOCKS_MAX digs) */
					s->esc_mine_wait_ms = s->now_ms + MPP_PICKUP_BUDGET_MS;
					s->phase_ms = s->now_ms;
				} else if(s->now_ms - s->esc_mine_t0_ms >= MPP_DIG_BUDGET_MS)
					why = "wall dig never echoed";
			} else {
				/* A landing without the rise ends one attempt (the server
				 * refused the place, or the press missed): jump again. */
				if(o->at_rest && s->esc_air) {
					snprintf(body, sizeof(body),
							 "\"try\":%u,\"used\":%d,\"seen\":%d,"
							 "\"tv\":%d,\"tx\":%d,\"ty\":%d,\"tz\":%d,"
							 "\"want\":[%d,%d,%d]",
							 s->esc_tries + 1u, s->esc_used ? 1 : 0,
							 s->esc_t_seen ? 1 : 0, s->esc_t_valid ? 1 : 0,
							 (int)s->esc_t_x, (int)s->esc_t_y, (int)s->esc_t_z,
							 (int)mpp_floor(o->x),
							 (int)mpp_floor(s->esc_base_y) - 1,
							 (int)mpp_floor(o->z));
					mpp_mark(s, "escape_try", body);
					s->esc_tries++;
					mpp_escape_attempt_reset(s);
				}
				if(!o->at_rest)
					s->esc_air = true;
				if(!held_ok && any) {
					if(o->at_rest)
						(void)mpp_press(s, out, MPP_ACT_HOTBAR_NEXT);
				} else if(!held_ok) {
					/* No block at all: find a hand-diggable wall cell next to
					 * the body (feet or head level), one cardinal at a time,
					 * once the last dig's drop has had its time to arrive. */
					if(!o->at_rest || s->now_ms < s->esc_mine_wait_ms)
						; /* wait */
					else if(s->esc_mines >= MPP_ESCAPE_BLOCKS_MAX)
						why = "no placeable block in the hotbar";
					else {
						if(!s->esc_scan_on) {
							s->esc_scan_on = true;
							s->esc_scan_t0_ms = s->now_ms;
						}
						uint32_t dir
							= (s->now_ms - s->esc_scan_t0_ms) / MPP_SCAN_DIR_MS;
						int32_t fx = mpp_floor(o->x), fy = mpp_floor(o->y),
								fz = mpp_floor(o->z);
						int32_t adx = o->tx > fx ? o->tx - fx : fx - o->tx;
						int32_t adz = o->tz > fz ? o->tz - fz : fz - o->tz;
						if(dir >= 4u)
							why = "no placeable block and no hand-diggable wall";
						else if(o->target_valid && adx + adz == 1
								&& (o->ty == fy || o->ty == fy + 1)
								&& mpp_hand_diggable(o->target_type)
								&& (s->now_ms - s->esc_scan_t0_ms)
										% MPP_SCAN_DIR_MS
									>= MPP_ESCAPE_AIM_SETTLE_MS) {
							s->esc_mining = true;
							s->esc_scan_on = false;
							s->esc_mx = o->tx;
							s->esc_my = o->ty;
							s->esc_mz = o->tz;
							s->esc_mine_old = o->target_type;
							s->esc_mine_t0_ms = s->now_ms;
							s->echo_seen = o->echo_serial;
						}
						if(!s->esc_mining && !why) {
							s->esc_mine_yaw
								= (s->heading_deg + 90 * (int)dir) % 360;
						}
						out->yaw_deg = s->esc_mine_yaw;
						out->pitch_deg = MPP_ESCAPE_MINE_PITCH;
					}
				} else if(!s->esc_above_ms
						  && o->y < s->esc_base_y + MPP_ESCAPE_PLACE_RISE)
					out->actions |= MPP_ACT_JUMP; /* released before USE */
				else {
					bool above = o->y >= s->esc_base_y + MPP_ESCAPE_PLACE_RISE;
					/* USE ONLY ON THE CELL UNDER THE FEET (hw1023c): a press
					 * with any other target -- or none, out of reach -- places
					 * nothing useful, and screen_ingame sends nothing at all
					 * without a hit. */
					bool on_feet = o->target_valid
						&& o->tx == mpp_floor(o->x)
						&& o->ty == mpp_floor(s->esc_base_y) - 1
						&& o->tz == mpp_floor(o->z);
					if(above) {
						s->esc_t_seen = true;
						s->esc_t_valid = o->target_valid;
						s->esc_t_x = o->tx;
						s->esc_t_y = o->ty;
						s->esc_t_z = o->tz;
					}
					if(!s->esc_above_ms)
						s->esc_above_ms = s->now_ms ? s->now_ms : 1u;
					if(!s->esc_gap)
						s->esc_gap = true; /* one neutral frame: a fresh edge */
					else if(s->esc_used) {
						if(s->now_ms < s->press_until_ms)
							out->actions |= s->press_act;
					} else if(above && on_feet
							  && s->now_ms - s->esc_above_ms
								  >= (s->esc_tries % 3u)
									  * MPP_ESCAPE_USE_STAGGER_MS) {
						if(mpp_press(s, out, MPP_ACT_USE))
							s->esc_used = true;
					}
				}
			}
			if(why) {
				snprintf(body, sizeof(body),
						 "\"cycle\":%u,\"blocks\":%u,\"tries\":%u,"
						 "\"mines\":%u,\"why\":\"%s\"",
						 s->cycle, s->esc_blocks, s->esc_tries, s->esc_mines,
						 why);
				mpp_mark(s, "escape_failed", body);
				if(mpp_walk_floors_met(s)) {
					mpp_walk_done_stuck(s, o);
					break;
				}
				s->phase = MPP_PH_WALK;
				mpp_fail(s, "walker made no progress on four headings in a "
							"row");
				return;
			}
			break;
		}

		case MPP_PH_BUILD_REST: {
			/* Inside ONE column and at exact rest, or the body straddles two
			 * columns and cannot fall into the dug cell (split_recovery's
			 * measured lesson). Two honest sub-steps with their OWN budgets and
			 * their own skip reasons: (1) come to rest; (2) nudge into the
			 * column with single-tick FORWARD taps, each issued only while the
			 * OBSERVED facing vector points at the column centre. */
			s->under_type = o->under_feet_type;
			bool centred = mpp_centred(o);
			int want_yaw = mpp_centre_yaw(o);
			out->aim = true;
			out->yaw_deg = centred ? 0 : want_yaw;
			out->pitch_deg = 90;
			if(o->in_water) {
				if(mpp_in_phase(s) >= 1000u)
					mpp_skip_build(s, "in water");
				break;
			}
			if(s->now_ms < s->nudge_until_ms) {
				out->actions |= MPP_ACT_FORWARD; /* the tap in progress */
				break;
			}
			if(!o->at_rest || s->now_ms < s->nudge_next_ms) {
				if(!s->rested && mpp_in_phase(s) >= MPP_REST_BUDGET_MS)
					mpp_skip_build(s, "never came to rest");
				else if(s->rested
						&& s->now_ms - s->rest_ms >= MPP_CENTRE_BUDGET_MS)
					mpp_skip_build(s, "never centred in one column");
				break;
			}
			if(!s->rested) {
				s->rested = true;
				s->rest_ms = s->now_ms;
			}
			if(centred) {
				if(!mpp_hand_diggable(o->under_feet_type)) {
					mpp_skip_build(s,
								   "block under the feet is not hand-diggable");
					break;
				}
				/* Never the same column twice running, however the body came
				 * to be here: a second dig in it is a SHAFT, not a build. */
				if(s->have_dig && mpp_floor(o->x) == s->dig_x
				   && mpp_floor(o->z) == s->dig_z) {
					mpp_skip_build(s, "still in the column of the last dig");
					break;
				}
				s->survey_dir = 0;
				s->level_mask = 0;
				s->aim_dir_t0_ms = s->now_ms;
				mpp_enter(s, MPP_PH_BUILD_SURVEY);
				break;
			}
			if(s->nudges >= MPP_NUDGE_MAX
			   || s->now_ms - s->rest_ms >= MPP_CENTRE_BUDGET_MS) {
				mpp_skip_build(s, "never centred in one column");
				break;
			}
			/* Face the centre first -- judged on the OBSERVED vector. */
			const char* why_c = mpp_centre_tap(s, o, out);
			if(why_c)
				mpp_skip_build(s, why_c);
			break;
		}

		case MPP_PH_BUILD_SURVEY: {
			static const int mpp_sdx[4] = {0, -1, 0, 1};
			static const int mpp_sdz[4] = {1, 0, -1, 0};
			unsigned d = s->survey_dir;
			int32_t fy = mpp_floor(o->y);
			out->aim = true;
			out->yaw_deg = (int)d * 90;
			out->pitch_deg = MPP_SURVEY_PITCH;
			bool on_dir = o->target_valid
				&& o->tx - mpp_floor(o->x) == mpp_sdx[d]
				&& o->tz - mpp_floor(o->z) == mpp_sdz[d];
			bool settled = s->now_ms - s->aim_dir_t0_ms >= 400u;
			bool level = settled && on_dir
				&& ((o->tface == MPP_FACE_TOP && o->ty == fy - 1
					 && o->target_top_clear)
					|| (o->ty == fy && mpp_is_plant(o->target_type)));
			if(level)
				s->level_mask |= 1u << d;
			if(!level && s->now_ms - s->aim_dir_t0_ms < MPP_SCAN_DIR_MS)
				break;
			s->survey_dir++;
			s->aim_dir_t0_ms = s->now_ms;
			if(s->survey_dir < 4u)
				break;
			if(mpp_bits4(s->level_mask) < MPP_SURVEY_MIN_LEVEL) {
				mpp_skip_build(s, "ground not level enough to dig safely");
				break;
			}
			if(!mpp_landing_support_matches(o, mpp_floor(o->x), fy - 1,
												  mpp_floor(o->z))) {
				mpp_skip_build(s, "unsafe one-block landing");
				break;
			}
			s->dig_x = mpp_floor(o->x);
			s->dig_y = mpp_floor(o->y) - 1;
			s->dig_z = mpp_floor(o->z);
			s->dig_old = o->under_feet_type;
			s->echo_seen = o->echo_serial;
			s->wclr_seen = o->watch_clears;
			s->dig_on_frames = 0;
			s->dig_on_seen = false;
			s->dig_prev_frame_ms = s->now_ms;
			s->dig_on_gap_max_ms = s->dig_frame_gap_max_ms = 0;
			s->dcell_armed = true;
			s->dcell_x = s->dig_x;
			s->dcell_y = s->dig_y;
			s->dcell_z = s->dig_z;
			s->dcell_serial = s->echo_seen;
			mpp_enter(s, MPP_PH_BUILD_DIG);
			break;
		}

		case MPP_PH_BUILD_DIG: {
			/* Re-read the support for THIS intended dig, before even arming
			 * ATTACK. A changed/unknown column cannot borrow the prior survey. */
			if(!mpp_landing_support_matches(o, s->dig_x, s->dig_y,
												 s->dig_z)) {
				mpp_skip_build(s, "unsafe one-block landing");
				break;
			}
			out->aim = true;
			out->yaw_deg = 0;
			out->pitch_deg = 90;
			out->watch = true;
			out->watch_x = s->dig_x;
			out->watch_y = s->dig_y;
			out->watch_z = s->dig_z;
			bool on_target = o->target_valid && o->tx == s->dig_x
				&& o->ty == s->dig_y && o->tz == s->dig_z;
			if(s->now_ms - s->dig_prev_frame_ms > s->dig_frame_gap_max_ms)
				s->dig_frame_gap_max_ms = s->now_ms - s->dig_prev_frame_ms;
			s->dig_prev_frame_ms = s->now_ms;
			if(on_target) {
				out->actions |= MPP_ACT_ATTACK; /* held: a dig is a hold */
				s->dig_on_frames++;
				if(s->dig_on_seen
				   && s->now_ms - s->dig_last_on_ms > s->dig_on_gap_max_ms)
					s->dig_on_gap_max_ms = s->now_ms - s->dig_last_on_ms;
				/* THE HOLD MUST BE CONTINUOUS (review B21 M2): a gap longer
				 * than MPP_DIG_HOLD_GAP_MS off the cell starts it over. The
				 * echo itself arrives a frame AFTER the block is gone -- when
				 * the crosshair no longer rests on the cell -- so the hold is
				 * measured up to the last on-cell frame, not up to now. */
				if(!s->dig_on_seen
				   || s->now_ms - s->dig_last_on_ms > MPP_DIG_HOLD_GAP_MS) {
					s->dig_on_seen = true;
					s->dig_on_t0_ms = s->now_ms;
				}
				s->dig_last_on_ms = s->now_ms;
			}
			/* hw1030g: ANY air echo for the cell since the baseline is the
			 * server's dig receipt -- a later record for the same cell (water
			 * flowing in, a region resend) must not mask it. */
			const struct mpp_echo* e = mpp_echo_for_type(
				o, s->echo_seen, s->dig_x, s->dig_y, s->dig_z, 0);
			if(e && e->type == 0
			   && (!s->dig_on_seen
				   || s->dig_last_on_ms - s->dig_on_t0_ms < MPP_DIG_MIN_HOLD_MS
				   || s->now_ms - s->dig_last_on_ms > MPP_DIG_HOLD_GAP_MS)) {
				/* the cell is air and this script barely touched it. hw1030r:
				 * the hold's shape rides on the marker (core-clock ms), so a
				 * real dig judged "barely touched" on the Wii shows why. The
				 * thresholds are unchanged. */
				char hold[192];
				snprintf(hold, sizeof(hold),
						 ",\"dig_on_seen\":%d,\"dig_on_t0\":%u,"
						 "\"dig_last_on\":%u,\"now\":%u,\"dig_on_frames\":%u,"
						 "\"on_gap_max_ms\":%u,\"frame_gap_max_ms\":%u",
						 s->dig_on_seen ? 1 : 0, (unsigned)s->dig_on_t0_ms,
						 (unsigned)s->dig_last_on_ms, (unsigned)s->now_ms,
						 s->dig_on_frames, (unsigned)s->dig_on_gap_max_ms,
						 (unsigned)s->dig_frame_gap_max_ms);
				mpp_skip_build_x(s, "dig cell changed without a dig", hold);
			} else if(e && e->type == 0) {
				s->digs++;
				s->dig_thirds |= 1u << mpp_play_third(s);
				s->have_dig = true; /* only a CREDITED dig names a column */
				snprintf(body, sizeof(body),
						 "\"n\":%u,\"x\":%d,\"y\":%d,\"z\":%d,\"old_id\":%u,"
						 "\"measured_ms\":%u",
						 s->digs, (int)s->dig_x, (int)s->dig_y, (int)s->dig_z,
						 (unsigned)s->dig_old, (unsigned)s->measured_ms);
				mpp_mark(s, "dig", body);
				s->pick_item = 0;
				for(int k = 0; k < MPP_INV_SLOTS; k++)
					s->pick_inv[k] = o->inv[k];
				mpp_enter(s, MPP_PH_BUILD_PICKUP);
			} else if(o->in_water && s->dig_on_frames < 30u) {
				/* Drifted into a fluid before any real digging happened. */
				mpp_skip_build(s, "in water");
			} else if(mpp_in_phase(s) >= MPP_DIG_BUDGET_MS) {
				/* Two different stories. If ATTACK was really held on the cell
				 * (>= ~0.5 s of frames) and the server never echoed the break,
				 * client and server DISAGREE -- a defect, the run fails. If the
				 * crosshair was (almost) never on the cell, nothing was
				 * attempted: the body moved off its column (a slope, a current,
				 * a push) -- terrain, so the leg is skipped by name. */
				/* A BURST IS NOT A MISSING ECHO: more than MPP_ECHO_RING echoes
				 * since the baseline matters only when the wait has RUN OUT and
				 * there is no watch slot -- then "my echo is not in the ring"
				 * proves nothing. Judged here, at the end, never on the burst
				 * itself (PC run 194515 skipped a leg on a burst that arrived
				 * before the dig had even completed). */
				if(!o->watch_supported
				   && o->echo_serial - s->echo_seen > MPP_ECHO_RING)
					mpp_skip_build(s, "server echoes overflowed the ring");
				else if(s->dig_on_frames >= 30u)
					mpp_fail_echo(s, o, "dig never echoed by the server",
								  s->dig_x, s->dig_y, s->dig_z);
				else
					mpp_skip_build(s, "crosshair never on the dig cell");
			}
			break;
		}

		case MPP_PH_BUILD_PICKUP: {
			int gained = -1;
			for(int k = 0; k < MPP_INV_SLOTS && gained < 0; k++)
				if(o->inv[k].item > 0
				   && (o->inv[k].item != s->pick_inv[k].item
					   || o->inv[k].count > s->pick_inv[k].count))
					gained = k;
			if(gained >= 0) {
				s->picks++;
				s->pick_item = o->inv[gained].item;
				snprintf(body, sizeof(body),
						 "\"n\":%u,\"slot\":%d,\"item\":%d,\"count\":%u",
						 s->picks, gained, (int)o->inv[gained].item,
						 (unsigned)o->inv[gained].count);
				mpp_mark(s, "pickup", body);
				s->place_dir = 0;
				mpp_after_pickup(s);
			} else if(mpp_in_phase(s) >= MPP_PICKUP_BUDGET_MS) {
				/* On a REMOTE server the drop is the server's physics, not
				 * ours (it hops 0.2 up and up to 0.1 sideways per tick): the
				 * own-column dig makes a pickup likely, not certain -- one real
				 * run lost 1 of 4 with the block provably broken under the
				 * feet. A single miss is narrated and the cycle goes on (the
				 * reserve rule simply places nothing); a run that loses MOST of
				 * its drops has a collect-path defect and fails by name. */
				s->pickups_missed++;
				snprintf(body, sizeof(body),
						 "\"n\":%u,\"x\":%d,\"y\":%d,\"z\":%d,\"measured_ms\":%u",
						 s->pickups_missed, (int)s->dig_x, (int)s->dig_y,
						 (int)s->dig_z, (unsigned)s->measured_ms);
				mpp_mark(s, "pickup_missed", body);
				if(s->pickups_missed >= 3u && s->pickups_missed * 2u > s->digs) {
					mpp_fail(s, "most dug items never reached the inventory");
					return;
				}
				mpp_after_pickup(s);
			}
			break;
		}

		case MPP_PH_BUILD_AIM: {
			/* Keep a RESERVE of one block. A fresh player who places everything
			 * it digs ends measured play with an EMPTY inventory, and "empty ==
			 * empty" across the rejoin would prove nothing about the server
			 * keeping the inventory (Astra B-6 wants exact item / count /
			 * damage). So a place only happens while at least two placeable
			 * blocks are held; the first cycle therefore keeps its block. */
			unsigned placeable = 0;
			for(int k = 0; k < MPP_INV_SLOTS; k++)
				if(o->inv[k].item > 0 && o->inv[k].item <= 255)
					placeable += o->inv[k].count;
			if(placeable < 2u) {
				if(s->places_this_cycle == 0u) {
					snprintf(body, sizeof(body), "\"cycle\":%u,\"held\":%u",
							 s->cycle, placeable);
					mpp_mark(s, "reserve_kept", body);
				}
				mpp_cycle_done(s);
				break;
			}
			/* Need a PLACEABLE block in the held hotbar slot first. */
			if(o->inv[o->held_hotbar].item <= 0
			   || o->inv[o->held_hotbar].item > 255) {
				bool any = false;
				for(int k = 0; k < 9; k++)
					any |= o->inv[k].item > 0 && o->inv[k].item <= 255;
				if(!any) {
					mpp_skip_build(s, "no placeable block in the hotbar");
					break;
				}
				(void)mpp_press(s, out, MPP_ACT_HOTBAR_NEXT);
				if(mpp_in_phase(s) >= MPP_PLACE_AIM_BUDGET_MS)
					mpp_skip_build(s, "hotbar never reached a block");
				break;
			}
			/* The place geometry only exists once the body has LANDED in the
			 * cell it dug (feet one below the surface, at rest): the first full
			 * run burned the whole aim budget in 4 of 13 cycles by sweeping
			 * while the body was still dropping. The scan starts here, not at
			 * phase entry. */
			if(!s->aim_ready) {
				if(o->at_rest && mpp_floor(o->y) == s->dig_y
				   && mpp_floor(o->x) == s->dig_x
				   && mpp_floor(o->z) == s->dig_z) {
					s->aim_ready = true;
					s->aim_t0_ms = s->now_ms;
					s->aim_dir_t0_ms = s->now_ms;
					s->place_dir = 0;
					s->clearing = false;
					s->place_retried = false;
					s->scan_done = false;
					s->scan_dir = 0;
					s->usable_mask = 0;
					s->places_allowed = 0;
				} else if(mpp_in_phase(s) >= MPP_DROP_BUDGET_MS
							  + MPP_PLACE_AIM_BUDGET_MS) {
					mpp_skip_build(s, "body never landed in the dug cell");
				}
				out->aim = true;
				out->yaw_deg = 0;
				out->pitch_deg = 35;
				break;
			}
			/* The core's yaw convention: 0 = +Z, 90 = -X, 180 = -Z, 270 = +X.
			 * The crosshair has to be on THE neighbour of the cardinal asked
			 * for, not on any neighbour: the scan's answer is only worth
			 * something if each bit means the cardinal it is filed under. */
			static const int mpp_dxs[4] = {0, -1, 0, 1};
			static const int mpp_dzs[4] = {1, 0, -1, 0};
			unsigned dir = s->scan_done ? (unsigned)s->place_dir : s->scan_dir;
			out->aim = true;
			out->yaw_deg = (int)dir * 90;
			out->pitch_deg = 35;
			bool on_dir = o->target_valid
				&& o->tx - mpp_floor(o->x) == mpp_dxs[dir]
				&& o->tz - mpp_floor(o->z) == mpp_dzs[dir];
			bool settled = s->now_ms - s->aim_dir_t0_ms >= 400u;
			bool usable = settled && on_dir && o->tface == MPP_FACE_TOP
				&& o->ty == mpp_floor(o->y) && o->target_top_clear;
			if(!s->scan_done) {
				/* PASS 1 -- READ ALL FOUR, PLACE ON NONE. */
				bool next = false;
				if(s->clearing) {
					/* Hold ATTACK on the plant until the SERVER says it is
					 * gone; then read this cardinal again. */
					out->watch = true;
					out->watch_x = s->clear_x;
					out->watch_y = s->clear_y;
					out->watch_z = s->clear_z;
					bool on_plant = o->target_valid && o->tx == s->clear_x
						&& o->ty == s->clear_y && o->tz == s->clear_z;
					const struct mpp_echo* ce = mpp_echo_for(
						o, s->clear_echo, s->clear_x, s->clear_y, s->clear_z);
					if(ce && ce->type == 0) {
						s->plants_cleared++;
						snprintf(body, sizeof(body),
								 "\"n\":%u,\"x\":%d,\"y\":%d,\"z\":%d",
								 s->plants_cleared, (int)s->clear_x,
								 (int)s->clear_y, (int)s->clear_z);
						mpp_mark(s, "plant_cleared", body);
						s->clearing = false;
						s->aim_dir_t0_ms = s->now_ms; /* re-settle, re-read */
					} else if(s->now_ms - s->clear_t0_ms
							  >= MPP_CLEAR_BUDGET_MS) {
						s->clearing = false; /* give this cardinal up */
						next = true;
					} else if(on_plant)
						out->actions |= MPP_ACT_ATTACK;
				} else if(settled && on_dir && o->ty == mpp_floor(o->y) + 1
						  && mpp_is_plant(o->target_type)) {
					s->clearing = true;
					s->clear_x = o->tx;
					s->clear_y = o->ty;
					s->clear_z = o->tz;
					s->clear_t0_ms = s->now_ms;
					s->clear_echo = o->echo_serial;
				} else if(usable) {
					s->usable_mask |= 1u << s->scan_dir;
					next = true;
				} else if(s->now_ms - s->aim_dir_t0_ms >= MPP_SCAN_DIR_MS)
					next = true;
				if(!next)
					break;
				s->scan_dir++;
				s->aim_dir_t0_ms = s->now_ms;
				if(s->scan_dir < 4u)
					break;
				/* the scan is complete: how many blocks may this hole take? */
				s->scan_done = true;
				unsigned n = mpp_bits4(s->usable_mask);
				s->places_allowed = n ? n - 1u : 0u;
				if(s->places_allowed > MPP_PLACES_PER_CYCLE)
					s->places_allowed = MPP_PLACES_PER_CYCLE;
				if(n == 0u)
					mpp_skip_build(s, "no usable top face on any cardinal");
				else if(s->places_allowed == 0u)
					mpp_skip_build(s, "placing would wall the only way out");
				else
					s->place_dir = mpp_pick_place_dir(s->usable_mask,
													  s->heading_deg);
				break;
			}
			/* PASS 2 -- place on the chosen cardinal. */
			if(usable) {
				/* The clicked cell is the neighbour AT the feet level (the
				 * body stands one lower than the surface after the dig, so
				 * that neighbour's top face is at eye-minus-0.62). */
				s->place_x = o->tx;
				s->place_y = o->ty + 1;
				s->place_z = o->tz;
				s->echo_seen = o->echo_serial;
				s->wclr_seen = o->watch_clears;
				s->place_item = o->inv[o->held_hotbar].item;
				mpp_enter(s, MPP_PH_BUILD_PLACE);
				break;
			}
			if(s->now_ms - s->aim_dir_t0_ms >= MPP_AIM_DIR_MS) {
				/* usable a second ago and not now: drop it, try another --
				 * still keeping one */
				s->usable_mask &= ~(1u << (unsigned)s->place_dir);
				int nd = mpp_pick_place_dir(s->usable_mask, s->heading_deg);
				if(nd >= 0) {
					s->place_dir = nd;
					s->aim_dir_t0_ms = s->now_ms;
				} else if(s->places_this_cycle > 0u)
					mpp_cycle_done(s); /* the catch-up place is optional */
				else
					mpp_skip_build(s, "no usable top face on any cardinal");
			}
			break;
		}

		case MPP_PH_BUILD_PLACE: {
			out->aim = true;
			out->yaw_deg = s->place_dir * 90;
			out->pitch_deg = 35;
			out->watch = true;
			out->watch_x = s->place_x;
			out->watch_y = s->place_y;
			out->watch_z = s->place_z;
			mpp_press_once(s, out, MPP_ACT_USE);
			const struct mpp_echo* e = mpp_echo_for(
				o, s->echo_seen, s->place_x, s->place_y, s->place_z);
			/* OURS means the block that was in the hand (review B17 M2): water
			 * or anything else arriving in that cell is not a place. */
			e = mpp_echo_for_type(o, s->echo_seen, s->place_x, s->place_y,
								  s->place_z, s->place_item);
			if(e) {
				s->place_retried = false;
				s->places++;
				s->place_thirds |= 1u << mpp_play_third(s);
				snprintf(body, sizeof(body),
						 "\"n\":%u,\"x\":%d,\"y\":%d,\"z\":%d,\"id\":%u,"
						 "\"measured_ms\":%u",
						 s->places, (int)s->place_x, (int)s->place_y,
						 (int)s->place_z, (unsigned)e->type,
						 (unsigned)s->measured_ms);
				mpp_mark(s, "place", body);
				s->places_this_cycle++;
				s->usable_mask &= ~(1u << (unsigned)s->place_dir); /* walled */
				/* Catch up ONE missed place per cycle: the reserve keeps places
				 * one behind digs on purpose; further behind means an earlier
				 * place leg was skipped, and the blocks are in the inventory.
				 * Only while the scan still leaves a way out (places_allowed). */
				int nd = mpp_pick_place_dir(s->usable_mask, s->heading_deg);
				if(s->places + 1u < s->digs
				   && s->places_this_cycle < s->places_allowed && nd >= 0) {
					s->place_dir = nd;
					s->aim_dir_t0_ms = s->now_ms;
					mpp_enter(s, MPP_PH_BUILD_AIM); /* scan kept, body landed */
				} else
					mpp_cycle_done(s);
			} else if(mpp_in_phase(s) >= MPP_PLACE_BUDGET_MS) {
				if(!o->watch_supported
				   && o->echo_serial - s->echo_seen > MPP_ECHO_RING) {
					/* judged at the END of the wait only -- see BUILD_DIG */
					s->place_retried = false;
					mpp_skip_build(s, "server echoes overflowed the ring");
				} else if(!s->place_retried) {
					/* read the face again, press once more */
					s->place_retried = true;
					s->aim_dir_t0_ms = s->now_ms;
					mpp_enter(s, MPP_PH_BUILD_AIM);
				} else {
					s->place_retried = false;
					if(++s->unechoed_legs >= MPP_UNECHOED_LEGS_MAX)
						mpp_fail_echo(s, o, "place never echoed by the server",
									  s->place_x, s->place_y, s->place_z);
					else
						mpp_skip_build(s, "place not echoed by the server");
				}
			}
			break;
		}

		case MPP_PH_INV_OPEN:
			if(o->screen == MPP_SCREEN_INVENTORY) {
				s->inv_src = mpp_find_stack(o);
				s->inv_dst = mpp_find_empty(o, s->inv_src);
				/* REL-14b: after the rejoin this leg IS the post-reconnect
				 * action; skipping it would pass a reconnect nobody acted in. */
				if(s->gen == 2u && (s->inv_src < 0 || s->inv_dst < 0)) {
					mpp_fail(s, "no post-reconnect action");
					return;
				}
				if(s->inv_src < 0) {
					/* Nothing collected yet (skipped builds): not a defect by
					 * itself -- the floors at p2_end decide. */
					mpp_mark(s, "inv_skipped", "\"why\":\"nothing to move\"");
					mpp_enter(s, MPP_PH_INV_CLOSE);
					break;
				}
				if(s->inv_dst < 0) {
					bool any_empty = false;
					for(int k = 0; k < MPP_INV_SLOTS; k++)
						any_empty |= o->inv[k].item <= 0;
					mpp_mark(s, "inv_skipped",
							 any_empty ? "\"why\":\"no safe destination slot\""
									   : "\"why\":\"nothing to move\"");
					mpp_enter(s, MPP_PH_INV_CLOSE);
					break;
				}
				s->inv_moving = o->inv[s->inv_src];
				mpp_enter(s, MPP_PH_INV_SRC);
			} else {
				(void)mpp_press(s, out, MPP_ACT_OPEN_INVENTORY);
				if(mpp_in_phase(s) >= MPP_INV_STEP_BUDGET_MS)
					mpp_fail(s, "inventory never opened");
			}
			break;

		case MPP_PH_INV_SRC:
		case MPP_PH_INV_DST: {
			int want = s->phase == MPP_PH_INV_SRC ? s->inv_src : s->inv_dst;
			if(o->screen != MPP_SCREEN_INVENTORY) {
				mpp_fail(s, "inventory closed under the script");
				return;
			}
			if(o->inv_focus_slot == want && !mpp_pressing(s)) {
				s->txn_seen = o->txn_serial;
				s->click_base = o->last_click_action_id;
				if(s->phase == MPP_PH_INV_SRC)
					s->have_pick_id = false;
				else
					s->have_put_id = false;
				mpp_enter(s, s->phase == MPP_PH_INV_SRC ? MPP_PH_INV_PICK
														: MPP_PH_INV_PUT);
			} else if(o->inv_focus_slot != want) {
				/* Window-0 main grid is 9 wide: rows 9..35 on top, hotbar 0..8
				 * below. Column first, then row; the impure half reports focus
				 * in the same inv[] indexing. */
				int fc = o->inv_focus_slot < 0 ? 0 : o->inv_focus_slot % 9;
				int wc = want % 9;
				int fr = o->inv_focus_slot < 0 ? 0
					: o->inv_focus_slot < 9	   ? 3
											   : o->inv_focus_slot / 9 - 1;
				int wr = want < 9 ? 3 : want / 9 - 1;
				unsigned act = fc < wc ? MPP_ACT_GUI_RIGHT
					: fc > wc		   ? MPP_ACT_GUI_LEFT
					: fr < wr		   ? MPP_ACT_GUI_DOWN
									   : MPP_ACT_GUI_UP;
				(void)mpp_press(s, out, act);
			}
			if((s->phase == MPP_PH_INV_SRC || s->phase == MPP_PH_INV_DST)
			   && mpp_in_phase(s) >= MPP_INV_STEP_BUDGET_MS * 3u)
				mpp_fail(s, "focus never reached the slot");
			break;
		}

		case MPP_PH_INV_PICK:
			mpp_press_once(s, out, MPP_ACT_GUI_CLICK);
			/* The id of the click THIS press produced: the client counter has
			 * moved off the baseline latched when the phase was entered. */
			if(!s->have_pick_id && o->last_click_action_id != s->click_base) {
				s->inv_action_pick = o->last_click_action_id;
				s->have_pick_id = true;
			}
			if(s->have_pick_id) {
				const struct mpp_txn* t
					= mpp_txn_for(o, s->txn_seen, s->inv_action_pick);
				if(t && !t->accepted) {
					mpp_fail(s, "server rejected the pick-up click");
					return;
				}
				/* The accepted transaction IS the server's word that the click
				 * happened. What the cursor holds is what moves: a pickup that
				 * merged into the source stack a tick before the click makes it
				 * MORE than planned, and one that lands in the emptied source
				 * slot a tick after makes that slot non-empty again -- neither
				 * is a failed pick. */
				if(t && o->cursor.item == s->inv_moving.item
				   && o->cursor.damage == s->inv_moving.damage
				   && o->cursor.count >= s->inv_moving.count) {
					s->inv_moving = o->cursor;
					mpp_enter(s, MPP_PH_INV_DST);
					break;
				}
			}
			if(mpp_in_phase(s) >= MPP_INV_STEP_BUDGET_MS)
				mpp_fail(s, "pick-up click never confirmed");
			break;

		case MPP_PH_INV_PUT:
			mpp_press_once(s, out, MPP_ACT_GUI_CLICK);
			if(!s->have_put_id && o->last_click_action_id != s->click_base) {
				s->inv_action_put = o->last_click_action_id;
				s->have_put_id = true;
			}
			if(s->have_put_id) {
				const struct mpp_txn* t
					= mpp_txn_for(o, s->txn_seen, s->inv_action_put);
				if(t && !t->accepted) {
					mpp_fail(s, "server rejected the put-down click");
					return;
				}
				/* ... and once the stack sits in the destination, later
				 * pickups MERGE into it (Beta fills a partial stack first), so
				 * the slot may already hold more than was moved. The marker
				 * still reports what was MOVED: that is what the server's
				 * slot_state receipt for this click says. A different ITEM is
				 * never this move. */
				if(t && o->cursor.item <= 0
				   && o->inv[s->inv_dst].item == s->inv_moving.item
				   && o->inv[s->inv_dst].damage == s->inv_moving.damage
				   && o->inv[s->inv_dst].count >= s->inv_moving.count) {
					/* gen 2's move is the post-reconnect action: its own
					 * marker, outside the measured-play floors */
					bool post = s->gen == 2u;
					if(!post) {
						s->inv_moves++;
						s->inv_last_third = mpp_play_third(s) + 1u;
						s->inv_thirds |= 1u << mpp_play_third(s);
					}
					snprintf(body, sizeof(body),
							 "%s\"n\":%u,\"window\":0,\"action_pick\":%d,"
							 "\"action_put\":%d,\"from_slot\":%d,\"to_slot\":%d,"
							 "\"item\":%d,\"count\":%u,\"damage\":%d,"
							 "\"measured_ms\":%u",
							 post ? "\"gen\":2," : "", s->inv_moves,
							 (int)s->inv_action_pick,
							 (int)s->inv_action_put, mpp_proto_slot(s->inv_src),
							 mpp_proto_slot(s->inv_dst),
							 (int)s->inv_moving.item,
							 (unsigned)s->inv_moving.count,
							 (int)s->inv_moving.damage,
							 (unsigned)s->measured_ms);
					mpp_mark(s, post ? "post_action" : "inv_move", body);
					mpp_enter(s, MPP_PH_INV_CLOSE);
					break;
				}
			}
			if(mpp_in_phase(s) >= MPP_INV_STEP_BUDGET_MS)
				mpp_fail(s, "put-down click never confirmed");
			break;

		case MPP_PH_INV_CLOSE:
			if(o->screen == MPP_SCREEN_INGAME) {
				s->inv_this_cycle = true;
				mpp_enter(s, s->gen == 2u ? MPP_PH_STAND : MPP_PH_BUILD_AIM);
			} else {
				(void)mpp_press(s, out, MPP_ACT_CLOSE);
				if(mpp_in_phase(s) >= MPP_INV_STEP_BUDGET_MS)
					mpp_fail(s, "inventory never closed");
			}
			break;

		case MPP_PH_PAUSE_REST:
			/* LAND FIRST, THEN OPEN THE MENU (PC run pc_mp_play_20260921_174423,
			 * the first to reach this half). CaveWii's pause menu stops the
			 * client tick bodies -- in a remote session too -- so a body that
			 * is in the air when the menu opens stays in the air for as long as
			 * it is open, and the at-rest snapshot below can never be taken:
			 * that run opened the menu mid-jump (server position frozen 1.8
			 * above the ground) and failed "body never came to rest before the
			 * leave". The walker's FORWARD / JUMP are released by leaving WALK;
			 * this phase only waits, with the world still running. */
			if(o->at_rest)
				mpp_enter(s, MPP_PH_PAUSE_OPEN);
			else if(mpp_in_phase(s) >= MPP_REST_BUDGET_MS)
				mpp_fail(s, "body never came to rest before the pause");
			break;

		case MPP_PH_PAUSE_OPEN:
			if(o->screen == MPP_SCREEN_PAUSE)
				mpp_enter(s, MPP_PH_PAUSE_FOCUS);
			else {
				(void)mpp_press(s, out, MPP_ACT_PAUSE);
				if(mpp_in_phase(s) >= MPP_MENU_STEP_BUDGET_MS)
					mpp_fail(s, "pause menu never opened");
			}
			break;

		case MPP_PH_PAUSE_FOCUS:
			/* The snapshot is taken with the pause menu OPEN and the body at
			 * REST: taken while the walker was still moving, the drift up to
			 * the real disconnect would eat the position tolerance. */
			if(!s->snap_done) {
				/* hw1030r: ...and with the inventory and the last dig cell's
				 * air echo both quiet for MPP_SETTLE_QUIET_MS -- a drop the
				 * server collects after p2_end must be IN the snapshot. */
				bool inv_quiet = s->now_ms - s->inv_change_ms
					>= MPP_SETTLE_QUIET_MS;
				bool dig_quiet = !s->dig_air_seen
					|| s->now_ms - s->dig_air_ms >= MPP_SETTLE_QUIET_MS;
				if(o->at_rest && inv_quiet && dig_quiet) {
					s->pre_x = o->x;
					s->pre_y = o->y;
					s->pre_z = o->z;
					s->pre_dim = o->dimension;
					for(int k = 0; k < MPP_INV_SLOTS; k++)
						s->pre_inv[k] = o->inv[k];
					mpp_snapshot(s, "snapshot_pre", o);
					s->snap_done = true;
				} else if(!o->at_rest
						  && mpp_in_phase(s) >= MPP_MENU_STEP_BUDGET_MS)
					mpp_fail(s, "body never came to rest before the leave");
				else if(mpp_in_phase(s) >= MPP_SETTLE_BUDGET_MS)
					mpp_fail(s, inv_quiet
							 ? "dig cell echo never settled before the leave"
							 : "inventory never settled before the leave");
				break;
			}
			if(o->focus == MPP_FOCUS_PAUSE_DISCONNECT && !mpp_pressing(s))
				mpp_enter(s, MPP_PH_PAUSE_LEAVE);
			else if(o->focus != MPP_FOCUS_PAUSE_DISCONNECT)
				(void)mpp_press(s, out, MPP_ACT_GUI_DOWN);
			if(s->phase == MPP_PH_PAUSE_FOCUS
			   && mpp_in_phase(s) >= MPP_MENU_STEP_BUDGET_MS * 2u)
				mpp_fail(s, "focus never reached the leave row");
			break;

		case MPP_PH_PAUSE_LEAVE:
			if(o->screen == MPP_SCREEN_PAUSE)
				mpp_press_once(s, out, MPP_ACT_GUI_CLICK);
			if(!o->remote_session || o->screen == MPP_SCREEN_TITLE)
				mpp_enter(s, MPP_PH_TITLE);
			else if(mpp_in_phase(s) >= MPP_TITLE_BUDGET_MS)
				mpp_fail(s, "leave row never ended the session");
			break;

		case MPP_PH_TITLE:
			if(o->screen == MPP_SCREEN_TITLE && !o->remote_session) {
				snprintf(body, sizeof(body), "\"gen\":1,\"ms\":%u",
						 (unsigned)mpp_in_phase(s));
				mpp_mark(s, "disconnected", body);
				s->gen = 2;
				s->rejoin_t0_ms = s->now_ms;
				mpp_mark(s, "rejoin_begin", "\"gen\":2");
				mpp_enter(s, MPP_PH_REJOIN_TITLE);
			} else if(mpp_in_phase(s) >= MPP_TITLE_BUDGET_MS)
				mpp_fail(s, "title never reached after the leave");
			break;

		case MPP_PH_REJOIN_TITLE:
			if(o->screen == MPP_SCREEN_MP_SETUP)
				mpp_enter(s, MPP_PH_REJOIN_SETUP);
			else if(o->screen == MPP_SCREEN_TITLE) {
				if(o->focus == MPP_FOCUS_TITLE_MULTIPLAYER)
					(void)mpp_press(s, out, MPP_ACT_GUI_CLICK);
				else
					(void)mpp_press(s, out, MPP_ACT_GUI_UP);
			}
			if(s->phase == MPP_PH_REJOIN_TITLE
			   && mpp_in_phase(s) >= MPP_MENU_STEP_BUDGET_MS)
				mpp_fail(s, "multiplayer screen never opened");
			break;

		case MPP_PH_REJOIN_SETUP:
			if(o->screen == MPP_SCREEN_MP_CONNECT
			   || o->screen == MPP_SCREEN_LOAD_WORLD || o->remote_session)
				mpp_enter(s, MPP_PH_READY);
			else if(o->screen == MPP_SCREEN_MP_SETUP) {
				if(o->focus == MPP_FOCUS_MPS_JOIN)
					(void)mpp_press(s, out, MPP_ACT_GUI_CLICK);
				else
					(void)mpp_press(s, out, MPP_ACT_GUI_DOWN);
			}
			if(s->phase == MPP_PH_REJOIN_SETUP
			   && mpp_in_phase(s) >= MPP_MENU_STEP_BUDGET_MS)
				mpp_fail(s, "join never started");
			break;

		case MPP_PH_VERIFY: {
			mpp_snapshot(s, "snapshot_post", o);
			float dx = o->x - s->pre_x, dz = o->z - s->pre_z;
			float dy = o->y - s->pre_y;
			const char* diag = NULL;
			if(o->dimension != s->pre_dim)
				diag = "dimension changed across the rejoin";
			else if(dx * dx + dz * dz > MPP_POS_TOL_H * MPP_POS_TOL_H
					|| dy > MPP_POS_TOL_V || dy < -MPP_POS_TOL_V)
				diag = "position changed across the rejoin";
			else
				for(int k = 0; k < MPP_INV_SLOTS && !diag; k++)
					if(!mpp_item_eq(&o->inv[k], &s->pre_inv[k]))
						diag = "inventory changed across the rejoin";
			uint32_t rejoin_ms = s->now_ms - s->rejoin_t0_ms;
			if(!diag && rejoin_ms > MPP_REJOIN_BUDGET_MS)
				diag = "rejoin past its budget";
			snprintf(body, sizeof(body), "\"ok\":%s,\"rejoin_ms\":%u,\"diag\":\"%s\"",
					 diag ? "false" : "true", (unsigned)rejoin_ms,
					 diag ? diag : "");
			mpp_mark(s, "verified", body);
			if(diag) {
				mpp_fail(s, diag);
				return;
			}
			/* REL-14b: the reconnect is not done until the server has
			 * accepted an action on the NEW session (Packet106 yes). */
			mpp_enter(s, MPP_PH_INV_OPEN);
			break;
		}

		case MPP_PH_STAND:
			if(mpp_in_phase(s) >= MPP_STAND_MS) {
				snprintf(body, sizeof(body),
						 "\"seq_total\":%u,\"teleports\":%u",
						 (unsigned)(s->seq + 1u), s->teleports);
				mpp_mark(s, "ok", body);
				s->phase = MPP_PH_OK;
			}
			break;

		default:
			break;
	}
}

#endif
