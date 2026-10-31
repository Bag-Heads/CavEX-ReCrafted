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
#include "recipe_browser.h"
#include "local_menu_host.h"
#include "../../platform/menu_sound.h"

#define GUI_WIDTH 176
// Beta GuiContainer.java:9 `protected int ySize = 166;` -- the default every container
// except GuiChest uses. 167 sampled one transparent row past the art (all six GUI
// sheets' last opaque row is v=165) and centred the window 1px high. GuiChest is the
// ONLY container that overrides ySize (to 114 + rows*18) -- see chest_layout.h.
#define GUI_HEIGHT 166

static struct container_menu menu;
static struct container_slot slots[CRAFTING_SIZE];
static size_t slots_index;
static uint8_t crafting_container;

static bool can_show_recipes(struct window_container* wc) {
 /* Browsing is independent of grid/cursor contents. Craft submission still
  * uses client_session_recipe_craft_request's authoritative busy gate. */
 return wc && wc->type==WINDOW_TYPE_WORKBENCH && wc->slot_count==CRAFTING_SIZE;
}

void screen_crafting_set_windowc(uint8_t container) {
	crafting_container = container;
}

static void screen_crafting_reset(struct screen* s, int width, int height) {
	input_pointer_enable(true);
 local_menu_host_reset(&gstate.players[0]);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	s->render3D = screen_ingame.render3D;
	struct recipe_browser* browser = &gstate.players[0].recipes;
	browser->active = false;
	if(width >= 640 && height >= 240
	   && can_show_recipes(gstate.players[0].windows[crafting_container]))
		recipe_browser_open(browser, 3);
	menu = (struct container_menu) {0};
	if(local_menu_host_open(&gstate.players[0], 0, crafting_container, width, height))
		return;

	slots_index = 0;

	for(int k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (8 + (k % INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.y = (84 + (k / INVENTORY_SIZE_HOTBAR) * 18) * GFX_GUI_SCALE,
			.slot = k + CRAFTING_SLOT_MAIN,
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
			.slot = k + CRAFTING_SLOT_HOTBAR,
		};
	}

	for(int k = 0; k < CRAFTING_SIZE_INPUT; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = (30 + (k % 3) * 18) * GFX_GUI_SCALE,
			.y = (17 + (k / 3) * 18) * GFX_GUI_SCALE,
			.slot = k + CRAFTING_SLOT_INPUT,
		};
	}

	slots[slots_index++] = (struct container_slot) {
		.x = 124 * GFX_GUI_SCALE,
		.y = 35 * GFX_GUI_SCALE,
		.slot = CRAFTING_SLOT_OUTPUT,
	};
}

/* One craft request in flight per screen. The reply is installed on the
 * completion EDGE (client_session bumps craft_reply_serial for both an
 * authoritative reply and a local retirement), so a lost reply cannot leave
 * the guide's input locked forever. */
static void screen_crafting_poll_craft(struct client_session* session,
	struct recipe_browser* browser) {
	if(!browser->pending) return;
	if(session->craft_pending
	   && session->craft_reply_serial == browser->pending_serial) return;
	recipe_browser_craft_reply(browser, session->craft_pending
		? RECIPE_CRAFT_TIMED_OUT : session->craft_reason);
}

