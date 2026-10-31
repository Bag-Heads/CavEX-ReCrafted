/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	Pure/headless test for the menus_dwell hardware profile's script
	(source/game/menu_autopilot_script.c). No gfx/game_state/GLFW dependency:
	a small MOCK screen-transition table stands in for the real GUI screens,
	encoding this profile's documented contract (screen_title.c/screen_
	select_world.c/screen_create_world.c/screen_pause.c/screen_options.c/
	screen_sign.c, traced by hand against the live tree when this profile was
	authored) -- see menu_autopilot_script.h's header comment for the per-step
	rationale. This proves the STATE MACHINE is correct (dwell timing, bounded
	wait/timeout, press-edge spacing, screen-gating, the HOME-on-title safety
	property); it does not and cannot prove the real screens still behave this
	way -- that is the real-Wii PASS.json's job, plus the individual screens'
	own existing tests.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/game/menu_autopilot_script.h"
int menu_ap_script_step_cworld_nav_mode(void); /* T05, defined in menu_autopilot_script.c */
#include "../source/game/menu_dwell_demo.h"

/* A conservative, sub-frame-rate dt so no dwell/wait is skipped past. A
 * variable so the stall-guard case can run the SAME walk at the 10 fps the
 * real-Wii runs dipped to (Sol round-3 review). */
static float g_tick_dt = 1.0f / 20.0f;
#define TICK_DT g_tick_dt

/* Sized comfortably above the worst-case tick count any test below drives
 * (the happy path alone is ~600+ ticks at 20Hz: DWELL_MENU x8 + DWELL_SIGN +
 * SETTLE_INGAME dominate). A silently-truncated log previously hid the
 * later presses entirely and produced a confusing wrong-count assertion
 * failure instead of an honest bounds problem -- size generously and assert
 * against overflow explicitly instead. */
#define MOCK_LOG_CAP 16384

struct mock_click_log {
	enum input_button buttons[MOCK_LOG_CAP];
	enum menu_ap_pointer_target pointer_targets[MOCK_LOG_CAP];
	/* push-0921 A7 part 1a: the no-pointer assertion, per tick, so the leg's
	 * "suppressed for its WHOLE duration" claim is checked tick-by-tick and
	 * not merely at its boundaries. */
	bool suppressed[MOCK_LOG_CAP];
	int steps[MOCK_LOG_CAP];
	size_t count;
};

static void log_reset(struct mock_click_log* log) {
	memset(log, 0, sizeof(*log));
}

static void log_push(struct mock_click_log* log, const struct menu_ap_output* out,
					 int step) {
	assert(log->count < MOCK_LOG_CAP); /* a silent drop here would corrupt every count assertion below */
	/* THE DOMINANCE INVARIANT, checked on EVERY tick of every case below: the
	 * script never asserts a synthetic pointer and no-pointer at the same
	 * time. menu_autopilot.c's seam resolves the pair suppression-first as a
	 * backstop; this is what makes that backstop unreachable in practice. */
	assert(!(out->pointer_suppressed
			 && out->pointer_target != MENU_AP_PT_NONE));
	log->buttons[log->count] = out->press ? out->button : IB_COUNT;
	log->pointer_targets[log->count] = out->pointer_target;
	log->suppressed[log->count] = out->pointer_suppressed;
	log->steps[log->count] = step;
	log->count++;
}

static size_t log_count_button(const struct mock_click_log* log,
								enum input_button b) {
	size_t n = 0;
	for(size_t k = 0; k < log->count; k++)
		if(log->buttons[k] == b)
			n++;
	return n;
}

/* The same count, restricted to the ticks OUTSIDE the no-pointer leg. The
 * exact per-button counts below were traced by hand against the script's step
 * table and are the thing that catches a silent over/undershoot in the OSK
 * and menu navigation. The A7 leg's own presses are data-dependent (how many
 * DOWNs reach an action strip depends on how many rows the list has), so
 * folding them into those totals would turn a precise contract into a number
 * nobody can re-derive. The leg is held to its own, stronger contract instead
 * -- which control the ring OBSERVABLY reached, and which screen actually
 * opened -- so nothing is lost by excluding it here. */
static size_t log_count_button_outside_leg(const struct mock_click_log* log,
											enum input_button b) {
	size_t n = 0;
	for(size_t k = 0; k < log->count; k++)
		if(log->buttons[k] == b && !log->suppressed[k])
			n++;
	return n;
}

/* The complement: presses made INSIDE the no-pointer leg. Used only where the
 * count is re-derivable from the case's own list length (A14's long-list
 * case), never as a blanket contract -- see the note above. */
static size_t log_count_button_in_leg(const struct mock_click_log* log,
									   enum input_button b) {
	size_t n = 0;
	for(size_t k = 0; k < log->count; k++)
		if(log->buttons[k] == b && log->suppressed[k])
			n++;
	return n;
}

/* Index of the first `b` at or after `from`, or log->count when there is
 * none. log_push runs EVERY tick (a tick with no press logs IB_COUNT), so an
 * index IS a tick number and a difference of indices IS a tick gap -- which
 * is how the dwell between the two inventory clicks below is measured. */
static size_t log_find(const struct mock_click_log* log, enum input_button b,
					   size_t from) {
	for(size_t k = from; k < log->count; k++)
		if(log->buttons[k] == b)
			return k;
	return log->count;
}

/* Mirrors screen_title.c's D-pad stepping precisely enough for this test:
 * TB_SINGLE=0 is the default selection and Multiplayer (TB_MULTI) sits
 * between it and "Mods and Texture Packs". Multiplayer is ENABLED whenever
 * main.c ran (title_button_enabled -> screen_multiplayer_setup != NULL,
 * registered unconditionally under CAVEWII_MP_CLIENT on PC and Wii alike),
 * so TWO GUI_DOWNs are needed to reach Mods -- one lands on Multiplayer. */
struct mock_engine {
	enum menu_ap_screen screen;
	int title_downs; /* GUI_DOWNs seen since the last title reset */
	/* ui-build-1026 T15: activates on the Tutorial world row (must stay 0:
	 * it would load the SHIPPED tutorial world), and a toggle that knocks the
	 * title focus off Tutorial once the ring has rested there `tutorial_drift`
	 * ticks (the dwell must fail by name, not re-earn its credit). */
	int tutorial_clicks;
	int tutorial_drift;
	int tutorial_rest_ticks;
	/* ui-build-1026 T15, Texture packs: two rows (0 = the current pack), the
	 * selection, the pending choice (-1 none), the ring on Apply, and a
	 * reload in flight. click_applies is the regression where ONE pointer
	 * click on a row applies the pack (it must only select). */
	int texpacks_rows, texpacks_sel, texpacks_pending;
	bool texpacks_apply_focus, texpacks_busy, texpacks_click_applies;
	int texpacks_row_clicks;
	/* Sol T15 review: the ring drifts from Apply to another strip button
	 * `texpacks_focus_drift` ticks after the pick (selection/pending kept). */
	int texpacks_focus_drift, texpacks_drift_ticks;
	bool texpacks_other_button;
	/* ui-build-1026 T15, Create World: the New seed sub-focus, the Details
	 * card, the open editor's field (0 name, 2 seed), its focused key cell,
	 * a typed-but-uncommitted draft, and the committed text. Toggles:
	 * cancel_commits (Cancel keeps the typed key -- must fail by name) and
	 * details_never_opens. */
	bool cworld_seed_new, cworld_details;
	int cworld_edit_row, cworld_osk_target;
	int cworld_draft;
	unsigned cworld_committed;
	bool cworld_cancel_commits, cworld_details_never_opens;
	int cworld_details_opens, cworld_osk_opens, cworld_keys_typed;
	/* ui-build-1026 T15c, Inventory pages: page 0/1, the Crafting tab, a
	 * swap that never lands (regression toggle), and any A on the Crafting
	 * page (would Craft: must stay 0). */
	int inv_page, inv_tab, inv_swaps, inv_tab_moves, craft_clicks;
	bool inv_swap_dead;
	/* CONSOLE create-world screen (2026-09-01): the OSK is a MODAL and the
	 * form is a five-row focus ring, so the mock tracks both. */
	bool cworld_modal;
	int cworld_down_seen;  /* GUI_DOWN inside the modal (the OSK grid) */
	int cworld_right_seen; /* GUI_RIGHT inside the modal (the OSK grid) */
	int cworld_focus;	   /* 0 name, 1 game mode, 2 seed, 3 create, 4 details/cancel */
	int cworld_mode;	   /* 0 survival, 1 creative */
	bool cworld_saw_creative; /* the selector reached Creative at least once */
	bool cworld_created_creative; /* the world was submitted while creative */
	bool sign_autopilot_would_open; /* test toggles: does the "existing autopilot" pulse succeed this run? */
	bool home_pressed_on_title;	 /* regression guard: HOME on title quits the real game */
	bool pointer_target_off_sworld; /* regression guard: pointer asserted while NOT on select-world */
	bool block_sworld_create; /* test toggles: simulate the Create button never responding */
	bool title_landed_on_multiplayer; /* regression guard: the stale one-DOWN nav */
	/* 2026-09-02 hardening (staged_menus_dwell_20260902_063552's adversarial
	 * review): test toggles for the STEP_CWORLD_MODE_SURVIVAL skew fix. */
	bool cworld_mode_skew; /* apply a LEFT press's mode flip ONE TICK LATE instead of immediately */
	bool cworld_left_pending; /* cworld_mode_skew's one-tick-delayed LEFT, still to apply */
	bool cworld_swallow_down; /* the form ring ignores GUI_DOWN (Create never reached) */
	bool cworld_block_create; /* Create never reports ready (e.g. a disabled Create) */
	bool cworld_open_on_down; /* a GUI_DOWN opens the editor modal (something else took over) */
	bool block_mode_toggle;   /* simulate the selector never responding to LEFT/RIGHT at all */
	/* Container legs (2026-09-11): the mock camera/world. A place pulse while
	 * in-game opens the screen whose menu_dwell_demo.h aim the walker holds
	 * (the real ray hits that block); nothing opens when containers_open is
	 * false (a missed aim on hardware). */
	int aim_yaw, aim_pitch;
	bool containers_open;
	int chest_ups;	/* GUI_UP presses on a chest: must stay 0 (Sort parked 2026-10-25; the leg only looks, then closes) */
	int chest_clicks; /* GUI_CLICK presses on a chest: must stay 0 -- an A there would pick up a stack */
	int inventory_rights; /* GUI_RIGHT presses on the inventory (slot focus -> portrait turn, b33e5f3c) */
	/* Drop-footer leg (b1-chat-opener-0919/16 G-0b): GUI_CLICK presses made
	 * while the inventory is up, and the FOCUS INDEX each one landed on. The
	 * script owns no slot concept (it stays pure), so "the same slot twice"
	 * is expressed here as "no focus move happened between the two clicks". */
	int inventory_clicks;
	int inventory_click_focus[4];
	/* A transfer action the walker must never synthesize on ANY screen: the
	 * capture leg picks a stack up and puts it straight back, it does not
	 * discard, halve or quick-move one. */
	bool forbidden_action_pressed;
	int opened_furnace, opened_chest, opened_dispenser;
	int pack_applies;	/* texture-pack screen: D-pad clicks that apply the selected row */
	/* The pause FOCUS row, today's approved full layout (screen_pause.c,
	 * 91bd8295c): 0 Back / 1 Achievements / 2 Options / 3 Texture packs /
	 * 4 Save and quit, clamped at both ends. The real screen is exercised by
	 * tests/test_menus_dwell_create_flow.c; this is the pure model of it. */
	int pause_downs;
	int achievements_opened; /* activate on the Achievements row */
	bool pause_swallow_down;  /* the pause ignores GUI_DOWN (focus stuck on Back) */
	int quit_done;		/* Save and quit accepted (title came back) */
	bool quit_fails;	/* the server refuses the unload: the pause stays up */
	/* ---- the NO-POINTER leg (push-0921 A7 part 1) ---------------------- */
	bool texpacks_seen;	/* the run reached Texture Packs at least once */
	int mp_visits;		/* screen_multiplayer_setup's MAIN view entered */
	/* Close (IB_INVENTORY) round-trips back to the title, per screen. The
	 * real screens: screen_select_world.c:1342 and screen_texturepacks.c:699
	 * (the Close role joined IB_HOME on both under A1/A5) and
	 * screen_multiplayer_setup.c:1609 (MPS_BUTTON_BACK). */
	int sworld_closes, texpacks_closes, mp_closes;
	/* hw1030i: D-pad row moves on Texture Packs (screen_texturepacks.c
	 * UP/DOWN move the selection and reveal it), the deepest row reached,
	 * and the ticks the LAST row sat selected (the scroll capture dwell). */
	int texpacks_max_sel, texpacks_last_row_ticks;
	/* THE SAFETY GUARD the whole count-free design exists for: a GUI_CLICK on
	 * Select World with NO synthetic pointer is rule (3) ACTIVATE(selected),
	 * and with the ring still in the row list that is sworld_play_selected()
	 * -- it LOADS a world (screen_select_world.c:1066, :976). The no-pointer
	 * leg must never issue one, on any of these cases. The Multiplayer twin
	 * would JOIN a server (screen_multiplayer_setup.c:1320). */
	bool sworld_row_activated;
	bool mp_row_activated;
	bool mp_close_blocked; /* test toggle: the Close never leaves the MP screen */
	/* ---- part 2: the two focus rings, modelled from the real screens ----
	 * Select World's strip in VISUAL order (screen_select_world.c's
	 * sworld_focus_order): Play / Create / Rename / Delete / Cancel, with
	 * Play, Rename and Delete enabled only while a row is selected. The
	 * Multiplayer strip is in ENUM order (its focus_step walks that): Join /
	 * Direct / Add / Edit / Delete / Refresh / Back, with Join, Edit, Delete
	 * and Refresh row-gated. LEFT/RIGHT skip disabled buttons and CLAMP at
	 * both ends on both screens. */
	int world_rows;	   /* rows in the world list */
	int sworld_sel;	   /* selected row */
	int sworld_slot;   /* enum menu_ap_sworld_focus */
	bool sworld_options_open;
	int server_rows;   /* saved servers */
	int mps_sel;
	int mps_slot;	   /* enum menu_ap_mps_focus */
	bool mps_options_open;
	int mps_view;	   /* enum menu_ap_mps_view */
	bool world_deleted;	  /* the confirm answered YES -- must never happen */
	bool sub_editor_saved; /* a sub-editor was left by Save, not by Close */
	int confirm_opens;
	int rename_opens, cworld_opens;
	int view_opens[5];	  /* per enum menu_ap_mps_view */
	bool mps_unobserved;  /* nothing registered the Multiplayer observers */
	/* A11 / review A9-5, A9-6. confirm_unsafe makes the modal open on the
	 * DESTRUCTIVE answer (a regression of screen_confirm's own default);
	 * delete_eats_row makes the Close leak a deletion, which is the outcome
	 * the count comparison exists to catch at any list length. */
	bool confirm_unsafe;
	bool delete_eats_row;
	bool rename_eats_row;
	/* A14 (the console defect, push-0921 A13): the Select World list STOPS
	 * responding to GUI_DOWN once the first sub-leg has been through the
	 * create-world screen. The mock was too kind before this -- its default
	 * three-row list let the DOWN walk reach the action strip inside any
	 * budget, so a walk that ran OUT of presses was never modelled at all, and
	 * the leg's first real card (645 worlds) found that on the console. Keyed
	 * off cworld_opens rather than a sub-leg index because the mock owns no
	 * sub-leg concept: "the first sub-leg has finished" is exactly "the
	 * create-world screen has been opened once". */
	bool sworld_swallow_downs_after_cworld;
	/* A15: the same shape for the RIGHT entry. `swallow_list_right` is the
	 * regression where a screen stops honouring the new entry but the list
	 * still walks -- the walk must then FALL BACK to the A14 DOWN converge and
	 * still complete, which is the only way that fallback is proven to be real
	 * rather than decorative. sworld_swallow_downs_after_cworld swallows BOTH
	 * ways out of the list, so its case still reaches the exhaustion it is
	 * about. */
	bool sworld_swallow_list_right;
	bool mps_swallow_list_right;
	bool mps_swallow_join_right;
};

