/* Lane 5 -- the APPROVED pause composition.
 *
 * Authority: docs/ui_design/approved/menus.html (menu selector -> Pause) and
 * docs/ui_design/approved/specs/menus.md ("Pause uses the approved small set
 * of meaningful actions. Show the assigned controller's detected name; no My
 * controller chooser. In split-screen, local pause does not pretend the world
 * is stopped. Host save/quit and guest leave preserve ownership."), plus the
 * cue row of specs/sound-events.md ("Existing category cue when a menu
 * actually opens or changes; existing scroll on changed user selection; Back
 * on successful return/resume.").
 *
 * The REAL screen_pause.c runs here: only the device/graphics/transport
 * boundary is replaced, so the action model, the geometry, the RPC pair and
 * the cue timing under test are the shipping ones. The lane-3 recovery gate
 * is exercised by test_input_wii_adapter, which links this same file and stays
 * green -- this test deliberately does not duplicate it, but it does prove the
 * one coupling the layout work could have broken: while recovery is active,
 * Resume is withheld and quit still works. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/game_state.h"
#include "game/gui/menu_header.h"
#include "game/gui/screen.h"
#include "game/gui/screen_stats_save.h"
#include "graphics/gui_util.h"
#include "network/server_interface.h"
#include "platform/controller_events.h"
#include "platform/input_inventory.h"
#include "platform/menu_sound.h"

/* ---- boundary doubles --------------------------------------------------- */
/* This focused fixture has no statistics owner. The actual async/error gate
 * and all three quit choices are exercised by test_achievements_r2. */
void screen_stats_save_open(unsigned owner, unsigned mask,
	void (*done)(void*, enum stats_exit_result), void* user) {
	assert(owner == 0 && (mask == 1u || mask == 3u));
	done(user, STATS_EXIT_SAVED);
}
void achievements_request_rescue(uint8_t slot) { (void)slot; }
bool achievements_rescue_unsaved(uint8_t slot) { assert(slot < 2); return true; }

struct game_state gstate;
struct screen screen_ingame, screen_load_world, screen_title, screen_options, screen_chat, screen_texturepacks, screen_achievements;
struct tex_gfx texture_pointer, texture_font, texture_controls,
	texture_gui_crafting, texture_gui_widgets;

static unsigned pause_rpcs, unload_rpcs;
static struct server_rpc last_unload; /* IMP-QUIT-UNSAVABLE: its payload */
static bool reject_rpc;
static enum menu_sound_event cues[32];
static size_t cue_count;
static struct screen* current;
static bool option_return_set;

static bool pressed[IB_COUNT];
static bool pointer_on;
static float ptr_x, ptr_y;
static struct input_player_frame owner_frame;
static struct input_device owner_device;
static bool owner_device_known;
static char drawn_labels[8][32];
static int drawn_count;
static enum input_button hint_actions[2];
static int hint_count;
static struct tex_gfx* bound_texture;
static int button_quads, picture_calls, picture_y, picture_kind;
static int view_height=480;
static struct controller_recovery recovery;

bool svin_rpc_send(struct server_rpc* rpc) {
	if(reject_rpc)
		return false;
	if(rpc->type == SRPC_TOGGLE_PAUSE)
		pause_rpcs++;
	else if(rpc->type == SRPC_UNLOAD_WORLD) {
		unload_rpcs++;
		last_unload = *rpc;
	}
	else
		assert(0 && "the approved pause sends only Beta's two RPCs");
	return true;
}
/* The remote "Leave game" teardown (2026-09-12 D2b). client_interface.c is
 * deliberately not linked here, so these three record the ORDER instead --
 * which is the whole assertion: drain, then the client-side WORLD_RESET,
 * then the chip-B unload send, exactly screen_load_world.c:397-405. In the
 * app clin_force_world_reset() settles the pending exit synchronously
 * (client_interface.c:1382 -> clin_process -> the local_entity==0 arm);
 * the stub leaves it armed so the test drives that settle by hand, which is
 * also what proves the arming happened BEFORE the reset. */
enum leave_step { LEAVE_DRAIN = 1, LEAVE_RESET, LEAVE_UNLOAD };
static enum leave_step leave_seq[4];
static size_t leave_seq_count;
static void record_leave(enum leave_step step) {
	assert(leave_seq_count < 4);
	leave_seq[leave_seq_count++] = step;
}
void clin_update_drain(void) {
	record_leave(LEAVE_DRAIN);
}
void clin_force_world_reset(void) {
	record_leave(LEAVE_RESET);
}
bool clin_send_unload_world(void) {
	record_leave(LEAVE_UNLOAD);
	return true;
}
void audio_menu_event(enum menu_sound_event e, ptime_t now) {
	(void)now;
	assert(cue_count < 32);
	cues[cue_count++] = e;
}
void audio_play_ui(uint16_t sound, float volume, float pitch) {
	(void)volume;
	(void)pitch;
	(void)sound;
	/* specs/sound-events.md gives the pause menu the shared menu cues only;
	 * Beta's random.click no longer belongs here. Reaching this is the
	 * regression. */
	assert(0 && "the approved pause plays no direct UI sound");
}
/* IMP-QUIT-UNSAVABLE: screen_confirm.c is not linked here (its own
 * controller tables live in the frontend suites); record what the pause asks
 * the approved confirm dialog to show and keep its callback. */
