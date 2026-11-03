/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#include <stdlib.h>
#include <string.h>

#include "../block/blocks.h"
#include "../item/items/item_map.h" /* colors->RGBA conversion (held map) */
#include "../network/client_interface.h" /* held-map mirror read */
#include "../platform/displaylist.h"
#include "../platform/gfx.h"
#include "../platform/texture.h" /* texture_map_bg / texture_map_live */
#include "../game/game_state.h" /* gstate.players: held-map owner -> viewport */
#include "gui_util.h"
#include "gfx_settings.h"
#include "render_item.h"
#include "render_model.h"
#include "render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../item/items.h" // direct include (was transitive via blocks.h pre-split)

static struct displaylist dl;
static uint8_t vertex_light[24];
static uint8_t vertex_light_inv[24];

static inline uint8_t MAX_U8(uint8_t a, uint8_t b) {
	return a > b ? a : b;
}

static uint8_t level_table_064[16] = {
	0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};

static uint8_t level_table_080[16] = {
	0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};

static inline uint8_t DIM_LIGHT(uint8_t l, uint8_t* table) {
	return (table[l >> 4] << 4) | table[l & 0x0F];
}

void render_item_init() {
	displaylist_init(&dl, 320, 3 * 2 + 2 * 1 + 1);
	memset(vertex_light, 0x0F, sizeof(vertex_light));
	memset(vertex_light_inv, 0xFF, sizeof(vertex_light_inv));
}

void render_item_update_light(uint8_t light) {
	memset(vertex_light, light, sizeof(vertex_light));
}

/* ci-1101/31: which split viewport the frame is drawing right now. main.c sets
 * it at the top of each viewport pass and back to 0 after the loop; every
 * item draw inside that pass (hotbar, held item, inventory, container) then
 * picks that player's compass/clock needle. Single player never leaves 0. */
static unsigned s_render_viewport = 0;

void render_item_set_viewport(unsigned viewport) {
	s_render_viewport = viewport < CLIENT_LOCAL_PLAYERS ? viewport : 0;
}

unsigned render_item_viewport(void) {
	return s_render_viewport;
}

uint8_t render_item_flat_uv(uint8_t origin, int cell, int k) {
	assert(cell == 16 || cell == 256);
	assert(k >= 0 && k <= 16);
	// Far tile edge in the u8 texcoord pipeline (review fix 2026-06-11): both
	// backends normalize u8 texcoords over 256 (PC /256.0, GX 8-frac u8), so
	// the outer edge of atlas row/column 15 -- 15*16 + 16 = 256 -- does not
	// fit in a uint8_t: it silently wrapped to 0 and smeared the whole atlas
	// across the tile. The Beta music records (texture_y 15) were the first
	// items ever registered on the last row. Clamp the edge to 255: a 1/256
	// sub-texel shave on that tile's outer edge (invisible at item scale),
	// not a wrap. The GUI/inventory path is immune (uint16 texquads); the
	// terrain atlas is immune by layout (14 cols x 18 px, max 253).
	// ci-1101/31 R7: a private 16x16 needle surface (cell 256) steps 16 per
	// strip so the extrusion sides sample the whole icon, not one texel.
	int v = (int)origin + k * (cell / 16);
	return (uint8_t)(v > 255 ? 255 : v);
}

