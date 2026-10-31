/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#ifndef GAME_GUI_HUD_SURVIVAL_ROW_H
#define GAME_GUI_HUD_SURVIVAL_ROW_H

/* Draws the Beta hearts + armor + air HUD rows from the real Beta
 * gui/icons.png (texture_icons), byte-identical to the pre-split
 * screen_ingame_render2D block this was extracted from -- see the UV/state
 * commentary in hud_survival_row.c. `hud_left` is the hotbar's left edge and
 * `heart_row_y` the hearts' unjittered top-left Y (both computed by the
 * caller off the current screen size), matching Beta's own layout anchors.
 *
 * SURVIVAL ONLY: maintainer ruling 2026-09-03 (docs/creative_mode.md,
 * "Creative HUD -- no hearts/armor/air") -- a creative world's HUD must not
 * draw health, armor, or air. The caller (screen_ingame.c) is the single
 * gate: it calls this function only when `!gstate.config_creative`. This
 * function itself does not re-check the mode -- keeping the gate at one call
 * site makes "does creative draw this row" answerable by reading the one
 * `if` in screen_ingame.c, and lets tests exercise the row-drawing logic in
 * isolation from the mode gate. */
void hud_survival_row_draw(int hud_left, int heart_row_y);

/* Explicit owner for each viewport. Requires an admitted body and inventory. */
struct client_session;
void hud_survival_row_draw_owned(const struct client_session* session,
	int hud_left, int heart_row_y);

/* SOL-REVIEW item 4 (10-31): the same row with the air bubbles optional.
 * Gameplay draws them (with_air=true, what hud_survival_row_draw_owned
 * does); hud_menu_draw_owned passes false so a container/menu screen never
 * shows the oxygen bubbles above its hearts and a full panel clears the HUD. */
#include <stdbool.h>
void hud_survival_row_draw_owned_ex(const struct client_session* session,
	int hud_left, int heart_row_y, bool with_air);

/* A2 (10-31): a menu's hearts + hotbar at exactly the gameplay positions of
 * a width x height pane (screen_ingame_hotbar_top). Hearts only in survival,
 * the same single gate gameplay uses. Defined in screen_ingame.c, next to
 * the gameplay draw whose anchors it reuses. */
void hud_menu_draw_owned(const struct client_session* owner, int width,
	int height);

#endif
