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

#ifndef TEXTURE_H
#define TEXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "texture_mip.h"

/* Main-thread loading service, with the same no-reentry/no-resource-free
 * contract as audio_registry_set_service. The frame must use independent
 * assets: textures are replaced in place during tex_init. Register before
 * tex_init and keep installed through backdrop publication/upload. It may run
 * several times per display frame; throttle actual presentation in the adapter.
 * Individual decode/upload and libc I/O calls remain measured by the adapter. */
typedef void (*tex_load_service_fn)(void* ctx);
void tex_set_load_service(tex_load_service_fn fn, void* ctx);
enum tex_ready { TEX_LOADING, TEX_READY, TEX_FAILED };
enum tex_ready tex_ready_state(void);
const char* tex_failure_reason(void);
/* Retry is the existing tex_init() entry; its bool and this state agree. */

#ifdef PLATFORM_WII
#include <gccore.h>
#endif

#ifdef PLATFORM_PC
#include <GL/glew.h>
#endif

enum tex_format {
	TEX_FMT_RGB16,
	TEX_FMT_RGBA16,
	TEX_FMT_RGBA32,
	TEX_FMT_I8,
	TEX_FMT_IA4,
};

struct tex_gfx {
#ifdef PLATFORM_WII
	GXTexObj obj;
#endif
#ifdef PLATFORM_PC
	GLuint id;
#endif
	size_t width, height;
	enum tex_format fmt;
	uint8_t* data;
	/* pack-cache-0927: what the loader produced, so the pack cache can
	   serialize it and replay the identical install. data_bytes = bytes at
	   `data` (Wii: GX texels incl. the mip chain; PC: width*height*4 RGBA);
	   lods = 1, or 4 for a Wii atlas mip chain (PC: 1, mips regenerated);
	   linear = the filter flag the load was made with. */
	size_t data_bytes;
	uint8_t lods;
	bool linear;
};

extern struct tex_gfx texture_fog;
extern struct tex_gfx texture_terrain;
/* Approved sign board median color, refreshed when the active pack loads. */
extern uint8_t texture_sign_color[3];
extern struct tex_gfx texture_particles;
extern struct tex_gfx texture_items;
/* Beta RenderArrow item/arrows.png. Optional pack art: NULL keeps the
 * existing item-model fallback when neither selected nor bundled pack supplies it. */