void render_item_flat(struct item* item, struct item_data* stack, mat4 view,
					  bool fullbright, enum render_item_env env) {
	assert(item && stack && view);

	uint8_t s, t;
	int cell = 16; /* texcoord extent of one icon (atlas cell or whole surface) */

	if(item_is_block(stack)) {
		struct block* b = blocks[stack->id];
		assert(b);

		struct block_data this_blk = (struct block_data) {
			.type = stack->id,
			.metadata = stack->durability,
			.sky_light = 15,
			.torch_light = 15,
		};

		uint8_t tex = b->getTextureIndex(
			&(struct block_info) {
				.block = &this_blk, .neighbours = NULL, .x = 0, .y = 0, .z = 0},
			SIDE_TOP);
		// DC-GRASS G2: the world grass top cell is raw/greyscale now (the
		// mesher tints it); this flat item quad has no tint, so use the baked
		// item cell (tile 0 x fallback, once) -- the same cell the 3D path's
		// grass_item_texture uses, and the pre-G2 look.
		if(stack->id == BLOCK_GRASS)
			tex = tex_atlas_lookup(TEXAT_GRASS_TOP_ITEM);

		s = TEX_OFFSET(TEXTURE_X(tex));
		t = TEX_OFFSET(TEXTURE_Y(tex));

		gfx_bind_texture(b->transparent ? &texture_anim : &texture_terrain);
	} else {
		s = item->render_data.item.texture_x * 16;
		t = item->render_data.item.texture_y * 16;

		/* ci-1101/31: in split viewport 1 the compass/clock come from THAT
		 * player's private 16x16 needle texture (texture.h), drawn whole,
		 * instead of the atlas cell player 1's needle is written into.
		 * Viewport 0 / single player: NULL, the atlas cell as always. */
		struct tex_gfx* needle = NULL;
		if(stack->id == ITEM_COMPASS || stack->id == ITEM_CLOCK)
			needle = item_needle_fx_split_texture(s_render_viewport,
												  stack->id == ITEM_CLOCK);
		if(needle) {
			s = t = 0;
			cell = 256; /* u16 texcoord: the whole 16x16 surface */
			gfx_bind_texture(needle);
		} else {
			gfx_bind_texture(&texture_items);
		}
	}

	if(env == R_ITEM_ENV_INVENTORY) {
		gfx_matrix_modelview(view);
		gutil_texquad(0, 0, s, t, cell, cell, 16 * GFX_GUI_SCALE, 16 * GFX_GUI_SCALE);
	} else {
		displaylist_reset(&dl);

		uint8_t light = fullbright ? *vertex_light_inv : *vertex_light;

		// Far edge: see render_item_flat_uv (the 256 -> 255 clamp story).
		uint8_t s1 = render_item_flat_uv(s, cell, 16);
		uint8_t t1 = render_item_flat_uv(t, cell, 16);

		#ifdef GFX_3D_ELEMENTS
			// left layer
			displaylist_pos(&dl, 0, 256, -16);
			displaylist_color(&dl, light);
			displaylist_texcoord(&dl, s1, t);

			displaylist_pos(&dl, 0, 0, -16);
			displaylist_color(&dl, light);
			displaylist_texcoord(&dl, s1, t1);

			displaylist_pos(&dl, 256, 0, -16);
			displaylist_color(&dl, light);
			displaylist_texcoord(&dl, s, t1);

			displaylist_pos(&dl, 256, 256, -16);
			displaylist_color(&dl, light);
			displaylist_texcoord(&dl, s, t);
		#endif


		// right layer
		displaylist_pos(&dl, 0, 256, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, s1, t);

		displaylist_pos(&dl, 256, 256, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, s, t);

		displaylist_pos(&dl, 256, 0, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, s, t1);

		displaylist_pos(&dl, 0, 0, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, s1, t1);
		
		#ifdef GFX_3D_ELEMENTS
				for(int k = 0; k < 16; k++) {
					// Same 256 -> 255 edge clamp as s1/t1 above: at k == 15 the
					// k+1 coordinate reaches the unrepresentable 256 on a row/
					// column-15 tile. One strip = one icon texel: 1 atlas unit,
					// or 16 units on a private 16x16 needle surface (R7).
					uint8_t sk = render_item_flat_uv(s, cell, k);
					uint8_t tk = render_item_flat_uv(t, cell, k);
					uint8_t sk1 = render_item_flat_uv(s, cell, k + 1);
					uint8_t tk1 = render_item_flat_uv(t, cell, k + 1);
					// front
					displaylist_pos(&dl, 256 - (k + 1) * 16, 256, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk1, t);

					displaylist_pos(&dl, 256 - (k + 1) * 16, 0, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk1, t1);

					displaylist_pos(&dl, 256 - (k + 1) * 16, 0, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk, t1);

					displaylist_pos(&dl, 256 - (k + 1) * 16, 256, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk, t);

					// back
					displaylist_pos(&dl, 256 - k * 16, 256, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk1, t);

					displaylist_pos(&dl, 256 - k * 16, 256, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk, t);
					displaylist_pos(&dl, 256 - k * 16, 0, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk, t1);

					displaylist_pos(&dl, 256 - k * 16, 0, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_080));
					displaylist_texcoord(&dl, sk1, t1);

					// top
					displaylist_pos(&dl, 0, 256 - k * 16, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s1, tk);

					displaylist_pos(&dl, 0, 256 - k * 16, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s1, tk1);

					displaylist_pos(&dl, 256, 256 - k * 16, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s, tk1);

					displaylist_pos(&dl, 256, 256 - k * 16, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s, tk);

					// bottom
					displaylist_pos(&dl, 0, 256 - (k + 1) * 16, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s1, tk);

					displaylist_pos(&dl, 256, 256 - (k + 1) * 16, 0);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s, tk);

					displaylist_pos(&dl, 256, 256 - (k + 1) * 16, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s, tk1);

					displaylist_pos(&dl, 0, 256 - (k + 1) * 16, -16);
					displaylist_color(&dl, DIM_LIGHT(light, level_table_064));
					displaylist_texcoord(&dl, s1, tk1);
				}
		#endif

		mat4 model = GLM_MAT4_IDENTITY_INIT;

		switch(env) {
			case R_ITEM_ENV_THIRDPERSON:
				glm_translate_make(model, (vec3) {-4.0F, -8.0F, -7.0F});
				glm_rotate_x(model, glm_rad(60.0F), model);
				glm_scale(model, (vec3) {9.0F, 9.0F, 9.0F});

				glm_translate(model, (vec3) {0.5F, 0.2F, 0.5F});
				glm_scale_uni(model, 1.5F);
				glm_rotate_y(model, glm_rad(90.0F), model);
				glm_rotate_z(model, glm_rad(335.0F), model);
				glm_translate(
					model, (vec3) {1.0F / 16.0F - 1.0F, -1.0F / 16.0F, 0.0F});
				break;
			case R_ITEM_ENV_ENTITY:
				glm_mat4_identity(model);
				#ifdef GFX_3D_ELEMENTS
					glm_scale_uni(model, 1.5F);
				#else
					if (item_is_block(stack)) glm_scale_uni(model, 0.25F);
				#endif
				glm_translate_make(model,
								   (vec3) {0.0F, 0.0F, 0.5F + 1.0F / 32.0F});
				break;
			case R_ITEM_ENV_MOB_HAND:
				// Identity: render_entity's hand matrix (view) is the entire
				// Beta renderEquippedItems chain; the icon renders in its
				// native [0..1]^2 local space.
				break;
			case R_ITEM_ENV_FIRSTPERSON:
				glm_translate_make(model, (vec3) {0.5F, 0.2F, 0.5F});
				#ifdef GFX_3D_ELEMENTS
					glm_scale_uni(model, 1.5F);
					glm_rotate_y(model, glm_rad(50.0F), model);
					glm_rotate_z(model, glm_rad(335.0F), model);
					glm_translate(
						model, (vec3) {1.0F / 16.0F - 1.0F, -1.0F / 16.0F, 0.0F});			
				#else
					glm_scale_uni(model, 0.5F);
				#endif
				break;
			default: break;
		}

		mat4 modelview;
		glm_mat4_mul(view, model, modelview);
		#ifndef GFX_3D_ELEMENTS
			glm_mat4_ins3(GLM_MAT3_IDENTITY, modelview); //for billboarding
		#endif
		gfx_matrix_modelview(modelview);

		gfx_lighting(true);
		#ifdef GFX_3D_ELEMENTS
			displaylist_render_immediate(&dl, (2 + 16 * 4) * 4);
		#else
			//billboard item should need only 4 vertices
			displaylist_render_immediate(&dl, 4);
		#endif

		// Restore the LIT (GX_VTXFMT0 CLR0 = GX_INDEX8) vertex descriptor that the
		// world/entity render pass expects. gfx_lighting(false) would leave CLR0 in
		// DIRECT mode; the NEXT entity/world draw in the pass submits GX_Color1x8
		// (an INDEX8 colour), so a DIRECT descriptor desyncs the GX FIFO and HANGS
		// the Wii -- which is exactly why an item-ENTITY (dropped item / arrow,
		// the first ones ever rendered in v2) froze the frame. render_entity leaves
		// it lit after a mob for the same reason. The HUD held-item envs render
		// last in the frame, so they keep the unlit-after behaviour.
		gfx_lighting(env == R_ITEM_ENV_ENTITY || env == R_ITEM_ENV_MOB_HAND);
	}
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
}

