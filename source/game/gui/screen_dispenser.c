/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

// Beta 1.7.3 GuiDispenser + ContainerDispenser (decompile 740c583). A FIXED
// (not row-count-driven) 176x166 window -- GuiContainer's plain default size,
// since GuiDispenser overrides neither xSize nor ySize (only GuiChest does,
// for its variable row count -- see chest_layout.h). Slot origins are
// ContainerDispenser's literal addSlot coordinates:
//   storage: 3x3, (62 + col*18, 17 + row*18), slot = col + row*3
//   player main: 3x9, (8 + col*18, 84 + row*18)
//   player hotbar: 9, (8 + col*18, 142)
// drawn as ONE 0,0..xSize,ySize background blit (GuiDispenser does not split
// the sheet the way GuiChest does -- gui/trap.png has no unused rows to
// skip). Title text is the literal "Dispenser" at (60,6) -- UNRELATED to the
// TileEntity's NBT "id" string ("Trap"; see tile_entities_nbt.h) and to
// getInvName() (also "Trap") -- GuiDispenser hardcodes the displayed title
// rather than deriving it. Mirrors screen_furnace.c's fixed-shape structure
// (no row-count math, no progress overlay -- the dispenser has none).

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
#define GUI_HEIGHT 166 // GuiContainer.java default (GuiDispenser overrides neither)

static struct container_menu menu;
static struct container_slot slots[DISPENSER_SIZE];
static size_t slots_index;
static uint8_t dispenser_container;

/* APPROVED DISPENSER COMPOSITION -- docs/ui_design/approved/menus.html
 * (menu selector -> Dispenser) and specs/menus.md, "Furnace and dispenser".
 * Same shape and the same reasoning as screen_furnace.c's note: the machine's
 * 3x3 plus all 36 carried slots, the item name and the commit-timed transfer
 * cue are already the shipping behaviour in both layouts; this lane adds the
 * approved notice/header composition (menu_header.h) and the P2 acceptance
 * gate on the split composition. */
static void dispenser_compose(struct menu_composition* c, int width, int height,
							  int off_x, int off_y) {
	menu_composition_begin(c, MENU_SCREEN_DISPENSER, 0, 0, width, height);
 if(gstate.players[0].container.active) {
  bool full=height>=480;int bx=(width-640)/2+(full?134:32),by=(height-(full?480:240))/2+(full?30:8); /* B3: 44 -> 30 */
  int bw=full?372:576;
  menu_composition_window(c,bx,by,bw,full?332:188);
  menu_composition_header(c,bx+12,by+8,bw-24,24);
  for(size_t k=0;k<gstate.players[0].container.count;k++)
   menu_composition_action(c,gstate.players[0].container.slots[k].x,gstate.players[0].container.slots[k].y,36,36);
  menu_composition_header_notice(c,gutil_connection_notice_width(0,gstate.players[1].local_player!=NULL,true),gutil_font_width("Dispenser",16));
 menu_composition_seal(c);return;
 }

	menu_composition_window(c, off_x, off_y, GUI_WIDTH * GFX_GUI_SCALE,
							GUI_HEIGHT * GFX_GUI_SCALE);
	menu_composition_header(c, off_x, off_y, GUI_WIDTH * GFX_GUI_SCALE,
							16 * GFX_GUI_SCALE);
	for(size_t k = 0; k < slots_index; k++)
		menu_composition_action(c, off_x + slots[k].x, off_y + slots[k].y,
								16 * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE);
	menu_composition_header_notice(c,gutil_connection_notice_width(0,gstate.players[1].local_player!=NULL,true),gutil_font_width("Dispenser",16));
 menu_composition_seal(c);
}

void screen_dispenser_set_windowc(uint8_t container) {
	dispenser_container = container;
}

