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
#include "../../platform/input_glyph.h"
#include "container_compact.h"
#include "recipe_browser.h"
#include "local_menu_host.h"
#include "../../platform/menu_sound.h"
#include "hud_survival_row.h"

#define GUI_WIDTH 372
#define GUI_HEIGHT 332

static struct container_menu menu;
static struct container_slot slots[INVENTORY_SIZE];
static size_t slots_index;

/* ui-build-1026/T12 (CONSENSUS 2026-10-26): this screen has two pages,
 * Inventory (the Beta ContainerPlayer slots + armor) and Crafting (the
 * personal recipe browser). Tap PAGE_SWAP (C / ZL / C-stick) swaps them. A
 * swap is presentation only: it sends nothing and never closes or reopens the
 * window, so the cursor stack and the four real 2x2 craftMatrix slots stay
 * exactly where they are -- Beta keeps ONE ContainerPlayer for the life of
 * GuiInventory (ContainerPlayer.java:13 builds craftMatrix once per
 * container). Closing from either page goes through the same close path, and
 * the window close is where the grid is emptied as Beta does
 * (ContainerPlayer.java:48-57 onCraftGuiClosed, Container.java:154-160 for
 * the held stack). The old "Recipes" button/focus entry is gone. */
static void inventory_page_crafting(struct recipe_browser* browser) {
	recipe_browser_open(browser, 2);
	audio_menu_event(MENU_SOUND_SECTION, time_get());
}
static void inventory_page_inventory(struct recipe_browser* browser) {
	browser->active = false;
	browser->details = false;
	audio_menu_event(MENU_SOUND_SECTION, time_get());
}

static void screen_inventory_reset(struct screen* s, int width, int height) {
	input_pointer_enable(true);
 local_menu_host_reset(&gstate.players[0]);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	s->render3D = screen_ingame.render3D;
 gstate.players[0].recipes.active=false;gstate.players[0].recipes.blocked=false;
	if(local_menu_host_open(&gstate.players[0], 0, WINDOWC_INVENTORY, width, height))
		return;
	local_menu_host_inventory_opened(&gstate.players[0], 0);

	menu = (struct container_menu) {.slot_extent=36};

	slots_index = 0;

	for(int k = 0; k < INVENTORY_SIZE_MAIN; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = 24 + (k % INVENTORY_SIZE_HOTBAR) * 36,
			.y = 176 + (k / INVENTORY_SIZE_HOTBAR) * 36,
			.slot = k + INVENTORY_SLOT_MAIN,
		};
	}

	for(int k = 0; k < INVENTORY_SIZE_HOTBAR; k++) {
		if(k
		   == (int)inventory_get_hotbar(
			   windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY])))
			menu.selected = slots_index;

		slots[slots_index++] = (struct container_slot) {
			.x = 24 + k * 36,
			.y = 288,
			.slot = k + INVENTORY_SLOT_HOTBAR,
		};
	}

	for(int k = 0; k < INVENTORY_SIZE_ARMOR; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = 58,
			.y = 33 + k * 28,
			.slot = k + INVENTORY_SLOT_ARMOR, .extent=28,
		};
	}

	for(int k = 0; k < INVENTORY_SIZE_CRAFTING; k++) {
		slots[slots_index++] = (struct container_slot) {
			.x = 190 + (k % 2) * 28,
			.y = 47 + (k / 2) * 28,
			.slot = k + INVENTORY_SLOT_CRAFTING, .extent=28,
		};
	}

	slots[slots_index++] = (struct container_slot) {
		.x = 286,
		.y = 61,
		.slot = INVENTORY_SLOT_OUTPUT, .extent=28,
	};
}

/* Same completion edge the workbench guide uses: an authoritative reply AND a
 * local retirement both bump craft_reply_serial, so the guide never stays
 * locked on a lost reply. */
static void screen_inventory_poll_craft(struct client_session* session,
	struct recipe_browser* browser) {
	if(!browser->pending) return;
	if(session->craft_pending
	   && session->craft_reply_serial == browser->pending_serial) return;
	recipe_browser_craft_reply(browser, session->craft_pending
		? RECIPE_CRAFT_TIMED_OUT : session->craft_reason);
}

/* The personal station may hold four inputs. A 3x3 recipe stays browsable and
 * says "Needs crafting table"; it is never executable here (specs/menus.md). */
static bool inventory_recipes_available(struct window_container* wc) {
 /* Browsing is independent of grid/cursor contents. Craft submission still
  * uses client_session_recipe_craft_request's authoritative busy gate. */
 return wc && wc->type==WINDOW_TYPE_INVENTORY && wc->slot_count==INVENTORY_SIZE;
}

