/*
	Copyright (c) 2022 ByteBit/xtreme8000
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

#include "hud_survival_row.h"

#include "../../entity/armor.h" // Batch J: getTotalArmorValue for the armor row
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/server_interface.h" // MAX_PLAYER_HEALTH / HEALTH_PER_HEART
#include "../../network/server_local_state.h" // OXYGEN_THRESHOLD
#include "../../platform/gfx.h"
#include "../../platform/texture.h"
#include "../../worldgen/java_random.h" // Batch J: bit-exact heart-jitter rand
#include "../game_state.h"
#include "../hud_hurt.h" // Batch J: heart blink/jitter state (fed via camera tick)

/* Health + armor + air HUD drawn from the real Beta gui/icons.png
 * (texture_icons), NOT the combined gui_2 atlas -- so a genuine Beta
 * texture pack shows its authentic sprites. Beta icons.png UVs (all 9x9)
 * [740c583 GuiIngame.renderGameOverlay:75-130]: heart container (16,0),
 * blink container (25,0), full/half heart (52,0)/(61,0), prev-health blink
 * ghosts (70,0)/(79,0); armor full/half/empty (34,9)/(25,9)/(16,9); air
 * bubble full (16,18), popping (25,18).
 *
 * Extracted verbatim (2026-09-03, creative-HUD lane) from
 * screen_ingame_render2D so the single caller-side `if(!gstate.config_creative)`
 * gate can hide the whole row without duplicating draw logic, and so this
 * logic is link-testable in isolation (test_hud_survival_row.c). No behaviour
 * change to the survival path -- see the equivalence guard in that test. */
void hud_survival_row_draw_owned(const struct client_session* session,
	int hud_left, int heart_row_y) {
	hud_survival_row_draw_owned_ex(session, hud_left, heart_row_y, true);
}

void hud_survival_row_draw_owned_ex(const struct client_session* session,
	int hud_left, int heart_row_y, bool with_air) {
	assert(session && session->local_player);
	gfx_bind_texture(&texture_icons);

	// Batch J heart blink + low-health jitter, state from hud_hurt.h. The
	// jitter rand is reseeded EVERY FRAME from the tick counter
	// [740c583 GuiIngame:66], so within one tick the offsets hold still;
	// jrand is the bit-exact java.util.Random (one nextInt(2) per heart, in
	// heart order -- Beta draws nothing else from it in this loop).
	bool hurt_blink = hud_hurt_blink(&session->camera.hurt);
	struct java_random hud_rand;
	jrand_set_seed(&hud_rand, hud_hurt_jitter_seed(&session->camera.hurt));
	// Beta jitter gate: health <= 4 half-hearts == 2 hearts
	// [740c583 GuiIngame:96-98], in the engine's 16-per-heart scale.
	bool heart_jitter
		= session->local_player->health <= 2 * HEALTH_PER_HEART;

	for(int k = 0; k < MAX_PLAYER_HEALTH / HEALTH_PER_HEART; k++) {
		int hx = hud_left + k * 8 * GFX_GUI_SCALE;
		int hy = heart_row_y;
		if(heart_jitter)
			hy += jrand_next_int_bound(&hud_rand, 2) * GFX_GUI_SCALE;
		// container first (white variant on blink frames, u = 16 + 9
		// [740c583 GuiIngame:99-101]), then overlays in Beta order:
		// prev-health ghost on blink frames [:102-109], current on top.
		gutil_texquad(hx, hy, hurt_blink ? 25 : 16, 0, 9, 9,
					  9 * GFX_GUI_SCALE, 9 * GFX_GUI_SCALE);
		if(hurt_blink) {
			int prev_hp
				= session->camera.hurt.prev_health - k * HEALTH_PER_HEART;
			if(prev_hp >= HEALTH_PER_HEART)
				gutil_texquad(hx, hy, 70, 0, 9, 9, 9 * GFX_GUI_SCALE,
							  9 * GFX_GUI_SCALE);
			else if(prev_hp >= HEALTH_PER_HEART / 2)
				gutil_texquad(hx, hy, 79, 0, 9, 9, 9 * GFX_GUI_SCALE,
							  9 * GFX_GUI_SCALE);
		}
		int heart_hp = session->local_player->health - k * HEALTH_PER_HEART;
		if(heart_hp >= HEALTH_PER_HEART)
			gutil_texquad(hx, hy, 52, 0, 9, 9, 9 * GFX_GUI_SCALE,
						  9 * GFX_GUI_SCALE);
		else if(heart_hp >= HEALTH_PER_HEART / 2)
			gutil_texquad(hx, hy, 61, 0, 9, 9, 9 * GFX_GUI_SCALE,
						  9 * GFX_GUI_SCALE);
	}

	// Batch J armor row [740c583 GuiIngame:75-93]: right-aligned at the
	// hearts' UNJITTERED y (Beta adds the jitter after the armor draw), only
	// while any armor value is present; icon per slot k compares k*2+1
	// against getTotalArmorValue's Beta-native units (armor.h port). Beta x =
	// w/2 + 91 - k*8 - 9, mirrored here off the hotbar's right edge.
	{
		int armor = armor_get_total_value(
			windowc_get_latest(session->windows[WINDOWC_INVENTORY]));
		if(armor > 0) {
			for(int k = 0; k < 10; k++) {
				int ax = hud_left + 182 * GFX_GUI_SCALE
					- (9 + k * 8) * GFX_GUI_SCALE;
				int au = (k * 2 + 1 < armor) ? 34
					: ((k * 2 + 1 == armor) ? 25 : 16);
				gutil_texquad(ax, heart_row_y, au, 9, 9, 9,
							  9 * GFX_GUI_SCALE, 9 * GFX_GUI_SCALE);
			}
		}
	}

	// draw oxygen bubbles while submerged [740c583 GuiIngame:120-130]: the
	// air row sits 9px above the health row (Beta's var7 - 32 - 9), full
	// bubbles (16,18) for ceil((air-2)*10/300), the remainder up to
	// ceil(air*10/300) as the popping sprite (25,18).
	// SOL-REVIEW item 4 (10-31): a menu passes with_air=false so a full
	// panel clears the HUD above the hearts; gameplay always passes true.
	if(with_air && session->in_water && session->oxygen >= OXYGEN_THRESHOLD) {
		// Beta bubble counts -> a full 10-bubble bar for air 273..300
		// depleting to 0 (oxygen runs 300..-20 here); the integer form
		// (x*10 + 299)/300 is ceil for positive x, guarded to 0 otherwise.
		// The prior (oxygen-OXYGEN_THRESHOLD)/32 collapsed the top 10-bubble
		// band to a single tick once MAX_OXYGEN became 300 (320 == 10*32).
		int full
			= session->oxygen > 2 ? ((session->oxygen - 2) * 10 + 299) / 300 : 0;
		int total
			= session->oxygen > 0 ? (session->oxygen * 10 + 299) / 300 : 0;
		for(int k = 0; k < total; k++) {
			gutil_texquad(hud_left + k * 8 * GFX_GUI_SCALE,
						  heart_row_y - 9 * GFX_GUI_SCALE, (k < full) ? 16 : 25,
						  18, 9, 9, 9 * GFX_GUI_SCALE, 9 * GFX_GUI_SCALE);
		}
	}
}

void hud_survival_row_draw(int hud_left, int heart_row_y) {
	hud_survival_row_draw_owned(&gstate.players[0], hud_left, heart_row_y);
}
