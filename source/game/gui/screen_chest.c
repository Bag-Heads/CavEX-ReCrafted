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

#include <stdio.h>

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
#include "chest_layout.h"
#include "screen.h"
#include "container_menu.h"
#include "hud_survival_row.h"
#include "../../platform/input_glyph.h"
#include "container_compact.h"

/* Frozen chest.html:10-18,152 overrides presentation, never wire slot IDs.
 * Q7 (maintainer 2026-10-31): the 90-slot double chest is shown ONE 27-slot
 * half at a time in this same 3-row panel (Section L/R and the header's
 * "<" ">" arrows swap halves), so every chest fits above the gameplay
 * hearts + hotbar and the hint row. */
#define GUI_WIDTH 174
#define GUI_HEIGHT 160
#define CHEST_SHOWN_ROWS (CHEST_SIZE_STORAGE / INVENTORY_SIZE_HOTBAR) /* 3 */

// 27 main + 9 hotbar -- the player half of any chest window, whatever its storage size.
#define CHEST_PLAYER_SLOTS (INVENTORY_SIZE_MAIN + INVENTORY_SIZE_HOTBAR)

// 27 storage + 36 player slots are ever shown at once; the half is menu.page.
static struct container_menu menu;
static struct container_slot slots[CHEST_SIZE];
static size_t slots_index;
static unsigned shown_half; /* the half slots[] was filled for */
static uint8_t chest_container;

/* menus_dwell walker (menu_autopilot.c): is the Sort dialog open on the chest. */
bool screen_chest_sort_open(void) {
	return menu.sort_open;
}

void screen_chest_set_windowc(uint8_t container) {
	chest_container = container;
}

// ROW COUNT AT RUNTIME, from the open window's size -- exactly Beta's
// `inventoryRows = lowerChestInventory.getSizeInventory() / 9` (GuiChest.java:17). A lone
// chest is 27 storage + 27 main + 9 hotbar = 63 slots -> 3 rows; an adjacent pair is
// 54 + 36 = 90 -> 6 rows. ONE screen serving both is what let the double chest stop
// borrowing the fork's iron-chest GUI (see block_chest.c onRightClick).
static int chest_storage_rows(void) {
	struct window_container* w = gstate.players[0].windows[chest_container];
	// Fall back to the single chest when the container is not up yet: reset() can run
	// before it exists, and 3 rows is the shape every earlier build assumed.
	if(!w || w->slot_count <= CHEST_PLAYER_SLOTS)
		return CHEST_SIZE_STORAGE / INVENTORY_SIZE_HOTBAR;
	int rows = (int)((w->slot_count - CHEST_PLAYER_SLOTS) / INVENTORY_SIZE_HOTBAR);
	// Never exceed what slots[] and the sheet can hold (Beta's largest chest is 6 rows).
	const int max_rows = (IRON_CHEST_SIZE - CHEST_PLAYER_SLOTS) / INVENTORY_SIZE_HOTBAR;
	return rows > max_rows ? max_rows : rows;
}

// Rows DRAWN: always the small chest's 3 (Q7 pages a 6-row window into them).
static int chest_rows(void) {
	return CHEST_SHOWN_ROWS;
}

// The wire slot bases are pure functions of the row count -- chest_layout.c, golden-tested.
static int chest_storage_count(void) { return chest_gui_storage_count(chest_storage_rows()); }
static int chest_main_base(void) { return chest_gui_main_base(chest_storage_rows()); }
static int chest_hotbar_base(void) { return chest_gui_hotbar_base(chest_storage_rows()); }
static unsigned chest_half_count(void) { return chest_storage_count() > 27 ? 2 : 1; }

static void chest_fill_slots(void) {
	slots_index = 0;
	shown_half = menu.page;

	const int rows = chest_rows();
	for(int k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		int sx, sy;
		sx = 7 + (k % 9) * 18;
		sy = 29 + rows * 18 + (k / 9) * 18;
		slots[slots_index++] = (struct container_slot) {
			.x = sx * GFX_GUI_SCALE,
			.y = sy * GFX_GUI_SCALE,
			.slot = (size_t)(k + chest_main_base()),
		};
	}

	for(int k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		int sx, sy;
		sx = 7 + k * 18;
		sy = 85 + rows * 18;
		slots[slots_index++] = (struct container_slot) {
			.x = sx * GFX_GUI_SCALE,
			.y = sy * GFX_GUI_SCALE,
			.slot = (size_t)(k + chest_hotbar_base()),
		};
	}

	// The shown half: storage 0..26 on page 0, 27..53 on page 1 (Q7).
	for(int k = 0; k < chest_gui_storage_count(rows); k++) {
		int sx, sy;
		sx = 7 + (k % 9) * 18;
		sy = 17 + (k / 9) * 18;
		slots[slots_index++] = (struct container_slot) {
			.x = sx * GFX_GUI_SCALE,
			.y = sy * GFX_GUI_SCALE,
			.slot = (size_t)(k + shown_half * CHEST_SIZE_STORAGE + CHEST_SLOT_STORAGE),
		};
	}
}

