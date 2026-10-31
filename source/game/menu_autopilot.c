/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2. See menu_autopilot.h.
*/

#include "menu_autopilot.h"
#include "save_reload_script.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_state.h"
#include "gui/screen.h"
#include "gui/widget_osk.h"
#include "gui/recipe_browser.h"
#include "menu_autopilot_script.h"
#include "autopilot.h"
#include "../platform/gfx.h"
#include "../platform/hw_telemetry.h"

/* THE MIRROR CHECK (A7 part 2). menu_autopilot_script.h restates screen.h's
 * three focus/view enums by VALUE so the pure script keeps no screen.h
 * dependency; this is the one TU that sees both, so it is where the two are
 * pinned together. If either enum is reordered or extended in the middle, the
 * BUILD breaks here -- instead of the walker silently seeking the wrong
 * control and the activate guard silently approving it. */
_Static_assert((int)MENU_AP_SWORLD_LIST == (int)SCREEN_SWORLD_FOCUS_LIST
				   && (int)MENU_AP_SWORLD_SELECT == (int)SCREEN_SWORLD_FOCUS_SELECT
				   && (int)MENU_AP_SWORLD_RENAME == (int)SCREEN_SWORLD_FOCUS_RENAME
				   && (int)MENU_AP_SWORLD_DELETE == (int)SCREEN_SWORLD_FOCUS_DELETE
				   && (int)MENU_AP_SWORLD_CREATE == (int)SCREEN_SWORLD_FOCUS_CREATE
				   && (int)MENU_AP_SWORLD_CANCEL == (int)SCREEN_SWORLD_FOCUS_CANCEL
				   && (int)MENU_AP_SWORLD_TAB_LOAD == (int)SCREEN_SWORLD_FOCUS_TAB_LOAD
				   && (int)MENU_AP_SWORLD_TAB_CREATE == (int)SCREEN_SWORLD_FOCUS_TAB_CREATE
				   && (int)MENU_AP_SWORLD_TAB_JOIN == (int)SCREEN_SWORLD_FOCUS_TAB_JOIN
				   && (int)MENU_AP_SWORLD_PREV == (int)SCREEN_SWORLD_FOCUS_PREV
				   && (int)MENU_AP_SWORLD_NEXT == (int)SCREEN_SWORLD_FOCUS_NEXT
				   && (int)MENU_AP_SWORLD_PLAY == (int)SCREEN_SWORLD_FOCUS_PLAY
				   && (int)MENU_AP_SWORLD_RETRY == (int)SCREEN_SWORLD_FOCUS_RETRY
				   && (int)MENU_AP_SWORLD_OPTIONS == (int)SCREEN_SWORLD_FOCUS_OPTIONS
				   && (int)MENU_AP_SWORLD_MODAL_RENAME == (int)SCREEN_SWORLD_FOCUS_MODAL_RENAME
				   && (int)MENU_AP_SWORLD_MODAL_DELETE == (int)SCREEN_SWORLD_FOCUS_MODAL_DELETE
				   && (int)MENU_AP_SWORLD_MODAL_BACK == (int)SCREEN_SWORLD_FOCUS_MODAL_BACK,
			   "menu_ap_sworld_focus drifted from screen_sworld_focus");
_Static_assert((int)MENU_AP_PAUSE_NONE == (int)SCREEN_PAUSE_FOCUS_NONE
				   && (int)MENU_AP_PAUSE_BACK == (int)SCREEN_PAUSE_FOCUS_BACK
				   && (int)MENU_AP_PAUSE_ACHIEVEMENTS == (int)SCREEN_PAUSE_FOCUS_ACHIEVEMENTS
				   && (int)MENU_AP_PAUSE_OPTIONS == (int)SCREEN_PAUSE_FOCUS_OPTIONS
				   && (int)MENU_AP_PAUSE_TEXPACKS == (int)SCREEN_PAUSE_FOCUS_TEXPACKS
				   && (int)MENU_AP_PAUSE_EXIT == (int)SCREEN_PAUSE_FOCUS_EXIT,
			   "menu_ap_pause_focus drifted from screen_pause_focus");
_Static_assert((int)MENU_AP_TITLE_NONE == (int)SCREEN_TITLE_FOCUS_NONE
				   && (int)MENU_AP_TITLE_PLAY == (int)SCREEN_TITLE_FOCUS_PLAY
				   && (int)MENU_AP_TITLE_TUTORIAL
					   == (int)SCREEN_TITLE_FOCUS_TUTORIAL
				   && (int)MENU_AP_TITLE_PACKS == (int)SCREEN_TITLE_FOCUS_PACKS
				   && (int)MENU_AP_TITLE_OPTIONS == (int)SCREEN_TITLE_FOCUS_OPTIONS
				   && (int)MENU_AP_TITLE_QUIT == (int)SCREEN_TITLE_FOCUS_QUIT,
			   "menu_ap_title_focus drifted from screen_title_focus");
/* ui-build-1026 T15: the Create World focus and the OSK values the sub-legs
 * compare against. */
_Static_assert((int)MENU_AP_CWORLD_NAME == (int)SCREEN_CWORLD_FOCUS_NAME
				   && (int)MENU_AP_CWORLD_SEED == (int)SCREEN_CWORLD_FOCUS_SEED
				   && (int)MENU_AP_CWORLD_NEW_SEED == (int)SCREEN_CWORLD_FOCUS_NEW_SEED
				   && (int)MENU_AP_CWORLD_MODE_SURVIVAL
					   == (int)SCREEN_CWORLD_FOCUS_MODE_SURVIVAL
				   && (int)MENU_AP_CWORLD_MODE_CREATIVE
					   == (int)SCREEN_CWORLD_FOCUS_MODE_CREATIVE
				   && (int)MENU_AP_CWORLD_CREATE == (int)SCREEN_CWORLD_FOCUS_CREATE
				   && (int)MENU_AP_CWORLD_DETAILS == (int)SCREEN_CWORLD_FOCUS_DETAILS
				   && (int)MENU_AP_CWORLD_CANCEL == (int)SCREEN_CWORLD_FOCUS_CANCEL
				   && (int)MENU_AP_CWORLD_DETAILS_BACK
					   == (int)SCREEN_CWORLD_FOCUS_DETAILS_BACK,
			   "menu_ap_cworld_focus drifted from screen_cworld_focus");