static void mock_reset(struct mock_engine* m, bool sign_autopilot_would_open) {
	memset(m, 0, sizeof(*m));
	m->screen = MENU_AP_SCREEN_TITLE;
	m->sign_autopilot_would_open = sign_autopilot_would_open;
	m->containers_open = true;
	/* The default card the no-pointer leg walks: worlds exist (the script
	 * created one long before the leg runs) and one saved server exists, so
	 * every conditionally-enabled control is reachable. The empty-list cases
	 * set these to 0 and expect the NAMED SKIPS instead. */
	m->world_rows = 3;
	m->server_rows = 1;
	m->sworld_slot = MENU_AP_SWORLD_LIST;
	m->mps_slot = MENU_AP_MPS_TAB_JOIN;
	m->mps_view = MENU_AP_MPS_VIEW_MAIN;
	m->texpacks_rows = 2;
	m->texpacks_pending = -1;
	m->cworld_committed = 0x5eedu;
}

/* screen_texturepacks.c screen_texpacks_* as the walker reads them. */
static int mock_texpacks_pick_row(const struct mock_engine* m) {
	int sel = m->texpacks_sel;
	if(sel + 1 < m->texpacks_rows)
		return sel + 1;
	return sel > 0 ? sel - 1 : sel;
}

/* screen_cworld_focus_target(): the mock's ring (0 name, 1 mode, 2 seed,
 * 3 create, 4 details, 5 cancel) by meaning. */
static int mock_cworld_focus(const struct mock_engine* m) {
	if(m->cworld_details)
		return MENU_AP_CWORLD_DETAILS_BACK;
	switch(m->cworld_focus) {
		case 0: return MENU_AP_CWORLD_NAME;
		case 1: return m->cworld_mode ? MENU_AP_CWORLD_MODE_CREATIVE
									  : MENU_AP_CWORLD_MODE_SURVIVAL;
		case 2: return m->cworld_seed_new ? MENU_AP_CWORLD_NEW_SEED
										  : MENU_AP_CWORLD_SEED;
		case 3: return MENU_AP_CWORLD_CREATE;
		case 4: return MENU_AP_CWORLD_DETAILS;
		default: return MENU_AP_CWORLD_CANCEL;
	}
}

/* What screen_sworld_focus_target() / screen_mps_focus_target() would report. */
static int mock_sworld_focus(const struct mock_engine* m) {
	return m->sworld_slot;
}

static int mock_mps_focus(const struct mock_engine* m) {
	return m->mps_slot;
}

static struct menu_ap_observation mock_observe(const struct mock_engine* m) {
	struct menu_ap_observation obs = {0};
	obs.cworld_mode_survival = m->cworld_mode == 0;
	obs.sworld_focus = mock_sworld_focus(m);
	obs.mps_focus = mock_mps_focus(m);
	obs.mps_view = m->mps_view;
	obs.mps_rows = m->server_rows;
	/* The real caller answers false when nothing registered the Multiplayer
	 * observers (they live in a link pool the engine core does not take --
	 * menu_autopilot.c's seam comment). The mock answers true except in the
	 * case built to cover that. */
	obs.mps_observed = !m->mps_unobserved;
	obs.sworld_rows = m->world_rows;
	/* screen_confirm_focus_is_safe(): only while the confirmation is up. */
	obs.confirm_focus_safe
		= m->screen == MENU_AP_SCREEN_CONFIRM && !m->confirm_unsafe;
	/* screen_cworld_editing() / screen_cworld_create_ready(): the modal, and
	 * "the ring is on an enabled Create with the modal closed" (Create is
	 * enabled throughout here: the name is never emptied). */
	obs.title_focus = m->screen == MENU_AP_SCREEN_TITLE ? m->title_downs
														  : MENU_AP_TITLE_NONE;
	obs.pause_focus = m->screen == MENU_AP_SCREEN_PAUSE ? m->pause_downs
														  : MENU_AP_PAUSE_NONE;
	obs.cworld_editing = m->cworld_modal;
	bool tp = m->screen == MENU_AP_SCREEN_TEXTUREPACKS;
	obs.texpacks_rows = m->texpacks_rows;
	obs.texpacks_selected = m->texpacks_sel;
	obs.texpacks_focused_row = m->texpacks_apply_focus || m->texpacks_other_button
		? -1 : m->texpacks_sel;
	obs.texpacks_apply_focused = m->texpacks_apply_focus;
	obs.texpacks_apply_enabled
		= m->texpacks_pending >= 0 && m->texpacks_pending != 0 && !m->texpacks_busy;
	obs.texpacks_pending_selected = m->texpacks_pending == m->texpacks_sel;
	obs.texpacks_busy = m->texpacks_busy;
	obs.texpacks_pick_row = tp ? mock_texpacks_pick_row(m) : -1;
	obs.cworld_focus = mock_cworld_focus(m);
	obs.cworld_details_open = m->cworld_details;
	obs.cworld_osk_profile = !m->cworld_modal ? -1
		: m->cworld_edit_row == 2 ? MENU_AP_OSK_PROFILE_CREATE_SEED
								  : MENU_AP_OSK_PROFILE_CREATE_NAME;
	obs.cworld_osk_target = m->cworld_modal ? m->cworld_osk_target : -1;
	obs.cworld_text_sig = m->cworld_committed + (unsigned)m->cworld_draft;
	obs.inv_page = m->screen == MENU_AP_SCREEN_INVENTORY ? m->inv_page : -1;
	obs.inv_tab = obs.inv_page == 1 ? m->inv_tab : -1;
	obs.cworld_create_ready = m->screen == MENU_AP_SCREEN_CREATE_WORLD
		&& !m->cworld_modal && m->cworld_focus == 3 && !m->cworld_block_create;
	return obs;
}

/* screen_set(&screen_select_world) runs the screen's reset(), which puts the
 * ring back on row 0 of the list. The Multiplayer sub-editors do NOT change
 * the screen, so that ring is PRESERVED across them -- the harder case, and
 * the real one. */
static void mock_sworld_reenter(struct mock_engine* m) {
	m->screen = MENU_AP_SCREEN_SELECT_WORLD;
	m->sworld_slot = m->world_rows ? MENU_AP_SWORLD_LIST
								 : MENU_AP_SWORLD_TAB_LOAD;
	m->sworld_options_open = false;
	if(m->world_rows > 0 && m->sworld_sel >= m->world_rows)
		m->sworld_sel = m->world_rows - 1;
}

/* Applies one frame's synthesized output to the mock screen graph, exactly
 * mirroring the real screens' documented behavior (see the file header). */