static void screen_crafting_update(struct screen* s, float dt) {
	(void)s;
	struct client_session* session = &gstate.players[0];
	struct recipe_browser* browser = &session->recipes;
	struct container_compact* compact = &session->container;
	struct container_menu* active_menu = compact->active ? &compact->menu : &menu;
	struct window_container* wc = session->windows[crafting_container];
	if(!wc) return;
	int width = gfx_width(), height = gfx_height();
 if(local_menu_host_active(session)) {
  struct container_input input;container_menu_read_input(&input,0,0,width,height);
  struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
  local_menu_host_update(session,&input,&glyphs,dt,container_menu_send,NULL);
  if(!local_menu_host_active(session))screen_set(&screen_ingame);
  return;
 }
 if(session->manual_grid.active) {
  struct container_input manual_input;container_menu_read_input(&manual_input,0,0,width,height);
  struct container_glyphs manual_glyphs;container_compact_active_glyphs(&manual_glyphs);
  if(manual_grid_step(&session->manual_grid,wc,crafting_container,&manual_input,&manual_glyphs,dt,container_menu_send,NULL)) {
   browser->active=true;browser->details=false;
   audio_menu_event(MENU_SOUND_BACK,time_get());
  }
  return;
 }
	struct container_input input;
	container_menu_read_input(&input, 0, 0, width, height);
	/* Recovery consumes the first connected frame before section handling too.
	 * A held shoulder button must not bypass the browser's reconnect guard. */
	if(browser->active && (input.suspended || browser->suspended)) {
		recipe_browser_step(browser, &input, width, height, dt);
		active_menu->close_requested = false;
		active_menu->suspended = input.suspended;
		return;
	}
	if(browser->blocked && can_show_recipes(wc)) browser->blocked = false;
	if(browser->active) {
		screen_crafting_poll_craft(session, browser);
  recipe_browser_refresh_availability(browser,wc,session->craft_pending||session->local_reply_failed);
		enum recipe_browser_action action
			= recipe_browser_step(browser, &input, width, height, dt);
		if(action == RECIPE_BROWSER_CRAFT) {
			/* A crafts ONE batch through the server-authoritative transaction.
			 * Nothing is predicted here: the server's slot updates precede its
			 * reply in the same FIFO, so Have/Need is re-read, never guessed. */
			uint16_t recipe = recipe_browser_selected_index(browser);
			if(recipe != RECIPE_CRAFT_INDEX_NONE
			   && client_session_recipe_craft_request(session, 0, crafting_container,
				   recipe, container_menu_send, NULL)) {
				browser->pending = true;
				browser->has_reason = false;
				browser->pending_serial = session->craft_reply_serial;
			} else {
				/* Refused before it was ever sent (busy window, held stack). */
				recipe_browser_craft_reply(browser, RECIPE_CRAFT_BUSY);
			}
			return;
		}
		if(action == RECIPE_BROWSER_MANUAL) {
   manual_grid_open(&session->manual_grid,3,width,height);browser->active=false;
   active_menu->return_blocked=active_menu->send_failed=false;browser->close_status[0]=0;
   audio_menu_event(MENU_SOUND_SECTION,time_get());return;
  }
		if(action != RECIPE_BROWSER_CLOSE && !active_menu->close_requested
		   && !input.suspended) return;
		struct container_input close = {.close = action == RECIPE_BROWSER_CLOSE,
			.suspended = input.suspended};
		if(container_menu_step(active_menu, compact->active ? compact->slots : slots,
			compact->active ? compact->count : slots_index, 0, 0, wc, crafting_container,
			&close, dt, container_menu_send, NULL)) screen_set(&screen_ingame);
		browser->close_status[0] = 0;
		if(active_menu->close_requested || active_menu->return_blocked || active_menu->send_failed)
			container_menu_label(active_menu, wc, 0, browser->close_status, sizeof browser->close_status);
		return;
	}
	/* Manual Grid: unchanged container semantics. The section cycle wraps back
	 * into the guide at its boundary, which is the return path the approved
	 * Manual Grid footer names ("Recipes"); a busy grid keeps the ordinary
	 * jump and says why. */
	if(!input.suspended && !input.close && !active_menu->close_requested
	   && (input.page != input.previous)) {
		if(!compact->active) {
			size_t slot = slots[menu.selected < slots_index ? menu.selected : 0].slot;
			bool wrap = (input.page && slot >= CRAFTING_SLOT_HOTBAR)
				|| (input.previous && slot < CRAFTING_SLOT_MAIN);
			if(wrap && can_show_recipes(wc)) recipe_browser_open(browser, 3);
			else {
				if(wrap) browser->blocked = true;
				container_menu_section_step(&menu, slots, slots_index, CRAFTING_SLOT_MAIN, &input);
			}
		} else if((input.previous && compact->view == 0)
			|| (input.page && compact->view == 1)) {
			if(can_show_recipes(wc)) recipe_browser_open(browser, 3);
			else browser->blocked = true;
		} else {
			container_compact_step(compact, wc, crafting_container, &input, dt,
				container_menu_send, NULL);
		}
		return; /* A section change cannot also pick, drop or transfer an item. */
	}
	if(compact->active) {
		if(container_compact_step(compact, wc, crafting_container, &input, dt,
			container_menu_send, NULL)) screen_set(&screen_ingame);
	} else if(container_menu_step(&menu, slots, slots_index,
		(width - GUI_WIDTH * GFX_GUI_SCALE) / 2,
		(height - GUI_HEIGHT * GFX_GUI_SCALE) / 2, wc, crafting_container,
		&input, dt, container_menu_send, NULL)) screen_set(&screen_ingame);
}