struct screen screen_confirm;
static char confirm_lines[4][64];
static enum screen_confirm_answer confirm_safe;
static void (*confirm_answer)(bool yes);
void screen_confirm_setup(const char* msg1, const char* msg2,
						  const char* yes_label, const char* no_label,
						  enum screen_confirm_answer safe_answer,
						  void (*cb)(bool yes)) {
	snprintf(confirm_lines[0], 64, "%s", msg1);
	snprintf(confirm_lines[1], 64, "%s", msg2);
	snprintf(confirm_lines[2], 64, "%s", yes_label);
	snprintf(confirm_lines[3], 64, "%s", no_label);
	confirm_safe = safe_answer;
	confirm_answer = cb;
}
void screen_set(struct screen* s) {
	current = s;
	if(s->reset)
		s->reset(s, 640, 480);
}
void screen_options_set_return(struct screen* s) {
	(void)s;
	option_return_set = true;
}
static bool texpacks_return_set, texpacks_ingame_set;
void screen_texturepacks_set_return(struct screen* s) {
	(void)s;
	texpacks_return_set = true;
}
void screen_texturepacks_set_ingame(bool on) {
	texpacks_ingame_set = on;
}
const struct controller_recovery* screen_controller_recovery_state(void) {
	return &recovery;
}
int gutil_connection_notice_width(unsigned player,bool split,bool header){(void)player;(void)split;(void)header;return 0;}
void screen_notice_compose(int x, int y, int w, int h, bool in_header) {
 (void)in_header;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}
int gfx_width(void) {
	return 640;
}
int gfx_height(void) {
	return view_height;
}
void gfx_texture(bool enabled) {
	(void)enabled;
}
void gfx_bind_texture(struct tex_gfx* t) { bound_texture=t; }
bool input_pressed(enum input_button b) {
	return b < IB_COUNT && pressed[b];
}
bool input_held(enum input_button b) {
	return b < IB_COUNT && pressed[b];
}
void input_pointer_enable(bool enable) {
	(void)enable;
}
bool input_pointer(float* x, float* y, float* angle) {
	if(!pointer_on)
		return false;
	*x = ptr_x;
	*y = ptr_y;
	*angle = 0.0f;
	return true;
}
const struct input_player_frame* input_player_frame(unsigned player) {
	return player == 0 ? &owner_frame : NULL;
}
void input_prompt_frame(unsigned player,struct input_player_frame* out){(void)player;*out=owner_frame;}
bool input_player_device(unsigned player, struct input_device* out) {
	if(player != 0 || !owner_device_known)
		return false;
	*out = owner_device;
	return true;
}
/* Render-side text/geometry: measured, not drawn. */
int gutil_font_width(const char* str, int scale) {
	return (int)strlen(str) * scale;
}
void gutil_text(int x, int y, const char* s, int scale, bool shadow) {
	(void)x;
	(void)y;
	(void)s;
	(void)scale;
	(void)shadow;
}
void gutil_text_rgb(int x,int y,const char* s,int scale,bool shadow,uint8_t r,uint8_t g,uint8_t b) {
 (void)x;(void)y;(void)scale;(void)shadow;(void)r;(void)g;(void)b;
 if(!strcmp(s,"Back to game")||!strcmp(s,"Chat")||!strcmp(s,"Achievements")||!strcmp(s,"Options...")||!strcmp(s,"Texture packs...")||!strcmp(s,"Save and quit")||!strcmp(s,"Leave game")) {
  assert(drawn_count<8); snprintf(drawn_labels[drawn_count++],32,"%s",s);
 }
}
int gutil_controller_picture_width(int picture,int h) {
 switch(picture) {
 case CONTROLLER_PICTURE_REMOTE:return h>=48?24*h/64:40*h/24;
 case CONTROLLER_PICTURE_NUNCHUK:return h>=48?56*h/64:40*h/24;
 case CONTROLLER_PICTURE_GAMECUBE:return h>=48?96*h/64:40*h/24;
 case CONTROLLER_PICTURE_CLASSIC:return h>=48?120*h/64:40*h/24;
 default:return 0;
 }
}
void gutil_controller_picture(int x,int y,int w,int h,int picture) {
 assert(h==64 || h==24);
 assert(w==gutil_controller_picture_width(picture,h));
 int slot_x=320-(h==64?146:105),slot_w=h==64?120:40;
 assert(x>=slot_x && x+w<=slot_x+slot_w);
 assert(x+w+(h==64?8:6)==(h==64?302:261));
 picture_calls++;picture_y=y;picture_kind=picture;
}
void gutil_texquad_col(int x, int y, int tx, int ty, int sx, int sy, int w,
					   int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	(void)x; (void)y; (void)tx; (void)ty; (void)sx; (void)sy; (void)w;
	(void)h; (void)r; (void)g; (void)b; (void)a;
 if(bound_texture==&texture_gui_widgets && sy==20 && (ty==66 || ty==86)) button_quads++;
}
void gutil_texquad_rt_any(int x, int y, float angle, int tx, int ty, int sx,
						  int sy, float w, float h) {
	(void)x; (void)y; (void)angle; (void)tx; (void)ty; (void)sx; (void)sy;
	(void)w; (void)h;
}

void gutil_button_draw(int x, int y, int w_units, const char* label,
					   bool enabled, bool hover) {
	(void)x; (void)y; (void)w_units; (void)enabled; (void)hover;
	assert(drawn_count < 8);
	snprintf(drawn_labels[drawn_count++], 32, "%s", label);
}
int gutil_control_icon(int x, enum input_button b, const char* str) {
	(void)x;
	if(!strcmp(str,"Select")||!strcmp(str,"Back")) {
		assert(hint_count < 2);
		hint_actions[hint_count++] = b;
	}
	return 32;
}
/* The A1 hint row (10-31): pause's row is Move / Select / Resume; record
 * the Select and Resume roles the way the old icon stub recorded Select and
 * Back, so the role table stays pinned. */