extern struct tex_gfx texture_arrow;
extern struct tex_gfx texture_minecart;
// Beta's painting motive atlas ("/art/kz.png" [740c583: RenderPainting.
// func_158_a]), 256x256. Absent from the bundled fCavEX pack (Mojang IP,
// same policy as every other Beta-exclusive asset) -- .data stays NULL until
// a real Beta pack supplies it; entity_painting.c's render function checks
// .data itself and draws nothing rather than garbage when it is unloaded.
extern struct tex_gfx texture_painting;
extern struct tex_gfx texture_creeper;
extern struct tex_gfx texture_zombie;
extern struct tex_gfx texture_skeleton;
extern struct tex_gfx texture_spider;
// Beta 1.7.3 mob/spider_eyes.png: the EMISSIVE eye OVERLAY, transparent except the
// glowing eye pixels. Drawn full-bright over the head with alpha-test so ONLY the
// eyes glow (not the surrounding face). NULL .data when a pack omits it -> no glow.
extern struct tex_gfx texture_spider_eyes;
extern struct tex_gfx texture_pig;
extern struct tex_gfx texture_wolf;
extern struct tex_gfx texture_wolf_tame;
extern struct tex_gfx texture_wolf_angry;
extern struct tex_gfx texture_cow;
extern struct tex_gfx texture_chicken;
extern struct tex_gfx texture_sheep;
extern struct tex_gfx texture_sheep_fur;
// Beta squid (mob/squid.png -- ships in the bundled pack) + slime
// (mob/slime.png -- ABSENT until the legal pack lands; NULL .data -> the
// renderers fall back to the creeper skin; RGBA16 keeps the gel's alpha when
// a real Beta pack overlays it).
extern struct tex_gfx texture_squid;
extern struct tex_gfx texture_slime;
// Nether natives (batch 1b): pigzombie biped skin + the ghast pair (base +
// fire attack variant). Absent from a pack -> the usual creeper fallback.
extern struct tex_gfx texture_pigzombie;
extern struct tex_gfx texture_ghast;
extern struct tex_gfx texture_ghast_fire;
extern struct tex_gfx texture_font;
extern struct tex_gfx texture_anim;
extern struct tex_gfx texture_gui_inventory;
extern struct tex_gfx texture_gui_crafting;
extern struct tex_gfx texture_gui_furnace;
extern struct tex_gfx texture_gui_chest;
extern struct tex_gfx texture_gui_dispenser; // Beta gui/trap.png (GuiDispenser)
extern struct tex_gfx texture_gui_iron_chest;
extern struct tex_gfx texture_gui2;
// Beta gui/icons.png (hearts, armour, air bubbles, ...). Kept separate from the
// combined gui_2 atlas so a genuine Beta texture pack supplies its authentic
// HUD icons -- see screen_ingame.c.
extern struct tex_gfx texture_icons;
// Beta gui/gui.png (hotbar widget + selected-slot highlight). Kept separate from
// fCavEX's gui_2 atlas so a genuine Beta pack supplies its authentic hotbar.
extern struct tex_gfx texture_gui_widgets;
extern struct tex_gfx texture_controls;
/* OPTIONAL scenic front-end backdrop (gui/menu-background.png). Resolved by the
 * ORDINARY pack-first/bundled-fallback path -- a pack that ships one themes the
 * front end, a pack that does not falls back to the bundled art, and a build
 * with neither leaves .data NULL. It is NOT a cache and NOT a per-pack
 * thumbnail: one texture object, replaced on every tex_init() like every other
 * entry here, so it inherits the same GPU fence/lifecycle. Consumers MUST check
 * .data (frontend_presentation.h's fep_backdrop does) and fall back to the
 * tiled-dirt menu background when it is absent. */
extern struct tex_gfx texture_menu_background, texture_nether_background;
/* Original title logo + subtitle tablet; bundled-only, optional, exact RGBA8. */
extern struct tex_gfx texture_title_logo;
extern struct tex_gfx texture_menu_reception;
extern struct tex_gfx texture_approved_controls;
extern struct tex_gfx texture_controller_pictures;
extern struct tex_gfx texture_pointer;
extern struct tex_gfx texture_clouds;
// render-slice (RENDER_DISPATCH.md): EntityRenderer.renderRainSnow's two
// precipitation textures [740c583]. Verified against the owned jar
// (eae3353f): snow.png is exhaustively grayscale (IA4 lossless); rain.png is
// genuinely blue-tinted RGBA (max per-channel delta 125 among visible
// texels) -- IA4 would silently discard that tint, so it stays RGBA16.
extern struct tex_gfx texture_rain;
extern struct tex_gfx texture_snow;
extern struct tex_gfx texture_sun;
extern struct tex_gfx texture_moon;
extern struct tex_gfx texture_shadow;
// Held-map render slice: the Beta misc/mapbg.png frame (optional asset) and
// the live 128x128 colour surface render_item_map re-uploads on change.
extern struct tex_gfx texture_map_bg;
extern struct tex_gfx texture_map_icons; /* optional pack misc/mapicons.png */
extern struct tex_gfx texture_map_live;
// ci-1101/23: the SECOND split viewport's live map surface. Both viewports
// draw in one GX frame, and a texture upload does not wait for the queued
// draw of the other viewport, so each viewport owns its own 128x128 surface
// (render_item_map_live_texture(viewport)). Single player uses texture_map_live.
extern struct tex_gfx texture_map_live_split;
// (Re)create both live map surfaces zero-filled (pack load, cache restore,
// pack reload) and bump the generation the held-map upload gate keys on.
void tex_map_live_init(void);
uint32_t tex_map_live_generation(void);
extern struct tex_gfx texture_water;
// Beta 1.7.3 misc/dial.png -- TextureWatchFX's rotated dial-ring texture. Mojang
// IP, never bundled (same policy as every other Beta-exclusive asset absent from
// the fCavEX pack, e.g. texture_painting's art/kz.png); .data stays NULL until a
// real Beta pack supplies it. item_needle_fx_tick() gates the clock-needle FX on
// this being non-NULL and a real 16x16, exactly like texture_painting's render
// function checking .data before drawing (see compass_fx.h / watch_fx.h).
extern struct tex_gfx texture_dial;