_Static_assert(MENU_AP_OSK_PROFILE_CREATE_NAME == (int)OSK_FRONT_PROFILE_CREATE_NAME
				   && MENU_AP_OSK_PROFILE_CREATE_SEED
					   == (int)OSK_FRONT_PROFILE_CREATE_SEED
				   && MENU_AP_OSK_CHAR_FIRST == (int)OSK_FRONT_CHAR_FIRST
				   && MENU_AP_OSK_CHAR_FIRST + MENU_AP_OSK_CHAR_CELLS
					   == (int)OSK_FRONT_CASE,
			   "menu_ap OSK mirrors drifted from widget_osk.h");
_Static_assert(MENU_AP_CRAFT_TABS == RECIPE_CATALOG_CATEGORIES,
			   "menu_ap craft tab count drifted from the recipe catalog");
_Static_assert((int)MENU_AP_MPS_LIST == (int)SCREEN_MPS_FOCUS_LIST
				   && (int)MENU_AP_MPS_NAME == (int)SCREEN_MPS_FOCUS_NAME
				   && (int)MENU_AP_MPS_QUICK == (int)SCREEN_MPS_FOCUS_QUICK
				   && (int)MENU_AP_MPS_JOIN == (int)SCREEN_MPS_FOCUS_JOIN
				   && (int)MENU_AP_MPS_DIRECT == (int)SCREEN_MPS_FOCUS_DIRECT
				   && (int)MENU_AP_MPS_ADD == (int)SCREEN_MPS_FOCUS_ADD
				   && (int)MENU_AP_MPS_EDIT == (int)SCREEN_MPS_FOCUS_EDIT
				   && (int)MENU_AP_MPS_DELETE == (int)SCREEN_MPS_FOCUS_DELETE
				   && (int)MENU_AP_MPS_REFRESH == (int)SCREEN_MPS_FOCUS_REFRESH
				   && (int)MENU_AP_MPS_BACK == (int)SCREEN_MPS_FOCUS_BACK
				   && (int)MENU_AP_MPS_OPTIONS == (int)SCREEN_MPS_FOCUS_OPTIONS
				   && (int)MENU_AP_MPS_TAB_LOAD == (int)SCREEN_MPS_FOCUS_TAB_LOAD
				   && (int)MENU_AP_MPS_TAB_CREATE == (int)SCREEN_MPS_FOCUS_TAB_CREATE
				   && (int)MENU_AP_MPS_TAB_JOIN == (int)SCREEN_MPS_FOCUS_TAB_JOIN
				   && (int)MENU_AP_MPS_PREV == (int)SCREEN_MPS_FOCUS_PREV
				   && (int)MENU_AP_MPS_NEXT == (int)SCREEN_MPS_FOCUS_NEXT,
			   "menu_ap_mps_focus drifted from screen_mps_focus");
_Static_assert((int)MENU_AP_MPS_VIEW_MAIN == (int)SCREEN_MPS_VIEW_MAIN
				   && (int)MENU_AP_MPS_VIEW_NAME == (int)SCREEN_MPS_VIEW_NAME
				   && (int)MENU_AP_MPS_VIEW_ADD == (int)SCREEN_MPS_VIEW_ADD
				   && (int)MENU_AP_MPS_VIEW_EDIT == (int)SCREEN_MPS_VIEW_EDIT
				   && (int)MENU_AP_MPS_VIEW_DIRECT == (int)SCREEN_MPS_VIEW_DIRECT,
			   "menu_ap_mps_view drifted from screen_mps_view");

/* THE MULTIPLAYER OBSERVER SEAM (A7 part 2).
 *
 * screen_sworld_focus_target() is called directly above because
 * screen_select_world.c is in the ENGINE CORE pool, the same one this file is
 * compiled into (CMakeLists CAVEX_CLIENT_SOURCES -> CAVEX_ENGINE_CORE_SOURCES
 * -> cavex_test_core). screen_multiplayer_setup.c is NOT: it lives in
 * CAVEX_MP_SOURCES with the whole network stack, which the ~200 pooled tests
 * deliberately do not link. Calling screen_mps_focus_target() /
 * screen_mps_view() from here therefore breaks the link of every one of those
 * tests, which is exactly what it did the first time it was tried.
 *
 * So they arrive through a hook instead, the same shape screen.c already uses
 * for `screen_multiplayer_setup` itself (a core-side pointer, NULL until a
 * build with the MP screens fills it in). Unregistered is not guessed at: the
 * observation says mps_observed=false and the walker SKIPS its four
 * Multiplayer sub-legs by name rather than pressing blind.
 *
 * WIRING OWED (one line, in a file this lane does not own): call
 *   menu_autopilot_set_mps_observers(screen_mps_focus_target, screen_mps_view,
 *                                     screen_mps_server_count);
 * beside the existing menu_autopilot_arm() in source/main.c. Until that
 * lands, menu_autopilot_np_mps_unobserved() is true on every armed run and
 * the Multiplayer half of the leg is UNPROVEN. */
/* The three walker masks ride the heartbeat as uint8_t
 * (source/platform/hw_telemetry.h's np_entered / np_skipped / np_focus): an
 * eighth sub-leg still fits, a ninth would truncate SILENTLY into a PASS that
 * looks like a clean run. Asserted here rather than in main.c because this is
 * the file that owns the masks; the number 8 is the heartbeat field's width,
 * named in that header. */
_Static_assert(MENU_AP_SUB_COUNT <= 8,
			   "walker sub-legs outgrew the heartbeat's 8-bit np_* masks");
