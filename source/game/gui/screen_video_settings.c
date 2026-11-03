/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Native Display menu: only the live View Bobbing setting is exposed.
 * Uncertified detail/reach presets and screen-safe-area adjustment stay in
 * the release backlog (docs/ui_design/native_rework_2026-09-04.md).
 * No simulation, lighting values or render-quality defaults change here. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../game_state.h"
#include "screen.h"
#include "frontend_presentation.h"
#include "../../platform/input_owner.h"
#include "../../platform/input_glyph.h"

enum video_id {
	VIDEO_VIEW_BOBBING = 5, /* EnumOptions ordinal [740c583 EnumOptions]. */
	VIDEO_DONE = 200,
};

enum video_control_index {
	VC_VIEW_BOBBING = 0,
	VC_DONE,
	VC_COUNT,
};

struct video_control {
	int id;
	const char* name;
	bool enabled;
};

static const struct video_control controls[VC_COUNT] = {
	{VIDEO_VIEW_BOBBING, "View Bobbing", true},
	{VIDEO_DONE, "Back", true},
};

static int selected = VC_VIEW_BOBBING; /* first enabled control */
static struct gutil_menu_focus focus;
static struct gutil_menu_repeat repeat;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static bool view_bobbing_value = true;
static bool save_failed;
static enum gutil_menu_origin origin;
static char labels[VC_COUNT][64];

/* GameSettings boolean label convention, with the actual live value. */
static void video_format_label(int id, bool value, char* out,
							   size_t out_size) {
	if(id == VIDEO_VIEW_BOBBING)
		snprintf(out, out_size, "View Bobbing: %s", value ? "ON" : "OFF");
	else
		snprintf(out, out_size, "Back");
}

static void video_refresh_labels(void) {
	for(size_t k = 0; k < VC_COUNT; k++)
		video_format_label(controls[k].id, view_bobbing_value, labels[k],
						   sizeof(labels[k]));
}

/* Approved studio.js geometry, scaled from its 640 x 480 canvas. */
static int opt_px(int v) { return v * gfx_height() / 480; }
static struct fep_rect opt_rect(int x, int y, int w, int h) {
	return fep_rect((gfx_width() - opt_px(640)) / 2 + opt_px(x),
		opt_px(y), opt_px(w), opt_px(h));
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
static void opt_text(int x, int y, int size, int room, const char* text, bool dark) {
	char fit[192];
	fep_text_fit(fit, sizeof(fit), text, opt_px(size), opt_px(room));
	struct fep_rect r = opt_rect(x, y, 0, 0);
	gutil_text_rgb(r.x, r.y, fit, opt_px(size), !dark,
		dark ? 32 : 255, dark ? 32 : 255, dark ? 32 : 255);
}
static void opt_button(struct fep_rect r, const char* text, bool enabled, bool focused) {
	fep_widget(r, focused && enabled);
	fep_bevel(r);
	if(!enabled) fep_fill(r, 0, 0, 0, 100);
	if(focused) fep_outline(r, opt_px(3), 255, 255, 255, 255);
	int size = opt_px(20);
	gutil_text_rgb(r.x + (r.w - gutil_font_width(text, size)) / 2,
		r.y + (r.h - size) / 2, text, size, true,
		enabled ? 255 : 160, enabled ? 255 : 160, enabled ? 255 : 160);
}
static struct fep_rect video_target_rect(int target) {
	if(target == VC_VIEW_BOBBING) return opt_rect(476, 132, 108, 44);
	if(target == VC_DONE) return opt_footer(false);
	return fep_rect(0, 0, 0, 0);
}

static bool video_commit(void) {
	int old_value
		= config_read_int(&gstate.config_user, "render.view_bobbing", 1);
	int new_value = view_bobbing_value ? 1 : 0;

	/* The texture-pack transaction precedent (options_commit): set -> apply
	 * -> save; on failure restore the old key, re-apply, re-save. */
	bool applied
		= config_set_int(&gstate.config_user, "render.view_bobbing", new_value)
		&& config_save(&gstate.config_user, "config.json");
	if(applied) {
		gstate.config.view_bobbing = view_bobbing_value;
		return true;
	}

	if(config_set_int(&gstate.config_user, "render.view_bobbing", old_value))
		(void)config_save(&gstate.config_user, "config.json");
	view_bobbing_value = old_value != 0;
	gstate.config.view_bobbing = view_bobbing_value;
	video_refresh_labels();
	return false;
}

static void video_leave(void) {
	/* Beta returns to the parent GuiOptions and saves on Done
	 * [740c583 GuiVideoSettings.actionPerformed]. */
	/* Second Back after a failed save: discard the draft and leave. The
	 * failed video_commit already restored the old value live and in the
	 * tree; a later toggle clears save_failed and re-arms the save. */
	if(save_failed) {
		screen_set(&screen_options);
		return;
	}
	if(video_commit())
		screen_set(&screen_options);
	else
		save_failed = true;
}

static void video_step_selection(int dir) {
	int k = selected;
	do
		k += dir;
	while(k >= 0 && k < (int)VC_COUNT && !controls[k].enabled);
	if(k >= 0 && k < (int)VC_COUNT)
		selected = k;
}

static void video_toggle_bobbing(void) {
	/* viewBobbing = !viewBobbing, applied immediately
	 * [740c583 GameSettings.setOptionValue:131]. */
	view_bobbing_value = !view_bobbing_value;
	gstate.config.view_bobbing = view_bobbing_value;
	save_failed = false;
	video_refresh_labels();
}

static void screen_video_settings_reset(struct screen* s, int width,
										int height) {
	s->render_world = screen_options_from_pause();
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	selected = VC_VIEW_BOBBING;
	origin = GUTIL_MENU_ORIGIN_DEFAULT;
	focus = (struct gutil_menu_focus) {0};
	repeat = (struct gutil_menu_repeat) {0};
	pointer_available = false;
	save_failed = false;
	view_bobbing_value = gstate.config.view_bobbing;
	video_refresh_labels();
}

static void screen_video_settings_update(struct screen* s, float dt) {
	(void)s;
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool up = input_held(IB_GUI_UP) || input_pressed(IB_GUI_UP);
	bool down = input_held(IB_GUI_DOWN) || input_pressed(IB_GUI_DOWN);
	bool left = input_held(IB_GUI_LEFT) || input_pressed(IB_GUI_LEFT);
	bool right = input_held(IB_GUI_RIGHT) || input_pressed(IB_GUI_RIGHT);
	bool navigating = up || down || left || right;
	bool owner = gutil_menu_pointer_focus(&focus, pointer_available, pointer_x, pointer_y, navigating);
	int hovered = -1;
	if(pointer_available) for(int i = 0; i < VC_COUNT; i++) {
		if(fep_rect_hit(video_target_rect(i), pointer_x, pointer_y)) {
			hovered = i;
			if(owner) { selected = i; origin = GUTIL_MENU_ORIGIN_POINTER; }
			break;
		}
	}
	int step = gutil_menu_repeat_step(&repeat, down - up, dt);
	if(navigating) origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	if(step) video_step_selection(step);
	if(!up && !down && selected == VC_VIEW_BOBBING && left != right
	   && (input_pressed(IB_GUI_LEFT) || input_pressed(IB_GUI_RIGHT))) {
		view_bobbing_value = right;
		gstate.config.view_bobbing = view_bobbing_value;
		save_failed = false;
		video_refresh_labels();
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	}
	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision d = gutil_menu_activate_decide((struct gutil_menu_activate_state) {
		.pressed = click, .navigated = navigating, .pointer_valid = pointer_available,
		.selection_origin = origin, .hovered = hovered, .hovered_enabled = hovered >= 0,
		.selected = selected, .selected_enabled = selected >= 0 && selected < VC_COUNT,
	});
	selected = d.selected; origin = d.selection_origin;
	if(d.action == GUTIL_MENU_ACTIVATE) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		if(d.target == VC_VIEW_BOBBING) video_toggle_bobbing();
		else if(d.target == VC_DONE) video_leave();
	}
	if(click) return;
	if(input_pressed(IB_GUI_CLICK_ALT) || input_pressed(IB_HOME) || input_pressed(IB_INVENTORY)) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		video_leave();
	}
}

