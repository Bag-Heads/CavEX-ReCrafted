/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2. See menu_autopilot_script.h.
*/

#include "menu_autopilot_script.h"
#include "menu_dwell_demo.h"

#include <string.h>

/* Dwell/wait budgets (seconds). Named constants so the commit's rationale
 * for each stays attached to the value instead of scattered as magic
 * numbers. */
#define DWELL_MENU 3.0f		/* liveness-proof dwell on a simple menu screen */
#define DWELL_SIGN 3.0f
#define SETTLE_INGAME 2.0f
#define AIM_HOLD 0.5f /* how long to hold the synthetic pointer before clicking */
#define WAIT_MENU 15.0f		/* menu -> menu transition (screen_set is synchronous; ample margin) */
#define WAIT_WORLD 90.0f	/* create-world submit -> screen_ingame: real worldgen + save I/O */
#define WAIT_SIGN 20.0f
#define WAIT_CONTAINER 6.0f	/* SOFT: aimed empty-hand right-click -> container screen (menu_dwell_demo.h) */
#define WAIT_QUIT 20.0f		/* SOFT: Save and quit -> title (the checkpoint can take seconds on the card) */
#define RELOAD_DWELL 6.0f		/* texture-pack apply: one deferred frame + the reload itself */
/* HARD: bound on STEP_CWORLD_MODE_CONFIRM's retry loop. Each retry attempt is
 * a fresh press+idle pair (2 ticks minimum), so at 20Hz this budgets ~30
 * attempts -- generous for a bug that (per the 2026-09-02 hardware review)
 * needs at most one retry to close a single dropped/late-consumed edge; a
 * selector that STILL never confirms the requested target (Survival by
 * default, Creative under --cw-cworld-mode creative) after this many genuine
 * rising-edge presses is a real regression, not a timing fluke, and the step
 * fails distinctly (STEP_CWORLD_MODE_FAILED) instead of proceeding blind. */
#define WAIT_MODE_SELECT 3.0f
/* HARD bound on the ticks a no-pointer-leg dwell may spend OFF its own screen
 * before the leg abandons. step_dwell zeroes its seconds timer on every such
 * tick, so a screen that keeps being knocked away would otherwise stall the
 * leg with no seconds-based budget ever expiring. Counted in off-screen ticks
 * rather than wall seconds on purpose -- it must never be able to fail a dwell
 * that is actually accruing, at any frame rate. 600 is 10 s at 60 fps and
 * 20 s at 30 fps: far beyond any real screen transition, far short of a
 * hardware run's 240 s. */
#define NP_DWELL_MAX_OFF_TICKS 600
/* Press bounds for the semantic focus walks. Neither is a correctness
 * parameter: no activate is ever issued unless the OBSERVATION says the ring
 * is on the named target that frame, so a bound that is too small costs a
 * named failure and never a wrong press. They exist only so a walk
 * terminates.
 * _CONVERGE covers the capped Join screen. _STRIP is spare route depth for
 * headers, actions, and the Options modal. Select World's budget additionally
 * follows its observed row count because reaching the header from a selected
 * row takes one UP edge per row. */
#define NP_WALK_MAX_CONVERGE 160
#define NP_WALK_MAX_STRIP 8
/* Slack beyond an observed Select World row count for actions and tabs. */
#define NP_WALK_CONVERGE_MARGIN 4
/* Press bound for the observed Create World walk. The approved form ring is
 * header -> Name -> Game Mode -> Seed -> Create -> Details -> footer, and
 * GUI_DOWN converges onto Create from every one of them in at most six
 * presses (the footer wraps to Name). Like the NP bounds this is never a
 * correctness parameter: Create is only ever activated on an observation. */
#define CWORLD_WALK_MAX 8
/* Press bound for STEP_CWORLD_NAV_MODE's observed probe (ui-build-1026 T05).
 * The approved Create screen walks DOWN in Beta's field order, Name -> Seed ->
 * Game mode -> Create world, and the walker has no focus observer for it --
 * but Game mode is the ONLY row on which GUI_RIGHT changes
 * screen_cworld_mode_is_survival(). So the probe alternates DOWN, RIGHT and
 * stops on the first RIGHT that leaves Survival: from a fresh form that is
 * DOWN (Seed), RIGHT (New seed focus, inert), DOWN (Game mode), RIGHT
 * (Creative) = 4 presses. A RIGHT never activates anything; A is never sent. */
#define CWORLD_MODE_WALK_MAX 8
/* THE STALL GUARD (hw1026c follow-up; Sol round-3 review). It measures
 * PROGRESS, not step age: the clock resets on any step change, on any tick
 * the walker presses, and on any change in what it OBSERVES (screen or any
 * focus/view/editor field) -- so a slow walk that is still pressing (the
 * empty-list Edit skip spends 320 ticks in one step: 32 s at 10 fps) never
 * trips it. What remains between resets is pure waiting with nothing moving,
 * and every legitimate such interval is budgeted in WALL SECONDS, so it is
 * the same at 10 fps as at 60: a dwell (<= RELOAD_DWELL 6 s), a screen wait
 * (WAIT_MENU 15 s), or a SOFT wait that skips on timeout (WAIT_SIGN /
 * WAIT_QUIT 20 s). The only tick-counted bound, NP_DWELL_MAX_OFF_TICKS, is
 * itself a failure path (np_fail), never a pass. 30 s clears the 20 s
 * maximum by 50%. STEP_WAIT_INGAME's 90 s WAIT_WORLD keeps its own bound.
 * Before the guard, a dwell whose screen never matched (step_dwell has no
 * timeout of its own) stalled with no marker at all. */
#define STALL_LIMIT 30.0f

/* What the walker can see change, folded to one value per tick. */
static unsigned stall_signature(enum menu_ap_screen screen,
								struct menu_ap_observation obs) {
	unsigned h = 2166136261u;
	const int v[] = {(int)screen, obs.sworld_focus, obs.mps_focus,
		obs.mps_view, obs.mps_rows, obs.sworld_rows, obs.pause_focus,
		obs.title_focus, obs.cworld_editing, obs.cworld_create_ready,
		obs.cworld_mode_survival, obs.confirm_focus_safe,
		obs.texpacks_rows, obs.texpacks_selected, obs.texpacks_focused_row,
		obs.texpacks_apply_focused, obs.texpacks_pending_selected,
		obs.texpacks_busy, obs.cworld_focus, obs.cworld_details_open,
		obs.cworld_osk_profile, obs.cworld_osk_target,
		(int)obs.cworld_text_sig, obs.inv_page, obs.inv_tab};
	for(unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++)
		h = (h ^ (unsigned)v[i]) * 16777619u;
	return h;
}

enum {
	STEP_TITLE_DWELL = 0,
	/* ui-build-1026 T15: the approved Tutorial world row (T03) is VISITED,
	 * focus-checked, and never activated. Its activate loads the SHIPPED
	 * tutorial world (screen_title.c title_start_tutorial) and the walker's
	 * later Save and quit would write that world back to the card -- and the
	 * route after it expects Texture packs, not a loaded world. So the visit
	 * is: an OBSERVED walk onto the row, a dwell during which the row must
	 * STAY focused every tick (the :8099 capture of the new title row), then
	 * the existing walk onward to Texture packs. Inserted at +2, so every
	 * later ordinal moved (menus_dwell contract 139 -> 141; see
	 * tmp/jobs/ui-build-1026/T15-for-T16.md). */
	STEP_TITLE_NAV_TUTORIAL,   /* OBSERVED walk to Tutorial world (Play -> Tutorial today) */
	STEP_TITLE_TUTORIAL_DWELL, /* dwell ON Tutorial, focus re-checked every tick; never activated */
	STEP_TITLE_NAV_MODS,	   /* OBSERVED walk to Texture packs (Tutorial -> Texture packs today) */
	STEP_TITLE_CLICK_MODS,	   /* gate: still on Texture packs; GUI_CLICK -> texture packs */
	STEP_WAIT_TEXPACKS,
	STEP_TEXPACKS_DWELL,
	/* ui-build-1026 T15: ONE pointer click on a pack row selects it and
	 * nothing else (T01/T06). The synthetic pointer is held on the observed
	 * row's card, one click is issued, and the screen must then report that
	 * row selected, the pending choice = that row's pack, the ring on Apply
	 * (or on the row when Apply is disabled because the pick is the current
	 * pack) -- and NO reload. Apply is never pressed; Done/HOME later
	 * discards the pending choice. Every miss ends the run at
	 * STEP_WALK_FAILED naming the step. */
	STEP_TEXPACKS_PICK_AIM,	   /* hold the pointer on texpacks_pick_row's card, then GUI_CLICK once */
	STEP_TEXPACKS_PICK_WAIT,   /* bounded: selected == pick, pending == pick, focus on Apply, no apply */
	STEP_TEXPACKS_PICK_DWELL,  /* capture dwell; the same facts re-checked every tick */
	STEP_TEXPACKS_NAV,		   /* GUI_DOWN once (a no-op if only one pack is installed; still exercises the input) */
	STEP_TEXPACKS_UP_A,		   /* GUI_UP x3: onto row 0 = the bundled default pack, whatever was active */
	STEP_TEXPACKS_APPLY_A,	   /* GUI_CLICK: apply the default (a full reload) */
	STEP_TEXPACKS_RELOAD_A,	   /* dwell across the deferred reload */
	STEP_TEXPACKS_DOWN_B,	   /* GUI_DOWN: row 1 = the first installed pack (earth-and-ash on the card) */
	STEP_TEXPACKS_APPLY_B,	   /* GUI_CLICK: apply it */
	STEP_TEXPACKS_RELOAD_B,
	STEP_TEXPACKS_UP_C,		   /* GUI_UP: back onto the default */
	STEP_TEXPACKS_APPLY_C,	   /* GUI_CLICK: apply the default again, so the card is left as found */
	STEP_TEXPACKS_RELOAD_C,
	STEP_TEXPACKS_LEAVE,	   /* HOME -> back to title */
	STEP_WAIT_TITLE_2,
	STEP_TITLE_DWELL_2,		   /* re-confirm liveness back on title (selection reset to Singleplayer) */
	STEP_TITLE_CLICK_SINGLE,   /* OBSERVED walk to Play, then GUI_CLICK -> select world */
	STEP_WAIT_SWORLD,
	STEP_SWORLD_DWELL,
	STEP_SWORLD_AIM_CREATE,	   /* hold a synthetic pointer on "Create New World", then click it */
	STEP_WAIT_CWORLD,
	STEP_CWORLD_DWELL,
	/* CONSOLE create-world screen (2026-09-01,
	   docs/ui_design/create_world_console_2026-09-01.md): the OSK is a MODAL now,
	   not an always-visible grid, and OSK "OK" commits the field instead of
	   pressing Create. The ring is Name -> Game Mode -> Seed -> Create ->
	   Cancel.

	   The FORM ring runs first, the modal second. Order matters for the capture
	   rig as well as for coverage: on PC input_text_active() is true while
	   the modal has the keyboard, and widget_osk deliberately suppresses the
	   d-pad then (a real keyboard owns the screen), so a PC run parks inside
	   the modal. Exercising the selector BEFORE opening the editor means the
	   PC screenshot rig still reaches the Creative frame. On Wii
	   input_text_active() is always false, so both halves ran there -- until
	   the 09-22 frontend editor, which hears only the player-0 owner frame.
	   Since menus-dwell-walker-1026 the modal half is retired (see the
	   reserved slots below) and the walk to Create is observed. */
	STEP_CWORLD_NAV_MODE,	   /* OBSERVED DOWN/RIGHT probe: Name -> Seed -> Game Mode, until RIGHT flips the mode */
	STEP_CWORLD_MODE_CREATIVE, /* GUI_RIGHT: Survival -> Creative (always, regardless
								  of target -- this hop IS the :8099 capture moment) */
	STEP_CWORLD_MODE_DWELL,	   /* the :8099 CAPTURE MOMENT: selector on Creative, hint bar up */
	STEP_CWORLD_MODE_CONFIRM, /* Verify the selector landed on THIS run's target
								  (menu_ap_state.target_creative, set once by
								  menu_ap_script_reset) before proceeding to
								  Create -- GUI_LEFT back to Survival (the
								  default target; every in-game step below then
								  behaves exactly as it did before the selector
								  existed) or GUI_RIGHT held on Creative
								  (--cw-cworld-mode creative /
								  HW_RUN_PROFILE_MENUS_DWELL_CREATIVE), retried
								  with a fresh rising edge each attempt until
								  cworld_mode_survival confirms the target
								  landed. Bounded by WAIT_MODE_SELECT; see the
								  header's 2026-09-02 design note. */
	STEP_CWORLD_MODE_FAILED,  /* TERMINAL/hard-fail: WAIT_MODE_SELECT elapsed and
								  the requested target never confirmed. A
								  distinct step ordinal so a real-hardware
								  heartbeat's menu_ap_step names exactly which
								  failure this was, instead of silently proceeding
								  into Create with an unconfirmed (wrong-mode)
								  world. */
	/* TERMINAL/hard-fail (menus-dwell-walker-1026): the observed walk to
	 * Create refused -- the editor modal was observed open, the screen left,
	 * the ring never reached an enabled Create within CWORLD_WALK_MAX, or
	 * Create was no longer ready on the activate frame. cworld_failed_step
	 * names which. Takes the ordinal of the retired STEP_CWORLD_NAV_NAME so
	 * every later ordinal -- and the menus_dwell step contract -- is
	 * unchanged. */
	STEP_CWORLD_CREATE_FAILED,
	/* TERMINAL/hard-fail: an OBSERVED focus walk (title -> Tutorial / Texture
	 * packs / Play, the Texture packs one-click pick, pause -> Options / Save
	 * and quit) ran out of presses, saw its screen leave, or found the focus
	 * moved off its target. walk_failed_step names which. (The four reserved
	 * slots of the retired 09-22 name-editor leg that followed it went to the
	 * Create World sub-legs below in T15.) */
	STEP_WALK_FAILED,
	/* ---- ui-build-1026 T15: the Create World SUB-LEGS ------------------
	 * One set of ordinals re-entered per sub-leg (s->cworld_sub names which,
	 * enum menu_ap_cworld_sub): the Details card, the name OSK and the seed
	 * keypad. Each walks the OBSERVED focus to its control (never LEFT/RIGHT
	 * on the Game mode row, which would change the mode), activates it on a
	 * released frame that still reads the target, waits for the observed
	 * card/editor, dwells, types ONE key in an editor (the opening key cell,
	 * proven by the text signature changing), backs out with Close, and must
	 * then read the form again with the ORIGINAL text signature -- Cancel
	 * keeps the old value. Any refusal ends the run at
	 * STEP_CWORLD_CREATE_FAILED with cworld_failed_step = the step and
	 * cworld_sub = the sub-leg. These replace the four reserved slots of the
	 * retired name-editor leg. */
	STEP_CW_SUB_BEGIN,
	STEP_CW_SUB_WALK,		   /* OBSERVED walk onto the sub-leg's control */
	STEP_CW_SUB_ARM,		   /* one released frame; latch the text signature */
	STEP_CW_SUB_ACTIVATE,	   /* gate: still on target; GUI_CLICK */
	STEP_CW_SUB_WAIT_IN,	   /* bounded: the card / the editor with the right profile */
	STEP_CW_SUB_DWELL,		   /* capture dwell, inside re-checked every tick */
	STEP_CW_SUB_KEY,		   /* editors only: gate on a key cell; GUI_CLICK types it */
	STEP_CW_SUB_WAIT_KEY,	   /* bounded: the draft changed */
	STEP_CW_SUB_CLOSE,		   /* Close: closes the card / Cancels the editor */
	STEP_CW_SUB_WAIT_OUT,	   /* bounded: back on the form, on the control, old text */
	STEP_CW_SUB_NEXT,
	STEP_CWORLD_NAV_CREATE,	   /* OBSERVED walk: GUI_DOWN (press + idle) until cworld_create_ready */
	STEP_CWORLD_SUBMIT,		   /* re-observe on a released frame, then GUI_CLICK on Create -> create + load */
	STEP_WAIT_INGAME,
	STEP_INGAME_SETTLE,
	STEP_SIGN_AIM,			   /* face the demo wall sign (MENU_DWELL_AIM_SIGN_*), hold AIM_HOLD */
	STEP_SIGN_USE,			   /* one place pulse = empty-hand right-click on the sign */
	STEP_WAIT_SIGN,			   /* SOFT: see header. Times out to STEP_PAUSE_OPEN, not abandonment. */
	STEP_SIGN_DWELL,
	STEP_SIGN_NAV,			   /* GUI_RIGHT x2: cell navigation */
	STEP_SIGN_EDIT_CHAR,	   /* GUI_UP once: one character edit (SRPC_WINDOW_CLICK) */
	STEP_SIGN_LEAVE,		   /* INVENTORY -> back to screen_ingame */
	STEP_WAIT_INGAME_2,
	/* Inventory leg (2026-09-11): the approved 372x332 inventory panel with
	 * its footer prompts is the screen the maintainer most wants to SEE on
	 * the console; with debug.auto_screenshot armed the Wii photographs it
	 * itself. Deterministic from screen_ingame (no server dependency), so a
	 * failure here is a real regression and ends the run like pause/options. */
	STEP_INVENTORY_OPEN,	   /* INVENTORY (from screen_ingame) -> screen_inventory */
	STEP_WAIT_INVENTORY,
	STEP_INVENTORY_DWELL,
	STEP_INVENTORY_FOCUS_RIGHT,  /* GUI_RIGHT x5: slot focus to the right of the grid -- the portrait turns to follow it (b33e5f3c) */
	STEP_INVENTORY_LOOK_DWELL,   /* hold there so the capture shows the turned portrait */
	/* HOLD leg (b1-chat-opener-0919/16 G-0b): GUI_CLICK picks the focused
	 * slot's stack onto the cursor, a full dwell holds it there so a capture
	 * period lands inside the HELD state (the only state in which the
	 * footer's Drop prompt is drawn at all), and a second GUI_CLICK on the
	 * SAME slot -- no focus press in between -- puts it straight back. */
	STEP_INVENTORY_PICK,
	STEP_INVENTORY_HOLD_DWELL,
	STEP_INVENTORY_PLACE,
	/* ui-build-1026 T15c: the two pages (T11/T12). A TAP of PAGE_SWAP (one
	 * walker press = press, held, release on the next frame -- input.c's
	 * walker level hook) swaps Inventory -> Crafting; PAGE (next) moves the
	 * Crafting page one category tab; a second tap swaps back; the existing
	 * Close leaves from the Inventory page. Each press is gated on the
	 * OBSERVED page, each result waited for with a bound, and GUI_CLICK is
	 * never pressed on the Crafting page (its footer's first action is
	 * Craft). Any miss ends the run at STEP_WALK_FAILED naming the step. */
	STEP_INV_SWAP_TO_CRAFT,	   /* gate page == Inventory; tap PAGE_SWAP */
	STEP_INV_WAIT_CRAFT,	   /* bounded: page == Crafting; latch the tab */
	STEP_INV_CRAFT_DWELL,	   /* capture dwell on the Crafting page */
	STEP_INV_TAB,			   /* gate page == Crafting; PAGE once */
	STEP_INV_WAIT_TAB,		   /* bounded: tab == latched + 1 (mod 8), still Crafting */
	STEP_INV_TAB_DWELL,		   /* capture dwell on the new tab */
	STEP_INV_SWAP_BACK,		   /* gate page == Crafting; tap PAGE_SWAP */
	STEP_INV_WAIT_INV,		   /* bounded: page == Inventory */
	STEP_INVENTORY_LEAVE,	   /* INVENTORY -> back to screen_ingame (container close) */
	STEP_WAIT_INGAME_INV,
	/* Container legs (2026-09-11, menu_dwell_demo.h): the server placed a
	 * furnace, a chest, a double chest and a dispenser on the ground around
	 * spawn; each leg aims the held look at one, requests one place pulse (an
	 * empty-hand right-click = Beta onBlockActivated), dwells on the container
	 * screen and closes it with INVENTORY. SOFT like the sign: a miss skips to
	 * the next leg with its latch unset. */
	STEP_FURNACE_AIM, STEP_FURNACE_USE, STEP_WAIT_FURNACE, STEP_FURNACE_DWELL,
	STEP_FURNACE_LEAVE, STEP_WAIT_INGAME_FURNACE,
	STEP_CHEST_AIM, STEP_CHEST_USE, STEP_WAIT_CHEST, STEP_CHEST_DWELL,
	/* RESERVED, never entered: the chest leg's Sort visit (UP x7 onto the
	 * header Sort entry, open, dwell, close) was removed when the maintainer
	 * parked Sort on 2026-10-25 (a CavEX addition, not Beta 1.7.3); with no
	 * Sort entry its A press picked up a chest stack. The seven slots keep
	 * every later ordinal -- and so the versioned menus_dwell step contract
	 * (tools/wii_telemetry_server.py MENUS_DWELL_STEP_CONTRACT,
	 * tests/test_menus_dwell_contract.c) -- unchanged. */
	STEP_RESERVED_CHEST_1, STEP_RESERVED_CHEST_2, STEP_RESERVED_CHEST_3,
	STEP_RESERVED_CHEST_4, STEP_RESERVED_CHEST_5, STEP_RESERVED_CHEST_6,
	STEP_RESERVED_CHEST_7, STEP_CHEST_LEAVE, STEP_WAIT_INGAME_CHEST,
	STEP_LARGE_AIM, STEP_LARGE_USE, STEP_WAIT_LARGE, STEP_LARGE_DWELL,
	STEP_LARGE_LEAVE, STEP_WAIT_INGAME_LARGE,
	STEP_DISPENSER_AIM, STEP_DISPENSER_USE, STEP_WAIT_DISPENSER,
	STEP_DISPENSER_DWELL, STEP_DISPENSER_LEAVE, STEP_WAIT_INGAME_DISPENSER,
	STEP_PAUSE_OPEN,		   /* HOME (from screen_ingame) -> pause */
	STEP_WAIT_PAUSE,
	STEP_PAUSE_DWELL,
	STEP_PAUSE_NAV_OPTIONS,	   /* OBSERVED walk to Options (Back -> Achievements -> Options since 91bd8295c) */
	STEP_PAUSE_CLICK_OPTIONS, /* gate: still on Options; GUI_CLICK -> options (opened from pause) */
	STEP_WAIT_OPTIONS,
	STEP_OPTIONS_DWELL,
	STEP_OPTIONS_SLIDER,	   /* GUI_RIGHT once on the opening focus: today the Music row's "+" (clamps at 100%) */
	STEP_OPTIONS_LEAVE,	   /* HOME -> back to pause (screen_options_set_return) */
	STEP_WAIT_PAUSE_2,
	STEP_PAUSE_DWELL_2,
	STEP_PAUSE_RESUME,		   /* HOME (from pause) -> resume to screen_ingame */
	STEP_WAIT_INGAME_3,
	STEP_QUIT_OPEN,			   /* HOME -> pause, for Save and quit */
	STEP_WAIT_PAUSE_3,
	STEP_QUIT_NAV,			   /* OBSERVED walk to Save and quit (4 rows down in the full layout today) */
	STEP_QUIT_CLICK,		   /* gate: still on Save and quit; GUI_CLICK: request the unload */
	STEP_WAIT_QUIT,			   /* SOFT: the title within WAIT_QUIT, else the save was refused */
	STEP_TITLE_DWELL_3,
	/* ---- THE NO-POINTER LEG (push-0921 A7 part 1) ----------------------
	 * Re-walks the front end from the title with the pointer asserted
	 * UNAVAILABLE for the leg's whole duration, so the profile proves the
	 * menus are operable by buttons alone rather than merely that they are
	 * operable. It runs LAST -- the quit leg has just unloaded the world, so
	 * the title is genuinely the front end again -- and its terminal step is
	 * the PASS ordinal (menu_ap_script_step_required_for_pass).
	 *
	 * STEP_NP_FAILED is numbered FIRST in the block ON PURPOSE: max_step is a
	 * high-water mark, so a failure step placed after the terminal one would
	 * let an abandoned leg satisfy the very gate it is supposed to guard. */
	STEP_NP_FAILED,			/* TERMINAL: a leg wait ran out; np_failed_step names which */
	STEP_NP_TITLE_DWELL,	/* the leg's opening liveness dwell, suppression on */
	STEP_NP_SWORLD_ACT,		/* gate: title focus on Play (walk if not), then activate */
	STEP_NP_WAIT_SWORLD,
	STEP_NP_SWORLD_DWELL,
	/* ---- the part-2 SUB-LEG WALK (one set of ordinals, re-entered per
	 * sub-leg; np_sub names which one is running) -------------------------
	 * Seven sub-legs -- three on Select World, four on Multiplayer -- share
	 * these steps rather than each getting its own ten. Written out they
	 * would be seventy near-identical cases: unreviewable, and every copy a
	 * place to forget the one rule that matters (never activate unless the
	 * OBSERVATION says the ring is on the named target THIS frame). The
	 * PASS gate is unaffected: the loop only leaves for the leg's tail once
	 * np_sub has passed the last sub-leg, so the terminal ordinal still
	 * cannot be reached without every sub-leg having run or been named as a
	 * skip. */
	STEP_NP_SUB_BEGIN,
	STEP_NP_SUB_TO_STRIP,	/* semantic route to the observed target */
	STEP_NP_SUB_TO_STRIP_DOWN, /* retired ordinal, retained for telemetry stability */
	STEP_NP_SUB_CLAMP,		/* retired ordinal, retained for telemetry stability */
	STEP_NP_SUB_SEEK,		/* retired ordinal, retained for telemetry stability */
	STEP_NP_SUB_ARM,		/* one released frame, then re-observe (R-B: never navigate+activate in one frame) */
	STEP_NP_SUB_ACTIVATE,
	STEP_NP_SUB_WAIT_IN,
	STEP_NP_SUB_DWELL,
	STEP_NP_SUB_CLOSE,
	STEP_NP_SUB_WAIT_OUT,
	STEP_NP_SUB_NEXT,
	STEP_NP_SWORLD_CLOSE,	/* Close -> Cancel -> title (screen_select_world.c:1342) */
	STEP_NP_WAIT_TITLE_A,
	STEP_NP_TEXPACKS_NAV,	/* OBSERVED walk to Texture packs (Play -> Texture packs today) */
	STEP_NP_TEXPACKS_ACT,
	STEP_NP_WAIT_TEXPACKS,
	STEP_NP_TEXPACKS_DWELL,
	/* hw1030i proof gap: the list shows three rows, so a capture never saw
	 * the rest. D-pad DOWN (selection only -- pending/apply untouched) until
	 * the LAST row is selected, which reveals it, then one more capture
	 * dwell. Close below still applies nothing. */
	STEP_NP_TEXPACKS_SCROLL,
	STEP_NP_TEXPACKS_SCROLL_DWELL,
	STEP_NP_TEXPACKS_CLOSE, /* Close -> Done -> title (screen_texturepacks.c:699) */
	STEP_NP_WAIT_TITLE_B,
	STEP_NP_MP_NAV,			/* gate: title focus on Play (walk if not), then activate */
	STEP_NP_MP_ACT,			/* wait for Load, then walk to the Join tab */
	STEP_NP_WAIT_MP,
	STEP_NP_MP_DWELL,
	STEP_NP_MP_CLOSE,		/* Close -> MPS_BUTTON_BACK -> title (screen_multiplayer_setup.c:1609) */
	STEP_NP_WAIT_TITLE_C,
	STEP_NP_TITLE_DWELL_END,
	STEP_NP_LEG_DONE,		/* THE PASS ORDINAL: the whole leg completed */
	STEP_DONE,
	STEP_COUNT,
};

