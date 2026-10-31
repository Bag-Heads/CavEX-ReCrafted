/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* mp_chat_login_3m phase driver -- see chat_login_script.h for the shape and
 * docs/chat_login_profile.md for the binding contract. */

#include "chat_login_script.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../platform/input.h"
#include "../platform/input_inventory.h" /* lead ruling 2: the real-pad refusal */
#include "chat_ring.h"
#include "game_state.h" /* mp_chat_chord_1m: the LIVE debug-overlay flag */
#include "gui/chat_composer.h"
#include "gui/chat_composer_model.h"
#include "gui/widget_osk.h"
#include "hw_run.h"
#include "../item/inventory.h" /* the chord leg reads the hotbar slot */

/* ---- the twelve chat roles --------------------------------------------- */

/* The SAME twelve roles widget_osk.c's chat_arm_button[] re-arms, IN THE SAME
 * ORDER -- contract section 9 ruling 5 fixes the trace mask bits as
 * "bit0 LEFT, bit1 RIGHT, bit2 UP, bit3 DOWN, bit4 CLICK, bit5 BACK, bit6
 * PAGE, bit7 PREVIOUS (= OSK_CHAT_ARM_LEFT..PREVIOUS order)", so the driver's
 * role index IS the OSK_CHAT_ARM_* index and a req/cp/cd mask needs no
 * translation table that could drift. */
enum {
	CL_BTN_LEFT = OSK_CHAT_ARM_LEFT,
	CL_BTN_RIGHT = OSK_CHAT_ARM_RIGHT,
	CL_BTN_UP = OSK_CHAT_ARM_UP,
	CL_BTN_DOWN = OSK_CHAT_ARM_DOWN,
	CL_BTN_CLICK = OSK_CHAT_ARM_CLICK,
	CL_BTN_BACK = OSK_CHAT_ARM_BACK,
	CL_BTN_PAGE = OSK_CHAT_ARM_PAGE,
	CL_BTN_PREVIOUS = OSK_CHAT_ARM_PREVIOUS,
	CL_BTN_SEND = OSK_CHAT_ARM_SEND,
	CL_BTN_CANCEL = OSK_CHAT_ARM_CANCEL,
	CL_BTN_HOME = OSK_CHAT_ARM_HOME,
	CL_BTN_CHAT = OSK_CHAT_ARM_CHAT,
	CL_BTN_COUNT = OSK_CHAT_ARM_COUNT
};

/* Naming ACTIONS (not codes) is what lets one route drive the Wii's
 * D-pad/Nunchuk codes and the PC's keyboard/mouse codes through the identical
 * production input_binding_levels call. */
static const enum input_button cl_button[CL_BTN_COUNT] = {
	[CL_BTN_CHAT] = IB_CHAT,
	[CL_BTN_UP] = IB_GUI_UP,
	[CL_BTN_DOWN] = IB_GUI_DOWN,
	[CL_BTN_LEFT] = IB_GUI_LEFT,
	[CL_BTN_RIGHT] = IB_GUI_RIGHT,
	[CL_BTN_CLICK] = IB_GUI_CLICK,
	[CL_BTN_BACK] = IB_GUI_CLICK_ALT,
	[CL_BTN_PAGE] = IB_GUI_PAGE,
	[CL_BTN_PREVIOUS] = IB_GUI_PREVIOUS,
	[CL_BTN_SEND] = IB_GUI_QUICK_MOVE,
	[CL_BTN_CANCEL] = IB_INVENTORY,
	[CL_BTN_HOME] = IB_HOME,
};

#define CL_BIT(k) (1u << (k))

/* P1's refusal leg holds the opener this long before letting go. It only has
 * to clear the Wii opener's tap window (INPUT_CHAT_TAP_MS, 500 ms) with room
 * for one slow frame; P1's own budget is 15 s, so the margin is free.
 * b1-chat-opener-0919: before that ruling this leg asked for Chat+Left and
 * exercised the D-pad rock refusal instead. */
#define CHAT_LOGIN_OPENER_HOLD_MS 800u

/* The MARKER CONTRACT this driver emits, echoed in p0.session and used by the
 * collector to pick its marker table (tools/wii_telemetry_server.py's
 * CHAT_LOGIN_CONTRACTS). Contract 1 -- no `contract` key at all -- is every
 * run recorded before this, whose P1 refusal leg was the Up+Left D-pad CHORD.
 * Contract 2 renames that leg p1.hold_refused, because what it now refuses is
 * an opener held past its tap window: Astra ruling 2 D2, "an 800 ms refusal
 * must never be presented as hardware chord proof". */
#define CHAT_LOGIN_MARKER_CONTRACT 2u

/* ---- mp_chat_chord_1m (b1-chat-opener-0919, Astra ruling 2 D2) ----------
 * The SEPARATE chord leg. Its own profile, its own event name and its own
 * marker contract, so a chord claim can never be read out of a login run.
 * Roles: the debug-overlay chord is Minus+Plus, and the two halves the
 * twelve-role vocabulary carries are IB_GUI_PAGE (Remote Plus, code 8) and
 * IB_GUI_PREVIOUS (Remote Minus, code 9) -- config_wii.json binds the chord
 * itself as 9008. The virtual pad always presents INPUT_CAT_NUNCHUK, so this
 * profile exercises the REMOTE family; the Classic shared-button case (Plus
 * is both the opener and a chord half there) is covered by
 * tests/test_input_wii_adapter.c and stays owed to physical acceptance. */
/* Review finding 4: DISJOINT from the login contract numbers (1, 2, ...).
 * Both profiles record a `contract` integer in p0.session and both land
 * in a PASS.json, so a bare "contract": 1 used to mean either "an old
 * Up+Left login run" or "a chord run" depending on which file you were
 * holding. The chord series starts at 101 and the collector keys its
 * tables on the PROFILE as well as the number. */
/* 101 -> 102 (brief 07 A, Astra ruling 3 D2): p2.chord order 0 INVERTED -- it
 * used to require the Plus-first order to fire too, and now requires it NOT
 * to. 101 never produced a hardware run, but a number whose meaning flipped is
 * spent, never reused, or an old stream would have its claim read backwards.
 * The marker also gained slot_before/slot_after. */
/* 102 -> 103 (brief 12 D-1, real-Wii run mp_chat_chord_1m_db01e56_0919_173900):
 * contract 102 IS UNPASSABLE ON HARDWARE and is therefore spent, not merely
 * superseded. It was written when BOTH press orders toggled the overlay -- two
 * toggles leave it as it was found -- and kept "p3.closed.overlay must be
 * false" after 102 made order 0 a NEGATIVE leg. With exactly ONE legal toggle
 * the console is left with F3 ON, which is precisely what that run reported:
 * every product assertion held, and the driver failed its own last check. So
 * the contract gains a RESTORE leg (p4.chord_restore: a second correct-order
 * chord, which must fire exactly once and put the overlay back OFF), and
 * p3.closed now asserts the overlay state the ONE legal toggle really implies
 * instead of demanding the state two toggles used to leave. A 102 stream
 * graded by these tables would read "no restore chord" as a missing marker and
 * "overlay on at p3.closed" as a contradiction, so 102 is refused by number. */
/* 103 -> 104 (hostile review 17, M-1/M-2): p2.chord gained a `fires` COUNT
 * beside its `fired` bool, so the marker set CHANGED -- a 103 stream carries
 * strictly less evidence than the tables now grade, and the collector would
 * have to invent the count it never received. P4 had already argued the point
 * it is bumped for ("a bool would accept three toggles, which end ON again")
 * and P2 was still grading the ruled chord with a bool, so a double fire
 * passed p2 and was rejected four legs later at p3.closed with "the ruled
 * chord fired but the debug overlay is off" -- a COUNTING defect wearing a
 * product defect's diagnosis, the same wrong-leg failure that made 102
 * unpassable. 103 never produced a hardware run either (C-1: the E-2 gate it
 * shipped with refuses the profile before P1 on every card), so like 101 and
 * 102 it is spent rather than merely superseded, and refused by number.
 *
 * 104 ALSO carries C-1's restaging of that gate (lead ruling on hostile review
 * 17): the E-2 refusal LEFT p0.session -- which therefore no longer carries a
 * `chord_bound` it could not know -- and became its own marker,
 * p1.chord_bound, published on the first frame after the pad bind has given
 * cl_chord_scan a live table to read. 104 was created inside this same
 * unmerged series and has never run anywhere, so the moved refusal is folded
 * into it rather than spending another number. */
#define CHAT_CHORD_MARKER_CONTRACT 104u
#define CHAT_CHORD_EVENT "chat_chord"
#define CHAT_CHORD_TOTAL_MS 55000u
#define CHAT_CHORD_P0_MS 40000u
#define CHAT_CHORD_P1_MS 8000u
#define CHAT_CHORD_P2_MS 12000u
#define CHAT_CHORD_P3_MS 12000u
#define CHAT_CHORD_P4_MS 12000u
/* C-1: how long the E-2 gate may wait for the FIRST cl_chord_scan after the
 * pad bind before it gives up and fails by name. The scan rides an input poll
 * that only reaches player 0 once something OWNS player 0, so "the table has
 * been read" is an event to wait for, not a state to assume -- but a card that
 * never polls must still end with a verdict rather than a phase timeout. */
#define CHAT_CHORD_SCAN_WAIT_MS 1000u
/* Whether the configured bindings can form the chord at all. The Wii config
 * binds 9008/212213/409410; the PC config binds a single key, so a PC run
 * legitimately records fired:false -- the same platform split
 * CHAT_LOGIN_CHORD_GATED already carries, and the collector requires true on
 * "wii". The "chat did not open" half is asserted on BOTH platforms. */
#ifdef PLATFORM_PC
#define CHAT_CHORD_ON_DEVICE false
#else
#define CHAT_CHORD_ON_DEVICE true
#endif

/* ---- the P4 route (contract section 2) ---------------------------------- */

/* One requested action and the production focus it must land on. `ch` is the
 * character the focused cell carries (0 where the cell produces none); for a
 * CLICK it is therefore also the byte the draft must gain. EVERY entry -- the
 * GUI_PAGE tab press included -- publishes its own p4.key: the contract counts
 * one per REQUESTED press, 37 directional + 1 page + 15 clicks = 53 (section 9
 * ruling 1). */
struct cl_route_step {
	unsigned char btn;
	unsigned char row, col;
	unsigned char page;
	char ch;
};

#define L (unsigned char)OSK_PAGE_LOWER
#define N (unsigned char)OSK_PAGE_NUM
static const struct cl_route_step cl_route[] = {
	/* 1: '/' (1,0) -> (3,9) */
	{CL_BTN_DOWN, 2, 0, L, 'a'},
	{CL_BTN_DOWN, 3, 0, L, 'z'},
	{CL_BTN_LEFT, 3, 9, L, '/'},
	{CL_BTN_CLICK, 3, 9, L, '/'},
	/* 2: 'l' (3,9) -> (2,8) */
	{CL_BTN_UP, 2, 9, L, ';'},
	{CL_BTN_LEFT, 2, 8, L, 'l'},
	{CL_BTN_CLICK, 2, 8, L, 'l'},
	/* 3: 'o' (2,8) -> (1,8) */
	{CL_BTN_UP, 1, 8, L, 'o'},
	{CL_BTN_CLICK, 1, 8, L, 'o'},
	/* 4: 'g' (1,8) -> (2,4) */
	{CL_BTN_DOWN, 2, 8, L, 'l'},
	{CL_BTN_LEFT, 2, 7, L, 'k'},
	{CL_BTN_LEFT, 2, 6, L, 'j'},
	{CL_BTN_LEFT, 2, 5, L, 'h'},
	{CL_BTN_LEFT, 2, 4, L, 'g'},
	{CL_BTN_CLICK, 2, 4, L, 'g'},
	/* 5: 'i' (2,4) -> (1,7) */
	{CL_BTN_UP, 1, 4, L, 't'},
	{CL_BTN_RIGHT, 1, 5, L, 'y'},
	{CL_BTN_RIGHT, 1, 6, L, 'u'},
	{CL_BTN_RIGHT, 1, 7, L, 'i'},
	{CL_BTN_CLICK, 1, 7, L, 'i'},
	/* 6: 'n' (1,7) -> (3,5) */
	{CL_BTN_DOWN, 2, 7, L, 'k'},
	{CL_BTN_DOWN, 3, 7, L, ','},
	{CL_BTN_LEFT, 3, 6, L, 'm'},
	{CL_BTN_LEFT, 3, 5, L, 'n'},
	{CL_BTN_CLICK, 3, 5, L, 'n'},
	/* 7: space (3,5) -> (4,1); 5*3/10 = 1 = Space */
	{CL_BTN_DOWN, 4, 1, L, ' '},
	{CL_BTN_CLICK, 4, 1, L, ' '},
	/* 8: 'h' (4,1) -> (2,5); 1*10/3 = 3 */
	{CL_BTN_UP, 3, 3, L, 'c'},
	{CL_BTN_UP, 2, 3, L, 'f'},
	{CL_BTN_RIGHT, 2, 4, L, 'g'},
	{CL_BTN_RIGHT, 2, 5, L, 'h'},
	{CL_BTN_CLICK, 2, 5, L, 'h'},
	/* 9: 'u' (2,5) -> (1,6) */
	{CL_BTN_UP, 1, 5, L, 'y'},
	{CL_BTN_RIGHT, 1, 6, L, 'u'},
	{CL_BTN_CLICK, 1, 6, L, 'u'},
	/* 10: 'n' (1,6) -> (3,5) */
	{CL_BTN_DOWN, 2, 6, L, 'j'},
	{CL_BTN_DOWN, 3, 6, L, 'm'},
	{CL_BTN_LEFT, 3, 5, L, 'n'},
	{CL_BTN_CLICK, 3, 5, L, 'n'},
	/* 11: 't' (3,5) -> (1,4) */
	{CL_BTN_UP, 2, 5, L, 'h'},
	{CL_BTN_UP, 1, 5, L, 'y'},
	{CL_BTN_LEFT, 1, 4, L, 't'},
	{CL_BTN_CLICK, 1, 4, L, 't'},
	/* 12: 'e' (1,4) -> (1,2) */
	{CL_BTN_LEFT, 1, 3, L, 'r'},
	{CL_BTN_LEFT, 1, 2, L, 'e'},
	{CL_BTN_CLICK, 1, 2, L, 'e'},
	/* 13: 'r' (1,2) -> (1,3) */
	{CL_BTN_RIGHT, 1, 3, L, 'r'},
	{CL_BTN_CLICK, 1, 3, L, 'r'},
	/* 14: '2' -- the tab step. The clamp keeps (1,3), which is '4' on Numbers. */
	{CL_BTN_PAGE, 1, 3, N, '4'},
	{CL_BTN_LEFT, 1, 2, N, '3'},
	{CL_BTN_LEFT, 1, 1, N, '2'},
	{CL_BTN_CLICK, 1, 1, N, '2'},
	/* 15: '2' again -- click only. */
	{CL_BTN_CLICK, 1, 1, N, '2'},
};
#undef L
#undef N
#define CL_ROUTE_LEN ((unsigned)(sizeof(cl_route) / sizeof(cl_route[0])))