/* The SAME trap, for the fourth mask, which had no guard at all until review
 * A16-8 (np_flags is uint8_t in the same header and was already six bits deep;
 * a ninth flag would have truncated to 0 in main.c's (uint8_t) cast and the
 * collector would have graded a flagged run as clean). MENU_AP_NP_FLAG_ALL is
 * the union the enum's own file keeps, so adding a flag without adding it
 * there is what this catches next. */
_Static_assert(MENU_AP_NP_FLAG_ALL <= 0xFFu,
			   "walker np_flags outgrew the heartbeat's 8-bit np_flags field");

static int (*s_mps_focus)(void);
static int (*s_mps_view)(void);
static int (*s_mps_rows)(void);

void menu_autopilot_set_mps_observers(int (*focus)(void), int (*view)(void),
									  int (*rows)(void)) {
	s_mps_focus = focus;
	s_mps_view = view;
	s_mps_rows = rows;
}

/* BOOT-CREDITS-1028 T5: the boot credit cards own the front end (main.c
 * sets this every frame from boot_gate_blocks_title()). */
static bool s_front_end_blocked;
void menu_autopilot_set_front_end_blocked(bool blocked) {
	s_front_end_blocked = blocked;
}

static struct menu_ap_state s_state;
/* menu_ap_report_fail: the abandonment line was admitted / logged. */
static bool s_fail_reported, s_fail_logged;
static bool s_armed;
static bool s_finished; /* mirrors s_state.finished as of the last frame_tick() */
static struct menu_ap_output s_last_output;
/* Resolved once per frame alongside s_last_output, since the pure script
 * only knows the ABSTRACT target (screen.h/gfx.h are off limits to it). */
static float s_pointer_x, s_pointer_y;
/* The resolved target had a real position this frame (a texture-pack card
 * scrolled out of the viewport has none: the pointer then reads unavailable
 * and the pick fails by name instead of clicking elsewhere). */
static bool s_pointer_valid;

static enum input_category s_device_family = INPUT_CAT_NUNCHUK;

enum input_category menu_autopilot_device_parse(const char* value) {
	if(value && (!strcmp(value, "gamecube") || !strcmp(value, "gc")))
		return INPUT_CAT_GAMECUBE;
	if(value && !strcmp(value, "classic"))
		return INPUT_CAT_CLASSIC_CONTROLLER;
	return INPUT_CAT_NUNCHUK; /* the floor controller, as before */
}

enum input_category menu_autopilot_device_family(void) {
	return s_device_family;
}

void menu_autopilot_arm(bool target_creative) {
	menu_ap_script_reset(&s_state, target_creative);
	s_device_family = menu_autopilot_device_parse(config_read_string(
		&gstate.config_user, "debug.autopilot_device", "nunchuk"));
	input_walker_family_set(s_device_family);
	if(s_device_family != INPUT_CAT_NUNCHUK)
		fprintf(stderr, "[menu_ap] debug.autopilot_device: walker presents "
				"family %d (glyph proof only)\n", (int)s_device_family);
	/* debug.menus_dwell_pack_legs (--cw-set, ephemeral): capture runs opt
	 * into the three texture-pack applies; they stall the main thread 3-5+ s
	 * each on the card, which a plain run's 5 s heartbeat-stale rule would
	 * (rightly) call a hang. Pair with the +stale40 point token. */
	menu_ap_script_enable_pack_legs(
		&s_state,
		config_read_int(&gstate.config_user, "debug.menus_dwell_pack_legs", 0) != 0);
	/* The PLATFORM FACT the pure script may not test for itself (A7 part 1c,
	 * lead ruling): can a real keyboard own an OSK-backed screen's Close?
	 * input_text_active() is a hard `return false` on the Wii
	 * (source/platform/input.c:934), so the console walks those screens with
	 * the Close role normally; on PC it is true for as long as a text screen
	 * is armed, and every OSK-backed screen gates its Close behind
	 * !input_text_active() -- screen_rename_world.c:229 (armed from that
	 * screen's own reset, :113), screen_multiplayer_setup.c:1855/:1983/:2122
	 * (armed on sub-editor entry, :954/:967/:990) and
	 * screen_create_world.c:426. So on PC a button-only walker cannot leave
	 * them, which is BOUNDED here by a marker rather than faked. */
#ifdef PLATFORM_WII
	menu_ap_script_set_text_input_owns_close(&s_state, false);
#else
	menu_ap_script_set_text_input_owns_close(&s_state, true);
#endif
	/* THE PC ENTRY LEVER (push-0921 A14): `debug.menu_ap_start_np` = 1 starts
	 * the walker at the no-pointer leg instead of the top of the script, so
	 * the leg can be exercised against the REAL screens on PC -- which is
	 * where its 645-world console failure would have been caught, had the PC
	 * app been able to reach the leg at all (it parks in the create-world
	 * OSK long before it).
	 *
	 * Read HERE, next to the other debug.* walker key, so the lever is inert
	 * on both counts a lever must be inert on: the key is only consulted at
	 * all when this function runs (hw_run.menu_autopilot armed the profile),
	 * and a run that does not set it takes the byte-identical path it took
	 * before this line existed. A run that DOES set it carries
	 * MENU_AP_NP_FLAG_ENTRY_LEVER on every heartbeat from the first tick, and
	 * tools/wii_telemetry_server.py refuses any non-zero np_flags by name --
	 * so this can never be used to satisfy a console gate. */
	if(config_read_int(&gstate.config_user, "debug.menu_ap_start_np", 0) != 0) {
		menu_ap_script_start_at_np_leg(&s_state);
		fprintf(stderr,
				"[menu_ap] debug.menu_ap_start_np: starting at the no-pointer "
				"leg (step %d) -- this run is flagged entry_lever and CANNOT "
				"pass a console verdict\n",
				s_state.step);
	}
	s_armed = true;
	s_finished = false;
	s_fail_reported = false;
	s_fail_logged = false;
	s_last_output = (struct menu_ap_output) {0};
}