bool gutil_hint_row_layout(const struct input_player_frame* input, int x,
	int y, int width, int height, bool split, enum gutil_hint_align align,
	const struct gutil_hint_item* items, int count, struct gutil_hint_row* out) {
	(void)input; (void)x; (void)y; (void)width; (void)height; (void)split;
	(void)align;
	memset(out, 0, sizeof *out);
	for(int k = 0; k < count && k < GUTIL_HINT_ROW_MAX; k++) {
		out->cells[k].item = k;
		out->cells[k].label = items[k].label;
		out->cells[k].glyph[0] = items[k].roles[0];
		out->count++;
	}
	return out->count > 0;
}
void gutil_hint_row_draw(const struct gutil_hint_row* row) {
	for(int k = 0; k < row->count; k++)
		if(!strcmp(row->cells[k].label, "Select")
		   || !strcmp(row->cells[k].label, "Resume")) {
			assert(hint_count < 2);
			hint_actions[hint_count++] = (enum input_button)row->cells[k].glyph[0];
		}
}
struct gutil_button_bounds gutil_button_rect(int x, int y, int w_units) {
	struct gutil_button_bounds b = {x, y, w_units * GFX_GUI_SCALE,
									20 * GFX_GUI_SCALE};
	return b;
}
bool gutil_button_hit(const struct gutil_button_bounds* b, float x, float y) {
	return x >= b->x && x < b->x + b->width && y >= b->y
		&& y < b->y + b->height;
}

/* The production helpers have their own truth-table target
 * (test_menu_activate_rule); this target cannot link gui_util.c (it doubles
 * the renderer half of that file above), so it carries a VERBATIM copy of
 * gutil_menu_pointer_focus + gutil_menu_activate_decide from
 * source/graphics/gui_util.c. Keep it verbatim: a drifted copy tests a rule
 * the game does not run (Fable G4 review item 5 found the selected != NONE
 * guard and the navigating-while-invalid seed missing here). */
bool gutil_menu_pointer_focus(struct gutil_menu_focus* focus, bool valid,
							  float x, float y, bool navigating) {
	if(!valid) {
		focus->pointer_valid = false;
		focus->pointer_owner = false;
		if(navigating)
			focus->initialized = true;
		return false;
	}
	if(!focus->pointer_valid) {
		focus->pointer_owner = !focus->initialized;
		focus->initialized = true;
		focus->x = x;
		focus->y = y;
	}
	float dx = x - focus->x, dy = y - focus->y;
	float threshold = GUTIL_MENU_POINTER_JITTER;
	if(navigating)
		focus->pointer_owner = false;
	else if(dx * dx + dy * dy >= threshold * threshold)
		focus->pointer_owner = true;
	if(navigating || focus->pointer_owner) {
		focus->x = x;
		focus->y = y;
	}
	focus->pointer_valid = true;
	return focus->pointer_owner;
}

/* Astra ruling 2's pure activation decision, extended by ruling 5 D2 for a
 * stationary pointer after directional input. gui_util.h carries the full
 * contract. This function reads only the caller's current state; the focus
 * helper owns the movement threshold and the caller maintains origin. */
struct gutil_menu_decision
gutil_menu_activate_decide(struct gutil_menu_activate_state in) {
	struct gutil_menu_decision out = {
		.action = GUTIL_MENU_NOTHING,
		.target = GUTIL_MENU_TARGET_NONE,
		.selected = in.selected,
		.selection_origin = in.selection_origin,
	};
	if(!in.pressed)
		return out; /* no fresh edge: the caller's re-arm has not opened yet */
	if(in.navigated)
		return out; /* R-B: this frame already moved the highlight */

	/* Astra ruling 5 D2: a direction press wins over a sprite resting on
	 * another enabled target. gutil_menu_pointer_focus changes the selection
	 * origin back to POINTER only after real movement (its 4 GUI-unit
	 * threshold), so this branch remains stable through IR jitter. A disabled
	 * hovered target still consumes the press under ruling 2 item 2. */
	/* Astra D2-destructive ruling (option B, 2026-09-24): a destructive
	 * confirmation does NOT take this branch -- the mismatch falls through
	 * to rule (1)'s FOCUS_ONLY, which executes neither answer, and only a
	 * later fresh press with hovered == selected activates. */
	if(!in.destructive_dialog
	   && in.pointer_valid && in.hovered != GUTIL_MENU_TARGET_NONE
	   && in.hovered_enabled && in.hovered != in.selected
	   && in.selection_origin == GUTIL_MENU_ORIGIN_DIRECTIONAL) {
		if(in.selected != GUTIL_MENU_TARGET_NONE && in.selected_enabled) {
			out.action = GUTIL_MENU_ACTIVATE;
			out.target = in.selected;
		}
		return out;
	}

	if(in.pointer_valid && in.hovered != GUTIL_MENU_TARGET_NONE) {
		/* (1) */
		if(!in.hovered_enabled)
			return out; /* consumed; never a fallback to `selected` */
		if(in.hovered == in.selected) {
			out.action = GUTIL_MENU_ACTIVATE;
			out.target = in.hovered;
			return out;
		}
		out.action = GUTIL_MENU_FOCUS_ONLY;
		out.target = in.hovered;
		out.selected = in.hovered;
		out.selection_origin = GUTIL_MENU_ORIGIN_POINTER;
		return out;
	}

	/* (2), R-A + R-E: with the sprite on NO control, only a selection the
	 * player moved to with a direction press is activatable. A POINTER-armed
	 * one is what the pointer itself chose (A6-1); a DEFAULT one is what the
	 * screen chose, and firing it on a background click discarded a
	 * sub-editor's typing (A9-2). */
	if(in.pointer_valid
	   && in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL)
		return out;

	if(in.selected == GUTIL_MENU_TARGET_NONE || !in.selected_enabled)
		return out;

	/* R-D: a destructive selection needs the player to have MEANT it. The
	 * hover branch above already returned for a sprite resting on it, so
	 * reaching here with a destructive selection means the sprite is NOT on
	 * it -- and then only a direction press is intent. */
	if(in.selected_destructive
	   && in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL)
		return out;

	/* (2) directional half, and (3): the highlighted target. */
	out.action = GUTIL_MENU_ACTIVATE;
	out.target = in.selected;
	return out;
}