/* ---- per-frame trace (contract section 3) ------------------------------- */

#define CL_TRACE_BATCH 16u

struct cl_trace_row {
	uint32_t t;
	unsigned req, cp, cd;
	char edge; /* '^' request press edge, 'v' request release edge, 0 none */
	int rep;
	/* SIGNED ms: this is production's own pre-reload countdown, and a stepping
	 * frame legitimately reports <= 0 (contract section 11 H2). */
	int rem;
	int r, c;
};

/* A LEG is one timed stretch of the trace -- P2's hold, P2's tap, P2's page
 * tap, P3's disconnect. Section 11 H1: the cadence base is per leg, reported on
 * that leg's own result marker, and a leg with a frame gap over this bound is
 * INCONCLUSIVE rather than slow, because one stalled frame can hide a missed
 * repeat inside its own duration. */
#define CL_LEG_STALL_MS 250u

/* ---- driver state ------------------------------------------------------- */

static struct {
	bool armed;
	/* ---- PLAY MODE (B4a) ----
	 * Armed only by chat_login_arm_play. Everything this flag gates is dead
	 * for mp_chat_login_3m and mp_chat_chord_1m, which is what keeps those two
	 * trace-equivalent (tests/golden/chat_login/shared_login_trace.txt). */
	bool play;
	/* REL-14b-0: play mode that acquires the pad and parks WITHOUT typing a
	 * login (chat_login_arm_play_actions). Survives a restart. */
	bool actions_only;
	/* When the CURRENT login started: arm time, or the last
	 * chat_login_play_restart. The total budget is per LOGIN, so the parked
	 * stretch between two logins is charged to neither. Stays 0 outside play
	 * mode, where s.now_ms - 0 is exactly today's check. */
	uint32_t login_start_ms;
	/* The actions the caller asked the parked pad to assert, resolved to codes
	 * in chat_login_pad_for through the LIVE table like every other request. */
	enum input_button play_actions[CHAT_LOGIN_PLAY_ACTIONS];
	unsigned play_count;
	/* The two rules that end a RUN outside play mode and can only LATCH inside
	 * it (the play driver owns completion): first fault wins. */
	bool play_fault;
	char play_diag[96];
	/* mp_chat_chord_1m instead of mp_chat_login_3m: a different phase script,
	 * a different event name and a different marker contract, in this one TU
	 * because both drive the SAME virtual pad seam. */
	bool chord;
	/* 0 = Plus half first (the WRONG order -- must toggle nothing),
	 * 1 = Minus half first (the ruled order -- must toggle the overlay). */
	unsigned chord_order;
	bool chord_fired;       /* the overlay ACTION reached the owner frame */
	/* ... how MANY times it did, within the current leg. Both chord legs claim
	 * "fired exactly ONCE" (P2's ruled order, contract 104; P4's restore,
	 * 103): a bool cannot tell one toggle from three, and three would leave
	 * the console's overlay ON again. Both legs are GRADED on this count. */
	unsigned chord_fires;
	/* Whether the RULED order (order 1) really fired, latched out of P2 so P3
	 * can assert the overlay state that ONE legal toggle implies. */
	bool chord_ruled_fired;
	/* E-2: whether the LIVE binding table can form the F3 chord at all, read
	 * off the map in chat_login_pad_for every poll. The release config leaves
	 * input.debug_overlay explicitly EMPTY, so this is false there and the
	 * profile refuses to start instead of failing at p3. */
	bool chord_bound;
	/* C-1 (hostile review 17, lead ruling): whether that scan has happened AT
	 * ALL yet. chat_login_pad_for is reached only from input_players_poll,
	 * which skips any player whose owner device is NONE -- and nobody owns
	 * player 0 on an unattended console until P1's own input_player_bind. So
	 * the scan CANNOT run before the bind, which is why the gate that used to
	 * read s.chord_bound in P0 refused every card: P0 needed the scan, the
	 * scan needed the bind, and the bind needed P0 to finish. */
	bool chord_scanned;
	/* The E-2 gate's own one-shot state: armed when the bind lands, done once
	 * it has published its verdict (either way). */
	bool chord_gate_armed, chord_gate_done;
	uint32_t chord_gate_ms;
	bool chord_chat_opened; /* ... and chat must NOT have opened with it */
	/* ... and the hotbar must NOT have stepped: Minus/Plus are its taps.
	 * -1 means "no inventory window to read", which the collector refuses on
	 * the console rather than accepting as proof. */
	int chord_slot_before;
	bool overlay_at_start;  /* the live flag after P0 forced it off */
	struct chat_login_hooks hooks;
	enum chat_login_phase phase;
	uint32_t seq;
	/* The run nonce the harness passed with --cw-mp-username, echoed in
	 * p0.session (contract section 9 ruling 7). Copied at arm time so the
	 * driver never reaches back into the run config. */
	char username[24];

	bool clock_set;
	uint32_t base_ms, now_ms, prev_ms;
	uint32_t phase_start_ms, step_start_ms;
	unsigned step;
	unsigned frames_in_step;
	bool done_sent;
	uint32_t max_frame_ms;

	/* One in-flight marker, retried VERBATIM (same seq, same bytes). */
	char pend[1024];
	char pend_key[64];
	bool pend_valid;

	/* SEAM S1 */
	/* The inventory substitution (site 3) is enabled BEFORE the bind and is
	 * therefore its own flag. input_inventory_poll withholds `published` for
	 * INPUT_IDENTITY_STABLE_MS (250) after an identity edge, so a pad that
	 * enabled the substitution and bound in the same poll read as ABSENT to
	 * screen_controller_recovery for a quarter of a second -- long enough for
	 * the production modal to latch LOST, which needs a deliberate Resume the
	 * profile never gives. Measured on real hardware c2fc977_09.17: p0.session
	 * ok at 1727 ms, p1.pad_bound failed "recovery modal" at 1919 ms. */
	bool publish;
	bool bound;
	bool pad_connected;
	unsigned want, want_prev;
	struct chat_login_pad pad;
	unsigned vocab_count;
	int vocab[CHAT_LOGIN_PAD_CODES * 2];

	/* P1 */
	bool chat_pressed_seen;
	bool chord_opened; /* the PC leg had to close a composer the gate let open */
	/* Section 11 M4: what the chord leg really observed, so "refused" is a
	 * CONCLUSION from two observations rather than an inference from one. */
	bool chord_chat_pressed, chord_screen_changed;
	/* Review finding 2: the refusal leg times its hold from the first frame
	 * the owner frame really showed IB_CHAT DOWN, never from the frame the
	 * request was made -- the gesture arms on the POLL that saw the press
	 * (input_tap_gesture.h), so a load burst between the two used to eat the
	 * hold and could leave it INSIDE the 500 ms tap window. */
	bool hold_seen;
	uint32_t hold_seen_ms, hold_ms;

	/* P2/P3 trace */
	struct cl_trace_row trace[CL_TRACE_BATCH];
	unsigned trace_count;
	uint32_t trace_base_ms;
	bool trace_recording;
	uint32_t trace_dropped;
	char trace_key[16]; /* "p2.trace" / "p3.trace" -- the leg being recorded */
	/* Section 12: WHICH leg these rows belong to. `t` restarts at each leg's
	 * first press, and all of P2's batches otherwise look alike, so without
	 * this the collector reads the tap leg's t=1 as the hold leg's timeline
	 * going backwards. It segments the rows by this field, requires monotonic
	 * `t` within a leg only, and matches each leg to its own result marker. */
	char leg_name[12];
	/* Per-LEG cadence base (section 11 H1). Reset at each leg's first request. */
	uint32_t leg_max_frame_ms;
	/* SEAM S1 site 3, section 11 M5: real hardware reported connected/fresh on
	 * the driver's own channel while it was bound. Latched by the inventory
	 * substitution, consumed on the next driver frame. */
	bool real_pad_seen;
	/* run.ok back-pressure guard (section 11 M3). */
	unsigned ok_frames;
	/* The last frame the driver actually observed, so a TIMEOUT can say what
	 * it was looking at. The first real-Wii run burned P1's whole budget on
	 * screen_load_world and its diag said only "phase timeout". */
	unsigned last_screen, last_req, last_cp, last_cd;
	bool last_owner_connected, last_chat_pressed;

	/* P2 */
	unsigned steps_seen;
	uint32_t first_step_ms, last_step_ms;
	uint32_t first_repeat_ms, repeat_total_ms;
	unsigned repeat_count;
	uint32_t hold_max_frame_ms;
	uint32_t release_req_ms, release_lag_ms;
	bool release_seen;
	unsigned after_release_steps;
	int page_before, page_after;
	uint32_t page_remaining_ms;
	unsigned page_extra_steps;

	/* P3 */
	unsigned frozen_frames;
	unsigned moves_while_down;
	unsigned rearm_at_freeze;
	int frozen_row, frozen_col, frozen_page;
	unsigned frozen_draft_len;
	uint32_t reconnect_ms;
	unsigned frames_blocked;
	/* Section 11 L3: a frame whose CONSUMED down-mask was empty must precede
	 * the first post-reconnect step, or the re-arm did not really demand a
	 * neutral pass. Latched from the same trace rows the collector re-derives
	 * it from. */
	bool p3_watch, p3_neutral_seen, p3_step_seen, p3_neutral_before_step;

	/* P4 */
	unsigned route_at;	/* index into cl_route */
	unsigned key_index; /* the contract's p4.key `i`, 0..51 */

	/* P5/P6 */
	uint32_t lines_before, lines_after;
	uint32_t send_seq;
	uint32_t send_ms;
	bool ring_slot_used[CHAT_RING_CAPACITY];
	bool ring_full_at_send;
	unsigned ack_index;
	uint32_t ack_wait_ms;
} s;

/* ---- plumbing ----------------------------------------------------------- */

void chat_login_reset(void) {
	memset(&s, 0, sizeof(s));
}

void chat_login_arm(const struct hw_run_config* cfg,
					const struct chat_login_hooks* hooks) {
	chat_login_reset();
	if(!cfg || !hooks || !cfg->enabled
	   || (cfg->profile != HW_RUN_PROFILE_MP_CHAT_LOGIN_3M
		   && cfg->profile != HW_RUN_PROFILE_MP_CHAT_CHORD_1M))
		return;
	s.chord = cfg->profile == HW_RUN_PROFILE_MP_CHAT_CHORD_1M;
	s.hooks = *hooks;
	snprintf(s.username, sizeof(s.username), "%s", cfg->mp_username);
	s.armed = true;
}

/* PLAY MODE's arm (B4a 2a). No hw_run config and no profile gate: the caller
 * -- mp_play_script.c -- decides when a login is due, twice per run. The
 * username is the run nonce p0.session echoes, copied here exactly as
 * chat_login_arm copies cfg->mp_username so the driver never reaches back into
 * anybody's config. */
void chat_login_arm_play(const struct chat_login_hooks* hooks,
						 const char* username) {
	chat_login_reset();
	if(!hooks)
		return;
	s.hooks = *hooks;
	snprintf(s.username, sizeof(s.username), "%s", username ? username : "");
	s.play = true;
	s.armed = true;
}

void chat_login_arm_play_actions(const struct chat_login_hooks* hooks,
								 const char* username) {
	chat_login_arm_play(hooks, username);
	s.actions_only = s.armed;
}

bool chat_login_armed(void) {
	return s.armed;
}

enum chat_login_phase chat_login_phase_now(void) {
	return s.phase;
}

uint32_t chat_login_seq_now(void) {
	return s.seq;
}

unsigned chat_login_step_now(void) {
	return s.step;
}

static const char* cl_phase_name(void) {
	switch(s.phase) {
		case CHAT_LOGIN_P0: return "p0";
		case CHAT_LOGIN_P1: return "p1";
		case CHAT_LOGIN_P2: return "p2";
		case CHAT_LOGIN_P3: return "p3";
		case CHAT_LOGIN_P4: return "p4";
		case CHAT_LOGIN_P5: return "p5";
		case CHAT_LOGIN_P6: return "p6";
		default: return "run";
	}
}

/* Format and try to publish one marker. `key` identifies WHICH marker is in
 * flight: a caller retrying the same marker next frame re-flushes the STORED
 * line (same seq, same bytes) instead of re-formatting it, and a caller that
 * asks for a different marker while one is still pending is refused until the
 * ring drains. That is what lets the collector assume a gapless `seq`.
 * The published NAME is the key minus its "<phase>." prefix and any trailing
 * digits (batch/index keys), exactly as split_recovery does it. */
static bool cl_mark_corr(const char* key, uint32_t corr, bool ok,
						 const char* fields_fmt, ...) {
	if(s.pend_valid && strcmp(s.pend_key, key) != 0)
		return false;
	if(!s.pend_valid) {
		char fields[768];
		fields[0] = 0;
		if(fields_fmt && fields_fmt[0]) {
			va_list ap;
			va_start(ap, fields_fmt);
			vsnprintf(fields, sizeof(fields), fields_fmt, ap);
			va_end(ap);
		}
		const char* dot = strchr(key, '.');
		const char* name = dot ? dot + 1 : key;
		char pretty[48];
		snprintf(pretty, sizeof(pretty), "%s", name);
		for(size_t k = strlen(pretty); k > 0; k--) {
			if(pretty[k - 1] < '0' || pretty[k - 1] > '9')
				break;
			pretty[k - 1] = 0;
		}
		int n = snprintf(s.pend, sizeof(s.pend),
			"{\"event\":\"%s\",\"seq\":%u,\"phase\":\"%s\","
			"\"marker\":\"%s\",\"ms\":%u,\"corr\":%u,\"ok\":%s%s%s}",
			s.chord ? CHAT_CHORD_EVENT : "chat_login",
			s.seq + 1u, cl_phase_name(), pretty, s.now_ms, corr,
			ok ? "true" : "false", fields[0] ? "," : "", fields);
		if(n <= 0 || (size_t)n >= sizeof(s.pend))
			return false;
		snprintf(s.pend_key, sizeof(s.pend_key), "%s", key);
		s.pend_valid = true;
	}
	if(!s.hooks.emit || !s.hooks.emit(s.hooks.user, s.pend))
		return false;
	s.pend_valid = false;
	s.pend_key[0] = 0;
	s.seq++;
	return true;
}

#define cl_mark(key, ok, ...) cl_mark_corr((key), 0u, (ok), __VA_ARGS__)