/* ui-build-1026/T16a route: a heap block while armed (see struct
 * copy_route below), plus the summary its getters read after it is freed.
 * Declared here so the input hooks below, which predate it, can see it. */
struct copy_route;
static struct copy_route* s_copy;
static struct {
	bool armed, finished, ok, flushed;
	int step, failed_step;
} s_copy_sum;
static bool copy_owns_input(enum input_button b, bool* pressed);

bool menu_autopilot_active(void) {
	return (s_armed && !s_finished) || (s_copy_sum.armed && !s_copy_sum.finished);
}

static enum menu_ap_screen classify_screen(void) {
	struct screen* cur = gstate.players[0].current_screen;
	if(cur == &screen_title)
		return MENU_AP_SCREEN_TITLE;
	if(cur == &screen_texturepacks)
		return MENU_AP_SCREEN_TEXTUREPACKS;
	if(cur == &screen_select_world)
		return MENU_AP_SCREEN_SELECT_WORLD;
	if(cur == &screen_create_world)
		return MENU_AP_SCREEN_CREATE_WORLD;
	if(cur == &screen_ingame)
		return MENU_AP_SCREEN_INGAME;
	if(cur == &screen_inventory)
		return MENU_AP_SCREEN_INVENTORY;
	if(cur == &screen_furnace)
		return MENU_AP_SCREEN_FURNACE;
	if(cur == &screen_chest) /* single and double chest share it (Beta GuiChest) */
		return screen_chest_sort_open() ? MENU_AP_SCREEN_CHEST_SORT
										: MENU_AP_SCREEN_CHEST;
	if(cur == &screen_dispenser)
		return MENU_AP_SCREEN_DISPENSER;
	if(cur == &screen_sign)
		return MENU_AP_SCREEN_SIGN;
	if(cur == &screen_pause)
		return MENU_AP_SCREEN_PAUSE;
	if(cur == &screen_options)
		return MENU_AP_SCREEN_OPTIONS;
	/* A pointer, not a static struct: main.c registers it under
	 * CAVEWII_MP_CLIENT. A build without it leaves the title's Multiplayer
	 * row disabled, and the no-pointer leg's bounded wait then fails by name
	 * rather than pressing on into a screen that is not there. Only the MAIN
	 * view is distinguishable from here -- screen_multiplayer_setup.c's
	 * `view` is static with no production accessor, which is exactly what
	 * part 2's screen_mps_view() adds. */
	if(screen_multiplayer_setup && cur == screen_multiplayer_setup)
		return MENU_AP_SCREEN_MULTIPLAYER;
	if(cur == &screen_rename_world)
		return MENU_AP_SCREEN_RENAME_WORLD;
	if(cur == &screen_confirm)
		return MENU_AP_SCREEN_CONFIRM;
	/* ui-build-1026/T16a: same engine-core pool (CAVEX_CLIENT_SOURCES). */
	if(cur == &screen_convert_backup)
		return MENU_AP_SCREEN_CONVERT_BACKUP;
	return MENU_AP_SCREEN_OTHER; /* incl. the transient screen_load_world */
}

/* THE WALKER TRACE (push-0921 A14). One stderr line per CHANGE of the walker's
 * reported state, for ARMED runs only.
 *
 * The console already had this: main.c writes a `menu_ap_step` line into
 * sd:/cw_boot_trace.txt on every step transition -- but hw_boot_trace_write is
 * PLATFORM_WII-only, so the PC app, the run that is supposed to find a leg
 * defect BEFORE console time is spent, printed nothing at all about the walker.
 * That asymmetry is half of why the leg's first real screens were a Wii.
 *
 * Only on change, so a 240 s run at 60 fps emits tens of lines, not 14 400;
 * the np_* fields are included because a step ordinal alone cannot say which
 * sub-leg is running or what the ring was observed on -- which is precisely
 * what the console tape could not answer either. */
static float s_trace_elapsed; /* seconds since arm, summed from the frame dt */

static void menu_ap_trace(enum menu_ap_screen screen,
						   struct menu_ap_observation obs) {
	static int last_step = -1, last_sub = -2, last_screen = -1, last_focus = -1;
	static unsigned last_entered = ~0u, last_skipped = ~0u, last_focus_r = ~0u,
					last_flags = ~0u;
	int focus = screen == MENU_AP_SCREEN_MULTIPLAYER ? obs.mps_focus
													 : obs.sworld_focus;
	if(s_state.step == last_step && s_state.np_sub == last_sub
	   && (int)screen == last_screen && focus == last_focus
	   && s_state.np_entered == last_entered
	   && s_state.np_skipped == last_skipped
	   && s_state.np_focus_reached == last_focus_r
	   && s_state.np_flags == last_flags)
		return;
	last_step = s_state.step;
	last_sub = s_state.np_sub;
	last_screen = (int)screen;
	last_focus = focus;
	last_entered = s_state.np_entered;
	last_skipped = s_state.np_skipped;
	last_focus_r = s_state.np_focus_reached;
	last_flags = s_state.np_flags;
	fprintf(stderr,
			"[menu_ap] t=%.1f step=%d max=%d screen=%d sub=%d focus=%d "
			"rows=%d entered=0x%02x skipped=0x%02x reached=0x%02x flags=0x%02x "
			"failed_step=%d failed_sub=%d\n",
			(double)s_trace_elapsed, s_state.step, s_state.max_step,
			(int)screen, s_state.np_sub, focus, obs.sworld_rows,
			s_state.np_entered, s_state.np_skipped, s_state.np_focus_reached,
			s_state.np_flags, s_state.np_failed_step, s_state.np_failed_sub);
}

