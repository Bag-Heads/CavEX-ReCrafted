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

#include <assert.h>
#include <m-lib/m-string.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../daytime.h"
#include "../game/game_state.h"
#include "../graphics/biome_color.h"
#include "../graphics/compass_fx.h"
#include "../graphics/flame_fx.h" // anim.png fire+16 strip fallback
#include "../graphics/gui_util.h"
#include "../graphics/texture_atlas.h"
#include "../graphics/water_fx.h"
#include "../graphics/watch_fx.h"
#include "mem2.h"
#include "texture.h"
#include "texture_cache.h" /* pack-cache-0927: opt-in pre-baked textures */
#include "tex_beta_compose.h" /* raw-jar pack safety net */
#include "texture_decode.h"
#include "thread.h"
#include "../game/gui/backdrop_bake.h" /* BACKDROP_BAKE_* recipe/output constants only */
#include "../lodepng/lodepng.h"
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static tex_load_service_fn s_load_service;
static void* s_load_service_ctx;
#ifdef PLATFORM_PC
static pthread_t s_load_service_thread;
#else
static lwp_t s_load_service_thread;
#endif
static bool s_servicing;
static enum tex_ready s_ready = TEX_LOADING;
static const char* s_failure = "";

void tex_set_load_service(tex_load_service_fn fn, void* ctx) {
	s_load_service = fn;
	s_load_service_ctx = ctx;
#ifdef PLATFORM_PC
	s_load_service_thread = pthread_self();
#else
	s_load_service_thread = LWP_GetSelf();
#endif
}

static void tex_load_service(void) {
	if(!s_load_service) return;
#ifdef PLATFORM_PC
	if(!pthread_equal(pthread_self(), s_load_service_thread)) return;
#else
	if(LWP_GetSelf() != s_load_service_thread) return;
#endif
	if(s_servicing) return;
	s_servicing = true;
	s_load_service(s_load_service_ctx);
	s_servicing = false;
}

enum tex_ready tex_ready_state(void) { return s_ready; }
const char* tex_failure_reason(void) { return s_failure; }

struct tex_gfx texture_fog;
struct tex_gfx texture_terrain;
uint8_t texture_sign_color[3] = {149,115,72};
struct tex_gfx texture_particles;
struct tex_gfx texture_items;
struct tex_gfx texture_arrow;
struct tex_gfx texture_minecart;
struct tex_gfx texture_painting;
struct tex_gfx texture_creeper;
struct tex_gfx texture_zombie;
struct tex_gfx texture_skeleton;
struct tex_gfx texture_spider;
struct tex_gfx texture_spider_eyes;
struct tex_gfx texture_pig;
struct tex_gfx texture_wolf;
struct tex_gfx texture_wolf_tame;
struct tex_gfx texture_wolf_angry;
struct tex_gfx texture_cow;
struct tex_gfx texture_chicken;
struct tex_gfx texture_sheep;
struct tex_gfx texture_sheep_fur;
struct tex_gfx texture_squid;
struct tex_gfx texture_pigzombie;
struct tex_gfx texture_ghast;
struct tex_gfx texture_ghast_fire;
struct tex_gfx texture_slime;
struct tex_gfx texture_font;
struct tex_gfx texture_anim;
struct tex_gfx texture_gui_inventory;
struct tex_gfx texture_gui_crafting;
struct tex_gfx texture_gui_furnace;
struct tex_gfx texture_gui_chest;
struct tex_gfx texture_gui_dispenser;
struct tex_gfx texture_gui_iron_chest;
struct tex_gfx texture_gui2;
struct tex_gfx texture_icons;
struct tex_gfx texture_gui_widgets;
struct tex_gfx texture_controls;
struct tex_gfx texture_menu_background; // optional scenic front-end backdrop
struct tex_gfx texture_title_logo; // optional bundled title branding
struct tex_gfx texture_menu_reception; // optional original 9-frame server status sheet
struct tex_gfx texture_approved_controls;
struct tex_gfx texture_controller_pictures;
struct tex_gfx texture_pointer;
struct tex_gfx texture_clouds;
struct tex_gfx texture_rain;
struct tex_gfx texture_snow;
struct tex_gfx texture_sun;
struct tex_gfx texture_moon;
struct tex_gfx texture_shadow;
struct tex_gfx texture_map_bg;   // held-map frame (optional asset)
struct tex_gfx texture_map_icons;
struct tex_gfx texture_map_live; // held-map live 128x128 surface
struct tex_gfx texture_water;
struct tex_gfx texture_dial; // TextureWatchFX dial ring (optional; see texture.h)

struct tex_gfx texture_mob_char;

struct tex_gfx texture_armor_chain1;
struct tex_gfx texture_armor_chain2;
struct tex_gfx texture_armor_cloth1;
struct tex_gfx texture_armor_cloth2;
struct tex_gfx texture_armor_gold1;
struct tex_gfx texture_armor_gold2;
struct tex_gfx texture_armor_iron1;
struct tex_gfx texture_armor_iron2;
struct tex_gfx texture_armor_diamond1;
struct tex_gfx texture_armor_diamond2;

#define distance_2d(x1, y1, x2, y2)                                            \
	(((x1) - (x2)) * ((x1) - (x2)) + ((y1) - (y2)) * ((y1) - (y2)))

static void gen_texture_fog(uint8_t* img, size_t size) {
	// Radial fog ramp sampled by world distance (TEXMTX1 in wii/gfx.c maps the far edge R
	// to radius size/2). Beta 1.7.3 terrain fog = GL_LINEAR start 0.25*R, end R
	// (EntityRenderer.setupFog, 740c583): fade over the outer 75% for a smooth terrain->sky
	// dissolve. The old (size/2 - 9)/8 form was an 8-texel hard edge window => abrupt horizon.
	// LOCKSTEP with the PC shader (resources/fragment.shader): identical 0.25R..R remap.
	const float R = size / 2.0F;
	for(size_t y = 0; y < size; y++) {
		for(size_t x = 0; x < size; x++) {
			float d = (sqrt(distance_2d(R, R, x + 0.5F, y + 0.5F)) - R * 0.25F)
				/ (R * 0.75F);

			uint8_t* pixel = img + (x + y * size) * 4;
			pixel[0] = pixel[1] = pixel[2]
				= roundf(glm_clamp(d * 255.0F, 0.0F, 255.0F));
			pixel[3] = 255;
		}
	}
}

/* menus.html signFace(): stable luminance-order median of the plank tile.
 * Read once per pack load, retaining no source image or per-frame allocation. */
static void sign_color_init(void) {
 size_t width=0,height=0;uint8_t* image=tex_read("terrain.png",&width,&height);
 texture_sign_color[0]=149;texture_sign_color[1]=115;texture_sign_color[2]=72;
 if(!image)return;
 if(width>=256&&height>=256&&width%16==0&&height%16==0) {
  uint8_t colors[256][3];size_t tile=width/16;
  for(size_t k=0;k<256;k++) {
   size_t x=4*tile+(k%16)*tile/16,y=(k/16)*(height/16)/16;
   memcpy(colors[k],image+4*(y*width+x),3);
   /* Stable insertion preserves the source order for equal luminance. */
   size_t at=k;uint8_t color[3];memcpy(color,colors[k],3);
   unsigned sum=(unsigned)color[0]+color[1]+color[2];
   while(at&&sum<(unsigned)colors[at-1][0]+colors[at-1][1]+colors[at-1][2]) {
    memcpy(colors[at],colors[at-1],3);at--;
   }
   memcpy(colors[at],color,3);
  }
  memcpy(texture_sign_color,colors[128],3);
 }
 free(image);
}

static void tex_map_live_init(void);
/* Raw-jar pack helpers (defined after the pack-root resolver below). */
static bool tex_raw_jar_pack(void);
static uint8_t* tex_raw_jar_compose(bool anim);
static bool tex_raw_jar_pack_has(const char* filename);

/* The pack load proper: every file-backed texture, the sign colour, the
   biome colormaps and both atlases, in the original order. pack-cache-0927:
   each load site reports its slot (tex_cache_note_slot, a no-op unless a
   cache-on miss is being recorded). */