static void mock_apply(struct mock_engine* m, const struct menu_ap_output* out) {
	if((out->pointer_target == MENU_AP_PT_SWORLD_CREATE
		&& m->screen != MENU_AP_SCREEN_SELECT_WORLD)
	   || (out->pointer_target == MENU_AP_PT_TEXPACKS_ROW
		   && m->screen != MENU_AP_SCREEN_TEXTUREPACKS))
		m->pointer_target_off_sworld = true;

	if(out->aim_set) {
		m->aim_yaw = out->aim_yaw_deg;
		m->aim_pitch = out->aim_pitch_deg;
	}
	if(out->place_pulse && m->screen == MENU_AP_SCREEN_INGAME) {
		/* The pulse is an empty-hand right-click on whatever the held aim
		 * points at (menu_dwell_demo.h); the mock resolves the aim to the
		 * block the server placed there. */
		if(m->aim_yaw == MENU_DWELL_AIM_SIGN_YAW && m->aim_pitch == MENU_DWELL_AIM_SIGN_PITCH) {
			if(m->sign_autopilot_would_open)
				m->screen = MENU_AP_SCREEN_SIGN;
		} else if(m->containers_open) {
			if(m->aim_yaw == MENU_DWELL_AIM_FURNACE_YAW && m->aim_pitch == MENU_DWELL_AIM_FURNACE_PITCH) {
				m->screen = MENU_AP_SCREEN_FURNACE; m->opened_furnace++;
			} else if(m->aim_yaw == MENU_DWELL_AIM_CHEST_YAW && m->aim_pitch == MENU_DWELL_AIM_CHEST_PITCH) {
				m->screen = MENU_AP_SCREEN_CHEST; m->opened_chest++;
			} else if(m->aim_yaw == MENU_DWELL_AIM_LARGE_YAW && m->aim_pitch == MENU_DWELL_AIM_LARGE_PITCH) {
				m->screen = MENU_AP_SCREEN_CHEST; m->opened_chest++;
			} else if(m->aim_yaw == MENU_DWELL_AIM_DISPENSER_YAW && m->aim_pitch == MENU_DWELL_AIM_DISPENSER_PITCH) {
				m->screen = MENU_AP_SCREEN_DISPENSER; m->opened_dispenser++;
			}
		}
	}

	if(!out->press)
		return;

	if(out->button == IB_GUI_DROP || out->button == IB_GUI_QUICK_MOVE
	   || out->button == IB_GUI_SPLIT)
		m->forbidden_action_pressed = true;

	if(m->screen == MENU_AP_SCREEN_FURNACE || m->screen == MENU_AP_SCREEN_DISPENSER) {
		if(out->button == IB_INVENTORY)
			m->screen = MENU_AP_SCREEN_INGAME; /* container_menu_read_input .close */
		return;
	}
	if(m->screen == MENU_AP_SCREEN_CHEST) {
		if(out->button == IB_GUI_UP)
			m->chest_ups++;
		else if(out->button == IB_GUI_CLICK)
			m->chest_clicks++; /* a real chest would move a stack: forbidden */
		else if(out->button == IB_INVENTORY)
			m->screen = MENU_AP_SCREEN_INGAME;
		return;
	}

	if(m->screen == MENU_AP_SCREEN_TITLE) {
		if(out->button == IB_HOME)
			m->home_pressed_on_title = true; /* would call gstate.quit = true for real */
		/* Approved title (ui-build-1026 T03): 0 Play / 1 Tutorial world /
		 * 2 Texture packs / 3 Options / 4 Quit, clamped (screen_title.c). */
		if(out->button == IB_GUI_DOWN && m->title_downs < MENU_AP_TITLE_QUIT)
			m->title_downs++;
		if(out->button == IB_GUI_UP && m->title_downs > 0)
			m->title_downs--;
		if(out->button == IB_GUI_CLICK) {
			if(m->title_downs == MENU_AP_TITLE_PACKS) {
				m->screen = MENU_AP_SCREEN_TEXTUREPACKS;
				m->texpacks_seen = true;
			} else if(m->title_downs == MENU_AP_TITLE_PLAY)
				m->screen = MENU_AP_SCREEN_SELECT_WORLD;
			else if(m->title_downs == MENU_AP_TITLE_TUTORIAL) {
				m->tutorial_clicks++;
				m->screen = MENU_AP_SCREEN_OTHER; /* would load the shipped world */
			} else
				m->screen = MENU_AP_SCREEN_OTHER; /* Tutorial / Options / Quit: never wanted */
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_TEXTUREPACKS) {
		/* screen_texturepacks.c:699 -- the Close role joined IB_HOME here
		 * under Astra Lane A item 1; both run texpacks_done() -> title. */
		if(out->button == IB_HOME || out->button == IB_INVENTORY) {
			if(out->button == IB_INVENTORY)
				m->texpacks_closes++;
			m->screen = MENU_AP_SCREEN_TITLE;
			m->title_downs = 0; /* screen_title_reset() re-defaults selection */
			/* Done/Home discards the pending choice; the next entry resets. */
			m->texpacks_sel = 0;
			m->texpacks_pending = -1;
			m->texpacks_apply_focus = false;
		} else if(out->button == IB_GUI_CLICK
				  && out->pointer_target == MENU_AP_PT_TEXPACKS_ROW) {
			/* T01/T06: ONE click on a row SELECTS it, sets the pending
			 * choice, and puts the ring on Apply when Apply is enabled. */
			m->texpacks_row_clicks++;
			m->texpacks_sel = out->pointer_row;
			m->texpacks_pending = out->pointer_row;
			m->texpacks_apply_focus = out->pointer_row != 0;
			if(m->texpacks_click_applies)
				m->texpacks_busy = true;
		} else if(out->button == IB_GUI_CLICK)
			m->pack_applies++; /* deferred apply + reload; the screen stays */
		else if(out->button == IB_GUI_UP || out->button == IB_GUI_DOWN) {
			m->texpacks_apply_focus = false; /* UP/DOWN take the ring back */
			/* ...and move the SELECTION only (pending is untouched). */
			if(out->button == IB_GUI_UP && m->texpacks_sel > 0)
				m->texpacks_sel--;
			if(out->button == IB_GUI_DOWN
			   && m->texpacks_sel + 1 < m->texpacks_rows)
				m->texpacks_sel++;
			if(m->texpacks_sel > m->texpacks_max_sel)
				m->texpacks_max_sel = m->texpacks_sel;
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_RENAME_WORLD) {
		if(out->button == IB_INVENTORY && m->rename_eats_row
		   && m->world_rows > 0)
			m->world_rows--;
		/* screen_rename_world.c:229 -- Close backs out to Select World
		 * WITHOUT renaming. (On PC the same line is gated behind
		 * !input_text_active(), which is why the walker does not enter here
		 * at all when text_input_owns_close is set.) */
		if(out->button == IB_INVENTORY)
			mock_sworld_reenter(m);
		return;
	}
	if(m->screen == MENU_AP_SCREEN_CONFIRM) {
		/* screen_confirm.c:196 -- the back-out answers NO unconditionally,
		 * whatever the highlight is, and sworld_delete_confirmed(false)
		 * returns to Select World having deleted nothing. An ACTIVATE here
		 * would fire `selected`, which the modal set to the safe answer on
		 * entry -- but nothing production-visible can observe that, so the
		 * walker deliberately does not press it (see the .c). */
		if(out->button == IB_INVENTORY) {
			/* screen_confirm.c:196 answers NO -- unless this case is
			 * simulating the leak the count check exists to catch. */
			if(m->delete_eats_row && m->world_rows > 0)
				m->world_rows--;
			mock_sworld_reenter(m);
		} else if(out->button == IB_GUI_CLICK)
			m->world_deleted = true; /* what the walker must never risk */
		return;
	}
	if(m->screen == MENU_AP_SCREEN_MULTIPLAYER) {
		/* A sub-editor is up: only the Close leaves it, and it saves nothing
		 * (screen_multiplayer_setup.c:1855/:1983/:2122 -> mps_return_to_main).
		 * The screen pointer never changed, so the MAIN ring is preserved. */
		if(m->mps_view != MENU_AP_MPS_VIEW_MAIN) {
			if(out->button == IB_INVENTORY) {
				m->mps_view = MENU_AP_MPS_VIEW_MAIN;
				if(m->mps_slot == MENU_AP_MPS_EDIT)
					m->mps_options_open = true;
			}
			else if(out->button == IB_GUI_CLICK)
				m->sub_editor_saved = true; /* the ring opens on Cancel, but the leg presses nothing here */
			return;
		}
		if(out->button == IB_INVENTORY) {
			if(m->mps_options_open) {
				m->mps_options_open = false;
				m->mps_slot = MENU_AP_MPS_OPTIONS;
				return;
			}
			m->mp_closes++;
			if(!m->mp_close_blocked) {
				m->screen = MENU_AP_SCREEN_TITLE;
				m->title_downs = 0;
			}
			return;
		}
		if(out->button == IB_GUI_UP || out->button == IB_GUI_DOWN
		   || out->button == IB_GUI_LEFT || out->button == IB_GUI_RIGHT) {
			bool up = out->button == IB_GUI_UP, down = out->button == IB_GUI_DOWN;
			if(m->mps_options_open) {
				static const int modal[] = {MENU_AP_MPS_EDIT, MENU_AP_MPS_DELETE,
					MENU_AP_MPS_REFRESH, MENU_AP_MPS_BACK};
				if(!m->server_rows) {
					m->mps_slot = m->mps_slot == MENU_AP_MPS_REFRESH
						? MENU_AP_MPS_BACK : MENU_AP_MPS_REFRESH;
					return;
				}
				int at = 0;
				while(at < 4 && modal[at] != m->mps_slot) at++;
				m->mps_slot = modal[(at + 1) % 4];
			} else if(m->mps_slot == MENU_AP_MPS_LIST) {
				int next = m->mps_sel;
				if(up && next > 0)
					next--;
				if(down && next + 1 < m->server_rows)
					next++;
				if(next != m->mps_sel)
					m->mps_sel = next;
				else if(down) m->mps_slot = MENU_AP_MPS_JOIN;
				else if(up) m->mps_slot = MENU_AP_MPS_TAB_JOIN;
				if(out->button == IB_GUI_RIGHT && !m->mps_swallow_list_right)
					m->mps_slot = MENU_AP_MPS_JOIN;
			} else if(m->mps_slot == MENU_AP_MPS_TAB_JOIN) {
				if(up) m->mps_slot = MENU_AP_MPS_NAME;
				else if(down) m->mps_slot = m->server_rows ? MENU_AP_MPS_LIST : MENU_AP_MPS_ADD;
			} else if(m->mps_slot == MENU_AP_MPS_NAME && down)
				m->mps_slot = MENU_AP_MPS_TAB_JOIN;
			else if(up && m->mps_slot >= MENU_AP_MPS_JOIN
				&& m->mps_slot <= MENU_AP_MPS_OPTIONS)
				m->mps_slot = m->server_rows ? MENU_AP_MPS_LIST
										 : MENU_AP_MPS_TAB_JOIN;
			else if(m->mps_slot == MENU_AP_MPS_JOIN)
				m->mps_slot = out->button == IB_GUI_LEFT ? MENU_AP_MPS_DIRECT
					: m->mps_swallow_join_right ? MENU_AP_MPS_JOIN
					: MENU_AP_MPS_OPTIONS;
			else if(m->mps_slot == MENU_AP_MPS_DIRECT)
				m->mps_slot = out->button == IB_GUI_LEFT ? MENU_AP_MPS_ADD : MENU_AP_MPS_JOIN;
			else if(m->mps_slot == MENU_AP_MPS_ADD && out->button == IB_GUI_RIGHT)
				m->mps_slot = MENU_AP_MPS_DIRECT;
			return;
		}
		if(out->button == IB_GUI_CLICK) {
			int focus = mock_mps_focus(m);
			if(focus == MENU_AP_MPS_LIST || focus == MENU_AP_MPS_JOIN
			   || focus == MENU_AP_MPS_QUICK)
				m->mp_row_activated = true; /* would JOIN a server */
			else if(focus == MENU_AP_MPS_NAME)
				m->mps_view = MENU_AP_MPS_VIEW_NAME;
			else if(focus == MENU_AP_MPS_DIRECT)
				m->mps_view = MENU_AP_MPS_VIEW_DIRECT;
			else if(focus == MENU_AP_MPS_ADD)
				m->mps_view = MENU_AP_MPS_VIEW_ADD;
			else if(focus == MENU_AP_MPS_OPTIONS) {
				m->mps_options_open = true;
				m->mps_slot = m->server_rows ? MENU_AP_MPS_EDIT : MENU_AP_MPS_REFRESH;
			} else if(focus == MENU_AP_MPS_EDIT && m->mps_options_open) {
				m->mps_options_open = false;
				m->mps_view = MENU_AP_MPS_VIEW_EDIT;
			}
			else if(focus == MENU_AP_MPS_DELETE) {
				m->screen = MENU_AP_SCREEN_CONFIRM; /* the walker never aims here */
				m->confirm_opens++;
			} else if(focus == MENU_AP_MPS_BACK) {
				m->screen = MENU_AP_SCREEN_TITLE;
				m->title_downs = 0;
			}
			if(m->mps_view != MENU_AP_MPS_VIEW_MAIN)
				m->view_opens[m->mps_view]++;
			return;
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_SELECT_WORLD) {
		if(out->button == IB_GUI_CLICK) {
			if(out->pointer_target == MENU_AP_PT_SWORLD_CREATE) {
				if(!m->block_sworld_create)
					m->screen = MENU_AP_SCREEN_CREATE_WORLD;
				return;
			}
			int focus = mock_sworld_focus(m);
			if(focus == MENU_AP_SWORLD_LIST || focus == MENU_AP_SWORLD_PLAY)
				m->sworld_row_activated = true; /* would LOAD a world */
			else if(focus == MENU_AP_SWORLD_TAB_CREATE) {
				m->screen = MENU_AP_SCREEN_CREATE_WORLD;
				m->cworld_opens++;
			} else if(focus == MENU_AP_SWORLD_OPTIONS) {
				m->sworld_options_open = true;
				m->sworld_slot = MENU_AP_SWORLD_MODAL_RENAME;
			} else if(focus == MENU_AP_SWORLD_MODAL_RENAME) {
				m->sworld_options_open = false;
				m->screen = MENU_AP_SCREEN_RENAME_WORLD;
				m->rename_opens++;
			} else if(focus == MENU_AP_SWORLD_MODAL_DELETE) {
				m->sworld_options_open = false;
				m->screen = MENU_AP_SCREEN_CONFIRM;
				m->confirm_opens++;
			} else if(focus == MENU_AP_SWORLD_TAB_JOIN) {
				m->screen = MENU_AP_SCREEN_MULTIPLAYER;
				m->mps_slot = MENU_AP_MPS_TAB_JOIN;
				m->mp_visits++;
			} else if(focus == MENU_AP_SWORLD_CANCEL) {
				m->screen = MENU_AP_SCREEN_TITLE;
				m->title_downs = 0;
			}
			return;
		}
		if(out->button == IB_GUI_UP || out->button == IB_GUI_DOWN
		   || out->button == IB_GUI_LEFT || out->button == IB_GUI_RIGHT) {
			bool up = out->button == IB_GUI_UP, down = out->button == IB_GUI_DOWN;
			/* The screen stops accepting the ways OUT of the list (see the
			 * field's comment): the press is consumed and the ring does not
			 * move, which is what a converge walk running out of budget looks
			 * like from the walker's side whatever the underlying cause. */
			bool stuck_in_list = m->sworld_swallow_downs_after_cworld
				&& m->cworld_opens > 0 && m->sworld_slot == MENU_AP_SWORLD_LIST;
			if(stuck_in_list
			   && (down || out->button == IB_GUI_RIGHT))
				return;
			if(m->sworld_options_open) {
				static const int modal[] = {MENU_AP_SWORLD_MODAL_RENAME,
					MENU_AP_SWORLD_MODAL_DELETE, MENU_AP_SWORLD_MODAL_BACK};
				int at = 0;
				while(at < 3 && modal[at] != m->sworld_slot) at++;
				m->sworld_slot = modal[(at + 1) % 3];
			} else if(m->sworld_slot == MENU_AP_SWORLD_LIST) {
				int next = m->sworld_sel;
				if(up && next > 0)
					next--;
				if(down && next + 1 < m->world_rows)
					next++;
				if(next != m->sworld_sel)
					m->sworld_sel = next;
				else if(down) m->sworld_slot = MENU_AP_SWORLD_PLAY;
				else if(up) m->sworld_slot = MENU_AP_SWORLD_TAB_LOAD;
				if(out->button == IB_GUI_RIGHT && !m->sworld_swallow_list_right)
					m->sworld_slot = MENU_AP_SWORLD_PLAY;
			} else if(m->sworld_slot >= MENU_AP_SWORLD_TAB_LOAD
				  && m->sworld_slot <= MENU_AP_SWORLD_TAB_JOIN
				  && out->button == IB_GUI_RIGHT)
				m->sworld_slot = m->sworld_slot == MENU_AP_SWORLD_TAB_JOIN
					? MENU_AP_SWORLD_TAB_LOAD : m->sworld_slot + 1;
			else if(m->sworld_slot == MENU_AP_SWORLD_PLAY) {
				if(out->button == IB_GUI_RIGHT) m->sworld_slot = MENU_AP_SWORLD_OPTIONS;
				else if(down) m->sworld_slot = MENU_AP_SWORLD_TAB_LOAD;
			} else if(m->sworld_slot == MENU_AP_SWORLD_OPTIONS && down)
				m->sworld_slot = MENU_AP_SWORLD_TAB_LOAD;
			return;
		}
		if(out->button == IB_INVENTORY || out->button == IB_HOME) {
			if(m->sworld_options_open) {
				m->sworld_options_open = false;
				m->sworld_slot = MENU_AP_SWORLD_OPTIONS;
				return;
			}
			/* screen_select_world.c:1342 -- Close joined IB_HOME on the
			 * Cancel id, so both back out to the title. */
			if(out->button == IB_INVENTORY)
				m->sworld_closes++;
			m->screen = MENU_AP_SCREEN_TITLE;
			m->title_downs = 0;
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_CREATE_WORLD) {
		if(m->cworld_modal) {
			/* T05a frontend editor: it opens on its first key cell; A types
			 * that cell's character into the field (the draft); Close is
			 * OSK_EV_CANCEL, which restores the pre-edit text. Directions
			 * move the key focus (counted, never expected from the walker). */
			if(out->button == IB_GUI_DOWN)
				m->cworld_down_seen++;
			if(out->button == IB_GUI_RIGHT)
				m->cworld_right_seen++;
			if(out->button == IB_GUI_CLICK && m->cworld_osk_target >= MENU_AP_OSK_CHAR_FIRST) {
				m->cworld_draft++;
				m->cworld_keys_typed++;
			}
			if(out->button == IB_INVENTORY) {
				if(m->cworld_cancel_commits)
					m->cworld_committed += (unsigned)m->cworld_draft;
				m->cworld_draft = 0;
				m->cworld_modal = false; /* focus returns to the edited row */
			}
			return;
		}
		/* The Details card: its one control is Back; Close closes the card
		 * and CONSUMES the frame (screen_create_world.c). */
		if(m->cworld_details) {
			if(out->button == IB_INVENTORY || out->button == IB_GUI_CLICK)
				m->cworld_details = false;
			return;
		}
		/* screen_create_world.c:472 -- with no modal up, Close leaves the
		 * form for Select World, creating nothing. This is the CREATE
		 * sub-leg's exit (A7 part 2); the older create leg never presses it. */
		if(out->button == IB_INVENTORY) {
			mock_sworld_reenter(m);
			return;
		}
		/* The form's focus ring (cworld_form_model.c): wraps over five rows;
		 * Create is enabled throughout because the name is never emptied. */
		if(out->button == IB_GUI_DOWN && m->cworld_open_on_down) {
			m->cworld_modal = true;
			m->cworld_edit_row = 0;
			m->cworld_osk_target = MENU_AP_OSK_CHAR_FIRST;
			return;
		}
		/* ui-build-1026 T05: the approved screen walks DOWN in Beta's field
		 * order -- Name(0) -> Seed(2) -> Game mode(1) -> Create(3) -> Name,
		 * and Details/Cancel(4) sit to the RIGHT of Create. RIGHT on the seed
		 * row only moves to New seed (inert here), so it changes nothing. */
		static const int cw_down[6] = {2, 3, 1, 0, 0, 0};
		static const int cw_up[6] = {3, 2, 0, 1, 1, 1};
		if(out->button == IB_GUI_DOWN && !m->cworld_swallow_down) {
			m->cworld_focus = cw_down[m->cworld_focus];
			m->cworld_seed_new = false;
		}
		if(out->button == IB_GUI_UP) {
			m->cworld_focus = cw_up[m->cworld_focus];
			m->cworld_seed_new = false;
		}
		/* Create world | Details | Cancel, no wrap; New seed RIGHT of Seed. */
		if(out->button == IB_GUI_RIGHT && m->cworld_focus >= 3
		   && m->cworld_focus < 5)
			m->cworld_focus++;
		else if(out->button == IB_GUI_LEFT && m->cworld_focus > 3)
			m->cworld_focus--;
		if(m->cworld_focus == 2 && (out->button == IB_GUI_RIGHT
									|| out->button == IB_GUI_LEFT))
			m->cworld_seed_new = out->button == IB_GUI_RIGHT;
		/* Mode-select skew/failure simulation (2026-09-02 hardening tests):
		 *   cworld_mode_skew: a LEFT press's effect on cworld_mode lands ONE
		 *     TICK LATE instead of immediately -- the exact shape of the bug
		 *     the adversarial review named (a rising edge synthesized on
		 *     frame N only visible to the widget on frame N+1). Applied
		 *     BEFORE this tick's own button so a press issued last tick
		 *     lands now, then this tick's press (if any) queues for the NEXT
		 *     tick.
		 *   block_mode_toggle: LEFT never takes effect at all -- a real
		 *     regression, not a timing fluke. Scoped to LEFT only (RIGHT
		 *     stays live) so the initial STEP_CWORLD_MODE_CREATIVE press can
		 *     still reach Creative for the capture dwell; it is the RETURN
		 *     to Survival that never confirms, which is what the script
		 *     must catch and fail on. */
		if(m->cworld_left_pending) {
			m->cworld_left_pending = false;
			if(m->cworld_focus == 1) {
				m->cworld_mode ^= 1;
				if(m->cworld_mode)
					m->cworld_saw_creative = true;
			}
		}
		if(out->button == IB_GUI_LEFT && m->cworld_focus == 1) {
			if(m->block_mode_toggle) {
				/* swallowed permanently -- LEFT never registers */
			} else if(m->cworld_mode_skew) {
				m->cworld_left_pending = true; /* swallowed THIS tick only */
			} else {
				m->cworld_mode ^= 1;
				if(m->cworld_mode)
					m->cworld_saw_creative = true;
			}
		} else if(out->button == IB_GUI_RIGHT && m->cworld_focus == 1) {
			m->cworld_mode ^= 1;
			if(m->cworld_mode)
				m->cworld_saw_creative = true;
		}
		if(out->button == IB_GUI_CLICK) {
			if(m->cworld_focus == 0 || (m->cworld_focus == 2 && !m->cworld_seed_new)) {
				m->cworld_modal = true; /* A on a text row opens the editor */
				m->cworld_edit_row = m->cworld_focus;
				m->cworld_osk_target = MENU_AP_OSK_CHAR_FIRST;
				m->cworld_osk_opens++;
			} else if(m->cworld_focus == 4) {
				if(!m->cworld_details_never_opens) {
					m->cworld_details = true;
					m->cworld_details_opens++;
				}
			} else if(m->cworld_focus == 5)
				mock_sworld_reenter(m); /* Cancel: the walker never aims here */
			else if(m->cworld_focus == 1)
				m->cworld_mode ^= 1;
			else if(m->cworld_focus == 3) {
				m->cworld_created_creative = m->cworld_mode == 1;
				m->screen = MENU_AP_SCREEN_INGAME;
			}
			/* focus 4 = Cancel would leave; the script never presses it. */
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_INGAME) {
		if(out->button == IB_HOME)
			m->screen = MENU_AP_SCREEN_PAUSE;
		/* b1-chat-opener-0919/07 C: screen_ingame.c reads the GAMEPLAY OPEN
		 * role here (input_pressed(IB_OPEN_INVENTORY)), not the menu Close
		 * role -- and menu_autopilot_force_button forces the exact button the
		 * script names, so no binding-level fallback stands between the two.
		 * A plain IB_INVENTORY in-game is a Close with nothing to close: it
		 * must do NOTHING, which is what test_container_legs_soft_skip's
		 * "presses nothing in-game" assertion rests on. */
		else if(out->button == IB_OPEN_INVENTORY) {
			m->screen = MENU_AP_SCREEN_INVENTORY; /* screen_ingame.c: OPEN_INVENTORY -> screen_inventory */
			m->inv_page = 0; /* the inventory opens on its Inventory page */
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_INVENTORY) {
		/* T12 pages: a PAGE_SWAP tap swaps; PAGE moves the Crafting tab; an A
		 * on the Crafting page would Craft. */
		if(out->button == IB_GUI_PAGE_SWAP) {
			if(!m->inv_swap_dead) {
				m->inv_page ^= 1;
				m->inv_swaps++;
			}
			return;
		}
		if(m->inv_page == 1) {
			if(out->button == IB_GUI_PAGE) {
				m->inv_tab = (m->inv_tab + 1) % MENU_AP_CRAFT_TABS;
				m->inv_tab_moves++;
			} else if(out->button == IB_GUI_CLICK)
				m->craft_clicks++;
			else if(out->button == IB_INVENTORY) {
				m->inv_page = 0;
				m->screen = MENU_AP_SCREEN_INGAME;
			}
			return;
		}
		if(out->button == IB_INVENTORY)
			m->screen = MENU_AP_SCREEN_INGAME; /* container_menu_read_input .close */
		else if(out->button == IB_GUI_RIGHT)
			m->inventory_rights++; /* slot focus moves; the portrait follows it */
		else if(out->button == IB_GUI_CLICK) {
			/* container_menu.c: A on a focused slot PICKS UP that slot's
			 * stack onto the cursor, and A again on the SAME slot puts it
			 * back. The mock records which focus index each click landed on,
			 * so "the second click is on the same slot" is checkable. */
			assert(m->inventory_clicks
				   < (int)(sizeof(m->inventory_click_focus)
						   / sizeof(m->inventory_click_focus[0])));
			m->inventory_click_focus[m->inventory_clicks++]
				= m->inventory_rights;
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_SIGN) {
		if(out->button == IB_INVENTORY)
			m->screen = MENU_AP_SCREEN_INGAME;
		return;
	}
	if(m->screen == MENU_AP_SCREEN_PAUSE) {
		if(out->button == IB_GUI_DOWN) {
			if(m->pause_downs < 4 && !m->pause_swallow_down)
				m->pause_downs++;
			return;
		}
		if(out->button == IB_GUI_UP) {
			if(m->pause_downs > 0)
				m->pause_downs--;
			return;
		}
		if(out->button == IB_GUI_CLICK) {
			if(m->pause_downs == 1) {
				/* The Achievements board: the screen the stale one-DOWN
				 * walker opened on hw1026b. Classified OTHER. */
				m->achievements_opened++;
				m->screen = MENU_AP_SCREEN_OTHER;
			} else if(m->pause_downs == 0 || m->pause_downs == 3) {
				m->screen = m->pause_downs == 0 ? MENU_AP_SCREEN_INGAME
												 : MENU_AP_SCREEN_OTHER;
			} else if(m->pause_downs == 4) {
				/* Save and quit: the pause waits for the server; a refused
				 * unload keeps it up (quit_fails), an accepted one settles
				 * on the title. */
				if(!m->quit_fails) {
					m->screen = MENU_AP_SCREEN_TITLE;
					m->title_downs = 0;
					m->quit_done++;
				}
			} else
				m->screen = MENU_AP_SCREEN_OPTIONS;
			m->pause_downs = 0;
		} else if(out->button == IB_HOME) {
			m->screen = MENU_AP_SCREEN_INGAME; /* resume */
			m->pause_downs = 0;
		}
		return;
	}
	if(m->screen == MENU_AP_SCREEN_OPTIONS) {
		if(out->button == IB_HOME)
			m->screen = MENU_AP_SCREEN_PAUSE; /* opened-from-pause return */
		return;
	}
}

/* Drives the script against the mock for up to `max_ticks`, applying the
 * script's output to the mock every tick and -- once the mock reaches
 * screen_ingame post-create AND `sign_autopilot_would_open` -- flipping the
 * mock to screen_sign exactly once, standing in for the EXISTING
 * debug.autopilot place-pulse mechanism this profile relies on (out of this
 * module's scope; see the header). Returns the tick count actually used. */
static int drive(struct menu_ap_state* s, struct mock_engine* m,
				  struct mock_click_log* log, int max_ticks) {
	int t;
	for(t = 0; t < max_ticks; t++) {
		struct menu_ap_output out;
		/* The caller's live-model observation, read BEFORE this tick's
		 * script call so it reflects only effects already applied through
		 * the PREVIOUS tick -- exactly what a real caller reading
		 * screen_cworld_mode_is_survival() at the top of a frame would see. */
		if(m->tutorial_drift && m->screen == MENU_AP_SCREEN_TITLE
		   && m->title_downs == MENU_AP_TITLE_TUTORIAL
		   && ++m->tutorial_rest_ticks >= m->tutorial_drift)
			m->title_downs = MENU_AP_TITLE_OPTIONS; /* something else moved the ring */
		if(m->texpacks_focus_drift && m->screen == MENU_AP_SCREEN_TEXTUREPACKS
		   && m->texpacks_apply_focus
		   && ++m->texpacks_drift_ticks >= m->texpacks_focus_drift) {
			m->texpacks_apply_focus = false; /* the ring moved to Done */
			m->texpacks_other_button = true;
		}
		if(m->screen == MENU_AP_SCREEN_TEXTUREPACKS && m->texpacks_rows > 0
		   && m->texpacks_sel == m->texpacks_rows - 1)
			m->texpacks_last_row_ticks++;
		struct menu_ap_observation obs = mock_observe(m);
		int step_before = s->step; /* the step this tick RAN on, not the next */
		bool more = menu_ap_script_tick(s, m->screen, obs, TICK_DT, &out);
		log_push(log, &out, step_before);
		mock_apply(m, &out);

		if(!more)
			break;
	}
	return t;
}

static void test_full_happy_path_with_sign(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_enable_pack_legs(&s, true); /* capture-run opt-in: three pack applies */
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);

	/* Generous tick budget: every dwell/wait constant in the .c is well
	 * under a few minutes of scripted (not wall-clock) ticks at 20Hz. */
	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600); /* must finish before the budget -- not still pending */
	assert(menu_ap_script_finished(&s));
	if(s.max_step < menu_ap_script_step_required_for_pass())
		fprintf(stderr, "happy path stopped: step=%d max=%d failed=%d sub=%d "
						"screen=%d focus=%d mps=%d parent=%u\n",
				s.step, s.max_step, s.np_failed_step, s.np_failed_sub,
				(int)m.screen, mock_sworld_focus(&m), mock_mps_focus(&m),
				(unsigned)s.np_parent_stage);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(s.sign_reached);
	assert(m.screen == MENU_AP_SCREEN_TITLE); /* resumed from pause, then Save and quit settled */
	assert(!m.home_pressed_on_title);
	assert(!m.pointer_target_off_sworld);

	/* EXACT press counts for the whole run (CODESTYLE 4.3: exact tick
	 * counts, not windows), traced against the script's own step table:
	 *   DOWN=6    (title-nav 1 + texpacks-nav 1 + cworld-nav-row 3 + pause-nav 1) + 1 pack row + 3 quit-nav (2026-09-11)
	 *   RIGHT=8   (cworld-nav-col 5 + sign-nav 2 + options-slider 1) + 5 inventory focus (2026-09-11)
	 *   CLICK=6   (title x2 + sworld-create + cworld-type + cworld-submit + pause-options) + 3 pack applies + 1 Save and quit
	 *   UP=1      (sign-edit-char)  + 4 pack rows + 1 (the 7 chest Sort-nav UPs left with Sort, 2026-10-25)
	 *   (menus-dwell-walker-1026, 2026-10-26: the old name-editor leg -- UP
	 *   to Name, open, type, DOWN x3 + RIGHT x5 to the old OSK's OK, OK, and
	 *   a blind DOWN x3 to Create -- is retired; the observed walk from Game
	 *   Mode presses DOWN x2 to Create. Net vs the totals asserted before
	 *   that: DOWN 14 -> 10, RIGHT 14 -> 9, CLICK 14 -> 11, UP 6 -> 5.
	 *   Then the pause walks: Options is 2 rows down and Save and quit 4
	 *   since the Achievements row (91bd8295c), DOWN 1 + 3 -> 2 + 4: DOWN
	 *   10 -> 12. ui-build-1026 T03: Tutorial world sits between Play and
	 *   Texture packs, so the title-nav walk to Packs is DOWN x2: 12 -> 13.
	 *   ui-build-1026 T05: Beta field order puts Seed above Game mode; the
	 *   observed mode probe presses DOWN, RIGHT, DOWN, RIGHT (Creative) and
	 *   the walk to Create is then DOWN x1: DOWN 13 -> 13, RIGHT 9 -> 10.
	 *   ui-build-1026 T15: the Texture packs one-click pick (+1 CLICK, with
	 *   the pointer on the row card), then the Create sub-legs from Game
	 *   mode: Details = DOWN, RIGHT, CLICK, Close; name OSK = UP x3, CLICK
	 *   open, CLICK one key, Close (Cancel); seed keypad = DOWN, CLICK open,
	 *   CLICK one key, Close; the walk to Create then starts on Seed, DOWN
	 *   x2 (was x1). DOWN 13 -> 16, RIGHT 10 -> 11, UP 5 -> 8, CLICK 11 ->
	 *   17, INVENTORY 6 -> 9.)
	 *   HOME=4    (texpacks-leave + pause-open + options-leave + pause-resume) + 1 quit-open
	 *   INVENTORY=2 (sign-leave + inventory-leave; the inventory OPEN moved to
	 *             IB_OPEN_INVENTORY, counted on its own below)
	 *             + 4 container legs (furnace, chest, double chest, dispenser)
	 *   OPEN_INVENTORY=1 (STEP_INVENTORY_OPEN, the only gameplay open)
		 * A script that presses DOWN/RIGHT a different number of times than the
	 * real OSK widget needs would either undershoot (mock never reaches
	 * Done, the drive() loop times out, caught above) or overshoot silently
	 * -- this pins the count so a silent overshoot is caught too. */
	assert(!m.title_landed_on_multiplayer);
	/* Counted OUTSIDE the A7 no-pointer leg -- see
	 * log_count_button_outside_leg -- so these stay the exact, hand-traced
	 * numbers they have always been. */
	assert(log_count_button_outside_leg(&log, IB_GUI_DOWN) == 16);
	assert(log_count_button_outside_leg(&log, IB_GUI_RIGHT) == 11);
	assert(m.inventory_rights == 5);
	/* + 2 inventory hold-leg clicks (pick up, put back -- G-0b) */
	assert(log_count_button_outside_leg(&log, IB_GUI_CLICK) == 17);
	assert(log_count_button_outside_leg(&log, IB_GUI_LEFT) == 1);
	assert(log_count_button_outside_leg(&log, IB_GUI_UP) == 8);
	assert(log_count_button_outside_leg(&log, IB_HOME) == 5);
	/* Pack legs: three applies (default, installed, default); quit leg: the
	 * title came back and the walker says so. */
	assert(m.pack_applies == 3 && s.pack_applies == 3);
	assert(m.quit_done == 1 && s.quit_reached && !s.quit_failed);
	assert(m.screen == MENU_AP_SCREEN_TITLE);
	/* Outside the leg, as above: the six original Closes (the chest Sort
	 * close went with Sort, 2026-10-25). The leg's own
	 * Closes are held to their own contract in the part-2 cases. */
	assert(log_count_button_outside_leg(&log, IB_INVENTORY) == 9);
	/* The role split is pinned from BOTH sides: exactly one gameplay open on
	 * the new role, and the six Closes above still on the old one. */
	assert(log_count_button_outside_leg(&log, IB_OPEN_INVENTORY) == 1);
	/* Container legs: every demo screen opened exactly once from its own
	 * aimed pulse, and the latches say so. The chest legs only look and
	 * close: no UP, no A, so no stack can move (Sort parked 2026-10-25). */
	assert(m.opened_furnace == 1 && m.opened_chest == 2 && m.opened_dispenser == 1);
	assert(m.chest_ups == 0 && m.chest_clicks == 0);
	assert(s.furnace_reached && s.chest_reached && s.large_chest_reached
		   && s.dispenser_reached);
	/* The walker never opens the name editor any more (its frontend OSK
	 * reads only the owner frame, which synthetic presses never reach --
	 * menu_autopilot_script.h), so nothing was pressed inside a modal. */
	assert(m.cworld_down_seen == 0);
	assert(m.cworld_right_seen == 0);
	assert(m.achievements_opened == 0); /* hw1026b: the stale DOWN x1 opened it */
	assert(s.map_fail == 0 && !s.stalled); /* a completed run names no failure */
	/* The console screen's own contract: no modal was left open, the
	 * selector reached Creative for the capture dwell, and the world was
	 * nonetheless CREATED in survival so every in-game step below is
	 * unchanged. */
	assert(!m.cworld_modal);
	assert(m.cworld_saw_creative);
	assert(m.cworld_mode == 0);
	assert(!m.cworld_created_creative);

	printf("[test_full_happy_path_with_sign] PASS (ticks=%d, max_step=%d/%d)\n",
		   used, s.max_step, menu_ap_script_step_count() - 1);
}

/* ---- the inventory HOLD leg (b1-chat-opener-0919/16 G-0b) ----------------
 * The Drop prompt only joins the Inventory footer while the cursor HOLDS a
 * stack (Astra menu review 2026-09-19 MUST item 4), so every capture of that
 * footer so far photographed the EMPTY row -- the one state that cannot show
 * what the card claims. The walker now picks a stack up, HOLDS it for a full
 * dwell (so a 3 s capture period lands inside the held state) and puts it
 * straight back on the SAME slot.
 *
 * What is pinned here, and why each part matters:
 *   - exactly TWO clicks, both inside the inventory leg, in that order;
 *   - the second lands on the same focus index as the first (put-back, not a
 *     move to somewhere else) -- the script stays PURE, so this is expressed
 *     as "no focus press between them" rather than as an observation;
 *   - the gap between them IS the dwell, measured in ticks;
 *   - no Drop / Quick Move / Split is ever synthesized, on any screen: the
 *     leg must not discard, halve or transfer the maintainer's items;
 *   - the steps after it are untouched (leave, containers, pause, quit). */
static void test_inventory_hold_leg(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));

	/* The leg's bounds: the single gameplay OPEN, and the close that ends it. */
	size_t open = log_find(&log, IB_OPEN_INVENTORY, 0);
	assert(open < log.count);
	size_t leave = log_find(&log, IB_INVENTORY, open);
	assert(leave < log.count);
	/* The five focus moves come FIRST, then the two clicks -- step order. */
	size_t right5 = open;
	for(int k = 0; k < 5; k++) {
		right5 = log_find(&log, IB_GUI_RIGHT, right5 + 1);
		assert(right5 < leave);
	}
	size_t pick = log_find(&log, IB_GUI_CLICK, open);
	assert(pick > right5 && pick < leave);
	size_t place = log_find(&log, IB_GUI_CLICK, pick + 1);
	assert(place < leave);
	assert(log_find(&log, IB_GUI_CLICK, place + 1) >= leave
		   && "a third click inside the inventory leg");

	/* THE DWELL. One index = one tick (log_find's note), so the gap between
	 * the two clicks is the held-state dwell in ticks. DWELL_MENU is 3.0 s
	 * and TICK_DT is 1/20 s: 60 ticks nominally, 61 as the float accumulator
	 * actually crosses 3.0 (60 additions of 0.05f land at 2.99999...), plus
	 * the one tick the transition itself costs. That is the SAME dwell the
	 * two neighbouring inventory steps use, and it is long enough that a
	 * full 3 s console capture period cannot fall outside the held state. */
	size_t gap = place - pick;
	assert(gap >= 61 && gap <= 62);
	assert((float)gap * TICK_DT >= 3.0f);

	/* Both clicks on the SAME slot: no focus press moved between them. */
	assert(m.inventory_clicks == 2);
	assert(m.inventory_click_focus[0] == m.inventory_click_focus[1]);
	assert(m.inventory_click_focus[0] == 5); /* the focus the leg already walked to */

	/* Never a discard, a half-stack or a transfer -- on any screen. */
	assert(!m.forbidden_action_pressed);
	assert(log_count_button(&log, IB_GUI_DROP) == 0);
	assert(log_count_button(&log, IB_GUI_QUICK_MOVE) == 0);
	assert(log_count_button(&log, IB_GUI_SPLIT) == 0);

	/* The later steps are unchanged by the insert. */
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(s.furnace_reached && s.chest_reached && s.large_chest_reached
		   && s.dispenser_reached);
	assert(s.quit_reached && !s.quit_failed);
	assert(m.screen == MENU_AP_SCREEN_TITLE);
	assert(!m.home_pressed_on_title);

	printf("[test_inventory_hold_leg] PASS (ticks=%d, dwell=%zu ticks, "
		   "max_step=%d/%d)\n",
		   used, gap, s.max_step, menu_ap_script_step_count() - 1);
}

/* ---- the NO-POINTER leg (push-0921 A7 part 1, Astra Lane A item 5) -------
 * "The new no-pointer walker leg must actually suppress pointer availability."
 * Part 1 is the COUNT-FREE half: from the title, reach Select World, Texture
 * Packs and Multiplayer by direction + activate, and back out of each with
 * the Close role alone -- with the pointer asserted UNAVAILABLE on every tick
 * of the leg. Nothing inside a list or an action strip is activated, because
 * reaching a strip costs `rows + 1` presses that this pure module cannot
 * count; part 2 adds those on top of the production focus observers.
 *
 * The four things pinned here:
 *   - the leg is the LAST thing the script does, and its terminal step is the
 *     PASS ordinal, so the profile's gate cannot be met without finishing it;
 *   - pointer_suppressed is true on EVERY tick from the leg's first step to
 *     its last, and false on every tick before it (the create-world leg still
 *     needs its one synthetic pointer);
 *   - all three screens were entered AND closed, in order;
 *   - no press that could LOAD a world or JOIN a server was ever issued. */
/* hw1030i proof gap: Texture Packs shows three rows, so the no-pointer leg
 * scrolls the list with D-pad DOWN to the LAST row (revealing it) and holds
 * a capture dwell there before Close. Selection only: no row activate, no
 * apply, the pending choice untouched, and Close still the only way out. */
static void test_np_texpacks_scrolls_to_last_row(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_start_at_np_leg(&s);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.texpacks_rows = 5; /* default, Beta, E&A pack, Isabella II, one more */
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.np_leg_done && s.np_failed_step == 0);
	assert(m.texpacks_max_sel == 4 && "the last row was selected (revealed)");
	assert((float)m.texpacks_last_row_ticks * TICK_DT >= 3.0f
		&& "a full capture dwell on the last row");
	assert(m.texpacks_closes == 1 && m.pack_applies == 0
		&& m.texpacks_row_clicks == 0);
	assert(m.screen == MENU_AP_SCREEN_TITLE);
	printf("  np texpacks scroll: last row %d held %d ticks\n",
		m.texpacks_max_sel, m.texpacks_last_row_ticks);
}

static void test_no_pointer_leg(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));

	/* The leg ran, completed, and left the walker on the title. */
	assert(s.np_leg_done);
	assert(s.np_failed_step == 0);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(m.screen == MENU_AP_SCREEN_TITLE);

	/* Each screen entered once by direction + activate and closed once by the
	 * Close role. The Select World visit is the leg's, plus the ONE the
	 * create-world leg already made earlier in the run. */
	assert(m.sworld_closes == 1);
	assert(m.texpacks_closes == 1);
	assert(m.mp_visits == 1 && m.mp_closes == 1);
	assert(!m.title_landed_on_multiplayer); /* the deliberate visit is not the 09-01 derail */

	/* THE SAFETY PROPERTY. A GUI_CLICK on either list screen with no pointer
	 * is rule (3) ACTIVATE(selected), which with the ring still on a row
	 * LOADS a world / JOINS a server. The count-free leg issues none. */
	assert(!m.sworld_row_activated);
	assert(!m.mp_row_activated);

	/* THE SUPPRESSION. Find the leg's tick span by its step ordinals (every
	 * leg step is >= the failure step and <= the terminal step, and the step
	 * table is contiguous), then check every tick inside and outside it. */
	size_t first = log.count, last = 0;
	for(size_t k = 0; k < log.count; k++)
		if(log.suppressed[k]) {
			if(first == log.count)
				first = k;
			last = k;
		}
	assert(first < log.count && "the leg never suppressed the pointer at all");
	for(size_t k = first; k <= last; k++)
		assert(log.suppressed[k]
			   && "a tick INSIDE the no-pointer leg let the pointer through");
	for(size_t k = 0; k < first; k++)
		assert(!log.suppressed[k]
			   && "the pointer was suppressed BEFORE the leg started");
	/* The suppressed span runs to the END of the script: the leg is the last
	 * thing it does. Exactly ONE tick follows it -- STEP_DONE, which presses
	 * nothing and touches no pointer -- so suppression is released only once
	 * the walker has genuinely stopped driving. */
	assert(log.count - 1 - last == 1);
	assert(log.buttons[log.count - 1] == IB_COUNT);
	assert(log.pointer_targets[log.count - 1] == MENU_AP_PT_NONE);
	/* ... and the one synthetic pointer the script still owns is entirely
	 * OUTSIDE it, which is what makes "no synthetic-pointer substitution for
	 * this leg" (Astra Lane A item 5) a checked statement. */
	for(size_t k = first; k <= last; k++)
		assert(log.pointer_targets[k] == MENU_AP_PT_NONE);
	size_t pointer_ticks = 0;
	for(size_t k = 0; k < log.count; k++)
		if(log.pointer_targets[k] != MENU_AP_PT_NONE)
			pointer_ticks++;
	assert(pointer_ticks > 0 && "the create-world leg lost its pointer");

	printf("[test_no_pointer_leg] PASS (ticks=%d, leg ticks=%zu..%zu, "
		   "max_step=%d, required=%d)\n",
		   used, first, last, s.max_step,
		   menu_ap_script_step_required_for_pass());
}

