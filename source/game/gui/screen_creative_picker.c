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
#include "screen_creative_picker.h"

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

/* Single-player keeps its historical state; split state lives in each host. */
static struct creative_picker solo;

static int picker_rows(const struct creative_picker* p) {
	return p->compact ? 2 : PICKER_ROWS;
}
static int picker_page_size(const struct creative_picker* p) {
	return PICKER_COLS * picker_rows(p);
}

static size_t picker_page_count(const struct creative_picker* p) {
	size_t total = creative_picker_count();
	size_t pages = (total + picker_page_size(p) - 1) / picker_page_size(p);
	return pages ? pages : 1;
}

/* Grid origin in framebuffer pixels. */
static void picker_grid_origin(struct creative_picker* p, int width, int height, int* gx, int* gy) {
	(void)height;
	*gx = (width - PICKER_COLS * PICKER_CELL * GFX_GUI_SCALE) / 2;
	*gy = (p->compact ? 16 : 40) * GFX_GUI_SCALE;
}

static void picker_cell_rect(struct creative_picker* p, int width, int height, int cell, int* x, int* y) {
	int gx, gy;
	picker_grid_origin(p, width, height, &gx, &gy);
	*x = gx + (cell % PICKER_COLS) * PICKER_CELL * GFX_GUI_SCALE;
	*y = gy + (cell / PICKER_COLS) * PICKER_CELL * GFX_GUI_SCALE;
}