static void screen_inventory_update(struct screen* s, float dt) {
	(void)s;
	struct client_session* session = &gstate.players[0];
	struct recipe_browser* browser = &session->recipes;
	struct window_container* wc = session->windows[WINDOWC_INVENTORY];
	struct container_compact* compact = &session->container;
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
  if(manual_grid_step(&session->manual_grid,wc,WINDOWC_INVENTORY,&manual_input,&manual_glyphs,dt,container_menu_send,NULL)) {
   browser->active=true;browser->details=false;
   audio_menu_event(MENU_SOUND_BACK,time_get());
  }
  return;
 }
	struct container_menu* active_menu = compact->active ? &compact->menu : &menu;
	struct container_input input;
	container_menu_read_input(&input, 0, 0, width, height);
	/* The Inventory page's bottom row is a pointer target (WIRING-PLAN Q2):
	 * its Crafting cell is the page swap. The other cells are applied again
	 * by the container step that owns them. */
	if(!browser->active && wc && !active_menu->sort_open
	   && (active_menu->focus.pointer_owner || !active_menu->focus.initialized)) {
		struct input_player_frame frame;
		input_prompt_frame(0, &frame);
		struct container_input row = input;
		if(container_row_click(CONTAINER_ROW_INVENTORY,
			inventory_get_picked_item(windowc_get_latest(wc), NULL), false, &frame,
			0, 0, width, height, &row) == CONTAINER_ROW_PAGE_SWAP)
			input = row;
	}
	if(browser->active) {
		/* Recovery consumes the first connected frame before paging too. */
		if(input.suspended || browser->suspended) {
			recipe_browser_step(browser, &input, width, height, dt);
			active_menu->close_requested = false;
			active_menu->suspended = input.suspended;
			return;
		}
		screen_inventory_poll_craft(session, browser);
		/* The row's Inventory cell is the page swap too (Sol review 10-31
		 * item 2), read before the step so the click is not eaten. */
		{
			struct container_glyphs glyphs;
			container_compact_active_glyphs(&glyphs);
			if(recipe_browser_row_page_swap(browser, &glyphs, &input, width, height))
				input.page_swap = true;
		}
		/* Crafting -> Inventory. Never while a craft is in flight (its reply is
		 * owned by this page) or while a close is already returning a stack. */
		if(input.page_swap && !browser->pending && !browser->close_after_craft
		   && !active_menu->close_requested) {
			inventory_page_inventory(browser);
			return;
		}
		recipe_browser_refresh_availability(browser,wc,session->craft_pending||session->local_reply_failed);
		enum recipe_browser_action action
			= recipe_browser_step(browser, &input, width, height, dt);
		if(action == RECIPE_BROWSER_CRAFT) {
			uint16_t recipe = recipe_browser_selected_index(browser);
			if(recipe != RECIPE_CRAFT_INDEX_NONE
			   && client_session_recipe_craft_request(session, 0, WINDOWC_INVENTORY,
				   recipe, container_menu_send, NULL)) {
				browser->pending = true;
				browser->has_reason = false;
				browser->pending_serial = session->craft_reply_serial;
			} else recipe_browser_craft_reply(browser, RECIPE_CRAFT_BUSY);
			return;
		}
		if(action == RECIPE_BROWSER_MANUAL) {
			/* T12 fix r1: no Manual Grid popup for the personal grid. The
			 * shortcut goes to the page that holds the REAL 2x2 craftMatrix
			 * slots and puts focus on its first slot. */
			inventory_page_inventory(browser);
			browser->close_status[0] = 0;
			if(compact->active)
				container_compact_focus_slot(compact, INVENTORY_SLOT_CRAFTING);
			else for(size_t k = 0; k < slots_index; k++)
				if(slots[k].slot == INVENTORY_SLOT_CRAFTING) {
					menu.selected = k;
					menu.selection_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
					menu.focus = (struct gutil_menu_focus) {.initialized = true};
					menu.repeat = (struct gutil_menu_repeat) {0};
					menu.pointer_on_slot = false;
					break;
				}
			return;
		}
		/* Close at the root closes the whole inventory window from this page,
		 * through the same acknowledged-return close the Inventory page uses
		 * (same shape as screen_crafting.c). */
		if(action != RECIPE_BROWSER_CLOSE && !active_menu->close_requested) return;
		struct container_input close = {.close = action == RECIPE_BROWSER_CLOSE};
		if(!wc) return;
		if(container_menu_step(active_menu, compact->active ? compact->slots : slots,
			compact->active ? compact->count : slots_index, 0, 0, wc, WINDOWC_INVENTORY,
			&close, dt, container_menu_send, NULL)) screen_set(&screen_ingame);
		browser->close_status[0] = 0;
		if(active_menu->close_requested || active_menu->return_blocked || active_menu->send_failed)
			container_menu_label(active_menu, wc, 0, browser->close_status, sizeof browser->close_status);
		return;
	}
	/* Inventory -> Crafting. The section cycle (Previous/Next) stays the
	 * ordinary container contract and never changes page. */
	if(input.page_swap && !input.suspended && !active_menu->sort_open
	   && !active_menu->close_requested && !active_menu->suspended) {
		if(inventory_recipes_available(wc)) inventory_page_crafting(browser);
		return;
	}
	if(compact->active) {
		container_compact_screen_update(compact, WINDOWC_INVENTORY, dt);
		return;
	}
	int off_x = (width-640)/2+134;
	int off_y = (height-480)/2+30 /* B3: 44 -> 30 */;
	container_menu_update(&menu, slots, slots_index, off_x, off_y,
		WINDOWC_INVENTORY, dt);
}