extern struct tex_gfx texture_mob_char;

extern struct tex_gfx texture_armor_chain1;
extern struct tex_gfx texture_armor_chain2;
extern struct tex_gfx texture_armor_cloth1;
extern struct tex_gfx texture_armor_cloth2;
extern struct tex_gfx texture_armor_gold1;
extern struct tex_gfx texture_armor_gold2;
extern struct tex_gfx texture_armor_iron1;
extern struct tex_gfx texture_armor_iron2;
extern struct tex_gfx texture_armor_diamond1;
extern struct tex_gfx texture_armor_diamond2;

/* Called ONCE at the top of tex_init(), before any texture object is replaced.
   Backend setup, and on Wii also a GPU FENCE: tex_init is re-entrant (the
   texture-pack screen and world-select both call it at runtime, from the screen
   update that runs immediately BEFORE gfx_flip_buffers) and the reload frees each
   old texel buffer with mem2_free while the previous frame's draws may still be
   reading it. That memory goes straight back to the heap the display lists churn
   through, so the GP can end up streaming display-list bytes as texels. One fence
   per reload, not one per texture. */
void tex_init_pre(void);
void tex_init_post(void);
// Returns false when a core texture needed for safe rendering did not load.
// Optional pack assets may remain absent without failing initialization.
bool tex_init(void);

uint8_t* tex_read(const char* filename, size_t* width, size_t* height);
/* True for the controller-art sheets that always load from the bundled
 * assets (SD-root assets/ on Wii) and never from a texture pack. */
bool tex_bundled_only(const char* filename);
/* The roots a bundled-only file (and the boot credits) is looked up in, in
   order: Wii {"sd:/assets", "assets"} (the app-folder copy when the player
   installed only apps/CavEX/), PC {"assets"}. Returns the count written
   (at most `max`). The _for variant takes the platform explicitly so a PC
   test can check the Wii list. */
size_t tex_bundled_roots_for(bool wii, const char** roots, size_t max);
size_t tex_bundled_roots(const char** roots, size_t max);
uint8_t* tex_read_gfx(const char* filename, size_t* width, size_t* height,
					  enum tex_format type);
uint8_t* tex_read_atlas(const char* filename, size_t* width, size_t* height);

/* GX's converters consume fixed 4x4 or 8x4 tiles. Apply the same validation
 * on PC so a pack accepted there cannot fail only after it reaches a Wii. */
static inline bool tex_image_dimensions_valid(size_t width, size_t height) {
	return width > 0 && height > 0 && width <= 1024 && height <= 1024;
}

static inline bool tex_gfx_dimensions_valid(size_t width, size_t height,
										 enum tex_format type) {
	if(!tex_image_dimensions_valid(width, height))
		return false;
	size_t wmod = (type == TEX_FMT_I8 || type == TEX_FMT_IA4) ? 8 : 4;
	return (width % wmod) == 0 && (height % 4) == 0;
}

bool tex_gfx_load(struct tex_gfx* tex, void* img, size_t width, size_t height,
				  enum tex_format type, bool linear);
