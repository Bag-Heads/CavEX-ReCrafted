/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Creative phase-2 ITEM PICKER (docs/creative_mode.md phase 2). NET-NEW
 * addition under the 2026-08-25 maintainer exception -- Beta 1.7.3 has no
 * creative mode, so there is no decompile to mirror; the design is
 * Legacy-Console-style pointer-first, Wiimote-is-the-floor:
 * - IR pointer hovers a cell, IB_GUI_CLICK grants one full stack (a
 *   fire-and-forget SRPC_CREATIVE_GIVE; the server validates and pushes the
 *   authoritative slot back -- no client-side inventory writes, per the
 *   ghost-hotbar write-through lesson).
 * - D-pad moves the cell cursor for pointer-less controllers (Classic/GC);
 *   LEFT/RIGHT past the grid edge flips the page, DOWN past the bottom row
 *   reaches the button row, so every control is reachable without a pointer.
 * - IB_INVENTORY/IB_HOME leave to the game; the "Inventory" button opens the
 *   ordinary survival inventory screen (armor slots, crafting) which remains
 *   fully functional in creative.
 * The item list (featured-first ordering, include predicate) lives in
 * creative_picker_list.c. This screen only reads gstate.config_creative to
 * exist; world rules are untouched. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../item/items.h" // direct include (was transitive via gui_util.h pre-split)
#include "../../network/server_interface.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../game_state.h"
#include "creative_picker_list.h"
#include "screen.h"

#define PICKER_COLS 9
#define PICKER_ROWS 5
#define PICKER_PAGE (PICKER_COLS * PICKER_ROWS)
#define PICKER_CELL 18 /* GUI units per cell; 16px icon + 2px frame */

/* Button-row cursor positions, after the grid cells 0..PICKER_PAGE-1. */
enum picker_button {
	PB_PREV = PICKER_PAGE,
	PB_NEXT,
	PB_INVENTORY,
	PB_DONE,
	PB_COUNT,
};

static int cursor = 0; /* 0..PICKER_PAGE-1 grid cell, or PB_* button */
static size_t page = 0;
static int pointer_target = -1;
/* Astra menu review item 13: hover used to assign `cursor` on EVERY update
 * while the pointer was merely VALID, so a resting Wii Remote snapped the
 * grid cursor back under the pointer the instant after a D-pad step. Same
 * movement-based arbitration screen_options.c uses. */
static struct gutil_menu_focus focus;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
/* WHO CHOSE the highlighted cell -- see screen_title.c's note (R-A). */
static enum gutil_menu_origin picker_origin;

static size_t picker_page_count(void) {
	size_t total = creative_picker_count();
	size_t pages = (total + PICKER_PAGE - 1) / PICKER_PAGE;
	return pages ? pages : 1;
}

/* Grid origin in framebuffer pixels. */
static void picker_grid_origin(int width, int height, int* gx, int* gy) {
	(void)height;
	*gx = (width - PICKER_COLS * PICKER_CELL * GFX_GUI_SCALE) / 2;
	*gy = 40 * GFX_GUI_SCALE;
}

static void picker_cell_rect(int width, int height, int cell, int* x, int* y) {
	int gx, gy;
	picker_grid_origin(width, height, &gx, &gy);
	*x = gx + (cell % PICKER_COLS) * PICKER_CELL * GFX_GUI_SCALE;
	*y = gy + (cell / PICKER_COLS) * PICKER_CELL * GFX_GUI_SCALE;
}

/* Button geometry, one row under the grid: [<] [>] ... [Inventory] [Done]. */
static void picker_button_rect(int width, int height, int which, int* bx,
							   int* by, int* bw) {
	int gx, gy;
	picker_grid_origin(width, height, &gx, &gy);
	int s = GFX_GUI_SCALE;
	*by = gy + (PICKER_ROWS * PICKER_CELL + 6) * s;
	switch(which) {
		case PB_PREV:
			*bx = gx;
			*bw = 20;
			break;
		case PB_NEXT:
			*bx = gx + 24 * s;
			*bw = 20;
			break;
		case PB_INVENTORY:
			*bx = gx + 48 * s;
			*bw = 70;
			break;
		default: /* PB_DONE */
			*bx = gx + 122 * s;
			*bw = 40;
			break;
	}
}

static void picker_flip_page(int dir) {
	size_t pages = picker_page_count();
	page = (page + pages + (size_t)(dir < 0 ? pages - 1 : 1)) % pages;
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
}

/* A grid cell is a real target only while this page actually has an entry in
 * it; the tail of the last page is empty space wearing a cell's coordinates.
 * The four PB_* buttons are always live. [ruling 2 item 2: only enabled,
 * currently valid targets may activate.] */
static bool picker_cell_has_entry(int cell) {
	struct item_data it;
	return cell >= 0 && cell < PICKER_PAGE
		&& creative_picker_entry(page * PICKER_PAGE + (size_t)cell, &it);
}

