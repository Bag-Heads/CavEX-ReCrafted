/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * menus-dwell-walker-1026: the menus_dwell WALKER (menu_autopilot_script.c)
 * driving the REAL screens it walks -- title, Select World, Create World (with
 * the frontend editor and form model) and the pause menu -- through the REAL
 * PLATFORM_WII input.c, on the same libogc driver doubles as
 * test_frontend_play_flow (tests/support/frontend_screen_rig.h).
 *
 * Why this exists: two real-Wii menus_dwell regressions in a row came from a
 * UI change the walker's fixed press counts did not follow, while
 * test_menu_autopilot_script's hand-written screen model stayed green:
 *   - 09-22 (b472eb040): the create-world editor became the frontend OSK,
 *     which hears only the player-0 OWNER frame; the walker opened it and
 *     could never close it (every run stalled at STEP_WAIT_INGAME, 37).
 *   - 10-05 (91bd8295c): the pause gained an Achievements row; the walker's
 *     one DOWN opened Achievements instead of Options (hw1026b stalled at
 *     STEP_WAIT_OPTIONS, 93) and its DOWN x3 to Save and quit lands on
 *     Texture packs.
 * This test has no model of those screens: it goes RED when the walker and
 * the real screens disagree.
 *
 * The frame order is production's (main.c): the walker ticks first with the
 * screens' observations (menu_autopilot.c menu_autopilot_frame_tick), the
 * current screen's update() then reads input_pressed() (which the walker's
 * output forces, exactly as menu_autopilot_force_button does), and
 * input_poll() closes the frame.
 *
 * WHAT IS REAL AND WHAT IS A DOUBLE. Real: title, Select World, Create World,
 * pause, input.c, the OSK and the walker. Doubles, each the smallest stand-in
 * for a screen this host rig cannot run: in-game (HOME opens the pause, as
 * screen_ingame does), Options (Close/HOME returns to the pause, as
 * options_leave does), Texture packs (two rows, one-click pending pick and
 * HOME/Close discards it; this double does NOT prove the pack screen), the
 * Achievements board (inert -- reaching it IS the failure), the world load
 * (a Create attempt hands in-game over), the server's unload answer
 * (screen_pause_exit_settled(true) once SRPC_UNLOAD_WORLD went out) and the
 * stats-save gate (durable: done(SAVED) at once, its no-dialog path). The
 * in-game legs (sign, inventory, containers) need a live world and are not
 * walked here; the run is cut at Create and resumed at STEP_PAUSE_OPEN.
 */

/* The rig defines DISARMED menu-autopilot doubles. This test is the armed
 * walker, so the rig's three are renamed out of the way and the real-shaped
 * ones are defined below. */
#define menu_autopilot_active fe_rig_unused_menu_autopilot_active
#define menu_autopilot_force_button fe_rig_unused_menu_autopilot_force_button
#define menu_autopilot_force_pointer fe_rig_unused_menu_autopilot_force_pointer
/* ...and the rig's inert Rename World double makes way for the REAL screen
 * (screen_rename_world.c, linked by this target): the no-pointer leg enters
 * it, and its editor is exactly the owner-frame-only class of control that
 * stalled hw1026c. */
#define screen_rename_world fe_rig_unused_screen_rename_world
#define screen_rename_world_setup fe_rig_unused_screen_rename_world_setup
#define FE_RIG_OWN_NOTICE_COMPOSE /* this suite defines its own below */
#include "support/frontend_screen_rig.h"
#undef menu_autopilot_active
#undef menu_autopilot_force_button
#undef menu_autopilot_force_pointer
#undef screen_rename_world
#undef screen_rename_world_setup
extern struct screen screen_rename_world;
void screen_rename_world_setup(const char* directory_path,
							   const char* current_name);
extern struct screen screen_multiplayer_setup_impl;

/* Optional drawing-only backdrop, absent in this headless input rig. */
struct tex_gfx texture_menu_background;

#include "game/gui/cworld_form_model.h"
#include "game/gui/play_session.h"
#include "game/gui/screen_stats_save.h"
#include "network/client_interface.h"
#include "platform/controller_events.h"
#include "platform/menu_sound.h"
#include "game/menu_autopilot_script.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern struct cworld_form screen_cworld_test_snapshot(bool* details_is_open);
#ifdef _WIN32
#include <direct.h>
#define md_mkdir(p) _mkdir(p)
#define md_rmdir(p) _rmdir(p)
#define md_chdir(p) _chdir(p)
#else
#include <unistd.h>
#define md_mkdir(p) mkdir(p, 0777)
#define md_rmdir(p) rmdir(p)
#define md_chdir(p) chdir(p)
#endif

#define MD_RUN_DIR "tmp_menus_dwell_create_flow_run"
#define MD_SAVES_DIR "saves"
#define MD_DT (1.0F / 60.0F)
/* Far more than any segment needs (the longest is ~8 dwells of 3 s): a stall
 * is a stall, not a slow pass. */
