/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/*
	menus_dwell hardware profile: the PURE menu-navigation script.

	This is the deterministic state machine that drives the title/select-world/
	texture-packs/create-world/sign/inventory/furnace/chest/double-chest/dispenser/pause/options screens with zero human input,
	so the recently-landed menu/OSK code (pointer rows, gutil_button migration,
	sign-OSK, pause/options) can be hardware-certified. It contains NO gfx,
	game_state, or screen.h dependency -- the caller (menu_autopilot.c) supplies
	an abstract "which screen is active" classification, an abstract "is the
	create-world Game Mode selector on Survival" observation, and physical
	width/height, and reads back an abstract "press this button" / "assert the
	pointer at this named target" instruction. This keeps the state machine
	itself PC-unit-testable with plain asserts (CODESTYLE 3.2: prefer pure,
	injected-world logic for anything testable), the same shape as
	block/redstone_power.c's callback-world model.

	Design notes (see the commit body for the full per-screen trace):
	  * Every screen this reaches (title, texture-packs, select-world's row
	    list, create-world's form ring, pause, options, the sign edit grid) is
	    driven through the SAME D-pad/GUI_CLICK primitives every screen
	    already exposes for the Classic/GameCube controller -- no new
	    interaction semantics are added to any screen.
	  * Exactly ONE interaction (select-world's "Create New World" button)
	    has no D-pad equivalent (GUI_CLICK with no pointer always activates
	    the row-selected world, screen_select_world.c sworld_activate_button's
	    default path) -- MENU_AP_PT_SWORLD_CREATE asserts a synthetic pointer
	    there for exactly one step, the same input shape a real Wiimote IR
	    pointer produces. Every other step is pure D-pad/button.
	  * THE NO-POINTER LEG (push-0921 A7 part 1, Astra Lane A item 5): the
	    tail of the script re-walks the front end with the pointer made
	    UNAVAILABLE to the screens for the leg's whole duration
	    (menu_ap_output.pointer_suppressed), reaching Select World, Texture
	    Packs and Multiplayer from the title and backing out of each with the
	    Close role alone. It is the LAST leg and its terminal step is the
	    PASS ordinal, so the profile's gate cannot be met without it. Part 1
	    is deliberately COUNT-FREE: it activates nothing inside a list or an
	    action strip, because reaching a strip costs `rows + 1` presses and
	    this module is pure -- it cannot see the row count, and a wrong count
	    would leave the ring on a list row whose activate LOADS a world
	    (screen_select_world.c:1066) or JOINS a server
	    (screen_multiplayer_setup.c:1320). The strip steps (Create / Rename /
	    Delete / Add / Edit / Direct Connect) wait on the production
	    read-only focus observers being added under part 2; until they exist
	    NO press is issued that could activate an unobserved target.
	    (Part 2 gave the module a row count after all -- screen_sworld_row_
	    count through menu_ap_observation.sworld_rows -- and push-0921 A14
	    made the DOWN walk's press budget follow it. It is still a BUDGET and
	    not a route: the activate guard remains the observed focus, so a count
	    that arrives wrong costs a named failure, never a wrong press.
	    push-0921 A15 then removed the crossing altogether: both list screens
	    now enter their action strip on ONE GUI RIGHT off the CURRENT row, so
	    the walk costs the same on a 650-world card as on a three-world one,
	    and A14's converge became the named FALLBACK step
	    STEP_NP_SUB_TO_STRIP_DOWN. The guard is unchanged -- still the
	    observed focus, re-read on the very frame of the press.)
	  * Every step that can trigger a screen transition is followed by a
	    BOUNDED wait for the destination screen; a wait that times out
	    abandons the script (menu_ap_script_finished) rather than pressing
	    blindly on an unexpected screen. The one exception is the sign step:
	    reaching it depends on the EXISTING debug.autopilot "aim" + one-shot
	    place-pulse mechanism (first hw_run integration of that path), which
	    carries first-time hardware risk the other steps do not, so its wait
	    is SOFT -- a timeout skips forward to the pause/options steps instead
	    of abandoning the whole run (menu_ap_state.sign_wait_timed_out latches
	    so the caller can log which run this was). See docs/deviations.md for
	    the record.
	  * STEP_CWORLD_MODE_CONFIRM (2026-09-02 hardening): a single bare
	    one-frame press cannot be trusted to land -- staged_menus_dwell_
	    20260902_063552 (real-Wii) froze after a run whose adversarial review
	    named this exact step as the #1 suspect for creating the world
	    CREATIVE instead of SURVIVAL. The step now re-presses toward the
	    requested target (a fresh rising edge each attempt, never held across
	    ticks -- holding it would wrap back to the other mode, since there
	    are only two) until the caller's `cworld_mode_survival` observation
	    confirms the target landed, bounded by WAIT_MODE_SELECT; running out
	    that budget without ever confirming is a HARD, distinctly-coded
	    failure (STEP_CWORLD_MODE_FAILED) rather than a silent proceed-anyway.
	  * `target_creative` (2026-09-02, the --cw-cworld-mode knob): the SAME
	    confirm-loop generalized to either target instead of hardcoded to
	    Survival. menu_ap_script_reset() sets it once at arm time; the
	    default (false, Survival) reproduces the pre-knob behavior
	    byte-for-byte -- the script always visits Creative first regardless
	    of target (that hop is the fixed :8099 capture moment proving the
	    selector works), and STEP_CWORLD_MODE_CONFIRM either returns to
	    Survival (target_creative=false, unchanged) or verifies it stayed on
	    Creative (target_creative=true, zero extra presses needed since the
	    hop already landed there) before Create. This exists so a deliberate
	    hardware reproduction of a CREATIVE-world menus_dwell run (the exact
	    scenario staged_menus_dwell_20260902_063552 hit by accident) can be
	    requested on purpose -- HW_RUN_PROFILE_MENUS_DWELL_CREATIVE /
	    --cw-cworld-mode creative (source/game/hw_run.h) -- instead of never
	    being reproducible without a live selector bug.
	  * CREATE IS OBSERVED, NOT COUNTED (menus-dwell-walker-1026): the walker
	    no longer types into the create-world name editor. Since b472eb040
	    (09-22) that editor is widget_osk_frontend.c, which reads ONLY the
	    assigned player-0 owner frame (osk_frontend_input_frame); the
	    walker's synthetic presses reach input_pressed(), never an owner
	    frame, and the rig claims no player-0 controller -- so once open the
	    modal could never be closed by the walker, under any key layout, and
	    every menus_dwell since 09-21 stalled at STEP_WAIT_INGAME. The name
	    keeps Beta's default "New World" (Create is enabled), and the walk
	    to Create is driven by two observations of the REAL screen:
	    `cworld_editing` (the modal is open) and `cworld_create_ready` (the
	    ring is on an enabled Create and the modal is closed). A DOWN is
	    issued only while Create is not yet focused; activate is issued only
	    on a released frame that still observes Create ready. Any other
	    reading ends the run at STEP_CWORLD_CREATE_FAILED, with
	    cworld_failed_step naming which gate refused. Pinned against the real
	    screen + OSK by tests/test_menus_dwell_create_flow.c.
*/

#ifndef GAME_MENU_AUTOPILOT_SCRIPT_H
#define GAME_MENU_AUTOPILOT_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../platform/input.h"