/* ---- helpers ------------------------------------------------------------ */

static void reset_probe(void) {
	memset(pressed, 0, sizeof(pressed));
	pointer_on = false;
 button_quads=picture_calls=0;bound_texture=NULL;
	cue_count = 0;
	drawn_count = 0;
	hint_count = 0;
	pause_rpcs = unload_rpcs = 0;
	reject_rpc = false;
	option_return_set = false;
	leave_seq_count = 0;
	current = &screen_pause;
}

static void open_pause(void) {
	reset_probe();
	screen_pause.reset(&screen_pause, 640, 480);
	cue_count = 0; /* the open cue is asserted separately */
}

static bool saw(enum menu_sound_event e) {
	for(size_t k = 0; k < cue_count; k++)
		if(cues[k] == e)
			return true;
	return false;
}

/* Save and quit / Leave game is reachable ONLY through its named row since the
 * maintainer's 2026-09-19 decision on Astra menu review item 8 ("Close = Back /
 * Resume, consistently; quitting only through the named menu row"), so every
 * case that used to press the Close role to quit walks the approved list to
 * the last row and selects it there. */
static void quit_via_row(void) {
	size_t rows = screen_pause_test_button_count(true);
	assert(rows > 0);
	for(size_t k = 1; k < rows; k++) {
		memset(pressed, 0, sizeof(pressed));
		pressed[IB_GUI_DOWN] = true;
		screen_pause.update(&screen_pause, 0.016f);
	}
	const char* label = NULL;
	assert(screen_pause_test_button(rows - 1, true, !gstate.remote_session, NULL,
									&label));
	assert(!strcmp(label, gstate.remote_session ? "Leave game" : "Save and quit")
		   && "the exit row moved -- quit_via_row would select something else");
	memset(pressed, 0, sizeof(pressed));
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	memset(pressed, 0, sizeof(pressed));
}

/* ---- 1. the approved action set ---------------------------------------- */

static void test_action_set(void) {
	/* Full: Back to game, Achievements, Options..., Texture packs...,
	 * Save and quit (Texture packs... is the Astra ui9 addition, no Beta id). */
	assert(screen_pause_test_button_count(true) == 5);
	static const char* const full_host[5]
		= {"Back to game", "Achievements", "Options...", "Texture packs...", "Save and quit"};
	static const int ids[5] = {4, 5, 0, -1, 1}; /* Beta ids, unchanged mapping */
	for(size_t k = 0; k < 5; k++) {
		int id = -1;
		const char* label = NULL;
		assert(screen_pause_test_button(k, true, true, &id, &label));
		assert(id == ids[k]);
		assert(strcmp(label, full_host[k]) == 0);
	}
	/* Approved compact grid: Back, Achievements, Options, Exit. */
	assert(screen_pause_test_button_count(false) == 4);
	const char* label = NULL;
	assert(screen_pause_test_button(0, false, true, NULL, &label));
	assert(strcmp(label, "Back to game") == 0);
	assert(screen_pause_test_button(1, false, true, NULL, &label));
	assert(strcmp(label, "Achievements") == 0);
	assert(screen_pause_test_button(2, false, true, NULL, &label));
	assert(strcmp(label, "Options...") == 0);
	assert(screen_pause_test_button(3, false, true, NULL, &label));
	assert(strcmp(label, "Save and quit") == 0);
	assert(!screen_pause_test_button(4, false, true, NULL, &label));
	/* The real board restores Achievements; Statistics remains absent. */
	for(int full = 0; full < 2; full++)
		for(size_t k = 0; k < screen_pause_test_button_count(full != 0); k++) {
			assert(screen_pause_test_button(k, full != 0, true, NULL, &label));
			/* Achievements now has a real board; Statistics remains absent. */
			assert(strcmp(label, "Statistics") != 0);
		}
	puts("  pause: approved action set, both layouts 1/1");
}

/* ---- 2. host vs guest exit --------------------------------------------- */

static void test_host_guest(void) {
	const char* label = NULL;
	assert(screen_pause_test_button(4, true, true, NULL, &label)); /* row 4 after Achievements and Texture packs... */
	assert(strcmp(label, "Save and quit") == 0);
	assert(screen_pause_test_button(4, true, false, NULL, &label));
	assert(strcmp(label, "Leave game") == 0);

	/* The label follows world ownership through the live render path too. */
	gstate.remote_session = false;
	open_pause();
	screen_pause.render2D(&screen_pause, 640, 480);
	assert(drawn_count == 5);
	assert(strcmp(drawn_labels[4], "Save and quit") == 0);

	/* Remote chat opens through the in-game chat socket and resolved binding;
	 * the pause menu keeps the same five actions as single-player. */
	gstate.remote_session = true;
	open_pause();
	screen_pause.render2D(&screen_pause, 640, 480);
	assert(drawn_count == 5);
	assert(strcmp(drawn_labels[2], "Options...") == 0);
	assert(strcmp(drawn_labels[4], "Leave game") == 0);
	gstate.remote_session = false;
	puts("  pause: host save-and-quit vs guest leave 1/1");
}

/* ---- 3. detected controller name, no chooser --------------------------- */