/* THE ABANDONMENT RECORD (hw1026c follow-up). The heartbeat already sits on
 * its 200 B spare floor (hw_telemetry.h), so the walker's named failure is
 * sent ONCE as its own observer line instead of widening every heartbeat:
 *   {"event":"menu_ap_fail","map_fail":N,"step":S,"sub":U,"stalled":B,
 *    "np_failed_step":..,"walk_failed_step":..,"cworld_failed_step":..}
 * map_fail is 1 + the ordinal that gave up (menu_autopilot_script.h). The
 * lossless-retry producer is used and the line is re-offered every frame
 * until admitted, so a busy ring defers it rather than losing it. */
static void menu_ap_report_fail(void) {
	if(s_fail_reported || !s_state.map_fail)
		return;
	char line[320];
	int n = snprintf(line, sizeof(line),
		"{\"event\":\"menu_ap_fail\",\"map_fail\":%d,\"step\":%d,"
		"\"max\":%d,\"sub\":%d,\"stalled\":%s,\"np_failed_step\":%d,"
		"\"np_failed_sub\":%d,\"walk_failed_step\":%d,"
		"\"cworld_failed_step\":%d,\"cworld_sub\":%d,\"screen\":%d}",
		s_state.map_fail, s_state.step, s_state.max_step, s_state.np_sub,
		s_state.stalled ? "true" : "false", s_state.np_failed_step,
		s_state.np_failed_sub, s_state.walk_failed_step,
		s_state.cworld_failed_step, s_state.cworld_sub,
		(int)classify_screen());
	if(n < 0 || (size_t)n >= sizeof(line)) {
		s_fail_reported = true; /* cannot happen at these widths */
		return;
	}
	if(!s_fail_logged) {
		s_fail_logged = true;
		fprintf(stderr, "[menu_ap] ABANDONED %s\n", line);
	}
	s_fail_reported = hw_telemetry_emit_observer_retry(line)
		|| !hw_telemetry_active();
}

int menu_autopilot_map_fail(void) {
	return s_armed ? s_state.map_fail : 0;
}

static void menu_autopilot_copy_tick(void);

void menu_autopilot_frame_tick(void) {
	if(s_copy_sum.armed) {
		if(!s_front_end_blocked) /* no copy-route press under the cards */
			menu_autopilot_copy_tick();
		return;
	}
	if(!s_armed)
		return;
	if(s_finished) {
		menu_ap_report_fail();
		return;
	}

	enum menu_ap_screen screen = classify_screen();
	/* Each field is only meaningful (and only ever read by the script) while
	 * its own screen is up, but all four are cheap, side-effect-free reads
	 * that always hold valid (if stale/irrelevant) state, so they are taken
	 * unconditionally rather than behind a screen test that could drift out
	 * of step with the script's own idea of where it is. */
	struct menu_ap_observation obs = {
		.cworld_mode_survival = screen_cworld_mode_is_survival(),
		.sworld_focus = screen_sworld_focus_target(),
		/* NOT called directly -- see menu_autopilot_set_mps_observers. */
		.mps_focus = s_mps_focus ? s_mps_focus() : 0,
		.mps_view = s_mps_view ? s_mps_view() : 0,
		.mps_rows = s_mps_rows ? s_mps_rows() : 0,
		.mps_observed = s_mps_focus != NULL && s_mps_view != NULL
			&& s_mps_rows != NULL,
		/* Both are in the engine core with this file (screen_select_world.c,
		 * screen_confirm.c), so unlike the Multiplayer pair they are direct
		 * calls -- see the seam comment above for why that distinction is a
		 * link-pool fact and not a style choice. */
		.sworld_rows = screen_sworld_row_count(),
		.confirm_focus_safe = screen_confirm_focus_is_safe(),
		/* Create World: the same engine-core link pool, direct calls. */
		.cworld_editing = screen_cworld_editing(),
		.cworld_create_ready = screen_cworld_create_ready(),
		.pause_focus = screen_pause_focused_action(),
		.title_focus = screen_title_focused_row(),
		/* ui-build-1026 T15: Texture packs and Create World, all engine-core
		 * read-only observers (screen.h). */
		.texpacks_rows = screen_texpacks_row_count(),
		.texpacks_selected = screen_texpacks_selected_row(),
		.texpacks_focused_row = screen_texpacks_focused_row(),
		.texpacks_apply_focused = screen_texpacks_apply_focused(),
		.texpacks_apply_enabled = screen_texpacks_apply_enabled(),
		.texpacks_pending_selected = screen_texpacks_pending_selected(),
		.texpacks_busy = screen_texpacks_busy(),
		.texpacks_pick_row = -1,
		.cworld_focus = screen_cworld_focus_target(),
		.cworld_details_open = screen_cworld_details_open(),
		.cworld_osk_profile = screen_cworld_osk_profile(),
		.cworld_osk_target = screen_cworld_osk_target(),
		.cworld_text_sig = screen_cworld_text_signature(),
		.inv_page = screen_inventory_page(),
		.inv_tab = screen_inventory_crafting_tab(),
		.front_end_blocked = s_front_end_blocked,
	};
	/* The pack card the one-click pick may aim at: a row OTHER than the
	 * selected one when one is fully visible (so the click proves it
	 * selects), else the selected row itself. */
	if(screen == MENU_AP_SCREEN_TEXTUREPACKS) {
		int sel = obs.texpacks_selected;
		const int cand[3] = {sel + 1, sel - 1, sel};
		float x, y;
		for(int k = 0; k < 3 && sel >= 0; k++)
			if(screen_texpacks_row_center(cand[k], gfx_width(), gfx_height(),
										  &x, &y)) {
				obs.texpacks_pick_row = cand[k];
				break;
			}
	}
	menu_ap_script_tick(&s_state, screen, obs, gstate.stats.dt,
						 &s_last_output);
	s_finished = s_state.finished;
	if(s_finished)
		menu_ap_report_fail();
	/* The SAME dt the script is driven by, so a trace timestamp can never
	 * disagree with the dwell/wait budgets it is used to judge. */
	s_trace_elapsed += gstate.stats.dt;
	menu_ap_trace(screen, obs);

	/* Container/sign legs: re-aim the held look and/or request one
	 * empty-hand right-click pulse (autopilot.c aim mode owns both). */
	if(s_last_output.aim_set)
		autopilot_aim_set_deg(s_last_output.aim_yaw_deg, s_last_output.aim_pitch_deg);
	if(s_last_output.place_pulse)
		autopilot_request_place();

	s_pointer_valid = true;
	if(s_last_output.pointer_target == MENU_AP_PT_SWORLD_CREATE)
		screen_sworld_create_button_center(gfx_width(), gfx_height(),
											&s_pointer_x, &s_pointer_y);
	else if(s_last_output.pointer_target == MENU_AP_PT_TEXPACKS_ROW)
		s_pointer_valid = screen == MENU_AP_SCREEN_TEXTUREPACKS
			&& screen_texpacks_row_center(s_last_output.pointer_row,
										  gfx_width(), gfx_height(),
										  &s_pointer_x, &s_pointer_y);
}

