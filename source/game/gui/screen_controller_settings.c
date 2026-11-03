/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Approved Player 1 look preferences. The input layer owns the response
 * curve; this screen stages bounded values and applies them only after the
 * user config is successfully saved. The third row (aim-snap-1005) is the
 * opt-in soft-snap melee assist, Off / Low / Normal (docs/deviations.md
 * "Soft-snap melee assist"); it follows the Invert row's pattern. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../game_state.h"
#include "frontend_presentation.h"
#include "screen.h"

enum controller_target {
	CT_INVERT = 0,
	CT_SENS_DOWN,
	CT_SENS_UP,
	CT_DONE,
	CT_ASSIST, /* appended: ids 0..3 keep their meaning */
	CT_COUNT,
};

static int selected = CT_INVERT;
static struct gutil_menu_focus focus;
static struct gutil_menu_repeat repeat;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static enum gutil_menu_origin origin;
static int sensitivity = 3;
static bool inverted;
static int assist; /* enum attack_assist_level */
static bool save_failed;

static const char* const assist_names[] = {"Off", "Low", "Normal"};

static int opt_px(int v) { return v * gfx_height() / 480; }
static struct fep_rect opt_rect(int x, int y, int w, int h) {
	return fep_rect((gfx_width() - opt_px(640)) / 2 + opt_px(x), opt_px(y),
					opt_px(w), opt_px(h));
}

static struct fep_rect opt_footer(bool draw) {
	/* The centred hint row (controller UI redesign 10-31, A1 helper); the
	 * Back hit target is the row's own Back cell, so pointer == drawn. */
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	static const struct gutil_hint_item items[] = {
		{{IB_GUI_UP}, 1, "Move", 1, false},
		{{IB_GUI_CLICK}, 1, "Select", 2, false},
		{{IB_INVENTORY}, 1, "Back", 0, true}};
	struct gutil_hint_row row;
	(void)gutil_hint_row_layout(&prompts, 0, 0, gfx_width(), gfx_height(),
		false, GUTIL_HINT_ALIGN_CENTRE, items, 3, &row);
	struct fep_rect back = fep_rect(0, 0, 0, 0);
	for(int k = 0; k < row.count; k++)
		if(row.cells[k].item == 2)
			back = fep_rect(row.cells[k].rect.x, row.cells[k].rect.y,
				row.cells[k].rect.width, row.cells[k].rect.height);
	if(draw)
		gutil_hint_row_draw(&row);
	return back;
}

static void opt_text(int x, int y, int size, int room, const char* text,
					 bool dark) {
	char fit[192];
	fep_text_fit(fit, sizeof(fit), text, opt_px(size), opt_px(room));
	struct fep_rect r = opt_rect(x, y, 0, 0);
	gutil_text_rgb(r.x, r.y, fit, opt_px(size), !dark, dark ? 32 : 255,
					dark ? 32 : 255, dark ? 32 : 255);
}

static void opt_button(struct fep_rect r, const char* text, bool enabled,
					   bool focused) {
	fep_widget(r, focused && enabled);
	fep_bevel(r);
	if(!enabled)
		fep_fill(r, 0, 0, 0, 100);
	if(focused)
		fep_outline(r, opt_px(3), 255, 255, 255, 255);
	int size = opt_px(20);
	gutil_text_rgb(r.x + (r.w - gutil_font_width(text, size)) / 2,
					r.y + (r.h - size) / 2, text, size, true,
					enabled ? 255 : 160, enabled ? 255 : 160,
					enabled ? 255 : 160);
}

static struct fep_rect controller_target_rect(int target) {
	switch(target) {
		case CT_INVERT: return opt_rect(476, 128, 108, 44);
		case CT_SENS_DOWN: return opt_rect(332, 212, 60, 44);
		case CT_SENS_UP: return opt_rect(524, 212, 60, 44);
		case CT_ASSIST: return opt_rect(476, 296, 108, 44);
		case CT_DONE: return opt_footer(false);
		default: return fep_rect(0, 0, 0, 0);
	}
}

static bool controller_target_enabled(int target) {
	if(target == CT_SENS_DOWN)
		return sensitivity > 1;
	if(target == CT_SENS_UP)
		return sensitivity < 5;
	return target == CT_INVERT || target == CT_ASSIST || target == CT_DONE;
}

static int controller_row(int target) {
	if(target == CT_INVERT)
		return 0;
	if(target == CT_SENS_DOWN || target == CT_SENS_UP)
		return 1;
	if(target == CT_ASSIST)
		return 2;
	return 3;
}

