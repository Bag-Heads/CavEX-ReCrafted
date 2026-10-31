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
#include "hud_survival_row.h"
#include "menu_header.h"

#define GUI_WIDTH 176
// Beta GuiContainer.java:9 `protected int ySize = 166;` -- the default every container
// except GuiChest uses. 167 sampled one transparent row past the art (all six GUI
// sheets' last opaque row is v=165) and centred the window 1px high. GuiChest is the
// ONLY container that overrides ySize (to 114 + rows*18) -- see chest_layout.h.
#define GUI_HEIGHT 166

static struct container_menu menu;
static struct container_slot slots[FURNACE_SIZE];
static size_t slots_index;
static uint8_t furnace_container;

/* APPROVED FURNACE COMPOSITION -- docs/ui_design/approved/menus.html
 * (menu selector -> Furnace) and specs/menus.md, "Furnace and dispenser":
 *
 *   "Show machine slots AND all 36 carried slots, with item names and
 *    remembered selection. Never restrict transfer sources to the hotbar.
 *    Keep the approved positioning/spacing and split-screen readability; the
 *    machine, bag, selection label and button hints have distinct space.
 *    Sound for item movement must follow a completed authoritative move."
 *
 * Both halves are already true here: the FULL layout is Beta's own
 * GuiFurnace panel (3 machine + 27 main + 9 hotbar = all 36 carried slots,
 * `container_menu_item_name` for the name, `container_menu`'s commit-timed
 * item cue for the sound), and the COMPACT layout is container_compact's
 * approved split composition. What this lane adds is the third approved
 * requirement, the one the base tree had no answer for: WHERE a controller
 * notice composes so it never covers a slot, a prompt or the HUD
 * (menu_header.h), plus the split-composition admission gate below. */
static void furnace_compose(struct menu_composition* c, int width, int height,
							int off_x, int off_y) {
	menu_composition_begin(c, MENU_SCREEN_FURNACE, 0, 0, width, height);
 if(gstate.players[0].container.active) {
  bool full=height>=480;int bx=(width-640)/2+(full?134:32),by=(height-(full?480:240))/2+(full?30:8); /* B3: 44 -> 30 */
  int bw=full?372:576;
  menu_composition_window(c,bx,by,bw,full?332:188);
  menu_composition_header(c,bx+12,by+8,bw-24,24);
  for(size_t k=0;k<gstate.players[0].container.count;k++)
   menu_composition_action(c,gstate.players[0].container.slots[k].x,gstate.players[0].container.slots[k].y,36,36);
  menu_composition_header_notice(c,gutil_connection_notice_width(0,gstate.players[1].local_player!=NULL,true),gutil_font_width("Furnace",16));
 menu_composition_seal(c);return;
 }

	menu_composition_window(c, off_x, off_y, GUI_WIDTH * GFX_GUI_SCALE,
							GUI_HEIGHT * GFX_GUI_SCALE);
	/* Beta draws its title at (60,6) in the panel; that strip is the header. */
	menu_composition_header(c, off_x, off_y, GUI_WIDTH * GFX_GUI_SCALE,
							16 * GFX_GUI_SCALE);
	for(size_t k = 0; k < slots_index; k++)
		menu_composition_action(c, off_x + slots[k].x, off_y + slots[k].y,
								16 * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE);
	menu_composition_header_notice(c,gutil_connection_notice_width(0,gstate.players[1].local_player!=NULL,true),gutil_font_width("Furnace",16));
 menu_composition_seal(c);
}

void screen_furnace_set_windowc(uint8_t container) {
	furnace_container = container;
}