bool menu_autopilot_force_button(enum input_button b, bool* out) {
	/* save_reload_3m (R12): its one-frame pause-menu press. Answers false
	 * on its first statement unless that profile armed the driver. */
	if(save_reload_force_button((int)b, out))
		return true;
	if(s_copy_sum.armed)
		return copy_owns_input(b, out);
	if(!s_armed || s_finished)
		return false;
	if(!s_last_output.press || s_last_output.button != b)
		return false;
	*out = true;
	return true;
}

bool menu_autopilot_force_pointer(float* x, float* y, float* angle,
								   bool* available) {
	/* T16a: the copy route is button-only and OWNS the pointer for its whole
	 * window (reads unavailable), for the same stray-IR reason as below. */
	if(s_copy_sum.armed && !s_copy_sum.finished) {
		*available = false;
		return true;
	}
	if(!s_armed || s_finished)
		return false;
	/* THE NO-POINTER SEAM (A7 part 1a, Astra Lane A item 5). This function is
	 * the ONE place the walker touches the pointer, and input_pointer()
	 * (source/platform/input.c:213 PC / :861 Wii) is the ONE production query
	 * every screen reads -- so asserting unavailability HERE is the screens
	 * seeing exactly what they see on a console with the Remote pointed away.
	 * No second query, and no screen changes.
	 *
	 * It is checked FIRST so it DOMINATES pointer_target: a target asserted
	 * by mistake inside the leg can never hand the pointer back. The script
	 * never emits both (test_menu_autopilot_script's log_push pins that on
	 * every tick of every case), so this is a backstop, not an arbitration
	 * the leg depends on.
	 *
	 * Inert outside the leg: pointer_suppressed is false on every other step
	 * and on an idle/finished output, and the two early-outs above mean an
	 * unarmed or finished walker never reaches this line at all -- the real
	 * pointer is returned untouched, byte-identical to before A7. */
	if(s_last_output.pointer_suppressed) {
		*available = false;
		return true;
	}
	/* Owns the ENTIRE scripted window, not only the tick it asserts a
	 * position -- see the header: a coincidentally-valid stray IR reading
	 * must never land an unintended click while this profile is driving. */
	if(s_last_output.pointer_target == MENU_AP_PT_NONE || !s_pointer_valid) {
		*available = false;
		return true;
	}
	*x = s_pointer_x;
	*y = s_pointer_y;
	*angle = 0.0f;
	*available = true;
	return true;
}

int menu_autopilot_screen(void) {
	return s_armed ? (int)classify_screen() : -1;
}

int menu_autopilot_step(void) {
	return s_armed ? s_state.step : 0;
}

int menu_autopilot_max_step(void) {
	return s_armed ? s_state.max_step : 0;
}

int menu_autopilot_step_count(void) {
	return menu_ap_script_step_count();
}

int menu_autopilot_step_required_for_pass(void) {
	return menu_ap_script_step_required_for_pass();
}

bool menu_autopilot_sign_reached(void) {
	return s_armed && s_state.sign_reached;
}

bool menu_autopilot_sign_wait_timed_out(void) {
	return s_armed && s_state.sign_wait_timed_out;
}

int menu_autopilot_np_failed_step(void) {
	return s_armed ? s_state.np_failed_step : 0;
}

bool menu_autopilot_np_leg_done(void) {
	return s_armed && s_state.np_leg_done;
}

bool menu_autopilot_np_osk_legs_skipped(void) {
	return s_armed && s_state.np_osk_legs_skipped;
}

int menu_autopilot_sub(void) {
	return s_armed ? s_state.np_sub : 0;
}

int menu_autopilot_np_failed_sub(void) {
	return s_armed ? s_state.np_failed_sub : 0;
}

unsigned menu_autopilot_np_focus_reached(void) {
	return s_armed ? s_state.np_focus_reached : 0u;
}

unsigned menu_autopilot_np_entered(void) {
	return s_armed ? s_state.np_entered : 0u;
}

unsigned menu_autopilot_np_skipped(void) {
	return s_armed ? s_state.np_skipped : 0u;
}

bool menu_autopilot_np_mps_unobserved(void) {
	return s_armed && s_state.np_mps_unobserved;
}

unsigned menu_autopilot_np_flags(void) {
	return s_armed ? s_state.np_flags : 0u;
}

int menu_autopilot_np_rows(void) {
	/* -1 (never latched) is reported as 0: the heartbeat field is the LATCHED
	 * world count, and "no count was taken" and "no worlds" are told apart by
	 * np_entered/np_skipped, not by a negative here. */
	return s_armed && s_state.np_rows > 0 ? s_state.np_rows : 0;
}

bool menu_autopilot_pointer_suppressed(void) {
	/* Mirrors menu_autopilot_force_pointer's own gate: a finished walker has
	 * handed the pointer back, so it reports false even if the last output
	 * still carried the flag. */
	return s_armed && !s_finished && s_last_output.pointer_suppressed;
}