#ifdef GFX_3D_ELEMENTS
// DC-GRASS G2: the world grass atlas cells are raw/untinted now (the mesher
// tints them per vertex), but this item display list carries no tint channel.
// The held / inventory / dropped grass block therefore samples item-only
// cells with the colour baked in ONCE (texture_atlas.c TEXAT_GRASS_*_ITEM:
// tile 0 x fallback top, tile 38 x fallback over tile 3 sides) -- exactly what
// it showed before G2. Bottom = dirt (BlockGrass.java:12-21 side 0 -> tile 2).
// Through block_info.texture_override, so render_block_full is unchanged.
static uint8_t grass_item_texture(struct block_info* this, enum side side) {
	(void)this;
	switch(side) {
		case SIDE_TOP: return tex_atlas_lookup(TEXAT_GRASS_TOP_ITEM);
		case SIDE_BOTTOM: return tex_atlas_lookup(TEXAT_DIRT);
		default: return tex_atlas_lookup(TEXAT_GRASS_SIDE_ITEM);
	}
}
#endif

void render_item_block(struct item* item, struct item_data* stack, mat4 view,
					   bool fullbright, enum render_item_env env) {
	assert(item && stack && view);
	assert(item_is_block(stack));
	#ifdef GFX_3D_ELEMENTS
		struct block* b = blocks[stack->id];
		assert(b);

		struct block_data neighbour_blk = (struct block_data) {
			.type = BLOCK_AIR,
			.metadata = 0,
			.sky_light = 15,
			.torch_light = 0,
		};

		struct block_info neighbour = (struct block_info) {
			.block = &neighbour_blk,
			.neighbours = NULL,
			.x = 0,
			.y = 0,
			.z = 0,
		};

		struct block_data this_blk = (struct block_data) {
			.type = stack->id,
			.metadata = item->render_data.block.has_default ?
				item->render_data.block.default_metadata :
				stack->durability,
			.sky_light = 15,
			.torch_light = env == R_ITEM_ENV_INVENTORY ? 15 : b->luminance,
		};

		struct block_data n[6] = {
			neighbour_blk, neighbour_blk, neighbour_blk,
			neighbour_blk, neighbour_blk, neighbour_blk,
		};

		struct block_info this = (struct block_info) {
			.block = &this_blk,
			.neighbours = n,
			.x = 0,
			.y = 0,
			.z = 0,
			.texture_override
			= stack->id == BLOCK_GRASS ? grass_item_texture : NULL,
		};

		displaylist_reset(&dl);

		size_t vertices = 0;

		for(int k = 0; k < 6; k++) {
			vertices += b->renderBlock(&dl, &this, (enum side)k, &neighbour,
									   fullbright ? vertex_light_inv : vertex_light,
									   false);
			if(b->renderBlockAlways)
				vertices += b->renderBlockAlways(
					&dl, &this, (enum side)k, &neighbour,
					fullbright ? vertex_light_inv : vertex_light, false);
		}

		mat4 model;

		if(env == R_ITEM_ENV_INVENTORY) {
			glm_translate_make(model, (vec3) {3 * 2, 3 * 2, -16});
			glm_scale(model, (vec3) {20, 20, -20});
			glm_translate(model, (vec3) {0.5F, 0.5F, 0.5F});
			glm_rotate_z(model, glm_rad(180.0F), model);
			glm_rotate_x(model, glm_rad(-30.0F), model);
			glm_rotate_y(
				model,
				glm_rad((item->render_data.block.has_default ?
							 item->render_data.block.default_rotation * 90.0F :
							 0)
						- 45.0F),
				model);
			glm_translate(model, (vec3) {-0.5F, -0.5F, -0.5F});
		} else if(env == R_ITEM_ENV_THIRDPERSON) {
			glm_translate_make(model, (vec3) {-4.0F, -14.0F, 3.0F});
			glm_rotate_x(model, glm_rad(22.5F), model);
			glm_rotate_y(model, glm_rad(45.0F), model);
			glm_scale(model, (vec3) {6.0F, 6.0F, 6.0F});
		} else {
			glm_mat4_identity(model);
		}

		mat4 modelview;
		glm_mat4_mul(view, model, modelview);
		gfx_matrix_modelview(modelview);

		gfx_bind_texture(b->transparent ? &texture_anim : &texture_terrain);
		gfx_lighting(true);
		displaylist_render_immediate(&dl, vertices * 4);
		gfx_matrix_modelview(GLM_MAT4_IDENTITY);
		// Same restore rule as the flat-item path above (and for the same
		// hard-won reason -- see that comment): in the ENTITY pass the NEXT
		// entity's immediate render submits GX_Color1x8 (INDEX8) bytes, so
		// leaving CLR0 in DIRECT desyncs the GX FIFO and can HANG the Wii.
		// This 3D-BLOCK path never got the flat path's fix: every dropped
		// BLOCK item (cobble/dirt -- the most common drops) left DIRECT
		// behind, and any mob drawn after it in the same pass streamed its
		// whole body malformed. HUD envs render last in the frame and keep
		// the unlit-after behaviour, exactly like the flat path.
		gfx_lighting(env == R_ITEM_ENV_ENTITY || env == R_ITEM_ENV_MOB_HAND);
	#else
		render_item_flat(item, stack, view, fullbright, env);
	#endif
}