/* A bounded wait inside the leg that never lands must ABANDON, name which
 * step ran out, and leave max_step BELOW the pass ordinal -- the whole point
 * of numbering STEP_NP_FAILED below the gate. Modelled on the real failure:
 * the Multiplayer screen's Close does nothing (which is exactly what a PC run
 * sees on the OSK-backed sub-editors, input_text_active() owning the role). */
static void test_no_pointer_leg_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.mp_close_blocked = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600); /* abandoned, not still "more" at the budget */
	assert(menu_ap_script_finished(&s));

	assert(!s.np_leg_done);
	assert(s.np_failed_step != 0 && "the leg failed without naming a step");
	assert(s.max_step < menu_ap_script_step_required_for_pass()
		   && "a FAILED leg satisfied the PASS gate");
	/* It got as far as it honestly could: the earlier two screens closed. */
	assert(m.sworld_closes == 1 && m.texpacks_closes == 1);
	assert(m.mp_visits == 1 && m.mp_closes >= 1);
	assert(m.screen == MENU_AP_SCREEN_MULTIPLAYER); /* stuck where it failed */
	assert(!m.sworld_row_activated && !m.mp_row_activated);
	/* The pointer stayed suppressed right through the failure and the
	 * abandonment -- a leg that gives up must not hand the pointer back. */
	assert(log.suppressed[log.count - 1]);

	printf("[test_no_pointer_leg_fails_by_name] PASS (ticks=%d, "
		   "np_failed_step=%d, max_step=%d < required=%d)\n",
		   used, s.np_failed_step, s.max_step,
		   menu_ap_script_step_required_for_pass());
}