// Atlas variant: tells the loader the exact origin and pitch of the logical
// cells (including the atlas builder's leading margin and edge padding).
// On Wii this triggers software
// generation of 4 mip levels with per-tile clamping so neighbouring
// tiles don't bleed into each other at small mips, and sets the texobj
// to GX_LIN_MIP_LIN trilinear filtering. On PC it currently behaves
// identically to tex_gfx_load with linear=true — adding mipmap support
// on the GL side is a follow-up.
bool tex_gfx_load_atlas(struct tex_gfx* tex, void* img, size_t width,
						size_t height, enum tex_format type,
						const struct tex_atlas_grid* grid);
/* Missing from both selected and default packs clears the previous optional
 * asset and succeeds. Decode/dimension/backend failures return false. */
bool tex_gfx_load_file(struct tex_gfx* tex, const char* filename,
					   enum tex_format type, bool linear);
/* ===== PER-PACK PREVIEW THUMBNAIL ======================================
 * The pack selector needs the image of the pack a ROW names, which is never
 * the pack the game is currently running. So this reads from an EXPLICIT
 * installed root -- the caller's own string, already platform-resolved -- and
 * touches neither paths.texturepack nor any global atlas: it cannot show one
 * pack's art on another pack's row, and it cannot disturb what is on screen.
 *
 * "<root>/pack.png" first (a pack that ships a deliberate preview), then
 * "<root>/terrain.png" (every real pack has one), decoded through the same
 * bounded/validated PNG path as the atlases and nearest-downsampled to a
 * TEX_PREVIEW_DIM square. Neither present, or either corrupt, is NOT a
 * failure of the screen: it returns false with `tex` CLEARED, and the caller
 * draws its neutral tile.
 *
 * LIFECYCLE, on the caller: `tex` is cleared FIRST, so the outcome is
 * unambiguous (the new preview, or empty -- never the previous row's image
 * behind a failed load). That free is the same hazard tex_init() has, so a
 * caller replacing a texture the frame in flight may still be reading must
 * bracket with tex_init_pre()/tex_init_post() exactly as tex_init does. */
#define TEX_PREVIEW_DIM 40
bool tex_preview_load(struct tex_gfx* tex, const char* pack_root);
void tex_gfx_clear(struct tex_gfx* tex);
/* Optional pack-owned Nether scene. Never falls back to another pack. */
int tex_scene_backdrop_state_of(const char* pack, bool nether);
int tex_scene_backdrop_publish(const char* pack, const uint8_t* rgba, unsigned w, unsigned h, bool nether);
bool tex_scene_background_load(bool nether);
/* pack-cache-0927 backend hooks (texture_cache.c). The texel buffer a cache
   hit reads into is allocated where the backend's own loader would put it
   (Wii: MEM2 CARVE_TEXTURE; PC: malloc). tex_gfx_restore installs it exactly
   as the matching load would finish -- Wii: `data` is the GX bytes and the
   GXTexObj is initialised with the same calls; PC: `data` is the RGBA image
   and goes through tex_gfx_load / tex_gfx_load_atlas (atlas mips rebuilt
   from the identical LOD 0). Ownership of `data` passes on every path. */
void* tex_cache_texel_alloc(size_t bytes);
void tex_cache_texel_free(void* p);
bool tex_gfx_restore(struct tex_gfx* tex, void* data, size_t data_bytes,
					 size_t width, size_t height, enum tex_format type,
					 unsigned lods, bool linear, bool atlas);
/* The texture-read observer (texture_cache.c records every file a recorded
   load examines). root_tag: 0 = the selected pack, 1 = the bundled default,
   2 = the bundled-only controller-art root. NULL = none (the default). */
typedef void (*tex_read_observer_fn)(int root_tag, const char* filename);
void tex_set_read_observer(tex_read_observer_fn fn);
/* Resolved path of a root tag for the CURRENT configuration. */
const char* tex_root_for_tag(int root_tag);
void tex_gfx_bind(struct tex_gfx* tex, int slot);
void tex_gfx_lookup(struct tex_gfx* tex, int x, int y, uint8_t* color);
void tex_gfx_wrap_mode(struct tex_gfx* tex, bool repeat);