/* RFC 8259 escaping for every string the driver did not author itself. */
static void cl_json_escape(char* dst, size_t cap, const char* src) {
	size_t w = 0;
	if(!dst || !cap)
		return;
	for(size_t r = 0; src && src[r] && w + 1 < cap; r++) {
		unsigned char ch = (unsigned char)src[r];
		const char* two = NULL;
		switch(ch) {
			case '"': two = "\\\""; break;
			case '\\': two = "\\\\"; break;
			case '\n': two = "\\n"; break;
			case '\r': two = "\\r"; break;
			case '\t': two = "\\t"; break;
			case '\b': two = "\\b"; break;
			case '\f': two = "\\f"; break;
			default: break;
		}
		if(two) {
			if(w + 2 >= cap)
				break;
			dst[w++] = two[0];
			dst[w++] = two[1];
		} else if(ch < 0x20 || ch == 0x7F) {
			if(w + 6 >= cap)
				break;
			w += (size_t)snprintf(dst + w, cap - w, "\\u%04X", ch);
		} else
			dst[w++] = (char)ch;
	}
	dst[w] = 0;
}

static void cl_unbind(void) {
	if(!s.bound && !s.publish)
		return;
	s.bound = false;
	s.publish = false;
	s.want = s.want_prev = 0;
	s.pad.count = 0;
	s.pad.connected = false;
	s.vocab_count = 0;
	input_player_unbind(0);
	chat_controller_mode_set(false);
}

/* End the run. `marker` is the contract marker the phase was AWAITING; NULL
 * publishes "<phase>.fail" instead. The marker is one best-effort attempt;
 * profile_done is UNCONDITIONAL, so a run always carries its verdict. The
 * virtual pad is withdrawn on this path exactly as on the ok path. */
static void cl_fail_at(const char* marker, const char* diag) {
	char status[96];
	const char* phase = cl_phase_name();
	if(s.pend_valid) {
		s.pend_valid = false;
		s.pend_key[0] = 0;
	}
	char escaped[160];
	cl_json_escape(escaped, sizeof(escaped), diag);
	char key[80];
	snprintf(key, sizeof(key), "%s.%s", phase, marker ? marker : "fail");
	(void)cl_mark(key, false, "\"diag\":\"%s\"", escaped);
	snprintf(status, sizeof(status), "%s:%s", phase, diag);
	s.phase = CHAT_LOGIN_FAILED;
	cl_unbind();
	if(!s.done_sent && s.hooks.profile_done) {
		s.done_sent = true;
		s.hooks.profile_done(s.hooks.user, status);
	}
}

/* `why` plus the last observed frame, inside the contract's 96-char diag cap:
 *   "phase timeout scr=3 st=3 req=0 cp=0 cd=0 own=1 chat=0"
 * scr = CHAT_LOGIN_SCREEN_*, st = the phase step, req/cp/cd = the requested /
 * consumed-pressed / consumed-down role masks in hex (section 9 ruling 5 bit
 * order), own = the owner frame's connected flag, chat = the owner frame's
 * pressed[IB_CHAT] the refusal leg waits on. */
static const char* cl_state_diag(const char* why) {
	static char buf[96];
	snprintf(buf, sizeof(buf),
			 "%s scr=%u st=%u req=%X cp=%X cd=%X own=%u chat=%u", why,
			 s.last_screen, s.step, s.last_req, s.last_cp, s.last_cd,
			 s.last_owner_connected ? 1u : 0u, s.last_chat_pressed ? 1u : 0u);
	return buf;
}

static void cl_fail(const char* diag) {
	cl_fail_at(NULL, diag);
}

static void cl_enter(enum chat_login_phase phase) {
	s.phase = phase;
	s.phase_start_ms = s.now_ms;
	s.step = 0;
	s.step_start_ms = s.now_ms;
	s.frames_in_step = 0;
}

static void cl_step(unsigned next) {
	s.step = next;
	s.step_start_ms = s.now_ms;
	s.frames_in_step = 0;
}

/* The marker the current (phase, step) is WAITING for. A phase failure
 * publishes on that marker -- "the collector reads WHICH observation never
 * arrived" -- and only a failure with no outstanding marker falls back to
 * "<phase>.fail". A pure function of the phase machine, so it cannot drift out
 * of step with the steps themselves the way a stored copy could. */
/* REL-14b-0: the P1 step an action-only driver takes after p1.pad_bound in
 * place of the hold/opener legs (steps 3..10 never run for it). */
#define CL_STEP_ACTIONS_READY 11u
static void cl_ok_tail(void);

static const char* cl_awaiting(void) {
	if(s.chord) {
		switch(s.phase) {
			case CHAT_LOGIN_P0: return "session";
			case CHAT_LOGIN_P1: return "pad_bound";
			case CHAT_LOGIN_P2: return "chord";
			case CHAT_LOGIN_P3: return s.step <= 2 ? "opened" : "closed";
			case CHAT_LOGIN_P4: return "chord_restore";
			default: return NULL;
		}
	}
	switch(s.phase) {
		case CHAT_LOGIN_P0: return "session";
		case CHAT_LOGIN_P1:
			if(s.step == CL_STEP_ACTIONS_READY)
				return "actions_ready";
			return s.step <= 2 ? "pad_bound" : s.step <= 7 ? "hold_refused"
														   : "chat_open";
		case CHAT_LOGIN_P2:
			return s.step <= 2 ? "hold_result"
				: s.step <= 5  ? "tap_result"
							   : "page_hold";
		case CHAT_LOGIN_P3: return s.step <= 3 ? "frozen" : "rearmed";
		case CHAT_LOGIN_P4:
			return s.step <= 1 ? "origin"
				: s.route_at >= CL_ROUTE_LEN ? "draft"
											 : "key";
		case CHAT_LOGIN_P5:
			return s.step == 0 ? "sent_requested"
				: s.step <= 2  ? "composer_cleared"
							   : "lines_sent";
		case CHAT_LOGIN_P6: return "ack";
		default: return NULL;
	}
}

/* The TOTAL cap: mp_chat_chord_1m is a 60 s profile, so its own cap has to
 * fire inside that window exactly as the login profile's 180 s cap does. */
static uint32_t cl_total_budget(void) {
	return s.chord ? CHAT_CHORD_TOTAL_MS : CHAT_LOGIN_TOTAL_MS;
}

static uint32_t cl_phase_budget(void) {
	if(s.chord) {
		switch(s.phase) {
			case CHAT_LOGIN_P0: return CHAT_CHORD_P0_MS;
			case CHAT_LOGIN_P1: return CHAT_CHORD_P1_MS;
			case CHAT_LOGIN_P2: return CHAT_CHORD_P2_MS;
			case CHAT_LOGIN_P3: return CHAT_CHORD_P3_MS;
			case CHAT_LOGIN_P4: return CHAT_CHORD_P4_MS;
			default: return CHAT_CHORD_TOTAL_MS;
		}
	}
	switch(s.phase) {
		case CHAT_LOGIN_P0: return CHAT_LOGIN_P0_MS;
		case CHAT_LOGIN_P1: return CHAT_LOGIN_P1_MS;
		case CHAT_LOGIN_P2: return CHAT_LOGIN_P2_MS;
		case CHAT_LOGIN_P3: return CHAT_LOGIN_P3_MS;
		case CHAT_LOGIN_P4: return CHAT_LOGIN_P4_MS;
		case CHAT_LOGIN_P5: return CHAT_LOGIN_P5_MS;
		case CHAT_LOGIN_P6: return CHAT_LOGIN_P6_MS;
		default: return CHAT_LOGIN_TOTAL_MS;
	}
}

/* ---- SEAM S1 ------------------------------------------------------------ */

static void cl_vocab_add(int code) {
	for(unsigned k = 0; k < s.vocab_count; k++)
		if(s.vocab[k] == code)
			return;
	if(s.vocab_count < sizeof(s.vocab) / sizeof(s.vocab[0]))
		s.vocab[s.vocab_count++] = code;
}

static void cl_code_add(struct chat_login_pad* pad, int code) {
	for(unsigned k = 0; k < pad->count; k++)
		if(pad->codes[k] == code)
			return;
	if(pad->count < CHAT_LOGIN_PAD_CODES)
		pad->codes[pad->count++] = code;
}

/* E-2: can the LIVE binding table form the F3 chord at all?
 *
 * Derived, never written as "9008": some binding of IB_DEBUG_OVERLAY must be a
 * CHORD (the HOLD*1000+PRESS encoding input.c decodes) whose two halves are the
 * codes this config binds to the two roles the driver actually presses --
 * IB_GUI_PREVIOUS and IB_GUI_PAGE. That is exactly the chord the leg drives, so
 * a config that binds some OTHER chord still answers false and is refused,
 * rather than leaving the leg to press two buttons that form nothing. */
static bool cl_chord_scan(int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
						  const size_t* lengths) {
	for(size_t k = 0; k < lengths[IB_DEBUG_OVERLAY]
		&& k < INPUT_BINDINGS_PER_ACTION; k++) {
		int code = map[IB_DEBUG_OVERLAY][k];
		/* The two-component encoding only: a code whose quotient is itself a
		 * composite is not a pair of physical halves. */
		if(code < 1008 || code / 1000 >= 1008)
			continue;
		int hold = code / 1000, press = code % 1000;
		bool has_hold = false, has_press = false;
		for(size_t j = 0; j < lengths[IB_GUI_PREVIOUS]
			&& j < INPUT_BINDINGS_PER_ACTION; j++)
			has_hold |= map[IB_GUI_PREVIOUS][j] == hold;
		for(size_t j = 0; j < lengths[IB_GUI_PAGE]
			&& j < INPUT_BINDINGS_PER_ACTION; j++)
			has_press |= map[IB_GUI_PAGE][j] == press;
		if(has_hold && has_press)
			return true;
	}
	return false;
}

bool chat_login_pad_for(unsigned player,
						int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
						const size_t* lengths, struct chat_login_pad* out) {
	if(!s.armed || player != 0 || !out || !map || !lengths)
		return false;
	/* C-1: the SCAN sits ABOVE the `!s.bound` gate. It is pure over
	 * map/lengths and needs nothing the bind provides, and keeping it below
	 * the return bought nothing -- the poll that calls this function only
	 * reaches player 0 once player 0 is owned, so the first scan lands on the
	 * poll AFTER the bind either way. Above the gate the driver at least gets
	 * that first scan on the very next poll instead of on the next frame the
	 * pad happens to be asked for. Only the READING moves: the substitution
	 * below still needs the bind, so `read`, `user` and every frame, level,
	 * glyph, edge and inventory decision stay byte-identical for normal play
	 * and for every other profile (source/platform/input.c:1781). */
	s.chord_bound = cl_chord_scan(map, lengths);
	s.chord_scanned = true;
	if(!s.bound)
		return false;
	s.vocab_count = 0;
	s.pad.count = 0;
	s.pad.connected = s.pad_connected;
	for(int k = 0; k < CL_BTN_COUNT; k++) {
		int b = (int)cl_button[k];
		size_t n = lengths[b];
		if(n > INPUT_BINDINGS_PER_ACTION)
			n = INPUT_BINDINGS_PER_ACTION;
		for(size_t j = 0; j < n; j++) {
			int code = map[b][j];
			/* >= 1008 is a CHORD encoding, never a physical code: its two
			 * halves are resolved through this same reader, so asserting the
			 * composite would double-count. < 0 is an unset slot. */
			if(code < 0 || code >= 1008)
				continue;
			cl_vocab_add(code);
			if(s.want & CL_BIT(k))
				cl_code_add(&s.pad, code);
		}
	}
	/* B4a 2c: the ONLY place the vocabulary reaches past the twelve chat
	 * roles, and it is reachable only from a PARKED play-mode driver --
	 * chat_login_play_request refuses in every other state and a restart
	 * releases the request before the next login runs. So for
	 * mp_chat_login_3m and mp_chat_chord_1m the loop below does not exist:
	 * same codes, same vocabulary, same native-key answer as today.
	 *
	 * Resolution is the SAME as above on purpose (live table, chord encodings
	 * skipped because their halves come through this same reader), so a play
	 * action is asserted exactly as a chat role is. */
	if(s.play && s.phase == CHAT_LOGIN_PARKED) {
		for(unsigned k = 0; k < s.play_count; k++) {
			int b = (int)s.play_actions[k];
			if(b < 0 || b >= IB_COUNT)
				continue;
			size_t n = lengths[b];
			if(n > INPUT_BINDINGS_PER_ACTION)
				n = INPUT_BINDINGS_PER_ACTION;
			for(size_t j = 0; j < n; j++) {
				int code = map[b][j];
				if(code < 0 || code >= 1008)
					continue;
				cl_vocab_add(code);
				cl_code_add(&s.pad, code);
			}
		}
	}
	*out = s.pad;
	return true;
}

bool chat_login_pad_key(void* user, int key) {
	const struct chat_login_pad* pad = user;
	if(!pad)
		return false;
	for(unsigned k = 0; k < pad->count; k++)
		if(pad->codes[k] == key)
			return true;
	return false;
}

bool chat_login_pad_inventory(struct input_device device,
							  struct input_identity* identity, bool* connected,
							  bool* fresh, bool* neutral) {
	/* Gated on PUBLISH, not on bound: the inventory has to have been carrying
	 * the virtual pad for its whole stability window before anything binds, or
	 * the recovery gate reads the owner as absent on the bind frame. The
	 * pre-bind refusal check runs BEFORE this flag is set, so it still reads
	 * the raw hardware and can never refuse against its own reflection. */
	if(!s.armed || !s.publish || !identity || !connected || !fresh || !neutral)
		return false;
	if(device.kind != CHAT_LOGIN_PAD_DEVICE_KIND
	   || device.channel != CHAT_LOGIN_PAD_CHANNEL)
		return false;
	/* Section 11 M5: the in/out parameters still carry the RAW hardware read on
	 * entry. A real pad reporting connected or fresh on the driver's own
	 * channel would race the seam for the same inventory slot, so latch it and
	 * let the next driver frame fail the run by name rather than substituting
	 * over a controller somebody just plugged in. */
	if(*connected || *fresh)
		s.real_pad_seen = true;
	*identity = (struct input_identity) {true, INPUT_CAT_NUNCHUK};
	*connected = true;
	*fresh = true;
	*neutral = true;
	return true;
}

bool chat_login_pad_native_key(int key, bool* value) {
	if(!s.armed || !s.bound || !value)
		return false;
	bool known = false;
	for(unsigned k = 0; k < s.vocab_count && !known; k++)
		known = s.vocab[k] == key;
	if(!known)
		return false;
	*value = s.pad_connected && chat_login_pad_key(&s.pad, key);
	return true;
}

/* ---- observation helpers ------------------------------------------------ */

static unsigned cl_mask_from(const struct input_player_frame* f, bool pressed) {
	unsigned mask = 0;
	if(!f || !f->connected)
		return 0;
	for(int k = 0; k < CL_BTN_COUNT; k++) {
		int b = (int)cl_button[k];
		if(pressed ? f->pressed[b] : f->down[b])
			mask |= CL_BIT(k);
	}
	return mask;
}

static void cl_request(unsigned mask) {
	s.want = mask;
}

/* Open a timed leg: its own cadence base, its own time origin, its own trace
 * key. Section 11 H1/M1. */