#define MD_FRAME_BUDGET (60 * 120)

/* ---- the screens this rig cannot run (see the header) ------------------- */

/* The Achievements board: inert. A walker that lands here has failed. */
struct screen screen_achievements = {0};

/* The stats-save gate's durable path: no dialog, done(SAVED) at once. */
void screen_stats_save_open(unsigned owner, unsigned mask,
							void (*done)(void*, enum stats_exit_result),
							void* user) {
	(void)owner;
	(void)mask;
	if(done)
		done(user, STATS_EXIT_SAVED);
}

/* Link-only doubles screen_pause.c references and this rig does not carry
 * (screen.c, the menu sound dispatcher, the client interface, the
 * achievements store). None is on a path a host-world pause walk reaches
 * except the sound cue, which has no observable effect here. */
void audio_menu_event(enum menu_sound_event event, ptime_t now) {
	(void)event;
	(void)now;
}
void achievements_request_rescue(uint8_t slot) { (void)slot; }
void clin_update_drain(void) { assert(false && "remote Leave in a host walk"); }
void clin_force_world_reset(void) {
	assert(false && "remote Leave in a host walk");
}
void screen_texturepacks_set_ingame(bool on) { (void)on; }
void screen_notice_compose(int x, int y, int w, int h, bool in_header) {
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)in_header;
}
const struct controller_recovery* screen_controller_recovery_state(void) {
	static const struct controller_recovery none
		= {.phase = CONTROLLER_RECOVERY_NONE};
	return &none;
}

static void md_ingame_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	if(input_pressed(IB_HOME))
		screen_set(&screen_pause);
}
static void md_options_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	if(input_pressed(IB_HOME) || input_pressed(IB_INVENTORY))
		screen_set(&screen_pause);
}
/* Test-only pack double. No Apply/reload is allowed in this rig. Geometry is
 * deliberately local; the real screen has its own test_texturepacks_screen. */
static int md_pack_selected, md_pack_focused, md_pack_pending;
static unsigned md_pack_clicks;
int screen_texpacks_row_count(void) { return 2; }
int screen_texpacks_selected_row(void) { return md_pack_selected; }
int screen_texpacks_focused_row(void) { return md_pack_focused; }
bool screen_texpacks_apply_focused(void) { return md_pack_focused == -1; }
bool screen_texpacks_apply_enabled(void) { return md_pack_pending != 0; }
bool screen_texpacks_pending_selected(void) {
	return md_pack_pending == md_pack_selected;
}
bool screen_texpacks_busy(void) { return false; }
bool screen_texpacks_row_center(int row, int width, int height, float* x,
								float* y) {
	if(row < 0 || row >= screen_texpacks_row_count())
		return false;
	*x = width / 2.0F;
	*y = height / 3.0F + row * 60.0F;
	return true;
}
static void md_texpacks_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	if(input_pressed(IB_HOME) || input_pressed(IB_INVENTORY)) {
		md_pack_selected = md_pack_focused = md_pack_pending = 0;
		screen_set(&screen_title);
		return;
	}
	/* screen_texturepacks.c: UP/DOWN move the SELECTION (and the ring) only;
	 * the no-pointer leg's hw1030i scroll walks DOWN to the last row. */
	if(input_pressed(IB_GUI_DOWN)
	   && md_pack_selected + 1 < screen_texpacks_row_count())
		md_pack_focused = ++md_pack_selected;
	if(input_pressed(IB_GUI_UP) && md_pack_selected > 0)
		md_pack_focused = --md_pack_selected;
	if(input_pressed(IB_GUI_CLICK)) {
		float x, y, angle;
		assert(input_pointer(&x, &y, &angle)
			   && "unexpected Apply click in the pack double");
		bool hit = false;
		for(int row = 0; row < screen_texpacks_row_count(); row++) {
			float cx, cy;
			assert(screen_texpacks_row_center(row, gfx_width(), gfx_height(),
											 &cx, &cy));
			if(x == cx && y == cy) {
				md_pack_selected = md_pack_pending = row;
				md_pack_focused = row ? -1 : row;
				md_pack_clicks++;
				hit = true;
			}
		}
		assert(hit && "walker pointer missed the pack row");
	}
}

/* ---- the armed walker, shaped exactly like menu_autopilot.c ------------- */

static bool md_armed;
static struct menu_ap_output md_out;
static float md_px, md_py;

bool menu_autopilot_active(void) { return md_armed; }

/* menu_autopilot.c menu_autopilot_force_button: the button the script names
 * reads as pressed this frame; every other button falls through to the real
 * merged mask. */
bool menu_autopilot_force_button(enum input_button b, bool* out) {
	if(!md_armed)
		return false;
	if(!md_out.press || md_out.button != b)
		return false;
	*out = true;
	return true;
}