/* The caller's classification of gstate.players[0].current_screen. MENU_AP_SCREEN_OTHER
 * covers every screen the script does not name explicitly (including the
 * transient screen_load_world) -- a step waiting for a specific screen simply
 * never matches it. */
enum menu_ap_screen {
	MENU_AP_SCREEN_OTHER = 0,
	MENU_AP_SCREEN_TITLE,
	MENU_AP_SCREEN_TEXTUREPACKS,
	MENU_AP_SCREEN_SELECT_WORLD,
	MENU_AP_SCREEN_CREATE_WORLD,
	MENU_AP_SCREEN_INGAME,
	MENU_AP_SCREEN_SIGN,
	MENU_AP_SCREEN_PAUSE,
	MENU_AP_SCREEN_OPTIONS,
	MENU_AP_SCREEN_INVENTORY, /* screen_inventory: the single-player full inventory (2026-09-11) */
	MENU_AP_SCREEN_FURNACE,	  /* screen_furnace (container legs, 2026-09-11) */
	MENU_AP_SCREEN_CHEST,	  /* screen_chest: single AND double chest (Beta uses one GUI) */
	MENU_AP_SCREEN_CHEST_SORT, /* screen_chest with its Sort dialog open */
	MENU_AP_SCREEN_DISPENSER,  /* screen_dispenser */
	MENU_AP_SCREEN_MULTIPLAYER, /* screen_multiplayer_setup, ANY view (push-0921 A7) */
	MENU_AP_SCREEN_RENAME_WORLD, /* screen_rename_world (A7 part 2) */
	MENU_AP_SCREEN_CONFIRM,		 /* screen_confirm, the generic yes/no modal (A7 part 2) */
	MENU_AP_SCREEN_CONVERT_BACKUP, /* screen_convert_backup (ui-build-1026/T16a; appended) */
};

/* What the IMPURE caller can see of the live front-end this tick (A7 part 2).
 * Everything here is a PRODUCTION, side-effect-free observer in
 * source/game/gui/screen.h -- this module never guesses at a screen's private
 * state, and the walk steps below are written so that NO activate is ever
 * issued unless the observation says the ring is on the named target this
 * very frame. The ints carry enum screen_sworld_focus / screen_mps_focus /
 * enum screen_mps_view; they are ints here so this header keeps its "no
 * gfx/game_state/screen.h dependency" property (see the file header). A
 * caller that cannot cheaply know a field may pass 0 (_NONE / _MAIN): the
 * walks then simply never confirm their target and fail or skip by name,
 * which is the safe direction. */
struct menu_ap_observation {
	bool cworld_mode_survival;
	int sworld_focus; /* screen_sworld_focus_target() */
	int mps_focus;	  /* screen_mps_focus_target() */
	int mps_view;	  /* screen_mps_view() */
	int mps_rows;	  /* screen_mps_server_count() */
	/* Are the Multiplayer focus, view, and row-count answers real? The screen lives in
	 * a different link pool from this walker's caller (CAVEX_MP_SOURCES vs
	 * the engine core), so its observers reach menu_autopilot.c through a
	 * registration hook that only a build with the MP screens linked can
	 * fill. False = "nobody answered", and the Multiplayer sub-legs are then
	 * NAMED SKIPS rather than 160 blind presses at a screen this build cannot
	 * see into. See menu_autopilot.h's menu_autopilot_set_mps_observers. */
	bool mps_observed;
	/* screen_sworld_row_count(): how many worlds the Select World list holds
	 * right now. The delete sub-leg's "nothing was destroyed" proof is a
	 * COUNT taken before and after -- checking that focus still reports a row
	 * only proves anything on a ONE-world card, which the maintainer's is not
	 * (review A9-5). 0 when the list is empty or not built. */
	int sworld_rows;
	/* screen_confirm_focus_is_safe(): the confirmation is up AND its
	 * highlighted answer is the non-destructive one its opener declared. The
	 * leg backs out with Close, which answers NO whatever the highlight is --
	 * so without this observation a regression in the safe-answer default
	 * would be invisible to the very run meant to catch it (review A9-6). */
	bool confirm_focus_safe;
	/* Create World (menus-dwell-walker-1026), both from the REAL screen
	 * (screen_cworld_editing / screen_cworld_create_ready):
	 *   cworld_editing      -- the name/seed editor modal is open.
	 *   cworld_create_ready -- the modal is closed AND the focus ring is on
	 *                          an ENABLED Create, so an activate press this
	 *                          frame would attempt the create.
	 * Read only by STEP_CWORLD_NAV_CREATE / STEP_CWORLD_SUBMIT. A caller that
	 * passes zeroes makes the walk run out and fail by name -- never a blind
	 * press on Create. */
	bool cworld_editing;
	bool cworld_create_ready;
	/* screen_pause_focused_action() / screen_title_focused_row(), as
	 * enum menu_ap_pause_focus / menu_ap_title_focus (NONE = -1 when that
	 * screen is not up). Read by the OBSERVED focus walks only. */
	int pause_focus;
	int title_focus;
	/* ui-build-1026 T15, Texture packs (screen.h screen_texpacks_*):
	 * row count, selected row (-1 none), the row the ring is on (-1 = a
	 * strip button), Apply focused / enabled, the pending choice is the
	 * selected row's pack, a reload is in flight, and the row the caller can
	 * put the synthetic pointer on (the first fully visible of selected+1,
	 * selected-1, selected; -1 when none is). */
	int texpacks_rows;
	int texpacks_selected;
	int texpacks_focused_row;
	bool texpacks_apply_focused;
	bool texpacks_apply_enabled;
	bool texpacks_pending_selected;
	bool texpacks_busy;
	int texpacks_pick_row;
	/* ui-build-1026 T15, Create World (screen.h screen_cworld_*): the focus
	 * target (enum menu_ap_cworld_focus), the Details card, the OSK modal's
	 * profile / focused key (-1 when closed), and the committed-text
	 * signature (name + seed; the draft while editing). */
	int cworld_focus;
	bool cworld_details_open;
	int cworld_osk_profile;
	int cworld_osk_target;
	unsigned cworld_text_sig;
	/* ui-build-1026 T15c, Inventory (screen_inventory_page /
	 * screen_inventory_crafting_tab): 0 Inventory / 1 Crafting / -1 not up,
	 * and the Crafting page's category tab (-1 off that page). */
	int inv_page;
	int inv_tab;
	/* BOOT-CREDITS-1028 T5 (hw1028c/d): the boot credit cards still own the
	 * front end (the title is drawn underneath but every menu input is
	 * consumed until admission). While true the script holds: no press, no
	 * dwell/wait/stall clock, no step change -- a walk started under the
	 * cards could only run out of presses (walk_failed_step=1). */
	bool front_end_blocked;
};

/* Mirrors of screen.h's enum screen_cworld_focus and widget_osk.h's OSK
 * profile/target values, by VALUE (pinned by _Static_assert in
 * menu_autopilot.c). */