/* Button geometry, one row under the grid: [<] [>] ... [Inventory] [Done]. */
static void picker_button_rect(struct creative_picker* p, int width, int height, int which, int* bx,
							   int* by, int* bw) {
	int gx, gy;
	picker_grid_origin(p, width, height, &gx, &gy);
	int s = GFX_GUI_SCALE;
	*by = gy + (picker_rows(p) * PICKER_CELL + 6) * s;
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

static void picker_flip_page(struct creative_picker* p, int dir) {
	size_t pages = picker_page_count(p);
	p->page = (p->page + pages + (size_t)(dir < 0 ? pages - 1 : 1)) % pages;
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
}

/* A grid cell is a real target only while this page actually has an entry in
 * it; the tail of the last page is empty space wearing a cell's coordinates.
 * The four PB_* buttons are always live. [ruling 2 item 2: only enabled,
 * currently valid targets may activate.] */
static bool picker_cell_has_entry(struct creative_picker* p, int cell) {
	struct item_data it;
	return cell >= 0 && cell < picker_page_size(p)
		&& creative_picker_entry(p->page * picker_page_size(p) + (size_t)cell, &it);
}

static void picker_grant(struct creative_picker* p, int cell, container_send_fn send, void* user) {
	struct item_data it;
	size_t index = p->page * picker_page_size(p) + (size_t)cell;
	if(!creative_picker_entry(index, &it))
		return;
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	send(user, &(struct server_rpc) {
		.type = SRPC_CREATIVE_GIVE,
		.payload.creative_give.id = it.id,
		.payload.creative_give.durability = it.durability,
	});
}

static enum creative_picker_action picker_activate(struct creative_picker* p, int target, container_send_fn send, void* user) {
	switch(target) {
		case PB_PREV: picker_flip_page(p, -1); break;
		case PB_NEXT: picker_flip_page(p, 1); break;
		case PB_INVENTORY:
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			return CREATIVE_PICKER_INVENTORY;
		case PB_DONE:
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			return CREATIVE_PICKER_CLOSE;
		default: picker_grant(p, target, send, user); break;
	}
	return CREATIVE_PICKER_KEEP;
}

static void screen_creative_picker_reset(struct screen* s, int width,
										 int height) {
	(void)s;
	struct creative_picker* p = &solo;
	p->compact = false;
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	p->cursor = 0;
	p->picker_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	p->focus = (struct gutil_menu_focus) {0};
	p->pointer_target = -1;
	if(p->page >= picker_page_count(p))
		p->page = 0;
}

static void picker_move_cursor(struct creative_picker* p, int dx, int dy) {
	if(p->cursor >= PICKER_PAGE) {
		/* button row: LEFT/RIGHT walk the buttons, UP returns to the grid */
		if(dx)
			p->cursor = p->cursor + dx < PB_PREV
				? PB_PREV
				: (p->cursor + dx >= PB_COUNT ? PB_COUNT - 1 : p->cursor + dx);
		if(dy < 0)
			p->cursor = picker_page_size(p) - PICKER_COLS; /* bottom-left cell */
		return;
	}
	int col = p->cursor % PICKER_COLS, row = p->cursor / PICKER_COLS;
	if(dx) {
		col += dx;
		if(col < 0) {
			picker_flip_page(p, -1);
			col = PICKER_COLS - 1;
		} else if(col >= PICKER_COLS) {
			picker_flip_page(p, 1);
			col = 0;
		}
	}
	if(dy) {
		row += dy;
		if(row < 0)
			row = 0;
		else if(row >= picker_rows(p)) {
			p->cursor = PB_PREV; /* into the button row */
			return;
		}
	}
	p->cursor = row * PICKER_COLS + col;
}

static enum creative_picker_action picker_update(struct creative_picker* p,
	const struct input_player_frame* frame, int width, int height,
	container_send_fn send, void* user) {
	p->pointer_available = frame->pointer_valid;
	p->pointer_x = frame->pointer_x;
	p->pointer_y = frame->pointer_y;
	p->pointer_angle = frame->pointer_angle;
	bool up = frame->pressed[IB_GUI_UP], down = frame->pressed[IB_GUI_DOWN];
	bool left = frame->pressed[IB_GUI_LEFT], right = frame->pressed[IB_GUI_RIGHT];
	bool navigating = up || down || left || right;
	bool pointer_owner = gutil_menu_pointer_focus(&p->focus, p->pointer_available,
												  p->pointer_x, p->pointer_y,
												  navigating);
	p->pointer_target = -1;
	if(p->pointer_available) {
		for(int k = 0; k < picker_page_size(p); k++) {
			int x, y;
			picker_cell_rect(p, width, height, k, &x, &y);
			int c = PICKER_CELL * GFX_GUI_SCALE;
			if(p->pointer_x >= x && p->pointer_x < x + c && p->pointer_y >= y
			   && p->pointer_y < y + c) {
				p->pointer_target = k;
				break;
			}
		}
		for(int b = PB_PREV; p->pointer_target < 0 && b < PB_COUNT; b++) {
			int bx, by, bw;
			picker_button_rect(p, width, height, b, &bx, &by, &bw);
			struct gutil_button_bounds bounds = gutil_button_rect(bx, by, bw);
			if(gutil_button_hit(&bounds, p->pointer_x, p->pointer_y))
				p->pointer_target = b;
		}
	}
	/* An empty grid cell is a hover with nothing behind it: the page simply
	 * has no entry there. It is a DISABLED target -- the press is consumed,
	 * never redirected to whatever the cursor happens to be on. */
	bool hovered_enabled = p->pointer_target >= PICKER_PAGE
		|| (p->pointer_target >= 0
			&& picker_cell_has_entry(p, p->pointer_target));
	if(pointer_owner && p->pointer_target >= 0 && hovered_enabled) {
		p->cursor = p->pointer_target;
		p->picker_origin = GUTIL_MENU_ORIGIN_POINTER;
	}

	if(up)
		picker_move_cursor(p, 0, -1);
	if(down)
		picker_move_cursor(p, 0, 1);
	if(left)
		picker_move_cursor(p, -1, 0);
	if(right)
		picker_move_cursor(p, 1, 0);
	if(navigating)
		p->picker_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;

	/* The ONE activation path (ruling 2 item 1). `cursor` is exactly what the
	 * grid draws highlighted (picker_draw_frame), so the decision and the
	 * highlight cannot name different cells. */
	bool activate_pressed = frame->pressed[IB_GUI_CLICK];
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = activate_pressed,
			.navigated = navigating,
			.pointer_valid = p->pointer_available,
			.selection_origin = p->picker_origin,
			.hovered = p->pointer_target < 0 ? GUTIL_MENU_TARGET_NONE
										  : p->pointer_target,
			.hovered_enabled = hovered_enabled,
			.selected = p->cursor,
			.selected_enabled = p->cursor >= PICKER_PAGE
				|| (p->cursor >= 0 && picker_cell_has_entry(p, p->cursor)),
			/* R-D: this screen's targets open screens, they do not destroy
			 * anything by themselves. Select World's Delete and Multiplayer's
			 * Delete open a CONFIRMATION -- the destructive answer lives
			 * there, and that dialog declares it. */
			.selected_destructive = false,
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY)
		p->cursor = decision.selected;
	p->picker_origin = decision.selection_origin;
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		return picker_activate(p, decision.target, send, user);
	}
	/* R-C: a consumed press reaches nothing else this frame. */
	if(activate_pressed)
		return CREATIVE_PICKER_KEEP;

	if(frame->pressed[IB_INVENTORY] || frame->pressed[IB_HOME]) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		return CREATIVE_PICKER_CLOSE;
	}
	return CREATIVE_PICKER_KEEP;
}