static void screen_inventory_render2D(struct screen* s, int width, int height) {
 if(local_menu_host_active(&gstate.players[0])) {
  struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
  local_menu_host_render(&gstate.players[0],&glyphs);
  screen_notice_compose(0,0,0,0,true); /* already painted by the owner host */
  return;
 }
 if(gstate.players[0].manual_grid.active) {
  struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
  manual_grid_render(&gstate.players[0].manual_grid,gstate.players[0].windows[WINDOWC_INVENTORY],0,0,&glyphs);
  return;
 }

	struct recipe_browser* browser = &gstate.players[0].recipes;
	if(browser->active) {
		struct container_glyphs glyphs;
		container_compact_active_glyphs(&glyphs);
		struct window_container* wc = gstate.players[0].windows[WINDOWC_INVENTORY];
		if(wc) recipe_browser_render(browser, windowc_get_latest(wc),
			INVENTORY_SLOT_MAIN, 0, 0, width, height, &glyphs);
		return;
	}
	if(gstate.players[0].container.active) {
		container_compact_screen_render(&gstate.players[0].container, WINDOWC_INVENTORY);
		return;
	}

 (void)s;
 struct input_player_frame prompts;input_prompt_frame(0,&prompts);
 struct container_compact presentation={.menu=menu,.count=slots_index,.width=width,.height=height,.type=WINDOW_TYPE_INVENTORY,.active=true};
 int off_x=(width-640)/2+134,off_y=(height-480)/2+30 /* B3: 44 -> 30 */;
 for(size_t k=0;k<slots_index;k++){presentation.slots[k]=slots[k];presentation.slots[k].x+=off_x;presentation.slots[k].y+=off_y;}
 container_inventory_render(&presentation,gstate.players[0].windows[WINDOWC_INVENTORY],0,0,&prompts);
 /* B3 (10-31): hearts + hotbar at the gameplay positions (was h-36) */
 if(height>=480)hud_menu_draw_owned(&gstate.players[0],width,height);

	if(menu.pointer_available && menu.focus.pointer_owner) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(menu.pointer_x, menu.pointer_y, glm_rad(menu.pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE, 48 * GFX_GUI_SCALE);
	}
}

/* READ-ONLY OBSERVATION (push-0921/B4b). This screen's focus state is file-
 * static (menu / slots / slots_index above), so an unattended scripted driver
 * cannot ask "which slot am I on" without an accessor here. It answers the
 * PROTOCOL window-0 slot number (main 9..35, hotbar 36..44) or -1; see
 * container_menu.h for every -1 case it inherits. Observes only, changes
 * nothing, and nothing in the game branches on it. */
int screen_inventory_focus_player_slot(void) {
	/* The statics survive the screen being left, so a stale focus would
	 * otherwise be reported for whatever screen is really up. */
	if(gstate.players[0].current_screen != &screen_inventory)
		return -1;
	/* The Crafting page owns NO window-slot focus, and the two alternate
	 * presentations drive their own focus entirely. */
	if(gstate.players[0].recipes.active || local_menu_host_active(&gstate.players[0])
	   || gstate.players[0].manual_grid.active)
		return -1;
	return container_menu_focus_player_slot(&menu, slots, slots_index);
}

/* ui-build-1026 T15: menus_dwell walker observers (screen.h). Reads only. */
int screen_inventory_page(void) {
	if(gstate.players[0].current_screen != &screen_inventory)
		return -1;
	return gstate.players[0].recipes.active ? 1 : 0;
}

int screen_inventory_crafting_tab(void) {
	return screen_inventory_page() == 1 ? (int)gstate.players[0].recipes.category
										: -1;
}

struct screen screen_inventory = {
	.reset = screen_inventory_reset,
	.update = screen_inventory_update,
	.render2D = screen_inventory_render2D,
	.render3D = NULL,
	.render_world = true,
};