enum menu_ap_cworld_focus {
	MENU_AP_CWORLD_NAME = 0,
	MENU_AP_CWORLD_SEED,
	MENU_AP_CWORLD_NEW_SEED,
	MENU_AP_CWORLD_MODE_SURVIVAL,
	MENU_AP_CWORLD_MODE_CREATIVE,
	MENU_AP_CWORLD_CREATE,
	MENU_AP_CWORLD_DETAILS,
	MENU_AP_CWORLD_CANCEL,
	MENU_AP_CWORLD_DETAILS_BACK,
};
#define MENU_AP_OSK_PROFILE_CREATE_NAME 1
#define MENU_AP_OSK_PROFILE_CREATE_SEED 2
#define MENU_AP_OSK_CHAR_FIRST 5 /* OSK_FRONT_CHAR_FIRST */
#define MENU_AP_OSK_CHAR_CELLS 30 /* page cells before OSK_FRONT_CASE */
#define MENU_AP_CRAFT_TABS 8 /* RECIPE_CATALOG_CATEGORIES (recipe_browser.h) */

/* Mirrors of screen.h's enum screen_pause_focus / screen_title_focus, by
 * VALUE (pinned by _Static_assert in menu_autopilot.c). */
enum menu_ap_pause_focus {
	MENU_AP_PAUSE_NONE = -1,
	MENU_AP_PAUSE_BACK = 0,
	MENU_AP_PAUSE_ACHIEVEMENTS,
	MENU_AP_PAUSE_OPTIONS,
	MENU_AP_PAUSE_TEXPACKS,
	MENU_AP_PAUSE_EXIT,
};
enum menu_ap_title_focus {
	MENU_AP_TITLE_NONE = -1,
	MENU_AP_TITLE_PLAY = 0,
	MENU_AP_TITLE_TUTORIAL, /* ui-build-1026 T03: Tutorial world row */
	MENU_AP_TITLE_PACKS,
	MENU_AP_TITLE_OPTIONS,
	MENU_AP_TITLE_QUIT,
};

/* The focus values this module compares against, mirrored from screen.h's
 * enums by VALUE. They are asserted equal to the real enums in
 * menu_autopilot.c (the one TU that sees both), so a drift in either enum
 * breaks the build instead of silently re-aiming a walk at the wrong
 * control -- the failure mode that makes a blind press dangerous. */
enum menu_ap_sworld_focus {
	MENU_AP_SWORLD_NONE = 0,
	MENU_AP_SWORLD_LIST,
	MENU_AP_SWORLD_SELECT,
	MENU_AP_SWORLD_RENAME,
	MENU_AP_SWORLD_DELETE,
	MENU_AP_SWORLD_CREATE,
	MENU_AP_SWORLD_CANCEL,
	MENU_AP_SWORLD_TAB_LOAD,
	MENU_AP_SWORLD_TAB_CREATE,
	MENU_AP_SWORLD_TAB_JOIN,
	MENU_AP_SWORLD_PREV,
	MENU_AP_SWORLD_NEXT,
	MENU_AP_SWORLD_PLAY,
	MENU_AP_SWORLD_RETRY,
	MENU_AP_SWORLD_OPTIONS,
	MENU_AP_SWORLD_MODAL_RENAME,
	MENU_AP_SWORLD_MODAL_DELETE,
	MENU_AP_SWORLD_MODAL_BACK,
};

enum menu_ap_mps_focus {
	MENU_AP_MPS_NONE = 0,
	MENU_AP_MPS_LIST,
	MENU_AP_MPS_NAME,
	MENU_AP_MPS_QUICK,
	MENU_AP_MPS_JOIN,
	MENU_AP_MPS_DIRECT,
	MENU_AP_MPS_ADD,
	MENU_AP_MPS_EDIT,
	MENU_AP_MPS_DELETE,
	MENU_AP_MPS_REFRESH,
	MENU_AP_MPS_BACK,
	MENU_AP_MPS_OPTIONS,
	MENU_AP_MPS_TAB_LOAD,
	MENU_AP_MPS_TAB_CREATE,
	MENU_AP_MPS_TAB_JOIN,
	MENU_AP_MPS_PREV,
	MENU_AP_MPS_NEXT,
};

enum menu_ap_mps_view {
	MENU_AP_MPS_VIEW_MAIN = 0,
	MENU_AP_MPS_VIEW_NAME,
	MENU_AP_MPS_VIEW_ADD,
	MENU_AP_MPS_VIEW_EDIT,
	MENU_AP_MPS_VIEW_DIRECT,
};

/* A synthetic-pointer target, resolved to physical pixel coordinates by the
 * IMPURE caller (menu_autopilot.c), which alone knows screen_select_world's
 * button geometry via screen_sworld_create_button_center(). Kept as a named
 * target rather than raw coordinates so this header stays free of any
 * screen-specific magic numbers. */
enum menu_ap_pointer_target {
	MENU_AP_PT_NONE = 0,
	MENU_AP_PT_SWORLD_CREATE,
	MENU_AP_PT_TEXPACKS_ROW, /* ui-build-1026 T15: menu_ap_output.pointer_row's card */
};

/* One tick's worth of synthesized input, resolved fresh by every
 * menu_ap_script_tick() call (never carries state between calls). */
struct menu_ap_output {
	bool press;				 /* fire `button` as a one-frame press-edge this tick */
	enum input_button button;	 /* valid iff press */
	enum menu_ap_pointer_target pointer_target; /* MENU_AP_PT_NONE = don't touch the pointer */
	int pointer_row; /* MENU_AP_PT_TEXPACKS_ROW only: which list row */
	/* THE NO-POINTER ASSERTION (push-0921 A7 part 1a, Astra Lane A item 5).
	 * The opposite of pointer_target: while true the screens must see
	 * pointer_available == FALSE through the SAME production query they read
	 * on a console with the Remote pointed away -- input_pointer()
	 * (source/platform/input.c), whose ONE menus_dwell seam is
	 * menu_autopilot_force_pointer(). No second query is introduced.
	 *
	 * It DOMINATES pointer_target: the seam answers "unavailable" first, so a
	 * target asserted by mistake can never re-enable the pointer inside the
	 * leg. The script itself never emits both (pinned by
	 * test_menu_autopilot_script's dominance case), so the dominance is a
	 * BACKSTOP, not an arbitration the leg relies on.
	 *
	 * Inert everywhere else: false on every step outside the no-pointer leg,
	 * false on an idle/finished output, and never consulted at all until
	 * menu_autopilot_arm() has run (menu_autopilot_force_pointer returns
	 * false while unarmed, so the real pointer passes straight through). */
	bool pointer_suppressed;
	/* Container/sign legs (2026-09-11): re-aim the held look (camera degrees,
	 * menu_dwell_demo.h) and/or request ONE empty-hand right-click pulse.
	 * Both are one-tick outputs, applied by menu_autopilot.c through
	 * autopilot_aim_set_deg / autopilot_request_place. */
	bool aim_set;
	int aim_yaw_deg;
	int aim_pitch_deg;
	bool place_pulse;
};