/* --- held map (held-map render slice part 2, 2026-08-26) -------------------
 * Beta swaps the ENTIRE generic held-item render for a bespoke map sequence
 * when the held stack is Item.mapItem [740c583 ItemRenderer
 * .renderItemInFirstPerson:154-220]: (its own swing/equip chain) -> scale
 * 0.38 -> rotY 90 -> rotZ 180 -> translate(-1,-1,0) -> scale 1/64 -> the
 * misc/mapbg.png frame quad at (-7..135)^2 map pixels -> MapItemRenderer
 * .renderMap's live 128x128 colour texture. screen_ingame dispatches maps
 * BEFORE the generic held-item pose, with the acting owner's explicit
 * pitch/swing/equip inputs. Both hands use RenderPlayer.drawFirstPersonHand.
 * The map name label remains deferred; player icons are drawn below.
 *
 * The live surface re-uploads ONLY when the client mirror's version moved
 * (change-gated vs Beta's every-frame full upload -- the transport commit's
 * documented perf deviation; output-identical because the mirror only
 * versions on real byte change).
 *
 * ci-1101/23: ONE live surface and ONE change gate PER split viewport. Both
 * viewports render inside the same GX frame; tex_gfx_subimage on Wii is
 * DCStoreRange + GX_InvalidateTexAll, which never waits for the FIFO, so a
 * shared surface let P2's upload overwrite P1's still-queued draw (P1 showed
 * P2's map) and re-converted 64 KB twice every frame. Single player and the
 * existing test entry point stay on viewport 0 / texture_map_live. */