static void picker_draw_frame(int x, int y, int size, uint8_t r, uint8_t g,
							  uint8_t b, uint8_t a) {
	int t = GFX_GUI_SCALE; /* frame thickness */
	gutil_texquad_col(x, y, 0, 0, 0, 0, size, t, r, g, b, a);
	gutil_texquad_col(x, y + size - t, 0, 0, 0, 0, size, t, r, g, b, a);
	gutil_texquad_col(x, y, 0, 0, 0, 0, t, size, r, g, b, a);
	gutil_texquad_col(x + size - t, y, 0, 0, 0, 0, t, size, r, g, b, a);
}

void creative_picker_render(struct creative_picker* p, unsigned player,
	int width, int height) {
	int gx, gy;
	picker_grid_origin(p, width, height, &gx, &gy);
	int sc = GFX_GUI_SCALE;

	/* dark world overlay + a slightly darker panel behind the grid */
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gutil_texquad_col(gx - 8 * sc, gy - (p->compact ? 16 : 26) * sc, 0, 0, 0, 0,
					  (PICKER_COLS * PICKER_CELL + 16) * sc,
					  ((p->compact ? 16 : 26) + picker_rows(p) * PICKER_CELL + 34) * sc, 0, 0, 0,
					  120);
	gfx_texture(true);

	gutil_text(p->compact ? 24 : gx, gy - (p->compact ? 12 : 18) * sc, "Creative", 8 * sc, true);
	char page_label[32];
	snprintf(page_label, sizeof(page_label), "%u/%u", (unsigned)(p->page + 1),
			 (unsigned)picker_page_count(p));
	int page_x = (p->compact ? width - 24 : gx + PICKER_COLS * PICKER_CELL * sc)
		- gutil_font_width(page_label, 8 * sc);
	gutil_text(page_x, gy - (p->compact ? 12 : 18) * sc, page_label, 8 * sc, true);
	if(p->compact)
		gutil_connection_notices_in_header(player, true, 140, 8, page_x - 156, 24);

	/* cells */
	struct item_data hovered = {0};
	bool have_hovered = false;
	for(int k = 0; k < picker_page_size(p); k++) {
		int x, y;
		picker_cell_rect(p, width, height, k, &x, &y);
		struct item_data it;
		if(!creative_picker_entry(p->page * picker_page_size(p) + (size_t)k, &it))
			break;
		gutil_draw_item(&it, x + 1 * sc, y + 1 * sc, 0);
		if(p->cursor == k) {
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
					   gy + (picker_rows(p) * PICKER_CELL + 6) * sc + 26 * sc,
					   it->name, 8 * sc, true);
	}

	gfx_bind_texture(&texture_gui_widgets);
	static const char* const button_labels[]
		= {"<", ">", "Inventory", "Done"};
	for(int b = PB_PREV; b < PB_COUNT; b++) {
		int bx, by, bw;
		picker_button_rect(p, width, height, b, &bx, &by, &bw);
		gutil_button_draw(bx, by, bw, button_labels[b - PB_PREV], true,
						  p->cursor == b);
		gfx_bind_texture(&texture_gui_widgets);
	}

	/* The centred hint row (controller UI redesign 10-31, A1 helper). */
	{
		struct input_player_frame hint_f;
		input_prompt_frame(player, &hint_f);
		const struct gutil_hint_item hint_items[] = {
			{{IB_GUI_UP}, 1, "Select", 0, false},
			{{IB_GUI_CLICK}, 1, "Grant", 1, false},
			{{IB_INVENTORY}, 1, "Leave", 2, true}};
		struct gutil_hint_row hint_row;
		if(gutil_hint_row_layout(&hint_f, 0, 0, width, height, p->compact,
				GUTIL_HINT_ALIGN_CENTRE, hint_items, 3, &hint_row))
			gutil_hint_row_draw(&hint_row);
	}

	if(p->pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(p->pointer_x, p->pointer_y, glm_rad(p->pointer_angle), 0,
							 0, 256, 256, 48 * sc, 48 * sc);
	}
}