/* Is `step` inside the no-pointer leg? The block is contiguous, so this is
 * the ONE place the leg's extent is stated -- menu_ap_script_tick sets
 * out->pointer_suppressed from it once per tick, before the switch, so no
 * individual step can forget to assert it. */
static bool np_leg_step(int step) {
	return step >= STEP_NP_FAILED && step <= STEP_NP_LEG_DONE;
}

/* ---- the part-2 sub-leg table ------------------------------------------
 * One row per sub-leg, in walk order. `enter_view` is the screen_mps_view()
 * the sub-editor must report (>= 0) or -1 when entry is proven by the SCREEN
 * changing instead. `osk_backed` marks the screens whose Close is owned by a
 * real keyboard wherever input_text_active() can be true: those are walked to
 * the focus and then deliberately NOT entered there, with a named skip.
 * `conditional` marks a control the screen DISABLES without a list row -- an
 * unreachable one is a named skip, not a failure, because the screen is
 * behaving correctly. */
struct np_sub_def {
	unsigned char host;			/* enum menu_ap_screen: which list screen hosts it */
	unsigned char focus_target; /* menu_ap_sworld_focus / menu_ap_mps_focus */
	unsigned char parent_focus; /* Options action that reveals focus_target, or 0 */
	unsigned char enter_screen; /* enum menu_ap_screen expected once entered */
	signed char enter_view;		/* menu_ap_mps_view, or -1 */
	unsigned char osk_backed;
	unsigned char conditional;
};

static const struct np_sub_def np_subs[MENU_AP_SUB_COUNT] = {
	[MENU_AP_SUB_SWORLD_CREATE] = {MENU_AP_SCREEN_SELECT_WORLD,
								   MENU_AP_SWORLD_TAB_CREATE, 0,
								   MENU_AP_SCREEN_CREATE_WORLD, -1, 0, 0},
	[MENU_AP_SUB_SWORLD_RENAME] = {MENU_AP_SCREEN_SELECT_WORLD,
								   MENU_AP_SWORLD_MODAL_RENAME,
								   MENU_AP_SWORLD_OPTIONS,
								   MENU_AP_SCREEN_RENAME_WORLD, -1, 1, 1},
	[MENU_AP_SUB_SWORLD_DELETE] = {MENU_AP_SCREEN_SELECT_WORLD,
								   MENU_AP_SWORLD_MODAL_DELETE,
								   MENU_AP_SWORLD_OPTIONS,
								   MENU_AP_SCREEN_CONFIRM, -1, 0, 1},
	[MENU_AP_SUB_MPS_DIRECT] = {MENU_AP_SCREEN_MULTIPLAYER,
								MENU_AP_MPS_DIRECT, 0, MENU_AP_SCREEN_MULTIPLAYER,
								MENU_AP_MPS_VIEW_DIRECT, 1, 0},
	[MENU_AP_SUB_MPS_ADD] = {MENU_AP_SCREEN_MULTIPLAYER,
							 MENU_AP_MPS_ADD, 0, MENU_AP_SCREEN_MULTIPLAYER,
							 MENU_AP_MPS_VIEW_ADD, 1, 0},
	[MENU_AP_SUB_MPS_EDIT] = {MENU_AP_SCREEN_MULTIPLAYER,
							  MENU_AP_MPS_EDIT, MENU_AP_MPS_OPTIONS,
							  MENU_AP_SCREEN_MULTIPLAYER,
							  MENU_AP_MPS_VIEW_EDIT, 1, 1},
	[MENU_AP_SUB_MPS_NAME] = {MENU_AP_SCREEN_MULTIPLAYER,
							  MENU_AP_MPS_NAME, 0, MENU_AP_SCREEN_MULTIPLAYER,
							  MENU_AP_MPS_VIEW_NAME, 1, 0},
};

/* The sub-legs hosted by each list screen, as a half-open range of the table
 * -- the loop walks [first, end) and then leaves for that screen's Close. */
#define NP_SUB_SWORLD_FIRST MENU_AP_SUB_SWORLD_CREATE
#define NP_SUB_SWORLD_END (MENU_AP_SUB_MPS_DIRECT)
#define NP_SUB_MPS_FIRST MENU_AP_SUB_MPS_DIRECT
#define NP_SUB_MPS_END (MENU_AP_SUB_COUNT)

/* This screen's OBSERVED directional focus, in that screen's own enum. */
static int np_observed_focus(const struct np_sub_def* def,
							  struct menu_ap_observation obs) {
	return def->host == MENU_AP_SCREEN_SELECT_WORLD ? obs.sworld_focus
													 : obs.mps_focus;
}

/* One semantic step toward an approved frontend target. This deliberately
 * follows the screens' public focus meanings instead of reproducing their
 * private indices. Repeated calls converge from every focus the approved
 * Load and Join tabs can expose, and every press is still a real input edge. */
static enum input_button np_route_button(const struct np_sub_def* def,
									 int focus, int target) {
	if(def->host == MENU_AP_SCREEN_SELECT_WORLD) {
		if(focus >= MENU_AP_SWORLD_MODAL_RENAME) {
			if(target < MENU_AP_SWORLD_MODAL_RENAME)
				return IB_INVENTORY;
			/* Rename / Delete / Back is a LINEAR list that CLAMPS (the
			 * approved modal opens on Back): toward the target, never "DOWN
			 * and wrap" -- which pinned the ring on Back until the walk ran
			 * out and both Rename and Delete were recorded as row-gated
			 * skips on a card that HAD a world (hw1026c, np_rows=1). */
			return focus > target ? IB_GUI_UP : IB_GUI_DOWN;
		}
		if(focus == MENU_AP_SWORLD_LIST)
			return IB_GUI_RIGHT; /* row -> Play without walking remembered rows */
		if(focus == MENU_AP_SWORLD_PLAY || focus == MENU_AP_SWORLD_RETRY)
			return target == MENU_AP_SWORLD_OPTIONS ? IB_GUI_RIGHT : IB_GUI_DOWN;
		if(focus == MENU_AP_SWORLD_OPTIONS)
			return IB_GUI_DOWN;
		if(focus == MENU_AP_SWORLD_PREV || focus == MENU_AP_SWORLD_NEXT)
			return IB_GUI_DOWN;
		if(focus >= MENU_AP_SWORLD_TAB_LOAD
		   && focus <= MENU_AP_SWORLD_TAB_JOIN)
			return IB_GUI_RIGHT;
		return IB_GUI_UP;
	}

	if(focus == MENU_AP_MPS_NAME)
		return target == MENU_AP_MPS_NAME ? IB_GUI_UP : IB_GUI_DOWN;
	if((focus == MENU_AP_MPS_EDIT || focus == MENU_AP_MPS_DELETE
		|| focus == MENU_AP_MPS_REFRESH)
	   && target != MENU_AP_MPS_EDIT && target != MENU_AP_MPS_DELETE
	   && target != MENU_AP_MPS_REFRESH && target != MENU_AP_MPS_BACK)
		return IB_INVENTORY;
	if(target == MENU_AP_MPS_NAME)
		return IB_GUI_UP;
	if(focus == MENU_AP_MPS_LIST)
		return target == MENU_AP_MPS_NAME ? IB_GUI_UP : IB_GUI_RIGHT;
	if(focus >= MENU_AP_MPS_TAB_LOAD && focus <= MENU_AP_MPS_TAB_JOIN)
		return target == MENU_AP_MPS_NAME ? IB_GUI_UP : IB_GUI_DOWN;
	if(focus == MENU_AP_MPS_JOIN)
		return target == MENU_AP_MPS_OPTIONS ? IB_GUI_RIGHT : IB_GUI_LEFT;
	if(focus == MENU_AP_MPS_DIRECT)
		return target == MENU_AP_MPS_ADD ? IB_GUI_LEFT : IB_GUI_RIGHT;
	if(focus == MENU_AP_MPS_ADD)
		return IB_GUI_RIGHT;
	/* The Options modal is a linear Edit/Delete/Refresh/Back list; walk
	 * TOWARD the target (a clamping list never wraps -- the Select World
	 * modal's hw1026c lesson), which converges whether it clamps or wraps. */
	if(focus == MENU_AP_MPS_EDIT || focus == MENU_AP_MPS_DELETE
	   || focus == MENU_AP_MPS_REFRESH) {
		if(target != MENU_AP_MPS_EDIT && target != MENU_AP_MPS_DELETE
		   && target != MENU_AP_MPS_REFRESH && target != MENU_AP_MPS_BACK)
			return IB_INVENTORY;
		return focus > target ? IB_GUI_UP : IB_GUI_DOWN;
	}
	return IB_GUI_RIGHT;
}

/* How many presses the walk onto this sub-leg's host ring may spend.
 *
 * For the Select World row list the answer is OBSERVED, not guessed: the
 * screen publishes its row count (screen_sworld_row_count -> obs.sworld_rows)
 * and the DOWN walk costs one press per row plus one to leave the list. For
 * everything else -- the Multiplayer screen, whose list is capped and has no
 * row-count observer, and the UP approach onto the player-name field -- the
 * fixed fallback applies. A count that arrives short (the list resolves its
 * rows over several frames) only shortens the budget for the frames it is
 * short on; the walk re-reads it every tick, and the leg's own count latch
 * (np_rows) is taken after a three-second dwell for the same reason. */
static int np_converge_budget(const struct np_sub_def* def,
							   struct menu_ap_observation obs) {
	if(def->host != MENU_AP_SCREEN_SELECT_WORLD)
		return NP_WALK_MAX_CONVERGE;
	int rows = obs.sworld_rows > 0 ? obs.sworld_rows : 0;
	return rows + NP_WALK_CONVERGE_MARGIN;
}