struct tex_gfx* render_item_map_live_texture(unsigned viewport) {
	return viewport == 0 ? &texture_map_live : &texture_map_live_split;
}

/* Viewport index of a held-map owner: its slot in gstate.players, 0 for any
 * owner outside that array (tests, single player stand-ins). */
static unsigned render_item_map_viewport_of(const struct client_session* owner) {
	for(unsigned v = 0; v < CLIENT_LOCAL_PLAYERS; v++)
		if(owner == &gstate.players[v]) return v;
	return 0;
}

bool render_item_map_upload_viewport(unsigned viewport, int32_t map_id) {
	/* Upload on mirror change. HEAP scratch, allocated once on first
	 * held-map render -- deliberately NOT a static: as a 64KB .bss array
	 * this buffer grew .bss by 0x10080 and shifted __Arena1Lo (and every
	 * runtime-placed region above it) up ~68KB, and real Broadway died
	 * pre-FAT on every boot -- the 2026-08-26 "breaker #2" hardware bisect
	 * (37385a8 content FAILS, revert PASSES, same launch path), mechanism
	 * identified 2026-08-27 by readelf layout diff after -fstack-usage
	 * refuted the stack-frame class. Heap placement leaves the static
	 * layout byte-identical to the proven-booting baseline. Render thread
	 * only; the scratch is consumed by tex_gfx_subimage before return, so
	 * one buffer serves every viewport. The change gate is per viewport. */
	static uint8_t* map_rgba = NULL;
	static uint32_t uploaded_version[CLIENT_LOCAL_PLAYERS];
	static int32_t uploaded_id[CLIENT_LOCAL_PLAYERS]
		= {INT32_MIN, INT32_MIN};
	static uint32_t uploaded_generation = 0;
	assert(viewport < CLIENT_LOCAL_PLAYERS);
	if(viewport >= CLIENT_LOCAL_PLAYERS) viewport = 0;
	/* R5: a texture-pack reload / cache restore recreates BOTH surfaces
	 * zero-filled (tex_map_live_init). Forget every viewport's upload so the
	 * unchanged map is re-uploaded instead of drawing blank until its
	 * version moves. */
	uint32_t generation = tex_map_live_generation();
	if(uploaded_generation != generation) {
		for(unsigned v = 0; v < CLIENT_LOCAL_PLAYERS; v++)
			uploaded_id[v] = INT32_MIN;
		uploaded_generation = generation;
	}
	if(!map_rgba) {
		map_rgba = malloc(128 * 128 * 4);
		if(!map_rgba) return false;
	}
	uint32_t version = 0;
	const uint8_t* colors
		= clin_held_map_colors(map_id, &version);
	/* [740c583 MapData.colors, MapItemRenderer.renderMap]: lookup/OOM
	 * misses draw the blank checkerboard, never a previous map's terrain. */
	if(uploaded_id[viewport] != map_id || uploaded_version[viewport] != version) {
		item_map_colors_to_rgba(colors, map_rgba);
		tex_gfx_subimage(render_item_map_live_texture(viewport), 0, 0, 128, 128,
			map_rgba);
		uploaded_id[viewport] = map_id;
		uploaded_version[viewport] = version;
	}

	return true;
}