/* menu_autopilot.c menu_autopilot_force_pointer: the walker owns the pointer
 * for its whole window -- unavailable unless it names a target. */
bool menu_autopilot_force_pointer(float* x, float* y, float* angle,
								  bool* available) {
	if(!md_armed)
		return false;
	if(md_out.pointer_suppressed
	   || md_out.pointer_target == MENU_AP_PT_NONE) {
		*available = false;
		return true;
	}
	*x = md_px;
	*y = md_py;
	*angle = 0.0F;
	*available = true;
	return true;
}

static struct screen* md_current(void) {
	return gstate.players[0].current_screen;
}

static enum menu_ap_screen md_classify(const struct screen* s) {
	if(s == &screen_title)
		return MENU_AP_SCREEN_TITLE;
	if(s == &screen_texturepacks)
		return MENU_AP_SCREEN_TEXTUREPACKS;
	if(s == &screen_select_world)
		return MENU_AP_SCREEN_SELECT_WORLD;
	if(s == &screen_create_world)
		return MENU_AP_SCREEN_CREATE_WORLD;
	if(s == &screen_ingame)
		return MENU_AP_SCREEN_INGAME;
	if(s == &screen_pause)
		return MENU_AP_SCREEN_PAUSE;
	if(s == &screen_options)
		return MENU_AP_SCREEN_OPTIONS;
	if(screen_multiplayer_setup && s == screen_multiplayer_setup)
		return MENU_AP_SCREEN_MULTIPLAYER;
	if(s == &screen_rename_world)
		return MENU_AP_SCREEN_RENAME_WORLD;
	if(s == &screen_confirm)
		return MENU_AP_SCREEN_CONFIRM;
	return MENU_AP_SCREEN_OTHER;
}

/* menu_autopilot.c menu_autopilot_frame_tick's observation, for the screens
 * this rig runs for real. */
static struct menu_ap_observation md_observe(void) {
	struct menu_ap_observation obs = {0};
	obs.cworld_mode_survival = screen_cworld_mode_is_survival();
	obs.sworld_focus = screen_sworld_focus_target();
	obs.sworld_rows = screen_sworld_row_count();
	obs.cworld_editing = screen_cworld_editing();
	obs.cworld_create_ready = screen_cworld_create_ready();
	obs.cworld_focus = screen_cworld_focus_target();
	obs.cworld_details_open = screen_cworld_details_open();
	obs.cworld_osk_profile = screen_cworld_osk_profile();
	obs.cworld_osk_target = screen_cworld_osk_target();
	obs.cworld_text_sig = screen_cworld_text_signature();
	obs.texpacks_rows = screen_texpacks_row_count();
	obs.texpacks_selected = screen_texpacks_selected_row();
	obs.texpacks_focused_row = screen_texpacks_focused_row();
	obs.texpacks_apply_focused = screen_texpacks_apply_focused();
	obs.texpacks_apply_enabled = screen_texpacks_apply_enabled();
	obs.texpacks_pending_selected = screen_texpacks_pending_selected();
	obs.texpacks_busy = screen_texpacks_busy();
	obs.texpacks_pick_row = -1;
	if(md_current() == &screen_texturepacks) {
		const int sel = obs.texpacks_selected;
		const int cand[3] = {sel + 1, sel - 1, sel};
		float x, y;
		for(int k = 0; k < 3 && sel >= 0; k++)
			if(screen_texpacks_row_center(cand[k], gfx_width(), gfx_height(),
											 &x, &y)) {
				obs.texpacks_pick_row = cand[k];
				break;
			}
	}
	obs.pause_focus = screen_pause_focused_action();
	obs.title_focus = screen_title_focused_row();
	/* menu_autopilot.c reaches these through the registration hook main.c
	 * fills when the MP screens are linked; they are linked here. */
	obs.mps_focus = screen_mps_focus_target();
	obs.mps_view = screen_mps_view();
	obs.mps_rows = screen_mps_server_count();
	obs.mps_observed = true;
	obs.confirm_focus_safe = screen_confirm_focus_is_safe();
	return obs;
}

struct md_run {
	struct menu_ap_state s;
	int frames;
	unsigned create_attempts;
	unsigned create_attempts0;
	bool ever_editing;
	bool saw_achievements;
	bool world_loaded; /* the world-load double handed in-game over */
	/* Independent evidence from the REAL screen before each walker tick. */
	unsigned cw_steps[MENU_AP_CWORLD_SUB_COUNT];
	unsigned cw_inside[MENU_AP_CWORLD_SUB_COUNT];
	unsigned cw_keys[MENU_AP_CWORLD_SUB_COUNT];
	unsigned cw_sig[MENU_AP_CWORLD_SUB_COUNT];
	unsigned cw_changed, cw_restored;
	int fault, fault_sub;
	bool fault_fired;
	int trace_step, trace_focus; /* MD_TRACE=1 diagnostics */
};