/* The activate-forbidden set, stated as the lead ruled it rather than left
 * implicit in "we only press on the named target": these are the controls
 * whose activate LOADS a world or JOINS a server. The walk can rest on them
 * (the ring clamps onto Select / Join on its way to the right), it may just
 * never press activate there. */
static bool np_focus_forbidden(const struct np_sub_def* def, int focus) {
	if(def->host == MENU_AP_SCREEN_SELECT_WORLD)
		return focus == MENU_AP_SWORLD_LIST || focus == MENU_AP_SWORLD_SELECT;
	return focus == MENU_AP_MPS_LIST || focus == MENU_AP_MPS_JOIN
		|| focus == MENU_AP_MPS_QUICK;
}

/* Has the sub-leg's destination been reached? An MPS sub-editor is proven by
 * screen_mps_view() (the screen pointer does not change); everything else by
 * the screen itself. */
static bool np_sub_inside(const struct np_sub_def* def,
						   enum menu_ap_screen screen,
						   struct menu_ap_observation obs) {
	if(screen != (enum menu_ap_screen)def->enter_screen)
		return false;
	return def->enter_view < 0 || obs.mps_view == def->enter_view;
}

/* Back out on the host screen, with its sub-editor closed. */
static bool np_sub_outside(const struct np_sub_def* def,
							enum menu_ap_screen screen,
							struct menu_ap_observation obs) {
	if(screen != (enum menu_ap_screen)def->host)
		return false;
	return def->host != MENU_AP_SCREEN_MULTIPLAYER
		|| obs.mps_view == MENU_AP_MPS_VIEW_MAIN;
}

/* ---- ui-build-1026 T15: the Create World sub-leg table ------------------ */
struct cw_sub_def {
	int target;	 /* enum menu_ap_cworld_focus to activate */
	int profile; /* the OSK profile the editor must report, or 0 = the Details card */
};
static const struct cw_sub_def cw_subs[MENU_AP_CWORLD_SUB_COUNT] = {
	[MENU_AP_CWORLD_SUB_DETAILS] = {MENU_AP_CWORLD_DETAILS, 0},
	[MENU_AP_CWORLD_SUB_NAME_OSK] = {MENU_AP_CWORLD_NAME,
									 MENU_AP_OSK_PROFILE_CREATE_NAME},
	[MENU_AP_CWORLD_SUB_SEED_OSK] = {MENU_AP_CWORLD_SEED,
									 MENU_AP_OSK_PROFILE_CREATE_SEED},
};

/* The approved form's rows: Name / Seed + New seed / Game mode / Create +
 * Details + Cancel; -1 for anything else (the Details card's Back). */
static int cw_row(int focus) {
	switch(focus) {
		case MENU_AP_CWORLD_NAME: return 0;
		case MENU_AP_CWORLD_SEED:
		case MENU_AP_CWORLD_NEW_SEED: return 1;
		case MENU_AP_CWORLD_MODE_SURVIVAL:
		case MENU_AP_CWORLD_MODE_CREATIVE: return 2;
		case MENU_AP_CWORLD_CREATE:
		case MENU_AP_CWORLD_DETAILS:
		case MENU_AP_CWORLD_CANCEL: return 3;
		default: return -1;
	}
}

/* One step toward `target`: UP/DOWN between rows (every target is reached
 * without the ring's wrap), LEFT/RIGHT only within the target's OWN row --
 * never the Game mode row, so the walk cannot change the mode. -1 = no safe
 * press (an unknown focus). */
static int cw_route(int focus, int target) {
	int from = cw_row(focus), to = cw_row(target);
	if(from < 0 || to < 0)
		return -1;
	if(from != to)
		return to < from ? IB_GUI_UP : IB_GUI_DOWN;
	return focus > target ? IB_GUI_LEFT : IB_GUI_RIGHT;
}

/* Is the sub-leg's card / editor up, as itself? */
static bool cw_sub_inside(const struct cw_sub_def* def,
						  enum menu_ap_screen screen,
						  struct menu_ap_observation obs) {
	if(screen != MENU_AP_SCREEN_CREATE_WORLD)
		return false;
	if(!def->profile)
		return obs.cworld_details_open && !obs.cworld_editing
			&& obs.cworld_focus == MENU_AP_CWORLD_DETAILS_BACK;
	return obs.cworld_editing && !obs.cworld_details_open
		&& obs.cworld_osk_profile == def->profile;
}

void menu_ap_script_reset(struct menu_ap_state* s, bool target_creative) {
	if(!s)
		return;
	memset(s, 0, sizeof(*s));
	s->step = STEP_TITLE_DWELL;
	s->texpacks_pick_target = -1;
	s->inv_tab_before = -1;
	s->target_creative = target_creative;
	s->np_rows = -1; /* memset leaves 0, which is a REAL row count */
	s->np_failed_sub = -1;
}

bool menu_ap_script_finished(const struct menu_ap_state* s) {
	return !s || s->finished;
}

int menu_ap_script_step_count(void) {
	return STEP_COUNT;
}

int menu_ap_script_step_awaiting_sign(void) {
	return STEP_WAIT_SIGN;
}

int menu_ap_script_step_cworld_entry(void) {
	return STEP_WAIT_CWORLD;
}

int menu_ap_script_step_wait_ingame(void) {
	return STEP_WAIT_INGAME;
}

/* ui-build-1026 T05: the observed Game-mode probe's ordinal, so the script
 * test can name it as a cworld_failed_step gate. */
int menu_ap_script_step_cworld_nav_mode(void) {
	return STEP_CWORLD_NAV_MODE;
}

int menu_ap_script_step_title_tutorial_dwell(void) {
	return STEP_TITLE_TUTORIAL_DWELL;
}

int menu_ap_script_step_texpacks_pick(void) {
	return STEP_TEXPACKS_PICK_AIM;
}

int menu_ap_script_step_cworld_sub_begin(void) {
	return STEP_CW_SUB_BEGIN;
}

int menu_ap_script_step_inv_pages(void) {
	return STEP_INV_SWAP_TO_CRAFT;
}

int menu_ap_script_step_cworld_failed(void) {
	return STEP_CWORLD_CREATE_FAILED;
}

int menu_ap_script_step_walk_failed(void) {
	return STEP_WALK_FAILED;
}

int menu_ap_script_step_pause_entry(void) {
	return STEP_PAUSE_OPEN;
}

int menu_ap_script_step_wait_options(void) {
	return STEP_WAIT_OPTIONS;
}

/* ---- OBSERVED focus walks (menus-dwell-walker-1026 follow-up) ----------
 * Fixed press counts broke twice in one month (the 09-22 create-world editor,
 * the 10-05 pause Achievements row). A walk instead reads the REAL screen's
 * focus every tick and presses one step toward `target` (DOWN when below it
 * or unknown, UP when past it -- every walked list is linear and clamped),
 * with one released tick after each press so every press is a fresh rising
 * edge and the next decision reads THAT press's result. The bound is not a
 * correctness parameter: nothing is ever activated without an observation. */
#define FOCUS_WALK_MAX MENU_AP_FOCUS_WALK_MAX
enum walk_result { WALK_PENDING, WALK_REACHED, WALK_FAILED };

int menu_ap_focus_walk_button(int focus, int target) {
	if(focus == target)
		return -1;
	return focus > target ? IB_GUI_UP : IB_GUI_DOWN;
}

static enum walk_result step_walk(struct menu_ap_state* s,
								   struct menu_ap_output* out, bool on_screen,
								   int focus, int target) {
	if(!on_screen)
		return WALK_FAILED;
	if(s->repeat_phase) {
		s->repeat_phase = false;
		s->repeat_done++;
		return WALK_PENDING;
	}
	if(focus == target)
		return WALK_REACHED;
	if(s->repeat_done >= FOCUS_WALK_MAX)
		return WALK_FAILED;
	out->press = true;
	out->button = (enum input_button)menu_ap_focus_walk_button(focus, target);
	s->repeat_phase = true;
	return WALK_PENDING;
}

/* End the run at STEP_WALK_FAILED, naming the refusing step. */
static void walk_fail(struct menu_ap_state* s, int failed_step) {
	s->walk_failed_step = failed_step;
	s->step = STEP_WALK_FAILED;
	s->timer = 0.0f;
	s->repeat_done = 0;
	s->repeat_phase = false;
	s->finished = true;
}

/* End the run at the named Create World failure, recording which gate refused
 * (see STEP_CWORLD_CREATE_FAILED). */
static void cworld_fail(struct menu_ap_state* s, int failed_step) {
	s->cworld_failed_step = failed_step;
	s->step = STEP_CWORLD_CREATE_FAILED;
	s->timer = 0.0f;
	s->repeat_done = 0;
	s->repeat_phase = false;
	s->finished = true;
}

void menu_ap_script_enable_pack_legs(struct menu_ap_state* s, bool on) {
	s->pack_legs = on;
}

void menu_ap_script_set_text_input_owns_close(struct menu_ap_state* s, bool on) {
	s->text_input_owns_close = on;
}

int menu_ap_script_step_required_for_pass(void) {
	/* Was STEP_WAIT_INGAME_3 (title, texture-packs, select-world,
	 * create-world's focus ring + modal OSK + game-mode selector, pause and
	 * options all round-tripped). push-0921 A7 part 1 moves the gate to the
	 * END of the NO-POINTER leg: everything the old gate proved is still on
	 * the only path to it, and the run must now ALSO have re-walked the front
	 * end with the pointer suppressed. A leg that abandons lands on
	 * STEP_NP_FAILED, which is numbered below this, so it cannot pass.
	 *
	 * The sign step is still deliberately NOT required (see header). */
	return STEP_NP_LEG_DONE;
}

static void goto_step(struct menu_ap_state* s, int next) {
	s->step = next;
	s->stall_timer = 0.0f;
	s->timer = 0.0f;
	s->repeat_done = 0;
	s->repeat_phase = false;
	if(next > s->max_step)
		s->max_step = next;
}

void menu_ap_script_start_at_np_leg(struct menu_ap_state* s) {
	if(!s)
		return;
	/* The leg's own first step, reached the way every other step is reached so
	 * max_step is the honest high-water mark of where the script actually is
	 * -- a lever run's max_step counts only the leg, and the flag below is
	 * what stops that from ever reading as a completed script. */
	goto_step(s, STEP_NP_TITLE_DWELL);
	/* Raised HERE, before the first tick, rather than inside the leg: the very
	 * first heartbeat of a lever run must already carry it, so no tape can
	 * exist that was produced by the lever and does not say so. */
	s->np_flags |= MENU_AP_NP_FLAG_ENTRY_LEVER;
}

/* Dwell `seconds` while `screen == want`; the timer resets to 0 (no credit)
 * on any tick the screen does not match, so a dwell knocked off its screen
 * can never be reported as having proven liveness there. */
static bool step_dwell(struct menu_ap_state* s, enum menu_ap_screen screen,
						enum menu_ap_screen want, float dt, float seconds) {
	if(screen != want) {
		s->timer = 0.0f;
		return false;
	}
	s->timer += dt;
	return s->timer >= seconds;
}

enum wait_result { WAIT_PENDING, WAIT_REACHED, WAIT_TIMEOUT };

/* Wait (bounded) for `screen == want`; used after every step whose press can
 * trigger a screen_set(). */
static enum wait_result step_wait(struct menu_ap_state* s,
								   enum menu_ap_screen screen,
								   enum menu_ap_screen want, float dt,
								   float timeout_s) {
	if(screen == want)
		return WAIT_REACHED;
	s->timer += dt;
	return s->timer >= timeout_s ? WAIT_TIMEOUT : WAIT_PENDING;
}

/* Press `button` `target` times, with one idle tick between presses so each
 * is a fresh rising edge (input_pressed requires prev=false, cur=true --
 * holding a synthesized press for two consecutive ticks would only ever
 * register as one). Returns true once `target` presses have been issued. */
static bool step_press_repeat(struct menu_ap_state* s,
							   struct menu_ap_output* out,
							   enum input_button button, int target) {
	if(s->repeat_done >= target)
		return true;
	if(!s->repeat_phase) {
		out->press = true;
		out->button = button;
		s->repeat_phase = true;
	} else {
		s->repeat_phase = false;
		s->repeat_done++;
	}
	return false;
}

/* ---- no-pointer-leg helpers (push-0921 A7 part 1) ---------------------- */
/* Abandon the leg, recording WHICH bounded wait ran out. One shared terminal
 * step keeps the enum small; np_failed_step is what names the failure, since
 * a single ordinal in a heartbeat cannot. Because STEP_NP_FAILED sits BELOW
 * every other leg step, goto_step's high-water mark cannot be pushed past the
 * pass ordinal by failing. */
static void np_fail(struct menu_ap_state* s, int failed_step) {
	s->np_failed_step = failed_step;
	/* The walk's step ordinals are shared by all seven sub-legs, so the step
	 * alone cannot say WHICH one died -- record the sub-leg with it. Only for
	 * a failure INSIDE the walk, though: s->np_sub still holds the last
	 * sub-leg that ran, and reporting that against (say) a Multiplayer Close
	 * that would not leave the screen would name a sub-leg that had already
	 * finished successfully. -1 = "not in a sub-leg". */
	s->np_failed_sub = (failed_step >= STEP_NP_SUB_BEGIN
						&& failed_step <= STEP_NP_SUB_NEXT)
		? s->np_sub
		: -1;
	s->np_flags |= MENU_AP_NP_FLAG_LEG_FAILED;
	goto_step(s, STEP_NP_FAILED);
}

/* A bounded wait inside the leg: reached -> `on_reached`, timed out -> abandon
 * naming THIS step. Every leg transition goes through it, so "bounded, and a
 * timeout fails the leg by name" is structural rather than per-step
 * discipline. */
static void np_wait(struct menu_ap_state* s, enum menu_ap_screen screen,
					 enum menu_ap_screen want, float dt, int this_step,
					 int on_reached) {
	switch(step_wait(s, screen, want, dt, WAIT_MENU)) {
		case WAIT_REACHED: goto_step(s, on_reached); break;
		case WAIT_TIMEOUT: np_fail(s, this_step); break;
		case WAIT_PENDING: break;
	}
}

/* np_wait, for a gate that is a PREDICATE rather than a screen id -- a
 * Multiplayer sub-editor is entered and left without the screen pointer ever
 * changing, so "am I inside" is screen_mps_view()'s answer, not screen's. */
static void np_wait_for(struct menu_ap_state* s, bool reached, float dt,
						 int this_step, int on_reached) {
	if(reached) {
		goto_step(s, on_reached);
		return;
	}
	s->timer += dt;
	if(s->timer >= WAIT_MENU)
		np_fail(s, this_step);
}

/* A leg dwell: the SAME screen-gated dwell the rest of the script uses (a
 * tick off-screen resets the timer, so a dwell knocked off its screen earns
 * no credit), with the leg's own fail-by-name on the way out, and a HARD tick
 * bound (NP_DWELL_MAX_TICKS) on top so a screen that keeps being knocked away
 * mid-dwell cannot stall the leg forever. */
static void np_dwell(struct menu_ap_state* s, enum menu_ap_screen screen,
					  enum menu_ap_screen want, float dt, int this_step,
					  int next) {
	if(step_dwell(s, screen, want, dt, DWELL_MENU)) {
		goto_step(s, next);
		return;
	}
	/* step_dwell owns s->timer and zeroes it on every tick the screen does
	 * NOT match, so only those ticks can stall forever -- and only those are
	 * counted here. repeat_done is unused on a dwell step and goto_step
	 * clears it, so it carries the budget. Counting the OFF-screen ticks
	 * alone (rather than every tick) is what keeps a legitimate dwell safe at
	 * any frame rate: a faster frame rate buys more dwell ticks, it never
	 * brings the failure closer. */
	if(screen != want && ++s->repeat_done > NP_DWELL_MAX_OFF_TICKS)
		np_fail(s, this_step);
}

/* ---- the part-2 focus walk --------------------------------------------
 * Press `button` once, leave the next frame RELEASED, then look again --
 * repeat until the OBSERVED focus satisfies `done`, or the press budget runs
 * out. The released frame is not politeness: the activation rule's R-B makes
 * a frame that carries a navigation edge decide NOTHING and consume the
 * press, so a direction and an activate must never share a frame, and the
 * SEEK -> ARM -> ACTIVATE sequence below leaves two released frames between
 * the last direction and the press. */
enum np_walk_result { NP_WALK_PENDING, NP_WALK_DONE, NP_WALK_EXHAUSTED };

static enum np_walk_result np_walk(struct menu_ap_state* s,
									struct menu_ap_output* out, bool done,
									enum input_button button, int max_presses) {
	if(done)
		return NP_WALK_DONE;
	if(s->repeat_done >= max_presses)
		return NP_WALK_EXHAUSTED;
	if(!s->repeat_phase) {
		out->press = true;
		out->button = button;
		s->repeat_phase = true;
	} else {
		s->repeat_phase = false;
		s->repeat_done++;
	}
	return NP_WALK_PENDING;
}

/* Record a sub-leg outcome. The three masks are deliberately separate: "the
 * ring reached it", "it was entered and left" and "it was skipped, and why"
 * are different claims and a run must not be able to blur them. */
static void np_sub_mark(struct menu_ap_state* s, unsigned* mask) {
	*mask |= 1u << (unsigned)s->np_sub;
}

/* Close the screen the leg is on and wait for the title. The Close role is
 * the ONLY way out the leg uses -- never IB_HOME, whose own behaviour Astra
 * Lane A item 3 keeps untouched, and never an activate, which on either list
 * screen would be rule (3) ACTIVATE(selected) on a list row. */
static void np_close(struct menu_ap_state* s, struct menu_ap_output* out,
					  int next) {
	out->press = true;
	out->button = IB_INVENTORY;
	goto_step(s, next);
}

/* ---- container-leg helpers (2026-09-11) -------------------------------- */
/* Face a demo target and hold the look for AIM_HOLD so the crosshair ray has
 * settled on it before the pulse (the camera applies the aim on its next frame). */
static void container_aim(struct menu_ap_state* s, struct menu_ap_output* out,
						  float dt, int yaw_deg, int pitch_deg, int next) {
	out->aim_set = true;
	out->aim_yaw_deg = yaw_deg;
	out->aim_pitch_deg = pitch_deg;
	s->timer += dt;
	if(s->timer >= AIM_HOLD)
		goto_step(s, next);
}

/* SOFT wait for the container screen the pulse should have opened: reached =
 * latch + dwell; timeout = skip to the next leg (the miss is visible in the
 * capture frames and the unset latch, never a run failure). */