struct menu_ap_state {
	int step;			 /* current script step (opaque ordinal; see .c) */
	float timer;		 /* reused as dwell-seconds or wait-seconds per step; reset on every transition */
	int repeat_done;	 /* presses completed so far within a multi-press step */
	bool repeat_phase;	 /* 2-phase press/idle latch: false=ready to press, true=press just issued */
	bool finished;		 /* true once the script has completed OR abandoned after a hard timeout */
	/* Container-leg latches (bonus evidence, never required for PASS): did
	 * the walker ever observe each container screen. The Sort-dialog latch went
	 * with Sort itself (parked by the maintainer 2026-10-25). */
	bool furnace_reached, chest_reached, large_chest_reached, dispenser_reached;
	bool pack_legs;		 /* opt-in (debug.menus_dwell_pack_legs): each apply stalls the main thread 3-5+ s on the card */
	int pack_applies;	 /* texture-pack Apply clicks issued (default, earth-and-ash, default) */
	bool quit_reached;	 /* Save and quit brought the title back (unload settled) */
	bool quit_failed;	 /* Save and quit never left the pause within WAIT_QUIT (save refused) */
	bool sign_reached;	 /* bonus evidence: did the script ever observe MENU_AP_SCREEN_SIGN */
	/* ui-build-1026 T15: the Tutorial world title row was held under the
	 * ring for a full dwell (never activated). Required by the route: a run
	 * cannot get past STEP_TITLE_TUTORIAL_DWELL without it. */
	bool title_tutorial_seen;
	/* ui-build-1026 T15, Texture packs one-click: the row the pick aimed at
	 * (-1 before the pick), and the latch that the pick selected it with ONE
	 * pointer click, left Apply unpressed and held through a full dwell. */
	int texpacks_pick_target;
	bool texpacks_pick_seen;
	/* ui-build-1026 T15, Create World sub-legs (enum menu_ap_cworld_sub):
	 * which one the shared steps are running, the text signature latched
	 * before the open (Cancel must restore it), and the per-sub-leg
	 * completion bits (1 << sub). cworld_osk_skipped: the OSK sub-legs were
	 * NOT walked because this platform's keyboard owns the editor
	 * (text_input_owns_close; never on the console). */
	int cworld_sub;
	unsigned cworld_sig_before;
	unsigned cworld_subs_done;
	bool cworld_osk_skipped;
	/* ui-build-1026 T15c, Inventory pages: the Crafting tab seen on arrival
	 * (-1 before), and the latch that Inventory -> Crafting (tap), one tab,
	 * Crafting -> Inventory (tap) all held under observation. */
	int inv_tab_before;
	bool inv_pages_seen;
	int max_step;		 /* monotonic high-water mark of steps reached; the PASS-gate evidence */
	/* One-shot latch (2026-09-02): set exactly once, the tick STEP_WAIT_SIGN's
	 * soft wait times out (never reset except by menu_ap_script_reset). Lets
	 * the impure caller (menu_autopilot.c/main.c) log/telemeter WHICH run this
	 * was on a real hardware trace, instead of the timeout being silently
	 * indistinguishable from any other soft-skip. */
	bool sign_wait_timed_out;
	/* Set once by menu_ap_script_reset() and never changed thereafter: which
	 * Game Mode STEP_CWORLD_MODE_CONFIRM should verify landed before Create.
	 * false (the default passed by every caller except the --cw-cworld-mode
	 * creative knob) = Survival, unchanged pre-knob behavior. */
	bool target_creative;
	/* ---- the NO-POINTER leg (push-0921 A7 part 1) ---------------------- */
	/* Platform FACT supplied by the impure caller, not a platform test done
	 * here (this module stays free of any #ifdef): true where
	 * input_text_active() CAN be true, i.e. where a real keyboard can own an
	 * OSK-backed screen and the Close role is gated off
	 * (screen_rename_world.c:229, screen_multiplayer_setup.c:1855/:1983/:2122,
	 * screen_create_world.c:426). PLATFORM_PC = true, PLATFORM_WII = false
	 * (input.c:934 returns a hard false there). Part 1's leg enters no
	 * OSK-backed screen, so it only LATCHES the marker below; part 2's
	 * sub-editor steps are the consumers. */
	bool text_input_owns_close;
	/* The marker: this run could not walk the OSK-backed screens because the
	 * platform's keyboard owns their Close. Latched once when the leg starts
	 * with text_input_owns_close set, so a PC run says so in its own trace
	 * instead of the skipped coverage being invisible. */
	bool np_osk_legs_skipped;
	/* FAIL BY NAME. The no-pointer leg's bounded waits all abandon into the
	 * single STEP_NP_FAILED ordinal (deliberately numbered BELOW the pass
	 * ordinal, so a failed leg can never satisfy the max_step gate); this
	 * records WHICH of them ran out, since one shared terminal step cannot.
	 * 0 = no leg step has failed. */
	int np_failed_step;
	/* Which Create World gate refused (STEP_CWORLD_NAV_CREATE or
	 * STEP_CWORLD_SUBMIT) when the run ended at STEP_CWORLD_CREATE_FAILED;
	 * 0 otherwise. */
	int cworld_failed_step;
	/* Which OBSERVED focus walk (title -> Texture packs / Play, pause ->
	 * Options / Save and quit) refused when the run ended at STEP_WALK_FAILED:
	 * the ordinal of the walk or of its activate gate. 0 otherwise. */
	int walk_failed_step;
	/* EVERY abandonment, named (hw1026c follow-up): 1 + the step ordinal at
	 * which the run gave up -- np_failed_step / walk_failed_step /
	 * cworld_failed_step when one of those named it, else the step that was
	 * running (a bounded wait's timeout, or the stall guard). 0 while the
	 * run is live and after a completed one. Carried on every heartbeat as
	 * map_fail. */
	int map_fail;
	/* The stall guard fired: the step at map_fail - 1 neither advanced nor
	 * abandoned for STALL_LIMIT seconds. */
	bool stalled;
	float stall_timer; /* seconds with no progress: no step change, press, or observed change */
	unsigned stall_sig; /* last tick's observation signature (menu_ap_script.c) */
	/* The leg completed: reached the title again after Select World, Texture
	 * Packs and Multiplayer each round-tripped by Close, with the pointer
	 * suppressed throughout. */
	bool np_leg_done;
	/* ---- part 2: the observation-driven sub-legs ----------------------- */
	/* Which sub-leg (enum menu_ap_sub) the shared walk is running. The walk
	 * steps are one set of ordinals re-entered per sub-leg, so this is what
	 * names the position -- and what np_failed_sub reports on a failure. */
	int np_sub;
	/* The sub-leg whose walk ran out or whose gate never opened, valid iff
	 * np_failed_step != 0. Together they read "sub-leg X died at step Y". */
	int np_failed_sub;
	/* NAMED SKIPS -- a bitmask of (1 << enum menu_ap_sub). A skipped sub-leg
	 * is NOT a failure: it is a sub-leg whose target could not legitimately
	 * be reached (an empty list disables Rename/Delete, no saved server
	 * disables Edit) or must not be entered on this platform (the OSK-backed
	 * screens where input_text_active() owns the Close). Every one of them is
	 * reported rather than being invisible, which is the whole point. */
	unsigned np_skipped;
	/* The sub-legs whose target the walk actually OBSERVED under the ring,
	 * same bit layout. A skipped OSK sub-leg still sets this: "the focus
	 * reached it, the enter/leave is what was skipped" is a materially
	 * different claim from "never got near it", and the two must not blur. */
	unsigned np_focus_reached;
	/* The sub-legs that were fully entered and left, same bit layout. */
	unsigned np_entered;
	/* The Multiplayer sub-legs were skipped because NOTHING ANSWERED the
	 * screen's focus/view observers in this build (see
	 * menu_ap_observation.mps_observed) -- a wiring gap, not a screen
	 * behaving correctly, and therefore a materially different skip from the
	 * other two. A run with this set has NOT proven the Multiplayer half. */
	bool np_mps_unobserved;
	/* THE FLAGS the heartbeat carries (MENU_AP_NP_FLAG_*). Set once, never
	 * cleared except by reset. On a console run the collector requires this
	 * to be ZERO: every bit is either a wiring gap or a proof that did not
	 * hold, and a PASS on top of any of them would certify nothing. */
	unsigned np_flags;
	/* screen_sworld_row_count() latched when the leg's Select World dwell
	 * COMPLETES (not on arrival: the row list resolves a couple of entries
	 * per frame, so a count taken the instant the screen appears can be short
	 * and would make its own later comparison fail). -1 = never latched. */
	int np_rows;
	/* A failure that must not abandon the leg WHERE it was found, because
	 * leaving a modal open is worse than the failure: the delete confirm's
	 * safe-answer check sets this, the sub-leg still backs out with Close,
	 * and the abandon happens once the screen is clean again. 0 = none. */
	int np_pending_fail_step;
	/* True after the current sub-leg opened its approved Options modal. The
	 * same observed-input walker then seeks the modal action; no screen state
	 * is set or inferred from a press. */
	unsigned char np_parent_stage; /* 0 main, 1 modal opening/open, 2 final action */
};