/* The OSK marker (A7 part 1c, lead ruling): where the platform's keyboard can
 * own an OSK-backed screen's Close, the run must SAY the skipped coverage was
 * skipped rather than leave it invisible. The fact comes from the caller; the
 * script carries no platform conditional. */
static void test_text_input_owns_close_marker(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;

	/* Wii: the keyboard never owns the role, so nothing is marked. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(!s.text_input_owns_close);
	assert(!s.np_osk_legs_skipped);
	assert(s.np_leg_done);

	/* PC: the marker latches, and the leg still completes -- part 1 enters no
	 * OSK-backed screen, so the marker names what part 2 will have to skip
	 * HERE rather than failing a run that proved everything it claimed. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, true);
	mock_reset(&m, true);
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(s.text_input_owns_close);
	assert(s.np_osk_legs_skipped);
	assert(s.np_leg_done);

	/* reset() clears both again (it is a full re-arm, not a patch). */
	menu_ap_script_reset(&s, false);
	assert(!s.text_input_owns_close && !s.np_osk_legs_skipped
		   && !s.np_leg_done && s.np_failed_step == 0);

	printf("[test_text_input_owns_close_marker] PASS\n");
}

/* ---- part 2: the observation-driven sub-legs --------------------------- */
#define SUB_BIT(k) (1u << (unsigned)(k))
#define SUB_ALL_OSK                                                            \
	(SUB_BIT(MENU_AP_SUB_SWORLD_RENAME) | SUB_BIT(MENU_AP_SUB_MPS_DIRECT)      \
	 | SUB_BIT(MENU_AP_SUB_MPS_ADD) | SUB_BIT(MENU_AP_SUB_MPS_EDIT)            \
	 | SUB_BIT(MENU_AP_SUB_MPS_NAME))
#define SUB_ALL ((1u << MENU_AP_SUB_COUNT) - 1u)

/* THE SAFETY INVARIANTS, asserted for every part-2 case rather than once:
 * nothing was loaded, joined, deleted or saved, and no activate ever shared a
 * frame with a direction edge (R-B would have consumed it, and on a confirm
 * that used to fire the destructive answer). */
static void assert_sub_legs_harmless(const struct mock_engine* m,
									  const struct mock_click_log* log) {
	assert(!m->sworld_row_activated && "an activate would have LOADED a world");
	assert(!m->mp_row_activated && "an activate would have JOINED a server");
	assert(!m->world_deleted && "the delete confirm was answered with activate");
	assert(!m->sub_editor_saved && "a sub-editor was left by Save, not Close");
	/* R-B, checked from the log itself and scoped to the LEG: every activate
	 * the leg issues has at least one fully RELEASED tick in front of it, so
	 * a direction edge can never share its frame. Deliberately stricter than
	 * R-B needs -- the rule only forbids the SAME frame, and the walker emits
	 * one button per frame anyway -- because the leg's whole safety argument
	 * is that the observation it activates on was read after the last
	 * navigation settled. The pre-leg script is NOT held to this: it presses
	 * a row DOWN and activates on the very next frame in the texture-pack
	 * legs, which R-B permits (different frames) and which has been on
	 * hardware for months. */
	for(size_t k = 1; k < log->count; k++)
		if(log->buttons[k] == IB_GUI_CLICK && log->suppressed[k])
			assert(log->buttons[k - 1] == IB_COUNT
				   && "a leg activate landed one tick after a press");
}

/* The console case: input_text_active() is a hard false there, so all seven
 * sub-legs are walked to their control, entered and left. */
static void test_sub_legs_console(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	assert(s.np_leg_done);
	assert(s.np_failed_step == 0);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());

	/* Every sub-leg reached its named control AND round-tripped through it;
	 * none was skipped. */
	assert(s.np_focus_reached == SUB_ALL);
	assert(s.np_entered == SUB_ALL);
	assert(s.np_skipped == 0u);
	assert(!s.np_osk_legs_skipped);

	/* The screens those entries actually opened -- proved from the MOCK's
	 * side, so a walker that set its own latches without the screen moving
	 * would fail here. The create-world screen is opened TWICE: once by the
	 * old pointer leg near the top of the script, once by this one. */
	assert(m.cworld_opens == 1 && m.rename_opens == 1);
	assert(m.confirm_opens == 1); /* Delete, and only Delete */
	assert(m.view_opens[MENU_AP_MPS_VIEW_DIRECT] == 1);
	assert(m.view_opens[MENU_AP_MPS_VIEW_ADD] == 1);
	assert(m.view_opens[MENU_AP_MPS_VIEW_EDIT] == 1);
	assert(m.view_opens[MENU_AP_MPS_VIEW_NAME] == 1);
	assert(m.mps_view == MENU_AP_MPS_VIEW_MAIN); /* every sub-editor closed */
	/* The world survived the delete confirm -- the outcome check, not a latch. */
	assert(m.world_rows == 3);
	assert_sub_legs_harmless(&m, &log);

	/* Still no pointer anywhere in the leg. */
	for(size_t k = 0; k < log.count; k++)
		if(log.suppressed[k])
			assert(log.pointer_targets[k] == MENU_AP_PT_NONE);

	printf("[test_sub_legs_console] PASS (ticks=%d, entered=0x%02x, "
		   "max_step=%d, required=%d)\n",
		   used, s.np_entered, s.max_step,
		   menu_ap_script_step_required_for_pass());
}

/* The PC case: a real keyboard owns the Close on every OSK-backed screen, so
 * those five sub-legs prove the FOCUS reached their control and then stop,
 * with a named skip. Create and Delete are not OSK-backed and still run. */
static void test_sub_legs_osk_platform(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, true);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.np_leg_done && s.np_failed_step == 0);

	/* The distinction that must never blur: the ring REACHED all seven, and
	 * exactly the five OSK-backed ones were not entered. */
	assert(s.np_focus_reached == SUB_ALL);
	assert(s.np_skipped == SUB_ALL_OSK);
	assert(s.np_entered == (SUB_ALL & ~SUB_ALL_OSK));
	assert(s.np_osk_legs_skipped);
	assert(m.rename_opens == 0);
	assert(m.view_opens[MENU_AP_MPS_VIEW_ADD] == 0);
	assert(m.view_opens[MENU_AP_MPS_VIEW_NAME] == 0);
	/* ... and the two that are not OSK-backed still did the whole trip. */
	assert(m.cworld_opens == 1 && m.confirm_opens == 1);
	assert(m.world_rows == 3);
	assert_sub_legs_harmless(&m, &log);

	printf("[test_sub_legs_osk_platform] PASS (ticks=%d, skipped=0x%02x, "
		   "reached=0x%02x)\n",
		   used, s.np_skipped, s.np_focus_reached);
}

/* Empty lists: the screens DISABLE Rename, Delete and Edit, and the focus
 * ring skips a disabled button rather than landing on it. That is the screen
 * being correct, so those sub-legs are NAMED SKIPS and the leg still passes.
 * The always-enabled ones (Create, Direct Connect, Add, the name editor) must
 * still run. */
static void test_sub_legs_empty_lists(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 0;
	m.server_rows = 0;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.np_leg_done && "an empty list must SKIP, not fail the leg");
	assert(s.np_failed_step == 0);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());

	unsigned row_gated = SUB_BIT(MENU_AP_SUB_SWORLD_RENAME)
		| SUB_BIT(MENU_AP_SUB_SWORLD_DELETE) | SUB_BIT(MENU_AP_SUB_MPS_EDIT);
	assert(s.np_skipped == row_gated);
	assert(s.np_entered == (SUB_ALL & ~row_gated));
	/* A skipped-for-being-disabled sub-leg is NOT "focus reached": the ring
	 * genuinely never rested on it, and claiming otherwise would be the one
	 * lie this bookkeeping exists to prevent. */
	assert((s.np_focus_reached & row_gated) == 0u);
	assert(m.rename_opens == 0 && m.confirm_opens == 0);
	assert(m.view_opens[MENU_AP_MPS_VIEW_EDIT] == 0);
	assert(m.cworld_opens == 1);
	assert(m.view_opens[MENU_AP_MPS_VIEW_DIRECT] == 1);
	assert(m.view_opens[MENU_AP_MPS_VIEW_ADD] == 1);
	assert(m.view_opens[MENU_AP_MPS_VIEW_NAME] == 1);
	assert_sub_legs_harmless(&m, &log);

	printf("[test_sub_legs_empty_lists] PASS (ticks=%d, skipped=0x%02x)\n", used,
		   s.np_skipped);
}

/* A sub-editor that will not close fails the leg BY NAME -- naming both the
 * step and the sub-leg, which one shared set of step ordinals cannot do on
 * its own -- and leaves max_step below the pass ordinal. */
static void test_sub_leg_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 0;	/* skip the Select World row-gated pair */
	m.server_rows = 1;
	m.mp_close_blocked = true; /* the MAIN-view Close never leaves */
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_failed_step != 0);
	assert(s.max_step < menu_ap_script_step_required_for_pass());
	/* The four Multiplayer sub-legs all completed; what will not close is the
	 * MAIN view itself, which is NOT one of them -- so np_failed_sub must say
	 * "not in a sub-leg" rather than blame the last one that ran and passed. */
	assert(s.np_entered != 0u);
	assert(s.np_failed_sub == -1);
	assert_sub_legs_harmless(&m, &log);

	printf("[test_sub_leg_fails_by_name] PASS (ticks=%d, failed step=%d "
		   "sub=%d)\n",
		   used, s.np_failed_step, s.np_failed_sub);
}

/* No Multiplayer observers registered (the engine-core link pool does not
 * take screen_multiplayer_setup.c, so an unwired build genuinely cannot see
 * that screen's focus or view). The four Multiplayer sub-legs must then be
 * NAMED SKIPS with their own distinct latch -- never a blind walk, and never
 * quietly folded in with the two legitimate skip reasons. */
static void test_sub_legs_mps_unobserved(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.mps_unobserved = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.np_leg_done && s.np_failed_step == 0);
	assert(s.np_mps_unobserved && "the wiring gap was not reported");

	unsigned mps = SUB_BIT(MENU_AP_SUB_MPS_DIRECT) | SUB_BIT(MENU_AP_SUB_MPS_ADD)
		| SUB_BIT(MENU_AP_SUB_MPS_EDIT) | SUB_BIT(MENU_AP_SUB_MPS_NAME);
	assert(s.np_skipped == mps);
	assert((s.np_focus_reached & mps) == 0u); /* the ring never went near them */
	assert(s.np_entered == (SUB_ALL & ~mps)); /* Select World's three still ran */
	/* NOT ONE press at the screen it cannot see into: no sub-editor opened,
	 * and the Multiplayer visit is still only the dwell + Close of part 1. */
	assert(m.view_opens[MENU_AP_MPS_VIEW_ADD] == 0);
	assert(m.view_opens[MENU_AP_MPS_VIEW_DIRECT] == 0);
	assert(m.view_opens[MENU_AP_MPS_VIEW_NAME] == 0);
	assert(m.mp_visits == 1 && m.mp_closes == 1);
	assert_sub_legs_harmless(&m, &log);

	printf("[test_sub_legs_mps_unobserved] PASS (ticks=%d, skipped=0x%02x)\n",
		   used, s.np_skipped);
}

/* ---- A14: THE CONSOLE DEFECT ------------------------------------------
 * push-0921 A13, real Wii, build 82e7bfe_09.21, three identical runs: the leg
 * died in its FIRST sub-leg (Select World -> Create) with np_flags=16
 * (leg_failed), np_entered=0, np_focus=0 and np_rows=645, and the capture
 * frames show the focus ring resting on world row 160 -- which is not a screen
 * state, it is the walk's old flat 160-press budget. The card's world list
 * holds ~645 rows (one leftover world per historical rig run) and the DOWN
 * walk out of that list costs one press per row.
 *
 * The mock reproduced NONE of this because its default list is three rows
 * long: every budget reaches the strip. This case is that card. */