static void test_controller_identity(void) {
	assert(strcmp(screen_pause_test_controller_name(INPUT_CAT_NUNCHUK),
				  "Wii Remote + Nunchuk")
		   == 0);
	assert(strcmp(screen_pause_test_controller_name(INPUT_CAT_CLASSIC_CONTROLLER),
				  "Classic Controller")
		   == 0);
	assert(strcmp(screen_pause_test_controller_name(INPUT_CAT_GAMECUBE),
				  "GameCube controller")
		   == 0);
	/* The name is read from the LIVE assigned device, not chosen: the render
	 * pass follows the owner frame's detected category for all three
	 * approved families and never draws a fourth "chooser" row. */
	static const enum input_category families[4]
		= {INPUT_CAT_WIIMOTE, INPUT_CAT_NUNCHUK, INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	for(int k = 0; k < 4; k++) {
		owner_frame.category = families[k];
		owner_frame.connected = true;
		open_pause();
		screen_pause.render2D(&screen_pause, 640, 480);
  assert(drawn_count==5 && button_quads==10);
  assert(picture_calls==1 && picture_y==348); /* status line follows the 5th row */
	}
	puts("  pause: detected controller name for all three families, no "
		 "chooser 1/1");
}

/* ---- 4. cue map -------------------------------------------------------- */

static void test_cues(void) {
	/* Opening the panel is the category cue, once. */
	reset_probe();
	screen_pause.reset(&screen_pause, 640, 480);
	assert(cue_count == 1 && cues[0] == MENU_SOUND_SECTION);

	/* Changed selection is the scroll cue; an unchanged edge is silent. */
	open_pause();
	pressed[IB_GUI_DOWN] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(cue_count == 1 && cues[0] == MENU_SOUND_FOCUS);
	pressed[IB_GUI_DOWN] = false;
	pressed[IB_GUI_UP] = true;
	cue_count = 0;
	screen_pause.update(&screen_pause, 0.016f);
	assert(cue_count == 1 && cues[0] == MENU_SOUND_FOCUS);
	/* Top of the list: UP again changes nothing, so it is silent. */
	cue_count = 0;
	screen_pause.update(&screen_pause, 0.016f);
	assert(cue_count == 0);

	/* A successful resume is Back -- and only AFTER the toggle is accepted. */
	open_pause();
	pressed[IB_HOME] = true;
	reject_rpc = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(cue_count == 0 && current == &screen_pause);
	reject_rpc = false;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 1 && saw(MENU_SOUND_BACK));
	assert(current == &screen_ingame);
	puts("  pause: open=category, changed selection=scroll, accepted "
		 "resume=Back, rejected resume silent 1/1");
}

/* ---- 5. the Beta RPC pair is unchanged --------------------------------- */

/* A refused unload (the server answers RETURN_INGAME): the pause stays up,
 * the live world is paused again through a second toggle, nothing reached
 * the title, and the retry is one click away. */
static void test_exit_refused_keeps_pause(void) {
	open_pause();
	quit_via_row(); /* 2026-09-19: the row, not the Close role */
	assert(pause_rpcs == 1 && unload_rpcs == 1 && screen_pause_exit_pending());
	assert(current != &screen_title && !gstate.paused);
	screen_pause_exit_settled(false);
	assert(!screen_pause_exit_pending());
	assert(pause_rpcs == 2 && gstate.paused && current != &screen_title);
	/* settled twice is a no-op */
	screen_pause_exit_settled(true);
	assert(current != &screen_title && pause_rpcs == 2);
	puts("PASS refused Save and quit keeps the pause up, re-pauses the world, never shows the title");
}

/* IMP-QUIT-UNSAVABLE: a refusal caused only by an unsavable chunk opens the
 * approved confirm dialog over the re-paused world -- never a hang on
 * "Saving world..." -- and both answers re-send the unload: "Try again" (the
 * safe answer, also Back) plain, "Quit anyway" excusing exactly the chunk the
 * server named. */
static void test_exit_unsavable_dialog(void) {
	open_pause();
	quit_via_row();
	assert(pause_rpcs == 1 && unload_rpcs == 1 && screen_pause_exit_pending());
	assert(!last_unload.payload.unload_world.accept_unsaved);
	confirm_answer = NULL;
	screen_pause_exit_unsavable(3, -2, 1);
	assert(!screen_pause_exit_pending() && "the dialog, not a frozen pause");
	assert(pause_rpcs == 2 && gstate.paused && "the live world re-paused");
	assert(current == &screen_confirm && confirm_answer);
	assert(!strcmp(confirm_lines[0], "Couldn't save part of the world"));
	assert(!strcmp(confirm_lines[1], "Chunk 3, -2"));
	assert(!strcmp(confirm_lines[2], "Quit anyway"));
	assert(!strcmp(confirm_lines[3], "Try again"));
	assert(confirm_safe == SCREEN_CONFIRM_ANSWER_NO);

	/* Try again: the ordinary Save and quit, from the pause screen. */
	void (*answer)(bool) = confirm_answer;
	answer(false);
	assert(current == &screen_pause && screen_pause_exit_pending());
	assert(pause_rpcs == 3 && unload_rpcs == 2 && !gstate.paused);
	assert(!last_unload.payload.unload_world.accept_unsaved);

	/* Still unsavable (three chunks now): asked again, never stuck. */
	screen_pause_exit_unsavable(5, 7, 3);
	assert(current == &screen_confirm && gstate.paused && pause_rpcs == 4);
	assert(!strcmp(confirm_lines[1], "3 chunks, first 5, 7"));

	/* Quit anyway: excuses exactly what the server named, once. */
	answer = confirm_answer;
	answer(true);
	assert(current == &screen_pause && screen_pause_exit_pending());
	assert(pause_rpcs == 5 && unload_rpcs == 3);
	assert(last_unload.payload.unload_world.accept_unsaved
		   && last_unload.payload.unload_world.chunk_x == 5
		   && last_unload.payload.unload_world.chunk_z == 7
		   && last_unload.payload.unload_world.count == 3);
	screen_pause_exit_settled(true);
	assert(current == &screen_title);

	/* The answer is one request only: the next quit is plain again. */
	open_pause();
	quit_via_row();
	assert(!last_unload.payload.unload_world.accept_unsaved);
	screen_pause_exit_settled(true);

	/* No pending exit: a stray unsavable answer changes nothing. */
	open_pause();
	unsigned before = pause_rpcs;
	screen_pause_exit_unsavable(1, 1, 1);
	assert(current == &screen_pause && pause_rpcs == before);
	puts("PASS unsavable chunk at quit -> Couldn't save part of the world (Quit anyway / Try again), never a hang");
}