/* THE BIT TABLE, one bit per sub-leg, shared by np_entered / np_skipped /
 * np_focus_reached and mirrored in tools/wii_telemetry_server.py. The bit IS
 * (1 << enum menu_ap_sub), so the enum below is the single definition and the
 * collector's table is a copy that its own test pins. */
#define MENU_AP_NP_BIT(sub) (1u << (unsigned)(sub))

/* np_flags. Every one of these means "this run did not prove what a clean run
 * proves", which is why the console verdict requires all of them clear.
 *
 * ONE TABLE, THREE CONSUMERS (push-0921 A23, review A20-7). The enum, the
 * union MENU_AP_NP_FLAG_ALL and everything downstream are GENERATED from the
 * X-macro below -- the `window registry` precedent in this repo -- because
 * every hand-written second copy of this list has gone stale on schedule:
 * A16-8 replaced one hand-written OR in the test with another in this header,
 * and a flag added to the enum and to nothing else still left
 * NP_FLAG_NAMES == MENU_AP_NP_FLAG_ALL (both stale) and
 * _Static_assert(MENU_AP_NP_FLAG_ALL <= 0xFFu) true -- i.e. the exact 9th-flag
 * truncation the assert exists to prevent stayed reachable through ONE
 * omission. Add a row here and the union grows by itself: the assert in
 * menu_autopilot.c sees the new width, and tests/test_menus_dwell_contract.c
 * goes RED until tools/wii_telemetry_server.py's NP_FLAG_NAMES names it too.
 *
 * What each bit means (the prose the enum used to carry, unchanged):
 *
 *   bit 0 MPS_UNOBSERVED -- nothing registered the Multiplayer observers in
 *     this build, so the four Multiplayer sub-legs were skipped unwalked.
 *   bit 1 OSK_SKIP -- at least one OSK-backed sub-leg was reached but not
 *     entered, because this platform's keyboard owns the Close role. Expected
 *     on PC, and a defect on the console, where input_text_active() is a hard
 *     false.
 *   bit 2 ROW_COUNT_MISMATCH -- the Select World row count changed across a
 *     sub-leg that must not change it (the delete confirm, or the rename
 *     editor).
 *   bit 3 CONFIRM_NOT_SAFE -- the delete confirmation did NOT open on its
 *     non-destructive answer.
 *   bit 4 LEG_FAILED -- the leg abandoned; np_failed_step / np_failed_sub name
 *     where.
 *   bit 5 ENTRY_LEVER -- THE PC ENTRY LEVER (push-0921 A14) was used: this run
 *     did NOT walk the earlier legs -- it was started directly at the
 *     no-pointer leg from the title by menu_ap_script_start_at_np_leg(), so
 *     its max_step is a high-water mark of the leg ALONE and its earlier
 *     coverage (texture packs, create-world's form ring, the pause/options round
 *     trip, the sign and container legs, Save and quit) never happened. Set
 *     from the lever's very first tick and never cleared, so a run that used
 *     it can never satisfy a console verdict: the collector refuses any
 *     non-zero np_flags by name.
 *   bit 6 STRIP_FALLBACK -- THE RIGHT STRIP ENTRY DID NOT TAKE on at least one
 *     sub-leg: the walk reached the action strip through
 *     STEP_NP_SUB_TO_STRIP_DOWN -- A14's DOWN converge -- instead (review
 *     A16-3). The fallback keeps the leg alive on a card whose screens predate
 *     A15, which is why it exists; what it must not do is let such a card
 *     CERTIFY A15's claim, "GUI RIGHT from any list row enters the action
 *     strip". Nothing else in the tape can tell the two apart: the fallback
 *     step is numbered BELOW the clamp, so max_step is identical, and the only
 *     other difference is a press count no heartbeat carries. Any non-zero
 *     np_flags is refused by the collector, which is the right severity -- the
 *     screens on that card do not implement the thing under test.
 *
 * The bit NUMBER is the second column, written out rather than implied by row
 * order, because it is wire state: tools/wii_telemetry_server.py's
 * NP_FLAG_NAMES keys on it and a real tape's np_flags=0x40 has to keep meaning
 * the same thing next year. */
#define MENU_AP_NP_FLAG_TABLE(X)                                               \
	X(MENU_AP_NP_FLAG_MPS_UNOBSERVED, 0)                                       \
	X(MENU_AP_NP_FLAG_OSK_SKIP, 1)                                             \
	X(MENU_AP_NP_FLAG_ROW_COUNT_MISMATCH, 2)                                   \
	X(MENU_AP_NP_FLAG_CONFIRM_NOT_SAFE, 3)                                     \
	X(MENU_AP_NP_FLAG_LEG_FAILED, 4)                                           \
	X(MENU_AP_NP_FLAG_ENTRY_LEVER, 5)                                          \
	X(MENU_AP_NP_FLAG_STRIP_FALLBACK, 6)

#define MENU_AP_NP_FLAG_ENUMERATOR(name, bit) name = 1u << (bit),
enum { MENU_AP_NP_FLAG_TABLE(MENU_AP_NP_FLAG_ENUMERATOR) };
#undef MENU_AP_NP_FLAG_ENUMERATOR

/* EVERY flag the walker can raise, as one mask -- DERIVED from the table, so
 * it cannot disagree with it. Two consumers: menu_autopilot.c asserts this
 * fits the heartbeat's uint8_t np_flags, and tests/test_menus_dwell_contract.c
 * compares the collector's NP_FLAG_NAMES against it. */
#define MENU_AP_NP_FLAG_UNION(name, bit) | name
#define MENU_AP_NP_FLAG_ALL (0u MENU_AP_NP_FLAG_TABLE(MENU_AP_NP_FLAG_UNION))