static void test_sub_legs_long_world_list(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false); /* the console */
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 650; /* the maintainer's card, rounded up */
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 3000);
	assert(used < 20 * 3000);
	assert(menu_ap_script_finished(&s));

	/* THE CLAIM: a long list is not a defect, and the leg completes on it. */
	assert(s.np_leg_done && "the leg must complete on a 650-world card");
	assert(s.np_failed_step == 0 && s.np_flags == 0u);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(s.np_rows == 650);

	/* All seven entered -- in particular Rename and Delete, which a walk that
	 * merely ran out of presses used to record as "row-gated skips" on a card
	 * with six hundred rows in the list. */
	assert(s.np_focus_reached == SUB_ALL);
	assert(s.np_entered == SUB_ALL);
	assert(s.np_skipped == 0u);
	assert(m.cworld_opens == 1 && m.rename_opens == 1 && m.confirm_opens == 1);
	assert(m.world_rows == 650 && !m.world_deleted);
	assert_sub_legs_harmless(&m, &log);

	/* THE PRICE, pinned so it cannot grow back unnoticed.
	 *
	 * It USED to be 3 * 650 + 4 = 1954 DOWN presses: the Select World screen
	 * resets its ring to row 0 on every arrival, so each of the three sub-legs
	 * hosted there re-crossed the whole list (rows - 1 to reach the last row,
	 * one more to step off it into the strip), plus one DOWN off the single
	 * server row into the Multiplayer strip. A14 made that budget follow the
	 * OBSERVED row count so it could not run out; it still COST one press per
	 * row, and on the console's pace ~27 s per crossing of that card.
	 *
	 * A15 removed the crossings entirely: GUI RIGHT enters the strip from
	 * whatever row the ring is on. What is left is the leg's FIXED navigation
	 * -- 2 DOWNs from the title to Mods and Texture Packs, 1 to Multiplayer --
	 * and nothing that scales with the list at all. */
	/* ui-build-1026 T03: +1, the title walk to Texture packs passes the
	 * Tutorial world row (DOWN x2). */
	/* hw1030i: +1, the Texture Packs scroll walks DOWN to the last pack row
	 * (the mock has 2) -- bounded by the PACK count, never the world list. */
	assert(log_count_button_in_leg(&log, IB_GUI_DOWN) == 7
		   && "the approved route's fixed DOWN count changed");

	printf("[test_sub_legs_long_world_list] PASS (ticks=%d, rows=%d, leg "
		   "DOWNs=%zu, entered=0x%02x)\n",
		   used, s.np_rows, log_count_button_in_leg(&log, IB_GUI_DOWN),
		   s.np_entered);
}

/* A15: the leg's cost DOES NOT SCALE WITH THE LIST any more. The claim is
 * made the only way that cannot be fudged -- run the identical leg on a
 * three-row card and on a 650-row card and compare every button's in-leg
 * press count. A number derived by hand could be wrong in the same direction
 * twice; two runs that differ only in how many worlds exist cannot. */
static void test_strip_entry_does_not_scale_with_the_list(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	static struct mock_click_log small, big;
	static const enum input_button watched[]
		= {IB_GUI_UP, IB_GUI_DOWN, IB_GUI_LEFT, IB_GUI_RIGHT, IB_GUI_CLICK,
		   IB_INVENTORY};

	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 3;
	log_reset(&small);
	assert(drive(&s, &m, &small, 20 * 3000) < 20 * 3000);
	assert(s.np_leg_done && s.np_entered == SUB_ALL && s.np_flags == 0u);

	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 650;
	m.sworld_sel = 649; /* the retained selection is at the deepest row */
	log_reset(&big);
	assert(drive(&s, &m, &big, 20 * 3000) < 20 * 3000);
	assert(s.np_leg_done && s.np_entered == SUB_ALL && s.np_flags == 0u);

	for(size_t k = 0; k < sizeof(watched) / sizeof(watched[0]); k++)
		assert(log_count_button_in_leg(&small, watched[k])
				   == log_count_button_in_leg(&big, watched[k])
			   && "the no-pointer leg's press cost still depends on the "
				  "length of the world list");

	printf("[test_strip_entry_does_not_scale_with_the_list] PASS (3 rows vs "
		   "650 rows: DOWN %zu == %zu, RIGHT %zu == %zu)\n",
		   log_count_button_in_leg(&small, IB_GUI_DOWN),
		   log_count_button_in_leg(&big, IB_GUI_DOWN),
		   log_count_button_in_leg(&small, IB_GUI_RIGHT),
		   log_count_button_in_leg(&big, IB_GUI_RIGHT));
}

#if 0 /* Retired with the approved tab/modal routes: these inject defects into
		  * the removed flat action strips, which no production screen has. */
/* ... and the FALLBACK is real. A screen that stops honouring the RIGHT entry
 * (the regression this walk must survive, since the walker ships ahead of the
 * screens it drives on a card) must not strand the leg: the walk falls back to
 * A14's DOWN converge, whose budget is the OBSERVED row count, and the leg
 * still completes -- paying the old per-row price, which is exactly how the
 * fallback announces itself in a trace. */
static void test_strip_entry_falls_back_to_the_down_converge(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 650;
	m.sworld_swallow_list_right = true;
	m.mps_swallow_list_right = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 3000);
	assert(used < 20 * 3000);
	assert(s.np_leg_done && "the DOWN converge fallback did not carry the leg");
	assert(s.np_failed_step == 0);
	/* AND IT SAYS SO (review A16-3). This case used to assert np_flags == 0,
	 * i.e. the suite itself certified that a completely swallowed RIGHT entry
	 * produces a clean tape -- which is exactly how a card whose screens
	 * predate A15 would have passed the console gate with the card's headline
	 * claim untested. The leg still COMPLETES (the fallback's job), and the
	 * run is refused by the collector (any non-zero np_flags). */
	assert(s.np_flags == MENU_AP_NP_FLAG_STRIP_FALLBACK
		   && "the DOWN-converge fallback left no trace in the tape");
	assert(s.np_entered == SUB_ALL && s.np_skipped == 0u);
	assert(m.world_rows == 650 && !m.world_deleted);
	/* The old price, paid again: three crossings of 650 rows, the leg's three
	 * fixed title DOWNs, and one DOWN off the single server row -- plus the
	 * one RIGHT per entry that the screen swallowed, which is not a DOWN. */
	assert(log_count_button_in_leg(&log, IB_GUI_DOWN) == 3 * 650 + 5); /* +1 Tutorial row (ui-build-1026 T03) */
	assert_sub_legs_harmless(&m, &log);

	/* The tick count IS the before/after number for "the leg no longer scales
	 * with the list": this run is the old behaviour on a 650-world card, and
	 * test_sub_legs_long_world_list is the same card with the RIGHT entry
	 * live. */
	printf("[test_strip_entry_falls_back_to_the_down_converge] PASS (ticks=%d, "
		   "leg DOWNs=%zu, entered=0x%02x)\n",
		   used, log_count_button_in_leg(&log, IB_GUI_DOWN), s.np_entered);
}

/* A converge walk that runs OUT of presses fails the sub-leg BY NAME and
 * records NO skip -- even for a conditional target, where "the button is
 * disabled" would otherwise be a legitimate skip. Reaching the strip at all is
 * never optional: both host screens always hold at least one enabled strip
 * button (Create/Cancel, Direct Connect/Back), so a ring still stuck in the
 * list is a failure of the walk, not the screen exercising its own rules. The
 * old code handed an exhausted converge to SEEK, which turned it into exactly
 * that false skip. */
static void test_converge_exhaustion_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	/* The Select World list stops accepting DOWN once the CREATE sub-leg has
	 * been through the create-world screen, so the failure lands on RENAME --
	 * a CONDITIONAL sub-leg, which is the one a false skip could hide in. */
	m.sworld_swallow_downs_after_cworld = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_flags & MENU_AP_NP_FLAG_LEG_FAILED);
	assert(s.np_failed_step != 0);
	assert(s.np_failed_sub == MENU_AP_SUB_SWORLD_RENAME);
	assert(s.max_step < menu_ap_script_step_required_for_pass());
	/* The first sub-leg still did its whole trip; the second claimed nothing. */
	assert(s.np_entered == SUB_BIT(MENU_AP_SUB_SWORLD_CREATE));
	assert(s.np_skipped == 0u && "a walk that ran out must not record a skip");
	assert((s.np_focus_reached & SUB_BIT(MENU_AP_SUB_SWORLD_RENAME)) == 0u);
	assert(m.rename_opens == 0 && m.confirm_opens == 0);
	assert(m.world_rows == 3 && !m.world_deleted);
	assert_sub_legs_harmless(&m, &log);

	printf("[test_converge_exhaustion_fails_by_name] PASS (failed step=%d "
		   "sub=%d, skipped=0x%02x)\n",
		   s.np_failed_step, s.np_failed_sub, s.np_skipped);
}
#endif

/* A nonempty approved Load graph that refuses row -> Play must fail by name;
 * it is not the legitimate empty-list case and may not be silently skipped. */
static void test_semantic_route_exhaustion_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.sworld_swallow_list_right = true;
	log_reset(&log);
	assert(drive(&s, &m, &log, 20 * 600) < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_flags & MENU_AP_NP_FLAG_LEG_FAILED);
	assert(s.np_failed_sub == MENU_AP_SUB_SWORLD_CREATE);
	assert(s.np_skipped == 0u);
	assert(m.world_rows == 3 && !m.world_deleted);
	assert_sub_legs_harmless(&m, &log);
	printf("[test_semantic_route_exhaustion_fails_by_name] PASS "
		   "(failed step=%d sub=%d)\n", s.np_failed_step, s.np_failed_sub);
}

static void test_nonempty_join_options_exhaustion_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.mps_swallow_join_right = true;
	log_reset(&log);
	assert(drive(&s, &m, &log, 20 * 600) < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_flags & MENU_AP_NP_FLAG_LEG_FAILED);
	assert(s.np_failed_sub == MENU_AP_SUB_MPS_EDIT);
	assert((s.np_skipped & MENU_AP_NP_BIT(MENU_AP_SUB_MPS_EDIT)) == 0u);
	assert(m.server_rows == 1 && !m.sub_editor_saved);
	assert_sub_legs_harmless(&m, &log);
	printf("[test_nonempty_join_options_exhaustion_fails_by_name] PASS "
		   "(failed step=%d sub=%d)\n", s.np_failed_step, s.np_failed_sub);
}

/* THE PC ENTRY LEVER (A14 item 1). Off, the script is what it has always been
 * -- the other cases in this file are that contract, and the first tick still
 * runs on step 0. On, the script starts at the no-pointer leg's first step,
 * carries MENU_AP_NP_FLAG_ENTRY_LEVER from its very first tick (so the
 * collector refuses the tape by name), and runs the leg IDENTICALLY: the
 * lever is an entry point, not a second code path. That last claim is checked
 * literally -- the (step, button) sequence of every suppressed tick of a lever
 * run equals the same sequence from a normal run. */
static void test_entry_lever_starts_at_np_leg(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	static struct mock_click_log plain, lever;

	/* --- lever OFF: unchanged --- */
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&plain);
	int plain_ticks = drive(&s, &m, &plain, 20 * 600);
	assert(plain_ticks < 20 * 600);
	assert(plain.steps[0] == 0 && "a lever-free run still starts at step 0");
	assert(!plain.suppressed[0] && "step 0 is outside the leg");
	assert(s.np_leg_done && s.np_flags == 0u);
	assert(!(s.np_flags & MENU_AP_NP_FLAG_ENTRY_LEVER));
	assert(s.max_step >= menu_ap_script_step_required_for_pass());

	/* --- lever ON: starts at the leg, flagged from tick 0 --- */
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	menu_ap_script_start_at_np_leg(&s);
	assert(s.np_flags == MENU_AP_NP_FLAG_ENTRY_LEVER
		   && "the flag must be up BEFORE the first tick");
	assert(s.max_step == s.step && s.step > 0);
	int lever_first_step = s.step;
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&lever);
	int lever_ticks = drive(&s, &m, &lever, 20 * 600);
	assert(lever_ticks < 20 * 600);
	assert(lever.steps[0] == lever_first_step);
	assert(lever.suppressed[0] && "the lever starts INSIDE the leg");

	/* The leg itself did the whole job, and the flag survived to the end. */
	assert(s.np_leg_done && s.np_failed_step == 0);
	assert(s.np_entered == SUB_ALL && s.np_skipped == 0u);
	assert(s.np_flags == MENU_AP_NP_FLAG_ENTRY_LEVER);
	/* ... yet the run can never be graded as a pass: the flag is a refusal in
	 * the collector, and nothing before the leg ran. */
	assert(m.cworld_opens == 1 && "only the leg's own create-world visit");
	assert(!s.sign_reached && !s.quit_reached && m.quit_done == 0);
	assert(m.opened_furnace == 0 && m.opened_chest == 0
		   && m.opened_dispenser == 0 && m.pack_applies == 0);
	assert(!s.furnace_reached && !s.chest_reached && !s.dispenser_reached);

	/* THE SAME LEG, tick for tick. */
	size_t p = 0, l = 0;
	size_t matched = 0;
	for(;;) {
		while(p < plain.count && !plain.suppressed[p])
			p++;
		while(l < lever.count && !lever.suppressed[l])
			l++;
		if(p >= plain.count || l >= lever.count)
			break;
		assert(plain.steps[p] == lever.steps[l]);
		assert(plain.buttons[p] == lever.buttons[l]);
		p++;
		l++;
		matched++;
	}
	assert(matched > 100 && "the leg comparison matched almost nothing");
	while(p < plain.count && !plain.suppressed[p])
		p++;
	while(l < lever.count && !lever.suppressed[l])
		l++;
	assert(p >= plain.count && l >= lever.count
		   && "one run's leg is longer than the other's");

	printf("[test_entry_lever_starts_at_np_leg] PASS (plain=%d ticks, "
		   "lever=%d ticks from step %d, %zu leg ticks identical)\n",
		   plain_ticks, lever_ticks, lever_first_step, matched);
}

/* ---- A11 item 2: the delete/rename proofs ------------------------------
 * Review A9-5: "the world still exists" used to be "focus still reports a
 * LIST row", which is only a proof on a ONE-world card. Review A9-6: nothing
 * observed that the confirmation opened on its safe answer, so a regression
 * of that default would be invisible to the run built to catch it. Both are
 * now observations, and both FAIL THE LEG BY NAME with their own np_flags
 * bit -- these three cases are what say so. */
static void test_delete_row_count_proof(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;

	/* A clean multi-world card: the count is latched and unchanged, so the
	 * leg passes and reports no flags at all. THREE worlds on purpose -- on
	 * one world the old focus-based check would also have passed, and the
	 * point is that this one holds where that one did not. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.np_leg_done && s.np_flags == 0u);
	assert(s.np_rows == 3 && m.world_rows == 3);
	assert(m.confirm_opens == 1);
	assert_sub_legs_harmless(&m, &log);

	/* The delete confirm's Close silently eats a world (what a regression in
	 * screen_confirm's back-out would look like). The count comparison must
	 * catch it EVEN THOUGH two worlds still remain -- exactly the case the
	 * old focus-based check waved through. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.delete_eats_row = true;
	log_reset(&log);
	used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_flags & MENU_AP_NP_FLAG_ROW_COUNT_MISMATCH);
	assert(s.np_flags & MENU_AP_NP_FLAG_LEG_FAILED);
	assert(s.np_failed_sub == MENU_AP_SUB_SWORLD_DELETE);
	assert(s.max_step < menu_ap_script_step_required_for_pass());
	assert(m.world_rows == 2 && "the case did not actually eat a row");

	/* The same guard around the RENAME sub-leg, which must not change how
	 * many worlds exist either. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.rename_eats_row = true;
	log_reset(&log);
	used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_flags & MENU_AP_NP_FLAG_ROW_COUNT_MISMATCH);
	assert(s.np_failed_sub == MENU_AP_SUB_SWORLD_RENAME);

	printf("[test_delete_row_count_proof] PASS (latched rows=3, both leaks "
		   "caught by name)\n");
}

/* The confirmation opening on the DESTRUCTIVE answer must be seen, flagged,
 * and must still leave by Close -- abandoning with a modal up would be worse
 * than the failure, and the Close answers NO regardless of the highlight, so
 * nothing is destroyed on the way out. */
static void test_confirm_not_safe_is_seen(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.confirm_unsafe = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(!s.np_leg_done);
	assert(s.np_flags & MENU_AP_NP_FLAG_CONFIRM_NOT_SAFE);
	assert(s.np_flags & MENU_AP_NP_FLAG_LEG_FAILED);
	assert(s.np_failed_sub == MENU_AP_SUB_SWORLD_DELETE);
	/* It LEFT first: the confirm was entered and left, the world survived,
	 * and no activate was ever pressed on the modal. */
	assert(s.np_entered & MENU_AP_NP_BIT(MENU_AP_SUB_SWORLD_DELETE));
	assert(m.world_rows == 3 && !m.world_deleted);
	assert(m.screen == MENU_AP_SCREEN_SELECT_WORLD);
	assert_sub_legs_harmless(&m, &log);

	printf("[test_confirm_not_safe_is_seen] PASS (flags=0x%02x, left by "
		   "Close first)\n",
		   s.np_flags);
}

/* np_flags is the collector's whole basis for "a console PASS means the leg
 * really entered things", so each bit must be reachable and none may fire on
 * a clean console run. */
static void test_np_flags_bits(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;

	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, true); /* the PC platform */
	mock_reset(&m, true);
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(s.np_flags == MENU_AP_NP_FLAG_OSK_SKIP);

	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.mps_unobserved = true;
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(s.np_flags == MENU_AP_NP_FLAG_MPS_UNOBSERVED);

	/* An empty world list is NOT a flag: the screen disabling Rename and
	 * Delete is correct behaviour, reported through np_skipped instead. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	m.world_rows = 0;
	m.server_rows = 0;
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(s.np_leg_done && s.np_flags == 0u);
	assert(s.np_rows == 0);

	/* The retired flat-strip fallback bit remains reserved in the wire format;
	 * approved tab/modal routes never raise it. */
	menu_ap_script_reset(&s, false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, true);
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(s.np_leg_done && s.np_flags == 0u);

	printf("[test_np_flags_bits] PASS\n");
}