static void screen_video_settings_render2D(struct screen* s, int width, int height) {
	(void)s;
	if(screen_options_from_pause()) fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 180);
	else if(!fep_backdrop(width, height, 90)) gutil_bg();
	opt_text(32, 22, 28, 576, "Display", false);
	struct fep_rect panel = opt_rect(32, 100, 576, 188);
	fep_paper(panel); fep_bevel(panel);
	opt_text(60, 143, 20, 336, "View bobbing", true);
	opt_button(video_target_rect(VC_VIEW_BOBBING), view_bobbing_value ? "On" : "Off", true, selected == VC_VIEW_BOBBING);
	if(view_bobbing_value && selected != VC_VIEW_BOBBING)
		fep_outline(video_target_rect(VC_VIEW_BOBBING), opt_px(2), 48, 48, 48, 255);
	opt_text(60, 221, 16, 500, "Moves the view while walking.", true);
	if(save_failed)
		opt_text(60, 304, 16, 520,
			"Could not save. Press Back again to leave without saving", false);
	struct fep_rect back = opt_footer(true);
	if(selected == VC_DONE) fep_outline(back, opt_px(3), 255, 255, 255, 255);
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0, 0, 256, 256, opt_px(48), opt_px(48));
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_video_settings_test_target_rect(int target, struct gutil_button_bounds* out) {
	struct fep_rect r = video_target_rect(target);
	if(out) *out = (struct gutil_button_bounds) {r.x, r.y, r.w, r.h};
	return r.w > 0 && r.h > 0;
}
int screen_video_settings_test_focus(void) { return selected; }
size_t screen_video_settings_test_control_count(void) {
	return VC_COUNT;
}

bool screen_video_settings_test_control(size_t idx, int* id, const char** name,
										bool* enabled) {
	if(idx >= VC_COUNT)
		return false;
	if(id)
		*id = controls[idx].id;
	if(name)
		*name = controls[idx].name;
	if(enabled)
		*enabled = controls[idx].enabled;
	return true;
}

void screen_video_settings_test_format_label(int id, bool value, char* out,
											 size_t out_size) {
	video_format_label(id, value, out, out_size);
}
#endif

struct screen screen_video_settings = {
	.reset = screen_video_settings_reset,
	.update = screen_video_settings_update,
	.render2D = screen_video_settings_render2D,
	.render3D = NULL,
	.render_world = false,
};