/* The part-2 sub-legs, in the order the leg walks them. The ordinal IS the
 * name on a heartbeat (np_sub / np_failed_sub) and the bit position in
 * np_skipped / np_focus_reached / np_entered. */
enum menu_ap_sub {
	MENU_AP_SUB_SWORLD_CREATE = 0, /* Create New World -> the form -> Close */
	MENU_AP_SUB_SWORLD_RENAME,	   /* Rename (needs a row; OSK-backed) */
	MENU_AP_SUB_SWORLD_DELETE,	   /* Delete (needs a row) -> the confirm -> back out */
	MENU_AP_SUB_MPS_DIRECT,		   /* Direct Connect (OSK-backed) */
	MENU_AP_SUB_MPS_ADD,		   /* Add Server (OSK-backed) */
	MENU_AP_SUB_MPS_EDIT,		   /* Edit (needs a server row; OSK-backed) */
	MENU_AP_SUB_MPS_NAME,		   /* the player-name editor (OSK-backed) */
	MENU_AP_SUB_COUNT,
};

/* ui-build-1026 T15: the Create World sub-legs, in walk order. */
enum menu_ap_cworld_sub {
	MENU_AP_CWORLD_SUB_DETAILS = 0, /* Details card: open, dwell, Close */
	MENU_AP_CWORLD_SUB_NAME_OSK,	/* name OSK: open, one key, Cancel keeps the old name */
	MENU_AP_CWORLD_SUB_SEED_OSK,	/* seed keypad: open, one key, Cancel keeps the old seed */
	MENU_AP_CWORLD_SUB_COUNT,
};

/* `target_creative` selects which Game Mode STEP_CWORLD_MODE_CONFIRM
 * confirms before Create -- see the header's design note. Pass false for
 * the default (Survival, byte-identical to before this parameter existed). */
void menu_ap_script_reset(struct menu_ap_state* s, bool target_creative);
/* After reset: run the three texture-pack applies (see pack_legs). Off by
 * default so a plain menus_dwell certification never stalls its heartbeats. */
void menu_ap_script_enable_pack_legs(struct menu_ap_state* s, bool on);

/* The OBSERVED focus walk's one decision (step_walk), exposed so a test can
 * drive a REAL screen with it: the button that moves `focus` one step toward
 * `target` on a linear, clamped list whose ordinals are the visual order --
 * IB_GUI_DOWN when below it (or NONE = -1), IB_GUI_UP when past it -- or -1
 * once reached. A walk gives up after MENU_AP_FOCUS_WALK_MAX presses. */
#define MENU_AP_FOCUS_WALK_MAX 8
int menu_ap_focus_walk_button(int focus, int target);
/* After reset: tell the script whether this platform's keyboard can own an
 * OSK-backed screen's Close (menu_ap_state.text_input_owns_close). The CALLER
 * decides -- this module carries no platform conditional. */
void menu_ap_script_set_text_input_owns_close(struct menu_ap_state* s, bool on);

/* THE PC ENTRY LEVER (push-0921 A14, `debug.menu_ap_start_np`). After reset:
 * start the script at the NO-POINTER leg's first step instead of the top, so
 * the leg can be exercised against the REAL screens in the PC app.
 *
 * WHY IT EXISTS. The PC app cannot walk the script's earlier create-world leg
 * -- screen_create_world arms the physical keyboard while its OSK modal is up
 * (screen_create_world.c:274) and every OSK-backed screen gates its Close
 * behind !input_text_active(), so a button-only walker parks there. The
 * no-pointer leg therefore reached real hardware having only ever run against
 * this module's mock transition table, and it FAILED on the console in its
 * first sub-leg (push-0921 A13, build 82e7bfe_09.21: a 645-world card, the
 * DOWN walk out of the row list ran out of presses). This lever is how that
 * class of defect is found on PC first.
 *
 * WHAT IT COSTS. The run skips everything before the leg, so its max_step
 * proves only the leg -- which is why it raises MENU_AP_NP_FLAG_ENTRY_LEVER
 * from the first tick and the collector refuses the tape by name. It is a
 * DEVELOPMENT ENTRY POINT, never a way to pass a gate.
 *
 * Inert unless called: a run that never calls it is byte-identical to before
 * this function existed (pinned by test_menu_autopilot_script's lever case,
 * which compares the whole step/press log against a run without it). */
void menu_ap_script_start_at_np_leg(struct menu_ap_state* s);

/* Advance one frame. `screen` is the caller's classification of
 * gstate.players[0].current_screen for THIS frame; `dt` is the frame's delta time in
 * seconds. `obs` is the caller's OBSERVATION of the live screens this frame
 * (see struct menu_ap_observation). obs.cworld_mode_survival is consulted
 * ONLY inside STEP_CWORLD_MODE_CONFIRM; the three focus/view fields ONLY
 * inside the part-2 sub-legs -- any value is harmless everywhere else, so a
 * caller with none of those screens active may pass zeroes. Always writes
 * *out (an all-false/MENU_AP_PT_NONE output means "do nothing to input this
 * frame"). Returns true while the script still has work to do; once it
 * returns false (menu_ap_script_finished(s) is also true from then on), the
 * caller should stop invoking it -- or may keep calling it harmlessly, since
 * a finished script always emits an empty output and never re-arms itself. */
bool menu_ap_script_tick(struct menu_ap_state* s, enum menu_ap_screen screen,
						  struct menu_ap_observation obs, float dt,
						  struct menu_ap_output* out);

bool menu_ap_script_finished(const struct menu_ap_state* s);

/* The total number of steps in the script (exposed so telemetry/tests can
 * report progress as a fraction, and so the PASS gate's required-step
 * threshold has a named upper bound to check itself against). */
int menu_ap_script_step_count(void);

/* The step ordinal at which the script (softly) starts waiting for the sign
 * screen -- i.e. the point by which STEP_INGAME_SETTLE has already dwelled
 * its full settle time with the screen continuously in-game. Callers that
 * simulate/inject the sign screen appearing (the real integration layer
 * doesn't need this; the menu_ap_script_tick test harness does, so it does
 * not race STEP_INGAME_SETTLE's own dwell) must wait for
 * `step >= this value` before doing so. */
int menu_ap_script_step_awaiting_sign(void);

/* The ordinal that waits for the Create World screen (STEP_WAIT_CWORLD) and
 * the one that waits for the world to load after Create (STEP_WAIT_INGAME).
 * For tests that drive the walker against the REAL create-world screen
 * (tests/test_menus_dwell_create_flow.c) by starting it at the first and
 * proving it reaches the second with Create actioned. */
int menu_ap_script_step_cworld_entry(void);
int menu_ap_script_step_wait_ingame(void);
/* ui-build-1026 T15: the Tutorial world row's focus-checked dwell, so a test
 * can name it as a walk_failed_step gate. */
int menu_ap_script_step_title_tutorial_dwell(void);
/* ui-build-1026 T15: the first Texture packs one-click step and the first
 * Create World sub-leg step (STEP_CW_SUB_BEGIN), for tests naming gates. */