static void test_sign_timeout_soft_skip(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/false); /* the sign never opens */
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	/* The sign is soft evidence only: never reaching it must NOT fail the
	 * script -- it must still reach the pause/options tail. */
	assert(!s.sign_reached);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(m.screen == MENU_AP_SCREEN_TITLE); /* the quit leg still ran */
	assert(!m.home_pressed_on_title);

	printf("[test_sign_timeout_soft_skip] PASS (ticks=%d, max_step=%d/%d)\n",
		   used, s.max_step, menu_ap_script_step_count() - 1);
}

static void test_container_legs_soft_skip(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/false);
	m.containers_open = false; /* every aimed right-click misses */
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	/* Container screens are bonus evidence like the sign: a miss skips the
	 * leg, presses nothing in-game (an OPEN_INVENTORY in-game would open the
	 * inventory), and the pause/options tail still runs. */
	assert(!s.furnace_reached && !s.chest_reached && !s.large_chest_reached
		   && !s.dispenser_reached);
	assert(m.opened_furnace == 0 && m.opened_chest == 0 && m.opened_dispenser == 0);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(m.screen == MENU_AP_SCREEN_TITLE); /* the quit leg still ran */
	/* only the inventory leg's open/close touched the two inventory roles
	 * in-game: the OPEN on IB_OPEN_INVENTORY, the close on IB_INVENTORY --
	 * plus the no-pointer leg's own Closes, which are counted separately,
	 * and the three Create World sub-leg Closes on the form (T15). */
	assert(log_count_button_outside_leg(&log, IB_INVENTORY) == 1 + 3);
	assert(log_count_button_outside_leg(&log, IB_OPEN_INVENTORY) == 1);

	printf("[test_container_legs_soft_skip] PASS (ticks=%d, max_step=%d/%d)\n",
		   used, s.max_step, menu_ap_script_step_count() - 1);
}

static void test_quit_leg_soft_skip(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.quit_fails = true; /* the server refuses the unload: the pause stays */
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	/* The quit itself is honest: not reached, flagged failed, the pause still
	 * up so the capture carries its "Save failed: ..." line.
	 *
	 * A7 part 1 CHANGED what this costs, on purpose. The PASS ordinal is now
	 * the no-pointer leg's terminal step, and that leg walks the TITLE -- a
	 * refused unload never leaves the pause, so it can never reach it. A
	 * run whose world would not unload therefore no longer passes the menus
	 * scenario gate. That is the honest reading: the leg is the profile's
	 * proof that the front end is operable by buttons alone, and this run
	 * never got to the front end at all. */
	assert(!s.quit_reached && s.quit_failed && m.quit_done == 0);
	assert(s.max_step < menu_ap_script_step_required_for_pass());
	assert(!s.np_leg_done && m.mp_visits == 0 && m.mp_closes == 0);
	assert(s.pack_applies == 0 && m.pack_applies == 0); /* pack legs are opt-in */
	assert(m.screen == MENU_AP_SCREEN_PAUSE);
	assert(log_count_button(&log, IB_HOME) == 5);

	printf("[test_quit_leg_soft_skip] PASS (ticks=%d, max_step=%d/%d)\n", used,
		   s.max_step, menu_ap_script_step_count() - 1);
}

static void test_stuck_screen_abandons_honestly(void) {
	/* select-world never responds to the synthetic click (e.g. a real
	 * regression in sworld_pointer_button's hit-test): the script must
	 * eventually ABANDON (finished=true) rather than spin forever or fake a
	 * PASS by pressing blindly on the wrong screen forever. */
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.block_sworld_create = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600); /* must have abandoned, not run out the clock still "more" */
	assert(menu_ap_script_finished(&s));
	assert(s.max_step < menu_ap_script_step_required_for_pass());
	assert(!m.home_pressed_on_title);
	assert(m.screen == MENU_AP_SCREEN_SELECT_WORLD); /* never got further */
	/* ...and SAYS where (hw1026c follow-up): every abandonment is named. */
	assert(s.map_fail == s.step + 1);
	printf("[test_stuck_screen_abandons_honestly] PASS (ticks=%d, max_step=%d)\n",
		   used, s.max_step);
}

static void test_never_presses_home_on_title(void) {
	/* Regression guard in isolation, independent of the mock's own screen
	 * graph: replay the exact button sequence from a full run and assert
	 * IB_HOME never coincides with a tick where the mock still reports
	 * MENU_AP_SCREEN_TITLE. screen_title.c wires IB_HOME to gstate.quit=true
	 * (Beta's Quit Game mirror) -- pressing it there would end the whole
	 * hardware run instead of dwelling the menus. */
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, true);
	log_reset(&log);
	drive(&s, &m, &log, 20 * 600);
	assert(!m.home_pressed_on_title);
	printf("[test_never_presses_home_on_title] PASS\n");
}

static void test_output_is_all_false_when_idle(void) {
	/* A dwell tick that hasn't crossed its threshold yet must emit a
	 * completely inert output (no press, no pointer) -- proves dwelling
	 * really is "do nothing, just prove liveness", not a hidden action. */
	struct menu_ap_state s;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	struct menu_ap_output out;
	struct menu_ap_observation obs = {0};
	bool more = menu_ap_script_tick(&s, MENU_AP_SCREEN_TITLE, obs, TICK_DT,
									 &out);
	assert(more);
	assert(!out.press);
	assert(out.pointer_target == MENU_AP_PT_NONE);
	/* INERTNESS, half 1: the no-pointer assertion is off outside the leg, so
	 * an armed walker sitting on the title leaves the real pointer exactly as
	 * it was before this field existed. */
	assert(!out.pointer_suppressed);
	printf("[test_output_is_all_false_when_idle] PASS\n");
}

/* BOOT-CREDITS-1028 T5 (hw1028c/d leg A): the walker armed at boot under
 * the credit cards. The title is classified underneath, but the gate consumes
 * every menu input, so the focus never moves: WITHOUT the hold the Tutorial
 * walk runs out of presses and fails at step 1 (walk_failed_step=1, the
 * hardware signature). WITH front_end_blocked the script stays inert for the
 * whole cold bake (here 60 s), then walks normally once the title is
 * admitted. */
static void test_holds_while_boot_cards_own_input(void) {
	struct menu_ap_state s;
	struct menu_ap_output out;
	struct menu_ap_observation obs = {0};
	obs.title_focus = MENU_AP_TITLE_PLAY; /* never moves: input consumed */
	/* the bug shape: no hold, presses swallowed -> named failure at step 1 */
	menu_ap_script_reset(&s, /*target_creative=*/false);
	for(int i = 0; i < 20 * 600 && !s.finished; i++)
		(void)menu_ap_script_tick(&s, MENU_AP_SCREEN_TITLE, obs, TICK_DT, &out);
	assert(s.finished && s.walk_failed_step == 1);
	/* the fix: held for 60 s of frames, nothing pressed, nothing advanced */
	menu_ap_script_reset(&s, /*target_creative=*/false);
	struct menu_ap_state fresh = s;
	obs.front_end_blocked = true;
	for(int i = 0; i < 60 * 60; i++) {
		assert(menu_ap_script_tick(&s, MENU_AP_SCREEN_TITLE, obs, TICK_DT,
								   &out));
		assert(!out.press && !out.pointer_suppressed
			   && out.pointer_target == MENU_AP_PT_NONE);
	}
	assert(memcmp(&s, &fresh, sizeof(s)) == 0 && "the hold moved the script");
	/* admitted: the same walk now reaches Tutorial (focus follows presses) */
	obs.front_end_blocked = false;
	int presses = 0;
	for(int i = 0; i < 20 * 600 && s.step <= 1 && !s.finished; i++) {
		(void)menu_ap_script_tick(&s, MENU_AP_SCREEN_TITLE, obs, TICK_DT, &out);
		if(out.press && out.button == IB_GUI_DOWN) {
			obs.title_focus++;
			presses++;
		}
	}
	assert(!s.finished && s.walk_failed_step == 0 && s.step > 1);
	assert(obs.title_focus == MENU_AP_TITLE_TUTORIAL && presses == 1);
	printf("[test_holds_while_boot_cards_own_input] PASS (unheld run fails "
		   "at step 1; held 3600 ticks inert; then walks to Tutorial)\n");
}

static void test_finished_state_is_sticky_and_inert(void) {
	struct menu_ap_state s;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	s.finished = true;
	s.max_step = 3;
	struct menu_ap_output out;
	memset(&out, 0xAA, sizeof(out)); /* poison, so a missed write is visible */
	struct menu_ap_observation obs = {0};
	bool more = menu_ap_script_tick(&s, MENU_AP_SCREEN_TITLE, obs, TICK_DT,
									 &out);
	assert(!more);
	assert(!out.press);
	assert(out.pointer_target == MENU_AP_PT_NONE);
	/* INERTNESS, half 2: a FINISHED script emits no suppression either, so it
	 * can never hold the pointer down after the run is over. (menu_autopilot.c
	 * stops consulting the output entirely at that point -- this is the
	 * belt-and-braces half of the same statement.) */
	assert(!out.pointer_suppressed);
	assert(s.max_step == 3); /* untouched */
	printf("[test_finished_state_is_sticky_and_inert] PASS\n");
}

static void test_reset_is_all_zero_except_step(void) {
	struct menu_ap_state s;
	memset(&s, 0xAA, sizeof(s)); /* poison the whole struct first */
	menu_ap_script_reset(&s, /*target_creative=*/false);
	assert(s.step == 0);
	assert(s.timer == 0.0f);
	assert(s.repeat_done == 0);
	assert(!s.repeat_phase);
	assert(!s.finished);
	assert(!s.sign_reached);
	assert(s.max_step == 0);
	assert(!s.sign_wait_timed_out);
	assert(!s.target_creative); /* the target_creative=false passed above */
	assert(!s.text_input_owns_close && !s.np_osk_legs_skipped
		   && !s.np_leg_done && s.np_failed_step == 0);
	assert(s.np_sub == 0 && s.np_skipped == 0u && s.np_focus_reached == 0u
		   && s.np_entered == 0u && !s.np_mps_unobserved);
	/* The two fields reset() must NOT leave at memset's 0: np_rows -1
	 * because zero IS a real world count, np_failed_sub -1 because zero is
	 * a real sub-leg ordinal (Select World's Create). */
	assert(s.np_rows == -1 && s.np_failed_sub == -1);
	assert(s.np_flags == 0u && s.np_pending_fail_step == 0);
	printf("[test_reset_is_all_zero_except_step] PASS\n");

	memset(&s, 0xAA, sizeof(s));
	menu_ap_script_reset(&s, /*target_creative=*/true);
	assert(s.target_creative); /* reset() takes the caller's value, not always false */
	printf("[test_reset_is_all_zero_except_step] PASS (target_creative=true leg)\n");
}

static void test_mode_select_survives_one_frame_late_press(void) {
	/* The exact hardening this hardens against (staged_menus_dwell_
	 * 20260902_063552, real-Wii): a bare one-frame LEFT press whose effect
	 * on the model lands one tick late must NOT let the world be created
	 * Creative. cworld_mode_skew makes the mock apply the press's mode flip
	 * one tick after it fires -- the SAME shape adversarial review named as
	 * the #1 suspect -- and the run must still finish with Survival. */
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.cworld_mode_skew = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(m.cworld_saw_creative);	 /* the capture dwell still reached Creative */
	assert(m.cworld_mode == 0);	 /* but the confirmed final state is Survival */
	assert(!m.cworld_created_creative); /* and Create was pressed AFTER that confirmation */
	/* Counted OUTSIDE the A7 leg: the leg's own LEFT presses are its action-
	 * strip clamp and have nothing to do with the game-mode selector this
	 * case is about -- reporting the raw total would read as a regression. */
	printf("[test_mode_select_survives_one_frame_late_press] PASS (ticks=%d, "
		   "left_presses=%zu)\n",
		   used, log_count_button_outside_leg(&log, IB_GUI_LEFT));
}

static void test_mode_select_never_confirms_fails_distinctly(void) {
	/* A selector that NEVER responds at all (not a timing fluke -- a real
	 * regression) must abandon the whole script rather than proceed into
	 * Create with an unconfirmed mode. */
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.block_mode_toggle = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600); /* must have abandoned, not run out the clock still "more" */
	assert(menu_ap_script_finished(&s));
	assert(s.max_step < menu_ap_script_step_required_for_pass());
	assert(m.screen == MENU_AP_SCREEN_CREATE_WORLD); /* never reached Create */
	assert(!m.cworld_created_creative);
	printf("[test_mode_select_never_confirms_fails_distinctly] PASS "
		   "(ticks=%d, step=%d)\n",
		   used, s.step);
}

/* menus-dwell-walker-1026: the observed walk to Create fails BY NAME, and
 * never presses Create, when the real screen does not say Create is ready. */
static void cworld_walk_fails_by_name(const char* label, bool swallow_down,
									  bool block_create, bool open_on_down,
									  int want_gate) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.cworld_swallow_down = swallow_down;
	m.cworld_block_create = block_create;
	m.cworld_open_on_down = open_on_down;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_cworld_failed());
	assert(s.cworld_failed_step == want_gate);
	assert(s.max_step < menu_ap_script_step_wait_ingame());
	assert(m.screen == MENU_AP_SCREEN_CREATE_WORLD); /* Create never actioned */
	printf("[test_cworld_walk_fails_by_name/%s] PASS (ticks=%d, gate=%d)\n",
		   label, used, s.cworld_failed_step);
}

/* The pause walk to Options fails BY NAME when the focus never moves, and
 * never activates a row it did not observe (the hw1026b failure opened the
 * Achievements board instead). */
static void test_pause_walk_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.pause_swallow_down = true;
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_walk_failed());
	/* STEP_PAUSE_NAV_OPTIONS is the step after STEP_PAUSE_DWELL, two before
	 * STEP_WAIT_OPTIONS. */
	assert(s.walk_failed_step == menu_ap_script_step_wait_options() - 2);
	assert(m.screen == MENU_AP_SCREEN_PAUSE);
	assert(m.achievements_opened == 0);
	printf("[test_pause_walk_fails_by_name] PASS (ticks=%d, gate=%d)\n", used,
		   s.walk_failed_step);
}

/* THE STALL GUARD (hw1026c follow-up): a dwell whose screen never appears has
 * no timeout of its own; it must still end the run, named, instead of
 * stalling to the profile cap with nothing on the tape. */
/* Sol round-3 BLOCKING: the guard measures PROGRESS, not step age. On an
 * empty server list the Edit sub-leg legitimately spends its whole
 * NP_WALK_MAX_CONVERGE walk (160 presses + 160 released ticks = 320 ticks) in
 * ONE step before the named skip -- 32 s at 10 fps. Every one of those ticks
 * presses or releases, so the guard must never trip; the leg must finish. */