enum md_fault { MD_NO_FAULT, MD_WRONG_DEST, MD_MISSING_OPEN,
				MD_MISSING_KEY, MD_MISSING_CLOSE };

/* Offsets in the hand-counted 36..46 sub-leg block; the contract test pins
 * its start independently. This evidence never trusts subs_done alone. */
static bool md_cworld_complete(const struct md_run* r) {
	if(r->s.cworld_subs_done != 7u || r->s.cworld_osk_skipped
	   || r->cw_changed != 6u || r->cw_restored != 7u)
		return false;
	for(int sub = 0; sub < MENU_AP_CWORLD_SUB_COUNT; sub++) {
		unsigned steps = sub == MENU_AP_CWORLD_SUB_DETAILS ? 0x73Fu : 0x7FFu;
		if(r->cw_steps[sub] != steps || r->cw_inside[sub] < 180u
		   || r->cw_keys[sub] != (sub == MENU_AP_CWORLD_SUB_DETAILS ? 0u : 1u))
			return false;
	}
	return true;
}

/* ONE production frame. */
static void md_frame(struct md_run* r) {
	struct menu_ap_observation obs = md_observe();
	int old_step = r->s.step;
	int sub = r->s.cworld_sub;
	int cw_step = old_step - menu_ap_script_step_cworld_sub_begin();
	bool cw_leg = cw_step >= 0 && cw_step <= 10;
	if(cw_leg) {
		assert(sub >= 0 && sub < MENU_AP_CWORLD_SUB_COUNT);
		r->cw_steps[sub] |= 1u << (unsigned)cw_step;
		if(cw_step == 0)
			r->cw_sig[sub] = obs.cworld_text_sig;
		if(cw_step == 5) {
			assert(md_current() == &screen_create_world);
			if(sub == MENU_AP_CWORLD_SUB_DETAILS)
				assert(obs.cworld_details_open && !obs.cworld_editing
					   && obs.cworld_focus == MENU_AP_CWORLD_DETAILS_BACK);
			else
				assert(obs.cworld_editing && !obs.cworld_details_open
					   && obs.cworld_osk_profile == (sub == MENU_AP_CWORLD_SUB_NAME_OSK
							? MENU_AP_OSK_PROFILE_CREATE_NAME : MENU_AP_OSK_PROFILE_CREATE_SEED)
					   && obs.cworld_osk_target == MENU_AP_OSK_CHAR_FIRST);
			r->cw_inside[sub]++;
		}
		if(cw_step == 8 && sub != MENU_AP_CWORLD_SUB_DETAILS
		   && obs.cworld_text_sig != r->cw_sig[sub])
			r->cw_changed |= 1u << (unsigned)sub;
		if(cw_step == 10 && !obs.cworld_editing && !obs.cworld_details_open
		   && obs.cworld_text_sig == r->cw_sig[sub]
		   && obs.cworld_focus == (sub == MENU_AP_CWORLD_SUB_DETAILS
				? MENU_AP_CWORLD_DETAILS : sub == MENU_AP_CWORLD_SUB_NAME_OSK
				? MENU_AP_CWORLD_NAME : MENU_AP_CWORLD_SEED))
			r->cw_restored |= 1u << (unsigned)sub;
	}
	r->ever_editing |= obs.cworld_editing;
	(void)menu_ap_script_tick(&r->s, md_classify(md_current()), obs, MD_DT,
							  &md_out);
	if(getenv("MD_TRACE") && (r->s.step != r->trace_step || obs.sworld_focus != r->trace_focus || md_out.press)) {
		fprintf(stderr, "T pf=%d step=%d sub=%d screen=%d sfocus=%d mfocus=%d view=%d press=%d btn=%d stage=%d\n", obs.pause_focus, r->s.step, r->s.np_sub, (int)md_classify(md_current()), obs.sworld_focus, obs.mps_focus, obs.mps_view, (int)md_out.press, (int)md_out.button, (int)r->s.np_parent_stage);
		r->trace_step = r->s.step; r->trace_focus = obs.sworld_focus;
	}
	if(md_out.pointer_target == MENU_AP_PT_SWORLD_CREATE)
		assert(screen_sworld_create_button_center(gfx_width(), gfx_height(),
													  &md_px, &md_py));
	else if(md_out.pointer_target == MENU_AP_PT_TEXPACKS_ROW)
		assert(screen_texpacks_row_center(md_out.pointer_row, gfx_width(),
											 gfx_height(), &md_px, &md_py));
	bool inject = cw_leg && sub == r->fault_sub && r->fault != MD_NO_FAULT
		&& md_out.press && ((cw_step == 3 && (r->fault == MD_WRONG_DEST
			|| r->fault == MD_MISSING_OPEN))
			|| (cw_step == 6 && r->fault == MD_MISSING_KEY)
			|| (cw_step == 8 && r->fault == MD_MISSING_CLOSE));
	if(inject) {
		r->fault_fired = true;
		if(r->fault != MD_WRONG_DEST)
			md_out.press = false;
	}
	if(cw_leg && cw_step == 6 && md_out.press)
		r->cw_keys[sub]++;
	struct screen* cur = md_current();
	if(cur->update)
		cur->update(cur, MD_DT);
	if(inject && r->fault == MD_WRONG_DEST)
		screen_set(&screen_options);
	r->saw_achievements |= md_current() == &screen_achievements;
	/* The server's unload answer (client_interface WORLD_RESET -> settle). */
	if(screen_pause_exit_pending() && fe_rpc_last == SRPC_UNLOAD_WORLD)
		screen_pause_exit_settled(true);
	fe_idle(); /* input_poll() at the bottom of the frame */
	r->create_attempts = fe_world_create_attempts - r->create_attempts0;
}