static void screen_dispenser_reset(struct screen* s, int width, int height) {
	input_pointer_enable(true);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	s->render3D = screen_ingame.render3D;
 /* Both viewport sizes use the approved simultaneous machine and bag. */
 if(container_compact_screen_open(&gstate.players[0].container,dispenser_container,width,height))return;
	gstate.players[0].container.active = false;

	menu = (struct container_menu) {0};

	slots_index = 0;

	// ContainerDispenser.java:9-11 -- 3x3 storage at (62+col*18, 17+row*18).
	for(int row = 0; row < 3; row++) {
		for(int col = 0; col < 3; col++) {
			slots[slots_index++] = (struct container_slot) {
				.x = (62 + col * 18) * GFX_GUI_SCALE,
				.y = (17 + row * 18) * GFX_GUI_SCALE,
				.slot = (size_t)(col + row * 3) + DISPENSER_SLOT_STORAGE,
			};
		}
	}

	for(int k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (8 + (k % INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.y = (84 + (k / INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.slot = k + DISPENSER_SLOT_MAIN,
		};
	}

	for(int k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		if(k
		   == (int)inventory_get_hotbar(
			   windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY])))
			menu.selected = slots_index;

		slots[slots_index++] = (struct container_slot) {
			.x = (8 + k * 18) * GFX_GUI_SCALE,
			.y = 142 * GFX_GUI_SCALE,
			.slot = k + DISPENSER_SLOT_HOTBAR,
		};
	}
}

static void screen_dispenser_update(struct screen* s, float dt) {
	if(gstate.players[0].container.active) {
		container_compact_screen_update(&gstate.players[0].container, dispenser_container, dt);
		return;
	}
	(void)s;
	container_menu_update(&menu, slots, slots_index,
		(gfx_width() - GUI_WIDTH * GFX_GUI_SCALE) / 2,
		(gfx_height() - GUI_HEIGHT * GFX_GUI_SCALE) / 2, dispenser_container, dt);
}

static void screen_dispenser_render2D(struct screen* s, int width, int height) {
	if(gstate.players[0].container.active) {
		container_compact_screen_render(&gstate.players[0].container, dispenser_container);
		/* B3 (10-31): the HUD is drawn by container_compact_screen_render
		 * at the gameplay positions (hud_menu_draw_owned). */
		struct menu_composition compact;
		dispenser_compose(&compact, width, height, 0, 0);
		screen_notice_compose(compact.notice.x,compact.notice.y,compact.notice.w,compact.notice.h,compact.notice_in_header);
		return;
	}
	struct inventory* inv
		= windowc_get_latest(gstate.players[0].windows[dispenser_container]);

	// darken background
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gfx_texture(true);

	int off_x = (width - GUI_WIDTH * GFX_GUI_SCALE) / 2;
	int off_y = (height - GUI_HEIGHT * GFX_GUI_SCALE) / 2;

	// draw inventory -- ONE blit (GuiDispenser.drawGuiContainerBackgroundLayer:
	// drawTexturedModalRect(x, y, 0, 0, xSize, ySize), no row-skip split).
	gfx_bind_texture(&texture_gui_dispenser);
	gutil_texquad(off_x, off_y, 0, 0, GUI_WIDTH, GUI_HEIGHT,
				 GUI_WIDTH * GFX_GUI_SCALE, GUI_HEIGHT * GFX_GUI_SCALE);

	// GuiDispenser.drawGuiContainerForegroundLayer: "Dispenser" at (60,6),
	// "Inventory" at (8, ySize-96+2) = (8,72).
	gutil_text(off_x + 60 * GFX_GUI_SCALE, off_y + 6 * GFX_GUI_SCALE,
			  "\2478Dispenser", 8 * GFX_GUI_SCALE, false);
	gutil_text(off_x + 8 * GFX_GUI_SCALE, off_y + 72 * GFX_GUI_SCALE,
			  "\2478Inventory", 8 * GFX_GUI_SCALE, false);

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
	container_menu_row(dispenser_container);

	struct item_data item;
	if(inventory_get_picked_item(inv, &item)) {
		if(menu.pointer_available && menu.pointer_on_slot) {
			gutil_draw_item(&item, menu.pointer_x - 8 * GFX_GUI_SCALE, menu.pointer_y - 8 * GFX_GUI_SCALE, 0);
		} else {
			gutil_draw_item(&item, off_x + selection->x, off_y + selection->y,
							0);
		}
	}
	container_menu_item_name(&menu, dispenser_container, selection->slot, width);

	/* Approved notice composition for THIS frame (menu_header.h). */
	struct menu_composition composition;
	dispenser_compose(&composition, width, height, off_x, off_y);
	screen_notice_compose(composition.notice.x,composition.notice.y,composition.notice.w,composition.notice.h,composition.notice_in_header);

	if(menu.pointer_available && menu.focus.pointer_owner) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(menu.pointer_x, menu.pointer_y, glm_rad(menu.pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_dispenser = {
	.reset = screen_dispenser_reset,
	.update = screen_dispenser_update,
	.render2D = screen_dispenser_render2D,
	.render3D = NULL,
	.render_world = true,
};