static void screen_crafting_render2D(struct screen* s, int width, int height) {
 if(local_menu_host_active(&gstate.players[0])) {
  struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
  local_menu_host_render(&gstate.players[0],&glyphs);
  screen_notice_compose(0,0,0,0,true); /* already painted by the owner host */
  return;
 }
 if(gstate.players[0].manual_grid.active) {
  struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
  manual_grid_render(&gstate.players[0].manual_grid,gstate.players[0].windows[crafting_container],0,0,&glyphs);
  return;
 }

	(void)s;
	struct recipe_browser* browser = &gstate.players[0].recipes;
	if(browser->active) {
		struct container_glyphs glyphs;
		container_compact_active_glyphs(&glyphs);
		struct window_container* wc = gstate.players[0].windows[crafting_container];
		if(wc) recipe_browser_render(browser, windowc_get_latest(wc), CRAFTING_SLOT_MAIN,
			0, 0, width, height, &glyphs);
		return;
	}
	if(gstate.players[0].container.active) {
		container_compact_screen_render(&gstate.players[0].container, crafting_container);
		if(browser->blocked)
			gutil_text(24, 2, "Clear grid and carried stack for recipes", 16, true);
		return;
	}
	struct inventory* inv
		= windowc_get_latest(gstate.players[0].windows[crafting_container]);

	// darken background
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gfx_texture(true);

	int off_x = (width - GUI_WIDTH * GFX_GUI_SCALE) / 2;
	int off_y = (height - GUI_HEIGHT * GFX_GUI_SCALE) / 2;

	// draw inventory
	gfx_bind_texture(&texture_gui_crafting);
	gutil_texquad(off_x, off_y, 0, 0, GUI_WIDTH, GUI_HEIGHT, GUI_WIDTH * GFX_GUI_SCALE,
				  GUI_HEIGHT * GFX_GUI_SCALE);
	gutil_text(off_x + 28 * GFX_GUI_SCALE, off_y + 6 * GFX_GUI_SCALE, "\2478Crafting", 8 * GFX_GUI_SCALE, false);
	if(browser->blocked)
		gutil_text(144, 48, "Clear grid and carried stack for recipes", 16, true);

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
	container_menu_row(crafting_container);

	struct item_data item;
	if(inventory_get_picked_item(inv, &item)) {
		if(menu.pointer_available && menu.pointer_on_slot) {
			gutil_draw_item(&item, menu.pointer_x - 8 * GFX_GUI_SCALE, menu.pointer_y - 8 * GFX_GUI_SCALE, 0);
		} else {
			gutil_draw_item(&item, off_x + selection->x, off_y + selection->y,
							0);
		}
	}
	container_menu_item_name(&menu, crafting_container, selection->slot, width);

	if(menu.pointer_available && menu.focus.pointer_owner) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(menu.pointer_x, menu.pointer_y, glm_rad(menu.pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_crafting = {
	.reset = screen_crafting_reset,
	.update = screen_crafting_update,
	.render2D = screen_crafting_render2D,
	.render3D = NULL,
	.render_world = true,
};