static bool tex_load_pack(void) {
	bool loaded_ok = true;
	sign_color_init();

	// Beta biome grass/foliage colormaps: now consumed by the chunk mesher to
	// tint grass/leaf faces (#77b). Loaded from the active texture pack
	// (misc/grasscolor.png, misc/foliagecolor.png); if the pack omits them the
	// lookups return white (no tint), so this stays graceful.
	biome_color_init();

	size_t w = 0, h = 0;
	void* output = tex_atlas_block("terrain.png", &w, &h);
	if(output) {
		struct tex_atlas_grid grid;
		bool grid_ok = tex_atlas_grid_init(&grid, w, h);
		assert(grid_ok);
		if(!grid_ok) {
			free(output);
			output = NULL;
		}
		if(output
		   && !tex_gfx_load_atlas(&texture_terrain, output, w, h,
							  TEX_FMT_RGBA16, &grid))
			loaded_ok = false;
	}
	if(!output) {
		tex_gfx_clear(&texture_terrain);
		loaded_ok = false;
	}
	tex_cache_note_slot(&texture_terrain);

	// Beta-standard paths (so a real Beta pack fully overrides; the bundled
	// fCavEX assets are laid out to match). Font: Beta font/default.png.
#define TEX_LOAD_FILE(tex, filename, type, linear)                             \
	do {                                                                         \
		if(!tex_gfx_load_file((tex), (filename), (type), (linear))) {               \
			loaded_ok = false;                                                       \
			if(!s_failure[0]) s_failure = "Required texture unavailable: " filename;   \
		}                                                                          \
		tex_cache_note_slot(tex);                                                  \
	} while(0)
	TEX_LOAD_FILE(&texture_font, "font/default.png", TEX_FMT_I8, false);
	// A failed first load leaves zero dimensions. gutil_reset_font divides by
	// the per-character width, so only rebuild metrics from a usable texture.
	if(texture_font.data && texture_font.width >= 16 && texture_font.height >= 16)
		gutil_reset_font(&texture_font);

	// anim.png: tex_gfx_load_file's body, open-coded so the decoded RGBA can be
	// repaired before tex_gfx_load takes ownership of it: a pack without
	// Beta's second fire strip (cols 6 & 13 transparent or the old magenta
	// placeholder) gets cols 3/10 copied there, so every fire+16 user can
	// sample cols 6/13 unconditionally -- including chunk display lists
	// baked under the previous pack (packs can be applied in-game).
	// A raw b1.7.3.jar unzip (tex_beta_compose.h) has no anim.png: build the
	// Pack Helper's one -- beta-base Beta colours + the pack's own ice --
	// instead of falling back to the Earth & Ash sheet.
	const bool raw_jar = tex_raw_jar_pack();
	{
		size_t aw = 0, ah = 0;
		uint8_t* anim = NULL;
		if(raw_jar && !tex_raw_jar_pack_has("anim.png")
		   && tex_raw_jar_pack_has("terrain.png")) {
			anim = tex_raw_jar_compose(true);
			if(anim)
				aw = ah = TEX_BETA_CANON;
		}
		if(!anim)
			anim = tex_read_gfx("anim.png", &aw, &ah, TEX_FMT_RGBA32);
		if(anim)
			flame_fx_anim_prepare_pack_atlas(anim, aw, ah);
		if(!anim)
			tex_gfx_clear(&texture_anim); // optional asset: same as load_file
		else if(!tex_gfx_load(&texture_anim, anim, aw, ah, TEX_FMT_RGBA32,
							  false))
			loaded_ok = false;
		tex_cache_note_slot(&texture_anim);
	}

	size_t pw = 0, ph = 0;
	void* pout = tex_atlas_particles("particles.png", &pw, &ph);
	if(pout) {
		struct tex_atlas_grid grid;
		bool grid_ok = tex_atlas_grid_init(&grid, pw, ph);
		assert(grid_ok);
		if(!grid_ok) {
			free(pout);
			pout = NULL;
		}
		if(pout
		   && !tex_gfx_load_atlas(&texture_particles, pout, pw, ph,
							  TEX_FMT_RGBA16, &grid))
			loaded_ok = false;
	}
	if(!pout) {
		tex_gfx_clear(&texture_particles);
		loaded_ok = false;
	}
	tex_cache_note_slot(&texture_particles);

	TEX_LOAD_FILE(&texture_gui_inventory, "gui/inventory.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_gui_crafting, "gui/crafting.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_gui_furnace, "gui/furnace.png", TEX_FMT_RGBA16,
				  false);
	// A raw jar names the chest sheet gui/container.png (the Pack Helper's
	// source_names for gui/chest.png); same 1x pass-through as the helper.
	{
		bool chest_done = false;
		if(raw_jar) {
			size_t cw = 0, ch = 0;
			void* img = tex_read_gfx("gui/container.png", &cw, &ch,
									 TEX_FMT_RGBA16);
			if(img) {
				if(!tex_gfx_load(&texture_gui_chest, img, cw, ch,
								 TEX_FMT_RGBA16, false))
					loaded_ok = false;
				tex_cache_note_slot(&texture_gui_chest);
				chest_done = true;
			}
		}
		if(!chest_done)
			TEX_LOAD_FILE(&texture_gui_chest, "gui/chest.png", TEX_FMT_RGBA16,
						  false);
	}
	// Beta GuiDispenser's background sheet [decompile 740c583:
	// GuiDispenser.drawGuiContainerBackgroundLayer -- "/gui/trap.png", the
	// same "Trap" naming holdover as the TileEntity NBT id]. NOT YET present
	// in this repo's default (Isabella-II-derived) asset pack -- neither
	// assets/gui/ nor a supplied texture pack currently ships it, so this
	// loads as the documented-optional NULL/cleared case (tex_gfx_load_file
	// above) and the dispenser GUI's background renders blank until the
	// asset is supplied (the project's tmp/beta_pack workflow, or a future
	// art-import pass) -- functionally complete (slots/clicks/items all
	// work), cosmetically incomplete. Flagged, not silent.
	TEX_LOAD_FILE(&texture_gui_dispenser, "gui/trap.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_gui_iron_chest, "gui/iron_chest.png", TEX_FMT_RGBA16,
				  false);
	// A raw jar has no gui_2.png: compose the helper's HUD sheet (bundled
	// gui_2.png + the pack's gui.png/icons.png regions) rather than show the
	// Earth & Ash hotbar. Any failure is logged and keeps the bundled sheet.
	{
		uint8_t* gui2 = NULL;
		if(raw_jar && !tex_raw_jar_pack_has("gui_2.png")
		   && tex_raw_jar_pack_has("gui/gui.png")
		   && tex_raw_jar_pack_has("gui/icons.png"))
			gui2 = tex_raw_jar_compose(false);
		if(gui2) {
			if(!tex_gfx_load(&texture_gui2, gui2, TEX_BETA_CANON,
							 TEX_BETA_CANON, TEX_FMT_RGBA16, false))
				loaded_ok = false;
			tex_cache_note_slot(&texture_gui2);
		} else {
			TEX_LOAD_FILE(&texture_gui2, "gui_2.png", TEX_FMT_RGBA16, false);
		}
	}
	// Beta's standard HUD-icon sheet (hearts, air bubbles, ...). The repo ships a
	// fCavEX-art default at gui/icons.png; a real Beta pack's gui/icons.png drops
	// in and the hearts/bubbles become authentically Beta with no code change.
	TEX_LOAD_FILE(&texture_icons, "gui/icons.png", TEX_FMT_RGBA16, false);
	// Beta's hotbar/widgets sheet (real gui/gui.png); fCavEX-art default shipped
	// at assets/gui/gui.png so a fresh checkout still renders a hotbar.
	TEX_LOAD_FILE(&texture_gui_widgets, "gui/gui.png", TEX_FMT_RGBA16, false);
	// Items: Beta keeps the item atlas at gui/items.png (not items.png) -- this is
	// what makes flint & steel etc. refresh with a Beta pack instead of staying
	// fCavEX.
	TEX_LOAD_FILE(&texture_items, "gui/items.png", TEX_FMT_RGBA16, false);
	// RenderArrow uses the separate Beta sheet, not the GUI item icon.
	TEX_LOAD_FILE(&texture_arrow, "item/arrows.png", TEX_FMT_RGBA16, false);
	// Entities: Beta puts the minecart at item/cart.png and mobs under mob/.
	TEX_LOAD_FILE(&texture_minecart, "item/cart.png", TEX_FMT_RGBA16, false);
	// Painting motive atlas [740c583: RenderPainting.func_158_a
	// `loadTexture("/art/kz.png")`]. The REAL Beta kz.png is Mojang IP and is
	// never committed here; the bundled default pack instead ships an
	// ORIGINAL CC0 replacement at assets/art/kz.png (tools/gen_kz_
	// placeholder.py -- see assets/README.md), tiled at every EnumArt
	// offset/size so paintings render with real, distinguishable pixels out
	// of the box. The pack-first lookup above still means a user's own real
	// Beta texture pack's art/kz.png wins automatically. entity_painting_
	// render still checks texture_painting.data itself (a corrupt/missing
	// bundled file is still a documented, graceful miss like gui/trap.png).
	TEX_LOAD_FILE(&texture_painting, "art/kz.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_creeper, "mob/creeper.png", TEX_FMT_RGBA16, false);
	// Beta zombie skin. The bundled fCavEX pack has no mob/zombie.png, so this
	// stays unloaded (texture_zombie.data == NULL) and render_entity_zombie falls
	// back to the creeper texture; a real Beta 1.7.3 pack supplies it.
	TEX_LOAD_FILE(&texture_zombie, "mob/zombie.png", TEX_FMT_RGBA16, false);
	// Beta skeleton + spider skins (mob/skeleton.png, mob/spider.png; both 64x32
	// in the beta-1.7.3 pack). If a pack omits them the renderers fall back to the
	// creeper skin so the model still draws.
	TEX_LOAD_FILE(&texture_skeleton, "mob/skeleton.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_spider, "mob/spider.png", TEX_FMT_RGBA16, false);
	// Emissive eye overlay (transparent except the eye pixels). RGBA16 keeps the
	// 1-bit alpha the alpha-test cutout needs. Absent in the bundled fcavex pack ->
	// .data stays NULL and the glow pass is skipped; a real Beta pack supplies it.
	TEX_LOAD_FILE(&texture_spider_eyes, "mob/spider_eyes.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_pig, "mob/pig.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_wolf, "mob/wolf.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_wolf_tame, "mob/wolf_tame.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_wolf_angry, "mob/wolf_angry.png", TEX_FMT_RGBA16,
				  false);
	// Beta cow + chicken skins (mob/cow.png, mob/chicken.png; 64x32). Absent from the
	// bundled legal pack (only char/creeper/pig ship), so these stay unloaded (.data==NULL)
	// and render_entity_cow/chicken fall back to the creeper skin; a real Beta pack supplies them.
	TEX_LOAD_FILE(&texture_cow, "mob/cow.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_chicken, "mob/chicken.png", TEX_FMT_RGBA16, false);
	// Beta sheep: the bare body skin (mob/sheep.png) + the grayscale wool overlay
	// (mob/sheep_fur.png, tinted by the fleece colour). Both 64x32. Absent -> creeper fallback.
	TEX_LOAD_FILE(&texture_sheep, "mob/sheep.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_sheep_fur, "mob/sheep_fur.png", TEX_FMT_RGBA16,
				  false);
	// Beta squid (64x32, ships in the bundled pack) + slime (64x32, ABSENT until
	// the legal pack lands -- NULL .data falls back to the creeper skin; do NOT
	// commit the copyrighted Beta slime.png). RGBA16's alpha carries the gel
	// translucency once a real pack overlays mob/slime.png.
	TEX_LOAD_FILE(&texture_squid, "mob/squid.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_slime, "mob/slime.png", TEX_FMT_RGBA16, false);
	// Nether natives (batch 1b): mob/pigzombie.png (64x32 biped) + the ghast
	// pair (64x32; ghast_fire.png swaps in while the attack counter > 10).
	// Absent -> creeper fallback (pigzombie/ghast) or the base ghast skin
	// (ghast_fire), so a partial pack still renders.
	TEX_LOAD_FILE(&texture_pigzombie, "mob/pigzombie.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_ghast, "mob/ghast.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_ghast_fire, "mob/ghast_fire.png", TEX_FMT_RGBA16,
				  false);


	TEX_LOAD_FILE(&texture_controls, "controls.png", TEX_FMT_RGBA16, false);
	// The scenic front-end backdrop: the same pack-first/bundled-fallback
	// resolution and the same reload lifecycle as every entry above, but
	// deliberately NOT through TEX_LOAD_FILE, because that macro's call sites
	// are also the PACK REQUIREMENT MANIFEST -- artwork/cc0-pack's
	// verify_full_pack.py builds `required` by scanning them, so a
	// TEX_LOAD_FILE here declares that every complete standalone pack must
	// ship a backdrop. It must not: the backdrop is OPTIONAL, a pack that
	// omits it falls back to the bundled assets/gui/menu-background.png (and
	// with neither, fep_backdrop draws nothing and the screen takes the
	// tiled-dirt menu background). A pack that DOES ship one still overrides
	// the bundled file through the same resolution, with no folder-theme
	// detection anywhere.
	// A genuinely FAILED load still fails loudly: tex_gfx_load_file returns
	// true only for the absent-asset case; an image that is present but whose
	// GPU upload/alloc is refused returns false and clears loaded_ok, exactly
	// as the macro would.
	/* BG-BAKE-1028/3: author art, else this pack's verified generated
	 * backdrop, else NONE (pack dirt) -- never the bundled scenery for a
	 * non-default pack. See tex_menu_background_load below. */
	if(!tex_menu_background_load())
		loaded_ok = false;
	tex_cache_note_slot(&texture_menu_background);
	if(!tex_gfx_load_file(&texture_title_logo, "gui/title-logo.png",
						  TEX_FMT_RGBA32, false))
		loaded_ok = false;
	tex_cache_note_slot(&texture_title_logo);
	if(!tex_gfx_load_file(&texture_menu_reception, "gui/reception.png",
						  TEX_FMT_RGBA16, false))
		loaded_ok = false;
	tex_cache_note_slot(&texture_menu_reception);
	TEX_LOAD_FILE(&texture_approved_controls, "gui/approved-controls.png",
		TEX_FMT_RGBA32, false);
	TEX_LOAD_FILE(&texture_controller_pictures, "gui/controller-pictures.png",
		TEX_FMT_RGBA32, false);
	TEX_LOAD_FILE(&texture_pointer, "pointer.png", TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_clouds, "environment/clouds.png", TEX_FMT_IA4, false);
	// render-slice (RENDER_DISPATCH.md): the partial-pack trap's THIRD
	// occurrence -- environment/ only ever shipped clouds.png (see
	// make_beta_pack.ps1's now-fixed $map). rain.png is genuinely
	// blue-tinted (RGBA16, verified non-grayscale against the owned jar);
	// snow.png is exhaustively grayscale (IA4, matches clouds.png's own
	// format choice, lossless). Both need vertical WRAP: renderRainSnow's V
	// coordinate (column Y / 4 + drift) routinely exceeds [0,1] for any
	// column more than 4 blocks tall.
	TEX_LOAD_FILE(&texture_rain, "environment/rain.png", TEX_FMT_RGBA16,
				  false);
	if(texture_rain.data)
		tex_gfx_wrap_mode(&texture_rain, true);
	TEX_LOAD_FILE(&texture_snow, "environment/snow.png", TEX_FMT_IA4, false);
	if(texture_snow.data)
		tex_gfx_wrap_mode(&texture_snow, true);
	TEX_LOAD_FILE(&texture_sun, "terrain/sun.png", TEX_FMT_RGB16, false);
	TEX_LOAD_FILE(&texture_moon, "terrain/moon.png", TEX_FMT_RGB16, false);
	TEX_LOAD_FILE(&texture_shadow, "misc/shadow.png", TEX_FMT_IA4, false);
	/* Held-map render slice (2026-08-26): Beta binds /misc/mapbg.png for the
	 * in-hand map frame [740c583 ItemRenderer.renderItemInFirstPerson].
	 * Optional like the other misc/ entries (absent = frame renders blank,
	 * the texture_gui_dispenser precedent). */
	TEX_LOAD_FILE(&texture_map_bg, "misc/mapbg.png", TEX_FMT_RGBA16, false);
	/* MapItemRenderer.java:78,86-101: 4x4 sheet of 8x8 icons. */
	TEX_LOAD_FILE(&texture_map_icons, "misc/mapicons.png", TEX_FMT_RGBA16, false);
	if(!texture_map_icons.data)
		fprintf(stderr, "[map] misc/mapicons.png absent; player icons unavailable\n");
	tex_map_live_init();
	TEX_LOAD_FILE(&texture_water, "misc/water.png", TEX_FMT_RGBA16, false);
	if(texture_water.data)
		tex_gfx_wrap_mode(&texture_water, true);
	// TextureWatchFX's dial ring [740c583: `ImageIO.read(.../misc/dial.png)`].
	// Optional/Mojang-IP like texture_painting's art/kz.png -- see texture.h.
	TEX_LOAD_FILE(&texture_dial, "misc/dial.png", TEX_FMT_RGBA16, false);

	TEX_LOAD_FILE(&texture_armor_chain1, "armor/chain_1.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_chain2, "armor/chain_2.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_cloth1, "armor/cloth_1.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_cloth2, "armor/cloth_2.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_gold1, "armor/gold_1.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_gold2, "armor/gold_2.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_iron1, "armor/iron_1.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_iron2, "armor/iron_2.png", TEX_FMT_RGBA16,
				  false);
	TEX_LOAD_FILE(&texture_armor_diamond1, "armor/diamond_1.png",
				  TEX_FMT_RGBA16, false);
	TEX_LOAD_FILE(&texture_armor_diamond2, "armor/diamond_2.png",
				  TEX_FMT_RGBA16, false);

	TEX_LOAD_FILE(&texture_mob_char, "mob/char.png", TEX_FMT_RGBA16, false);
#undef TEX_LOAD_FILE
	return loaded_ok;
}

/* The LIVE 128x128 map surface, created once from a zeroed image and
 * re-filled through tex_gfx_subimage whenever the client's held-map
 * mirror version changes (render_item_map). Format is per-platform by
 * tex_gfx_subimage's own hard contract: PC updates assume an RGBA32
 * CPU mirror; Wii updates require TEX_FMT_RGBA16 (RGB5A3) -- the same
 * quantization deviation the terrain atlas already carries, documented
 * with the slice. */
static void tex_map_live_init(void) {
	/* OWNERSHIP: tex_gfx_load takes `zero` on every path -- the Wii
	 * impl frees it after converting to RGB5A3, the PC impl keeps it
	 * as the tex->data CPU mirror. The original slice freed it here
	 * too: on Wii that was a double-free on the newlib heap in the
	 * middle of gfx_setup (BREAKER #2 -- the 2026-08-26 held-map
	 * boot-death, root-caused 2026-08-27 by remote content bisect:
	 * this init block alone reproduced the no-telemetry death), and
	 * on PC it left tex->data dangling for every later
	 * tex_gfx_subimage mirror write. Do NOT free zero here. */
	uint8_t* zero = calloc(1, 128 * 128 * 4);
	if(zero) {
#ifdef PLATFORM_WII
		tex_gfx_load(&texture_map_live, zero, 128, 128, TEX_FMT_RGBA16,
					 false);
#else
		tex_gfx_load(&texture_map_live, zero, 128, 128, TEX_FMT_RGBA32,
					 false);
#endif
	}
}

/* pack-cache-0927: what the fresh load does between its load sites that is
   not a texture, replayed after a cache restore: the font metrics and the
   three vertical-wrap textures (both depend only on restored slots), and
   the generated live map surface. */
static void tex_after_restore(void) {
	if(texture_font.data && texture_font.width >= 16 && texture_font.height >= 16)
		gutil_reset_font(&texture_font);
	if(texture_rain.data)
		tex_gfx_wrap_mode(&texture_rain, true);
	if(texture_snow.data)
		tex_gfx_wrap_mode(&texture_snow, true);
	tex_map_live_init();
	if(texture_water.data)
		tex_gfx_wrap_mode(&texture_water, true);
}

bool tex_init() {
	if(s_servicing) return false;
	s_ready = TEX_LOADING;
	s_failure = "";
	tex_load_service();
	tex_init_pre();
	bool loaded_ok = true;
	/* pack-cache-0927: OFF (the default) = tex_cache_try_restore returns
	   false without touching anything and tex_cache_after_load returns at
	   once -- the fresh load below is the whole of it, as before. */
	if(tex_cache_try_restore()) {
		tex_after_restore();
	} else {
		loaded_ok = tex_load_pack();
		tex_cache_after_load(loaded_ok);
	}

	size_t fog_size = 128;
	uint8_t* fog = malloc(fog_size * fog_size * 4);
	if(fog) { // skip the fog texture on OOM rather than gen_texture_fog() writing through NULL
		gen_texture_fog(fog, fog_size);
		if(!tex_gfx_load(
			   &texture_fog, fog, fog_size, fog_size, TEX_FMT_I8, true))
			loaded_ok = false;
	} else
		loaded_ok = false;

	// Beta TextureWaterFX: prime the animated water tile now that the block atlas
	// (and the captured water slot) exist. Re-runs on pack reload (tex_init re-entry).
	//
	// Gated to match its tick partner (main.c: `#ifndef GFX_FANCY_LIQUIDS
	// water_fx_anim_tick();`). Under fancy liquids -- which the shipping Wii
	// build defines -- water renders from anim.png, so this terrain-atlas tile is
	// never sampled: priming it was pure dead work AND it left that atlas cell's
	// LOD0 out of sync with LOD1-3 of the mip chain (the subimage writes LOD0
	// only). The tick half was gated in f3e3911; the init half was missed.
#ifndef GFX_FANCY_LIQUIDS
	water_fx_anim_init();
#endif

	// Beta TextureCompassFX/TextureWatchFX: capture the static compass/clock icon
	// cells (and the optional dial ring) now that texture_items/texture_dial exist.
	// Unconditional (unlike the liquids gate above -- items.png always samples this
	// atlas, GFX_FANCY_LIQUIDS or not); the per-tick update itself stays behind the
	// "gfx.item_needle_anim" config gate (main.c), so capturing here costs nothing
	// when the toggle is off. Re-runs on pack reload like every capture above.
	item_needle_fx_init();

	// Some backends keep fixed texture-map bindings outside normal draw calls.
	// Reload those only after every replacement object is fully initialized.
	if(texture_fog.data)
		tex_init_post();

	// These are not optional: terrain and fog are persistent GX bindings, while
	// font and clouds are read on the CPU and require non-zero dimensions.
	loaded_ok = loaded_ok && texture_terrain.data && texture_font.data
		&& texture_font.width >= 16
		&& texture_font.height >= 16 && texture_clouds.data && texture_fog.data;
	tex_load_service();
	s_ready = loaded_ok ? TEX_READY : TEX_FAILED;
	if(!loaded_ok && !s_failure[0])
		s_failure = "Required texture missing, invalid or upload failed.";
	return loaded_ok;
}

// --- Beta 1.7.3 TextureWaterFX animated still-water (graphics/water_fx.*) ---
// One ripple-field sim feeding the captured terrain-atlas water slot in place each
// 20Hz client tick. Render-layer only; NO allocation on the tick path (the upload
// writes into the already-sized atlas buffer), so it can't touch the MEM2 free-list.
static struct water_fx s_water_fx;
static bool s_water_fx_ready = false;

void water_fx_anim_init(void) {
	// Fixed seed: water FX is render-layer / non-parity, so a stable seed just keeps
	// boots reproducible. Drive it only when the live slot was captured AND the atlas
	// tile is the 16x16 the FX produces (HD packs: skip -> water keeps its static tile).
	water_fx_init(&s_water_fx, 0x9E3779B9u);
	s_water_fx_ready = (g_water_atlas_x >= 0 && g_water_atlas_y >= 0
						&& g_water_atlas_tile == WATER_FX_DIM && texture_terrain.data);
	if(s_water_fx_ready) {
		water_fx_tick(&s_water_fx); // prime so the first frame isn't the flat tile
		tex_gfx_subimage(&texture_terrain, g_water_atlas_x, g_water_atlas_y,
						 WATER_FX_DIM, WATER_FX_DIM, water_fx_pixels(&s_water_fx));
	}
}

void water_fx_anim_tick(void) {
	if(!s_water_fx_ready)
		return;
	water_fx_tick(&s_water_fx);
	tex_gfx_subimage(&texture_terrain, g_water_atlas_x, g_water_atlas_y, WATER_FX_DIM,
					 WATER_FX_DIM, water_fx_pixels(&s_water_fx));
}

// --- Beta 1.7.3 TextureCompassFX / TextureWatchFX item-needle FX -----------
// (graphics/compass_fx.*, graphics/watch_fx.*). Two 16x16 sub-uploads into the
// already-resident item atlas, once per 20Hz client tick, gated end-to-end by
// the "gfx.item_needle_anim" config toggle (T2: opt-in, default OFF on Wii /
// ON on PC -- see items_object.h and CLAUDE.md's T2 discipline; this has not
// been measured on real Wii hardware, so it stays off there until it is).
//
// items.png is loaded as a PLAIN (non-atlas) 256x256, 16px-cell texture (the
// TEX_LOAD_FILE call above, no tex_gfx_load_atlas margin/grid remap) --
// render_item_flat already reads it the same direct way (`s = texture_x*16;
// t = texture_y*16;`), so the compass/clock cell origins below are exactly
// Beta's iconCoord * 16 [740c583: Item.java `compass.setIconCoord(6,3)`,
// `pocketSundial.setIconCoord(6,4)`; also cited at item_types.h's ITEM_COMPASS/
// ITEM_CLOCK]. A non-vanilla (HD, non-16px-grid) items.png cannot be safely
// indexed this way, so init() below refuses to arm unless the atlas is exactly
// the vanilla 256x256 layout -- the same "only the FX's own tile size" guard
// water_fx_anim_init applies to the terrain atlas.
#define ITEM_NEEDLE_ATLAS_DIM 256 // vanilla items.png: 16x16 cells of 16px
#define ITEM_NEEDLE_COMPASS_TX 6
#define ITEM_NEEDLE_COMPASS_TY 3
#define ITEM_NEEDLE_CLOCK_TX 6
#define ITEM_NEEDLE_CLOCK_TY 4

static struct compass_fx_state s_compass_fx_state;
static struct watch_fx_state s_watch_fx_state;
static uint8_t s_compass_fx_base[COMPASS_FX_BYTES];
static uint8_t s_watch_fx_base[WATCH_FX_BYTES];
static uint8_t s_watch_fx_dial[WATCH_FX_BYTES];
static bool s_item_needle_ready = false;      // compass+clock cells captured
static bool s_item_needle_dial_ready = false; // + a real dial.png loaded

static void item_needle_capture_cell(struct tex_gfx* tex, int cell_x,
									 int cell_y, uint8_t* out /* *_BYTES */) {
	for(int y = 0; y < 16; y++) {
		for(int x = 0; x < 16; x++) {
			tex_gfx_lookup(tex, cell_x * 16 + x, cell_y * 16 + y,
						  out + (y * 16 + x) * 4);
		}
	}
}

void item_needle_fx_init(void) {
	s_item_needle_ready = false;
	s_item_needle_dial_ready = false;

	if(!texture_items.data || texture_items.width != ITEM_NEEDLE_ATLAS_DIM
	   || texture_items.height != ITEM_NEEDLE_ATLAS_DIM)
		return; // non-vanilla atlas layout: keep the static icons, no crash.

	compass_fx_state_init(&s_compass_fx_state);
	watch_fx_state_init(&s_watch_fx_state);
	item_needle_capture_cell(&texture_items, ITEM_NEEDLE_COMPASS_TX,
							 ITEM_NEEDLE_COMPASS_TY, s_compass_fx_base);
	item_needle_capture_cell(&texture_items, ITEM_NEEDLE_CLOCK_TX,
							 ITEM_NEEDLE_CLOCK_TY, s_watch_fx_base);
	s_item_needle_ready = true;

	if(texture_dial.data && texture_dial.width == WATCH_FX_DIM
	   && texture_dial.height == WATCH_FX_DIM) {
		item_needle_capture_cell(&texture_dial, 0, 0, s_watch_fx_dial);
		s_item_needle_dial_ready = true;
	}
}

void item_needle_fx_tick(bool enabled) {
	if(!enabled || !s_item_needle_ready)
		return;

	// has_target mirrors Beta's `mc.theWorld != null && mc.thePlayer != null`;
	// game_state_client_entities_active is this engine's equivalent "world is
	// actually loaded and playable" gate (game_state.h).
	bool has_target
		= game_state_client_entities_active(&gstate) && gstate.players[0].local_player;
	bool is_nether = gstate.world.dimension == WORLD_DIM_NETHER;
	// Caller-drawn RNG for the (rare, cosmetic-only) nether spin branch -- see
	// compass_fx.h/watch_fx.h's two-RNG-rule header comment. rand() is already
	// used elsewhere in this codebase for non-parity client-only cosmetics.
	double nether_rand01
		= is_nether ? (double)rand() / ((double)RAND_MAX + 1.0) : 0.0;

	double spawn_x = 0.0, spawn_z = 0.0, player_x = 0.0, player_z = 0.0;
	double player_yaw_rad = 0.0;
	if(has_target) {
		spawn_x = (double)gstate.world_spawn_x;
		spawn_z = (double)gstate.world_spawn_z;
		player_x = (double)gstate.players[0].local_player->pos[0];
		player_z = (double)gstate.players[0].local_player->pos[2];
		// [740c583: EntityPlayer.rotationYaw]. gstate.players[0].local_player->orient[0] is
		// ALREADY rotationYaw*PI/180 in Beta's own atan2(dz,dx) convention (see
		// compass_fx.h's header comment + entity_boat.c's turn-toward-motion
		// citation) -- no extra scale/offset needed here.
		player_yaw_rad = (double)gstate.players[0].local_player->orient[0];
	}

	uint8_t compass_frame[COMPASS_FX_BYTES];
	compass_fx_tick(&s_compass_fx_state, s_compass_fx_base, has_target,
					is_nether, nether_rand01, spawn_x, spawn_z, player_x,
					player_z, player_yaw_rad, compass_frame);
	tex_gfx_subimage(&texture_items, ITEM_NEEDLE_COMPASS_TX * 16,
					 ITEM_NEEDLE_COMPASS_TY * 16, COMPASS_FX_DIM,
					 COMPASS_FX_DIM, compass_frame);

	// Clock: skip entirely (no sim, no upload) until a real dial.png exists --
	// there is nothing to redraw over the static icon otherwise (watch_fx_tick
	// would just re-upload an unchanged copy every tick for free, which is the
	// "bounded, only when it can change anything" discipline this FX is gated
	// on in the first place).
	if(s_item_needle_dial_ready) {
		// [740c583: World.getCelestialAngle(1.0F)] -- the same wrapped day
		// fraction main.c already derives for the sky renderer, via the
		// shared daytime_celestial_angle() helper (daytime.h/.c).
		double celestial_angle = 0.0;
		if(has_target) {
			float raw_ticks = daytime_get_time();
			float wrapped = fmodf(raw_ticks, (float)DAY_LENGTH_TICKS);
			if(wrapped < 0.0F)
				wrapped += (float)DAY_LENGTH_TICKS;
			celestial_angle = (double)daytime_celestial_angle(
				wrapped / (float)DAY_LENGTH_TICKS);
		}

		uint8_t clock_frame[WATCH_FX_BYTES];
		watch_fx_tick(&s_watch_fx_state, s_watch_fx_base, s_watch_fx_dial,
					 has_target, is_nether, nether_rand01, celestial_angle,
					 clock_frame);
		tex_gfx_subimage(&texture_items, ITEM_NEEDLE_CLOCK_TX * 16,
						 ITEM_NEEDLE_CLOCK_TY * 16, WATCH_FX_DIM, WATCH_FX_DIM,
						 clock_frame);
	}
}

// Default (bundled) texture pack. A selected pack only needs to override the
// files it ships; everything it omits falls back to here.
#define TEX_DEFAULT_PACK "assets"

/* pack-cache-0927: NULL except while a cache-on miss is being recorded. */
static tex_read_observer_fn s_read_observer;

void tex_set_read_observer(tex_read_observer_fn fn) {
	s_read_observer = fn;
}

const char* tex_root_for_tag(int root_tag) {
	if(root_tag == 0)
		return config_read_path(&gstate.config_user, "paths.texturepack",
								TEX_DEFAULT_PACK);
#ifdef PLATFORM_WII
	if(root_tag == 2)
		return "sd:/assets";
#endif
	return TEX_DEFAULT_PACK;
}

enum tex_read_kind {
	TEX_READ_ANY,
	TEX_READ_GFX,
	TEX_READ_ATLAS,
};

static bool tex_source_dimensions_valid(enum tex_read_kind kind,
										unsigned width, unsigned height,
										enum tex_format type) {
	if(!tex_image_dimensions_valid(width, height))
		return false;
	if(kind == TEX_READ_ANY)
		return true;
	if(!tex_gfx_dimensions_valid(width, height, type))
		return false;
	return kind != TEX_READ_ATLAS
		|| (width == height && width >= 16 && (width % 16) == 0);
}

/* Original title branding is optional and bundled-only, like the shared art.
 * The three controller-art sheets: bundled-only (SD-root assets/ on Wii). They
 * stay on the loader list (the macro call sites) so every tool keeps an
 * explicit policy for them; the Pack Helper's conversion_map marks each one
 * "bundled_only", required false, so no pack is asked to ship or generate
 * them, and a pack copy is ignored here. */
/* The Wii hand is shared across packs; font/default.png remains pack-first.
 * The cache key includes this policy for every slot, invalidating old arrows. */
static const char* const tex_bundled_only_files[] = {
	"pointer.png",
	"gui/title-logo.png",
	"controls.png",
	"gui/approved-controls.png",
	"gui/controller-pictures.png",
};

bool tex_bundled_only(const char* filename) {
	for(size_t k = 0; filename && k < sizeof(tex_bundled_only_files)
			/ sizeof(tex_bundled_only_files[0]); k++)
		if(strcmp(filename, tex_bundled_only_files[k]) == 0)
			return true;
	return false;
}

/* ---- Raw-jar packs (tex_beta_compose.h). Every probe and read is reported
 * to the pack-cache observer so a cached load is invalidated when the player
 * adds, removes or edits any file the decision looked at. ---- */
static const char* tex_selected_pack(void) {
	return config_read_path(&gstate.config_user, "paths.texturepack",
							TEX_DEFAULT_PACK);
}

static bool tex_raw_jar_pack_has(const char* filename) {
	if(s_read_observer)
		s_read_observer(0, filename);
	return tex_beta_file_exists(tex_selected_pack(), filename);
}

static bool tex_raw_jar_pack(void) {
	const char* pack = tex_selected_pack();
	if(strcmp(pack, TEX_DEFAULT_PACK) == 0)
		return false;
	if(s_read_observer) {
		s_read_observer(0, "gui/container.png");
		s_read_observer(0, "gui/chest.png");
	}
	bool raw = tex_beta_raw_jar_layout(pack);
	if(raw)
		fprintf(stderr, "[texture] %s is an unconverted Beta jar layout: "
				"chest <- gui/container.png, gui_2.png/anim.png composed "
				"at load (Pack Helper transforms)\n", pack);
	return raw;
}

/* anim: beta-base anim.png + pack terrain.png ice; else gui_2: bundled
 * gui_2.png + pack gui/gui.png + gui/icons.png. Caller owns the 256x256
 * RGBA result; NULL (already logged) means use the bundled sheet. */
static uint8_t* tex_raw_jar_compose(bool anim) {
	const char* pack = tex_selected_pack();
	if(s_read_observer) {
		if(anim) {
			s_read_observer(0, "terrain.png");
			s_read_observer(1, TEX_BETA_BASE_ANIM);
		} else {
			s_read_observer(0, "gui/gui.png");
			s_read_observer(0, "gui/icons.png");
			s_read_observer(1, "gui_2.png");
		}
	}
	uint8_t* out;
	if(anim) {
		out = tex_beta_compose_anim(pack, TEX_DEFAULT_PACK "/" TEX_BETA_BASE_ANIM,
									tex_load_service);
	} else {
		out = tex_beta_compose_gui2(pack, TEX_DEFAULT_PACK, tex_load_service);
	}
	fprintf(stderr, "[texture] raw-jar %s %s\n", anim ? "anim.png" : "gui_2.png",
			out ? "composed" : "NOT composed; bundled sheet used");
	return out;
}

static uint8_t* tex_read_impl(const char* filename, size_t* width,
								   size_t* height, enum tex_read_kind kind,
								   enum tex_format type) {
	assert(filename && width && height);
	if(!filename || !width || !height)
		return NULL;
	*width = 0;
	*height = 0;

	const char* pack
		= config_read_path(&gstate.config_user, "paths.texturepack",
						   TEX_DEFAULT_PACK);
	/* Title branding and controller art are shared UI contracts: the button
	 * cells (old or themed sheets can omit families or change the fixed 32px
	 * cell layout) AND the approved controller pictures (a pack redrawing
	 * them broke "unchanged controller artwork across packs"; Astra
	 * pack-0926 #1). Always the bundled sheets; a pack copy is ignored. */
	bool shared_controls = tex_bundled_only(filename);
	if(shared_controls) {
#ifdef PLATFORM_WII
		/* Homebrew launchers need not start us in the SD root. */
		pack = "sd:/assets";
#else
		pack = TEX_DEFAULT_PACK;
#endif
	}

	const char* candidates[2] = {pack, TEX_DEFAULT_PACK};
	size_t count = shared_controls || strcmp(pack, TEX_DEFAULT_PACK) == 0 ? 1 : 2;
	for(size_t k = 0; k < count; k++) {
		tex_load_service();
		uint8_t* img = NULL;
		size_t w = 0, h = 0;
		if(s_read_observer)
			s_read_observer(k ? 1 : shared_controls ? 2 : count == 1 ? 1 : 0,
							filename);
		string_t tmp;
		string_init_printf(tmp, "%s/%s", candidates[k], filename);
		enum tex_png_decode_status decode_status;
		img = tex_png_decode32_bounded(
			string_get_cstr(tmp), 1024, 1024, &w, &h, &decode_status);
		tex_load_service();
		string_clear(tmp);
		if(img && tex_source_dimensions_valid(kind, w, h, type)) {
			*width = w;
			*height = h;
			return img;
		}
		if(img || decode_status == TEX_PNG_DECODE_FILE_TOO_LARGE
		   || decode_status == TEX_PNG_DECODE_INVALID_DIMENSIONS)
			fprintf(stderr,
					"[texture] rejecting %s/%s at %zux%zu (status %d); "
					"trying bundled fallback\n",
					candidates[k], filename, w, h, (int)decode_status);
		free(img);
	}
	return NULL;
}

uint8_t* tex_read(const char* filename, size_t* width, size_t* height) {
	return tex_read_impl(
		filename, width, height, TEX_READ_ANY, TEX_FMT_RGBA32);
}

uint8_t* tex_read_gfx(const char* filename, size_t* width, size_t* height,
					  enum tex_format type) {
	return tex_read_impl(filename, width, height, TEX_READ_GFX, type);
}

uint8_t* tex_read_atlas(const char* filename, size_t* width, size_t* height) {
	return tex_read_impl(
		filename, width, height, TEX_READ_ATLAS, TEX_FMT_RGBA16);
}

bool tex_gfx_load_file(struct tex_gfx* tex, const char* filename,
					   enum tex_format type, bool linear) {
	assert(filename);

	size_t width, height;
	void* img = tex_read_gfx(filename, &width, &height, type);

	if(!img) {
		/* Optional assets intentionally stay NULL when neither pack has them.
		 * Clear a prior pack's image instead of silently mixing packs. */
		tex_gfx_clear(tex);
		return true;
	}

	return tex_gfx_load(tex, img, width, height, type, linear);
}

/* The pack selector's per-row thumbnail. See texture.h for the contract; the
 * two things this file has to get exactly right are the ROOT and the BUFFER.
 *
 * ROOT: `pack_root` is used verbatim -- no config read, no TEX_DEFAULT_PACK
 * fallback chain. A row that names the bundled pack passes "assets" itself, so
 * a pack that ships neither file falls back to the caller's neutral tile
 * instead of quietly showing the DEFAULT pack's art under another pack's name.
 *
 * BUFFER: the decoded image is ours until we hand `small` to tex_gfx_load,
 * which OWNS it from that call on -- PC keeps it as tex->data, Wii converts it
 * and free()s it, and BOTH free it on every failure path. So `small` is never
 * touched after the call, on either outcome, and the only buffer we free is
 * the full-size decode. */
bool tex_preview_load(struct tex_gfx* tex, const char* pack_root) {
	assert(tex);
	if(!tex)
		return false;
	tex_gfx_clear(tex);
	if(!pack_root || !*pack_root)
		return false;

	static const char* const preview_names[2] = {"pack.png", "terrain.png"};
	for(size_t k = 0; k < 2; k++) {
		string_t path;
		string_init_printf(path, "%s/%s", pack_root, preview_names[k]);
		size_t w = 0, h = 0;
		enum tex_png_decode_status status;
		uint8_t* img = tex_png_decode32_bounded(string_get_cstr(path), 1024, 1024,
												&w, &h, &status);
		string_clear(path);
		if(!img)
			continue; /* absent or rejected: try the next name, then give up */
		if(!tex_image_dimensions_valid(w, h)) {
			free(img);
			continue;
		}
		/* WHAT we downsample, per the approved prototype (studio.css:36-37,
		 * pack-style.css:24): the card shows ONE BLOCK, not a mosaic.
		 *
		 * pack.png is the pack author's DELIBERATE cover art -- a whole picture
		 * composed for this slot -- so it is scaled in full. That is the native
		 * adaptation of the prototype's <img> thumbnail: the web mock had a
		 * single authored image per card, and a pack that ships one gets it
		 * shown as authored, overriding the atlas-cell rule below.
		 *
		 * terrain.png is the FALLBACK and is an atlas, never cover art. A
		 * whole-atlas thumbnail reads as noise, so only the TOP-LEFT cell (grass
		 * top in Beta 1.7.3) is sampled -- a 16x16 grid, hence w/16 x h/16. A
		 * sheet whose extent is not a multiple of 16 is not that grid; it is
		 * skipped rather than sliced at a wrong stride. No global/active atlas
		 * is consulted: the bytes come from THIS row's own file. */
		size_t src_w = w, src_h = h;
		if(strcmp(preview_names[k], "terrain.png") == 0) {
			if(w % 16 != 0 || h % 16 != 0) {
				free(img);
				continue;
			}
			src_w = w / 16;
			src_h = h / 16;
		}
		size_t dim = (size_t)TEX_PREVIEW_DIM;
		uint8_t* small = malloc(dim * dim * 4);
		if(!small) {
			free(img);
			return false;
		}
		/* NEAREST, not a box filter: block art is blocky on purpose, averaging
		 * only muddies it -- and this must stay cheap enough to run on a menu
		 * transition. Both indices stay strictly inside the sampled region
		 * (y < dim => y*src_h/dim < src_h <= h), and the region's origin is
		 * (0,0), so the row stride below is still the full image width. */
		for(size_t y = 0; y < dim; y++) {
			size_t sy = y * src_h / dim;
			for(size_t x = 0; x < dim; x++) {
				size_t sx = x * src_w / dim;
				memcpy(small + (y * dim + x) * 4, img + (sy * w + sx) * 4, 4);
			}
		}
		free(img);
		/* 40x40 satisfies GX's 4-aligned RGB5A3 tiles (tex_gfx_dimensions_valid);
		 * nonmipmap + GX_NEAR keeps the blocky pack look the card wants. */
		return tex_gfx_load(tex, small, dim, dim, TEX_FMT_RGBA16, false);
	}
	return false;
}

/* ---- BG-BAKE-1028 task 3: generated menu-backdrop cache ------------------
 *
 * Resolution of gui/menu-background.png (texture_menu_background):
 *   1. the bundled pack ("assets"): its own file, unchanged behaviour;
 *   2. any other pack that ships gui/menu-background.png: that AUTHOR art,
 *      verbatim -- never replaced, never written;
 *   3. otherwise this pack's GENERATED backdrop, only if its identity
 *      verifies (below);
 *   4. otherwise nothing: the screens draw the pack's own dirt (gutil_bg).
 *      The bundled scenery is never shown under another pack's name.
 *
 * The generated cache lives apart from author art, in
 *   <pack>/.cavewii/backdrop/menu-background.png   (the 1024x576 bake)
 *   <pack>/.cavewii/backdrop/menu-background.id    (its identity record)
 * The identity record marks the PNG as generated and binds it to
 *   recipe version (BACKDROP_BAKE_RECIPE_VERSION) + output size + an FNV-1a
 *   digest over the pack-resolved bytes of every file the bake's frame
 *   samples (TEX_BACKDROP_INPUTS) + the PNG's own size and digest.
 * A record that is missing, unparsable, for another recipe/pack content, or
 * whose PNG is absent/short/different is rejected (state STALE/CORRUPT/
 * MISSING), so an interrupted publication can never be shown after reboot.
 *
 * Publication (tex_backdrop_publish): encode in memory; write png.tmp and
 * id.tmp with every fwrite/fflush/fclose checked; then remove the old .id
 * (the cache is now invalid, never half-valid), replace the .png, and
 * rename the .id last -- the .id rename is the single commit point. Any
 * failure removes the temporaries and reports a reason; nothing is retried
 * here. */

#define TEX_BACKDROP_DIR ".cavewii/backdrop"
#define TEX_BACKDROP_PNG TEX_BACKDROP_DIR "/menu-background.png"
#define TEX_BACKDROP_ID TEX_BACKDROP_DIR "/menu-background.id"
#define TEX_BACKDROP_AUTHOR_FILE "gui/menu-background.png"
#define TEX_BACKDROP_ID_MAGIC "CWBACKDROP generated 1"

/* Every pack file the bake's frame samples: the terrain atlas, the liquid
 * animation strip, the biome colormaps, clouds, sun and moon. A byte change
 * in any of them (or in the bundled file it falls back to) changes the
 * identity and invalidates the cache. */
static const char* const tex_backdrop_inputs[] = {
	"terrain.png",
	"anim.png",
	"misc/grasscolor.png",
	"misc/foliagecolor.png",
	"environment/clouds.png",
	"terrain/sun.png",
	"terrain/moon.png",
};

enum tex_backdrop_state {
	TEX_BACKDROP_BUNDLED = 0, /* the bundled pack: its own shipped art */
	TEX_BACKDROP_AUTHOR,      /* pack ships gui/menu-background.png */
	TEX_BACKDROP_CACHED,      /* verified generated backdrop */
	TEX_BACKDROP_MISSING,     /* nothing generated yet */
	TEX_BACKDROP_STALE,       /* generated for another recipe/pack content */
	TEX_BACKDROP_CORRUPT,     /* record/PNG pair incomplete or mismatched */
};

enum tex_backdrop_publish_result {
	TEX_BACKDROP_PUBLISHED = 0,
	TEX_BACKDROP_ERR_AUTHOR,  /* refused: the pack ships its own art */
	TEX_BACKDROP_ERR_INPUT,   /* wrong size / no pixels */
	TEX_BACKDROP_ERR_ALLOC,   /* encode workspace */
	TEX_BACKDROP_ERR_DIR,     /* cannot create the cache folder */
	TEX_BACKDROP_ERR_OPEN,    /* cannot create a file (read-only card) */
	TEX_BACKDROP_ERR_WRITE,   /* short write / flush failure (card full) */
	TEX_BACKDROP_ERR_CLOSE,   /* close failed (data not committed) */
	TEX_BACKDROP_ERR_RENAME,  /* publish step failed */
};

/* Runtime-armed test seam: fail the n-th file operation of the given kind
 * (0 = off), or stop a publication after `stop_after` completed steps
 * (1 png.tmp, 2 id.tmp, 3 old id removed, 4 png in place; 0 = off) to model
 * a power cut at each boundary. Never armed by the engine. */
enum tex_backdrop_test_op {
	TEX_BACKDROP_OP_ALLOC,
	TEX_BACKDROP_OP_OPEN,
	TEX_BACKDROP_OP_WRITE,
	TEX_BACKDROP_OP_CLOSE,
	TEX_BACKDROP_OP_RENAME,
	TEX_BACKDROP_OP_COUNT,
};
static unsigned s_bd_fail_at[TEX_BACKDROP_OP_COUNT];
static unsigned s_bd_stop_after;
void tex_backdrop_test_fail(int op, unsigned nth) {
	if(op >= 0 && op < TEX_BACKDROP_OP_COUNT) s_bd_fail_at[op] = nth;
}
void tex_backdrop_test_stop_after(unsigned step) {
	s_bd_stop_after = step;
}
static bool bd_op_ok(enum tex_backdrop_test_op op) {
	if(!s_bd_fail_at[op]) return true;
	return --s_bd_fail_at[op] != 0;
}

static uint64_t bd_fnv(uint64_t h, const void* data, size_t n) {
	const uint8_t* p = data;
	for(size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= UINT64_C(0x100000001b3);
	}
	return h;
}
#define BD_FNV_INIT UINT64_C(0xcbf29ce484222325)

static uint64_t bd_fnv_u32(uint64_t h, uint32_t v) {
	uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
					(uint8_t)v};
	return bd_fnv(h, b, 4);
}

/* Size + digest of one file; false if it cannot be opened. */
static bool bd_hash_file(const char* path, uint32_t* size, uint64_t* digest) {
	FILE* f = fopen(path, "rb");
	if(!f) return false;
	static uint8_t buf[4096];
	uint64_t h = BD_FNV_INIT;
	uint32_t n = 0;
	size_t got;
	while((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		h = bd_fnv(h, buf, got);
		n += (uint32_t)got;
		tex_load_service();
	}
	bool ok = !ferror(f);
	fclose(f);
	*size = n;
	*digest = h;
	return ok;
}

static bool bd_is_default(const char* pack) {
	return !pack || !*pack || strcmp(pack, TEX_DEFAULT_PACK) == 0;
}

static void bd_path(char* out, size_t size, const char* pack, const char* rel) {
	snprintf(out, size, "%s/%s", pack, rel);
}

static bool bd_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f) return false;
	fclose(f);
	return true;
}

/* The pack's content identity for the bake: recipe + output size + every
 * input as the loader resolves it (pack copy first, else the bundled one). */
uint64_t tex_backdrop_identity(const char* pack) {
	uint64_t h = bd_fnv(BD_FNV_INIT, "CWBD", 4);
	h = bd_fnv_u32(h, BACKDROP_BAKE_RECIPE_VERSION);
	h = bd_fnv_u32(h, BACKDROP_BAKE_OUT_W);
	h = bd_fnv_u32(h, BACKDROP_BAKE_OUT_H);
	for(size_t k = 0; k < sizeof(tex_backdrop_inputs)
			/ sizeof(tex_backdrop_inputs[0]); k++) {
		const char* name = tex_backdrop_inputs[k];
		h = bd_fnv(h, name, strlen(name) + 1);
		const char* roots[2] = {pack, TEX_DEFAULT_PACK};
		uint32_t tag = 0xFFFFFFFFu, size = 0;
		uint64_t digest = 0;
		for(uint32_t r = 0; r < 2; r++) {
			char path[700];
			bd_path(path, sizeof(path), roots[r], name);
			if(bd_hash_file(path, &size, &digest)) {
				tag = r;
				break;
			}
		}
		h = bd_fnv_u32(h, tag);
		h = bd_fnv_u32(h, size);
		h = bd_fnv(h, &digest, sizeof(digest));
	}
	return h;
}

static bool bd_author_present(const char* pack) {
	char path[700];
	bd_path(path, sizeof(path), pack, TEX_BACKDROP_AUTHOR_FILE);
	return bd_exists(path);
}

/* Public entry points (texture.h) take/return int: enum tex_backdrop_state. */
int tex_backdrop_state_of(const char* pack) {
	if(bd_is_default(pack)) return TEX_BACKDROP_BUNDLED;
	if(bd_author_present(pack)) return TEX_BACKDROP_AUTHOR;
	char id_path[700], png_path[700];
	bd_path(id_path, sizeof(id_path), pack, TEX_BACKDROP_ID);
	bd_path(png_path, sizeof(png_path), pack, TEX_BACKDROP_PNG);
	FILE* f = fopen(id_path, "rb");
	if(!f)
		return bd_exists(png_path) ? TEX_BACKDROP_CORRUPT : TEX_BACKDROP_MISSING;
	char magic[64] = {0};
	unsigned recipe = 0, w = 0, h = 0;
	unsigned long png_size = 0;
	unsigned long long pack_id = 0, png_digest = 0;
	bool parsed = fgets(magic, sizeof(magic), f) != NULL
		&& fscanf(f, "recipe %u\nsize %ux%u\npack %16llx\npng %lu %16llx\n",
				  &recipe, &w, &h, &pack_id, &png_size, &png_digest) == 6;
	fclose(f);
	size_t ml = strlen(magic);
	while(ml && (magic[ml - 1] == '\n' || magic[ml - 1] == '\r')) magic[--ml] = 0;
	if(!parsed || strcmp(magic, TEX_BACKDROP_ID_MAGIC) != 0)
		return TEX_BACKDROP_CORRUPT;
	if(recipe != BACKDROP_BAKE_RECIPE_VERSION || w != BACKDROP_BAKE_OUT_W
	   || h != BACKDROP_BAKE_OUT_H
	   || (uint64_t)pack_id != tex_backdrop_identity(pack))
		return TEX_BACKDROP_STALE;
	uint32_t size = 0;
	uint64_t digest = 0;
	if(!bd_hash_file(png_path, &size, &digest) || size != png_size
	   || digest != (uint64_t)png_digest)
		return TEX_BACKDROP_CORRUPT;
	return TEX_BACKDROP_CACHED;
}

const char* tex_backdrop_publish_reason(int r) {
	switch((enum tex_backdrop_publish_result)r) {
		case TEX_BACKDROP_PUBLISHED: return "Background saved.";
		case TEX_BACKDROP_ERR_AUTHOR: return "This pack has its own background.";
		case TEX_BACKDROP_ERR_INPUT: return "Background picture was incomplete.";
		case TEX_BACKDROP_ERR_ALLOC: return "Not enough memory to save the background.";
		case TEX_BACKDROP_ERR_DIR:
		case TEX_BACKDROP_ERR_OPEN: return "SD card is locked or full.";
		case TEX_BACKDROP_ERR_WRITE:
		case TEX_BACKDROP_ERR_CLOSE: return "SD card is full or failed to write.";
		case TEX_BACKDROP_ERR_RENAME: return "SD card refused to save the background.";
	}
	return "Background could not be saved.";
}

static bool bd_mkdir(const char* path) {
#ifdef _WIN32
	int r = _mkdir(path);
#else
	int r = mkdir(path, 0777);
#endif
	if(r == 0) return true;
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static enum tex_backdrop_publish_result bd_write_file(const char* path,
	const void* data, size_t len) {
	if(!bd_op_ok(TEX_BACKDROP_OP_OPEN)) return TEX_BACKDROP_ERR_OPEN;
	FILE* f = fopen(path, "wb");
	if(!f) return TEX_BACKDROP_ERR_OPEN;
	bool ok = true;
	for(size_t off = 0; off < len && ok;) {
		size_t n = len - off;
		if(n > 16384) n = 16384;
		ok = bd_op_ok(TEX_BACKDROP_OP_WRITE)
			&& fwrite((const uint8_t*)data + off, 1, n, f) == n;
		off += n;
		tex_load_service();
	}
	ok = (fflush(f) == 0) && ok;
	tex_load_service();
	bool closed = bd_op_ok(TEX_BACKDROP_OP_CLOSE);
	closed = (fclose(f) == 0) && closed;
	tex_load_service();
	if(!ok) return TEX_BACKDROP_ERR_WRITE;
	if(!closed) return TEX_BACKDROP_ERR_CLOSE;
	return TEX_BACKDROP_PUBLISHED;
}

/* rename() does not replace an existing target on every libc we ship on
 * (MSVCRT); the target is removed first. The .id is removed BEFORE the .png
 * is replaced, so no window shows a record beside a foreign PNG. */
static bool bd_replace(const char* from, const char* to) {
	if(!bd_op_ok(TEX_BACKDROP_OP_RENAME)) return false;
	remove(to);
	return rename(from, to) == 0;
}

/* A serviced PNG uses lossless RGBA8 / filter 0 / stored DEFLATE blocks.
 * Each row is one IDAT chunk, so encoding + writing never needs a whole-image
 * workspace or an uninterrupted compressor call. Disk cost is ~2.37 MB at
 * the fixed recipe size (larger than the ordinary compressed cache). lodepng
 * still owns PNG chunk construction/CRC, and the decoder sees identical RGBA.
 * Scratch is one 4110-byte row plus one <=4122-byte chunk. No new dependency. */
static bool bd_png_part(FILE* f, const uint8_t* p, size_t n,
	uint32_t* size, uint64_t* digest) {
	bool ok = bd_op_ok(TEX_BACKDROP_OP_WRITE) && fwrite(p, 1, n, f) == n;
	if(ok) {
		*size += (uint32_t)n;
		*digest = bd_fnv(*digest, p, n);
	}
	tex_load_service();
	return ok;
}

static void bd_be32(uint8_t* p, uint32_t n) {
	p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
	p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}

static bool bd_png_chunk(FILE* f, const char* type, const uint8_t* p, size_t n,
	uint32_t* size, uint64_t* digest, bool* alloc_failed) {
	uint8_t* chunk = NULL;
	size_t len = 0;
	bool ok = lodepng_chunk_create(&chunk, &len, (unsigned)n, type, p) == 0;
	if(!ok) *alloc_failed = true;
	if(ok) ok = bd_png_part(f, chunk, len, size, digest);
	free(chunk);
	return ok;
}

static enum tex_backdrop_publish_result bd_write_png_serviced(const char* path,
	const uint8_t* rgba, unsigned width, unsigned height,
	uint32_t* size, uint64_t* digest) {
	if(!bd_op_ok(TEX_BACKDROP_OP_ALLOC)) return TEX_BACKDROP_ERR_ALLOC;
	uint8_t* row = malloc(4 * BACKDROP_BAKE_OUT_W + 14);
	if(!row) return TEX_BACKDROP_ERR_ALLOC;
	if(!bd_op_ok(TEX_BACKDROP_OP_OPEN)) { free(row); return TEX_BACKDROP_ERR_OPEN; }
	FILE* f = fopen(path, "wb");
	if(!f) { free(row); return TEX_BACKDROP_ERR_OPEN; }
	*size = 0;
	*digest = BD_FNV_INIT;
	static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	uint8_t ihdr[13] = {0};
	bd_be32(ihdr, width); bd_be32(ihdr + 4, height);
	ihdr[8] = 8; ihdr[9] = 6; /* RGBA8, noninterlaced */
	bool alloc_failed = false;
	bool ok = bd_png_part(f, sig, sizeof(sig), size, digest)
		&& bd_png_chunk(f, "IHDR", ihdr, sizeof(ihdr), size, digest, &alloc_failed);
	uint32_t a = 1, b = 0;
	unsigned raw_len = width * 4 + 1;
	for(unsigned y = 0; y < height && ok; y++) {
		size_t at = 0;
		if(y == 0) { row[at++] = 0x78; row[at++] = 0x01; }
		row[at++] = y + 1 == height ? 1 : 0; /* BFINAL, stored block */
		row[at++] = (uint8_t)raw_len;
		row[at++] = (uint8_t)(raw_len >> 8);
		row[at++] = (uint8_t)~raw_len;
		row[at++] = (uint8_t)(~raw_len >> 8);
		size_t raw = at;
		row[at++] = 0; /* filter: None */
		memcpy(row + at, rgba + (size_t)y * width * 4, width * 4);
		at += width * 4;
		for(size_t k = raw; k < at; k++) { a += row[k]; b += a; }
		a %= 65521; b %= 65521;
		if(y + 1 == height) { bd_be32(row + at, (b << 16) | a); at += 4; }
		ok = bd_png_chunk(f, "IDAT", row, at, size, digest, &alloc_failed);
	}
	if(ok) ok = bd_png_chunk(f, "IEND", NULL, 0, size, digest, &alloc_failed);
	ok = (fflush(f) == 0) && ok;
	tex_load_service();
	bool closed = bd_op_ok(TEX_BACKDROP_OP_CLOSE);
	closed = (fclose(f) == 0) && closed;
	tex_load_service();
	free(row);
	return alloc_failed ? TEX_BACKDROP_ERR_ALLOC : !ok ? TEX_BACKDROP_ERR_WRITE
		: !closed ? TEX_BACKDROP_ERR_CLOSE : TEX_BACKDROP_PUBLISHED;
}

int tex_backdrop_publish(const char* pack, const uint8_t* rgba,
	unsigned width, unsigned height) {
	if(bd_is_default(pack) || bd_author_present(pack))
		return TEX_BACKDROP_ERR_AUTHOR;
	if(!rgba || width != BACKDROP_BAKE_OUT_W || height != BACKDROP_BAKE_OUT_H)
		return TEX_BACKDROP_ERR_INPUT;
	char dir[700], png_path[700], id_path[700], png_tmp[720], id_tmp[720];
	bd_path(dir, sizeof(dir), pack, ".cavewii");
	if(!bd_mkdir(dir)) return TEX_BACKDROP_ERR_DIR;
	bd_path(dir, sizeof(dir), pack, TEX_BACKDROP_DIR);
	if(!bd_mkdir(dir)) return TEX_BACKDROP_ERR_DIR;
	bd_path(png_path, sizeof(png_path), pack, TEX_BACKDROP_PNG);
	bd_path(id_path, sizeof(id_path), pack, TEX_BACKDROP_ID);
	snprintf(png_tmp, sizeof(png_tmp), "%s.tmp", png_path);
	snprintf(id_tmp, sizeof(id_tmp), "%s.tmp", id_path);

	unsigned char* png = NULL;
	size_t png_len = 0;
	uint64_t digest = BD_FNV_INIT;
	enum tex_backdrop_publish_result r;
	if(s_load_service) {
		uint32_t bytes = 0;
		r = bd_write_png_serviced(png_tmp, rgba, width, height, &bytes, &digest);
		png_len = bytes;
	} else {
		if(!bd_op_ok(TEX_BACKDROP_OP_ALLOC)
		   || lodepng_encode32(&png, &png_len, rgba, width, height) != 0 || !png) {
			free(png);
			return TEX_BACKDROP_ERR_ALLOC;
		}
		digest = bd_fnv(BD_FNV_INIT, png, png_len);
		r = bd_write_file(png_tmp, png, png_len);
		free(png);
	}
	char record[256];
	int rn = snprintf(record, sizeof(record),
		"%s\nrecipe %u\nsize %ux%u\npack %016llx\npng %lu %016llx\n",
		TEX_BACKDROP_ID_MAGIC, (unsigned)BACKDROP_BAKE_RECIPE_VERSION,
		width, height, (unsigned long long)tex_backdrop_identity(pack),
		(unsigned long)png_len, (unsigned long long)digest);
	if(r == TEX_BACKDROP_PUBLISHED && s_bd_stop_after == 1) return r;
	if(r == TEX_BACKDROP_PUBLISHED)
		r = bd_write_file(id_tmp, record, (size_t)rn);
	if(r == TEX_BACKDROP_PUBLISHED && s_bd_stop_after == 2) return r;
	if(r != TEX_BACKDROP_PUBLISHED) {
		remove(png_tmp);
		remove(id_tmp);
		return r;
	}
	/* Commit sequence. */
	remove(id_path);
	if(s_bd_stop_after == 3) return r;
	if(!bd_replace(png_tmp, png_path)) {
		remove(png_tmp);
		remove(id_tmp);
		return TEX_BACKDROP_ERR_RENAME;
	}
	if(s_bd_stop_after == 4) return r;
	if(!bd_replace(id_tmp, id_path)) {
		remove(id_tmp);
		return TEX_BACKDROP_ERR_RENAME;
	}
	return TEX_BACKDROP_PUBLISHED;
}

/* Resolve and load texture_menu_background (see the top of this section).
 * Same contract as tex_gfx_load_file: true when the slot is loaded OR
 * deliberately empty, false only when present art failed to upload. */
bool tex_menu_background_load(void) {
	struct tex_gfx* tex = &texture_menu_background;
	const char* pack = config_read_path(&gstate.config_user,
		"paths.texturepack", TEX_DEFAULT_PACK);
	if(bd_is_default(pack))
		return tex_gfx_load_file(tex, TEX_BACKDROP_AUTHOR_FILE, TEX_FMT_RGBA16,
								 false);
	/* texture_cache dependency record: both candidate files of this pack. */
	if(s_read_observer) {
		s_read_observer(0, TEX_BACKDROP_AUTHOR_FILE);
		s_read_observer(0, TEX_BACKDROP_ID);
		s_read_observer(0, TEX_BACKDROP_PNG);
	}
	const char* rel = NULL;
	int st = tex_backdrop_state_of(pack);
	if(st == TEX_BACKDROP_AUTHOR) rel = TEX_BACKDROP_AUTHOR_FILE;
	else if(st == TEX_BACKDROP_CACHED) rel = TEX_BACKDROP_PNG;
	if(rel) {
		char path[700];
		bd_path(path, sizeof(path), pack, rel);
		size_t w = 0, h = 0;
		enum tex_png_decode_status status;
		uint8_t* img = tex_png_decode32_bounded(path, 1024, 1024, &w, &h,
												&status);
		if(img && tex_source_dimensions_valid(TEX_READ_GFX, w, h,
											  TEX_FMT_RGBA16))
			return tex_gfx_load(tex, img, w, h, TEX_FMT_RGBA16, false);
		fprintf(stderr, "[texture] rejecting %s (status %d); pack dirt shown\n",
				path, (int)status);
		free(img);
	}
	tex_gfx_clear(tex);
	return true;
}

#ifdef PLATFORM_WII
#include "wii/texture.c"
#endif

#ifdef PLATFORM_PC
#include "pc/texture.c"
#endif