static void container_wait(struct menu_ap_state* s, enum menu_ap_screen screen,
						   float dt, enum menu_ap_screen target, bool* latch,
						   int on_reached, int on_timeout) {
	switch(step_wait(s, screen, target, dt, WAIT_CONTAINER)) {
		case WAIT_REACHED:
			*latch = true;
			goto_step(s, on_reached);
			break;
		case WAIT_TIMEOUT: goto_step(s, on_timeout); break;
		case WAIT_PENDING: break;
	}
}

/* Close a container with INVENTORY (container_menu_read_input .close). If we
 * are already back in-game (a leg entered from a soft skip) press nothing: INVENTORY in-game would OPEN the inventory. */
static void container_leave(struct menu_ap_state* s, struct menu_ap_output* out,
							enum menu_ap_screen screen, int next) {
	if(screen != MENU_AP_SCREEN_INGAME) {
		out->press = true;
		out->button = IB_INVENTORY;
	}
	goto_step(s, next);
}

/* Bounded wait to be back in-game between legs; a container that will not
 * close is a real regression, so it ends the run like the pause/options legs. */
static void container_wait_ingame(struct menu_ap_state* s,
								  enum menu_ap_screen screen, float dt, int next) {
	switch(step_wait(s, screen, MENU_AP_SCREEN_INGAME, dt, WAIT_MENU)) {
		case WAIT_REACHED: goto_step(s, next); break;
		case WAIT_TIMEOUT: s->finished = true; break;
		case WAIT_PENDING: break;
	}
}