int menu_ap_script_step_texpacks_pick(void);
int menu_ap_script_step_cworld_sub_begin(void);
/* ui-build-1026 T15c: the first inventory page step (STEP_INV_SWAP_TO_CRAFT). */
int menu_ap_script_step_inv_pages(void);
/* The named terminal failure of the Create World walk. */
int menu_ap_script_step_cworld_failed(void);
/* The named terminal failure of the title / pause focus walks. */
int menu_ap_script_step_walk_failed(void);
/* STEP_PAUSE_OPEN (HOME from in-game -> pause) and STEP_WAIT_OPTIONS, for
 * tests that drive the walker's pause legs against the REAL pause screen. */
int menu_ap_script_step_pause_entry(void);
int menu_ap_script_step_wait_options(void);

/* The step ordinal that proves the FULL script completed -- since push-0921
 * A7 part 1 that is the NO-POINTER leg's terminal step, so the gate can only
 * be met by a run that finished the leg with the pointer suppressed. This is
 * the required-for-PASS threshold, and tools/wii_telemetry_server.py's
 * PROFILES["menus_dwell"]["min_map_step"] must equal it
 * (tests/test_menus_dwell_contract.c pins both sides). */
int menu_ap_script_step_required_for_pass(void);

/* ======================================================================
 * ui-build-1026/T16a: the world_copy_convert ROUTE.
 *
 * A second, SEPARATE pure machine (its own state, its own step ordinals) so
 * the menus_dwell step contract (MENUS_DWELL_STEP_CONTRACT, pinned by
 * tests/test_menus_dwell_contract.c) does not move by one ordinal. It drives
 *   Title -> Play -> Select World -> the fixture's red "Must be converted"
 *   row -> Play -> "Back up this world?" (Cancel focused) -> Back up &
 *   convert -> copy progress -> convert -> Done -> Back to worlds
 * on ONE disposable fixture world, found by its DIRECTORY name, never by
 * position: if the fixture row is absent the route fails by name and never
 * presses activate on any other row. Every activate is issued only on a
 * frame whose observation says the ring is on the named target.
 *
 * Variants (the device-side fault seams are product hooks, see
 * tmp/jobs/ui-build-1026/T16a-RIG.md):
 *   OK           Back up & convert to Done.
 *   LOW_SPACE    injected free-space value below required_bytes: the error
 *                screen ("The SD card is full"), Cancel, fixture unchanged.
 *   WRITE_ERROR  injected world_copy write fault: cleanup, error, Cancel.
 *   INTERRUPT    a controlled interruption once copying is under way: the
 *                route asks the product hook to stop the copy WITHOUT
 *                cleanup (leaving the journal + temp tree a power cut
 *                leaves) and ends; the rig then power-cycles.
 *   RECOVER      the boot after INTERRUPT: the list must show no partial
 *                world BEFORE anything is pressed, then the OK route runs.
 *
 * Evidence is a fixed set of `{"event":"wcc_*"}` observer lines (one per
 * bit in enum menu_ap_copy_marker), latched by this machine and formatted by
 * menu_ap_copy_format_marker() so the bytes the console sends are the bytes
 * tests/test_world_copy_profile.c checks. The collector
 * (tools/wii_telemetry_server.py, WORLD_COPY_MARKERS) fails closed on any
 * missing one.
 * ====================================================================== */

enum menu_ap_copy_variant {
	MENU_AP_COPY_OK = 0,
	MENU_AP_COPY_LOW_SPACE,
	MENU_AP_COPY_WRITE_ERROR,
	MENU_AP_COPY_INTERRUPT,
	MENU_AP_COPY_RECOVER,
	MENU_AP_COPY_VARIANT_COUNT
};
const char* menu_ap_copy_variant_name(enum menu_ap_copy_variant v);
/* -1 for an unknown name. */
int menu_ap_copy_variant_parse(const char* name);

/* screen_convert_backup.c's enum cb_state, by VALUE (the file pins the
 * ordinals its tests read: "0 ask ... 8 conversion error"). */
enum menu_ap_cb_state {
	MENU_AP_CB_NONE = -1, /* not observable (hook unregistered) */
	MENU_AP_CB_ASK = 0,
	MENU_AP_CB_VOLUME,
	MENU_AP_CB_COPYING,
	MENU_AP_CB_ERROR,
	MENU_AP_CB_CLEANING,
	MENU_AP_CB_CONVERTING,
	MENU_AP_CB_DONE,
	MENU_AP_CB_SAVE_PENDING,
	MENU_AP_CB_CONVERT_ERROR,
};
/* The FOCUSED button's action (screen_convert_backup.c enum cb_action). */
enum menu_ap_cb_action {
	MENU_AP_CB_ACT_NONE = -1,
	MENU_AP_CB_ACT_BACKUP = 0,
	MENU_AP_CB_ACT_NO_BACKUP,
	MENU_AP_CB_ACT_CANCEL,
	MENU_AP_CB_ACT_PLAY,
	MENU_AP_CB_ACT_BACK,
};
/* screen_convert_backup.c enum cb_fail. */
enum menu_ap_cb_fail {
	MENU_AP_CB_FAIL_NONE = 0,
	MENU_AP_CB_FAIL_VOLUME,
	MENU_AP_CB_FAIL_NO_MEMORY,
	MENU_AP_CB_FAIL_COPY,
	MENU_AP_CB_FAIL_CLEANUP,
};
/* world_copy.h enum world_copy_error values the route names. */
#define MENU_AP_WC_OK 0
#define MENU_AP_WC_IO_ERROR 6
#define MENU_AP_WC_LOW_SPACE 10
/* server_interface.h enum svin_convert_outcome values the route names. */
#define MENU_AP_CONVERT_PENDING 0
#define MENU_AP_CONVERT_SUCCEEDED 1

/* One tick's observation. Every field is a PRODUCTION read-only observer
 * (the owed hunks in T16a-RIG.md); `observed` is false until all of them are
 * registered, and the route then fails at its first step by name
 * (failed_step = MENU_AP_COPY_STEP_UNOBSERVED) instead of pressing blind. */
struct menu_ap_copy_obs {
	bool observed;
	int title_focus; /* enum menu_ap_title_focus */
	/* Select World */
	int sworld_focus;		  /* enum menu_ap_sworld_focus */
	bool sworld_settled;	  /* every row resolved (no archive open pending) */
	int sworld_rows;		  /* rows in the list */
	int sworld_selected;	  /* the selected row, -1 none */
	int fixture_row;		  /* row whose DIRECTORY is the fixture, -1 absent */
	bool fixture_needs_convert; /* ... and it carries "Must be converted" */
	int backup_row;			  /* row whose directory is "<fixture> (backup)" */
	bool partial_visible;	  /* any row for a world_copy temp/reservation */
	/* screen_convert_backup */
	int cb_state;  /* enum menu_ap_cb_state */
	int cb_action; /* enum menu_ap_cb_action of the focused button */
	int cb_fail;   /* enum menu_ap_cb_fail */
	int cb_copy_error;
	int cb_percent; /* copy percent, -1 while surveying / not copying */
	uint64_t cb_free_bytes; /* the ONE free-space value the copy used */
	uint32_t cb_cluster_bytes;
	uint64_t cb_required_bytes; /* world_copy_result.required_bytes */
	int cb_convert_outcome;
	bool cb_backup_published;
	bool cb_cleanup_done; /* screen_convert_backup_cleanup_done() */
};