static void cl_leg_begin(const char* trace_key, const char* leg) {
	snprintf(s.trace_key, sizeof(s.trace_key), "%s", trace_key);
	snprintf(s.leg_name, sizeof(s.leg_name), "%s", leg);
	/* Belt and braces: cl_leg_end already stopped recording at the previous
	 * leg's closer, so nothing should be buffered here. A row that survived
	 * would belong to the previous leg's time origin and would make `t` jump
	 * backwards at the seam -- exactly what the collector rejected (§12). */
	s.trace_count = 0;
	s.trace_base_ms = s.now_ms;
	s.leg_max_frame_ms = 0;
	s.trace_recording = true;
}

/* Record one trace row for the leg being timed. Rows are dropped (and counted)
 * rather than overwritten while a batch is still waiting for a sender slot --
 * a hole the marker reports is honest; a silently reordered timeline is not. */
static void cl_trace_row(const struct input_player_frame* owner,
						 const struct screen_chat_observation* obs) {
	if(!s.trace_recording)
		return;
	if(s.trace_count >= CL_TRACE_BATCH) {
		s.trace_dropped++;
		return;
	}
	struct cl_trace_row* row = &s.trace[s.trace_count++];
	bool rise = (s.want & ~s.want_prev) != 0;
	bool fall = (s.want_prev & ~s.want) != 0;
	row->t = s.now_ms - s.trace_base_ms;
	row->req = s.want;
	row->edge = rise ? '^' : fall ? 'v' : 0;
	row->cp = cl_mask_from(owner, true);
	row->cd = cl_mask_from(owner, false);
	row->rep = obs && obs->moved ? (obs->moved_repeat ? 2 : 1) : 0;
	/* Section 9 ruling 4 / section 11 H2: `rem` is the countdown the STEP
	 * DECISION saw -- post-decrement, PRE-reload -- and it is PRODUCTION's own
	 * number (struct osk_chat_result::repeat_seen_s, copied through
	 * screen_chat_observe), never a driver constant. A stepping frame therefore
	 * reports <= 0 because production really had run out, which is what makes
	 * "never a step while the countdown is positive" checkable. */
	row->rem = obs ? (int)(obs->repeat_seen_s * 1000.0F
						   + (obs->repeat_seen_s < 0.0F ? -0.5F : 0.5F))
				   : 0;
	row->r = obs ? obs->row : -1;
	row->c = obs ? obs->col : -1;
	/* Section 11 L3: the P3 re-arm watch reads the same rows the collector
	 * does -- a CONSUMED-neutral frame must precede the first step. */
	if(s.p3_watch) {
		if(row->cd == 0 && !s.p3_step_seen)
			s.p3_neutral_seen = true;
		if(row->rep && !s.p3_step_seen) {
			s.p3_step_seen = true;
			s.p3_neutral_before_step = s.p3_neutral_seen;
		}
	}
}

/* Publish one trace batch. Returns true when nothing is left to publish. */
static bool cl_trace_flush(const char* key) {
	if(s.trace_count == 0)
		return true;
	char frames[640];
	size_t w = 0;
	for(unsigned k = 0; k < s.trace_count && w + 1 < sizeof(frames); k++) {
		const struct cl_trace_row* row = &s.trace[k];
		char edge[2] = {row->edge, 0};
		int n = snprintf(frames + w, sizeof(frames) - w,
						 "%s%u:%X%s:%X:%X:%d:%d:%d:%d", k ? ";" : "", row->t,
						 row->req, edge, row->cp, row->cd, row->rep, row->rem,
						 row->r, row->c);
		if(n <= 0 || (size_t)n >= sizeof(frames) - w)
			break;
		w += (size_t)n;
	}
	frames[w < sizeof(frames) ? w : sizeof(frames) - 1] = 0;
	if(!cl_mark(key, true,
				"\"leg\":\"%s\",\"frames\":\"%s\",\"n\":%u,\"first_t\":%u,"
				"\"dropped\":%u",
				s.leg_name, frames, s.trace_count, s.trace[0].t,
				s.trace_dropped))
		return false;
	s.trace_count = 0;
	return true;
}

/* Close a timed leg: publish its trailing rows and STOP RECORDING, so every row
 * of the leg precedes its result marker and nothing is recorded in the boundary
 * window between one leg's closer and the next leg's first request (§13.2 --
 * the collector saw a batch that began at t=1152 and then restarted at t=1
 * because those boundary frames rode into the next leg's batch). Returns false
 * while the sender is still refusing the last batch; the caller retries. */
static bool cl_leg_end(const char* key) {
	if(!cl_trace_flush(key))
		return false;
	s.trace_recording = false;
	s.trace_key[0] = 0;
	return true;
}

/* ---- phases ------------------------------------------------------------- */

/* P0: the session the whole run needs, observed -- never assumed. */
/* Admitted remote gameplay -- P0's own predicate, re-checked by REL-14b-0's
 * action-only grant on the frame it is given. */
static bool cl_session_ready(const struct chat_login_ctx* ctx) {
	return ctx->remote_session && ctx->world_loaded
		&& !ctx->world_switch_pending && ctx->local_player
		&& ctx->screen_is_ingame;
}

static void cl_phase_p0(const struct chat_login_ctx* ctx) {
	/* THE REAL-WII FINDING (run 1e42bf5_09.18): world_loaded goes true the
	 * moment the world installs, but the client legitimately stays on
	 * screen_load_world until the remote safe core is installed AND meshed --
	 * up to LWORLD_NEAR_READY_TIMEOUT_S (30 s, screen_load_world.c:87). That
	 * console run sat there for its whole run with near_missing pinned at 85.
	 * P0 used to complete anyway, so the loading dwell was charged to P1's
	 * 15 s budget and the chord leg -- which needs screen_ingame to exist --
	 * timed out 14 s before the backstop could even fire. Waiting for
	 * ADMITTED gameplay here charges that dwell to P0's own 60 s budget, which
	 * covers the 30 s backstop with margin, and leaves every later phase
	 * measuring only its own work. On PC the dwell is sub-millisecond, which
	 * is exactly why this passed there and failed on hardware. */
	if(!(ctx->remote_session && ctx->world_loaded && !ctx->world_switch_pending
		 && ctx->local_player && ctx->screen_is_ingame))
		return;
	/* Contract section 9 ruling 7: the device ECHOES the run nonce it was
	 * handed, so the collector can require the server receipt's player name to
	 * equal this run's own derivation instead of trusting a timestamp. */
	char username[40];
	cl_json_escape(username, sizeof(username), s.username);
	if(!cl_mark("p0.session", true,
				"\"epoch\":%u,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,"
				"\"username\":\"%s\",\"platform\":\"%s\",\"screen\":%u,"
				"\"contract\":%u",
				ctx->epoch, (double)ctx->x, (double)ctx->y, (double)ctx->z,
				username, CHAT_LOGIN_PLATFORM, ctx->screen_id,
				CHAT_LOGIN_MARKER_CONTRACT))
		return;
	cl_enter(CHAT_LOGIN_P1);
	/* An action-only RECONNECT keeps its bind (step 0's refusal would read the
	 * pad's own reflection) but re-observes it: straight to the settle step. */
	if(s.actions_only && s.bound)
		cl_step(2);
}

/* Lead ruling 2: refuse to arm the virtual pad while ANY WPAD channel reports
 * connected, so it can never shadow (or be shadowed by) a real controller. */
/* Has the device INVENTORY taken the virtual pad yet? That is the state
 * screen_controller_recovery reads to decide whether player 0's owner is
 * present, and input_inventory_poll withholds it for INPUT_IDENTITY_STABLE_MS
 * (250) after the identity edge. On PC there is no inventory entry for a
 * keyboard owner and the recovery gate reads the owner frame instead, so there
 * is nothing to wait for -- the honest predicate is "the consumer can see it",
 * not "the inventory says so". */
static bool cl_pad_published(void) {
#ifdef PLATFORM_PC
	return true;
#else
	const struct input_inventory* inv = input_devices();
	if(!inv || CHAT_LOGIN_PAD_CHANNEL >= INPUT_DEVICE_COUNT)
		return false;
	const struct input_device_status* st = &inv->devices[CHAT_LOGIN_PAD_CHANNEL];
	return st->published.connected
		&& st->published.category == INPUT_CAT_NUNCHUK && st->armed;
#endif
}

static bool cl_real_pad_present(unsigned* channel) {
	const struct input_inventory* inv = input_devices();
	if(!inv)
		return false;
	for(unsigned k = 0; k < 4 && k < INPUT_DEVICE_COUNT; k++)
		if(inv->devices[k].observed.connected) {
			*channel = k;
			return true;
		}
	return false;
}

static void cl_phase_p1(const struct chat_login_ctx* ctx,
						const struct input_player_frame* owner,
						const struct screen_chat_observation* obs,
						bool obs_open) {
	switch(s.step) {
		case 0: {
			unsigned channel = 0;
			if(cl_real_pad_present(&channel)) {
				char diag[64];
				snprintf(diag, sizeof(diag), "real pad present ch%u", channel);
				cl_fail_at("pad_bound", diag);
				return;
			}
			/* The composer must not fall into PC keyboard text mode: with a
			 * driver at the controls the aliases have to run through the
			 * owner-frame path exactly as they do on a Wii. Set BEFORE the
			 * composer opens -- screen_chat_reset samples it once. */
			chat_controller_mode_set(true);
			/* Publish FIRST, bind LATER (step 1). */
			s.pad_connected = true;
			s.publish = true;
			cl_request(0);
			cl_step(1);
			return;
		}
		case 1: {
			/* Wait until the consumer that decides "is player 0's owner
			 * present" can actually SEE the pad, then bind -- observed state,
			 * never a frame count. On Wii that consumer is
			 * screen_controller_recovery reading the inventory's `published`
			 * identity, which input_inventory_poll withholds for 250 ms after
			 * the identity edge. On PC a keyboard owner has no inventory entry
			 * at all and the gate reads the owner frame, so there is nothing to
			 * wait for. */
			if(!cl_pad_published()) {
				if(s.now_ms - s.step_start_ms > 3000u) {
					cl_fail_at("pad_bound",
							   "inventory never published the pad");
					return;
				}
				return;
			}
			if(!input_player_bind(0, (struct input_device) {
									  CHAT_LOGIN_PAD_DEVICE_KIND,
									  CHAT_LOGIN_PAD_CHANNEL})) {
				cl_fail_at("pad_bound", "bind refused");
				return;
			}
			s.bound = true;
			cl_step(2);
			return;
		}
		case 2:
			/* A fresh bind blocks every action until the pad has been seen
			 * NEUTRAL (input_owner.h) -- give production that poll. */
			/* At least two polls after the bind before judging anything: the
			 * first seeds the owner frame, the second is the earliest one
			 * screen_controller_recovery can have evaluated with an owner in
			 * place (main.c:4066 runs before main.c:4140). A modal still up
			 * after that is the real thing and fails closed below. */
			if(s.frames_in_step < 2 || !owner || !owner->connected)
				return;
			if(ctx->recovery_modal) {
				if(s.now_ms - s.step_start_ms > 2000u) {
					cl_fail_at("pad_bound", "recovery modal");
					return;
				}
				return;
			}
			{
				/* Section 11 L1: report the binding production OBSERVED, not
				 * the one the driver asked for -- a bind that silently landed
				 * somewhere else must show in the marker, not be papered over
				 * by echoing the request back. */
				struct input_device bound = {INPUT_DEVICE_NONE, 0};
				bool ok = input_player_device(0, &bound);
				if(!cl_mark("p1.pad_bound", ok,
							"\"bound\":%s,\"kind\":%u,\"channel\":%u,"
							"\"category\":%u",
							ok ? "true" : "false", (unsigned)bound.kind,
							bound.channel, (unsigned)owner->category))
					return;
				if(!ok) {
					cl_fail("no device owns player 0 after the bind");
					return;
				}
			}
			/* mp_chat_chord_1m shares P0/P1 verbatim -- the same seam, the
			 * same refusal, the same OBSERVED binding -- and then runs its own
			 * script instead of the login phases. */
			if(s.chord) {
				cl_enter(CHAT_LOGIN_P2);
				return;
			}
			cl_step(s.actions_only ? CL_STEP_ACTIONS_READY : 3);
			return;
		case 3: /* refusal leg: hold the opener PAST its tap window */
			/* The opener lives on screen_ingame ONLY. world_loaded can latch
			 * while the loading screen is still presenting (measured in the
			 * real PC app, 2026-09-17: P0 passed at 154 ms and the first
			 * request landed on screen_load_world, where nothing consumes it),
			 * so wait for the screen the press is aimed at. */
			if(!ctx->screen_is_ingame)
				return;
			s.chord_chat_pressed = false;
			s.chord_screen_changed = false;
			cl_request(CL_BIT(CL_BTN_CHAT));
			cl_step(4);
			return;
		case 4:
			/* Section 11 M4: "the press reached the owner" and "the screen
			 * changed" are two separate OBSERVATIONS; refused is their
			 * conclusion, so a lost press can never read as a refusal.
			 *
			 * b1-chat-opener-0919: the gated press this leg asks for is no
			 * longer Chat+Left (the D-pad rock refusal) -- the Wii opener is
			 * a TAP of button 2, and its refusal is a hold longer than
			 * INPUT_CHAT_TAP_MS, which is the pointer-look recenter. The
			 * marker keeps its contract name and its section 11 M4 fields;
			 * only the gate it exercises changed. Where no gate exists (PC)
			 * the composer legitimately opens on the press edge, so the leg
			 * stops holding the moment it sees the screen change. */
			if(owner && owner->connected && owner->pressed[IB_CHAT])
				s.chord_chat_pressed = true;
			if(owner && owner->connected && owner->down[IB_CHAT]
			   && !s.hold_seen) {
				s.hold_seen = true;
				s.hold_seen_ms = s.now_ms;
			}
			if(ctx->screen_is_chat)
				s.chord_screen_changed = true;
			/* Review finding 2: the clock starts at the OBSERVED press. A frame
			 * the driver spent waiting for the poll to deliver it is not hold
			 * time, and counting it could release the opener while the gesture
			 * still had it inside the tap window -- reporting a correct build
			 * as a failure. */
			if(!s.chord_screen_changed
			   && (!s.hold_seen
				   || s.now_ms - s.hold_seen_ms <= CHAT_LOGIN_OPENER_HOLD_MS))
				return;
			s.hold_ms = s.hold_seen ? s.now_ms - s.hold_seen_ms : 0u;
			cl_request(0);
			cl_step(5);
			return;
		case 5: {
			if(owner && owner->connected && owner->pressed[IB_CHAT])
				s.chord_chat_pressed = true;
			if(ctx->screen_is_chat)
				s.chord_screen_changed = true;
			if(s.frames_in_step < 2)
				return;
			bool refused = s.chord_chat_pressed && !s.chord_screen_changed;
			/* The opener gate is compiled for the Wii only
			 * (input.c input_chat_open_edge). Where it does not exist the
			 * composer legitimately opens; the leg then records refused:false
			 * and closes what it opened. See docs/chat_login_profile.md. */
			bool expect = CHAT_LOGIN_CHORD_GATED;
			if(!cl_mark("p1.hold_refused",
						refused == expect && s.chord_chat_pressed,
						"\"refused\":%s,\"chat_pressed\":%s,"
						"\"screen_changed\":%s,\"hold_ms\":%u",
						refused ? "true" : "false",
						s.chord_chat_pressed ? "true" : "false",
						s.chord_screen_changed ? "true" : "false",
						s.hold_ms))
				return;
			if(!s.chord_chat_pressed) {
				cl_fail("the held press never reached the owner frame");
				return;
			}
			if(refused != expect) {
				cl_fail("opener gate answered unexpectedly");
				return;
			}
			s.chord_opened = s.chord_screen_changed;
			cl_step(s.chord_opened ? 6 : 8);
			return;
		}
		case 6: /* close the composer the ungated platform opened */
			if(!obs_open)
				return;
			cl_request(CL_BIT(CL_BTN_CANCEL));
			cl_step(7);
			return;
		case 7:
			cl_request(0);
			if(ctx->screen_is_chat)
				return;
			cl_step(8);
			return;
		case 8: /* the real opener: ONE frame down, released on the next --
				 * a TAP well inside the Wii opener's window, which is what
				 * opens the composer there (b1-chat-opener-0919). */
			if(s.frames_in_step < 2 || !ctx->screen_is_ingame)
				return;
			s.chat_pressed_seen = false;
			cl_request(CL_BIT(CL_BTN_CHAT));
			cl_step(9);
			return;
		case 9:
			if(owner && owner->connected && owner->pressed[IB_CHAT])
				s.chat_pressed_seen = true;
			cl_request(0);
			cl_step(10);
			return;
		case 10:
			if(owner && owner->connected && owner->pressed[IB_CHAT])
				s.chat_pressed_seen = true;
			if(!ctx->screen_is_chat || !obs_open)
				return;
			if(!s.chat_pressed_seen) {
				cl_fail_at("chat_open", "opener edge never reached the owner");
				return;
			}
			/* Contract section 10 ruling 1: with the inventory seam in place
			 * the recovery modal must NOT be showing here -- that is the
			 * observation which proves the virtual pad is present to every
			 * consumer, not only to the owner frame. */
			if(!cl_mark("p1.chat_open",
						obs->draft_len == 0 && !ctx->recovery_modal,
						"\"row\":%d,\"col\":%d,\"page\":%d,\"draft_len\":%u,"
						"\"recovery_modal\":%s",
						obs->row, obs->col, obs->page, obs->draft_len,
						ctx->recovery_modal ? "true" : "false"))
				return;
			if(ctx->recovery_modal) {
				cl_fail("recovery modal");
				return;
			}
			if(obs->draft_len != 0) {
				cl_fail("composer opened with a non-empty draft");
				return;
			}
			cl_enter(CHAT_LOGIN_P2);
			/* Recording starts with the HOLD leg's own first request (P2 step
			 * 0), not here: opening it at the phase boundary gave the leg two
			 * time origins one tick apart, and the second rebase made `t` run
			 * backwards inside a single named leg (section 12). */
			return;
		case CL_STEP_ACTIONS_READY:
			/* REL-14b-0: readiness is granted on THIS frame's observation, so
			 * a session lost after the bind is refused, never parked. */
			if(!cl_session_ready(ctx)) {
				cl_fail_at("actions_ready",
						   cl_state_diag("session lost before readiness"));
				return;
			}
			if(!cl_mark("p1.actions_ready", true, "\"typed_login\":false"))
				return;
			cl_ok_tail(); /* play mode: PARKED, pad kept, nothing reported */
			return;
		default: return;
	}
}