bool render_item_map_upload(int32_t map_id) {
	return render_item_map_upload_viewport(0, map_id);
}

/* MapItemRenderer.java:81-103. Twice Beta's map coordinates, inverted Y. */
void render_item_map_marker_transform(const struct item_map_marker* marker,
	mat4 view, mat4 out, uint8_t uv[4][2]) {
	glm_mat4_copy(view, out);
	/* Display-list positions are fixed-point /256 on both backends. */
	glm_translate(out, (vec3){(128.0F + marker->x) / 256.0F,
		(128.0F - marker->y) / 256.0F, -9.0F / 256.0F});
	glm_rotate_z(out, glm_rad(-marker->rotation * 360.0F / 16.0F), out);
	glm_scale(out, (vec3){8.0F / 256.0F, 8.0F / 256.0F, 6.0F / 256.0F});
	glm_translate(out, (vec3){-0.125F, -0.125F, 0});
	/* Signed Java % and /; wrap negative UVs into the same atlas cells. */
	int u0 = ((marker->type % 4 + 4) % 4) * 64;
	int v0 = ((marker->type / 4 + 4) % 4) * 64;
	int u1 = u0 + 64, v1 = v0 + 64;
	if(u1 == 256) u1 = 255;
	if(v1 == 256) v1 = 255;
	const uint8_t coords[4][2] = {{u0,v0},{u1,v0},{u1,v1},{u0,v1}};
	memcpy(uv, coords, sizeof(coords));
}