void creative_picker_open(struct creative_picker* p) {
	*p = (struct creative_picker){.compact = true, .pointer_target = -1};
}

enum creative_picker_action creative_picker_update(struct creative_picker* p,
	const struct input_player_frame* frame, int x, int y, int width, int height,
	container_send_fn send, void* user) {
	if(!frame || !frame->connected) {
		p->suspended = true;
		return CREATIVE_PICKER_KEEP;
	}
	/* Reconnect latch (same rule as the compact container views): after a
	 * loss, ignore input until every button has been released, so a button
	 * still held from before cannot grant an item or press Done. */
	if(p->suspended) {
		for(int b = 0; b < IB_COUNT; b++)
			if(frame->down[b] || frame->pressed[b]) return CREATIVE_PICKER_KEEP;
		p->suspended = false;
	}
	struct input_player_frame local = *frame;
	local.pointer_valid = input_owner_pointer(frame, x, y, width, height,
		&local.pointer_x, &local.pointer_y, &local.pointer_angle);
	return picker_update(p, &local, width, height, send, user);
}

/* Keep the selected catalogue entry when a partner joins or leaves. */
static void picker_change_layout(struct creative_picker* p, bool compact) {
	size_t first = p->page * picker_page_size(p);
	bool cell = p->cursor < PICKER_PAGE;
	if(cell) first += p->cursor;
	const size_t count = creative_picker_count();
	if(first >= count) first = count ? count - 1 : 0;
	p->compact = compact;
	p->page = first / picker_page_size(p);
	if(cell) p->cursor = first % picker_page_size(p);
	p->pointer_available = false;
	p->pointer_target = -1;
	p->focus = (struct gutil_menu_focus){.initialized = true};
}
void screen_creative_picker_to_compact(struct creative_picker* p) {
	*p = solo;
	picker_change_layout(p, true);
}
void screen_creative_picker_from_compact(const struct creative_picker* p) {
	solo = *p;
	picker_change_layout(&solo, false);
}
static bool picker_solo_send(void* user, struct server_rpc* rpc) {
	(void)user;
	return svin_rpc_send(rpc);
}
static void screen_creative_picker_update(struct screen* s, float dt) {
	(void)s; (void)dt;
	struct input_player_frame frame = {0};
	frame.pointer_valid = input_pointer(&frame.pointer_x, &frame.pointer_y,
		&frame.pointer_angle);
	const enum input_button buttons[] = {IB_GUI_UP, IB_GUI_DOWN, IB_GUI_LEFT,
		IB_GUI_RIGHT, IB_GUI_CLICK, IB_INVENTORY, IB_HOME};
	for(size_t k = 0; k < sizeof buttons / sizeof *buttons; k++)
		frame.pressed[buttons[k]] = input_pressed(buttons[k]);
	enum creative_picker_action action = picker_update(&solo, &frame,
		gfx_width(), gfx_height(), picker_solo_send, NULL);
	if(action == CREATIVE_PICKER_INVENTORY) screen_set(&screen_inventory);
	else if(action == CREATIVE_PICKER_CLOSE) screen_set(&screen_ingame);
}
static void screen_creative_picker_render2D(struct screen* s, int width, int height) {
	(void)s;
	creative_picker_render(&solo, 0, width, height);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* The cell/button the picker draws HIGHLIGHTED -- literally `cursor`, the same
 * variable the decision reads (A3 NEW-7). */
int screen_picker_test_highlight(void) {
	return solo.cursor;
}
#endif

struct screen screen_creative_picker = {
	.reset = screen_creative_picker_reset,
	.update = screen_creative_picker_update,
	.render2D = screen_creative_picker_render2D,
	.render3D = NULL,
	.render_world = true,
};