static void cl_note_step(const struct screen_chat_observation* obs) {
	if(!obs || !obs->moved)
		return;
	s.steps_seen++;
	if(s.steps_seen == 1) {
		s.first_step_ms = s.now_ms;
	} else {
		uint32_t gap = s.now_ms - s.last_step_ms;
		if(s.steps_seen == 2)
			s.first_repeat_ms = gap;
		else {
			s.repeat_total_ms += gap;
			s.repeat_count++;
		}
	}
	s.last_step_ms = s.now_ms;
}

static void cl_phase_p2(const struct chat_login_ctx* ctx,
						const struct screen_chat_observation* obs,
						bool obs_open) {
	(void)ctx;
	if(!obs_open) {
		cl_fail("composer closed during the repeat trace");
		return;
	}
	switch(s.step) {
		case 0: /* leg (a): hold RIGHT */
			s.steps_seen = 0;
			s.repeat_count = s.repeat_total_ms = 0;
			s.first_repeat_ms = 0;
			s.hold_max_frame_ms = 0;
			s.after_release_steps = 0;
			s.release_seen = false;
			cl_leg_begin("p2.trace", "hold");
			cl_request(CL_BIT(CL_BTN_RIGHT));
			cl_step(1);
			return;
		case 1:
			cl_note_step(obs);
			if(s.steps_seen < 7 && s.now_ms - s.step_start_ms < 2000u)
				return;
			s.release_req_ms = s.now_ms;
			cl_request(0);
			cl_step(2);
			return;
		case 2: {
			unsigned before = s.steps_seen;
			/* Section 11 M2, Astra's words applied exactly: a step in a frame
			 * whose CONSUMED down-mask still holds the direction is production
			 * behaving correctly and is never counted against the game. Only
			 * steps AFTER the consumed release are `after_release_steps`; the
			 * requested->consumed gap is `release_lag_ms`, reported and never
			 * scored. */
			if(!s.release_seen) {
				const struct input_player_frame* owner = input_player_frame(0);
				if(!owner || !owner->connected
				   || !owner->down[IB_GUI_RIGHT]) {
					s.release_seen = true;
					s.release_lag_ms = s.now_ms - s.release_req_ms;
				}
			}
			cl_note_step(obs);
			if(s.steps_seen != before && s.release_seen)
				s.after_release_steps++;
			if(s.now_ms - s.step_start_ms < 500u)
				return;
			s.hold_max_frame_ms = s.leg_max_frame_ms;
			if(!cl_leg_end("p2.trace"))
				return;
			{
				uint32_t mean = s.repeat_count ? s.repeat_total_ms / s.repeat_count : 0;
				bool ok = s.steps_seen >= 6 && s.after_release_steps == 0;
				if(!cl_mark("p2.hold_result", ok,
							"\"steps\":%u,\"first_repeat_ms\":%u,"
							"\"mean_repeat_ms\":%u,\"max_frame_ms\":%u,"
							"\"release_lag_ms\":%u,\"after_release_steps\":%u",
							s.steps_seen, s.first_repeat_ms, mean,
							s.hold_max_frame_ms, s.release_lag_ms,
							s.after_release_steps))
					return;
				if(!ok) {
					cl_fail("held repeat did not step cleanly");
					return;
				}
			}
			cl_step(3);
			return;
		}
		case 3: /* leg (b): a tap INSIDE the 250 ms delay */
			s.steps_seen = 0;
			cl_leg_begin("p2.trace", "tap");
			cl_request(CL_BIT(CL_BTN_RIGHT));
			cl_step(4);
			return;
		case 4:
			cl_note_step(obs);
			cl_request(0);
			cl_step(5);
			return;
		case 5:
			cl_note_step(obs);
			if(s.now_ms - s.step_start_ms < 400u)
				return;
			if(!cl_leg_end("p2.trace"))
				return;
			if(!cl_mark("p2.tap_result", s.steps_seen == 1,
						"\"steps\":%u,\"max_frame_ms\":%u", s.steps_seen,
						s.leg_max_frame_ms))
				return;
			if(s.steps_seen != 1) {
				cl_fail("a tap inside the repeat delay did not step exactly once");
				return;
			}
			cl_step(6);
			return;
		case 6: /* leg (c): hold DOWN, tap PAGE */
			s.steps_seen = 0;
			s.page_extra_steps = 0;
			s.page_before = obs->page;
			cl_leg_begin("p2.trace", "page");
			cl_request(CL_BIT(CL_BTN_DOWN));
			cl_step(7);
			return;
		case 7:
			cl_note_step(obs);
			if(s.frames_in_step < 3)
				return;
			cl_request(CL_BIT(CL_BTN_DOWN) | CL_BIT(CL_BTN_PAGE));
			cl_step(8);
			return;
		case 8:
			cl_note_step(obs);
			cl_request(CL_BIT(CL_BTN_DOWN));
			cl_step(9);
			return;
		case 9: {
			unsigned before = s.steps_seen;
			cl_note_step(obs);
			if(s.steps_seen != before)
				s.page_extra_steps++;
			if(s.frames_in_step == 1) {
				s.page_after = obs->page;
				s.page_remaining_ms
					= (uint32_t)(obs->repeat_remaining_s * 1000.0F + 0.5F);
			}
			if(s.now_ms - s.step_start_ms < 150u)
				return;
			cl_request(0);
			if(!cl_leg_end("p2.trace"))
				return;
			{
				bool ok = s.page_after != s.page_before && s.page_extra_steps == 0;
				if(!cl_mark("p2.page_hold", ok,
							"\"page_before\":%d,\"page_after\":%d,"
							"\"extra_steps\":%u,\"remaining_ms\":%u,"
							"\"max_frame_ms\":%u",
							s.page_before, s.page_after, s.page_extra_steps,
							s.page_remaining_ms, s.leg_max_frame_ms))
					return;
				if(!ok) {
					cl_fail("a page tap under a held direction misbehaved");
					return;
				}
			}
			cl_enter(CHAT_LOGIN_P3);
			cl_leg_begin("p3.trace", "reconnect");
			return;
		}
		default: return;
	}
}

static void cl_phase_p3(const struct chat_login_ctx* ctx,
						const struct screen_chat_observation* obs,
						bool obs_open) {
	if(!obs_open) {
		cl_fail("composer closed during the reconnect leg");
		return;
	}
	switch(s.step) {
		case 0: /* establish the held direction P3 freezes */
			cl_request(CL_BIT(CL_BTN_RIGHT));
			cl_step(1);
			return;
		case 1:
			if(s.frames_in_step < 3)
				return;
			s.frozen_row = obs->row;
			s.frozen_col = obs->col;
			s.frozen_page = obs->page;
			s.frozen_draft_len = obs->draft_len;
			s.moves_while_down = 0;
			s.frozen_frames = 0;
			s.pad_connected = false; /* the pad "goes away" while RIGHT is held */
			cl_step(2);
			return;
		case 2:
			if(obs->moved)
				s.moves_while_down++;
			if(++s.frozen_frames < 5)
				return;
			s.rearm_at_freeze = obs->rearm_mask;
			s.pad_connected = true; /* back, still holding RIGHT */
			s.reconnect_ms = s.now_ms;
			/* Section 11 L3: from here on, watch the trace rows for a
			 * consumed-neutral frame ahead of the first step. */
			s.p3_watch = true;
			s.p3_neutral_seen = s.p3_step_seen = false;
			s.p3_neutral_before_step = false;
			cl_step(3);
			return;
		case 3: {
			if(obs->moved)
				s.moves_while_down++;
			if(s.frames_in_step < 3)
				return;
			bool ok = s.moves_while_down == 0 && obs->row == s.frozen_row
				&& obs->col == s.frozen_col && obs->page == s.frozen_page
				&& obs->draft_len == s.frozen_draft_len;
			if(!cl_trace_flush("p3.trace"))
				return;
			if(!cl_mark("p3.frozen", ok,
						"\"moves_while_down\":%u,\"rearm_mask\":%u,"
						"\"draft_len\":%u,\"row\":%d,\"col\":%d,\"page\":%d",
						s.moves_while_down, s.rearm_at_freeze, obs->draft_len,
						obs->row, obs->col, obs->page))
				return;
			if(!ok) {
				cl_fail("a disconnected pad kept editing");
				return;
			}
			cl_request(0); /* the neutral frame the re-arm demands */
			cl_step(4);
			return;
		}
		case 4:
			/* A lost owner raises the PRODUCTION controller-recovery modal
			 * (screen.c screen_controller_recovery), which owns the whole
			 * update and suspends the composer until a deliberate Resume press
			 * on a frame the neutral gate was ALREADY open. Measured in the
			 * real PC app, 2026-09-17: without this the composer never ran
			 * again and P3 sat on `rearmed` for its whole budget. The press is
			 * IB_GUI_CLICK -- the modal's own action -- and it cannot leak into
			 * the draft, because the composer's re-arm blocks every chat button
			 * across the same disconnect. */
			if(s.frames_in_step < 3)
				return;
			if(!ctx->recovery_modal) {
				/* With the inventory seam the modal never opens (the pad is
				 * still PRESENT to every inventory consumer; only the owner
				 * frame went away), and then a Resume press would land on the
				 * composer and TYPE. Drive it only when the modal is really
				 * up -- observed state, never an assumption about platform. */
				cl_step(6);
				return;
			}
			cl_request(CL_BIT(CL_BTN_CLICK));
			cl_step(5);
			return;
		case 5:
			cl_request(0);
			cl_step(6);
			return;
		case 6:
			if(s.frames_in_step < 3)
				return;
			cl_request(CL_BIT(CL_BTN_RIGHT));
			cl_step(7);
			return;
		case 7: {
			if(!obs->moved)
				return;
			s.frames_blocked = s.now_ms - s.reconnect_ms;
			cl_request(0);
			if(!cl_leg_end("p3.trace"))
				return;
			{
				/* Section 11 L3: the verdict is the trace's own -- a frame
				 * whose CONSUMED mask was empty must precede the first
				 * post-reconnect step, or the re-arm let a button held across
				 * the reconnect act. */
				bool ok = s.p3_neutral_before_step;
				if(!cl_mark("p3.rearmed", ok,
							"\"frames_blocked\":%u,\"neutral_before_step\":%s,"
							"\"max_frame_ms\":%u",
							s.frames_blocked, ok ? "true" : "false",
							s.leg_max_frame_ms))
					return;
				if(!ok) {
					cl_fail("the first move after the reconnect needed no neutral frame");
					return;
				}
			}
			s.p3_watch = false; /* cl_leg_end already closed the trace */
			cl_enter(CHAT_LOGIN_P4);
			return;
		}
		default: return;
	}
}

/* P4 step 0: drive back to the canonical origin (LOWER, row 1, col 0) THROUGH
 * INPUT EVENTS and assert it -- the P2/P3 legs leave focus elsewhere and the
 * route table is fixed from that origin. Closed loop on the observed focus, so
 * no assumption about where the earlier legs finished can be wrong. */
static unsigned cl_origin_button(const struct screen_chat_observation* obs) {
	if(obs->page != OSK_PAGE_LOWER)
		return CL_BIT(CL_BTN_PREVIOUS);
	if(obs->row == OSK_CHAT_ROW_TABS)
		return CL_BIT(CL_BTN_DOWN);
	if(obs->row > OSK_CHAT_GRID_ROW_FIRST)
		return CL_BIT(CL_BTN_UP);
	if(obs->col != 0)
		return CL_BIT(CL_BTN_LEFT);
	return 0;
}