void render_item_map_transforms(float pitch, float swing, float equip,
	mat4 surface, mat4 hands[2]) {
	/* RetroMCP src_original ItemRenderer.java:154-203, in OpenGL order. */
	float root = sinf(sqrtf(swing) * GLM_PI);
	float tilt = glm_clamp(1.0F - pitch / 45.0F + 0.1F, 0, 1);
	tilt = -cosf(tilt * GLM_PI) * 0.5F + 0.5F;
	mat4 base = GLM_MAT4_IDENTITY_INIT;
	glm_translate(base, (vec3){-root * 0.4F,
		sinf(sqrtf(swing) * GLM_PI * 2.0F) * 0.2F,
		-sinf(swing * GLM_PI) * 0.2F});
	glm_translate(base, (vec3){0,
		-(1.0F - equip) * 1.2F - tilt * 0.5F + 0.04F, -0.9F * 0.8F});
	glm_rotate_y(base, glm_rad(90.0F), base);
	glm_rotate_z(base, glm_rad(tilt * -85.0F), base);
	for(unsigned i = 0; i < 2; i++) {
		int sign = (int)i * 2 - 1;
		glm_mat4_copy(base, hands[i]);
		glm_translate(hands[i], (vec3){0, -0.6F, 1.1F * sign});
		glm_rotate_x(hands[i], glm_rad(-45.0F * sign), hands[i]);
		glm_rotate_z(hands[i], glm_rad(-90.0F), hands[i]);
		glm_rotate_z(hands[i], glm_rad(59.0F), hands[i]);
		glm_rotate_y(hands[i], glm_rad(-65.0F * sign), hands[i]);
		/* RenderPlayer.drawFirstPersonHand -> ModelBiped right arm:
		 * pivot(-5,2,0), age=0 roll=.1 rad, box(-3,-2,-2)..(1,10,2).
		 * render_model_box {4,4,12} emits (X,height,depth) with Y up;
		 * convert (u,v,w) to Java (u-3,10-v,2-w), preserving skin UVs. */
		glm_scale_uni(hands[i], 1.0F / 16.0F);
		glm_translate(hands[i], (vec3){-5, 2, 0});
		glm_rotate_z(hands[i], 0.1F, hands[i]);
		glm_translate(hands[i], (vec3){-3, 10, 2});
		glm_rotate_x(hands[i], glm_rad(180.0F), hands[i]);
	}
	glm_mat4_copy(base, surface);
	glm_rotate_y(surface, glm_rad(-sinf(swing * swing * GLM_PI) * 20.0F), surface);
	glm_rotate_z(surface, glm_rad(-root * 20.0F), surface);
	glm_rotate_x(surface, glm_rad(-root * 80.0F), surface);
	glm_scale_uni(surface, 0.38F);
	glm_rotate_y(surface, glm_rad(90.0F), surface);
	glm_rotate_z(surface, glm_rad(180.0F), surface);
	glm_translate(surface, (vec3){-1, -1, 0});
	glm_scale_uni(surface, 1.0F / 64.0F);
	/* Display lists emit twice the map pixels, inverted Y, divided by
	 * 256 by both backends. Convert that representation to Java pixels. */
	glm_translate(surface, (vec3){0, 128, 0});
	glm_scale(surface, (vec3){128, -128, 128});
}