static void test_stall_guard_spares_slow_progress(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	g_tick_dt = 1.0f / 10.0f;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.world_rows = 0;
	m.server_rows = 0;
	log_reset(&log);
	int used = drive(&s, &m, &log, 10 * 1200);
	g_tick_dt = 1.0f / 20.0f;
	if(!s.np_leg_done)
		fprintf(stderr, "slow progress stopped: step=%d map_fail=%d stalled=%d "
						"np_failed=%d sub=%d\n",
				s.step, s.map_fail, (int)s.stalled, s.np_failed_step, s.np_sub);
	assert(!s.stalled && s.map_fail == 0
		   && "the stall guard aborted a slow but progressing walk");
	assert(s.np_leg_done
		   && (s.np_skipped & SUB_BIT(MENU_AP_SUB_MPS_EDIT)) != 0);
	printf("[test_stall_guard_spares_slow_progress] PASS (ticks=%d at 10 fps)\n",
		   used);
}

static void test_stall_guard_names_a_silent_stall(void) {
	struct menu_ap_state s;
	struct menu_ap_output out;
	struct menu_ap_observation obs = {0};
	obs.title_focus = MENU_AP_TITLE_NONE;
	obs.pause_focus = MENU_AP_PAUSE_NONE;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	int ticks = 0;
	/* STEP_TITLE_DWELL on a screen that is never the title: no press, no
	 * observed change -- a TRUE stall, which must still trip, at 20 fps and
	 * at 10 fps alike (the limit is wall seconds of no progress). */
	while(!s.finished && ticks < 20 * 120) {
		(void)menu_ap_script_tick(&s, MENU_AP_SCREEN_OTHER, obs, 0.05f, &out);
		ticks++;
	}
	assert(s.finished && s.stalled);
	assert(s.map_fail == 0 + 1); /* step 0, STEP_TITLE_DWELL, named */
	assert(ticks >= 20 * 30 && ticks <= 20 * 30 + 2);
	menu_ap_script_reset(&s, /*target_creative=*/false);
	ticks = 0;
	while(!s.finished && ticks < 10 * 120) {
		(void)menu_ap_script_tick(&s, MENU_AP_SCREEN_OTHER, obs, 0.1f, &out);
		ticks++;
	}
	assert(s.finished && s.stalled && s.map_fail == 1);
	assert(ticks >= 10 * 30 && ticks <= 10 * 30 + 2);
	printf("[test_stall_guard_names_a_silent_stall] PASS (ticks=%d)\n", ticks);
	/* A completed run carries no marker. */
}

static void test_cworld_walk_fails_by_name(void) {
	/* The happy path's own trace gives the two gate ordinals: NAV_CREATE is
	 * the step right before SUBMIT, SUBMIT the one right before WAIT_INGAME. */
	int submit = menu_ap_script_step_wait_ingame() - 1;
	int nav = submit - 1;
	/* DOWN never moves the ring: the observed Game-mode probe (T05) runs out
	 * its bound before any walk to Create -- RIGHT on Name flips nothing. */
	cworld_walk_fails_by_name("ring_stuck", true, false, false,
							  menu_ap_script_step_cworld_nav_mode());
	/* The ring moves but Create never reports ready (disabled Create). */
	cworld_walk_fails_by_name("create_disabled", false, true, false, nav);
	/* The editor modal is observed open: something else owns the screen.
	 * The first DOWN is the mode probe's (T05), so that is the gate. */
	cworld_walk_fails_by_name("editor_open", false, false, true,
							  menu_ap_script_step_cworld_nav_mode());
}

static void test_creative_target_creates_creative_world(void) {
	/* The deliberate hardware reproduction knob (--cw-cworld-mode creative /
	 * HW_RUN_PROFILE_MENUS_DWELL_CREATIVE, source/game/hw_run.h): with
	 * target_creative=true, STEP_CWORLD_MODE_CONFIRM must confirm CREATIVE
	 * (not press back to Survival) before Create, so the world this profile
	 * creates is Creative on purpose -- proving the same selector loop that
	 * staged_menus_dwell_20260902_063552 hit by accident can be reproduced
	 * intentionally. */
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/true);
	menu_ap_script_enable_pack_legs(&s, true); /* capture-run opt-in: three pack applies */
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);

	int used = drive(&s, &m, &log, 20 * 600);

	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(m.screen == MENU_AP_SCREEN_TITLE); /* resumed from pause, then Save and quit settled */
	assert(!m.home_pressed_on_title);
	assert(!m.pointer_target_off_sworld);

	/* The console screen's contract for THIS target: the selector reached
	 * Creative (as it always does, en route to the capture dwell) and
	 * STAYED there through Create -- zero GUI_LEFT presses needed, since the
	 * confirm step's very first check already agrees with the target. */
	assert(!m.cworld_modal);
	assert(m.cworld_saw_creative);
	assert(m.cworld_mode == 1);
	assert(m.cworld_created_creative);
	assert(log_count_button_outside_leg(&log, IB_GUI_LEFT) == 0);
	/* Same button-count contract as the happy path otherwise: pressing
	 * toward Creative and confirming it needs no MORE RIGHTs than the
	 * survival-target run's single MODE_CREATIVE hop already contributed. */
	assert(log_count_button_outside_leg(&log, IB_GUI_RIGHT) == 11); /* + 5 inventory focus moves (menus-dwell-walker-1026: -5 old OSK RIGHTs) */
	assert(log_count_button_outside_leg(&log, IB_GUI_DOWN) == 16); /* + pack row + quit walk x4 (menus-dwell-walker-1026: observed walks); title-nav DOWN x2 past Tutorial world (ui-build-1026 T03) */
	assert(log_count_button_outside_leg(&log, IB_GUI_CLICK) == 17); /* + 3 pack applies + Save and quit + 2 hold-leg (no name-editor clicks) */

	printf("[test_creative_target_creates_creative_world] PASS (ticks=%d, "
		   "max_step=%d/%d)\n",
		   used, s.max_step, menu_ap_script_step_count() - 1);
}

/* ui-build-1026 T03: the five-row title (Play / Tutorial world / Texture
 * packs / Options / Quit, clamped). From EVERY row, the walk's own decision
 * reaches Play, Tutorial and Texture packs within MENU_AP_FOCUS_WALK_MAX
 * presses, never overshooting (the review's Packs -> Tutorial -> Packs
 * bounce is exactly an overshoot). */
static void test_title_walks_five_rows(void) {
	const int targets[] = {MENU_AP_TITLE_PLAY, MENU_AP_TITLE_TUTORIAL,
						   MENU_AP_TITLE_PACKS};
	for(int start = MENU_AP_TITLE_PLAY; start <= MENU_AP_TITLE_QUIT; start++) {
		for(size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
			int focus = start, presses = 0, button;
			while((button = menu_ap_focus_walk_button(focus, targets[t]))
				  != -1) {
				assert(presses < MENU_AP_FOCUS_WALK_MAX
					   && "a title walk ran out of presses");
				if(button == IB_GUI_DOWN && focus < MENU_AP_TITLE_QUIT)
					focus++;
				else if(button == IB_GUI_UP && focus > MENU_AP_TITLE_PLAY)
					focus--;
				else
					assert(!"a title walk pressed into a clamp");
				presses++;
			}
			assert(focus == targets[t]);
			int distance = start > targets[t] ? start - targets[t]
											  : targets[t] - start;
			assert(presses == distance && "a title walk took a detour");
		}
	}
	puts("PASS title walks: every row of 5 reaches Play / Tutorial / Packs "
		 "in exactly its distance (<= MENU_AP_FOCUS_WALK_MAX)");
}

/* ui-build-1026 T15: the Tutorial world row is VISITED on the way to Texture
 * packs -- walked onto by observation, held for a full dwell -- and never
 * activated (its activate loads the shipped tutorial world). The visit adds
 * no presses: Play -> Tutorial -> Texture packs is still DOWN x2. */
static void test_title_tutorial_visit(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(s.title_tutorial_seen);
	assert(m.tutorial_clicks == 0 && "the walker activated the Tutorial row");
	/* The dwell itself issues no press. */
	for(size_t k = 0; k < log.count; k++)
		assert(log.steps[k] != menu_ap_script_step_title_tutorial_dwell()
			   || log.buttons[k] == IB_COUNT);
	printf("[test_title_tutorial_visit] PASS (ticks=%d)\n", used);
}

/* ...and a focus that leaves Tutorial mid-dwell ends the run BY NAME at the
 * dwell, before Texture packs, with nothing activated. */
static void test_title_tutorial_drift_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.tutorial_drift = 5;
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_walk_failed());
	assert(s.walk_failed_step == menu_ap_script_step_title_tutorial_dwell());
	assert(s.map_fail == menu_ap_script_step_title_tutorial_dwell() + 1);
	assert(!s.title_tutorial_seen && m.tutorial_clicks == 0);
	assert(!m.texpacks_seen && m.screen == MENU_AP_SCREEN_TITLE);
	printf("[test_title_tutorial_drift_fails_by_name] PASS (ticks=%d, gate=%d)\n",
		   used, s.walk_failed_step);
}

/* ui-build-1026 T15: the Texture packs one-click pick and the Create World
 * sub-legs, on the happy path: one pointer click selected the other row
 * (never applied), the Details card opened and closed, each editor opened on
 * its own profile, took ONE key, and Cancel restored the original text. */
static void test_t15_new_screens_happy(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);
	unsigned committed = m.cworld_committed;
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(s.texpacks_pick_seen && s.texpacks_pick_target == 1);
	assert(m.texpacks_row_clicks == 1 && m.pack_applies == 0);
	assert(s.cworld_subs_done == 0x7u && !s.cworld_osk_skipped);
	assert(m.cworld_details_opens == 1 && m.cworld_osk_opens == 2);
	assert(m.cworld_keys_typed == 2 && m.cworld_committed == committed);
	assert(m.cworld_down_seen == 0 && m.cworld_right_seen == 0);
	assert(!m.pointer_target_off_sworld);
	/* T15c: Inventory -> Crafting -> one tab -> Inventory, nothing crafted. */
	assert(s.inv_pages_seen && m.inv_swaps == 2 && m.inv_tab_moves == 1);
	assert(m.craft_clicks == 0 && m.inv_tab == 1 && s.inv_tab_before == 0);
	printf("[test_t15_new_screens_happy] PASS (ticks=%d)\n", used);
}

/* A pointer click that APPLIES the pack (instead of only selecting it) ends
 * the run by name at the pick, before Select World. */
static void test_t15_pick_never_applies(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.texpacks_click_applies = true;
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600 && menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_walk_failed());
	assert(s.walk_failed_step == menu_ap_script_step_texpacks_pick() + 1);
	assert(s.map_fail == s.walk_failed_step + 1 && !s.texpacks_pick_seen);
	assert(m.screen == MENU_AP_SCREEN_TEXTUREPACKS);
	printf("[test_t15_pick_never_applies] PASS (gate=%d)\n", s.walk_failed_step);
}

/* Cancel that KEEPS the typed key, and a Details card that never opens, each
 * end the run at STEP_CWORLD_CREATE_FAILED naming the step and sub-leg, with
 * the world never created. */
static void t15_cworld_fails(const char* label, bool cancel_commits,
							 bool details_never, int want_offset, int want_sub) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.cworld_cancel_commits = cancel_commits;
	m.cworld_details_never_opens = details_never;
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600 && menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_cworld_failed());
	assert(s.cworld_failed_step
		   == menu_ap_script_step_cworld_sub_begin() + want_offset);
	assert(s.cworld_sub == want_sub);
	assert(s.max_step < menu_ap_script_step_wait_ingame());
	assert(m.screen == MENU_AP_SCREEN_CREATE_WORLD);
	printf("[test_t15_cworld_fails/%s] PASS (gate=%d sub=%d)\n", label,
		   s.cworld_failed_step, s.cworld_sub);
}

static void test_t15_cworld_fails_by_name(void) {
	/* BEGIN +0, WALK +1, ARM +2, ACTIVATE +3, WAIT_IN +4, DWELL +5, KEY +6,
	 * WAIT_KEY +7, CLOSE +8, WAIT_OUT +9, NEXT +10. */
	t15_cworld_fails("cancel_commits", true, false, 9,
					 MENU_AP_CWORLD_SUB_NAME_OSK);
	t15_cworld_fails("details_never_opens", false, true, 4,
					 MENU_AP_CWORLD_SUB_DETAILS);
}

/* Where a real keyboard owns the editor (PC), the two OSK sub-legs are
 * skipped BY NAME; Details still runs and the run still completes. */
static void test_t15_osk_skipped_on_pc(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	menu_ap_script_set_text_input_owns_close(&s, true);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600);
	assert(s.max_step >= menu_ap_script_step_required_for_pass());
	assert(s.cworld_osk_skipped && s.cworld_subs_done == 0x1u);
	assert(m.cworld_osk_opens == 0 && m.cworld_details_opens == 1);
	printf("[test_t15_osk_skipped_on_pc] PASS (ticks=%d)\n", used);
}

/* T15c: a swap that never lands ends the run BY NAME at the wait, before
 * the inventory is closed, with nothing crafted. */
static void test_t15_inventory_swap_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.inv_swap_dead = true;
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600 && menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_walk_failed());
	assert(s.walk_failed_step == menu_ap_script_step_inv_pages() + 1);
	assert(m.screen == MENU_AP_SCREEN_INVENTORY && m.craft_clicks == 0);
	assert(!s.inv_pages_seen);
	printf("[test_t15_inventory_swap_fails_by_name] PASS (gate=%d)\n",
		   s.walk_failed_step);
}

/* Sol T15 review (blocking): the ring leaving Apply mid-dwell -- selection
 * and pending unchanged -- fails BY NAME at the dwell and never earns
 * texpacks_pick_seen. */
static void test_t15_pick_focus_drift_fails_by_name(void) {
	struct menu_ap_state s;
	struct mock_engine m;
	struct mock_click_log log;
	menu_ap_script_reset(&s, /*target_creative=*/false);
	mock_reset(&m, /*sign_autopilot_would_open=*/true);
	m.texpacks_focus_drift = 10; /* inside the 3 s dwell (60 ticks) */
	log_reset(&log);
	int used = drive(&s, &m, &log, 20 * 600);
	assert(used < 20 * 600 && menu_ap_script_finished(&s));
	assert(s.step == menu_ap_script_step_walk_failed());
	assert(s.walk_failed_step == menu_ap_script_step_texpacks_pick() + 2);
	assert(s.map_fail == s.walk_failed_step + 1);
	assert(!s.texpacks_pick_seen && m.pack_applies == 0);
	assert(m.texpacks_sel == 1 && m.texpacks_pending == 1);
	printf("[test_t15_pick_focus_drift_fails_by_name] PASS (gate=%d)\n",
		   s.walk_failed_step);
}

int main(void) {
	test_title_walks_five_rows();
	test_t15_pick_focus_drift_fails_by_name();
	test_t15_inventory_swap_fails_by_name();
	test_t15_new_screens_happy();
	test_t15_pick_never_applies();
	test_t15_cworld_fails_by_name();
	test_t15_osk_skipped_on_pc();
	test_title_tutorial_visit();
	test_title_tutorial_drift_fails_by_name();
	test_reset_is_all_zero_except_step();
	test_output_is_all_false_when_idle();
	test_holds_while_boot_cards_own_input();
	test_finished_state_is_sticky_and_inert();
	test_never_presses_home_on_title();
	test_full_happy_path_with_sign();
	test_inventory_hold_leg();
	test_no_pointer_leg();
	test_np_texpacks_scrolls_to_last_row();
	test_no_pointer_leg_fails_by_name();
	test_text_input_owns_close_marker();
	test_sub_legs_console();
	test_sub_legs_osk_platform();
	test_sub_legs_empty_lists();
	test_sub_leg_fails_by_name();
	test_sub_legs_mps_unobserved();
	test_sub_legs_long_world_list();
	test_strip_entry_does_not_scale_with_the_list();
	test_semantic_route_exhaustion_fails_by_name();
	test_nonempty_join_options_exhaustion_fails_by_name();
	test_entry_lever_starts_at_np_leg();
	test_delete_row_count_proof();
	test_confirm_not_safe_is_seen();
	test_np_flags_bits();
	test_sign_timeout_soft_skip();
	test_container_legs_soft_skip();
	test_quit_leg_soft_skip();
	test_stuck_screen_abandons_honestly();
	test_mode_select_survives_one_frame_late_press();
	test_mode_select_never_confirms_fails_distinctly();
	test_creative_target_creates_creative_world();
	test_cworld_walk_fails_by_name();
	test_pause_walk_fails_by_name();
	test_stall_guard_names_a_silent_stall();
	test_stall_guard_spares_slow_progress();
	printf("test_menu_autopilot_script: ALL PASS\n");
	return 0;
}