static void cl_phase_p4(const struct chat_login_ctx* ctx,
						const struct screen_chat_observation* obs,
						bool obs_open) {
	(void)ctx;
	if(!obs_open) {
		cl_fail("composer closed while typing");
		return;
	}
	if(s.step == 0) { /* origin: press */
		unsigned want = cl_origin_button(obs);
		if(want == 0) {
			if(!cl_mark("p4.origin", true, "\"row\":%d,\"col\":%d,\"page\":%d",
						obs->row, obs->col, obs->page))
				return;
			s.route_at = 0;
			s.key_index = 0;
			cl_step(2);
			return;
		}
		if(s.frames_in_step > 60) {
			cl_fail_at("origin", "could not reach the canonical origin");
			return;
		}
		cl_request(want);
		s.step = 1; /* keep frames_in_step: it is the origin-loop budget */
		return;
	}
	if(s.step == 1) { /* origin: release */
		cl_request(0);
		s.step = 0;
		return;
	}
	if(s.step == 2) { /* route: press */
		if(s.route_at >= CL_ROUTE_LEN) {
			char draft[CHAT_COMPOSER_CAP + 1];
			snprintf(draft, sizeof(draft), "%s", obs->draft ? obs->draft : "");
			bool ok = strcmp(draft, CHAT_LOGIN_COMMAND) == 0
				&& obs->draft_len == CHAT_LOGIN_COMMAND_LEN;
			char escaped[CHAT_COMPOSER_CAP * 2 + 2];
			cl_json_escape(escaped, sizeof(escaped), draft);
			if(!cl_mark("p4.draft", ok, "\"text\":\"%s\",\"len\":%u", escaped,
						obs->draft_len))
				return;
			if(!ok) {
				cl_fail("the draft is not the contract command");
				return;
			}
			cl_enter(CHAT_LOGIN_P5);
			return;
		}
		cl_request(CL_BIT(cl_route[s.route_at].btn));
		cl_step(3);
		return;
	}
	if(s.step == 3) { /* route: observe + release */
		const struct cl_route_step* want = &cl_route[s.route_at];
		cl_request(0);
		bool hit = obs->row == (int)want->row && obs->col == (int)want->col
			&& obs->page == (int)want->page;
		/* Contract section 9 ruling 1: ONE p4.key per REQUESTED press --
		 * 37 directional + 1 GUI_PAGE + 15 clicks = 53. The tab press is a
		 * press, so it carries its own marker like every other one. */
		char key[32];
		snprintf(key, sizeof(key), "p4.key%u", s.key_index);
		if(!cl_mark(key, hit,
					"\"i\":%u,\"want_row\":%u,\"want_col\":%u,\"want_page\":%u,"
					"\"row\":%d,\"col\":%d,\"page\":%d,\"draft_len\":%u,"
					"\"ch\":%d",
					s.key_index, (unsigned)want->row, (unsigned)want->col,
					(unsigned)want->page, obs->row, obs->col, obs->page,
					obs->draft_len, (int)(unsigned char)want->ch))
			return;
		if(!hit) {
			cl_fail("a route step landed off target");
			return;
		}
		s.key_index++;
		s.route_at++;
		cl_step(2);
		return;
	}
}

static void cl_phase_p5(const struct chat_login_ctx* ctx,
						const struct screen_chat_observation* obs,
						bool obs_open) {
	switch(s.step) {
		case 0: {
			uint32_t before = 0;
			if(!s.hooks.chat_lines_sent
			   || !s.hooks.chat_lines_sent(s.hooks.user, &before))
				return; /* no remote status yet: wait inside the budget */
			/* corr of a REQUEST marker is its OWN seq, so every later
			 * observation can point back at it. */
			uint32_t corr = s.seq + 1u;
			/* Contract section 9 ruling 3: the ring's head and count at the
			 * moment of the request travel WITH the request, so the collector
			 * can re-derive "already occupied" -- (index - head) mod size <
			 * count -- instead of taking the device's word for it. */
			uint32_t head = ctx->ring ? (uint32_t)ctx->ring->head : 0u;
			uint32_t count = ctx->ring ? (uint32_t)ctx->ring->count : 0u;
			if(!cl_mark_corr("p5.sent_requested", corr, true,
							 "\"lines_before\":%u,\"ring_head\":%u,"
							 "\"ring_count\":%u",
							 before, head, count))
				return;
			s.send_seq = corr;
			s.lines_before = before;
			s.send_ms = s.now_ms;
			/* The stale-ack baseline, by the SAME rule the collector applies. */
			s.ring_full_at_send = count >= CHAT_RING_CAPACITY;
			memset(s.ring_slot_used, 0, sizeof(s.ring_slot_used));
			for(uint32_t k = 0; k < count; k++)
				s.ring_slot_used[(head + k) % CHAT_RING_CAPACITY] = true;
			cl_request(CL_BIT(CL_BTN_SEND));
			cl_step(1);
			return;
		}
		case 1:
			cl_request(0);
			cl_step(2);
			return;
		case 2:
			if(obs_open || ctx->screen_is_chat) {
				/* SPEC "Error/refused enqueue": a refused submission RETAINS
				 * the draft and the screen, so the composer never clears. Name
				 * the refusal on p5.lines_sent -- the observation that really
				 * failed -- instead of letting the phase time out on
				 * composer_cleared with no counter in the evidence. */
				uint32_t after = s.lines_before;
				if(s.now_ms - s.send_ms < 1500u)
					return;
				if(s.hooks.chat_lines_sent)
					(void)s.hooks.chat_lines_sent(s.hooks.user, &after);
				if(after != s.lines_before)
					return; /* the line DID go out; the close is just late */
				if(!cl_mark_corr("p5.lines_sent", s.send_seq, false,
								 "\"before\":%u,\"after\":%u", s.lines_before,
								 after))
					return;
				cl_fail("the submission was refused: no chat line was queued");
				return;
			}
			if(!cl_mark_corr("p5.composer_cleared", s.send_seq, true,
							 "\"open\":%s,\"screen_ingame\":%s,\"draft_len\":%u",
							 obs_open ? "true" : "false",
							 ctx->screen_is_ingame ? "true" : "false",
							 obs->draft_len))
				return;
			cl_step(3);
			return;
		case 3: {
			uint32_t after = 0;
			if(!s.hooks.chat_lines_sent
			   || !s.hooks.chat_lines_sent(s.hooks.user, &after))
				return;
			if(after == s.lines_before)
				return; /* the remote pump has not queued the frame yet */
			s.lines_after = after;
			bool ok = after == s.lines_before + 1u;
			if(!cl_mark_corr("p5.lines_sent", s.send_seq, ok,
							 "\"before\":%u,\"after\":%u", s.lines_before,
							 after))
				return;
			if(!ok) {
				cl_fail("the submission was not exactly one chat line");
				return;
			}
			cl_enter(CHAT_LOGIN_P6);
			return;
		}
		default: return;
	}
}

/* What "the run ended OK" DOES, once run.ok is out. Outside play mode this is
 * verbatim what both call sites did before: withdraw the virtual pad and
 * report once. In PLAY MODE (B4a 2b) neither happens -- the pad stays bound
 * and published for the caller and the driver PARKS, because the mp_play
 * driver owns completion and still has a whole play phase, a disconnect and a
 * second login to run on this pad. */
static void cl_ok_tail(void) {
	if(s.play) {
		s.phase = CHAT_LOGIN_PARKED;
		/* Release the login script's own last request: from here on the only
		 * thing that may drive the pad is chat_login_play_request. */
		cl_request(0);
		s.want_prev = 0;
		s.trace_recording = false;
		return;
	}
	cl_unbind();
	if(!s.done_sent && s.hooks.profile_done) {
		s.done_sent = true;
		s.hooks.profile_done(s.hooks.user, "ok");
	}
}

static void cl_phase_p6(const struct chat_login_ctx* ctx) {
	if(!ctx->ring)
		return;
	if(s.ring_full_at_send) {
		cl_fail_at("ack", "chat ring was already full at send");
		return;
	}
	for(size_t k = 0; k < ctx->ring->count; k++) {
		size_t index = (ctx->ring->head + k) % CHAT_RING_CAPACITY;
		if(s.ring_slot_used[index])
			continue; /* occupied before the send: a stale line, never the ack */
		if(strcmp(ctx->ring->lines[index], CHAT_LOGIN_ACK_LINE) != 0)
			continue;
		s.ack_index = (unsigned)index;
		s.ack_wait_ms = s.now_ms - s.send_ms;
		if(!cl_mark_corr("p6.ack", s.send_seq, true,
						 "\"ring_index\":%u,\"wait_ms\":%u", s.ack_index,
						 s.ack_wait_ms))
			return;
		s.phase = CHAT_LOGIN_OK;
		if(!cl_mark("run.ok", true,
					"\"seq_total\":%u,\"max_frame_ms_run\":%u", s.seq + 1u,
					s.max_frame_ms))
			return;
		cl_ok_tail();
		return;
	}
}

/* run.ok is published by P6 itself (it is always the LAST line). A run that
 * reaches OK but had its run.ok refused retries it here. */
static void cl_finish_ok(void) {
	if(!cl_mark("run.ok", true, "\"seq_total\":%u,\"max_frame_ms_run\":%u",
				s.seq + 1u, s.max_frame_ms))
		return;
	cl_ok_tail();
}

/* ---- mp_chat_chord_1m's phases ------------------------------------------
 * Astra ruling 2 D2 + ruling 3 D2, in order: start from a KNOWN overlay state,
 * press the debug chord in BOTH press orders with neutral gaps, and assert by
 * markers that ONLY the ruled order (hold Minus, then press Plus) made the
 * overlay ACTION fire, that neither order opened chat or moved the hotbar
 * during or right after a chord, and that after a neutral re-arm the ordinary
 * opener gesture still opens the composer -- which is then closed again,
 * leaving the overlay OFF. */

/* The two halves of the Minus+Plus chord, as ROLES: production resolves them
 * to this platform's codes exactly as it does for every other request. */
#define CL_CHORD_PLUS CL_BIT(CL_BTN_PAGE)	  /* Remote Plus, code 8 */
#define CL_CHORD_MINUS CL_BIT(CL_BTN_PREVIOUS) /* Remote Minus, code 9 */

/* The local player's hotbar slot, exactly as screen_ingame.c reads it, or -1
 * where there is no inventory window to read yet. The collector treats -1 as
 * "never observable" and refuses it on "wii", because the claim "the chord did
 * not move the hotbar" needs a real reading to mean anything. */
static int cl_hotbar_slot(void) {
	struct window_container* wc = gstate.players[0].windows[WINDOWC_INVENTORY];
	struct inventory* inv = wc ? windowc_get_latest(wc) : NULL;
	return inv ? (int)inventory_get_hotbar(inv) : -1;
}

/* ---- the MODELLED console input layer (brief 12 D-1) --------------------
 *
 * A PC build binds no F3 chord and compiles no ORDER GATE
 * (input_chord_order_apply is PLATFORM_WII-only, source/platform/input.c), so
 * a PC run could never reach the state the real-Wii run
 * mp_chat_chord_1m_db01e56_0919_173900 reached and this driver got wrong: ONE
 * legal toggle spent and the console's debug overlay left ON. That is exactly
 * the state a PC suite must be able to drive, or the same defect hides from it
 * again.
 *
 * So the seam models the console's INPUT LAYER and nothing else. The test's
 * config binds a real chord, so IB_DEBUG_OVERLAY's press edge on the owner
 * frame is production's own; what is missing on PC is the gate that refuses
 * the wrong press order, and that is supplied here by calling PRODUCTION'S OWN
 * pure machine (input_chord_order_step, source/platform/input_owner.h -- the
 * same call input.c makes on the Wii) on the same two halves the config's
 * chord is made of. main.c's F3 hunk (main.c:4539-4542) is the harness's half:
 * chat_login_test_chord_edge() publishes this frame's answer so the test's main
 * loop can apply it. Everything the leg ASSERTS -- the phase sequence, every
 * marker, the hotbar readings, the restore chord and the final overlay state --
 * stays production's own.
 *
 * Kept out of every shipping binary by the build, exactly like screen_chat.c's
 * enqueue seam: only the cavex_test_* pools define CAVEWII_TESTING. */
#ifdef CAVEWII_TESTING
/* 0 = off, CHAT_LOGIN_CHORD_MODEL_BOUND = a console whose config binds the
 * chord (the gate below is modelled), CHAT_LOGIN_CHORD_MODEL_UNBOUND = a
 * console staged with the RELEASE config, which must be refused at p0. */
static int cl_chord_model;
static struct input_chord_order cl_chord_model_gate;
static bool cl_chord_model_fired;

void chat_login_test_chord_model(int mode) {
	cl_chord_model = mode;
	cl_chord_model_gate = (struct input_chord_order) {0};
	cl_chord_model_fired = false;
}

bool chat_login_test_chord_edge(void) { return cl_chord_model_fired; }
#endif

/* Whether the bindings this run is really using can form the chord at all.
 *
 * E-2 (Astra ergonomics 2, maintainer-accepted): the F3 chord LEFT the release
 * configuration -- config_wii.json now carries an explicit empty
 * input.debug_overlay -- so "a Wii run binds the chord" stopped being a
 * property of the PLATFORM and became a property of the CONFIG that was
 * staged. It is therefore DERIVED from the live binding table
 * (cl_chord_scan, called from chat_login_pad_for on every poll) instead of
 * compiled in, and a console run that was staged with the release config is
 * refused BY NAME at p0 rather than left to fail four legs later at p3. */
static bool cl_chord_on_device(void) {
#ifdef CAVEWII_TESTING
	if(cl_chord_model == CHAT_LOGIN_CHORD_MODEL_BOUND)
		return true;
#endif
	return s.chord_bound;
}

/* Whether THIS run must have the chord to mean anything. True on a console,
 * where the chord is the entire profile; false on PC, where the legs that do
 * not need it (a chord never opens chat, never moves the hotbar, the opener
 * still works after the re-arm) are still worth running. */
static bool cl_chord_required(void) {
#ifdef CAVEWII_TESTING
	if(cl_chord_model)
		return true; /* the model IS a console, in both of its modes */
#endif
	return CHAT_CHORD_ON_DEVICE;
}

/* The one place the overlay ACTION is read, so the modelled gate and the real
 * device answer can never drift apart. Called EXACTLY once per driver frame of
 * this profile (the gate is a state machine over polls, not over edges). */