// In-place update of a w*h RGBA8 region at atlas pixel (x,y) -- used by the animated
// water FX. PC: glTexSubImage2D + CPU-mirror update. Wii: RGB5A3 quantize + GX 4x4
// swizzle into LOD0 + DCStoreRange(touched range) + GX_InvalidateTexAll (the loader's
// flush-then-invalidate discipline; in-place, no allocation on the hot path).
void tex_gfx_subimage(struct tex_gfx* tex, int x, int y, int w, int h,
					  const uint8_t* rgba);

// Beta 1.7.3 TextureWaterFX animated still-water. water_fx_anim_init() runs after the
// block atlas is built (it reads the captured water slot); water_fx_anim_tick() advances
// one 20Hz client tick (drive it gated by !paused). No-op until the slot is captured.
void water_fx_anim_init(void);
void water_fx_anim_tick(void);

// Beta 1.7.3 TextureCompassFX / TextureWatchFX animated item-icon needles (see
// source/graphics/compass_fx.h and watch_fx.h for the ported algorithms).
// item_needle_fx_init() runs after texture_items (and the optional texture_dial)
// are loaded -- captures the static compass/clock icon cells (and the dial ring,
// if present) and resets the damped-spring smoothing state; safe to call again on
// every tex_init() re-entry (pack reload / world select), like water_fx_anim_init.
// item_needle_fx_tick(enabled) advances one 20Hz client tick and re-uploads the
// two 16x16 cells in place via tex_gfx_subimage; `enabled` is the caller's
// "gfx.item_needle_anim" config gate (main.c) -- false is a cheap no-op (no
// capture, no upload), matching water_fx_anim_tick's !paused gate shape. A
// missing/oversized items.png (non-vanilla atlas layout) or a missing dial.png
// degrade gracefully to the pre-existing static icon, never a crash.
void item_needle_fx_init(void);
void item_needle_fx_tick(bool enabled);
// ci-1101/31: split screen. Every local player (gstate.players[v]) has its own
// damped-spring needle state. Viewport 0 keeps writing the atlas cells exactly
// as before (single player and viewport 0 are byte-identical). Viewport 1's
// compass and clock are composited into a PRIVATE 16x16 texture pair instead
// (the atlas cell cannot be rewritten between the two viewport passes: both
// draw in one GX frame and tex_gfx_subimage never waits for the queued draw --
// the same hazard as the held map, ci-1101/23). The item draw path
// (render_item_flat) binds that texture, whole, for a compass/clock drawn in
// viewport 1. Returns NULL for viewport 0, for an unarmed FX, for a viewport
// whose player has no body, or before the first enabled tick wrote it -- the
// caller then samples the atlas cell as usual.
struct tex_gfx* item_needle_fx_split_texture(unsigned viewport, bool clock);

/* BG-BAKE-1028/3: the pack's menu backdrop (texture.c). States/results are
 * int-valued: tex_backdrop_state_of returns 0 bundled, 1 author art,
 * 2 verified generated cache, 3 missing, 4 stale, 5 corrupt;
 * tex_backdrop_publish returns 0 when published, else a reason code for
 * tex_backdrop_publish_reason. The generated cache lives in
 * <pack>/.cavewii/backdrop/ and never replaces author art. */
int tex_backdrop_state_of(const char* pack);
uint64_t tex_backdrop_identity(const char* pack);
int tex_backdrop_publish(const char* pack, const uint8_t* rgba, unsigned width,
	unsigned height);
const char* tex_backdrop_publish_reason(int r);
/* Reload texture_menu_background by the backdrop resolution (fence first). */
bool tex_menu_background_load(void);
/* Runtime-armed test seams (never armed by the engine). */
void tex_backdrop_test_fail(int op, unsigned nth);
void tex_backdrop_test_stop_after(unsigned step);

#endif