/* REMOTE "Leave game" (2026-09-12 D2b). The server answers SRPC_UNLOAD_WORLD
 * on a remote session with a GRACEFUL disconnect, and main.c's
 * clin_force_world_reset() is gated on the UNgraceful case -- so no
 * CRPC_WORLD_RESET ever arrives and the old code sat frozen on
 * "Saving world..." forever. The row now runs the proven remote-escape
 * sequence itself (screen_load_world.c:397-405). */
static void test_remote_leave_sequence(void) {
	gstate.remote_session = true;
	open_pause();
	quit_via_row(); /* 2026-09-19: the row, not the Close role */
	/* A remote world never pauses or sends a resume toggle. The unload
	 * rides clin_send_unload_world (the chip-B reliable sender)
	 * instead of a bare svin_rpc_send, so the server still sees exactly one
	 * SRPC_UNLOAD_WORLD -- not two. */
	assert(pause_rpcs == 0 && unload_rpcs == 0);
	assert(!gstate.paused);
	/* drain -> client-side WORLD_RESET -> unload, in that order, once each. */
	assert(leave_seq_count == 3);
	assert(leave_seq[0] == LEAVE_DRAIN && leave_seq[1] == LEAVE_RESET
		   && leave_seq[2] == LEAVE_UNLOAD);
	/* The exit is armed BEFORE the reset: screen_pause_exit_settled() early-
	 * returns unless it is, and in the app the reset is synchronous. Here the
	 * reset is a stub, so the pending exit is still visible ... */
	assert(screen_pause_exit_pending());
	assert(current != &screen_title);
	/* ... and the settle the reset performs completes the exit: no longer
	 * frozen (screen_pause_update returns early only while pending), title
	 * shown, and the world stays unpaused. */
	screen_pause_exit_settled(true);
	assert(!screen_pause_exit_pending());
	assert(current == &screen_title);
	assert(pause_rpcs == 0 && unload_rpcs == 0 && leave_seq_count == 3);

	/* SINGLE PLAYER is untouched: Beta's RPC pair through svin_rpc_send, and
	 * none of the three remote calls. */
	gstate.remote_session = false;
	open_pause();
	quit_via_row(); /* 2026-09-19: the row, not the Close role */
	assert(pause_rpcs == 1 && unload_rpcs == 1);
	assert(leave_seq_count == 0);
	assert(screen_pause_exit_pending() && current != &screen_title);
	screen_pause_exit_settled(true);
	assert(!screen_pause_exit_pending() && current == &screen_title);
	assert(leave_seq_count == 0);
	puts("PASS remote Leave game drains, forces the client WORLD_RESET and "
		 "sends one unload, then settles to the title; SP Leave unchanged");
}

/* A valid pointer over empty menu space must not veto a controller click on
 * the directionally selected Leave row. This is the headless PC driver's
 * pause/rejoin path: its pointer stays available while the pad drives focus. */
static void test_remote_leave_with_idle_pointer(void) {
	gstate.remote_session = true;
	open_pause();
	pointer_on = true;
	ptr_x = 1.0f;
	ptr_y = 1.0f;
	for(size_t k = 1; k < screen_pause_test_button_count(true); k++) {
		pressed[IB_GUI_DOWN] = true;
		screen_pause.update(&screen_pause, 0.016f);
		pressed[IB_GUI_DOWN] = false;
	}
	assert(screen_pause_focus_is_leave());
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(leave_seq_count == 3 && "idle pointer swallowed the selected Leave row");
	pressed[IB_GUI_CLICK] = false;
	gstate.remote_session = false;
}

static void test_remote_leave_pointer_arbitration(void) {
	struct menu_composition c;
	menu_composition_begin(&c, MENU_SCREEN_PAUSE, 0, 0, 640, 480);
	struct menu_rect first = menu_pause_row(&c, 0);
	gstate.remote_session = true;
	open_pause();
	pointer_on = true;
	ptr_x = (float)(first.x + first.w / 2);
	ptr_y = (float)(first.y + first.h / 2);
	screen_pause.update(&screen_pause, 0.016f);
	for(size_t k = 1; k < screen_pause_test_button_count(true); k++) {
		pressed[IB_GUI_DOWN] = true;
		screen_pause.update(&screen_pause, 0.016f);
		pressed[IB_GUI_DOWN] = false;
	}
	assert(screen_pause_focus_is_leave());
	/* Direction was the last input: a resting sprite on Back cannot take A. */
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(leave_seq_count == 3 && screen_pause_focus_is_leave());
	pressed[IB_GUI_CLICK] = false;
	open_pause();
	pointer_on = true;
	ptr_x = (float)(first.x + first.w / 2);
	ptr_y = (float)(first.y + first.h / 2);
	screen_pause.update(&screen_pause, 0.016f);
	for(size_t k = 1; k < screen_pause_test_button_count(true); k++) {
		pressed[IB_GUI_DOWN] = true;
		screen_pause.update(&screen_pause, 0.016f);
		pressed[IB_GUI_DOWN] = false;
	}
	screen_pause.update(&screen_pause, 0.016f);
	/* Movement past the threshold takes over and points back to Resume. */
	ptr_x = (float)(first.x + first.w / 2 + 12);
	ptr_y = (float)(first.y + first.h / 2);
	screen_pause.update(&screen_pause, 0.016f);
	assert(!screen_pause_focus_is_leave());
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(leave_seq_count == 0 && current == &screen_ingame);
	pressed[IB_GUI_CLICK] = false;

	open_pause();
	for(size_t k = 1; k < screen_pause_test_button_count(true) - 1; k++) {
		pressed[IB_GUI_DOWN] = true;
		screen_pause.update(&screen_pause, 0.016f);
		pressed[IB_GUI_DOWN] = false;
	}
	pressed[IB_GUI_DOWN] = pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(screen_pause_focus_is_leave() && leave_seq_count == 0);
	pressed[IB_GUI_DOWN] = pressed[IB_GUI_CLICK] = false;
	screen_pause.update(&screen_pause, 0.016f);
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(leave_seq_count == 3);
	pressed[IB_GUI_CLICK] = false;
	gstate.remote_session = false;
}