bool menu_ap_script_tick(struct menu_ap_state* s, enum menu_ap_screen screen,
						  struct menu_ap_observation obs, float dt,
						  struct menu_ap_output* out) {
	struct menu_ap_output zero = {0};
	if(!s || !out)
		return false;
	*out = zero;
	bool cworld_mode_survival = obs.cworld_mode_survival;
	/* The row the sub-leg walk is on. Valid only while s->np_sub is in range,
	 * which every case that reads it checks by being inside the walk. */
	const struct np_sub_def* sub
		= (s->np_sub >= 0 && s->np_sub < MENU_AP_SUB_COUNT)
		? &np_subs[s->np_sub]
		: &np_subs[0];

	if(screen == MENU_AP_SCREEN_SIGN)
		s->sign_reached = true;

	if(s->finished)
		return false;
	if(obs.front_end_blocked) /* the cards own input: hold, inert */
		return true;
	const int step_at_entry = s->step;

	/* THE NO-POINTER ASSERTION, decided ONCE per tick from the step this tick
	 * runs on (A7 part 1a). Set here rather than inside each leg case so no
	 * step -- including a dwell or a bounded wait, which emit no press at all
	 * -- can forget it, and so the leg's "suppressed for its WHOLE duration"
	 * claim is a property of the step table instead of per-case discipline.
	 * A finished script returns above with it false: the suppression can
	 * never outlive the run. */
	out->pointer_suppressed = np_leg_step(s->step);

	switch(s->step) {
		case STEP_TITLE_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TITLE, dt, DWELL_MENU))
				goto_step(s, STEP_TITLE_NAV_TUTORIAL);
			break;
		case STEP_TITLE_NAV_TUTORIAL:
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_TITLE,
							 obs.title_focus, MENU_AP_TITLE_TUTORIAL)) {
				case WALK_REACHED: goto_step(s, STEP_TITLE_TUTORIAL_DWELL); break;
				case WALK_FAILED: walk_fail(s, STEP_TITLE_NAV_TUTORIAL); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_TITLE_TUTORIAL_DWELL:
			/* No press at all: the row is held under the ring for a full
			 * dwell. The screen leaving, or the focus moving off Tutorial on
			 * ANY tick, is a named failure -- a dwell that silently re-earned
			 * its credit elsewhere would photograph the wrong row. Activating
			 * here is deliberately impossible (see the step table). */
			if(screen != MENU_AP_SCREEN_TITLE
			   || obs.title_focus != MENU_AP_TITLE_TUTORIAL) {
				walk_fail(s, STEP_TITLE_TUTORIAL_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= DWELL_MENU) {
				s->title_tutorial_seen = true;
				goto_step(s, STEP_TITLE_NAV_MODS);
			}
			break;
		case STEP_TITLE_NAV_MODS:
			/* Approved title: Play / Tutorial world / Texture packs /
			 * Options / Quit (ui-build-1026 T03). Starts on Tutorial since
			 * T15, so this is normally one DOWN. */
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_TITLE,
							 obs.title_focus, MENU_AP_TITLE_PACKS)) {
				case WALK_REACHED: goto_step(s, STEP_TITLE_CLICK_MODS); break;
				case WALK_FAILED: walk_fail(s, STEP_TITLE_NAV_MODS); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_TITLE_CLICK_MODS:
			if(screen != MENU_AP_SCREEN_TITLE
			   || obs.title_focus != MENU_AP_TITLE_PACKS) {
				walk_fail(s, STEP_TITLE_CLICK_MODS);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_WAIT_TEXPACKS);
			break;
		case STEP_WAIT_TEXPACKS:
			switch(step_wait(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_TEXPACKS_DWELL); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_TEXPACKS_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt,
						  DWELL_MENU))
				goto_step(s, STEP_TEXPACKS_PICK_AIM);
			break;
		case STEP_TEXPACKS_PICK_AIM:
			/* The row is latched ONCE, from the caller's observation of which
			 * card is fully visible; the click lands only after AIM_HOLD of a
			 * still pointer, like STEP_SWORLD_AIM_CREATE. Runs BEFORE any
			 * direction press on this screen, so the ring's origin is still
			 * the reset default and the click decides by the sprite. */
			if(screen != MENU_AP_SCREEN_TEXTUREPACKS || obs.texpacks_busy) {
				walk_fail(s, STEP_TEXPACKS_PICK_AIM);
				break;
			}
			if(s->texpacks_pick_target < 0)
				s->texpacks_pick_target = obs.texpacks_pick_row;
			if(s->texpacks_pick_target < 0
			   || s->texpacks_pick_target >= obs.texpacks_rows) {
				walk_fail(s, STEP_TEXPACKS_PICK_AIM);
				break;
			}
			out->pointer_target = MENU_AP_PT_TEXPACKS_ROW;
			out->pointer_row = s->texpacks_pick_target;
			s->timer += dt;
			if(s->timer >= AIM_HOLD) {
				out->press = true;
				out->button = IB_GUI_CLICK;
				goto_step(s, STEP_TEXPACKS_PICK_WAIT);
			}
			break;
		case STEP_TEXPACKS_PICK_WAIT: {
			/* The pointer stays on the card through the wait (the input skew
			 * STEP_WAIT_CWORLD documents). A reload starting is an Apply the
			 * walker never asked for: a named failure, at once. */
			if(screen != MENU_AP_SCREEN_TEXTUREPACKS || obs.texpacks_busy) {
				walk_fail(s, STEP_TEXPACKS_PICK_WAIT);
				break;
			}
			bool picked = obs.texpacks_selected == s->texpacks_pick_target
				&& obs.texpacks_pending_selected
				&& (obs.texpacks_apply_enabled
						? obs.texpacks_apply_focused
						: obs.texpacks_focused_row == s->texpacks_pick_target);
			if(picked) {
				goto_step(s, STEP_TEXPACKS_PICK_DWELL);
				break;
			}
			out->pointer_target = MENU_AP_PT_TEXPACKS_ROW;
			out->pointer_row = s->texpacks_pick_target;
			s->timer += dt;
			if(s->timer >= WAIT_MENU)
				walk_fail(s, STEP_TEXPACKS_PICK_WAIT);
			break;
		}
		case STEP_TEXPACKS_PICK_DWELL:
			/* No pointer and no press: the pick must simply HOLD -- still
			 * selected, still pending, and never applied. */
			/* ...and the ring stays where the pick put it, every tick: on
			 * Apply when Apply is enabled, else on the picked row (Sol T15
			 * review: a ring drifting to another strip button must not earn
			 * texpacks_pick_seen). */
			if(screen != MENU_AP_SCREEN_TEXTUREPACKS || obs.texpacks_busy
			   || obs.texpacks_selected != s->texpacks_pick_target
			   || !obs.texpacks_pending_selected
			   || !(obs.texpacks_apply_enabled
						? obs.texpacks_apply_focused
						: obs.texpacks_focused_row == s->texpacks_pick_target)) {
				walk_fail(s, STEP_TEXPACKS_PICK_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= DWELL_MENU) {
				s->texpacks_pick_seen = true;
				goto_step(s, STEP_TEXPACKS_NAV);
			}
			break;
		case STEP_TEXPACKS_NAV:
			out->press = true;
			out->button = IB_GUI_DOWN;
			goto_step(s, s->pack_legs ? STEP_TEXPACKS_UP_A : STEP_TEXPACKS_LEAVE);
			break;
		/* Pack apply legs (2026-09-11): three reloads through the deferred
		 * apply, ending on the default so the card is left as it was found.
		 * Row 0 is always the bundled default; row 1 the first installed
		 * pack. A D-pad click applies the selected row directly. */
		case STEP_TEXPACKS_UP_A:
			if(step_press_repeat(s, out, IB_GUI_UP, 3))
				goto_step(s, STEP_TEXPACKS_APPLY_A);
			break;
		case STEP_TEXPACKS_APPLY_A:
			out->press = true;
			out->button = IB_GUI_CLICK;
			s->pack_applies++;
			goto_step(s, STEP_TEXPACKS_RELOAD_A);
			break;
		case STEP_TEXPACKS_RELOAD_A:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt, RELOAD_DWELL))
				goto_step(s, STEP_TEXPACKS_DOWN_B);
			break;
		case STEP_TEXPACKS_DOWN_B:
			out->press = true;
			out->button = IB_GUI_DOWN;
			goto_step(s, STEP_TEXPACKS_APPLY_B);
			break;
		case STEP_TEXPACKS_APPLY_B:
			out->press = true;
			out->button = IB_GUI_CLICK;
			s->pack_applies++;
			goto_step(s, STEP_TEXPACKS_RELOAD_B);
			break;
		case STEP_TEXPACKS_RELOAD_B:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt, RELOAD_DWELL))
				goto_step(s, STEP_TEXPACKS_UP_C);
			break;
		case STEP_TEXPACKS_UP_C:
			out->press = true;
			out->button = IB_GUI_UP;
			goto_step(s, STEP_TEXPACKS_APPLY_C);
			break;
		case STEP_TEXPACKS_APPLY_C:
			out->press = true;
			out->button = IB_GUI_CLICK;
			s->pack_applies++;
			goto_step(s, STEP_TEXPACKS_RELOAD_C);
			break;
		case STEP_TEXPACKS_RELOAD_C:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt, RELOAD_DWELL))
				goto_step(s, STEP_TEXPACKS_LEAVE);
			break;
		case STEP_TEXPACKS_LEAVE:
			out->press = true;
			out->button = IB_HOME;
			goto_step(s, STEP_WAIT_TITLE_2);
			break;
		case STEP_WAIT_TITLE_2:
			switch(
				step_wait(s, screen, MENU_AP_SCREEN_TITLE, dt, WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_TITLE_DWELL_2); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_TITLE_DWELL_2:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TITLE, dt, DWELL_MENU))
				goto_step(s, STEP_TITLE_CLICK_SINGLE);
			break;
		case STEP_TITLE_CLICK_SINGLE:
			/* The title re-defaults to Play on arrival, so this is normally
			 * zero presses and one activate -- but it is OBSERVED, not
			 * assumed. The activate is issued only on a released tick that
			 * reads Play. */
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_TITLE,
							 obs.title_focus, MENU_AP_TITLE_PLAY)) {
				case WALK_REACHED:
					out->press = true;
					out->button = IB_GUI_CLICK;
					goto_step(s, STEP_WAIT_SWORLD);
					break;
				case WALK_FAILED: walk_fail(s, STEP_TITLE_CLICK_SINGLE); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_WAIT_SWORLD:
			switch(step_wait(s, screen, MENU_AP_SCREEN_SELECT_WORLD, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_SWORLD_DWELL); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_SWORLD_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_SELECT_WORLD, dt,
						  DWELL_MENU))
				goto_step(s, STEP_SWORLD_AIM_CREATE);
			break;
		case STEP_SWORLD_AIM_CREATE:
			if(screen != MENU_AP_SCREEN_SELECT_WORLD) {
				s->timer = 0.0f;
				break;
			}
			/* Asserted every tick once confirmed on select-world (harmless
			 * to repeat): the ONE interaction with no D-pad equivalent, see
			 * the header. */
			out->pointer_target = MENU_AP_PT_SWORLD_CREATE;
			s->timer += dt;
			if(s->timer >= AIM_HOLD) {
				out->press = true;
				out->button = IB_GUI_CLICK;
				goto_step(s, STEP_WAIT_CWORLD);
			}
			break;
		case STEP_WAIT_CWORLD:
			/* KEEP asserting the pointer while we are still on select-world.
			 * input_poll() runs at the END of the main loop (main.c), so the
			 * edge for the press synthesized on frame N is only visible to
			 * input_pressed() on frame N+1 -- by which time the AIM step had
			 * already advanced and stopped asserting the pointer, so
			 * screen_select_world saw click_pressed with pointer_available
			 * false and did nothing. Holding the target through the wait
			 * closes that skew without assuming its exact size. Still never
			 * asserted off select-world (the mock guards that). */
			if(screen == MENU_AP_SCREEN_SELECT_WORLD)
				out->pointer_target = MENU_AP_PT_SWORLD_CREATE;
			switch(step_wait(s, screen, MENU_AP_SCREEN_CREATE_WORLD, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_CWORLD_DWELL); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_CWORLD_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_CREATE_WORLD, dt,
						  DWELL_MENU))
				goto_step(s, STEP_CWORLD_NAV_MODE);
			break;
		case STEP_CWORLD_NAV_MODE:
			/* OBSERVED probe (see CWORLD_MODE_WALK_MAX). Even presses are
			 * DOWN, odd ones RIGHT, each followed by one released tick; after
			 * every RIGHT the real screen's mode is re-read. Leaving the
			 * screen or an open editor is a named failure, as is running out
			 * of presses -- never a blind walk onward. */
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || obs.cworld_editing) {
				cworld_fail(s, STEP_CWORLD_NAV_MODE);
				break;
			}
			if(s->repeat_phase) {
				s->repeat_phase = false;
				s->repeat_done++;
				break;
			}
			if(s->repeat_done > 0 && s->repeat_done % 2 == 0
			   && !obs.cworld_mode_survival) {
				/* RIGHT just flipped Survival -> Creative: the ring is on
				 * Game mode and the capture hop is already done. */
				goto_step(s, STEP_CWORLD_MODE_CREATIVE);
				break;
			}
			if(s->repeat_done >= CWORLD_MODE_WALK_MAX) {
				cworld_fail(s, STEP_CWORLD_NAV_MODE);
				break;
			}
			out->press = true;
			out->button = s->repeat_done % 2 == 0 ? IB_GUI_DOWN : IB_GUI_RIGHT;
			s->repeat_phase = true;
			break;
		case STEP_CWORLD_MODE_CREATIVE:
			/* The probe above ends ON Creative (that hop is the :8099 capture
			 * moment), so this step only re-presses if something knocked the
			 * selector back to Survival in between. */
			if(obs.cworld_mode_survival) {
				out->press = true;
				out->button = IB_GUI_RIGHT;
			}
			goto_step(s, STEP_CWORLD_MODE_DWELL);
			break;
		case STEP_CWORLD_MODE_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_CREATE_WORLD, dt,
						  DWELL_MENU))
				goto_step(s, STEP_CWORLD_MODE_CONFIRM);
			break;
		case STEP_CWORLD_MODE_CONFIRM: {
			/* Verify-before-proceed (2026-09-02 hardening -- see the header's
			 * design note): never trust a single bare press. Check the
			 * caller's live model FIRST; only press again if it still
			 * disagrees. `step_press_repeat`'s target is recomputed as
			 * repeat_done+1 every call so it keeps issuing a FRESH rising
			 * edge (press tick, idle tick, press tick, ...) rather than
			 * holding the button down -- holding either direction would wrap
			 * straight past the requested mode once it lands, since there
			 * are only two modes.
			 *
			 * Generalized (2026-09-02, --cw-cworld-mode / menus_dwell_
			 * creative): the SAME loop now confirms whichever mode
			 * `s->target_creative` requested, not hardcoded to Survival.
			 * Default target (false) reproduces the original behavior
			 * byte-for-byte: press GUI_LEFT until cworld_mode_survival is
			 * true. A creative target instead confirms the Creative hop
			 * above already landed (cworld_mode_survival is false) --
			 * normally true on the very first check, zero extra presses --
			 * and re-presses GUI_RIGHT only if some skew/regression knocked
			 * it back to Survival in between. */
			bool confirmed = s->target_creative ? !cworld_mode_survival
												 : cworld_mode_survival;
			if(confirmed) {
				s->cworld_sub = MENU_AP_CWORLD_SUB_DETAILS;
				goto_step(s, STEP_CW_SUB_BEGIN);
				break;
			}
			s->timer += dt;
			if(s->timer >= WAIT_MODE_SELECT) {
				goto_step(s, STEP_CWORLD_MODE_FAILED);
				break;
			}
			enum input_button retry_button =
				s->target_creative ? IB_GUI_RIGHT : IB_GUI_LEFT;
			(void)step_press_repeat(s, out, retry_button, s->repeat_done + 1);
			break;
		}
		case STEP_CWORLD_MODE_FAILED:
			/* Hard abandon: distinct from every other timeout so a real-run
			 * heartbeat's menu_ap_step names this exact failure. Never
			 * presses Create on an unconfirmed mode. */
			s->finished = true;
			break;
		/* ---- ui-build-1026 T15: the Create World sub-legs ------------- */
		case STEP_CW_SUB_BEGIN: {
			const struct cw_sub_def* def = &cw_subs[s->cworld_sub];
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || obs.cworld_editing
			   || obs.cworld_details_open) {
				cworld_fail(s, STEP_CW_SUB_BEGIN);
				break;
			}
			/* Where a real keyboard owns the open editor (PC), the walker's
			 * Close cannot reach it: the editors are skipped BY NAME rather
			 * than opened and abandoned. Never true on the console. */
			if(def->profile && s->text_input_owns_close) {
				s->cworld_osk_skipped = true;
				goto_step(s, STEP_CW_SUB_NEXT);
				break;
			}
			goto_step(s, STEP_CW_SUB_WALK);
			break;
		}
		case STEP_CW_SUB_WALK: {
			const struct cw_sub_def* def = &cw_subs[s->cworld_sub];
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || obs.cworld_editing
			   || obs.cworld_details_open) {
				cworld_fail(s, STEP_CW_SUB_WALK);
				break;
			}
			if(s->repeat_phase) {
				s->repeat_phase = false;
				s->repeat_done++;
				break;
			}
			if(obs.cworld_focus == def->target) {
				goto_step(s, STEP_CW_SUB_ARM);
				break;
			}
			int button = cw_route(obs.cworld_focus, def->target);
			if(button < 0 || s->repeat_done >= CWORLD_WALK_MAX) {
				cworld_fail(s, STEP_CW_SUB_WALK);
				break;
			}
			out->press = true;
			out->button = (enum input_button)button;
			s->repeat_phase = true;
			break;
		}
		case STEP_CW_SUB_ARM:
			/* One released frame between the last direction and the
			 * activate (R-B); latch the text Cancel must restore. */
			s->cworld_sig_before = obs.cworld_text_sig;
			goto_step(s, STEP_CW_SUB_ACTIVATE);
			break;
		case STEP_CW_SUB_ACTIVATE: {
			const struct cw_sub_def* def = &cw_subs[s->cworld_sub];
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || obs.cworld_editing
			   || obs.cworld_details_open || obs.cworld_focus != def->target) {
				cworld_fail(s, STEP_CW_SUB_ACTIVATE);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_CW_SUB_WAIT_IN);
			break;
		}
		case STEP_CW_SUB_WAIT_IN:
			if(cw_sub_inside(&cw_subs[s->cworld_sub], screen, obs)) {
				goto_step(s, STEP_CW_SUB_DWELL);
				break;
			}
			s->timer += dt;
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || s->timer >= WAIT_MENU)
				cworld_fail(s, STEP_CW_SUB_WAIT_IN);
			break;
		case STEP_CW_SUB_DWELL: {
			const struct cw_sub_def* def = &cw_subs[s->cworld_sub];
			/* Inside on EVERY tick, and nothing typed yet. */
			if(!cw_sub_inside(def, screen, obs)
			   || obs.cworld_text_sig != s->cworld_sig_before) {
				cworld_fail(s, STEP_CW_SUB_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= DWELL_MENU)
				goto_step(s, def->profile ? STEP_CW_SUB_KEY : STEP_CW_SUB_CLOSE);
			break;
		}
		case STEP_CW_SUB_KEY:
			/* The editor opens on its first key cell (osk_frontend_set_
			 * profile: 'q' on the name grid, '1' on the seed keypad). Only a
			 * focused CHARACTER cell is pressed -- never Done, Cancel or
			 * Random, which would end or reseed the edit. */
			if(!cw_sub_inside(&cw_subs[s->cworld_sub], screen, obs)
			   || obs.cworld_osk_target < MENU_AP_OSK_CHAR_FIRST
			   || obs.cworld_osk_target
				   >= MENU_AP_OSK_CHAR_FIRST + MENU_AP_OSK_CHAR_CELLS) {
				cworld_fail(s, STEP_CW_SUB_KEY);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_CW_SUB_WAIT_KEY);
			break;
		case STEP_CW_SUB_WAIT_KEY:
			if(!cw_sub_inside(&cw_subs[s->cworld_sub], screen, obs)) {
				cworld_fail(s, STEP_CW_SUB_WAIT_KEY);
				break;
			}
			if(obs.cworld_text_sig != s->cworld_sig_before) {
				goto_step(s, STEP_CW_SUB_CLOSE);
				break;
			}
			s->timer += dt;
			if(s->timer >= WAIT_MENU)
				cworld_fail(s, STEP_CW_SUB_WAIT_KEY);
			break;
		case STEP_CW_SUB_CLOSE:
			/* Close: the Details card closes and CONSUMES the frame; the
			 * editor answers OSK_EV_CANCEL (revert). Never an activate. */
			out->press = true;
			out->button = IB_INVENTORY;
			goto_step(s, STEP_CW_SUB_WAIT_OUT);
			break;
		case STEP_CW_SUB_WAIT_OUT: {
			const struct cw_sub_def* def = &cw_subs[s->cworld_sub];
			if(screen != MENU_AP_SCREEN_CREATE_WORLD) {
				cworld_fail(s, STEP_CW_SUB_WAIT_OUT);
				break;
			}
			if(!obs.cworld_editing && !obs.cworld_details_open
			   && obs.cworld_focus == def->target
			   && obs.cworld_text_sig == s->cworld_sig_before) {
				goto_step(s, STEP_CW_SUB_NEXT);
				break;
			}
			s->timer += dt;
			if(s->timer >= WAIT_MENU)
				cworld_fail(s, STEP_CW_SUB_WAIT_OUT);
			break;
		}
		case STEP_CW_SUB_NEXT:
			if(!(cw_subs[s->cworld_sub].profile && s->text_input_owns_close))
				s->cworld_subs_done |= 1u << (unsigned)s->cworld_sub;
			if(s->cworld_sub + 1 < MENU_AP_CWORLD_SUB_COUNT) {
				s->cworld_sub++;
				goto_step(s, STEP_CW_SUB_BEGIN);
			} else
				goto_step(s, STEP_CWORLD_NAV_CREATE);
			break;
		case STEP_CWORLD_NAV_CREATE:
			/* OBSERVED walk (menus-dwell-walker-1026). The ring is on Game
			 * Mode here (Seed and Create are below it), but nothing is
			 * assumed: every tick re-reads the real screen. An open editor
			 * or a screen that left is a named failure -- the walker never
			 * opens the editor, so either means something else took over.
			 * Each DOWN is followed by one released tick (a fresh rising
			 * edge per press, and the observation of THAT press's result is
			 * what decides the next one). */
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || obs.cworld_editing) {
				cworld_fail(s, STEP_CWORLD_NAV_CREATE);
				break;
			}
			if(s->repeat_phase) {
				s->repeat_phase = false;
				s->repeat_done++;
				break;
			}
			if(obs.cworld_create_ready) {
				goto_step(s, STEP_CWORLD_SUBMIT);
				break;
			}
			if(s->repeat_done >= CWORLD_WALK_MAX) {
				cworld_fail(s, STEP_CWORLD_NAV_CREATE);
				break;
			}
			out->press = true;
			out->button = IB_GUI_DOWN;
			s->repeat_phase = true;
			break;
		case STEP_CWORLD_SUBMIT:
			/* The activate frame is a DIFFERENT frame from the one that
			 * observed the ring arrive (never navigate+activate together),
			 * and it re-checks both gates on the frame of the press. */
			if(screen != MENU_AP_SCREEN_CREATE_WORLD || obs.cworld_editing
			   || !obs.cworld_create_ready) {
				cworld_fail(s, STEP_CWORLD_SUBMIT);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_WAIT_INGAME);
			break;
		case STEP_WAIT_INGAME:
			switch(step_wait(s, screen, MENU_AP_SCREEN_INGAME, dt,
							  WAIT_WORLD)) {
				case WAIT_REACHED: goto_step(s, STEP_INGAME_SETTLE); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_INGAME_SETTLE:
			if(step_dwell(s, screen, MENU_AP_SCREEN_INGAME, dt,
						  SETTLE_INGAME))
				goto_step(s, STEP_SIGN_AIM);
			break;
		case STEP_SIGN_AIM:
			/* Explicit aim + pulse (2026-09-11): the profile's fixed pitch of
			 * 108 descended into the demo floor in front of the sign, so the
			 * scheduled tick-40 pulse never hit it; the walker now faces the
			 * sign at eye level itself (menu_dwell_demo.h). */
			out->aim_set = true;
			out->aim_yaw_deg = MENU_DWELL_AIM_SIGN_YAW;
			out->aim_pitch_deg = MENU_DWELL_AIM_SIGN_PITCH;
			s->timer += dt;
			if(s->timer >= AIM_HOLD)
				goto_step(s, STEP_SIGN_USE);
			break;
		case STEP_SIGN_USE:
			out->place_pulse = true;
			goto_step(s, STEP_WAIT_SIGN);
			break;
		case STEP_WAIT_SIGN:
			/* SOFT wait: no press of our own here at all -- the EXISTING
			 * debug.autopilot "aim" + one-shot place-pulse mechanism (armed
			 * independently by hw_run) is what opens the sign, once its
			 * server-side demo block exists and the fixed aim lands on it.
			 * A timeout is not a failure of THIS script; it skips ahead. */
			switch(
				step_wait(s, screen, MENU_AP_SCREEN_SIGN, dt, WAIT_SIGN)) {
				case WAIT_REACHED: goto_step(s, STEP_SIGN_DWELL); break;
				case WAIT_TIMEOUT:
					/* One-shot latch: lets the impure caller log/telemeter
					 * WHICH gate this run's sign step never reached (see
					 * menu_autopilot_script.h's design note and main.c's
					 * hw_boot_trace_write call site). */
					s->sign_wait_timed_out = true;
					goto_step(s, STEP_INVENTORY_OPEN);
					break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_SIGN_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_SIGN, dt, DWELL_SIGN))
				goto_step(s, STEP_SIGN_NAV);
			break;
		case STEP_SIGN_NAV:
			if(step_press_repeat(s, out, IB_GUI_RIGHT, 2))
				goto_step(s, STEP_SIGN_EDIT_CHAR);
			break;
		case STEP_SIGN_EDIT_CHAR:
			out->press = true;
			out->button = IB_GUI_UP;
			goto_step(s, STEP_SIGN_LEAVE);
			break;
		case STEP_SIGN_LEAVE:
			out->press = true;
			out->button = IB_INVENTORY;
			goto_step(s, STEP_WAIT_INGAME_2);
			break;
		case STEP_WAIT_INGAME_2:
			switch(step_wait(s, screen, MENU_AP_SCREEN_INGAME, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_INVENTORY_OPEN); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_INVENTORY_OPEN:
			if(screen != MENU_AP_SCREEN_INGAME) {
				/* Same bounded wait as STEP_PAUSE_OPEN below: the WAIT_SIGN soft
				 * skip lands here while screen_sign may still be closing. */
				s->timer += dt;
				if(s->timer >= WAIT_MENU)
					s->finished = true;
				break;
			}
			out->press = true;
			/* brief 07 C: this step OPENS from screen_ingame, so it presses
			 * the open role. Every other IB_INVENTORY press in this file is a
			 * LEAVE / Back / close and stays on the Close role. */
			out->button = IB_OPEN_INVENTORY;
			goto_step(s, STEP_WAIT_INVENTORY);
			break;
		case STEP_WAIT_INVENTORY:
			switch(step_wait(s, screen, MENU_AP_SCREEN_INVENTORY, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_INVENTORY_DWELL); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_INVENTORY_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_INVENTORY, dt, DWELL_MENU))
				goto_step(s, STEP_INVENTORY_FOCUS_RIGHT);
			break;
		case STEP_INVENTORY_FOCUS_RIGHT:
			/* The inventory portrait follows the owning pointer or the slot
			 * focus (Astra b33e5f3c). The walker owns no pointer, so it walks
			 * the focus to the right edge of the bag grid and holds there:
			 * a turned head/body in the frame is the console-side proof. */
			if(step_press_repeat(s, out, IB_GUI_RIGHT, 5))
				goto_step(s, STEP_INVENTORY_LOOK_DWELL);
			break;
		case STEP_INVENTORY_LOOK_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_INVENTORY, dt, DWELL_MENU))
				goto_step(s, STEP_INVENTORY_PICK);
			break;
		/* ---- the HOLD leg (G-0b) -------------------------------------
		 * The Inventory footer only draws its Drop prompt while the cursor
		 * HOLDS a stack, so every console capture so far photographed the
		 * one state that cannot show it. These three steps put the screen
		 * into the held state and hold it there, with NO inventory
		 * observation of any kind: this module stays pure (its header's
		 * standing rule), so it cannot ask whether the focused slot has an
		 * item in it. It does not need to. With the capture rig's existing
		 * debug.give lever (main.c; EXTRA_SET=+set:debug.give=1 in
		 * tools/console_capture/capture_chain.sh) the bag is stocked and
		 * the pair reads pick-up / put-back; with an empty inventory
		 * container_menu.c makes BOTH clicks no-ops on an empty slot, so
		 * the leg costs one dwell and changes nothing. Either way nothing
		 * is discarded, halved or transferred -- there is deliberately no
		 * Drop, Split or Quick Move press anywhere in this script. */
		case STEP_INVENTORY_PICK:
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_INVENTORY_HOLD_DWELL);
			break;
		case STEP_INVENTORY_HOLD_DWELL:
			/* The SAME dwell the two neighbouring inventory steps use, so a
			 * 3 s console capture period cannot fall outside the held state. */
			if(step_dwell(s, screen, MENU_AP_SCREEN_INVENTORY, dt, DWELL_MENU))
				goto_step(s, STEP_INVENTORY_PLACE);
			break;
		case STEP_INVENTORY_PLACE:
			/* The same slot: the dwell above issues no press at all, so the
			 * focus has not moved since the pick. A fresh rising edge is
			 * guaranteed by that dwell (input_pressed needs prev=false). */
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_INV_SWAP_TO_CRAFT);
			break;
		/* ---- ui-build-1026 T15c: the two inventory pages ------------- */
		case STEP_INV_SWAP_TO_CRAFT:
			if(screen != MENU_AP_SCREEN_INVENTORY || obs.inv_page != 0) {
				walk_fail(s, STEP_INV_SWAP_TO_CRAFT);
				break;
			}
			out->press = true;
			out->button = IB_GUI_PAGE_SWAP;
			goto_step(s, STEP_INV_WAIT_CRAFT);
			break;
		case STEP_INV_WAIT_CRAFT:
			if(screen != MENU_AP_SCREEN_INVENTORY) {
				walk_fail(s, STEP_INV_WAIT_CRAFT);
				break;
			}
			if(obs.inv_page == 1 && obs.inv_tab >= 0) {
				s->inv_tab_before = obs.inv_tab;
				goto_step(s, STEP_INV_CRAFT_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= WAIT_MENU)
				walk_fail(s, STEP_INV_WAIT_CRAFT);
			break;
		case STEP_INV_CRAFT_DWELL:
			if(screen != MENU_AP_SCREEN_INVENTORY || obs.inv_page != 1
			   || obs.inv_tab != s->inv_tab_before) {
				walk_fail(s, STEP_INV_CRAFT_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= DWELL_MENU)
				goto_step(s, STEP_INV_TAB);
			break;
		case STEP_INV_TAB:
			if(screen != MENU_AP_SCREEN_INVENTORY || obs.inv_page != 1) {
				walk_fail(s, STEP_INV_TAB);
				break;
			}
			out->press = true;
			out->button = IB_GUI_PAGE;
			goto_step(s, STEP_INV_WAIT_TAB);
			break;
		case STEP_INV_WAIT_TAB:
			if(screen != MENU_AP_SCREEN_INVENTORY || obs.inv_page != 1) {
				walk_fail(s, STEP_INV_WAIT_TAB);
				break;
			}
			if(obs.inv_tab == (s->inv_tab_before + 1) % MENU_AP_CRAFT_TABS) {
				goto_step(s, STEP_INV_TAB_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= WAIT_MENU)
				walk_fail(s, STEP_INV_WAIT_TAB);
			break;
		case STEP_INV_TAB_DWELL:
			if(screen != MENU_AP_SCREEN_INVENTORY || obs.inv_page != 1
			   || obs.inv_tab
				   != (s->inv_tab_before + 1) % MENU_AP_CRAFT_TABS) {
				walk_fail(s, STEP_INV_TAB_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= DWELL_MENU)
				goto_step(s, STEP_INV_SWAP_BACK);
			break;
		case STEP_INV_SWAP_BACK:
			if(screen != MENU_AP_SCREEN_INVENTORY || obs.inv_page != 1) {
				walk_fail(s, STEP_INV_SWAP_BACK);
				break;
			}
			out->press = true;
			out->button = IB_GUI_PAGE_SWAP;
			goto_step(s, STEP_INV_WAIT_INV);
			break;
		case STEP_INV_WAIT_INV:
			if(screen != MENU_AP_SCREEN_INVENTORY) {
				walk_fail(s, STEP_INV_WAIT_INV);
				break;
			}
			if(obs.inv_page == 0) {
				s->inv_pages_seen = true;
				goto_step(s, STEP_INVENTORY_LEAVE);
				break;
			}
			s->timer += dt;
			if(s->timer >= WAIT_MENU)
				walk_fail(s, STEP_INV_WAIT_INV);
			break;
		case STEP_INVENTORY_LEAVE:
			out->press = true;
			out->button = IB_INVENTORY;
			goto_step(s, STEP_WAIT_INGAME_INV);
			break;
		case STEP_WAIT_INGAME_INV:
			switch(step_wait(s, screen, MENU_AP_SCREEN_INGAME, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_FURNACE_AIM); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		/* ---- container legs (menu_dwell_demo.h) ---------------------- */
		case STEP_FURNACE_AIM:
			container_aim(s, out, dt, MENU_DWELL_AIM_FURNACE_YAW,
						  MENU_DWELL_AIM_FURNACE_PITCH, STEP_FURNACE_USE);
			break;
		case STEP_FURNACE_USE:
			out->place_pulse = true;
			goto_step(s, STEP_WAIT_FURNACE);
			break;
		case STEP_WAIT_FURNACE:
			container_wait(s, screen, dt, MENU_AP_SCREEN_FURNACE,
						   &s->furnace_reached, STEP_FURNACE_DWELL,
						   STEP_CHEST_AIM);
			break;
		case STEP_FURNACE_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_FURNACE, dt, DWELL_MENU))
				goto_step(s, STEP_FURNACE_LEAVE);
			break;
		case STEP_FURNACE_LEAVE:
			container_leave(s, out, screen, STEP_WAIT_INGAME_FURNACE);
			break;
		case STEP_WAIT_INGAME_FURNACE:
			container_wait_ingame(s, screen, dt, STEP_CHEST_AIM);
			break;

		case STEP_CHEST_AIM:
			container_aim(s, out, dt, MENU_DWELL_AIM_CHEST_YAW,
						  MENU_DWELL_AIM_CHEST_PITCH, STEP_CHEST_USE);
			break;
		case STEP_CHEST_USE:
			out->place_pulse = true;
			goto_step(s, STEP_WAIT_CHEST);
			break;
		case STEP_WAIT_CHEST:
			container_wait(s, screen, dt, MENU_AP_SCREEN_CHEST,
						   &s->chest_reached, STEP_CHEST_DWELL, STEP_LARGE_AIM);
			break;
		case STEP_CHEST_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_CHEST, dt, DWELL_MENU))
				goto_step(s, STEP_CHEST_LEAVE); /* Beta chest: look, then close */
			break;
		case STEP_CHEST_LEAVE:
			container_leave(s, out, screen, STEP_WAIT_INGAME_CHEST);
			break;
		case STEP_WAIT_INGAME_CHEST:
			container_wait_ingame(s, screen, dt, STEP_LARGE_AIM);
			break;

		case STEP_LARGE_AIM:
			container_aim(s, out, dt, MENU_DWELL_AIM_LARGE_YAW,
						  MENU_DWELL_AIM_LARGE_PITCH, STEP_LARGE_USE);
			break;
		case STEP_LARGE_USE:
			out->place_pulse = true;
			goto_step(s, STEP_WAIT_LARGE);
			break;
		case STEP_WAIT_LARGE:
			container_wait(s, screen, dt, MENU_AP_SCREEN_CHEST,
						   &s->large_chest_reached, STEP_LARGE_DWELL,
						   STEP_DISPENSER_AIM);
			break;
		case STEP_LARGE_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_CHEST, dt, DWELL_MENU))
				goto_step(s, STEP_LARGE_LEAVE);
			break;
		case STEP_LARGE_LEAVE:
			container_leave(s, out, screen, STEP_WAIT_INGAME_LARGE);
			break;
		case STEP_WAIT_INGAME_LARGE:
			container_wait_ingame(s, screen, dt, STEP_DISPENSER_AIM);
			break;

		case STEP_DISPENSER_AIM:
			container_aim(s, out, dt, MENU_DWELL_AIM_DISPENSER_YAW,
						  MENU_DWELL_AIM_DISPENSER_PITCH, STEP_DISPENSER_USE);
			break;
		case STEP_DISPENSER_USE:
			out->place_pulse = true;
			goto_step(s, STEP_WAIT_DISPENSER);
			break;
		case STEP_WAIT_DISPENSER:
			container_wait(s, screen, dt, MENU_AP_SCREEN_DISPENSER,
						   &s->dispenser_reached, STEP_DISPENSER_DWELL,
						   STEP_PAUSE_OPEN);
			break;
		case STEP_DISPENSER_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_DISPENSER, dt, DWELL_MENU))
				goto_step(s, STEP_DISPENSER_LEAVE);
			break;
		case STEP_DISPENSER_LEAVE:
			container_leave(s, out, screen, STEP_WAIT_INGAME_DISPENSER);
			break;
		case STEP_WAIT_INGAME_DISPENSER:
			container_wait_ingame(s, screen, dt, STEP_PAUSE_OPEN);
			break;

		case STEP_PAUSE_OPEN:
			if(screen != MENU_AP_SCREEN_INGAME) {
				/* Reached either normally (from WAIT_INGAME_2) or via the
				 * WAIT_SIGN soft-timeout skip, which does not itself confirm
				 * we are back in-game yet (we may still be mid-transition
				 * off screen_sign). Accumulate a bounded wait rather than
				 * pressing HOME on an unexpected screen; once `screen`
				 * naturally becomes MENU_AP_SCREEN_INGAME this branch is no
				 * longer taken and the press below fires that same tick. */
				s->timer += dt;
				if(s->timer >= WAIT_MENU)
					s->finished = true;
				break;
			}
			out->press = true;
			out->button = IB_HOME;
			goto_step(s, STEP_WAIT_PAUSE);
			break;
		case STEP_WAIT_PAUSE:
			switch(step_wait(s, screen, MENU_AP_SCREEN_PAUSE, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_PAUSE_DWELL); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_PAUSE_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_PAUSE, dt, DWELL_MENU))
				goto_step(s, STEP_PAUSE_NAV_OPTIONS);
			break;
		case STEP_PAUSE_NAV_OPTIONS:
			/* Was ONE blind DOWN: since 91bd8295c (10-05) the second row is
			 * Achievements, so that press opened the Achievements board and
			 * every menus_dwell stalled at STEP_WAIT_OPTIONS (hw1026b). */
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_PAUSE,
							 obs.pause_focus, MENU_AP_PAUSE_OPTIONS)) {
				case WALK_REACHED: goto_step(s, STEP_PAUSE_CLICK_OPTIONS); break;
				case WALK_FAILED: walk_fail(s, STEP_PAUSE_NAV_OPTIONS); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_PAUSE_CLICK_OPTIONS:
			if(screen != MENU_AP_SCREEN_PAUSE
			   || obs.pause_focus != MENU_AP_PAUSE_OPTIONS) {
				walk_fail(s, STEP_PAUSE_CLICK_OPTIONS);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_WAIT_OPTIONS);
			break;
		case STEP_WAIT_OPTIONS:
			switch(step_wait(s, screen, MENU_AP_SCREEN_OPTIONS, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_OPTIONS_DWELL); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_OPTIONS_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_OPTIONS, dt, DWELL_MENU))
				goto_step(s, STEP_OPTIONS_SLIDER);
			break;
		case STEP_OPTIONS_SLIDER:
			out->press = true;
			out->button = IB_GUI_RIGHT;
			goto_step(s, STEP_OPTIONS_LEAVE);
			break;
		case STEP_OPTIONS_LEAVE:
			out->press = true;
			out->button = IB_HOME;
			goto_step(s, STEP_WAIT_PAUSE_2);
			break;
		case STEP_WAIT_PAUSE_2:
			switch(step_wait(s, screen, MENU_AP_SCREEN_PAUSE, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_PAUSE_DWELL_2); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_PAUSE_DWELL_2:
			if(step_dwell(s, screen, MENU_AP_SCREEN_PAUSE, dt, DWELL_MENU))
				goto_step(s, STEP_PAUSE_RESUME);
			break;
		case STEP_PAUSE_RESUME:
			out->press = true;
			out->button = IB_HOME;
			goto_step(s, STEP_WAIT_INGAME_3);
			break;
		case STEP_WAIT_INGAME_3:
			switch(step_wait(s, screen, MENU_AP_SCREEN_INGAME, dt,
							  WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_QUIT_OPEN); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		/* Save and quit leg (2026-09-11): the console flashed the title and
		 * dropped back in-game every time. The pause now waits for the
		 * server's answer; this leg proves the round trip (title = the
		 * unload settled) and, when the save is refused, leaves the pause's
		 * "Save failed: <stage>" line in the capture. */
		case STEP_QUIT_OPEN:
			out->press = true;
			out->button = IB_HOME;
			goto_step(s, STEP_WAIT_PAUSE_3);
			break;
		case STEP_WAIT_PAUSE_3:
			switch(step_wait(s, screen, MENU_AP_SCREEN_PAUSE, dt, WAIT_MENU)) {
				case WAIT_REACHED: goto_step(s, STEP_QUIT_NAV); break;
				case WAIT_TIMEOUT: s->finished = true; break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_QUIT_NAV:
			/* Was a blind DOWN x3 (Back -> Options -> Texture packs -> Save
			 * and quit); with the Achievements row that lands on Texture
			 * packs. The exit row is observed by what it does. */
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_PAUSE,
							 obs.pause_focus, MENU_AP_PAUSE_EXIT)) {
				case WALK_REACHED: goto_step(s, STEP_QUIT_CLICK); break;
				case WALK_FAILED: walk_fail(s, STEP_QUIT_NAV); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_QUIT_CLICK:
			if(screen != MENU_AP_SCREEN_PAUSE
			   || obs.pause_focus != MENU_AP_PAUSE_EXIT) {
				walk_fail(s, STEP_QUIT_CLICK);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_WAIT_QUIT);
			break;
		case STEP_WAIT_QUIT:
			switch(step_wait(s, screen, MENU_AP_SCREEN_TITLE, dt, WAIT_QUIT)) {
				case WAIT_REACHED:
					s->quit_reached = true;
					goto_step(s, STEP_TITLE_DWELL_3);
					break;
				case WAIT_TIMEOUT:
					s->quit_failed = true;
					s->finished = true;
					break;
				case WAIT_PENDING: break;
			}
			break;
		case STEP_TITLE_DWELL_3:
			if(step_dwell(s, screen, MENU_AP_SCREEN_TITLE, dt, DWELL_MENU))
				goto_step(s, STEP_NP_TITLE_DWELL);
			break;

		/* ---- THE NO-POINTER LEG (A7 part 1) -------------------------
		 * Every tick below emits pointer_suppressed (set once, before this
		 * switch, from np_leg_step) and NO pointer target, so the screens
		 * read pointer_available == false from input_pointer() for the
		 * leg's whole duration -- the same thing they read on a console
		 * with the Remote pointed away. Every transition is np_wait
		 * (bounded, fails by name); every exit is the Close role. */
		case STEP_NP_TITLE_DWELL:
			/* The marker for the coverage this PLATFORM cannot walk, latched
			 * where the leg begins rather than asserted per skipped step:
			 * part 1 enters no OSK-backed screen, part 2's sub-editor steps
			 * are what it will describe. */
			if(s->text_input_owns_close)
				s->np_osk_legs_skipped = true;
			np_dwell(s, screen, MENU_AP_SCREEN_TITLE, dt, STEP_NP_TITLE_DWELL,
					 STEP_NP_SWORLD_ACT);
			break;
		case STEP_NP_SWORLD_ACT:
			/* screen_title_reset() re-defaults the selection to Play on
			 * every arrival, and with no pointer the activation rule's case
			 * (3) activates exactly that highlighted row -- normally zero
			 * navigation presses, but observed rather than assumed. */
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_TITLE,
							 obs.title_focus, MENU_AP_TITLE_PLAY)) {
				case WALK_REACHED:
					out->press = true;
					out->button = IB_GUI_CLICK;
					goto_step(s, STEP_NP_WAIT_SWORLD);
					break;
				case WALK_FAILED: np_fail(s, STEP_NP_SWORLD_ACT); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_NP_WAIT_SWORLD:
			np_wait(s, screen, MENU_AP_SCREEN_SELECT_WORLD, dt,
					STEP_NP_WAIT_SWORLD, STEP_NP_SWORLD_DWELL);
			break;
		case STEP_NP_SWORLD_DWELL:
			if(step_dwell(s, screen, MENU_AP_SCREEN_SELECT_WORLD, dt,
						  DWELL_MENU)) {
				/* Latched HERE, at the END of the dwell, not on arrival:
				 * screen_select_world resolves only a couple of rows per
				 * frame (its watchdog budget), so a count read the instant
				 * the screen appears can be short of the real one -- and
				 * would then make the delete/rename comparisons below fail
				 * against their own stale baseline. Three seconds is four
				 * hundred rows of budget. */
				s->np_rows = obs.sworld_rows;
				s->np_sub = NP_SUB_SWORLD_FIRST;
				goto_step(s, STEP_NP_SUB_BEGIN);
			} else if(screen != MENU_AP_SCREEN_SELECT_WORLD
					  && ++s->repeat_done > NP_DWELL_MAX_OFF_TICKS)
				np_fail(s, STEP_NP_SWORLD_DWELL);
			break;

		/* ---- the shared sub-leg walk (part 2) ------------------------ */
		case STEP_NP_SUB_BEGIN:
			s->np_parent_stage = 0;
			/* Every sub-leg starts from its host screen. Anything else means
			 * the previous one left the front end somewhere unexpected, and
			 * pressing on from there is exactly what this leg must not do. */
			if(screen != (enum menu_ap_screen)sub->host) {
				np_fail(s, STEP_NP_SUB_BEGIN);
				break;
			}
			/* No answer from the Multiplayer screen's observers in this
			 * build: skip BY NAME rather than walk blind. Every press this
			 * leg makes is justified by an observation, so with no
			 * observation there is no justified press -- and 160 DOWNs at a
			 * screen we cannot see into is exactly the blind walking the
			 * whole design exists to refuse. */
			if(sub->host == MENU_AP_SCREEN_MULTIPLAYER && !obs.mps_observed) {
				s->np_mps_unobserved = true;
				s->np_flags |= MENU_AP_NP_FLAG_MPS_UNOBSERVED;
				np_sub_mark(s, &s->np_skipped);
				goto_step(s, STEP_NP_SUB_NEXT);
				break;
			}
			goto_step(s, STEP_NP_SUB_TO_STRIP);
			break;
		case STEP_NP_SUB_TO_STRIP: {
			int focus = np_observed_focus(sub, obs);
			int target = sub->parent_focus && s->np_parent_stage == 0
				? sub->parent_focus : sub->focus_target;
			int budget = sub->host == MENU_AP_SCREEN_SELECT_WORLD
				? np_converge_budget(sub, obs) + NP_WALK_MAX_STRIP
				: NP_WALK_MAX_CONVERGE;
			switch(np_walk(s, out, focus == target,
						   np_route_button(sub, focus, target), budget)) {
				case NP_WALK_DONE: goto_step(s, STEP_NP_SUB_ARM); break;
				case NP_WALK_EXHAUSTED:
					if(sub->conditional && s->np_parent_stage == 1) {
						out->press = true;
						out->button = IB_INVENTORY;
						s->np_parent_stage = 3;
						goto_step(s, STEP_NP_SUB_WAIT_IN);
					} else if(sub->conditional && s->np_parent_stage == 0
							  && ((sub->host == MENU_AP_SCREEN_SELECT_WORLD
								   && obs.sworld_rows == 0)
								  || (sub->host == MENU_AP_SCREEN_MULTIPLAYER
									  && obs.mps_rows == 0))) {
						np_sub_mark(s, &s->np_skipped);
						goto_step(s, STEP_NP_SUB_NEXT);
					} else
						np_fail(s, STEP_NP_SUB_TO_STRIP);
					break;
				case NP_WALK_PENDING: break;
			}
			break;
		}
		case STEP_NP_SUB_TO_STRIP_DOWN: {
			/* THE FALLBACK, and A14's step unchanged: DOWN converges on the
			 * action strip from anywhere above it (rows, then off the last row
			 * into the strip; on Multiplayer also name -> quick -> list ->
			 * strip), bounded by the OBSERVED row count. It runs only when the
			 * RIGHT entry above did not move the ring, so a run that needed it
			 * SAYS SO in its trace: this ordinal appears at all, and the leg
			 * pays a press per row again.
			 *
			 * EXHAUSTED is a real possibility (a screen that will not let the
			 * ring out of its list at all) and FAILS BY NAME -- never a skip.
			 * Running out of presses before the ring even left the list is not
			 * "the screen correctly disabled this sub-leg's target": BOTH host
			 * strips always hold at least one enabled button (Select World's
			 * Create and Cancel, Multiplayer's Direct Connect and Back), so
			 * the ring can always get there when the screen is up. Handing an
			 * exhausted converge to SEEK -- the step that legitimately turns
			 * "the named button is disabled" into a named skip -- would let a
			 * walk that simply ran out RECORD A SKIP THE SCREEN NEVER MADE for
			 * the three conditional targets (Rename, Delete, Edit). That is
			 * exactly the entered-vs-skipped blur the proof masks exist to
			 * prevent (review A9-3), so it is refused here and the sub-leg
			 * dies naming this step. */
			goto_step(s, STEP_NP_SUB_TO_STRIP);
			break;
		}
		case STEP_NP_SUB_CLAMP:
			/* LEFT does not wrap -- it clamps on the leftmost ENABLED button
			 * (screen_select_world.c's sworld_focus_step / the Multiplayer
			 * twin), so a fixed strip-width of presses normalises the ring
			 * from wherever the screen happened to put it. That leftmost
			 * button is usually one of the FORBIDDEN ones (Play Selected
			 * World / Join Server); resting there is fine, it is only
			 * activating there that is not. */
			goto_step(s, STEP_NP_SUB_TO_STRIP);
			break;
		case STEP_NP_SUB_SEEK: {
			goto_step(s, STEP_NP_SUB_TO_STRIP);
			break;
		}
		case STEP_NP_SUB_ARM:
			/* One deliberately empty frame between the last direction edge
			 * and the activate (R-B), and the place the UP approach records
			 * that its target really was observed. */
			if(!sub->parent_focus || s->np_parent_stage != 0)
				np_sub_mark(s, &s->np_focus_reached);
			goto_step(s, STEP_NP_SUB_ACTIVATE);
			break;
		case STEP_NP_SUB_ACTIVATE: {
			int focus = np_observed_focus(sub, obs);
			int target = sub->parent_focus && s->np_parent_stage == 0
				? sub->parent_focus : sub->focus_target;
			/* THE GUARD, re-checked on the very frame of the press rather
			 * than trusted from the SEEK that got here: the ring can move
			 * under a walker (a list resolving, a button going disabled), and
			 * an activate on the wrong control is the one mistake this leg
			 * cannot take back. */
			if(focus != target || np_focus_forbidden(sub, focus)) {
				np_fail(s, STEP_NP_SUB_ACTIVATE);
				break;
			}
			/* OSK-backed screens where a real keyboard owns the Close: the
			 * focus has been PROVEN to reach the control (np_focus_reached),
			 * and entering would park the walker with no way out. Named skip,
			 * so the gap is in the run's own record. */
			if(sub->osk_backed && s->text_input_owns_close
			   && (!sub->parent_focus || s->np_parent_stage != 0)) {
				np_sub_mark(s, &s->np_skipped);
				s->np_osk_legs_skipped = true;
				s->np_flags |= MENU_AP_NP_FLAG_OSK_SKIP;
				goto_step(s, STEP_NP_SUB_NEXT);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			if(sub->parent_focus && s->np_parent_stage == 0)
				s->np_parent_stage = 1;
			else if(sub->parent_focus)
				s->np_parent_stage = 2;
			goto_step(s, STEP_NP_SUB_WAIT_IN);
			break;
		}
		case STEP_NP_SUB_WAIT_IN:
			/* Options is an actual parent action. Wait until its modal reports
			 * a different semantic focus, then route the same real-input walker
			 * to Rename/Delete/Edit. */
			if(sub->parent_focus && s->np_parent_stage == 1) {
				int focus = np_observed_focus(sub, obs);
				if(screen == (enum menu_ap_screen)sub->host
				   && focus != sub->parent_focus)
					goto_step(s, STEP_NP_SUB_TO_STRIP);
				else if((s->timer += dt) >= WAIT_MENU)
					np_fail(s, STEP_NP_SUB_WAIT_IN);
				break;
			}
			if(sub->parent_focus && s->np_parent_stage == 3) {
				if(screen == (enum menu_ap_screen)sub->host
				   && np_observed_focus(sub, obs) == sub->parent_focus) {
					np_sub_mark(s, &s->np_skipped);
					goto_step(s, STEP_NP_SUB_NEXT);
				} else if((s->timer += dt) >= WAIT_MENU)
					np_fail(s, STEP_NP_SUB_WAIT_IN);
				break;
			}
			/* The delete confirmation is the ONE modal this leg opens whose
			 * highlighted answer decides whether a world survives. Close
			 * answers NO whatever the highlight is (screen_confirm.c:196), so
			 * the leg is safe either way -- but a safe-answer default that
			 * had regressed would otherwise be INVISIBLE to the run meant to
			 * catch it. Observe it, flag it, and still leave cleanly: the
			 * abandon is deferred to STEP_NP_SUB_NEXT (np_pending_fail_step)
			 * so the run never gives up with a modal still on screen. */
			if(np_sub_inside(sub, screen, obs)
			   && sub->enter_screen == MENU_AP_SCREEN_CONFIRM
			   && !obs.confirm_focus_safe && !s->np_pending_fail_step) {
				s->np_flags |= MENU_AP_NP_FLAG_CONFIRM_NOT_SAFE;
				s->np_pending_fail_step = STEP_NP_SUB_WAIT_IN;
			}
			np_wait_for(s, np_sub_inside(sub, screen, obs), dt,
						STEP_NP_SUB_WAIT_IN, STEP_NP_SUB_DWELL);
			break;
		case STEP_NP_SUB_DWELL:
			/* The same screen-gated dwell as everywhere else, but gated on
			 * the sub-leg's own "am I inside" predicate, which for a
			 * Multiplayer sub-editor is screen_mps_view() and not the screen
			 * pointer. */
			if(!np_sub_inside(sub, screen, obs)) {
				s->timer = 0.0f;
				if(++s->repeat_done > NP_DWELL_MAX_OFF_TICKS)
					np_fail(s, STEP_NP_SUB_DWELL);
				break;
			}
			s->timer += dt;
			if(s->timer >= DWELL_MENU)
				goto_step(s, STEP_NP_SUB_CLOSE);
			break;
		case STEP_NP_SUB_CLOSE:
			/* Close, never an activate -- and on the delete confirm that is
			 * load-bearing, not stylistic: screen_confirm.c's back-out answers
			 * NO unconditionally, whatever the highlight is, while an activate
			 * there fires whatever `selected` happens to be. See the header
			 * note; the confirm's highlight has no production observer, so an
			 * activate on it cannot be gated the way every other press in this
			 * leg is, and the thing it would do if the default ever regressed
			 * is delete one of the maintainer's worlds. */
			out->press = true;
			out->button = IB_INVENTORY;
			goto_step(s, STEP_NP_SUB_WAIT_OUT);
			break;
		case STEP_NP_SUB_WAIT_OUT:
			if(np_sub_outside(sub, screen, obs))
				np_sub_mark(s, &s->np_entered);
			np_wait_for(s, np_sub_outside(sub, screen, obs), dt,
						STEP_NP_SUB_WAIT_OUT, STEP_NP_SUB_NEXT);
			break;
		case STEP_NP_SUB_NEXT: {
			/* THE OUTCOME CHECK, and the one place this leg asserts something
			 * about the WORLD rather than about a screen. Review A9-5 killed
			 * the old version of it: it checked that focus still reported a
			 * LIST row, which only proves anything on a ONE-world card --
			 * delete one of the maintainer's several and a row still remains.
			 * A COUNT taken before the sub-leg and again after is the proof
			 * that holds at any list length. Applied to RENAME as well as
			 * DELETE: the rename editor must not change how many worlds
			 * exist either.
			 *
			 * np_rows < 0 means the dwell never latched one, which can only
			 * happen if the leg never settled on Select World at all -- the
			 * bounded waits above have already failed by then. */
			if((s->np_sub == MENU_AP_SUB_SWORLD_DELETE
				|| s->np_sub == MENU_AP_SUB_SWORLD_RENAME)
			   && (s->np_entered & MENU_AP_NP_BIT(s->np_sub)) && s->np_rows >= 0
			   && obs.sworld_rows != s->np_rows) {
				s->np_flags |= MENU_AP_NP_FLAG_ROW_COUNT_MISMATCH;
				np_fail(s, STEP_NP_SUB_NEXT);
				break;
			}
			/* A failure found INSIDE a modal, deferred to here so the leg
			 * left the screen clean first (see STEP_NP_SUB_WAIT_IN). */
			if(s->np_pending_fail_step) {
				int step = s->np_pending_fail_step;
				s->np_pending_fail_step = 0;
				np_fail(s, step);
				break;
			}
			int next = s->np_sub + 1;
			int end = sub->host == MENU_AP_SCREEN_SELECT_WORLD
				? NP_SUB_SWORLD_END
				: NP_SUB_MPS_END;
			if(next < end) {
				s->np_sub = next;
				goto_step(s, STEP_NP_SUB_BEGIN);
				break;
			}
			goto_step(s, sub->host == MENU_AP_SCREEN_SELECT_WORLD
							 ? STEP_NP_SWORLD_CLOSE
							 : STEP_NP_MP_CLOSE);
			break;
		}
		case STEP_NP_SWORLD_CLOSE:
			/* NOT an activate. The last sub-leg left the ring wherever it
			 * ended -- on a Select World arrival that is a row list, and an
			 * activate there is ACTIVATE(selected) on a ROW, i.e.
			 * sworld_play_selected(): it would LOAD a world. The strip's
			 * Cancel button would be one RIGHT plus a walk away (A15), but
			 * Close reaches the same Beta id 0 in one press and is the leg's
			 * one sanctioned way out of every screen. */
			np_close(s, out, STEP_NP_WAIT_TITLE_A);
			break;
		case STEP_NP_WAIT_TITLE_A:
			np_wait(s, screen, MENU_AP_SCREEN_TITLE, dt, STEP_NP_WAIT_TITLE_A,
					STEP_NP_TEXPACKS_NAV);
			break;
		case STEP_NP_TEXPACKS_NAV:
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_TITLE,
							 obs.title_focus, MENU_AP_TITLE_PACKS)) {
				case WALK_REACHED: goto_step(s, STEP_NP_TEXPACKS_ACT); break;
				case WALK_FAILED: np_fail(s, STEP_NP_TEXPACKS_NAV); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_NP_TEXPACKS_ACT:
			if(screen != MENU_AP_SCREEN_TITLE
			   || obs.title_focus != MENU_AP_TITLE_PACKS) {
				np_fail(s, STEP_NP_TEXPACKS_ACT);
				break;
			}
			out->press = true;
			out->button = IB_GUI_CLICK;
			goto_step(s, STEP_NP_WAIT_TEXPACKS);
			break;
		case STEP_NP_WAIT_TEXPACKS:
			np_wait(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt,
					STEP_NP_WAIT_TEXPACKS, STEP_NP_TEXPACKS_DWELL);
			break;
		case STEP_NP_TEXPACKS_DWELL:
			np_dwell(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt,
					 STEP_NP_TEXPACKS_DWELL, STEP_NP_TEXPACKS_SCROLL);
			break;
		case STEP_NP_TEXPACKS_SCROLL:
			if(screen != MENU_AP_SCREEN_TEXTUREPACKS || obs.texpacks_busy
			   || obs.texpacks_rows <= 0) {
				np_fail(s, STEP_NP_TEXPACKS_SCROLL);
				break;
			}
			switch(np_walk(s, out,
						   obs.texpacks_selected == obs.texpacks_rows - 1
							   && !s->repeat_phase,
						   IB_GUI_DOWN, NP_WALK_MAX_CONVERGE)) {
				case NP_WALK_DONE:
					goto_step(s, STEP_NP_TEXPACKS_SCROLL_DWELL);
					break;
				case NP_WALK_EXHAUSTED:
					np_fail(s, STEP_NP_TEXPACKS_SCROLL);
					break;
				case NP_WALK_PENDING: break;
			}
			break;
		case STEP_NP_TEXPACKS_SCROLL_DWELL:
			np_dwell(s, screen, MENU_AP_SCREEN_TEXTUREPACKS, dt,
					 STEP_NP_TEXPACKS_SCROLL_DWELL, STEP_NP_TEXPACKS_CLOSE);
			break;
		case STEP_NP_TEXPACKS_CLOSE:
			/* Close only -- never the activate that would APPLY the focused
			 * pack. The three deliberate applies are the opt-in pack legs
			 * near the top of this script; this leg changes nothing. */
			np_close(s, out, STEP_NP_WAIT_TITLE_B);
			break;
		case STEP_NP_WAIT_TITLE_B:
			np_wait(s, screen, MENU_AP_SCREEN_TITLE, dt, STEP_NP_WAIT_TITLE_B,
					STEP_NP_MP_NAV);
			break;
		case STEP_NP_MP_NAV:
			/* Multiplayer now lives behind Play -> Join, not on the title. */
			switch(step_walk(s, out, screen == MENU_AP_SCREEN_TITLE,
							 obs.title_focus, MENU_AP_TITLE_PLAY)) {
				case WALK_REACHED:
					out->press = true;
					out->button = IB_GUI_CLICK;
					goto_step(s, STEP_NP_MP_ACT);
					break;
				case WALK_FAILED: np_fail(s, STEP_NP_MP_NAV); break;
				case WALK_PENDING: break;
			}
			break;
		case STEP_NP_MP_ACT: {
			if(screen == MENU_AP_SCREEN_TITLE) {
				if((s->timer += dt) >= WAIT_MENU)
					np_fail(s, STEP_NP_MP_ACT);
				break;
			}
			if(screen != MENU_AP_SCREEN_SELECT_WORLD) {
				np_fail(s, STEP_NP_MP_ACT);
				break;
			}
			struct np_sub_def load = {.host = MENU_AP_SCREEN_SELECT_WORLD};
			if(obs.sworld_focus == MENU_AP_SWORLD_TAB_JOIN) {
				if(!s->repeat_phase) {
					out->press = true;
					out->button = IB_GUI_CLICK;
					goto_step(s, STEP_NP_WAIT_MP);
				} else
					s->repeat_phase = false;
			} else {
				if(!s->repeat_phase) {
					out->press = true;
					out->button = np_route_button(&load, obs.sworld_focus,
												 MENU_AP_SWORLD_TAB_JOIN);
					s->repeat_phase = true;
				} else
					s->repeat_phase = false;
			}
			break;
		}
		case STEP_NP_WAIT_MP:
			np_wait(s, screen, MENU_AP_SCREEN_MULTIPLAYER, dt, STEP_NP_WAIT_MP,
					STEP_NP_MP_DWELL);
			break;
		case STEP_NP_MP_DWELL:
			/* Same shape as the Select World dwell: the dwell proves liveness
			 * on the host, then the SHARED walk runs this screen's four
			 * sub-legs and returns to STEP_NP_MP_CLOSE when they are done. */
			if(step_dwell(s, screen, MENU_AP_SCREEN_MULTIPLAYER, dt,
						  DWELL_MENU)) {
				s->np_sub = NP_SUB_MPS_FIRST;
				goto_step(s, STEP_NP_SUB_BEGIN);
			} else if(screen != MENU_AP_SCREEN_MULTIPLAYER
					  && ++s->repeat_done > NP_DWELL_MAX_OFF_TICKS)
				np_fail(s, STEP_NP_MP_DWELL);
			break;
		case STEP_NP_MP_CLOSE:
			/* Same rule as Select World, with a sharper edge: an activate
			 * with the ring on a server ROW is a JOIN. Add / Edit / Direct
			 * Connect / the name editor all live in the action strip and
			 * wait for part 2. */
			np_close(s, out, STEP_NP_WAIT_TITLE_C);
			break;
		case STEP_NP_WAIT_TITLE_C:
			np_wait(s, screen, MENU_AP_SCREEN_TITLE, dt, STEP_NP_WAIT_TITLE_C,
					STEP_NP_TITLE_DWELL_END);
			break;
		case STEP_NP_TITLE_DWELL_END:
			np_dwell(s, screen, MENU_AP_SCREEN_TITLE, dt,
					 STEP_NP_TITLE_DWELL_END, STEP_NP_LEG_DONE);
			break;
		case STEP_NP_LEG_DONE:
			s->np_leg_done = true;
			goto_step(s, STEP_DONE);
			break;
		case STEP_NP_FAILED:
			/* TERMINAL. np_failed_step already names which wait ran out. */
			s->finished = true;
			break;

		case STEP_DONE:
		default: s->finished = true; break;
	}

	/* The stall guard: a tick with NO progress accrues; a step change (any
	 * goto_step, re-entries included), a press, or an observed change
	 * resets it. */
	unsigned sig = stall_signature(screen, obs);
	bool progressed = out->press || sig != s->stall_sig;
	s->stall_sig = sig;
	if(progressed)
		s->stall_timer = 0.0f;
	else if(!s->finished && s->step == step_at_entry
	   && s->step != STEP_WAIT_INGAME) {
		s->stall_timer += dt;
		if(s->stall_timer >= STALL_LIMIT) {
			s->stalled = true;
			s->finished = true;
		}
	}
	/* EVERY abandonment is named (hw1026c: the leg died with only
	 * STEP_NP_FAILED and np_flags 0x10 on the tape, and np_failed_step never
	 * left the console). map_fail is 1 + the ordinal that gave up -- the
	 * specific gate a named failure recorded, else the step that was
	 * running -- and 0 only for a run that has not ended or ended at
	 * STEP_DONE. The heartbeat carries it (hw_telemetry.h map_fail). */
	if(s->finished && !s->map_fail && s->step != STEP_DONE) {
		int at = s->np_failed_step ? s->np_failed_step
			: s->walk_failed_step ? s->walk_failed_step
			: s->cworld_failed_step ? s->cworld_failed_step
			: step_at_entry;
		s->map_fail = at + 1;
	}

	return !s->finished;
}