static void picker_grant(int cell) {
	struct item_data it;
	size_t index = page * PICKER_PAGE + (size_t)cell;
	if(!creative_picker_entry(index, &it))
		return;
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	svin_rpc_send(&(struct server_rpc) {
		.type = SRPC_CREATIVE_GIVE,
		.payload.creative_give.id = it.id,
		.payload.creative_give.durability = it.durability,
	});
}

static void picker_activate(int target) {
	switch(target) {
		case PB_PREV: picker_flip_page(-1); break;
		case PB_NEXT: picker_flip_page(1); break;
		case PB_INVENTORY:
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			screen_set(&screen_inventory);
			break;
		case PB_DONE:
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			screen_set(&screen_ingame);
			break;
		default: picker_grant(target); break;
	}
}

static void screen_creative_picker_reset(struct screen* s, int width,
										 int height) {
	(void)s;
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	cursor = 0;
	picker_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	focus = (struct gutil_menu_focus) {0};
	pointer_target = -1;
	if(page >= picker_page_count())
		page = 0;
}

static void picker_move_cursor(int dx, int dy) {
	if(cursor >= PICKER_PAGE) {
		/* button row: LEFT/RIGHT walk the buttons, UP returns to the grid */
		if(dx)
			cursor = cursor + dx < PB_PREV
				? PB_PREV
				: (cursor + dx >= PB_COUNT ? PB_COUNT - 1 : cursor + dx);
		if(dy < 0)
			cursor = PICKER_PAGE - PICKER_COLS; /* bottom-left cell */
		return;
	}
	int col = cursor % PICKER_COLS, row = cursor / PICKER_COLS;
	if(dx) {
		col += dx;
		if(col < 0) {
			picker_flip_page(-1);
			col = PICKER_COLS - 1;
		} else if(col >= PICKER_COLS) {
			picker_flip_page(1);
			col = 0;
		}
	}
	if(dy) {
		row += dy;
		if(row < 0)
			row = 0;
		else if(row >= PICKER_ROWS) {
			cursor = PB_PREV; /* into the button row */
			return;
		}
	}
	cursor = row * PICKER_COLS + col;
}

static void screen_creative_picker_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	int width = gfx_width(), height = gfx_height();
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool up = input_pressed(IB_GUI_UP), down = input_pressed(IB_GUI_DOWN);
	bool left = input_pressed(IB_GUI_LEFT), right = input_pressed(IB_GUI_RIGHT);
	bool navigating = up || down || left || right;
	bool pointer_owner = gutil_menu_pointer_focus(&focus, pointer_available,
												  pointer_x, pointer_y,
												  navigating);
	pointer_target = -1;
	if(pointer_available) {
		for(int k = 0; k < PICKER_PAGE; k++) {
			int x, y;
			picker_cell_rect(width, height, k, &x, &y);
			int c = PICKER_CELL * GFX_GUI_SCALE;
			if(pointer_x >= x && pointer_x < x + c && pointer_y >= y
			   && pointer_y < y + c) {
				pointer_target = k;
				break;
			}
		}
		for(int b = PB_PREV; pointer_target < 0 && b < PB_COUNT; b++) {
			int bx, by, bw;
			picker_button_rect(width, height, b, &bx, &by, &bw);
			struct gutil_button_bounds bounds = gutil_button_rect(bx, by, bw);
			if(gutil_button_hit(&bounds, pointer_x, pointer_y))
				pointer_target = b;
		}
	}
	/* An empty grid cell is a hover with nothing behind it: the page simply
	 * has no entry there. It is a DISABLED target -- the press is consumed,
	 * never redirected to whatever the cursor happens to be on. */
	bool hovered_enabled = pointer_target >= PICKER_PAGE
		|| (pointer_target >= 0
			&& picker_cell_has_entry(pointer_target));
	if(pointer_owner && pointer_target >= 0 && hovered_enabled) {
		cursor = pointer_target;
		picker_origin = GUTIL_MENU_ORIGIN_POINTER;
	}

	if(up)
		picker_move_cursor(0, -1);
	if(down)
		picker_move_cursor(0, 1);
	if(left)
		picker_move_cursor(-1, 0);
	if(right)
		picker_move_cursor(1, 0);
	if(navigating)
		picker_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;

	/* The ONE activation path (ruling 2 item 1). `cursor` is exactly what the
	 * grid draws highlighted (picker_draw_frame), so the decision and the
	 * highlight cannot name different cells. */
	bool activate_pressed = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = activate_pressed,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = picker_origin,
			.hovered = pointer_target < 0 ? GUTIL_MENU_TARGET_NONE
										  : pointer_target,
			.hovered_enabled = hovered_enabled,
			.selected = cursor,
			.selected_enabled = cursor >= PICKER_PAGE
				|| (cursor >= 0 && picker_cell_has_entry(cursor)),
			/* R-D: this screen's targets open screens, they do not destroy
			 * anything by themselves. Select World's Delete and Multiplayer's
			 * Delete open a CONFIRMATION -- the destructive answer lives
			 * there, and that dialog declares it. */
			.selected_destructive = false,
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY)
		cursor = decision.selected;
	picker_origin = decision.selection_origin;
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		picker_activate(decision.target);
		return;
	}
	/* R-C: a consumed press reaches nothing else this frame. */
	if(activate_pressed)
		return;

	if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME)) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		screen_set(&screen_ingame);
	}
}