static void test_rpc_pair(void) {
	open_pause();
	quit_via_row(); /* 2026-09-19: the row, not the Close role */
	/* [740c583 GuiIngameMenu.actionPerformed] id 1: resume, then
	 * changeWorld1(null). Both, in that order, exactly as before. */
	assert(pause_rpcs == 1 && unload_rpcs == 1);
	/* 2026-09-11: the title waits for the server's WORLD_RESET; the enqueue
	 * only arms the pending exit (buttons frozen, "Saving world..."). */
	assert(screen_pause_exit_pending());
	assert(current != &screen_title);
	assert(!gstate.paused);
	screen_pause_exit_settled(true);
	assert(!screen_pause_exit_pending());
	assert(current == &screen_title);

	/* Options... routes to the options screen with a return target and the
	 * category cue, and sends no RPC. */
	open_pause();
	pressed[IB_GUI_DOWN] = true;
	screen_pause.update(&screen_pause, 0.016f);
	screen_pause.update(&screen_pause, 0.016f); /* select Options... */
	pressed[IB_GUI_DOWN] = false;
	pressed[IB_GUI_CLICK] = true;
	cue_count = 0;
	screen_pause.update(&screen_pause, 0.016f);
	assert(option_return_set && current == &screen_options);
	assert(pause_rpcs == 0 && unload_rpcs == 0);
	assert(cue_count == 1 && cues[0] == MENU_SOUND_SECTION);
	puts("  pause: Beta RPC pair unchanged; Options... is a silent-of-RPC "
		 "submenu 1/1");
}

/* ---- 6. geometry matches the shared composition ------------------------ */

static void test_geometry(void) {
 const struct menu_rect full_rows[5]={{160,104,320,44},{160,150,320,44},{160,196,320,44},{160,242,320,44},{160,288,320,44}};
 const struct menu_rect compact_rows[4]={{48,68,266,44},{326,68,266,44},{48,120,266,44},{326,120,266,44}};
 struct menu_composition full,compact;
 screen_pause_test_composition(&full,640,480);
 screen_pause_test_composition(&compact,640,240);
 assert(full.header.y==56 && full.header.h==40);
 assert(full.header.y+full.header.h+8<=full.actions[0].y);
 for(int k=0;k<5;k++){struct menu_rect r=full.actions[k];assert(!memcmp(&r,&full_rows[k],sizeof r));}
 for(int k=0;k<4;k++){struct menu_rect r=compact.actions[k];assert(!memcmp(&r,&compact_rows[k],sizeof r));}
 view_height=240;open_pause();screen_pause.render2D(&screen_pause,640,240);
 assert(drawn_count==4 && button_quads==8 && picture_calls==1 && picture_y==176);
 assert(hint_count==2&&hint_actions[0]==IB_GUI_CLICK&&hint_actions[1]==IB_HOME);
 pressed[IB_GUI_CLICK_ALT]=true;screen_pause.update(&screen_pause,.016f);
 assert(pause_rpcs==1 && current==&screen_ingame);
 view_height=480;

	/* Pointer hit-testing must agree with menu_pause_row, which is also what
	 * the composition golden declares. Aim at row 2 (the exit row) and check
	 * the click lands there. */
	struct menu_composition c;
	menu_composition_begin(&c, MENU_SCREEN_PAUSE, 0, 0, 640, 480);
	screen_pause_test_composition(&c,640,480);
	struct menu_rect row = c.actions[4];
	open_pause();
	pointer_on = true;
	ptr_x = (float)(row.x + row.w / 2);
	ptr_y = (float)(row.y + row.h / 2);
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 1 && unload_rpcs == 1 && screen_pause_exit_pending());
	screen_pause_exit_settled(true);
	assert(current == &screen_title);

	/* Rows never leave the viewport and never collide. */
	for(int h = 240; h <= 480; h += 240) {
		struct menu_composition v;
		screen_pause_test_composition(&v,640,h);
		int rows = (int)v.action_count;
		for(int k = 0; k < rows; k++) {
			struct menu_rect a = v.actions[k];
			assert(a.x >= 0 && a.x + a.w <= 640);
			assert(a.y >= 0 && a.y + a.h <= h);
			for(int j = 0; j < k; j++) {
				struct menu_rect b = v.actions[j];
				assert(!menu_rect_overlap(&a, &b));
			}
		}
		struct menu_rect status = h==480?menu_pause_status(&v,rows):(struct menu_rect){0,176,640,24};
		assert(status.y + status.h <= h);
	}
	puts("  pause: rows agree with the shared composition, fit both "
		 "viewports, never collide 1/1");
}

/* ---- 7. the lane-3 recovery coupling ----------------------------------- */

static void test_recovery_coupling(void) {
	/* "Resume belongs exclusively to the modal while recovery is active.
	 * Save/quit remains available." The layout rework must not have changed
	 * either half. */
	recovery.phase = CONTROLLER_RECOVERY_LOST;
	open_pause();
	pressed[IB_HOME] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 0 && current == &screen_pause);

	open_pause();
	screen_pause_recovery_quit();
	assert(pause_rpcs == 1 && unload_rpcs == 1 && screen_pause_exit_pending());
	screen_pause_exit_settled(true);
	assert(current == &screen_title);
	recovery.phase = CONTROLLER_RECOVERY_NONE;
	puts("  pause: recovery withholds Resume, still permits quit 1/1");
}