static bool cl_chord_fire(const struct input_player_frame* owner) {
	bool edge = owner && owner->connected && owner->pressed[IB_DEBUG_OVERLAY];
#ifdef CAVEWII_TESTING
	/* H-1: the ORDER GATE is what a console HAS and a PC build does not
	 * (input_chord_order_apply is PLATFORM_WII-only), so it belongs to "the
	 * model IS a console" -- BOTH modes -- and never to the separate question
	 * of whether the staged config binds the chord. While it hung on
	 * CHAT_LOGIN_CHORD_MODEL_BOUND alone, the only mode that reads the LIVE
	 * scan was also the only mode that could not press a chord in order, so no
	 * passing test could ever be made to depend on cl_chord_scan answering
	 * true -- which is exactly how C-1 shipped unnoticed. */
	if(cl_chord_model) {
		/* The gate wants the PHYSICAL levels of the two halves, which is what
		 * input.c hands it on the Wii (it reads the device, upstream of the
		 * owner frame). Here they have to be reconstructed, because a chord
		 * that is fully down CONSUMES both of its component keys
		 * (input_binding_levels, source/platform/input_owner.h) -- so on the
		 * very poll the chord completes, both halves read FALSE and the chord
		 * reads true. The reconstruction is exact rather than approximate: a
		 * half is down when its own level is down OR when the chord is, since
		 * the chord can only be down while both halves physically are, and
		 * consumption is the only thing that clears a half's level. */
		bool chord = owner && owner->connected && owner->down[IB_DEBUG_OVERLAY];
		bool hold = chord
			|| (owner && owner->connected && owner->down[IB_GUI_PREVIOUS]);
		bool press = chord
			|| (owner && owner->connected && owner->down[IB_GUI_PAGE]);
		edge = input_chord_order_step(&cl_chord_model_gate, hold, press) && edge;
	}
	cl_chord_model_fired = edge;
#endif
	return edge;
}

static void cl_chord_p0(const struct chat_login_ctx* ctx) {
	if(!(ctx->remote_session && ctx->world_loaded && !ctx->world_switch_pending
		 && ctx->local_player && ctx->screen_is_ingame))
		return;
	/* A KNOWN overlay state, forced here rather than assumed from the card's
	 * config: clear the "force" mode too, because a forced HUD is re-asserted
	 * every frame and SUPPRESSES the very toggle this profile measures
	 * (main.c). hw_run_apply_profile already leaves force_overlay off for this
	 * profile; this is the belt to that braces. */
	gstate.force_debug_overlay = false;
	gstate.debug_overlay = false;
	s.overlay_at_start = gstate.debug_overlay;
	/* The E-2 refusal USED to live here, and could never work here (C-1).
	 * It reads the LIVE binding table, which cl_chord_scan can only see from
	 * inside an input poll that reaches player 0 -- and nobody owns player 0
	 * until P1 binds the virtual pad. P0 therefore always read a never-written
	 * `false` and refused every card, naming the very config that was staged.
	 * The gate is now cl_chord_gate(), on the first frame after the bind has
	 * produced a real reading; p0.session no longer claims a `chord_bound` it
	 * cannot know. */
	if(!cl_mark("p0.session", true,
				"\"platform\":\"%s\",\"contract\":%u,\"overlay\":%s,"
				"\"screen\":%u",
				CHAT_LOGIN_PLATFORM, CHAT_CHORD_MARKER_CONTRACT,
				s.overlay_at_start ? "true" : "false", ctx->screen_id))
		return;
	cl_enter(CHAT_LOGIN_P1);
}

/* ---- E-2, restaged: the chord-binding gate (contract 104) ----------------
 *
 * WHAT it asserts is unchanged from E-2 (Astra ergonomics 2): the F3 chord
 * LEFT the release configuration, so "a Wii run binds the chord" is a property
 * of the CONFIG that was staged, not of the platform, and a card staged with
 * the release config has nothing for this profile to measure. Refusing it BY
 * NAME beats pressing two buttons that form nothing through four legs and
 * failing at p3 with "the ruled order never reached IB_DEBUG_OVERLAY", which
 * reads like a product defect and is not one.
 *
 * WHERE it asserts it is what C-1 moved. The reading exists only after the
 * pad bind, so this runs on the first frame P1 has produced one -- still
 * before any typing and long before P2 presses anything -- and it HOLDS every
 * leg until it has an answer rather than guessing one.
 *
 * The two ways to have no answer are kept apart on purpose, because they need
 * opposite actions from the operator: a table that was read and binds no chord
 * is a STAGING error (stage the developer config), while a table that was
 * never read at all is a DRIVER/ownership fault (the poll never reached player
 * 0), and a single message for both would send the operator round the loop
 * C-1 already sent them round once.
 *
 * Returns false when no phase may run this frame: either the verdict failed
 * the run, or the gate is still waiting for its first reading. */
static bool cl_chord_gate(void) {
	if(s.chord_gate_done || !s.bound)
		return true;
	if(!s.chord_gate_armed) {
		s.chord_gate_armed = true;
		s.chord_gate_ms = s.now_ms;
	}
	if(!s.chord_scanned) {
		if(s.now_ms - s.chord_gate_ms < CHAT_CHORD_SCAN_WAIT_MS)
			return false; /* hold every leg until the table has been read */
		s.chord_gate_done = true;
		cl_fail_at("chord_bound",
				   "the binding table was never scanned after the pad bind: "
				   "no input poll reached player 0, so this run can say "
				   "nothing about the F3 chord");
		return false;
	}
	bool on_device = cl_chord_on_device();
	if(cl_chord_required() && !on_device) {
		s.chord_gate_done = true;
		cl_fail_at("chord_bound",
				   "no F3 chord is bound: this card carries the RELEASE config "
				   "(input.debug_overlay is empty) -- stage "
				   "tools/presets/config_wii_developer.json for this profile");
		return false;
	}
	/* The reading itself is evidence, so a PASSING run carries it too -- on
	 * BOTH platforms. A PC run legitimately records false here (its config
	 * binds a single key, not a chord) and goes on to run the legs that do not
	 * need one; only the console REQUIRES true. */
	if(!cl_mark("p1.chord_bound", true, "\"bound\":%s",
				on_device ? "true" : "false"))
		return false; /* the emit was refused: retry verbatim next frame */
	s.chord_gate_done = true;
	return true;
}

static void cl_chord_p2(const struct chat_login_ctx* ctx, bool fired) {
	/* Both observations are LATCHED across the whole leg: the overlay edge
	 * lands on one frame and the composer would open on another, so a leg that
	 * only looked at its last frame could miss either. */
	if(fired) {
		s.chord_fired = true;
		s.chord_fires++;
	}
	if(ctx->screen_is_chat)
		s.chord_chat_opened = true;
	switch(s.step) {
		case 0: /* the neutral gap every order starts from */
			if(s.frames_in_step < 3 || !ctx->screen_is_ingame)
				return;
			s.chord_fired = false;
			s.chord_fires = 0;
			s.chord_chat_opened = false;
			s.chord_slot_before = cl_hotbar_slot();
			cl_request(s.chord_order == 0 ? CL_CHORD_PLUS : CL_CHORD_MINUS);
			cl_step(1);
			return;
		case 1: /* ... then the OTHER half joins it: one press order */
			cl_request(CL_CHORD_PLUS | CL_CHORD_MINUS);
			cl_step(2);
			return;
		case 2: /* hold the complete chord for a frame */
			if(s.frames_in_step < 2)
				return;
			cl_step(3);
			return;
		case 3: /* release BOTH halves */
			cl_request(0);
			cl_step(4);
			return;
		case 4: {
			/* "right after a chord" is part of the claim, so give the release
			 * frames a chance to open the composer before judging. */
			if(s.frames_in_step < 3)
				return;
			/* Contract 102 (brief 07 A, Astra ruling 3 D2). The press ORDER
			 * decides what this leg must observe, because F3 is specifically
			 * "hold Minus, THEN press Plus":
			 *   order 0 = Plus half first  -> must toggle NOTHING, anywhere;
			 *   order 1 = Minus half first -> must toggle the overlay, on a
			 *             device whose config actually binds the chord.
			 * Either way chat stays shut and the hotbar does not move: Minus
			 * and Plus are the hotbar taps in the shipped CavEX scheme, so a
			 * leaked slot step is the regression this leg exists to catch. */
			int slot_after = cl_hotbar_slot();
			/* Contract 104 (hostile review 17, M-1). This leg is graded by the
			 * COUNT, not by a bool, for the reason P4 already spells out: a
			 * bool accepts three toggles, and three toggles leave the overlay
			 * exactly where one found it. Graded with a bool, a double fire
			 * passed HERE and was rejected four legs later at p3.closed with
			 * "the ruled chord fired but the debug overlay is off" -- a
			 * COUNTING defect diagnosed as a product defect, on the wrong leg,
			 * which is the class of failure that made contract 102 unpassable.
			 * Exactly ONE fire for the ruled order on a device that binds the
			 * chord; exactly ZERO in every other case. */
			unsigned want_fires
				= s.chord_order == 1 && cl_chord_on_device() ? 1u : 0u;
			bool ok = s.chord_fires == want_fires && !s.chord_chat_opened
				&& slot_after == s.chord_slot_before;
			if(!cl_mark("p2.chord", ok,
						"\"order\":%u,\"fired\":%s,\"fires\":%u,"
						"\"chat_opened\":%s,"
						"\"slot_before\":%d,\"slot_after\":%d,\"overlay\":%s",
						s.chord_order, s.chord_fired ? "true" : "false",
						s.chord_fires,
						s.chord_chat_opened ? "true" : "false",
						s.chord_slot_before, slot_after,
						gstate.debug_overlay ? "true" : "false"))
				return;
			if(s.chord_chat_opened) {
				cl_fail("the chord opened the chat composer");
				return;
			}
			if(slot_after != s.chord_slot_before) {
				cl_fail("the chord moved the hotbar");
				return;
			}
			if(s.chord_fires != want_fires) {
				/* Each miss gets its OWN name: too few is the product not
				 * reaching the action, too many on the ruled order is the
				 * counting defect this bump exists for, and any fire at all on
				 * the wrong order is the leg's whole point. */
				cl_fail(s.chord_fires < want_fires
							? "the ruled order (hold Minus, then press Plus) "
							  "never reached IB_DEBUG_OVERLAY"
						: s.chord_order == 0
							? "the WRONG press order (Plus first) toggled the "
							  "debug overlay"
						: want_fires == 0u
							? "a chord toggled the debug overlay on a device "
							  "that binds none"
							: "the ruled order toggled the debug overlay more "
							  "than once");
				return;
			}
			if(s.chord_order == 0) {
				s.chord_order = 1;
				cl_step(0);
				return;
			}
			/* What the ONE legal toggle did, carried into P3/P4: the console's
			 * overlay is now ON exactly when the ruled order fired. */
			s.chord_ruled_fired = s.chord_fired;
			cl_enter(CHAT_LOGIN_P3);
			return;
		}
		default: return;
	}
}

static void cl_chord_p3(const struct chat_login_ctx* ctx) {
	switch(s.step) {
		case 0: /* the neutral re-arm the gesture needs after the chord */
			if(s.frames_in_step < 3 || !ctx->screen_is_ingame)
				return;
			cl_request(CL_BIT(CL_BTN_CHAT));
			cl_step(1);
			return;
		case 1: /* one frame down, released here: a TAP inside the window */
			cl_request(0);
			cl_step(2);
			return;
		case 2:
			if(!ctx->screen_is_chat)
				return;
			if(!cl_mark("p3.opened", true, "\"opened\":true,\"overlay\":%s",
						gstate.debug_overlay ? "true" : "false"))
				return;
			cl_step(3);
			return;
		case 3: /* close what we opened */
			cl_request(CL_BIT(CL_BTN_CANCEL));
			cl_step(4);
			return;
		case 4:
			cl_request(0);
			if(ctx->screen_is_chat)
				return;
			/* Contract 103. Under 102 this asserted "the overlay is OFF",
			 * which was written when BOTH press orders toggled it (two
			 * toggles = as found). Since 102 made the wrong order a NEGATIVE
			 * leg exactly ONE legal toggle has happened, so the truthful
			 * assertion is that the overlay is ON precisely when the ruled
			 * order fired -- and P4 is what puts it back. The run
			 * mp_chat_chord_1m_db01e56_0919_173900 failed here with every
			 * product assertion holding: the leg, not the rule, was wrong. */
			if(!cl_mark("p3.closed", gstate.debug_overlay == s.chord_ruled_fired,
						"\"closed\":true,\"overlay\":%s,\"ruled_fired\":%s",
						gstate.debug_overlay ? "true" : "false",
						s.chord_ruled_fired ? "true" : "false"))
				return;
			if(gstate.debug_overlay != s.chord_ruled_fired) {
				cl_fail(gstate.debug_overlay
							? "the debug overlay is on although the ruled chord "
							  "never fired"
							: "the ruled chord fired but the debug overlay is "
							  "off");
				return;
			}
			cl_enter(CHAT_LOGIN_P4);
			return;
		default: return;
	}
}

/* ---- P4: put the console back the way the run found it ------------------
 *
 * Contract 103 (brief 12 D-1). P2 spends exactly ONE legal toggle, so the
 * profile cannot end after P3 without leaving the debug overlay ON -- which is
 * what the real-Wii run reported. This leg presses the chord ONCE MORE in the
 * RULED order (neutral first, hold Minus, then press Plus), and asserts the
 * three things that make it a restore rather than a second experiment: it
 * fired EXACTLY once (a bool would accept three toggles, which end ON again),
 * it did not open chat or move the hotbar -- Minus and Plus are the hotbar taps
 * -- and the overlay is now OFF. */
static void cl_chord_p4(const struct chat_login_ctx* ctx, bool fired) {
	if(fired)
		s.chord_fires++;
	if(ctx->screen_is_chat)
		s.chord_chat_opened = true;
	switch(s.step) {
		case 0: /* the neutral gap the gesture needs after P3's opener tap */
			if(s.frames_in_step < 3 || !ctx->screen_is_ingame)
				return;
			s.chord_fires = 0;
			s.chord_chat_opened = false;
			s.chord_slot_before = cl_hotbar_slot();
			cl_request(CL_CHORD_MINUS); /* the RULED order: Minus is held ... */
			cl_step(1);
			return;
		case 1:
			cl_request(CL_CHORD_PLUS | CL_CHORD_MINUS); /* ... then Plus joins */
			cl_step(2);
			return;
		case 2: /* hold the complete chord for a frame */
			if(s.frames_in_step < 2)
				return;
			cl_step(3);
			return;
		case 3: /* release BOTH halves */
			cl_request(0);
			cl_step(4);
			return;
		case 4: {
			/* Same settling window P2 gives itself, for the same reason: the
			 * composer would open on a release frame. */
			if(s.frames_in_step < 3)
				return;
			int slot_after = cl_hotbar_slot();
			unsigned want_fires = cl_chord_on_device() ? 1u : 0u;
			bool ok = s.chord_fires == want_fires && !s.chord_chat_opened
				&& slot_after == s.chord_slot_before && !gstate.debug_overlay;
			if(!cl_mark("p4.chord_restore", ok,
						"\"fired\":%s,\"fires\":%u,\"chat_opened\":%s,"
						"\"slot_before\":%d,\"slot_after\":%d,\"overlay\":%s",
						s.chord_fires ? "true" : "false", s.chord_fires,
						s.chord_chat_opened ? "true" : "false",
						s.chord_slot_before, slot_after,
						gstate.debug_overlay ? "true" : "false"))
				return;
			if(s.chord_chat_opened) {
				cl_fail("the restoring chord opened the chat composer");
				return;
			}
			if(slot_after != s.chord_slot_before) {
				cl_fail("the restoring chord moved the hotbar");
				return;
			}
			if(s.chord_fires != want_fires) {
				cl_fail(s.chord_fires < want_fires
							? "the restoring chord never reached IB_DEBUG_OVERLAY"
							: "the restoring chord toggled the debug overlay more "
							  "than once");
				return;
			}
			if(gstate.debug_overlay) {
				cl_fail("the run left the debug overlay on");
				return;
			}
			s.phase = CHAT_LOGIN_OK;
			return;
		}
		default: return;
	}
}