enum menu_ap_copy_step {
	MENU_AP_COPY_STEP_UNOBSERVED = 0, /* terminal failure: hooks missing */
	MENU_AP_COPY_STEP_WAIT_TITLE,
	MENU_AP_COPY_STEP_TITLE_PLAY,
	MENU_AP_COPY_STEP_WAIT_LIST,
	MENU_AP_COPY_STEP_LIST_PRE,
	MENU_AP_COPY_STEP_SEEK_ROW,
	MENU_AP_COPY_STEP_SEEK_PLAY,
	MENU_AP_COPY_STEP_WAIT_ASK,
	MENU_AP_COPY_STEP_SEEK_BACKUP,
	MENU_AP_COPY_STEP_RUN,
	MENU_AP_COPY_STEP_RESULT_DWELL,
	MENU_AP_COPY_STEP_LEAVE,
	MENU_AP_COPY_STEP_WAIT_LIST_POST,
	MENU_AP_COPY_STEP_DONE,	  /* terminal success */
	MENU_AP_COPY_STEP_FAILED, /* terminal failure: failed_step names where */
	/* terminal failure, appended: the worlds root was not the isolated
	 * per-run folder (menu_ap_copy_refuse), so nothing was pressed or listed */
	MENU_AP_COPY_STEP_ISOLATION,
	MENU_AP_COPY_STEP_COUNT
};

/* The evidence lines, in emission order. */
enum menu_ap_copy_marker {
	MENU_AP_WCC_BEGIN = 0, /* variant + fixture name */
	MENU_AP_WCC_LIST_PRE,  /* the list before any press */
	MENU_AP_WCC_ASK,	   /* the ask opened with Cancel focused */
	MENU_AP_WCC_VOLUME,	   /* free/cluster/required bytes the copy used */
	MENU_AP_WCC_COPY,	   /* progress seen, monotonic, cleanup seen */
	MENU_AP_WCC_RESULT,	   /* terminal screen state + error + outcome */
	MENU_AP_WCC_LIST_POST, /* the list after Back/Cancel */
	MENU_AP_WCC_CARD,	   /* the worlds root on the card, dot names too */
	MENU_AP_WCC_END,	   /* route verdict; ALWAYS sent, even on failure */
	MENU_AP_WCC_COUNT
};

#define MENU_AP_COPY_FIXTURE_MAX 64
/* The wcc_card listing: at most this many names, each at most NAME_MAX
 * bytes; anything beyond sets `truncated` (which the collector FAILS). The
 * whole line stays under the observer packet's 2303-byte limit. */
#define MENU_AP_COPY_CARD_NAMES 16
#define MENU_AP_COPY_CARD_NAME_MAX 96
#define MENU_AP_COPY_LINE_MAX 2048

struct menu_ap_copy_state {
	enum menu_ap_copy_variant variant;
	char fixture[MENU_AP_COPY_FIXTURE_MAX];
	int step;
	int failed_step; /* valid at STEP_FAILED / STEP_UNOBSERVED */
	float timer;
	float settle;	  /* seconds the list/result has held, this step */
	int presses;	  /* presses spent in the current step */
	bool press_phase; /* press / release latch: one edge per two ticks */
	bool finished;
	bool ok;
	bool interrupt_requested; /* INTERRUPT: the hook was asked, once */
	/* latched evidence */
	int pre_rows, pre_fixture_row, pre_backup_row;
	bool pre_needs_convert, pre_partial;
	bool ask_safe_default;
	uint64_t free_bytes, required_bytes;
	uint32_t cluster_bytes;
	bool volume_latched;
	bool saw_copying, saw_cleaning, monotonic;
	bool cleanup_done; /* latched with the result: cleanup provably ran */
	int pct_max, pct_last;
	int result_state, result_fail, result_copy_error, result_outcome;
	bool result_published;
	int post_rows, post_fixture_row, post_backup_row;
	bool post_needs_convert, post_partial;
	/* markers: latched (ready to send) / acknowledged (admitted) */
	unsigned markers_ready, markers_sent;
	/* wcc_card: taken by the impure driver once the route has ended (or at
	 * the interrupt), via menu_ap_copy_set_card. END waits for it. */
	bool card_taken, card_root_ok, card_truncated;
	int card_count;
	char card_names[MENU_AP_COPY_CARD_NAMES][MENU_AP_COPY_CARD_NAME_MAX];
};

struct menu_ap_copy_output {
	bool press;
	enum input_button button;
	/* INTERRUPT only: call the product's interruption hook THIS tick. */
	bool interrupt;
};

void menu_ap_copy_reset(struct menu_ap_copy_state* s,
						enum menu_ap_copy_variant variant, const char* fixture);
/* Advance one frame. Returns true while the route still owns input. */
bool menu_ap_copy_tick(struct menu_ap_copy_state* s, enum menu_ap_screen screen,
					   struct menu_ap_copy_obs obs, float dt,
					   struct menu_ap_copy_output* out);
/* The lowest latched-but-unsent marker, formatted as one JSON line; -1 when
 * none is pending. menu_ap_copy_marker_sent() acknowledges it. */
int menu_ap_copy_next_marker(const struct menu_ap_copy_state* s, char* buf,
							 size_t cap);
void menu_ap_copy_marker_sent(struct menu_ap_copy_state* s, int marker);
/* Format one marker regardless of the pending set (tests). Returns the
 * snprintf length, or -1 when it does not fit. */
int menu_ap_copy_format_marker(const struct menu_ap_copy_state* s, int marker,
							   char* buf, size_t cap);
/* Every marker the variant must send for a PASS (bit per marker). */
unsigned menu_ap_copy_required_markers(enum menu_ap_copy_variant v);
/* Review round 1 (Sol B1): the route may run only when the configured
 * worlds root is EXACTLY the expected per-run folder ".../wcc_runs/<id>"
 * (<id> = [A-Za-z0-9_]{1,40}). Anything else -- a failed setter leaving the
 * player's "saves", NULL, empty, a different id -- is false. */
bool menu_ap_copy_root_isolated(const char* configured, const char* expected);
/* A refused route: finished at MENU_AP_COPY_STEP_ISOLATION with no tick ever
 * run, card taken as root_ok=false with no names (the non-isolated root is
 * never listed); BEGIN, CARD and END are latched. */
void menu_ap_copy_refuse(struct menu_ap_copy_state* s,
						 enum menu_ap_copy_variant variant, const char* fixture);
/* Is the route waiting for its card listing (finished, not yet taken)? */
bool menu_ap_copy_card_wanted(const struct menu_ap_copy_state* s);
/* Record the worlds-root listing (every entry but "." and "..", dot names
 * included, in readdir order) and latch wcc_card. root_ok=false when the
 * directory could not be opened. Names longer than CARD_NAME_MAX-1 or beyond
 * CARD_NAMES set `truncated`. */
void menu_ap_copy_set_card(struct menu_ap_copy_state* s, bool root_ok,
						   const char* const* names, int count);

/* Route budgets (seconds). */
#define MENU_AP_COPY_WAIT_S 30.0F	   /* each screen/observer wait */
#define MENU_AP_COPY_RUN_S 600.0F	   /* copy + convert of the fixture */
#define MENU_AP_COPY_DWELL_S 2.0F	   /* result + list dwells */
#define MENU_AP_COPY_PRESS_MAX 64	   /* presses in one seek step */

#endif