/* ---- 8. the CLOSE role is Back / Resume -------------------------------- */

/* Astra menu review 2026-09-19, MUST item 8: the global Pause was the ONE
 * place in the whole UI where the Close role (Remote 1 / Classic X / GameCube
 * Y) did not mean Back -- it sent Beta's id-1 pair and dropped the player at
 * the title with no confirmation, while the split guest's local Pause ignored
 * the same button entirely. The maintainer settled the exception the same day:
 * "Close = Back / Resume, consistently; quitting only through the named menu
 * row." Save and quit keeps its row, its selection and the lane-3 recovery
 * entry point; HOME / Start are untouched. */
static void test_close_resumes(void) {
	/* Close alone == what HOME already did: one TOGGLE_PAUSE, NO unload, the
	 * world live again, gameplay on screen, the Back cue after the accept. */
	gstate.paused = true;
	open_pause();
	pressed[IB_INVENTORY] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 1 && "the Close role stopped resuming");
	assert(unload_rpcs == 0 && "the Close role still saves-and-quits");
	assert(!gstate.paused && current == &screen_ingame);
	assert(!screen_pause_exit_pending() && saw(MENU_SOUND_BACK));

	/* ... in a REMOTE session too, where the exit row runs the client-side
	 * teardown: none of those three calls may happen on a Close. */
	gstate.remote_session = true;
	gstate.paused = true;
	open_pause();
	pressed[IB_INVENTORY] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 0 && unload_rpcs == 0);
	assert(leave_seq_count == 0 && "the Close role ran the remote Leave teardown");
	assert(!screen_pause_exit_pending() && current == &screen_ingame);
	gstate.remote_session = false;

	/* A report carrying HOME and Close together is still exactly ONE resume --
	 * the same single-toggle guarantee the old HOME-owns-the-frame return
	 * gave, now for free because both edges mean the same thing. */
	gstate.paused = true;
	open_pause();
	pressed[IB_HOME] = pressed[IB_INVENTORY] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 1 && unload_rpcs == 0 && current == &screen_ingame);

	/* The NAMED ROW still quits, with Beta's RPC pair in Beta's order. */
	open_pause();
	quit_via_row();
	assert(pause_rpcs == 1 && unload_rpcs == 1 && screen_pause_exit_pending());
	screen_pause_exit_settled(true);
	assert(current == &screen_title);

	/* While recovery is active Close is a resume, so the modal withholds it
	 * exactly as it withholds HOME -- and quit is still permitted. */
	recovery.phase = CONTROLLER_RECOVERY_LOST;
	open_pause();
	pressed[IB_INVENTORY] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 0 && unload_rpcs == 0 && current == &screen_pause
		   && "recovery let the Close role resume");
	screen_pause_recovery_quit();
	assert(pause_rpcs == 1 && unload_rpcs == 1 && screen_pause_exit_pending());
	screen_pause_exit_settled(true);
	recovery.phase = CONTROLLER_RECOVERY_NONE;
	puts("  pause: Close resumes and never quits (SP + remote), HOME+Close is "
		 "one toggle, the named row still quits, recovery withholds Close 1/1");
}

/* W3-p1: a background A press is inert, but must not eat another button's
 * edge in that same controller report. A real row activation still owns the
 * frame, including when Close is pressed alongside it. */
static void test_blank_click_preserves_close_edges(void) {
	const enum input_button close_edges[] = {IB_HOME, IB_INVENTORY};
	for(size_t k = 0; k < sizeof(close_edges) / sizeof(close_edges[0]); k++) {
		gstate.paused = true;
		open_pause();
		pointer_on = true;
		ptr_x = ptr_y = 1.0f; /* blank space, no approved row */
		pressed[IB_GUI_CLICK] = true;
		pressed[close_edges[k]] = true;
		screen_pause.update(&screen_pause, 0.016f);
		assert(pause_rpcs == 1 && unload_rpcs == 0);
		assert(current == &screen_ingame && !gstate.paused);
		assert(cue_count == 1 && saw(MENU_SOUND_BACK));
	}

	gstate.paused = true;
	open_pause();
	pointer_on = true;
	ptr_x = ptr_y = 1.0f;
	pressed[IB_GUI_CLICK] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 0 && unload_rpcs == 0);
	assert(current == &screen_pause && gstate.paused && cue_count == 0);

	gstate.paused = true;
	open_pause();
	pressed[IB_HOME] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(pause_rpcs == 1 && current == &screen_ingame && !gstate.paused);

	gstate.paused = true;
	open_pause();
	pressed[IB_GUI_DOWN] = true; /* Options row */
	screen_pause.update(&screen_pause, 0.016f);
	screen_pause.update(&screen_pause, 0.016f);
	pressed[IB_GUI_DOWN] = false;
	pressed[IB_GUI_CLICK] = pressed[IB_INVENTORY] = true;
	screen_pause.update(&screen_pause, 0.016f);
	assert(option_return_set && current == &screen_options);
	assert(pause_rpcs == 0 && unload_rpcs == 0 && gstate.paused);
	puts("  pause: blank A preserves simultaneous HOME/Close once; real activation owns Close");
}

int main(void) {
	puts("approved pause composition");
	owner_frame.connected = true;
	owner_frame.category = INPUT_CAT_NUNCHUK;
	owner_device_known = false;
	test_action_set();
	test_host_guest();
	test_controller_identity();
	test_cues();
	test_rpc_pair();
	test_remote_leave_sequence();
	test_remote_leave_with_idle_pointer();
	test_remote_leave_pointer_arbitration();
	test_exit_refused_keeps_pause();
	test_exit_unsavable_dialog();
	test_geometry();
	test_recovery_coupling();
	test_close_resumes();
	test_blank_click_preserves_close_edges();
	puts("PASS approved pause actions");
	return 0;
}