/* ======================================================================
 * ui-build-1026/T16a: the world_copy_convert route's impure half.
 *
 * Memory (review round 1, Sol minor): everything the route needs -- its
 * state (with the card listing), output, hooks and the 2 KiB marker line --
 * lives in ONE heap block allocated by menu_autopilot_arm_copy_route and
 * freed once the route has ended and every marker was admitted. An unarmed
 * boot keeps only the pointer and the four summary words below (.bss, 24 B
 * on Broadway), and allocates nothing.
 * ====================================================================== */
struct copy_route {
	struct menu_ap_copy_state st;
	struct menu_ap_copy_output out;
	struct menu_autopilot_copy_hooks hooks;
	bool logged_end;
	char line[MENU_AP_COPY_LINE_MAX];
	/* the listing scratch, kept off the main-thread stack */
	char names[MENU_AP_COPY_CARD_NAMES + 1][MENU_AP_COPY_CARD_NAME_MAX];
};

/* Every hook, including the interruption seam: a route that could not
 * interrupt reports itself unobserved rather than pretending. */
static bool copy_hooks_complete(const struct menu_autopilot_copy_hooks* h) {
	return h->sworld_settled && h->sworld_selected_row
		&& h->sworld_row_of_directory && h->sworld_row_needs_convert
		&& h->sworld_partial_copy_visible && h->cb_state
		&& h->cb_focused_action && h->cb_fail && h->cb_copy_error
		&& h->cb_percent && h->cb_volume && h->cb_convert_outcome
		&& h->cb_backup_published && h->cb_interrupt;
}

bool menu_autopilot_arm_copy_route(int variant, const char* fixture,
								   const char* expected_root,
								   const struct menu_autopilot_copy_hooks* h) {
	if(s_copy) /* armed once per boot */
		return false;
	s_copy = calloc(1, sizeof(*s_copy));
	if(!s_copy) {
		fprintf(stderr, "[menu_ap] world_copy_convert: out of memory, route "
				"NOT armed\n");
		s_copy_sum.armed = true; /* finished+failed: no PASS without markers */
		s_copy_sum.finished = true;
		s_copy_sum.failed_step = MENU_AP_COPY_STEP_ISOLATION;
		return false;
	}
	s_copy_sum.armed = true;
	/* REVIEW ROUND 1 (Sol B1): the route runs only in the isolated per-run
	 * worlds root. Read the setting BACK: if the setter failed, or anything
	 * else leaves a different root (the player's saves), the route is refused
	 * by name -- no hooks, no fault seams, no presses, no listing of that
	 * root, no disk writes -- and only wcc_begin / wcc_card(root_ok=false) /
	 * wcc_end(failed_step=ISOLATION) are sent. */
	const char* configured
		= config_read_string(&gstate.config_user, "paths.worlds", "");
	if(!menu_ap_copy_root_isolated(configured, expected_root)) {
		menu_ap_copy_refuse(&s_copy->st, (enum menu_ap_copy_variant)variant,
							fixture);
		fprintf(stderr, "[menu_ap] world_copy_convert REFUSED: worlds root "
				"'%s' is not the isolated '%s'\n", configured ? configured : "",
				expected_root ? expected_root : "");
		return false;
	}
	if(h)
		s_copy->hooks = *h;
	menu_ap_copy_reset(&s_copy->st, (enum menu_ap_copy_variant)variant, fixture);
	fprintf(stderr,
			"[menu_ap] world_copy_convert route armed: variant=%s fixture=%s "
			"root=%s\n",
			menu_ap_copy_variant_name((enum menu_ap_copy_variant)variant),
			s_copy->st.fixture, expected_root);
	return true;
}

static struct menu_ap_copy_obs copy_observe(enum menu_ap_screen screen) {
	const struct menu_autopilot_copy_hooks* h = &s_copy->hooks;
	struct menu_ap_copy_obs o = {
		.observed = copy_hooks_complete(h),
		.fixture_row = -1,
		.backup_row = -1,
		.sworld_selected = -1,
		.cb_state = MENU_AP_CB_NONE,
		.cb_action = MENU_AP_CB_ACT_NONE,
		.cb_percent = -1,
	};
	if(!o.observed)
		return o;
	o.title_focus = screen_title_focused_row();
	o.sworld_focus = screen_sworld_focus_target();
	o.sworld_rows = screen_sworld_row_count();
	if(screen == MENU_AP_SCREEN_SELECT_WORLD) {
		char backup[MENU_AP_COPY_FIXTURE_MAX + 16];
		snprintf(backup, sizeof(backup), "%s (backup)", s_copy->st.fixture);
		o.sworld_settled = h->sworld_settled();
		o.sworld_selected = h->sworld_selected_row();
		o.fixture_row = h->sworld_row_of_directory(s_copy->st.fixture);
		o.fixture_needs_convert
			= o.fixture_row >= 0 && h->sworld_row_needs_convert(o.fixture_row);
		o.backup_row = h->sworld_row_of_directory(backup);
		o.partial_visible = h->sworld_partial_copy_visible();
	}
	if(screen == MENU_AP_SCREEN_CONVERT_BACKUP) {
		o.cb_state = h->cb_state();
		o.cb_action = h->cb_focused_action();
		o.cb_fail = h->cb_fail();
		o.cb_copy_error = h->cb_copy_error();
		o.cb_percent = h->cb_percent();
		if(!h->cb_volume(&o.cb_free_bytes, &o.cb_cluster_bytes,
						 &o.cb_required_bytes)) {
			o.cb_free_bytes = 0;
			o.cb_cluster_bytes = 0;
			o.cb_required_bytes = 0;
		}
		o.cb_convert_outcome = h->cb_convert_outcome();
		o.cb_backup_published = h->cb_backup_published();
		/* read directly (no hook): a plain observer of the same screen */
		o.cb_cleanup_done = screen_convert_backup_cleanup_done();
	}
	return o;
}

