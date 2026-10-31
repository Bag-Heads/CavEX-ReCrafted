/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../../graphics/gfx_util.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../graphics/render_model.h"
#include "../../item/items.h" // direct include (was transitive via gui_util.h pre-split)
#include "../../network/server_interface.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/time.h"
#include "../game_state.h"
#include "screen.h"
#include "container_menu.h"
#include "container_compact.h"

#define GUI_WIDTH 300
#define GUI_HEIGHT 190

static struct container_menu menu;
static struct container_slot slots[IRON_CHEST_SIZE];
static size_t slots_index;
static uint8_t iron_chest_container;

void screen_iron_chest_set_windowc(uint8_t container) {
	iron_chest_container = container;
}

static void screen_iron_chest_reset(struct screen* s, int width, int height) {
	input_pointer_enable(true);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	s->render3D = screen_ingame.render3D;
	if(container_compact_screen_open(&gstate.players[0].container, iron_chest_container, width, height))
		return;

	menu = (struct container_menu) {0};

	slots_index = 0;

	for(int k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (8 + (k % INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.y = (84 + (k / INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.slot = k + IRON_CHEST_SLOT_MAIN,
		};
	}

	for(int k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		if(k
		   == (int)inventory_get_hotbar(
			   windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY])))
			menu.selected = slots_index;

		slots[slots_index++] = (struct container_slot) {
			.x = (8 + k * 18) * GFX_GUI_SCALE,
			.y = (84 + 3 * 18 + 4) * GFX_GUI_SCALE,
			.slot = k + IRON_CHEST_SLOT_HOTBAR,
		};
	}

	for(int k = 0; k < IRON_CHEST_SIZE_MAIN_STORAGE; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (8 + (k % INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.y = (16 + (k / INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.slot = k + IRON_CHEST_SLOT_MAIN_STORAGE,
		};
	}

	// TODO: a less ugly GUI which can still fit in 320x200
	// HACK: hardcoded slot counts to awkwardly fit a double chest in 320x200
	for(int k = 0; k < 12; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (170 + (k % 4) * 18) * GFX_GUI_SCALE,
			.y = (16 + (k / 4) * 18) * GFX_GUI_SCALE,
			.slot = k + IRON_CHEST_SIZE_SIDE_STORAGE,
		};
	}
	for(int k = 0; k < 12; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (178 + (k % 4) * 18) * GFX_GUI_SCALE,
			.y = (84 + (k / 4) * 18) * GFX_GUI_SCALE,
			.slot = k + 12 + IRON_CHEST_SIZE_SIDE_STORAGE,
		};
	}
	for(int k = 0; k < 3; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (195 + (k % 4) * 18) * GFX_GUI_SCALE,
			.y = (84 + 3 * 18 + 4) * GFX_GUI_SCALE,
			.slot = k + 24 + IRON_CHEST_SIZE_SIDE_STORAGE,
		};
	}
}

static void screen_iron_chest_update(struct screen* s, float dt) {
	if(gstate.players[0].container.active) {
		container_compact_screen_update(&gstate.players[0].container, iron_chest_container, dt);
		return;
	}
	(void)s;
	container_menu_update(&menu, slots, slots_index,
		(gfx_width() - GUI_WIDTH * GFX_GUI_SCALE) / 2,
		(gfx_height() - GUI_HEIGHT * GFX_GUI_SCALE) / 2, iron_chest_container, dt);
}

static void screen_iron_chest_render2D(struct screen* s, int width, int height) {
	if(gstate.players[0].container.active) {
		container_compact_screen_render(&gstate.players[0].container, iron_chest_container);
		return;
	}
	struct inventory* inv
		= windowc_get_latest(gstate.players[0].windows[iron_chest_container]);

	// darken background
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gfx_texture(true);

	int off_x = (width - GUI_WIDTH * GFX_GUI_SCALE) / 2;
	int off_y = (height - GUI_HEIGHT * GFX_GUI_SCALE) / 2;

	// draw inventory
	gfx_bind_texture(&texture_gui_iron_chest);
	gutil_texquad(off_x, off_y, 0, 0, GUI_WIDTH, GUI_HEIGHT, GUI_WIDTH * GFX_GUI_SCALE,
				  GUI_HEIGHT * GFX_GUI_SCALE);
	gutil_text(off_x + 28 * GFX_GUI_SCALE, off_y + 6 * GFX_GUI_SCALE, "\2478Iron Chest", 8 * GFX_GUI_SCALE, false);

	struct container_slot* selection = slots + menu.selected;

	// draw items
	for(size_t k = 0; k < slots_index; k++) {
		struct item_data item;
		if((menu.selected != k || !inventory_get_picked_item(inv, NULL)
			|| (menu.pointer_available && menu.pointer_on_slot))
		   && inventory_get_slot(inv, slots[k].slot, &item))
			gutil_draw_item(&item, off_x + slots[k].x, off_y + slots[k].y, 1);
	}

	struct gutil_button_bounds focus_bounds = {
		off_x + selection->x, off_y + selection->y,
		16 * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE,
	};
	gutil_menu_focus_border(&focus_bounds);
	container_menu_row(iron_chest_container);

	struct item_data item;
	if(inventory_get_picked_item(inv, &item)) {
		if(menu.pointer_available && menu.pointer_on_slot) {
			gutil_draw_item(&item, menu.pointer_x - 8 * GFX_GUI_SCALE, menu.pointer_y - 8 * GFX_GUI_SCALE, 0);
		} else {
			gutil_draw_item(&item, off_x + selection->x, off_y + selection->y,
							0);
		}
	}
	container_menu_item_name(&menu, iron_chest_container, selection->slot, width);

	if(menu.pointer_available && menu.focus.pointer_owner) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(menu.pointer_x, menu.pointer_y, glm_rad(menu.pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_iron_chest = {
	.reset = screen_iron_chest_reset,
	.update = screen_iron_chest_update,
	.render2D = screen_iron_chest_render2D,
	.render3D = NULL,
	.render_world = true,
};