/* Reset focus on the player's live hotbar slot (the hotbar row follows the
 * main rows in slots[]). */
static void chest_focus_hotbar(void) {
	unsigned hotbar = inventory_get_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]));
	menu.selected = INVENTORY_SIZE_MAIN + (hotbar < INVENTORY_SIZE_HOTBAR ? hotbar : 0);
}

static void screen_chest_reset(struct screen* s, int width, int height) {
	input_pointer_enable(true);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	s->render3D = screen_ingame.render3D;
	if(container_compact_screen_open(&gstate.players[0].container, chest_container, width, height))
		return;

	menu = (struct container_menu) {0};
	menu.page_count = chest_half_count();

	chest_fill_slots();
	chest_focus_hotbar();
}

static void screen_chest_update(struct screen* s, float dt) {
	if(gstate.players[0].container.active) {
		container_compact_screen_update(&gstate.players[0].container, chest_container, dt);
		return;
	}
	(void)s;
	container_menu_update(&menu, slots, slots_index,
		(gfx_width() - GUI_WIDTH * GFX_GUI_SCALE) / 2,
		(gfx_height() - 480) / 2 + 24, chest_container, dt);
	/* Q7: Section L/R or a header arrow swapped the half; the cells keep
	 * their places (and the focus its cell), only the storage ids change. */
	if(menu.page != shown_half) chest_fill_slots();
}

static void screen_chest_render2D(struct screen* s, int width, int height) {
	if(gstate.players[0].container.active) {
		container_compact_screen_render(&gstate.players[0].container, chest_container);
		return;
	}
	struct inventory* inv
		= windowc_get_latest(gstate.players[0].windows[chest_container]);

	// darken background
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gfx_texture(true);
	/* B3 (10-31): every chest keeps hearts + hotbar at the gameplay positions;
	 * the 3-row panel (24..344) ends above the hearts (Q7 pages the double
	 * chest into it). */
	if(height >= 480)
		hud_menu_draw_owned(&gstate.players[0], width, height);

	int off_x = (width - GUI_WIDTH * GFX_GUI_SCALE) / 2;
	int off_y = (height - 480) / 2 + 24;

	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	if(menu.sort_open) {
		container_menu_sort_render(&menu, gstate.players[0].windows[chest_container], 0, 0, width, height, &prompts);
		goto pointer;
	}

	container_chest_panel(off_x, off_y, 348, GUI_HEIGHT * GFX_GUI_SCALE);
	container_chest_header_paged(off_x, off_y, 348,
		container_chest_title(&menu, gstate.players[0].windows[chest_container]),
		menu.page_count > 1, &prompts);
	gutil_text_rgb(off_x + 12, off_y + 36 + chest_rows() * 36,
		"Inventory", 16, false, 64, 64, 64);
	for(size_t k = 0; k < slots_index; k++)
		container_chest_slot(off_x + slots[k].x, off_y + slots[k].y);

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
	if(!menu.sort_focus) gutil_menu_focus_border(&focus_bounds);
	container_menu_row(chest_container);

	struct item_data item;
	if(inventory_get_picked_item(inv, &item)) {
		if(menu.pointer_available && menu.pointer_on_slot) {
			gutil_draw_item(&item, menu.pointer_x - 8 * GFX_GUI_SCALE, menu.pointer_y - 8 * GFX_GUI_SCALE, 0);
		} else {
			gutil_draw_item(&item, off_x + selection->x, off_y + selection->y,
							0);
		}
	}
	char label[128];
	container_menu_label(&menu, gstate.players[0].windows[chest_container],
		selection->slot, label, sizeof(label));
	container_chest_info((width - 640) / 2 + 506, (height - 480) / 2 + 32, 126, 0, label);

	container_menu_sort_render(&menu, gstate.players[0].windows[chest_container], 0, 0, width, height, &prompts);

pointer:
	if(menu.pointer_available && menu.focus.pointer_owner) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(menu.pointer_x, menu.pointer_y, glm_rad(menu.pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_chest = {
	.reset = screen_chest_reset,
	.update = screen_chest_update,
	.render2D = screen_chest_render2D,
	.render3D = NULL,
	.render_world = true,
};