static void screen_furnace_reset(struct screen* s, int width, int height) {
	input_pointer_enable(true);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	s->render3D = screen_ingame.render3D;
 /* Both viewport sizes use the approved simultaneous machine and bag. */
 if(container_compact_screen_open(&gstate.players[0].container,furnace_container,width,height))return;
	gstate.players[0].container.active = false;

	menu = (struct container_menu) {0};

	slots_index = 0;

	for(int k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (8 + (k % INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.y = (84 + (k / INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.slot = k + FURNACE_SLOT_MAIN,
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
			.slot = k + FURNACE_SLOT_HOTBAR,
		};
	}

	slots[slots_index++] = (struct container_slot) {
		.x = 56 * GFX_GUI_SCALE,
		.y = 17 * GFX_GUI_SCALE,
		.slot = FURNACE_SLOT_INPUT + 0,
	};

	slots[slots_index++] = (struct container_slot) {
		.x = 56 * GFX_GUI_SCALE,
		.y = 53 * GFX_GUI_SCALE,
		.slot = FURNACE_SLOT_INPUT + 1,
	};

	slots[slots_index++] = (struct container_slot) {
		.x = 116 * GFX_GUI_SCALE,
		.y = 35 * GFX_GUI_SCALE,
		.slot = FURNACE_SLOT_OUTPUT,
	};
}

static void screen_furnace_update(struct screen* s, float dt) {
	if(gstate.players[0].container.active) {
		container_compact_screen_update(&gstate.players[0].container, furnace_container, dt);
		return;
	}
	(void)s;
	container_menu_update(&menu, slots, slots_index,
		(gfx_width() - GUI_WIDTH * GFX_GUI_SCALE) / 2,
		(gfx_height() - GUI_HEIGHT * GFX_GUI_SCALE) / 2, furnace_container, dt);
}

static void screen_furnace_render2D(struct screen* s, int width, int height) {
	if(gstate.players[0].container.active) {
		container_compact_screen_render(&gstate.players[0].container, furnace_container);
		/* B3 (10-31): the HUD is drawn by container_compact_screen_render
		 * at the gameplay positions (hud_menu_draw_owned). */
		struct menu_composition compact;
		furnace_compose(&compact, width, height, 0, 0);
		screen_notice_compose(compact.notice.x,compact.notice.y,compact.notice.w,compact.notice.h,compact.notice_in_header);
		return;
	}
	struct inventory* inv
		= windowc_get_latest(gstate.players[0].windows[furnace_container]);

	// darken background
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gfx_texture(true);

	int off_x = (width - GUI_WIDTH * GFX_GUI_SCALE) / 2;
	int off_y = (height - GUI_HEIGHT * GFX_GUI_SCALE) / 2;

	// draw inventory
	gfx_bind_texture(&texture_gui_furnace);
	gutil_texquad(off_x, off_y, 0, 0, GUI_WIDTH, GUI_HEIGHT, GUI_WIDTH * GFX_GUI_SCALE,
				  GUI_HEIGHT * GFX_GUI_SCALE);

	// Flame + cook-progress arrow (Beta GuiFurnace.drawGuiContainerBackgroundLayer),
	// both sprites live at src x=176 in furnace.png (same bound texture). Progress
	// comes from the server via CRPC_WINDOW_PROGRESS (display-only).
	struct window_container* wc = gstate.players[0].windows[furnace_container];
	if(wc) {
		// flame: Beta draws it whenever isBurning() (furnaceBurnTime > 0), NOT only
		// when the scaled height is nonzero -- the last ~133 coal ticks scale to 0 but
		// Beta still shows the 2px base sliver. getBurnTimeRemainingScaled(12) grows it
		// bottom-up; currentItemBurnTime defaults to 200 when 0 (Beta div-by-zero guard).
		int cibt = wc->furnace_current_item_burn ? wc->furnace_current_item_burn : 200;
		int flame = wc->furnace_burn_time * 12 / cibt;
		if(flame < 0)
			flame = 0;
		if(flame > 12)
			flame = 12;
		if(wc->furnace_burn_time > 0)
			gutil_texquad(off_x + 56 * GFX_GUI_SCALE,
						  off_y + (36 + 12 - flame) * GFX_GUI_SCALE, 176, 12 - flame,
						  14, flame + 2, 14 * GFX_GUI_SCALE,
						  (flame + 2) * GFX_GUI_SCALE);

		// cook arrow: Beta draws it UNCONDITIONALLY (width getCookProgressScaled(24)+1,
		// always >= 1px), growing left-to-right -- the arrow base nub shows even idle.
		int arrow = wc->furnace_cook_time * 24 / 200;
		if(arrow < 0)
			arrow = 0;
		if(arrow > 24)
			arrow = 24;
		gutil_texquad(off_x + 79 * GFX_GUI_SCALE, off_y + 34 * GFX_GUI_SCALE, 176, 14,
					  arrow + 1, 16, (arrow + 1) * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE);
	}

	gutil_text(off_x + 60 * GFX_GUI_SCALE, off_y + 6 * GFX_GUI_SCALE, "\2478Furnace", 8 * GFX_GUI_SCALE, false);

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
	container_menu_row(furnace_container);

	struct item_data item;
	if(inventory_get_picked_item(inv, &item)) {
		if(menu.pointer_available && menu.pointer_on_slot) {
			gutil_draw_item(&item, menu.pointer_x - 8 * GFX_GUI_SCALE, menu.pointer_y - 8 * GFX_GUI_SCALE, 0);
		} else {
			gutil_draw_item(&item, off_x + selection->x, off_y + selection->y,
							0);
		}
	}
	container_menu_item_name(&menu, furnace_container, selection->slot, width);

	/* Approved notice composition for THIS frame (menu_header.h). */
	struct menu_composition composition;
	furnace_compose(&composition, width, height, off_x, off_y);
	screen_notice_compose(composition.notice.x,composition.notice.y,composition.notice.w,composition.notice.h,composition.notice_in_header);

	if(menu.pointer_available && menu.focus.pointer_owner) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(menu.pointer_x, menu.pointer_y, glm_rad(menu.pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_furnace = {
	.reset = screen_furnace_reset,
	.update = screen_furnace_update,
	.render2D = screen_furnace_render2D,
	.render3D = NULL,
	.render_world = true,
};