static void md_begin(struct md_run* r, struct screen* start, int step) {
	memset(r, 0, sizeof(*r));
	md_pack_selected = md_pack_focused = md_pack_pending = 0;
	md_pack_clicks = 0;
	play_session_clear_draft();
	screen_ingame.update = md_ingame_update;
	screen_options.update = md_options_update;
	screen_texturepacks.update = md_texpacks_update;
	screen_set(start);
	fe_idle();
	fe_rpc_last = -1;
	menu_ap_script_reset(&r->s, /*target_creative=*/false);
	if(step >= 0)
		r->s.step = r->s.max_step = step;
	r->create_attempts0 = fe_world_create_attempts;
	md_armed = true;
}

static void md_end(void) {
	md_armed = false;
	md_out = (struct menu_ap_output) {0};
}

static void md_report(const char* label, const struct md_run* r) {
	fprintf(stderr,
			"[%s] step=%d max=%d finished=%d screen=%d create_attempts=%u "
			"ever_editing=%d achievements=%d cworld_gate=%d cworld_sub=%d walk_gate=%d "
			"np_failed=%d frames=%d\n",
			label, r->s.step, r->s.max_step, (int)r->s.finished,
			(int)md_classify(md_current()), r->create_attempts,
			(int)r->ever_editing, (int)r->saw_achievements,
			r->s.cworld_failed_step, r->s.cworld_sub, r->s.walk_failed_step,
			r->s.np_failed_step, r->frames);
}

/* ---- the Create World walk (from STEP_WAIT_CWORLD) ---------------------- */

static void md_expect_created(const char* label) {
	struct md_run r;
	md_begin(&r, &screen_create_world, menu_ap_script_step_cworld_entry());
	for(r.frames = 0; r.frames < MD_FRAME_BUDGET; r.frames++) {
		md_frame(&r);
		if(r.create_attempts || r.s.finished)
			break;
	}
	/* A few more frames: the walker must now be WAITING for the world and
	 * must not press Create again (the rig's create always fails, so a
	 * second press would be a second attempt). */
	for(int k = 0; k < 120 && !r.s.finished; k++)
		md_frame(&r);
	md_end();
	struct cworld_form form = screen_cworld_test_snapshot(NULL);
	if(r.create_attempts != 1
	   || r.s.step != menu_ap_script_step_wait_ingame())
		md_report(label, &r);
	assert(r.create_attempts == 1
		   && "the walker never actioned Create on the real Create World screen");
	assert(r.s.step == menu_ap_script_step_wait_ingame() && !r.s.finished
		   && "after Create the walker must be waiting for the world");
	assert(r.frames < MD_FRAME_BUDGET && "Create walk timed out");
	assert(r.ever_editing && !form.editing && md_cworld_complete(&r)
		   && "Create passed before Details and both OSKs were proven");
	assert(!screen_cworld_details_open());
	/* What Create submitted: Beta's default name, Survival (the default
	 * target, after the capture hop to Creative and back). */
	assert(strcmp(form.name, "New World") == 0);
	assert(form.seed[0] == '\0');
	assert(form.mode == CWORLD_MODE_SURVIVAL);
	assert(form.focus == CWORLD_ROW_CREATE);
	printf("PASS walker actions Create on the real screen (%s, %d frames)\n",
		   label, r.frames);
	/* The evidence check must reject an early pass even if the walker's own
	 * completion mask is already set: each observed step/dwell/key matters. */
	struct md_run early = r;
	early.s.finished = true;
	early.s.max_step = menu_ap_script_step_required_for_pass();
	for(int sub = 0; sub < MENU_AP_CWORLD_SUB_COUNT; sub++) {
		for(unsigned bit = 0; bit < 11; bit++) {
			if(!(r.cw_steps[sub] & (1u << bit)))
				continue;
			early.cw_steps[sub] = r.cw_steps[sub] & ~(1u << bit);
			assert(!md_cworld_complete(&early));
		}
		early = r;
		early.cw_inside[sub] = 179;
		assert(!md_cworld_complete(&early));
		early = r;
	}
}