/* Re-offer the lowest pending marker every frame until the ring admits it
 * (the lossless-retry producer, as menu_ap_report_fail uses). */
static void copy_flush_markers(void) {
	for(int guard = 0; guard < MENU_AP_WCC_COUNT; guard++) {
		int k = menu_ap_copy_next_marker(&s_copy->st, s_copy->line,
										 sizeof(s_copy->line));
		if(k < 0)
			return;
		if(!hw_telemetry_emit_observer_retry(s_copy->line)
		   && hw_telemetry_active())
			return; /* busy ring: offered again next frame */
		fprintf(stderr, "[menu_ap] %s\n", s_copy->line);
		menu_ap_copy_marker_sent(&s_copy->st, k);
	}
}

/* Decision 2026-10-27 (T16a rig gap): the card-side evidence of leftovers
 * is taken ON THE DEVICE. The ISOLATED per-run worlds root (checked at arm;
 * a refused route never lists anything) is listed once the route has ended
 * (or at the interrupt). Include dot names and each marked world's
 * relative "/.incomplete" path: the final folder exists during copying,
 * but Select World must hide it until the marker is removed. */
static void copy_take_card(void) {
	const char* root
		= config_read_string(&gstate.config_user, "paths.worlds", "");
	const char* ptrs[MENU_AP_COPY_CARD_NAMES + 1];
	int n = 0;
	bool overflow = false, marker_ok = true;
	DIR* d = root && root[0] ? opendir(root) : NULL;
	if(d) {
		struct dirent* e;
		for(;;) {
			errno = 0;
			e = readdir(d);
			if(!e) {
				if(errno) marker_ok = false;
				break;
			}
			if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			if(n > MENU_AP_COPY_CARD_NAMES
			   || strlen(e->d_name) >= MENU_AP_COPY_CARD_NAME_MAX) {
				overflow = true; /* never shortened into a different name */
				continue;
			}
			snprintf(s_copy->names[n], sizeof(s_copy->names[n]), "%s",
					 e->d_name);
			ptrs[n] = s_copy->names[n];
			n++;
			/* Inspect markers even when level.dat has not been copied yet. */
			string_t marker;
			string_init_printf(marker, "%s/%s/.incomplete", root, e->d_name);
			struct stat st;
			if(stat(string_get_cstr(marker), &st) == 0) {
				if(n > MENU_AP_COPY_CARD_NAMES
				   || strlen(e->d_name) + strlen("/.incomplete")
					  >= MENU_AP_COPY_CARD_NAME_MAX) {
					overflow = true;
				} else {
					snprintf(s_copy->names[n], sizeof(s_copy->names[n]),
						 "%s/.incomplete", e->d_name);
					ptrs[n] = s_copy->names[n];
					n++;
				}
			} else if(errno != ENOENT && errno != ENOTDIR) {
				marker_ok = false; /* unreadable is never evidence of absence */
			}
			string_clear(marker);
		}
		if(closedir(d) != 0) marker_ok = false;
	}
	menu_ap_copy_set_card(&s_copy->st, d != NULL && marker_ok, ptrs, n);
	if(overflow)
		s_copy->st.card_truncated = true;
	fprintf(stderr, "[menu_ap] wcc card: root=%s ok=%d names=%d%s\n", root,
			d != NULL && marker_ok, n, overflow ? " (overflow)" : "");
}

static bool copy_owns_input(enum input_button b, bool* pressed) {
	if(!s_copy || s_copy->st.finished || !s_copy->out.press
	   || s_copy->out.button != b)
		return false;
	*pressed = true;
	return true;
}

static void copy_summarize(void) {
	s_copy_sum.finished = s_copy->st.finished;
	s_copy_sum.ok = s_copy->st.ok;
	s_copy_sum.step = s_copy->st.step;
	s_copy_sum.failed_step = s_copy->st.failed_step;
}

static void menu_autopilot_copy_tick(void) {
	if(!s_copy)
		return; /* freed after the last marker (or never allocated) */
	enum menu_ap_screen screen = classify_screen();
	if(!s_copy->st.finished) {
		menu_ap_copy_tick(&s_copy->st, screen, copy_observe(screen),
						  gstate.stats.dt, &s_copy->out);
		if(s_copy->out.interrupt && s_copy->hooks.cb_interrupt)
			s_copy->hooks.cb_interrupt();
	} else {
		s_copy->out = (struct menu_ap_copy_output) {0};
	}
	if(menu_ap_copy_card_wanted(&s_copy->st))
		copy_take_card();
	if(s_copy->st.finished && !s_copy->logged_end) {
		s_copy->logged_end = true;
		fprintf(stderr,
				"[menu_ap] world_copy_convert route %s at step %d "
				"(failed_step %d)\n",
				s_copy->st.ok ? "completed" : "ABANDONED", s_copy->st.step,
				s_copy->st.failed_step);
	}
	copy_flush_markers();
	copy_summarize();
	if(s_copy->st.finished
	   && (s_copy->st.markers_ready & ~s_copy->st.markers_sent) == 0u) {
		s_copy_sum.flushed = true;
		free(s_copy);
		s_copy = NULL;
	}
}

int menu_autopilot_copy_step(void) {
	return s_copy_sum.armed ? s_copy_sum.step : 0;
}

int menu_autopilot_copy_failed_step(void) {
	return s_copy_sum.armed ? s_copy_sum.failed_step : 0;
}

bool menu_autopilot_copy_finished(void) {
	return s_copy_sum.armed && s_copy_sum.finished;
}

bool menu_autopilot_copy_ok(void) {
	return s_copy_sum.armed && s_copy_sum.ok;
}

bool menu_autopilot_copy_markers_flushed(void) {
	return s_copy_sum.armed && s_copy_sum.flushed;
}