static int controller_target_for_row(int row) {
	return row == 0 ? CT_INVERT
		: row == 1  ? CT_SENS_DOWN
		: row == 2  ? CT_ASSIST
					: CT_DONE;
}

/* Left/Right step the level and stop at the ends; A cycles. */
static bool controller_set_assist(int next) {
	if(next < 0 || next > 2 || next == assist)
		return false;
	assist = next;
	save_failed = false;
	return true;
}

static void controller_adjust_sensitivity(int direction) {
	int next = sensitivity + direction;
	if(next < 1)
		next = 1;
	if(next > 5)
		next = 5;
	if(next != sensitivity) {
		sensitivity = next;
		save_failed = false;
	}
}

static bool controller_commit(void) {
	JSON_Value* before = json_value_deep_copy(gstate.config_user.root);
	if(!before)
		return false;
	bool saved
		= config_set_int(&gstate.config_user, "controls.look_sensitivity",
						 sensitivity)
		&& config_set_int(&gstate.config_user, "controls.invert_look",
						  inverted ? 1 : 0)
		&& config_set_int(&gstate.config_user, "controls.attack_assist", assist)
		&& config_save(&gstate.config_user, "config.json");
	if(!saved) {
		/* Restore the whole tree: this preserves absent or malformed values and
		 * also removes a `controls` object that dotset created from nothing. The
		 * transactional config_save contract preserves or recovers the prior
		 * disk file on failure. */
		json_value_free(gstate.config_user.root);
		gstate.config_user.root = before;
		return false;
	}
	json_value_free(before);
	return input_primary_look_set(sensitivity, inverted)
		&& input_attack_assist_set(assist);
}

static void controller_leave(void) {
	/* Second Back after a failed save: discard the draft and leave. The
	 * failed controller_commit already restored the config tree and never
	 * applied the live look/assist values, so nothing is left to undo. Any
	 * value edit clears save_failed and so re-arms the save-on-Back path.
	 * Without this a card that keeps refusing writes traps the player here. */
	if(save_failed) {
		screen_set(&screen_options);
		return;
	}
	if(controller_commit())
		screen_set(&screen_options);
	else
		save_failed = true;
}

static void screen_controller_settings_reset(struct screen* s, int width,
											 int height) {
	s->render_world = screen_options_from_pause();
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	selected = CT_INVERT;
	focus = (struct gutil_menu_focus) {0};
	repeat = (struct gutil_menu_repeat) {0};
	pointer_available = false;
	origin = GUTIL_MENU_ORIGIN_DEFAULT;
	save_failed = false;
	sensitivity = input_primary_look_sensitivity();
	inverted = input_primary_look_inverted();
	assist = input_attack_assist_level();
}

static void screen_controller_settings_update(struct screen* s, float dt) {
	(void)s;
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool up = input_held(IB_GUI_UP) || input_pressed(IB_GUI_UP);
	bool down = input_held(IB_GUI_DOWN) || input_pressed(IB_GUI_DOWN);
	bool left = input_held(IB_GUI_LEFT) || input_pressed(IB_GUI_LEFT);
	bool right = input_held(IB_GUI_RIGHT) || input_pressed(IB_GUI_RIGHT);
	bool navigating = up || down || left || right;
	bool owner = gutil_menu_pointer_focus(&focus, pointer_available, pointer_x,
										 pointer_y, navigating);
	int hovered = -1;
	if(pointer_available) {
		for(int i = 0; i < CT_COUNT; i++) {
			if(fep_rect_hit(controller_target_rect(i), pointer_x, pointer_y)) {
				hovered = i;
				if(owner) {
					selected = i;
					origin = GUTIL_MENU_ORIGIN_POINTER;
				}
				break;
			}
		}
	}
	int step = gutil_menu_repeat_step(&repeat,
		(up || down) ? down - up : 2 * (right - left), dt);
	if(navigating)
		origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	if(step == 1 || step == -1) {
		int row = controller_row(selected) + step;
		if(row < 0)
			row = 0;
		if(row > 3)
			row = 3;
		selected = controller_target_for_row(row);
	} else if(step && controller_row(selected) == 1) {
		selected = step > 0 ? CT_SENS_UP : CT_SENS_DOWN;
		int before = sensitivity;
		controller_adjust_sensitivity(step > 0 ? 1 : -1);
		if(before != sensitivity)
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	} else if(step && controller_row(selected) == 2) {
		if(controller_set_assist(assist + (step > 0 ? 1 : -1)))
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	} else if(step && controller_row(selected) == 0) {
		bool next = step > 0;
		if(next != inverted) {
			inverted = next;
			save_failed = false;
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		}
	}

	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision d = gutil_menu_activate_decide(
		(struct gutil_menu_activate_state) {
			.pressed = click,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = origin,
			.hovered = hovered,
			.hovered_enabled = controller_target_enabled(hovered),
			.selected = selected,
			.selected_enabled = controller_target_enabled(selected),
		});
	selected = d.selected;
	origin = d.selection_origin;
	if(d.action == GUTIL_MENU_ACTIVATE) {
		if(d.target == CT_INVERT) {
			inverted = !inverted;
			save_failed = false;
		} else if(d.target == CT_SENS_DOWN) {
			controller_adjust_sensitivity(-1);
		} else if(d.target == CT_SENS_UP) {
			controller_adjust_sensitivity(1);
		} else if(d.target == CT_ASSIST) {
			(void)controller_set_assist((assist + 1) % 3);
		} else if(d.target == CT_DONE) {
			controller_leave();
		}
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	}
	/* A click is always consumed, including disabled and focus-only clicks. */
	if(click)
		return;
	if(input_pressed(IB_GUI_CLICK_ALT) || input_pressed(IB_HOME)
	   || input_pressed(IB_INVENTORY)) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		controller_leave();
	}
}