void render_item_map(struct item* item, struct item_data* stack, mat4 view,
	bool fullbright, enum render_item_env env, const struct client_session* owner,
	float pitch, float swing, float equip) {
	assert(item && stack && view);

	if(env != R_ITEM_ENV_FIRSTPERSON) {
		/* Beta keeps the static sprite everywhere but first person. */
		render_item_flat(item, stack, view, fullbright, env);
		return;
	}

	assert(owner);
	unsigned viewport = render_item_map_viewport_of(owner);
	if(!render_item_map_upload_viewport(viewport, (int16_t)stack->durability)) {
		render_item_flat(item, stack, view, fullbright, env);
		return;
	}

	/* The current skin asset is shared; pose inputs are owner-local. */
	mat4 model, hands[2], modelview;
	render_item_map_transforms(pitch, swing, equip, model, hands);
	uint8_t light = fullbright ? *vertex_light_inv : *vertex_light;
	gfx_lighting(false);
	gfx_bind_texture(&texture_mob_char);
	for(unsigned i = 0; i < 2; i++) {
		mat4 arm;
		glm_mat4_mul(view, hands[i], arm);
		render_model_box(arm, (vec3){0, 0, 0}, (vec3){0, 0, 0},
			(vec3){0, 0, 0}, (ivec2){44, 20}, (ivec3){4, 4, 12},
			0.0F, false, gfx_lookup_light(light));
	}
	/* Arms submit direct RGBA; map display lists submit one light index.
	 * Restore INDEX8 before those bytes reach GX, then restore HUD direct. */
	gfx_lighting(true);
	glm_mat4_mul(view, model, modelview);
	gfx_matrix_modelview(modelview);

	/* Frame quad: Beta's 7-map-pixel border [ItemRenderer: byte var20 = 7]
	 * scaled to the 0..256 footprint (7 * 2 = 14 pos units). Full 0..1 UVs
	 * via the engine's u8 edge idiom (256 -> clamp 255, the documented
	 * 1/256 outer-edge shave). Optional asset: absent mapbg draws nothing
	 * (texture.c precedent). Drawn at z=0; the live surface uses Beta's
	 * -0.01 map-pixel depth below. */
	/* Retain the existing two-winding display list; backface culling selects
	 * the visible winding after converting the inverted map Y axis. */
	if(texture_map_bg.data) {
		gfx_bind_texture(&texture_map_bg);
		displaylist_reset(&dl);
		displaylist_pos(&dl, -14, 270, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 0, 0);
		displaylist_pos(&dl, 270, 270, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 255, 0);
		displaylist_pos(&dl, 270, -14, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 255, 255);
		displaylist_pos(&dl, -14, -14, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 0, 255);
		/* reversed winding */
		displaylist_pos(&dl, -14, -14, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 0, 255);
		displaylist_pos(&dl, 270, -14, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 255, 255);
		displaylist_pos(&dl, 270, 270, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 255, 0);
		displaylist_pos(&dl, -14, 270, 0);
		displaylist_color(&dl, light);
		displaylist_texcoord(&dl, 0, 0);
		displaylist_render_immediate(&dl, 8);
	}

	/* The live colour surface: map row 0 (north) at the TOP, so V=0 maps to
	 * the top edge. u8 texcoords at scale 256/128 = 2 with the 256->255
	 * edge clamp. */
	mat4 live;
	glm_mat4_copy(modelview, live);
	glm_translate(live, (vec3){0, 0, -0.01F / 128.0F});
	gfx_matrix_modelview(live);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(false);
	gfx_bind_texture(render_item_map_live_texture(viewport));
	displaylist_reset(&dl);
	displaylist_pos(&dl, 0, 256, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 0, 0);
	displaylist_pos(&dl, 256, 256, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 255, 0);
	displaylist_pos(&dl, 256, 0, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 255, 255);
	displaylist_pos(&dl, 0, 0, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 0, 255);
	/* reversed winding */
	displaylist_pos(&dl, 0, 0, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 0, 255);
	displaylist_pos(&dl, 256, 0, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 255, 255);
	displaylist_pos(&dl, 256, 256, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 255, 0);
	displaylist_pos(&dl, 0, 256, 0);
	displaylist_color(&dl, light);
	displaylist_texcoord(&dl, 0, 0);
	displaylist_render_immediate(&dl, 8);
	gfx_alpha_test(true);
	gfx_blending(MODE_OFF);

	/* RetroMCP MapItemRenderer.java:81-103: translate(x/2+64,y/2+64),
	 * rotate(rotation*360/16), scale(4,4,3), translate(-1/8,+1/8).
	 * Our map footprint is twice Beta's, with map Y inverted. */
	if(texture_map_icons.data) {
		mat4 marker_view;
		glm_mat4_copy(modelview, marker_view);
		/* The legacy icon helper includes a -9/256 depth bias. Convert it
		 * to MapItemRenderer's -0.02 pixel depth in the new map frame. */
		glm_translate(marker_view, (vec3){0, 0, 9.0F / 256.0F - 0.02F / 128.0F});
		struct item_map_marker markers[ITEM_MAP_MAX_MARKERS];
		size_t count = clin_held_map_markers((int16_t)stack->durability, markers);
		gfx_bind_texture(&texture_map_icons);
		for(size_t i = 0; i < count; i++) {
			const struct item_map_marker* marker = &markers[i];
			mat4 icon;
			uint8_t uv[4][2];
			render_item_map_marker_transform(marker, marker_view, icon, uv);
			gfx_matrix_modelview(icon);
			const int16_t xy[4][2] = {{-256,-256},{256,-256},{256,256},{-256,256}};
			displaylist_reset(&dl);
			for(unsigned winding = 0; winding < 2; winding++) {
				for(unsigned k = 0; k < 4; k++) {
					unsigned corner = winding ? 3 - k : k;
					displaylist_pos(&dl, xy[corner][0], xy[corner][1], 0);
					displaylist_color(&dl, light);
					displaylist_texcoord(&dl, uv[corner][0], uv[corner][1]);
				}
			}
			displaylist_render_immediate(&dl, 8);
		}
	}

	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_lighting(false);
}