/* Drop a real transition/key/Close, or send it to the wrong screen. Each
 * must terminate by its own named gate, below PASS, within the frame bound. */
static void case_create_failures(void) {
	input_init();
	for(int sub = 0; sub < MENU_AP_CWORLD_SUB_COUNT; sub++) {
		for(int fault = MD_WRONG_DEST; fault <= MD_MISSING_CLOSE; fault++) {
			if(sub == MENU_AP_CWORLD_SUB_DETAILS && fault == MD_MISSING_KEY)
				continue;
			struct md_run r;
			md_begin(&r, &screen_create_world, menu_ap_script_step_cworld_entry());
			r.fault = fault;
			r.fault_sub = sub;
			for(r.frames = 0; r.frames < MD_FRAME_BUDGET && !r.s.finished;
				r.frames++)
				md_frame(&r);
			md_end();
			int offset = fault == MD_MISSING_KEY ? 7
				: fault == MD_MISSING_CLOSE ? 9 : 4;
			if(!r.s.finished || r.s.cworld_failed_step
			   != menu_ap_script_step_cworld_sub_begin() + offset)
				md_report("injected Create failure", &r);
			assert(r.fault_fired && r.frames < MD_FRAME_BUDGET);
			assert(r.s.finished && !r.s.stalled && r.create_attempts == 0);
			assert(r.s.step == menu_ap_script_step_cworld_failed());
			assert(r.s.cworld_sub == sub);
			assert(r.s.cworld_failed_step
				   == menu_ap_script_step_cworld_sub_begin() + offset);
			assert(r.s.map_fail == r.s.cworld_failed_step + 1);
			assert(r.s.max_step < menu_ap_script_step_required_for_pass());
			assert(!md_cworld_complete(&r));
		}
	}
	printf("PASS wrong Create destinations, missing opens/keys/Close fail by name\n");
}

/* The real rig: no controller claims player 0. */
static void case_walker_creates_without_owner(void) {
	input_init();
	assert(!input_player_frame(0));
	md_expect_created("no controller claimed");
}

/* A human-attended run: a family owns player 0. */
static void case_walker_creates_with_owner(enum fe_family family) {
	fe_attach(family);
	md_expect_created(fe_family_name[family]);
}

/* ---- the front end from the TOP of the script --------------------------- */

/* Step 0 on the real title: Texture packs (by the title walk), back, Play,
 * Select World's Create New World by the walker's synthetic pointer, Create
 * World by the observed walk -- then the world-load double hands in-game
 * over and the walker must settle there. */
static void case_front_end_from_step_zero(void) {
	input_init();
	assert(!input_player_frame(0));
	struct md_run r;
	md_begin(&r, &screen_title, -1);
	bool saw_packs = false;
	for(r.frames = 0; r.frames < MD_FRAME_BUDGET; r.frames++) {
		md_frame(&r);
		saw_packs |= md_current() == &screen_texturepacks;
		if(r.create_attempts && !r.world_loaded) {
			r.world_loaded = true;
			screen_set(&screen_ingame); /* the world-load double */
		}
		if(r.s.finished
		   || (r.world_loaded
			   && r.s.step > menu_ap_script_step_wait_ingame()))
			break;
	}
	md_end();
	if(!(saw_packs && r.create_attempts == 1 && !r.s.finished
		 && r.s.step > menu_ap_script_step_wait_ingame()))
		md_report("front end from step 0", &r);
	assert(saw_packs && "the title walk never opened Texture packs");
	assert(r.frames < MD_FRAME_BUDGET);
	assert(r.create_attempts == 1 && r.ever_editing && md_cworld_complete(&r));
	assert(r.s.title_tutorial_seen && r.s.texpacks_pick_seen);
	assert(md_pack_clicks == 1 && md_pack_pending == 0);
	assert(!r.s.finished && r.s.step > menu_ap_script_step_wait_ingame()
		   && "the walker did not reach in-game after Create");
	printf("PASS front end from step 0 on the real title/Select World/Create "
		   "World (%d frames, step %d)\n",
		   r.frames, r.s.step);
}

/* ---- the pause legs (from STEP_PAUSE_OPEN) ------------------------------ */

/* HOME -> the REAL pause; walk to Options; Options and back; resume; HOME
 * again; walk to Save and quit; the unload settles on the REAL title; then
 * the no-pointer leg's first title walk (to Play) opens Select World. RED on
 * the hw1026b walker: its one DOWN opened Achievements and it died at
 * STEP_WAIT_OPTIONS (93). */
