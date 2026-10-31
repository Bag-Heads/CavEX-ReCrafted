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

#ifndef RENDER_MINECART_H
#define RENDER_MINECART_H

#include "../item/item_types.h"
#include "../platform/gfx.h"
#include "../cglm/types.h"

typedef struct {
    uint8_t u, v, w, h;
} UVRect;

void render_entity_minecart(mat4 view);
void render_entity_update_light(uint8_t light);
void render_entity_set_limb_swing(float amt);
void render_entity_init(void);
void render_entity_creeper(mat4 view, float headYawDeg, float bodyYawDeg, int frame, int fuse, float td);
void render_entity_zombie(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
/* WINDUP-SPEC: per-draw presentation only; zero preserves legacy matrices. */
void render_entity_zombie_cue(mat4 view, float headYawDeg, float bodyYawDeg,
							 int frame, float windup_degrees);
void render_entity_pigzombie_cue(mat4 view, float headYawDeg, float bodyYawDeg,
								int frame, float windup_degrees);
void render_entity_spider_cue(mat4 view, float headYawDeg, float bodyYawDeg,
							 int frame, float windup_degrees);
void render_entity_wolf_cue(mat4 view, float headYawDeg, float bodyYawDeg,
	int frame, uint8_t flags, int watched_health, bool shaking,
	float previous_shake, float current_shake,
	float previous_interest, float current_interest, float partial,
	float windup_degrees);
void render_entity_skeleton(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
// MP remote-player render follow-up: the shared ModelBiped geometry, textured
// with mob/char.png (texture_mob_char; falls back to the creeper skin like
// every other biped when the pack omits it -- render_entity_biped's own
// tex->data NULL check). held_item is an item id, 0 == empty hands.
// headPitchDeg is Beta's own rotationPitch in degrees (0 = level, ABSOLUTE --
// not relative to body); 2026-08-26 residual item 3 (see render_entity_anchor.h's
// render_entity_head_mv for the full citation).
// MP visual-parity pass (2026-08-31) items 2+3: is_sneak selects
// ModelBiped's isSneak pose [740c583]; swing_progress is EntityLiving.
// getSwingProgress's OWN render-time interpolated 0..1 value (the caller's
// job, see entity_remote_player.c) driving the arm-swing curve -- see
// render_biped_swing_pose.h for the full citation.
// Animation-parity pass (2026-09-03) item 4: is_riding selects ModelBiped's
// isRiding pose [740c583] -- see render_biped_swing_pose.h's
// render_biped_riding_pose() for the exact constants and citation.
// Animation-parity pass item 5: armor_ids[4] is indexed by enum armor_type
// (item_types.h) -- HELMET, CHESTPLATE, LEGGINGS, BOOTS, item id 0 == empty
// slot -- and renders RenderPlayer's armor-layer pass [740c583
// RenderPlayer.setArmorModel] over the base biped. Pass NULL for "no armor
// data available" (every non-player biped caller).
void render_entity_player(mat4 view, float headYawDeg, float bodyYawDeg,
						  float headPitchDeg, int frame, int held_item,
						  bool is_sneak, float swing_progress,
						  bool is_riding, const uint16_t armor_ids[4]);
void render_entity_spider(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
void render_entity_pig(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
void render_entity_wolf(mat4 view, float headYawDeg, float bodyYawDeg, int frame,
						uint8_t flags, int watched_health, bool shaking,
						float previous_shake, float current_shake,
						float previous_interest, float current_interest,
						float partial);
void render_entity_cow(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
void render_entity_chicken(mat4 view, float headYawDeg, float bodyYawDeg, int frame,
						   float wingDestPos);
// Beta 1.7.3 RenderSheep = ModelSheep2 (bare body, sheep.png) + ModelSheep1 (inflated wool
// overlay, sheep_fur.png, colour-tinted). fleeceColor is the wool metadata index 0..15
// (e->data.monster.fleece_color); the model tints the wool layer to that colour. sheared
// skips the ENTIRE wool overlay pass (Beta RenderSheep.shouldRenderPass !getSheared()).
void render_entity_sheep(mat4 view, float headYawDeg, float bodyYawDeg, int frame,
						 int fleeceColor, bool sheared);
// Beta 1.7.3 ModelSlime two-pass model (decompile-verified): opaque inner core
// (6x6x6 @ tex 0,16) + eyes (2x2x2 @ 32,0 / 32,4) + mouth (1x1x1 @ 32,8), then
// the translucent 8x8x8 gel shell (@ 0,0) blended SRC_ALPHA/ONE_MINUS_SRC_ALPHA
// on top. The caller scales the shared model matrix by the Beta size (1/2/4)
// about the feet -- this fn draws the size-1 model. mob/slime.png; absent ->
// creeper-skin fallback (opaque gel until a real pack overlays it).
void render_entity_slime(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
// Beta 1.7.3 ModelSquid (decompile-verified): 12x16x12 body + EIGHT 2x18x2
// tentacles on a radius-5px ring (each yawed i*-2PI/8 + PI/2), tentacle swing
// driven from the walk frame (the server's exact pulse phase is not networked
// -- documented approximation; body pitch/roll polish deferred). mob/squid.png.
void render_entity_squid(mat4 view, float headYawDeg, float bodyYawDeg, int frame);
// Nether natives (batch 1b): pig zombie = the shared biped with
// mob/pigzombie.png; ghast = 16^3 body + 9 tentacles at 4.5x (ModelGhast +
// RenderGhast rest scale), `attacking` swaps in ghast_fire.png.
void render_entity_pigzombie(mat4 view, float headYawDeg, float bodyYawDeg,
							 int frame);
void render_entity_ghast(mat4 view, float headYawDeg, float bodyYawDeg,
						 int frame, bool attacking);

#endif