/* ======================================================================
 * ui-build-1026/T16a: the world_copy_convert route (see the header). A
 * separate machine: nothing above reads or writes its state, so the
 * menus_dwell ordinals and their contract are untouched.
 * ====================================================================== */
#include <stdio.h>

static const char* const copy_variant_names[MENU_AP_COPY_VARIANT_COUNT] = {
	"ok", "low_space", "write_error", "interrupt", "recover"};

const char* menu_ap_copy_variant_name(enum menu_ap_copy_variant v) {
	return (unsigned)v < MENU_AP_COPY_VARIANT_COUNT ? copy_variant_names[v]
													: "invalid";
}

int menu_ap_copy_variant_parse(const char* name) {
	for(int k = 0; name && k < MENU_AP_COPY_VARIANT_COUNT; k++)
		if(strcmp(name, copy_variant_names[k]) == 0)
			return k;
	return -1;
}

static void copy_latch(struct menu_ap_copy_state* s, int marker);
static void copy_finish(struct menu_ap_copy_state* s, bool ok, int failed_at);
static size_t copy_json_escape(char* out, size_t cap, const char* in);

bool menu_ap_copy_card_wanted(const struct menu_ap_copy_state* s) {
	return s->finished && !s->card_taken;
}

void menu_ap_copy_set_card(struct menu_ap_copy_state* s, bool root_ok,
						   const char* const* names, int count) {
	if(s->card_taken)
		return;
	s->card_taken = true;
	s->card_root_ok = root_ok;
	s->card_count = 0;
	s->card_truncated = false;
	for(int k = 0; names && k < count; k++) {
		if(!names[k])
			continue;
		if(s->card_count >= MENU_AP_COPY_CARD_NAMES
		   || strlen(names[k]) >= MENU_AP_COPY_CARD_NAME_MAX) {
			s->card_truncated = true;
			continue;
		}
		snprintf(s->card_names[s->card_count++], MENU_AP_COPY_CARD_NAME_MAX,
				 "%s", names[k]);
	}
	copy_latch(s, MENU_AP_WCC_CARD);
	/* A listing whose escaped line would not fit is sent EMPTY and
	 * truncated (a named FAIL), never held back: END must still go out. The
	 * length is COUNTED (no buffer): the fixed part with truncated=true is
	 * the longer spelling, plus "\"name\"," per name. */
	size_t need = (size_t)snprintf(NULL, 0,
		"{\"event\":\"wcc_card\",\"root_ok\":false,\"truncated\":false,"
		"\"names\":[]}");
	for(int k = 0; k < s->card_count; k++)
		need += copy_json_escape(NULL, 0, s->card_names[k]) + 3;
	if(need >= MENU_AP_COPY_LINE_MAX) {
		s->card_count = 0;
		s->card_truncated = true;
	}
}