static void case_pause_legs(void) {
	input_init();
	assert(!input_player_frame(0));
	gstate.remote_session = false; /* a host world: "Save and quit" */
	struct md_run r;
	md_begin(&r, &screen_ingame, menu_ap_script_step_pause_entry());
	bool saw_options = false, back_on_title = false;
	for(r.frames = 0; r.frames < MD_FRAME_BUDGET; r.frames++) {
		md_frame(&r);
		saw_options |= md_current() == &screen_options;
		back_on_title |= r.s.quit_reached;
		if(r.s.finished
		   || (back_on_title && md_current() == &screen_select_world))
			break;
	}
	md_end();
	bool ok = saw_options && !r.saw_achievements && r.s.quit_reached
		&& !r.s.quit_failed && !r.s.finished
		&& md_current() == &screen_select_world;
	if(!ok)
		md_report("pause legs", &r);
	assert(!r.saw_achievements
		   && "the pause walk opened Achievements (hw1026b: step 93 stall)");
	assert(saw_options && "the pause walk never reached Options");
	assert(r.s.step != menu_ap_script_step_wait_options()
		   && "the walker is stuck at STEP_WAIT_OPTIONS (93)");
	assert(r.s.quit_reached && !r.s.quit_failed
		   && "Save and quit was never actioned (DOWN x3 lands on Texture packs)");
	assert(!r.s.finished && md_current() == &screen_select_world
		   && "the no-pointer leg's title walk to Play did not open Select World");
	assert(r.frames < MD_FRAME_BUDGET);
	printf("PASS pause legs on the real pause: Options, resume, Save and quit, "
		   "title -> Play (%d frames)\n",
		   r.frames);
}

/* ---- the no-pointer leg, to the PASS ordinal ---------------------------- */

/* From the title, pointer suppressed throughout: Select World's Create /
 * Rename / Delete sub-legs on a one-world card (the real Select World, Create
 * World, Rename World and confirm screens), Texture packs (double), then the
 * REAL Multiplayer screen's Direct / Add / Edit / player-name sub-legs, back
 * to the title and STEP_NP_LEG_DONE (161). RED on the hw1026c build: its
 * Direct Connect form heard only the owner frame, the walker's Close never
 * arrived and the leg died at STEP_NP_SUB_WAIT_OUT (122) -> STEP_NP_FAILED
 * (107), with no named marker on the tape. */
static void case_no_pointer_leg_to_pass(void) {
	input_init();
	assert(!input_player_frame(0));
	assert(md_mkdir(MD_SAVES_DIR "/world00") == 0 || errno == EEXIST);
	screen_multiplayer_setup = &screen_multiplayer_setup_impl;
	struct md_run r;
	md_begin(&r, &screen_title, -1);
	menu_ap_script_start_at_np_leg(&r.s);
	for(r.frames = 0; r.frames < 3 * MD_FRAME_BUDGET; r.frames++) {
		md_frame(&r);
		if(r.s.finished)
			break;
	}
	md_end();
	screen_multiplayer_setup = NULL;
	md_rmdir(MD_SAVES_DIR "/world00");
	const unsigned all_but_edit = 0x7Fu & ~(1u << MENU_AP_SUB_MPS_EDIT);
	bool ok = r.s.np_leg_done && r.s.max_step >= menu_ap_script_step_required_for_pass()
		&& (r.s.np_flags & ~(unsigned)MENU_AP_NP_FLAG_ENTRY_LEVER) == 0
		&& (r.s.np_entered & all_but_edit) == all_but_edit && !r.s.map_fail;
	if(!ok) {
		md_report("no-pointer leg", &r);
		fprintf(stderr,
				"[no-pointer leg] entered=0x%02x skipped=0x%02x focus=0x%02x "
				"flags=0x%02x rows=%d failed_step=%d failed_sub=%d "
				"map_fail=%d stalled=%d view=%d\n",
				r.s.np_entered, r.s.np_skipped, r.s.np_focus_reached,
				r.s.np_flags, r.s.np_rows, r.s.np_failed_step,
				r.s.np_failed_sub, r.s.map_fail, (int)r.s.stalled,
				screen_mps_view());
	}
	assert(!r.s.map_fail && "the walker abandoned the no-pointer leg");
	assert(r.frames < 3 * MD_FRAME_BUDGET && r.s.finished);
	assert((r.s.np_flags & (unsigned)MENU_AP_NP_FLAG_LEG_FAILED) == 0);
	assert(r.s.np_leg_done
		   && r.s.max_step >= menu_ap_script_step_required_for_pass()
		   && "the no-pointer leg never reached its PASS ordinal (161)");
	/* What the console gate (tools/wii_telemetry_server.py) demands: every
	 * always-enabled target ENTERED, Rename/Delete entered on a card with a
	 * world, no flag but this test's own entry lever. Edit has no saved
	 * server here and is a named row-gated skip. */
	assert((r.s.np_entered & all_but_edit) == all_but_edit
		   && "a no-pointer target was not entered");
	assert(r.s.np_skipped == (1u << MENU_AP_SUB_MPS_EDIT));
	assert((r.s.np_entered & ~r.s.np_focus_reached) == 0);
	assert((r.s.np_flags & ~(unsigned)MENU_AP_NP_FLAG_ENTRY_LEVER) == 0);
	printf("PASS no-pointer leg to step %d on the real Select World / Create / "
		   "Rename / confirm / Multiplayer screens (%d frames, entered=0x%02x "
		   "skipped=0x%02x)\n",
		   r.s.max_step, r.frames, r.s.np_entered, r.s.np_skipped);
}