static void picker_draw_frame(int x, int y, int size, uint8_t r, uint8_t g,
							  uint8_t b, uint8_t a) {
	int t = GFX_GUI_SCALE; /* frame thickness */
	gutil_texquad_col(x, y, 0, 0, 0, 0, size, t, r, g, b, a);
	gutil_texquad_col(x, y + size - t, 0, 0, 0, 0, size, t, r, g, b, a);
	gutil_texquad_col(x, y, 0, 0, 0, 0, t, size, r, g, b, a);
	gutil_texquad_col(x + size - t, y, 0, 0, 0, 0, t, size, r, g, b, a);
}

static void screen_creative_picker_render2D(struct screen* s, int width,
											int height) {
	(void)s;
	int gx, gy;
	picker_grid_origin(width, height, &gx, &gy);
	int sc = GFX_GUI_SCALE;

	/* dark world overlay + a slightly darker panel behind the grid */
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gutil_texquad_col(gx - 8 * sc, gy - 26 * sc, 0, 0, 0, 0,
					  (PICKER_COLS * PICKER_CELL + 16) * sc,
					  (26 + PICKER_ROWS * PICKER_CELL + 34) * sc, 0, 0, 0,
					  120);
	gfx_texture(true);

	gutil_text(gx, gy - 18 * sc, "Creative", 8 * sc, true);
	char page_label[32];
	snprintf(page_label, sizeof(page_label), "%u/%u", (unsigned)(page + 1),
			 (unsigned)picker_page_count());
	gutil_text(gx + (PICKER_COLS * PICKER_CELL) * sc
				   - gutil_font_width(page_label, 8 * sc),
			   gy - 18 * sc, page_label, 8 * sc, true);

	/* cells */
	struct item_data hovered = {0};
	bool have_hovered = false;
	for(int k = 0; k < PICKER_PAGE; k++) {
		int x, y;
		picker_cell_rect(width, height, k, &x, &y);
		struct item_data it;
		if(!creative_picker_entry(page * PICKER_PAGE + (size_t)k, &it))
			break;
		gutil_draw_item(&it, x + 1 * sc, y + 1 * sc, 0);
		if(cursor == k) {
			gfx_texture(false);
			picker_draw_frame(x, y, PICKER_CELL * sc, 0xFF, 0xFF, 0xFF, 200);
			gfx_texture(true);
			hovered = it;
			have_hovered = true;
		}
	}

	/* hovered item name under the grid (registry name; the picker offers
	 * only registered, renderable entries) */
	if(have_hovered) {
		struct item* it = item_get(&hovered);
		if(it)
			gutil_text(gx,
					   gy + (PICKER_ROWS * PICKER_CELL + 6) * sc + 26 * sc,
					   it->name, 8 * sc, true);
	}

	gfx_bind_texture(&texture_gui_widgets);
	static const char* const button_labels[]
		= {"<", ">", "Inventory", "Done"};
	for(int b = PB_PREV; b < PB_COUNT; b++) {
		int bx, by, bw;
		picker_button_rect(width, height, b, &bx, &by, &bw);
		gutil_button_draw(bx, by, bw, button_labels[b - PB_PREV], true,
						  cursor == b);
		gfx_bind_texture(&texture_gui_widgets);
	}

	/* The centred hint row (controller UI redesign 10-31, A1 helper). */
	{
		struct input_player_frame hint_f;
		input_prompt_frame(0, &hint_f);
		const struct gutil_hint_item hint_items[] = {
			{{IB_GUI_UP}, 1, "Select", 0, false},
			{{IB_GUI_CLICK}, 1, "Grant", 1, false},
			{{IB_INVENTORY}, 1, "Leave", 2, true}};
		struct gutil_hint_row hint_row;
		if(gutil_hint_row_layout(&hint_f, 0, 0, width, height, false,
				GUTIL_HINT_ALIGN_CENTRE, hint_items, 3, &hint_row))
			gutil_hint_row_draw(&hint_row);
	}

	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0,
							 0, 256, 256, 48 * sc, 48 * sc);
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* The cell/button the picker draws HIGHLIGHTED -- literally `cursor`, the same
 * variable the decision reads (A3 NEW-7). */
int screen_picker_test_highlight(void) {
	return cursor;
}
#endif

struct screen screen_creative_picker = {
	.reset = screen_creative_picker_reset,
	.update = screen_creative_picker_update,
	.render2D = screen_creative_picker_render2D,
	.render3D = NULL,
	.render_world = true,
};