bool menu_ap_copy_root_isolated(const char* configured, const char* expected) {
	if(!configured || !expected || strcmp(configured, expected) != 0)
		return false;
	const char* id = strstr(expected, "wcc_runs/");
	if(!id || (id != expected && id[-1] != '/'))
		return false;
	id += strlen("wcc_runs/");
	size_t len = strlen(id);
	if(len < 1 || len > 40)
		return false;
	for(size_t k = 0; k < len; k++) {
		char c = id[k];
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			 || (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

void menu_ap_copy_refuse(struct menu_ap_copy_state* s,
						 enum menu_ap_copy_variant variant, const char* fixture) {
	menu_ap_copy_reset(s, variant, fixture);
	if(!s->finished)
		copy_finish(s, false, MENU_AP_COPY_STEP_ISOLATION);
	else
		s->failed_step = MENU_AP_COPY_STEP_ISOLATION;
	menu_ap_copy_set_card(s, false, NULL, 0);
}

/* JSON string body: quote/backslash escaped, controls and non-ASCII as \u00XX
 * of the byte (lossless enough to tell names apart; never malformed). */
static size_t copy_json_escape(char* out, size_t cap, const char* in) {
	size_t n = 0;
	if(!out)
		cap = (size_t)-1; /* count only */
	for(const unsigned char* p = (const unsigned char*)in; *p; p++) {
		char tmp[8];
		int w;
		if(*p == '"' || *p == '\\')
			w = snprintf(tmp, sizeof(tmp), "\\%c", *p);
		else if(*p < 0x20 || *p >= 0x7F)
			w = snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
		else
			w = snprintf(tmp, sizeof(tmp), "%c", *p);
		if(n + (size_t)w + 1 > cap)
			return (size_t)-1;
		if(out)
			memcpy(out + n, tmp, (size_t)w);
		n += (size_t)w;
	}
	if(out && cap)
		out[n] = '\0';
	return n;
}

unsigned menu_ap_copy_required_markers(enum menu_ap_copy_variant v) {
	unsigned all = (1u << MENU_AP_WCC_COUNT) - 1u;
	if(v == MENU_AP_COPY_INTERRUPT)
		return all
			& ~((1u << MENU_AP_WCC_RESULT) | (1u << MENU_AP_WCC_LIST_POST));
	return all;
}

/* Fixture names travel in a JSON line and name a directory: a closed
 * character set, so neither can be malformed. */
static bool copy_fixture_name_ok(const char* name) {
	if(!name || !name[0] || strlen(name) >= MENU_AP_COPY_FIXTURE_MAX)
		return false;
	for(const char* p = name; *p; p++) {
		char c = *p;
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			 || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

static void copy_enter(struct menu_ap_copy_state* s, int step) {
	s->step = step;
	s->timer = 0.0F;
	s->settle = 0.0F;
	s->presses = 0;
	s->press_phase = false;
}

static void copy_latch(struct menu_ap_copy_state* s, int marker) {
	s->markers_ready |= 1u << marker;
}

static void copy_finish(struct menu_ap_copy_state* s, bool ok, int failed_at) {
	s->ok = ok;
	s->failed_step = ok ? 0 : failed_at;
	s->finished = true;
	copy_enter(s, ok ? MENU_AP_COPY_STEP_DONE
				  : failed_at == MENU_AP_COPY_STEP_UNOBSERVED
						|| failed_at == MENU_AP_COPY_STEP_ISOLATION
				  ? failed_at
				  : MENU_AP_COPY_STEP_FAILED);
	copy_latch(s, MENU_AP_WCC_END);
}

void menu_ap_copy_reset(struct menu_ap_copy_state* s,
						enum menu_ap_copy_variant variant, const char* fixture) {
	memset(s, 0, sizeof(*s));
	s->variant = variant;
	s->pre_fixture_row = s->pre_backup_row = -1;
	s->post_fixture_row = s->post_backup_row = -1;
	s->result_state = MENU_AP_CB_NONE;
	s->pct_max = s->pct_last = -1;
	s->monotonic = true;
	bool name_ok = copy_fixture_name_ok(fixture);
	snprintf(s->fixture, sizeof(s->fixture), "%s", name_ok ? fixture : "invalid");
	copy_enter(s, MENU_AP_COPY_STEP_WAIT_TITLE);
	copy_latch(s, MENU_AP_WCC_BEGIN);
	/* A bad variant or fixture name never reaches a single press. */
	if((unsigned)variant >= MENU_AP_COPY_VARIANT_COUNT || !name_ok)
		copy_finish(s, false, MENU_AP_COPY_STEP_WAIT_TITLE);
}

/* One press edge every two ticks (press, then a released tick), so every
 * press is a real edge the screen's input_pressed sees exactly once. */
static void copy_press(struct menu_ap_copy_state* s,
					   struct menu_ap_copy_output* out, enum input_button b) {
	out->press = true;
	out->button = b;
	s->press_phase = true;
	s->presses++;
}

static void copy_latch_list(struct menu_ap_copy_obs obs, int* rows, int* fixture,
							int* backup, bool* needs, bool* partial) {
	*rows = obs.sworld_rows;
	*fixture = obs.fixture_row;
	*backup = obs.backup_row;
	*needs = obs.fixture_needs_convert;
	*partial = obs.partial_visible;
}

static void copy_latch_volume(struct menu_ap_copy_state* s,
							  struct menu_ap_copy_obs obs) {
	if(s->volume_latched)
		return;
	s->volume_latched = true;
	s->free_bytes = obs.cb_free_bytes;
	s->cluster_bytes = obs.cb_cluster_bytes;
	s->required_bytes = obs.cb_required_bytes;
	copy_latch(s, MENU_AP_WCC_VOLUME);
}

static void copy_latch_result(struct menu_ap_copy_state* s,
							  struct menu_ap_copy_obs obs) {
	copy_latch_volume(s, obs);
	s->result_state = obs.cb_state;
	s->result_fail = obs.cb_fail;
	s->result_copy_error = obs.cb_copy_error;
	s->result_outcome = obs.cb_convert_outcome;
	s->result_published = obs.cb_backup_published;
	s->cleanup_done = obs.cb_cleanup_done;
	copy_latch(s, MENU_AP_WCC_COPY);
	copy_latch(s, MENU_AP_WCC_RESULT);
}

static bool copy_is_result(int st) {
	return st == MENU_AP_CB_ERROR || st == MENU_AP_CB_DONE
		|| st == MENU_AP_CB_SAVE_PENDING || st == MENU_AP_CB_CONVERT_ERROR;
}

/* The list must hold settled (every row resolved) for a dwell before its
 * facts are taken: rows resolve over several frames. */
static bool copy_list_settled(struct menu_ap_copy_state* s,
							  enum menu_ap_screen screen,
							  struct menu_ap_copy_obs obs, float dt) {
	if(screen == MENU_AP_SCREEN_SELECT_WORLD && obs.sworld_settled)
		s->settle += dt;
	else
		s->settle = 0.0F;
	return s->settle >= MENU_AP_COPY_DWELL_S;
}

bool menu_ap_copy_tick(struct menu_ap_copy_state* s, enum menu_ap_screen screen,
					   struct menu_ap_copy_obs obs, float dt,
					   struct menu_ap_copy_output* out) {
	*out = (struct menu_ap_copy_output) {0};
	if(s->finished)
		return false;
	if(!obs.observed) {
		copy_finish(s, false, MENU_AP_COPY_STEP_UNOBSERVED);
		return false;
	}
	s->timer += dt;
	int at = s->step;
	float budget = at == MENU_AP_COPY_STEP_RUN ? MENU_AP_COPY_RUN_S
											   : MENU_AP_COPY_WAIT_S;
	if(s->presses > MENU_AP_COPY_PRESS_MAX || s->timer >= budget) {
		copy_finish(s, false, at);
		return false;
	}
	/* The released tick after every press: nothing is decided on it, so the
	 * next decision reads an observation taken after the screen consumed
	 * the edge. */
	if(s->press_phase) {
		s->press_phase = false;
		return true;
	}

	switch(at) {
		case MENU_AP_COPY_STEP_WAIT_TITLE:
			if(screen == MENU_AP_SCREEN_TITLE)
				copy_enter(s, MENU_AP_COPY_STEP_TITLE_PLAY);
			break;
		case MENU_AP_COPY_STEP_TITLE_PLAY:
			if(screen == MENU_AP_SCREEN_SELECT_WORLD)
				copy_enter(s, MENU_AP_COPY_STEP_WAIT_LIST);
			else if(screen != MENU_AP_SCREEN_TITLE)
				copy_finish(s, false, at);
			else if(obs.title_focus == MENU_AP_TITLE_PLAY)
				copy_press(s, out, IB_GUI_CLICK);
			else
				copy_press(s, out, obs.title_focus > MENU_AP_TITLE_PLAY
								   ? IB_GUI_UP : IB_GUI_DOWN);
			break;
		case MENU_AP_COPY_STEP_WAIT_LIST:
			if(copy_list_settled(s, screen, obs, dt))
				copy_enter(s, MENU_AP_COPY_STEP_LIST_PRE);
			break;
		case MENU_AP_COPY_STEP_LIST_PRE:
			copy_latch_list(obs, &s->pre_rows, &s->pre_fixture_row,
							&s->pre_backup_row, &s->pre_needs_convert,
							&s->pre_partial);
			copy_latch(s, MENU_AP_WCC_LIST_PRE);
			/* Never a user world: no fixture row, or a fixture that is not
			 * tagged, ends the route before any press on the list. */
			if(screen != MENU_AP_SCREEN_SELECT_WORLD || s->pre_fixture_row < 0
			   || !s->pre_needs_convert)
				copy_finish(s, false, at);
			else
				copy_enter(s, MENU_AP_COPY_STEP_SEEK_ROW);
			break;
		case MENU_AP_COPY_STEP_SEEK_ROW:
			if(screen != MENU_AP_SCREEN_SELECT_WORLD || obs.fixture_row < 0)
				copy_finish(s, false, at);
			else if(obs.sworld_selected == obs.fixture_row
					&& (obs.sworld_focus == MENU_AP_SWORLD_LIST
						|| obs.sworld_focus == MENU_AP_SWORLD_PLAY))
				copy_enter(s, MENU_AP_COPY_STEP_SEEK_PLAY);
			else if(obs.sworld_focus == MENU_AP_SWORLD_LIST)
				copy_press(s, out, obs.sworld_selected < obs.fixture_row
								   ? IB_GUI_DOWN : IB_GUI_UP);
			else if(obs.sworld_focus == MENU_AP_SWORLD_PLAY)
				copy_press(s, out, IB_GUI_LEFT); /* Play -> the row list */
			else
				copy_press(s, out, IB_GUI_UP);
			break;
		case MENU_AP_COPY_STEP_SEEK_PLAY:
			if(screen == MENU_AP_SCREEN_CONVERT_BACKUP) {
				copy_enter(s, MENU_AP_COPY_STEP_WAIT_ASK);
				break;
			}
			if(screen != MENU_AP_SCREEN_SELECT_WORLD) {
				/* The transient load screen may sit between; anything else
				 * (in game!) means a world opened without the ask. */
				if(screen != MENU_AP_SCREEN_OTHER)
					copy_finish(s, false, at);
				break;
			}
			/* Activate ONLY on Play with the fixture row selected and still
			 * tagged, on this very frame. */
			if(obs.sworld_selected != obs.fixture_row || obs.fixture_row < 0
			   || !obs.fixture_needs_convert)
				copy_finish(s, false, at);
			else if(obs.sworld_focus == MENU_AP_SWORLD_PLAY)
				copy_press(s, out, IB_GUI_CLICK);
			else if(obs.sworld_focus == MENU_AP_SWORLD_LIST)
				copy_press(s, out, IB_GUI_RIGHT); /* row -> Play */
			else
				copy_finish(s, false, at);
			break;
		case MENU_AP_COPY_STEP_WAIT_ASK:
			if(screen == MENU_AP_SCREEN_CONVERT_BACKUP
			   && obs.cb_state == MENU_AP_CB_ASK) {
				s->ask_safe_default = obs.cb_action == MENU_AP_CB_ACT_CANCEL;
				copy_latch(s, MENU_AP_WCC_ASK);
				copy_enter(s, MENU_AP_COPY_STEP_SEEK_BACKUP);
			} else if(screen != MENU_AP_SCREEN_CONVERT_BACKUP) {
				copy_finish(s, false, at);
			}
			break;
		case MENU_AP_COPY_STEP_SEEK_BACKUP:
			if(screen != MENU_AP_SCREEN_CONVERT_BACKUP) {
				copy_finish(s, false, at);
			} else if(obs.cb_state != MENU_AP_CB_ASK) {
				/* Only Back up & convert leaves the ask for these states;
				 * anything else is a press that went elsewhere. */
				if(obs.cb_state == MENU_AP_CB_VOLUME
				   || obs.cb_state == MENU_AP_CB_COPYING
				   || obs.cb_state == MENU_AP_CB_ERROR
				   || obs.cb_state == MENU_AP_CB_CLEANING)
					copy_enter(s, MENU_AP_COPY_STEP_RUN);
				else
					copy_finish(s, false, at);
				/* this tick's observation belongs to RUN too */
				if(!s->finished)
					return menu_ap_copy_tick(s, screen, obs, 0.0F, out);
			} else if(obs.cb_action == MENU_AP_CB_ACT_BACKUP) {
				/* activate ONLY on Back up & convert, never on Convert
				 * without backup */
				copy_press(s, out, IB_GUI_CLICK);
			} else {
				copy_press(s, out, IB_GUI_UP);
			}
			break;
		case MENU_AP_COPY_STEP_RUN: {
			if(screen != MENU_AP_SCREEN_CONVERT_BACKUP) {
				copy_finish(s, false, at);
				break;
			}
			int st = obs.cb_state;
			if(st == MENU_AP_CB_COPYING) {
				s->saw_copying = true;
				if(obs.cb_percent >= 0) {
					copy_latch_volume(s, obs); /* survey done, start made */
					if(obs.cb_percent < s->pct_last)
						s->monotonic = false;
					s->pct_last = obs.cb_percent;
					if(obs.cb_percent > s->pct_max)
						s->pct_max = obs.cb_percent;
				}
				if(s->variant == MENU_AP_COPY_INTERRUPT && obs.cb_percent >= 1
				   && obs.cb_percent < 100) {
					s->interrupt_requested = true;
					out->interrupt = true;
					s->result_state = st;
					copy_latch(s, MENU_AP_WCC_COPY);
					copy_finish(s, true, 0);
				}
			} else if(st == MENU_AP_CB_CLEANING) {
				s->saw_cleaning = true;
				copy_latch_volume(s, obs);
			} else if(st == MENU_AP_CB_CONVERTING) {
				copy_latch_volume(s, obs);
			} else if(copy_is_result(st)) {
				copy_latch_result(s, obs);
				copy_enter(s, MENU_AP_COPY_STEP_RESULT_DWELL);
			} else if(st != MENU_AP_CB_VOLUME) {
				copy_finish(s, false, at);
			}
			break;
		}
		case MENU_AP_COPY_STEP_RESULT_DWELL:
			if(screen != MENU_AP_SCREEN_CONVERT_BACKUP
			   || obs.cb_state != s->result_state)
				copy_finish(s, false, at);
			else if(s->timer >= MENU_AP_COPY_DWELL_S)
				copy_enter(s, MENU_AP_COPY_STEP_LEAVE);
			break;
		case MENU_AP_COPY_STEP_LEAVE: {
			if(screen == MENU_AP_SCREEN_SELECT_WORLD) {
				copy_enter(s, MENU_AP_COPY_STEP_WAIT_LIST_POST);
				break;
			}
			if(screen != MENU_AP_SCREEN_CONVERT_BACKUP) {
				copy_finish(s, false, at); /* Play world must never run */
				break;
			}
			int want = s->result_state == MENU_AP_CB_ERROR ? MENU_AP_CB_ACT_CANCEL
														   : MENU_AP_CB_ACT_BACK;
			if(obs.cb_action == want)
				copy_press(s, out, IB_GUI_CLICK);
			else
				copy_press(s, out, IB_GUI_DOWN); /* Back / Cancel are last */
			break;
		}
		case MENU_AP_COPY_STEP_WAIT_LIST_POST:
			if(screen != MENU_AP_SCREEN_SELECT_WORLD
			   && screen != MENU_AP_SCREEN_OTHER) {
				copy_finish(s, false, at);
			} else if(copy_list_settled(s, screen, obs, dt)) {
				copy_latch_list(obs, &s->post_rows, &s->post_fixture_row,
								&s->post_backup_row, &s->post_needs_convert,
								&s->post_partial);
				copy_latch(s, MENU_AP_WCC_LIST_POST);
				copy_finish(s, true, 0);
			}
			break;
		default: copy_finish(s, false, at); break;
	}
	return !s->finished;
}

#define COPY_B(v) ((v) ? "true" : "false")

int menu_ap_copy_format_marker(const struct menu_ap_copy_state* s, int marker,
							   char* buf, size_t cap) {
	int n = -1;
	bool pre = marker == MENU_AP_WCC_LIST_PRE;
	switch(marker) {
		case MENU_AP_WCC_BEGIN:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_begin\",\"variant\":\"%s\","
						 "\"fixture\":\"%s\"}",
						 menu_ap_copy_variant_name(s->variant), s->fixture);
			break;
		case MENU_AP_WCC_LIST_PRE:
		case MENU_AP_WCC_LIST_POST:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_list_%s\",\"rows\":%d,"
						 "\"fixture_row\":%d,\"needs_convert\":%s,"
						 "\"backup_row\":%d,\"partial_visible\":%s}",
						 pre ? "pre" : "post", pre ? s->pre_rows : s->post_rows,
						 pre ? s->pre_fixture_row : s->post_fixture_row,
						 COPY_B(pre ? s->pre_needs_convert
									: s->post_needs_convert),
						 pre ? s->pre_backup_row : s->post_backup_row,
						 COPY_B(pre ? s->pre_partial : s->post_partial));
			break;
		case MENU_AP_WCC_ASK:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_ask\",\"safe_default\":%s}",
						 COPY_B(s->ask_safe_default));
			break;
		case MENU_AP_WCC_VOLUME:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_volume\",\"free_bytes\":%llu,"
						 "\"cluster_bytes\":%lu,\"required_bytes\":%llu}",
						 (unsigned long long)s->free_bytes,
						 (unsigned long)s->cluster_bytes,
						 (unsigned long long)s->required_bytes);
			break;
		case MENU_AP_WCC_COPY:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_copy\",\"saw_copying\":%s,"
						 "\"saw_cleaning\":%s,\"cleanup_done\":%s,"
						 "\"pct_max\":%d,\"monotonic\":%s,"
						 "\"interrupted\":%s}",
						 COPY_B(s->saw_copying), COPY_B(s->saw_cleaning),
						 COPY_B(s->cleanup_done), s->pct_max,
						 COPY_B(s->monotonic),
						 COPY_B(s->interrupt_requested));
			break;
		case MENU_AP_WCC_RESULT:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_result\",\"state\":%d,\"fail\":%d,"
						 "\"copy_error\":%d,\"outcome\":%d,"
						 "\"backup_published\":%s}",
						 s->result_state, s->result_fail, s->result_copy_error,
						 s->result_outcome, COPY_B(s->result_published));
			break;
		case MENU_AP_WCC_CARD: {
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_card\",\"root_ok\":%s,"
						 "\"truncated\":%s,\"names\":[",
						 COPY_B(s->card_root_ok), COPY_B(s->card_truncated));
			for(int k = 0; n >= 0 && (size_t)n < cap && k < s->card_count; k++) {
				char esc[MENU_AP_COPY_CARD_NAME_MAX * 6];
				if(copy_json_escape(esc, sizeof(esc), s->card_names[k])
				   == (size_t)-1)
					return -1;
				n += snprintf(buf + n, cap - (size_t)n, "%s\"%s\"",
							  k ? "," : "", esc);
			}
			if(n >= 0 && (size_t)n < cap)
				n += snprintf(buf + n, cap - (size_t)n, "]}");
			break;
		}
		case MENU_AP_WCC_END:
			n = snprintf(buf, cap,
						 "{\"event\":\"wcc_end\",\"variant\":\"%s\",\"ok\":%s,"
						 "\"step\":%d,\"failed_step\":%d}",
						 menu_ap_copy_variant_name(s->variant), COPY_B(s->ok),
						 s->step, s->failed_step);
			break;
		default: return -1;
	}
	if(n < 0 || (size_t)n >= cap)
		return -1;
	return n;
}

int menu_ap_copy_next_marker(const struct menu_ap_copy_state* s, char* buf,
							 size_t cap) {
	unsigned pending = s->markers_ready & ~s->markers_sent;
	for(int k = 0; k < MENU_AP_WCC_COUNT; k++) {
		if(!(pending & (1u << k)))
			continue;
		/* END goes last: the collector reads it as "nothing follows" --
		 * so it also waits for the card listing. */
		if(k == MENU_AP_WCC_END
		   && ((pending & ((1u << k) - 1u)) || !s->card_taken))
			continue;
		return menu_ap_copy_format_marker(s, k, buf, cap) >= 0 ? k : -1;
	}
	return -1;
}

void menu_ap_copy_marker_sent(struct menu_ap_copy_state* s, int marker) {
	if(marker >= 0 && marker < MENU_AP_WCC_COUNT)
		s->markers_sent |= 1u << marker;
}