/* ---- the root cause of the create-world stall, pinned ------------------- */

/* THE SEAM (hw1026c): while the walker is armed, its presses reach the
 * frontend editor through osk_frontend_input_frame, with no owner assigned.
 * Before it, the walker could open an editor it could never close (09-22:
 * Create World's name; 10-26: Direct Connect). Opened by one forced activate
 * on Name, closed by the editor's own Close role. */
static void case_walker_presses_reach_editor(void) {
	input_init();
	assert(!input_player_frame(0));
	play_session_clear_draft();
	screen_set(&screen_create_world);
	fe_idle();
	md_armed = true;
	md_out = (struct menu_ap_output) {.press = true, .button = IB_GUI_CLICK};
	screen_create_world.update(&screen_create_world, MD_DT);
	fe_idle();
	md_out = (struct menu_ap_output) {0};
	screen_create_world.update(&screen_create_world, MD_DT);
	fe_idle();
	struct cworld_form f = screen_cworld_test_snapshot(NULL);
	assert(f.editing && "a forced activate on Name did not open the editor");
	md_out = (struct menu_ap_output) {.press = true, .button = IB_INVENTORY};
	screen_create_world.update(&screen_create_world, MD_DT);
	fe_idle();
	md_out = (struct menu_ap_output) {0};
	f = screen_cworld_test_snapshot(NULL);
	assert(!f.editing
		   && "the walker's Close never reached the frontend editor");
	assert(strcmp(f.name, "New World") == 0);
	md_end();
	/* Disarmed, the editor is player 1's alone again. */
	assert(!menu_autopilot_active());
	printf("PASS walker presses reach the frontend editor while armed\n");
}

int main(int argc, char** argv) {
	/* Rig helpers this driver has no use for (draw observer, rename double). */
	(void)fe_render;
	(void)fe_hint_drawn;
	(void)fe_rename_forget;
	/* Same scratch discipline as test_frontend_play_flow: the Create path
	 * stat()s the saves directory and the screens can reach config_save with
	 * a relative path, so everything happens in a scratch directory under
	 * argv[1] (the BUILD tree under ctest), never the source root. */
	FILE* shipped_config = fopen("config_wii.json", "rb");
	assert(shipped_config
		   && "config_wii.json missing; is the CWD the source root?");
	if(argc > 1)
		assert(md_chdir(argv[1]) == 0
			   && "could not enter the scratch parent (argv[1])");
	remove(MD_RUN_DIR "/config.json");
	remove(MD_RUN_DIR "/config_wii.json");
	md_rmdir(MD_RUN_DIR "/" MD_SAVES_DIR);
	md_rmdir(MD_RUN_DIR);
	assert((md_mkdir(MD_RUN_DIR) == 0 || errno == EEXIST)
		   && "could not create the scratch run directory");
	{
		FILE* out = fopen(MD_RUN_DIR "/config_wii.json", "wb");
		assert(out);
		char buf[4096];
		size_t n;
		while((n = fread(buf, 1, sizeof(buf), shipped_config)) > 0)
			assert(fwrite(buf, 1, n, out) == n);
		fclose(shipped_config);
		fclose(out);
	}
	assert(md_chdir(MD_RUN_DIR) == 0 && "could not enter the scratch run dir");
	assert(md_mkdir(MD_SAVES_DIR) == 0 || errno == EEXIST);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	assert(config_set_string(&gstate.config_user, "paths.worlds",
							 MD_SAVES_DIR));
	assert(config_set_string(&gstate.config_user, "mp.username", ""));

	/* The pause legs first: the hw1026b failure (step 93) is the newest. */
	case_pause_legs();
	case_front_end_from_step_zero();
	case_walker_presses_reach_editor();
	case_no_pointer_leg_to_pass();
	case_walker_creates_without_owner();
	case_create_failures();
	for(int family = 0; family < FE_FAMILY_COUNT; family++)
		case_walker_creates_with_owner((enum fe_family)family);

	config_destroy(&gstate.config_user);
	remove("config.json");
	remove("config_wii.json");
	md_rmdir(MD_SAVES_DIR);
	assert(md_chdir("..") == 0);
	md_rmdir(MD_RUN_DIR);
	printf("test_menus_dwell_create_flow: ALL PASS\n");
	return 0;
}