/* ---- the frame entry ---------------------------------------------------- */

void chat_login_frame(const struct chat_login_ctx* ctx) {
	if(!s.armed || !ctx)
		return;
	if(!s.clock_set) {
		s.clock_set = true;
		s.base_ms = ctx->now_ms;
		s.prev_ms = 0;
	}
	uint32_t previous_ms = s.now_ms;
	s.now_ms = ctx->now_ms - s.base_ms; /* unsigned: wrap-safe */
	uint32_t gap = s.now_ms > previous_ms ? s.now_ms - previous_ms : 0;
	if(gap > s.max_frame_ms)
		s.max_frame_ms = gap;
	if(s.trace_recording && gap > s.leg_max_frame_ms)
		s.leg_max_frame_ms = gap;

	if(s.phase == CHAT_LOGIN_FAILED)
		return;
	/* B4a 2b/2f: a PARKED driver (play mode only) runs no phase and spends no
	 * budget -- the play driver owns the clock between two logins. The two
	 * rules that would END a run stay live here, but a parked driver has no
	 * run to end, so they LATCH for chat_login_play_fault instead of
	 * unbinding the pad the caller is still using and reporting a verdict that
	 * is not this module's to give. */
	if(s.phase == CHAT_LOGIN_PARKED) {
		if(!s.play_fault && (s.real_pad_seen || ctx->recovery_modal)) {
			const struct input_player_frame* parked = input_player_frame(0);
			s.last_screen = ctx->screen_id;
			s.last_req = s.want;
			s.last_cp = cl_mask_from(parked, true);
			s.last_cd = cl_mask_from(parked, false);
			s.last_owner_connected = parked && parked->connected;
			s.last_chat_pressed
				= parked && parked->connected && parked->pressed[IB_CHAT];
			s.play_fault = true;
			snprintf(s.play_diag, sizeof(s.play_diag), "%s",
					 cl_state_diag(s.real_pad_seen ? "real pad appeared ch3"
												   : "recovery modal"));
		}
		return;
	}
	/* Section 11 M3: the TOTAL cap is checked before the OK early-return, so a
	 * run that reached OK but cannot get run.ie out still ends with a verdict
	 * instead of idling past its window. */
	/* B4a 2b: the cap is per LOGIN, measured from the arm or from the last
	 * chat_login_play_restart. `login_start_ms` is 0 for the two existing
	 * profiles (nothing ever moves it), so this is byte-for-byte today's
	 * check for them. */
	if(s.now_ms - s.login_start_ms >= cl_total_budget() && !s.done_sent) {
		s.phase = CHAT_LOGIN_IDLE; /* the marker names the run, not a phase */
		cl_fail_at("timeout", cl_state_diag("total timeout")); /* -> run.timeout */
		return;
	}
	if(s.phase == CHAT_LOGIN_OK) {
		/* ... and a run.ok the sender keeps refusing is transport failure, not
		 * a reason to never report: name it and unbind. */
		if(!s.done_sent && ++s.ok_frames > 120u) {
			s.pend_valid = false;
			s.pend_key[0] = 0;
			cl_unbind();
			s.done_sent = true;
			if(s.hooks.profile_done)
				s.hooks.profile_done(s.hooks.user, "run:marker emit blocked");
			return;
		}
		if(!s.done_sent)
			cl_finish_ok();
		return;
	}
	/* Section 11 M5: a real pad appearing on the driver's own channel mid-run
	 * would race the seam for the same inventory slot. Latched by site 3 from
	 * the RAW hardware read, before the substitution. */
	if(s.real_pad_seen) {
		cl_fail_at(cl_awaiting(), cl_state_diag("real pad appeared ch3"));
		return;
	}
	/* Section 11 H1: one stalled frame can hide a missed repeat inside its own
	 * duration, so a trace leg with a gap this large is INCONCLUSIVE. */
	if(s.trace_recording && s.leg_max_frame_ms > CL_LEG_STALL_MS) {
		cl_fail_at(cl_awaiting(), cl_state_diag("frame stall in trace leg"));
		return;
	}
	if(s.phase == CHAT_LOGIN_IDLE)
		cl_enter(CHAT_LOGIN_P0);
	if(s.now_ms - s.phase_start_ms >= cl_phase_budget()) {
		/* A marker still sitting in the retry slot when the budget runs out is
		 * a TRANSPORT failure, not a missing observation. */
		if(s.pend_valid) {
			const char* dot = strchr(s.pend_key, '.');
			char awaited[sizeof(s.pend_key)];
			snprintf(awaited, sizeof(awaited), "%s", dot ? dot + 1 : s.pend_key);
			size_t n = strlen(awaited);
			while(n > 0 && awaited[n - 1] >= '0' && awaited[n - 1] <= '9')
				awaited[--n] = 0;
			cl_fail_at(awaited, cl_state_diag("marker emit blocked"));
		} else
			cl_fail_at(cl_awaiting(), cl_state_diag("phase timeout"));
		return;
	}

	/* Contract section 10 ruling 1: the inventory seam exists so player 0's
	 * owner never reads as ABSENT, and therefore so the controller-recovery
	 * modal never takes the update away from the composer. The ONE sanctioned
	 * window is P3's own disconnect leg, where a platform whose recovery gate
	 * reads the owner frame directly (a PC keyboard owner has no inventory
	 * entry) legitimately raises it and P3 clears it through the production
	 * Resume. Anywhere else the modal means the seam is not working. */
	if(ctx->recovery_modal
	   /* P1's own bind/settle window judges the modal itself (bounded, then
	    * fail-closed on "recovery modal"), and P3's disconnect leg raises it on
	    * purpose where the platform's gate reads the owner frame. */
	   && !(s.phase == CHAT_LOGIN_P1 && s.step <= 2)
	   && !(!s.chord && s.phase == CHAT_LOGIN_P3 && s.step >= 2
			&& s.step <= 6)) {
		cl_fail_at(cl_awaiting(), cl_state_diag("recovery modal"));
		return;
	}

	const struct input_player_frame* owner = input_player_frame(0);
	struct screen_chat_observation obs;
	bool obs_open = screen_chat_observe(&obs);
	/* Latch what this frame really looked like BEFORE any phase runs, so a
	 * timeout raised below reports the state it gave up on. */
	s.last_screen = ctx->screen_id;
	s.last_req = s.want;
	s.last_cp = cl_mask_from(owner, true);
	s.last_cd = cl_mask_from(owner, false);
	s.last_owner_connected = owner && owner->connected;
	s.last_chat_pressed = owner && owner->connected && owner->pressed[IB_CHAT];
	cl_trace_row(owner, obs_open ? &obs : NULL);
	/* Section 11 M1: one record per RENDERED FRAME for the whole leg -- publish
	 * a batch the moment 16 rows are full instead of waiting for the leg to
	 * end, so a long leg keeps its timeline complete. `dropped` therefore only
	 * ever counts frames lost to a batch the sender refused. */
	if(s.trace_count >= CL_TRACE_BATCH && s.trace_key[0])
		(void)cl_trace_flush(s.trace_key);
	s.want_prev = s.want;
	s.frames_in_step++;

	if(s.chord) {
		/* ONCE per frame, before any phase runs: the gate behind it is a state
		 * machine over POLLS, so a phase that read it conditionally would feed
		 * it a hole. */
		bool fired = cl_chord_fire(owner);
		/* E-2 (contract 104): the binding gate runs BEFORE any phase, on the
		 * first frame the bind has produced a reading, and holds every leg
		 * until it has one. See cl_chord_gate. */
		if(!cl_chord_gate())
			return;
		/* P0 and P1 are the login script's own -- the same seam, the same
		 * real-pad refusal, the same OBSERVED binding -- and P2/P3/P4 are this
		 * profile's. */
		switch(s.phase) {
			case CHAT_LOGIN_P0: cl_chord_p0(ctx); break;
			case CHAT_LOGIN_P1: cl_phase_p1(ctx, owner, &obs, obs_open); break;
			case CHAT_LOGIN_P2: cl_chord_p2(ctx, fired); break;
			case CHAT_LOGIN_P3: cl_chord_p3(ctx); break;
			case CHAT_LOGIN_P4: cl_chord_p4(ctx, fired); break;
			default: break;
		}
		return;
	}
	switch(s.phase) {
		case CHAT_LOGIN_P0: cl_phase_p0(ctx); break;
		case CHAT_LOGIN_P1: cl_phase_p1(ctx, owner, &obs, obs_open); break;
		case CHAT_LOGIN_P2: cl_phase_p2(ctx, &obs, obs_open); break;
		case CHAT_LOGIN_P3: cl_phase_p3(ctx, &obs, obs_open); break;
		case CHAT_LOGIN_P4: cl_phase_p4(ctx, &obs, obs_open); break;
		case CHAT_LOGIN_P5: cl_phase_p5(ctx, &obs, obs_open); break;
		case CHAT_LOGIN_P6: cl_phase_p6(ctx); break;
		default: break;
	}
}

/* ---- PLAY MODE: the caller's controls (B4a 2c/2d/2e/2f) -----------------
 *
 * Every one of these refuses -- and changes NOTHING -- unless play mode is
 * armed and the driver is parked, which is what keeps them invisible to
 * mp_chat_login_3m and mp_chat_chord_1m. */

bool chat_login_play_request(const enum input_button* actions, unsigned count) {
	if(!s.armed || !s.play || s.phase != CHAT_LOGIN_PARKED)
		return false;
	/* Validated in full BEFORE anything is written: a refused request must
	 * leave the pad exactly as it was, not half-replaced. An over-long list is
	 * refused rather than truncated -- silently dropping the caller's last
	 * action is the kind of quiet cap this project does not take. */
	if(count > CHAT_LOGIN_PLAY_ACTIONS || (count && !actions))
		return false;
	for(unsigned k = 0; k < count; k++)
		if((int)actions[k] < 0 || (int)actions[k] >= IB_COUNT)
			return false;
	for(unsigned k = 0; k < count; k++)
		s.play_actions[k] = actions[k];
	s.play_count = count; /* count == 0 releases everything */
	return true;
}

/* Clear everything that belongs to ONE login; keep everything that belongs to
 * the PROCESS or to the still-bound pad.
 *
 * Written as save / memset / restore rather than as a list of assignments on
 * purpose: this state struct is large and keeps growing, and a field forgotten
 * in a hand-written clear would leak the FIRST login's observations into the
 * SECOND login's markers -- a stale hold_seen, a stale ring baseline or a
 * stale route index each produce a wrong marker rather than a loud failure.
 * The memset makes "cleared" the default and every survivor is named below. */
static void cl_restart_login(void) {
	struct chat_login_hooks hooks = s.hooks;
	char username[sizeof(s.username)];
	char diag[sizeof(s.play_diag)];
	bool actions_only = s.actions_only;
	int vocab[sizeof(s.vocab) / sizeof(s.vocab[0])];
	unsigned vocab_count = s.vocab_count;
	uint32_t seq = s.seq, base_ms = s.base_ms, now_ms = s.now_ms,
			 prev_ms = s.prev_ms, max_frame_ms = s.max_frame_ms;
	bool clock_set = s.clock_set, publish = s.publish, bound = s.bound,
		 real_pad_seen = s.real_pad_seen, play_fault = s.play_fault;
	memcpy(username, s.username, sizeof(username));
	memcpy(diag, s.play_diag, sizeof(diag));
	memcpy(vocab, s.vocab, sizeof(vocab));

	memset(&s, 0, sizeof(s));

	s.armed = true;
	s.play = true;
	s.actions_only = actions_only;
	s.hooks = hooks;
	memcpy(s.username, username, sizeof(s.username));
	/* The stream's seq is GAPLESS across the whole process, not per login:
	 * the collector's "seq must be 1..N with no holes" rule is what makes a
	 * dropped marker visible, and restarting it would put a second 1 in the
	 * middle of the stream. The ms clock continues for the same reason. */
	s.seq = seq;
	s.clock_set = clock_set;
	s.base_ms = base_ms;
	s.now_ms = now_ms;
	s.prev_ms = prev_ms;
	s.max_frame_ms = max_frame_ms;
	/* The pad survives: still published to the device inventory, still bound
	 * to player 0, still connected -- which is exactly why the restarted
	 * login re-enters PAST P1's publish/bind steps. */
	s.publish = publish;
	s.bound = bound;
	s.pad_connected = true;
	s.vocab_count = vocab_count;
	memcpy(s.vocab, vocab, sizeof(s.vocab));
	/* Run-level latches, not login-level: a real pad that appeared once is
	 * still there, and a fault the caller has not acted on is not erased by
	 * asking for another login. */
	s.real_pad_seen = real_pad_seen;
	s.play_fault = play_fault;
	memcpy(s.play_diag, diag, sizeof(s.play_diag));
	/* The per-login budget starts HERE, so the parked stretch between two
	 * logins is charged to neither of them. */
	s.login_start_ms = now_ms;
}

bool chat_login_play_restart(void) {
	if(!s.armed || !s.play || s.phase != CHAT_LOGIN_PARKED || !s.bound)
		return false;
	cl_restart_login();
	/* The composer must not fall into PC keyboard text mode for the second
	 * login any more than for the first; screen_chat_reset samples this once,
	 * when the composer opens, so it is re-asserted before P1 asks for the
	 * opener. (P1 step 0, which sets it for a first login, is exactly the step
	 * a restart skips.) */
	chat_controller_mode_set(true);
	/* Re-enter at the first step AFTER the bind work. Step 0's real-pad
	 * refusal reads the device inventory, which is ALREADY carrying this
	 * driver's own published pad, so it would refuse against its own
	 * reflection; step 1's bind is already done. A restarted login therefore
	 * emits no p0.session and no p1.pad_bound, and every later marker
	 * unchanged -- stated in chat_login_script.h. */
	/* REL-14b-0: an action-only driver has no login to type; it re-observes
	 * the new generation's session and binding from P0 (see cl_phase_p0). */
	if(s.actions_only) {
		cl_enter(CHAT_LOGIN_P0);
		return true;
	}
	cl_enter(CHAT_LOGIN_P1);
	cl_step(3);
	return true;
}

void chat_login_play_end(void) {
	if(!s.play)
		return;
	cl_unbind();
	s.play_count = 0;
	s.phase = CHAT_LOGIN_IDLE;
	s.armed = false;
	s.play = false;
	/* Nothing may report a verdict for this run: completion belongs to the
	 * play driver, which is the whole reason play mode exists. */
	s.done_sent = true;
}

bool chat_login_play_fault(const char** diag) {
	if(!s.play_fault)
		return false;
	if(diag)
		*diag = s.play_diag;
	return true;
}