static void screen_controller_settings_render2D(struct screen* s, int width,
											   int height) {
	(void)s;
	if(screen_options_from_pause())
		fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 180);
	else if(!fep_backdrop(width, height, 90))
		gutil_bg();
	opt_text(32, 22, 28, 430, "Controller settings", false);
	opt_text(500, 28, 17, 108, "Player 1", false);
	struct fep_rect panel = opt_rect(32, 100, 576, 252);
	fep_paper(panel);
	fep_bevel(panel);
	opt_text(60, 141, 20, 350, "Invert look", true);
	opt_button(controller_target_rect(CT_INVERT), inverted ? "On" : "Off", true,
			   selected == CT_INVERT);
	if(inverted && selected != CT_INVERT)
		fep_outline(controller_target_rect(CT_INVERT), opt_px(2), 48, 48, 48,
					255);
	opt_text(60, 225, 20, 236, "Sensitivity", true);
	opt_button(controller_target_rect(CT_SENS_DOWN), "-",
			   controller_target_enabled(CT_SENS_DOWN),
			   selected == CT_SENS_DOWN);
	char value[16];
	snprintf(value, sizeof(value), "%d / 5", sensitivity);
	struct fep_rect value_rect = opt_rect(404, 225, 108, 20);
	gutil_text_rgb(value_rect.x
					+ (value_rect.w - gutil_font_width(value, opt_px(19))) / 2,
				value_rect.y, value, opt_px(19), false, 32, 32, 32);
	opt_button(controller_target_rect(CT_SENS_UP), "+",
			   controller_target_enabled(CT_SENS_UP), selected == CT_SENS_UP);
	opt_text(60, 309, 20, 350, "Attack assist", true);
	opt_button(controller_target_rect(CT_ASSIST), assist_names[assist], true,
			   selected == CT_ASSIST);
	if(assist && selected != CT_ASSIST)
		fep_outline(controller_target_rect(CT_ASSIST), opt_px(2), 48, 48, 48,
					255);
	if(save_failed)
		opt_text(60, 366, 16, 520,
				 "Could not save. Press Back again to leave without saving",
				 false);
	struct fep_rect back = opt_footer(true);
	if(selected == CT_DONE)
		fep_outline(back, opt_px(3), 255, 255, 255, 255);
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0, 0,
						 256, 256, opt_px(48), opt_px(48));
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_controller_settings_test_target_rect(
	int target, struct gutil_button_bounds* out) {
	struct fep_rect r = controller_target_rect(target);
	if(out)
		*out = (struct gutil_button_bounds) {r.x, r.y, r.w, r.h};
	return r.w > 0 && r.h > 0;
}
int screen_controller_settings_test_focus(void) { return selected; }
bool screen_controller_settings_test_target_enabled(int target) {
	return controller_target_enabled(target);
}
int screen_controller_settings_test_sensitivity(void) { return sensitivity; }
bool screen_controller_settings_test_inverted(void) { return inverted; }
int screen_controller_settings_test_assist(void) { return assist; }
bool screen_controller_settings_test_save_failed(void) { return save_failed; }
#endif

struct screen screen_controller_settings = {
	.reset = screen_controller_settings_reset,
	.update = screen_controller_settings_update,
	.render2D = screen_controller_settings_render2D,
	.render3D = NULL,
	.render_world = false,
};
