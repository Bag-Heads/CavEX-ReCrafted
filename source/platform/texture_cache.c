/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Pack cache, texture slice. Contract: texture_cache.h. */

#include "texture_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../game/game_state.h"
#include "../graphics/biome_color.h"
#include "../graphics/texture_atlas.h"
#include "mem2.h"
#include "pack_cache.h"
#include "pack_manifest.h"
#include "tex_gx_conv.h"
#include "texture.h"
#include "time.h"

#define TEX_MAGIC "CWTX"
#define HEADER_BYTES 96u
#define SLOT_REC_BYTES 16u
#define IO_CHUNK 4096u
#define DEP_NAME_MAX 256u
#define DEPS_MAX 512u
#define DEFAULT_PACK "assets"

/* ---- the slot table: every texture a pack load produces, in load order.
   It MUST list exactly the load sites tex_init runs (texture.c calls
   tex_cache_note_slot at each); a fresh load whose sites do not match this
   table, in order, is never published. texture_map_live and texture_fog are
   generated, not loaded, and stay outside the cache. ---- */
struct tex_slot {
	struct tex_gfx* tex;
	const char* file;
	enum tex_format fmt;
	bool linear;
	bool atlas;
};

#define S(t, f, fmt) {&t, f, fmt, false, false}
static const struct tex_slot s_slots[] = {
	{&texture_terrain, "terrain.png", TEX_FMT_RGBA16, true, true},
	S(texture_font, "font/default.png", TEX_FMT_I8),
	S(texture_anim, "anim.png", TEX_FMT_RGBA32),
	{&texture_particles, "particles.png", TEX_FMT_RGBA16, true, true},
	S(texture_gui_inventory, "gui/inventory.png", TEX_FMT_RGBA16),
	S(texture_gui_crafting, "gui/crafting.png", TEX_FMT_RGBA16),
	S(texture_gui_furnace, "gui/furnace.png", TEX_FMT_RGBA16),
	S(texture_gui_chest, "gui/chest.png", TEX_FMT_RGBA16),
	S(texture_gui_dispenser, "gui/trap.png", TEX_FMT_RGBA16),
	S(texture_gui_iron_chest, "gui/iron_chest.png", TEX_FMT_RGBA16),
	S(texture_gui2, "gui_2.png", TEX_FMT_RGBA16),
	S(texture_icons, "gui/icons.png", TEX_FMT_RGBA16),
	S(texture_gui_widgets, "gui/gui.png", TEX_FMT_RGBA16),
	S(texture_items, "gui/items.png", TEX_FMT_RGBA16),
	S(texture_arrow, "item/arrows.png", TEX_FMT_RGBA16),
	S(texture_minecart, "item/cart.png", TEX_FMT_RGBA16),
	S(texture_painting, "art/kz.png", TEX_FMT_RGBA16),
	S(texture_creeper, "mob/creeper.png", TEX_FMT_RGBA16),
	S(texture_zombie, "mob/zombie.png", TEX_FMT_RGBA16),
	S(texture_skeleton, "mob/skeleton.png", TEX_FMT_RGBA16),
	S(texture_spider, "mob/spider.png", TEX_FMT_RGBA16),
	S(texture_spider_eyes, "mob/spider_eyes.png", TEX_FMT_RGBA16),
	S(texture_pig, "mob/pig.png", TEX_FMT_RGBA16),
	S(texture_wolf, "mob/wolf.png", TEX_FMT_RGBA16),
	S(texture_wolf_tame, "mob/wolf_tame.png", TEX_FMT_RGBA16),
	S(texture_wolf_angry, "mob/wolf_angry.png", TEX_FMT_RGBA16),
	S(texture_cow, "mob/cow.png", TEX_FMT_RGBA16),
	S(texture_chicken, "mob/chicken.png", TEX_FMT_RGBA16),
	S(texture_sheep, "mob/sheep.png", TEX_FMT_RGBA16),
	S(texture_sheep_fur, "mob/sheep_fur.png", TEX_FMT_RGBA16),
	S(texture_squid, "mob/squid.png", TEX_FMT_RGBA16),
	S(texture_slime, "mob/slime.png", TEX_FMT_RGBA16),
	S(texture_pigzombie, "mob/pigzombie.png", TEX_FMT_RGBA16),
	S(texture_ghast, "mob/ghast.png", TEX_FMT_RGBA16),
	S(texture_ghast_fire, "mob/ghast_fire.png", TEX_FMT_RGBA16),
	S(texture_controls, "controls.png", TEX_FMT_RGBA16),
	S(texture_nether_background, "gui/nether-background.png", TEX_FMT_RGBA16),
	S(texture_menu_background, "gui/menu-background.png", TEX_FMT_RGBA16),
	S(texture_title_logo, "gui/title-logo.png", TEX_FMT_RGBA32),
	S(texture_menu_reception, "gui/reception.png", TEX_FMT_RGBA16),
	S(texture_approved_controls, "gui/approved-controls.png", TEX_FMT_RGBA32),
	S(texture_controller_pictures, "gui/controller-pictures.png",
	  TEX_FMT_RGBA32),
	S(texture_pointer, "pointer.png", TEX_FMT_RGBA16),
	S(texture_clouds, "environment/clouds.png", TEX_FMT_IA4),
	S(texture_rain, "environment/rain.png", TEX_FMT_RGBA16),
	S(texture_snow, "environment/snow.png", TEX_FMT_IA4),
	S(texture_sun, "terrain/sun.png", TEX_FMT_RGB16),
	S(texture_moon, "terrain/moon.png", TEX_FMT_RGB16),
	S(texture_shadow, "misc/shadow.png", TEX_FMT_IA4),
	S(texture_map_bg, "misc/mapbg.png", TEX_FMT_RGBA16),
	S(texture_map_icons, "misc/mapicons.png", TEX_FMT_RGBA16),
	S(texture_water, "misc/water.png", TEX_FMT_RGBA16),
	S(texture_dial, "misc/dial.png", TEX_FMT_RGBA16),
	S(texture_armor_chain1, "armor/chain_1.png", TEX_FMT_RGBA16),
	S(texture_armor_chain2, "armor/chain_2.png", TEX_FMT_RGBA16),
	S(texture_armor_cloth1, "armor/cloth_1.png", TEX_FMT_RGBA16),
	S(texture_armor_cloth2, "armor/cloth_2.png", TEX_FMT_RGBA16),
	S(texture_armor_gold1, "armor/gold_1.png", TEX_FMT_RGBA16),
	S(texture_armor_gold2, "armor/gold_2.png", TEX_FMT_RGBA16),
	S(texture_armor_iron1, "armor/iron_1.png", TEX_FMT_RGBA16),
	S(texture_armor_iron2, "armor/iron_2.png", TEX_FMT_RGBA16),
	S(texture_armor_diamond1, "armor/diamond_1.png", TEX_FMT_RGBA16),
	S(texture_armor_diamond2, "armor/diamond_2.png", TEX_FMT_RGBA16),
	S(texture_mob_char, "mob/char.png", TEX_FMT_RGBA16),
};
#undef S
#define SLOT_COUNT (sizeof(s_slots) / sizeof(s_slots[0]))

/* ---- state of the current tex_init ---- */
static const char* s_state; /* NULL = off */
static const char* s_reason = "";
static unsigned s_key_us, s_read_us, s_write_us;
static uint32_t s_tool_targets;
static bool s_recording;
static uint64_t s_key[3]; /* per target id (1, 2) */
static char s_dir[400];
static const char* s_pack;
static bool s_managed;
static struct pack_manifest s_manifest;
static int s_touched[SLOT_COUNT + 1];
static size_t s_touched_n;
static bool s_touched_bad;

struct dep {
	uint8_t tag;
	char name[DEP_NAME_MAX];
};
static struct dep* s_deps;
static size_t s_deps_n;
static bool s_deps_overflow;

/* One line per cache-on tex_init outcome (the field measurement hook). */
static void report(void) {
	fprintf(stderr,
			"[texture] pack cache %s (%s): source check %u us, read %u us, "
			"write %u us\n",
			s_state, s_reason, s_key_us, s_read_us, s_write_us);
}

static unsigned us_since(ptime_t t0) {
	return (unsigned)(time_diff_s(t0, time_get()) * 1000000.0F);
}

static bool cache_wanted(void) {
	JSON_Object* o = json_object(gstate.config_user.root);
	if(!o)
		return false;
	if(json_object_dothas_value_of_type(o, "pack.cache", JSONBoolean))
		return json_object_dotget_boolean(o, "pack.cache") == 1;
	const char* v = json_object_dotget_string(o, "pack.cache");
	return v && (strcmp(v, "true") == 0 || strcmp(v, "on") == 0);
}

static bool atlas_mipmaps(void) {
	return config_read_flag(&gstate.config_user, "render.mipmaps", false);
}

static bool is_default(void) {
	return strcmp(s_pack, DEFAULT_PACK) == 0;
}

void tex_cache_dir(char* out, size_t size) {
	const char* pack = config_read_path(&gstate.config_user,
										"paths.texturepack", DEFAULT_PACK);
	/* Bundled defaults cache beside the application, never inside assets/. */
	if(strcmp(pack, DEFAULT_PACK) == 0)
		snprintf(out, size, ".cavewii/bundled-textures");
	else
		snprintf(out, size, "%.300s/.cavewii/textures", pack);
}

static uint64_t fnv_str(uint64_t h, const char* s) {
	return pack_cache_fnv(h, s, strlen(s) + 1);
}

static uint64_t key_for(uint32_t target) {
	uint64_t h = pack_cache_fnv(PACK_CACHE_FNV_INIT, TEX_MAGIC, 4);
	h = pack_cache_fnv_u32(h, TEX_CACHE_SCHEMA);
	h = pack_cache_fnv_u32(h, TEX_CACHE_POLICY_VERSION);
	h = pack_cache_fnv_u32(h, TEX_GX_CONV_VERSION);
	h = pack_cache_fold_code_identity(h); /* Fable M3: this build's code */
	h = pack_cache_fnv_u32(h, target);
	h = pack_cache_fnv_u32(h, TEXAT_MAX);
	h = pack_cache_fnv_u32(h, TEX_GX_ATLAS_LODS);
	h = pack_cache_fnv_u32(h, atlas_mipmaps() ? 1u : 0u);
	h = pack_cache_fnv_u32(h, (uint32_t)SLOT_COUNT);
	for(size_t k = 0; k < SLOT_COUNT; k++) {
		h = fnv_str(h, s_slots[k].file);
		h = pack_cache_fnv_u32(h, (uint32_t)s_slots[k].fmt);
		h = pack_cache_fnv_u32(h, (s_slots[k].linear ? 1u : 0u)
								   | (s_slots[k].atlas ? 2u : 0u)
								   | (tex_bundled_only(s_slots[k].file) ? 4u
																		: 0u));
	}
	h = pack_cache_fnv_u32(h, is_default() ? 1u : 0u);
	/* astra-packkey-0927: a helper-managed pack is keyed by its manifest. */
	h = pack_cache_fnv_u32(h, s_managed ? 1u : 0u);
	if(s_managed) {
		h = pack_cache_fnv(h, &s_manifest.digest, sizeof(s_manifest.digest));
		h = pack_cache_fnv_u32(h, s_manifest.files);
	}
	return h;
}

/* ---- dependency recording (fresh load) and validation (hit) ---- */

static void observe(int tag, const char* filename) {
	/* The manifest digest covers the managed pack's files -- but not the
	 * engine-generated .cavewii/ backdrop (BG-BAKE-1028/3, texture.c): a bake
	 * published after this generation must invalidate it, so those names
	 * are always recorded and re-hashed. */
	if(tag == 0 && s_managed && strncmp(filename, ".cavewii/", 9) != 0)
		return;
	for(size_t k = 0; k < s_deps_n; k++)
		if(s_deps[k].tag == tag && strcmp(s_deps[k].name, filename) == 0)
			return;
	if(s_deps_n == DEPS_MAX || strlen(filename) >= DEP_NAME_MAX) {
		s_deps_overflow = true;
		return;
	}
	s_deps[s_deps_n].tag = (uint8_t)tag;
	snprintf(s_deps[s_deps_n].name, DEP_NAME_MAX, "%s", filename);
	s_deps_n++;
}

static uint64_t hash_dep(uint64_t h, int tag, const char* name) {
	static uint8_t buf[IO_CHUNK];
	h = pack_cache_fnv_u32(h, (uint32_t)tag);
	h = fnv_str(h, name);
	char path[700];
	snprintf(path, sizeof(path), "%s/%s", tex_root_for_tag(tag), name);
	FILE* f = fopen(path, "rb");
	if(!f)
		return pack_cache_fnv_u32(h, 0xFFFFFFFFu); /* absent */
	uint64_t fh = PACK_CACHE_FNV_INIT;
	uint32_t size = 0;
	size_t got;
	while((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		fh = pack_cache_fnv(fh, buf, got);
		size += (uint32_t)got;
	}
	fclose(f);
	h = pack_cache_fnv_u32(h, size);
	return pack_cache_fnv(h, &fh, sizeof(fh));
}

static uint64_t hash_deps(const struct dep* deps, size_t n) {
	uint64_t h = PACK_CACHE_FNV_INIT;
	for(size_t k = 0; k < n; k++)
		h = hash_dep(h, deps[k].tag, deps[k].name);
	return h;
}

/* ---- common prologue ---- */

static bool session_begin(void) {
	s_state = NULL;
	s_reason = "";
	s_key_us = s_read_us = s_write_us = 0;
	s_recording = false;
	if(!cache_wanted())
		return false;
	s_pack = config_read_path(&gstate.config_user, "paths.texturepack",
							  DEFAULT_PACK);
	s_managed = !is_default() && pack_manifest_load(s_pack, &s_manifest);
	if(!is_default() && s_manifest.status == PACK_MANIFEST_ERROR) {
		/* Fable M2: an unreadable manifest is not "unmanaged" -- keying it
		   so would churn the store. Nothing is read or written this load. */
		s_state = "bypass";
		s_reason = "helper manifest unreadable this load (no memory / I/O)";
		report();
		return false;
	}
	tex_cache_dir(s_dir, sizeof(s_dir));
	s_key[TEX_CACHE_TARGET_PC] = key_for(TEX_CACHE_TARGET_PC);
	s_key[TEX_CACHE_TARGET_WII] = key_for(TEX_CACHE_TARGET_WII);
	return true;
}

static void begin_recording(void) {
	if(!s_deps)
		s_deps = malloc(DEPS_MAX * sizeof(*s_deps));
	s_deps_n = 0;
	s_deps_overflow = s_deps == NULL;
	s_touched_n = 0;
	s_touched_bad = false;
	s_recording = true;
	tex_set_read_observer(observe);
}

void tex_cache_note_slot(struct tex_gfx* tex) {
	if(!s_recording)
		return;
	int idx = -1;
	for(size_t k = 0; k < SLOT_COUNT; k++)
		if(s_slots[k].tex == tex) {
			idx = (int)k;
			break;
		}
	if(idx < 0 || s_touched_n >= SLOT_COUNT)
		s_touched_bad = true;
	else
		s_touched[s_touched_n++] = idx;
}

/* ---- reading ---- */

struct slot_rec {
	uint8_t present, fmt, lods, flags;
	uint32_t width, height, bytes;
};

static bool slot_shape_ok(uint32_t target, size_t k, const struct slot_rec* r) {
	if(!r->present)
		return !r->fmt && !r->lods && !r->flags && !r->width && !r->height
			&& !r->bytes;
	if(r->fmt != (uint8_t)s_slots[k].fmt
	   || !tex_gfx_dimensions_valid(r->width, r->height, s_slots[k].fmt))
		return false;
	if(target == TEX_CACHE_TARGET_PC)
		return r->lods == 1 && r->bytes == r->width * r->height * 4u;
	if(r->lods == 1)
		/* Single-level is the Beta default. With mips requested, only a
		   grid that cannot isolate a chain may cache a single level; never
		   freeze a memory-pressure fallback. */
		return r->bytes == tex_gx_bytes(s_slots[k].fmt, r->width, r->height)
			&& (!s_slots[k].atlas || !atlas_mipmaps()
				|| tex_gx_atlas_bytes(r->width, r->height) == 0);
	return r->bytes != 0 && atlas_mipmaps() && r->lods == TEX_GX_ATLAS_LODS && s_slots[k].atlas
		&& r->bytes == tex_gx_atlas_bytes(r->width, r->height);
}

static bool read_into(struct pack_cache_reader* rd, uint8_t* dst, size_t n) {
	for(size_t off = 0; off < n;) {
		size_t c = n - off < IO_CHUNK ? n - off : IO_CHUNK;
		if(!pack_cache_read(rd, dst + off, c))
			return false;
		off += c;
	}
	return true;
}

/* Reads and validates one generation completely, then installs it. */
static const char* read_generation(const struct pack_cache_record* rec,
								   uint32_t target) {
	struct pack_cache_reader rd;
	const char* why = pack_cache_read_open(&rd, s_dir, rec);
	if(why)
		return why;
	uint8_t head[HEADER_BYTES];
	uint8_t tables[2 * TEXAT_MAX];
	struct dep* deps = NULL;
	struct slot_rec recs[SLOT_COUNT];
	void* bufs[SLOT_COUNT];
	uint8_t *grass = NULL, *foliage = NULL;
	memset(bufs, 0, sizeof(bufs));
	if(!pack_cache_read(&rd, head, HEADER_BYTES)) {
		why = "short header";
		goto out;
	}
	if(memcmp(head, TEX_MAGIC, 4) != 0
	   || pack_cache_get_u32(head + 4) != TEX_CACHE_SCHEMA) {
		why = "bad magic or schema";
		goto out;
	}
	if(pack_cache_get_u32(head + 8) != target) {
		why = "built for another target";
		goto out;
	}
	if(pack_cache_get_u64(head + 12) != rec->key
	   || pack_cache_get_u32(head + 20) != rec->generation) {
		why = "header disagrees with its commit record";
		goto out;
	}
	uint32_t n_slots = pack_cache_get_u32(head + 24);
	uint32_t n_deps = pack_cache_get_u32(head + 28);
	uint64_t dep_hash = pack_cache_get_u64(head + 32);
	uint32_t gw = pack_cache_get_u32(head + 60), gh = pack_cache_get_u32(head + 64);
	uint32_t fw = pack_cache_get_u32(head + 68), fh = pack_cache_get_u32(head + 72);
	if(n_slots != SLOT_COUNT || pack_cache_get_u32(head + 56) != TEXAT_MAX
	   || n_deps > DEPS_MAX || gw > 1024 || gh > 1024 || fw > 1024
	   || fh > 1024 || (!gw != !gh) || (!fw != !fh)) {
		why = "table shape differs";
		goto out;
	}
	if(!pack_cache_read(&rd, tables, sizeof(tables))) {
		why = "short atlas tables";
		goto out;
	}

	/* The examined-source list, then re-hash exactly those files. */
	ptime_t tk = time_get();
	deps = malloc((n_deps ? n_deps : 1) * sizeof(*deps));
	if(!deps) {
		why = "no memory for the source list";
		goto out;
	}
	for(uint32_t k = 0; k < n_deps; k++) {
		uint8_t b[3];
		if(!pack_cache_read(&rd, b, 3)) {
			why = "short source list";
			goto out;
		}
		unsigned len = b[1] | (unsigned)b[2] << 8;
		if(b[0] > 2 || len == 0 || len >= DEP_NAME_MAX
		   || !pack_cache_read(&rd, deps[k].name, len)) {
			why = "bad source list";
			goto out;
		}
		deps[k].tag = b[0];
		deps[k].name[len] = '\0';
		if(strlen(deps[k].name) != len) {
			why = "bad source list";
			goto out;
		}
	}
	bool deps_same = hash_deps(deps, n_deps) == dep_hash;
	s_key_us += us_since(tk);
	if(!deps_same) {
		why = "a source file changed";
		goto out;
	}

	uint64_t payload = 0;
	for(size_t k = 0; k < SLOT_COUNT; k++) {
		uint8_t b[SLOT_REC_BYTES];
		if(!pack_cache_read(&rd, b, sizeof(b))) {
			why = "short slot table";
			goto out;
		}
		recs[k] = (struct slot_rec) {b[0], b[1], b[2], b[3],
									 pack_cache_get_u32(b + 4),
									 pack_cache_get_u32(b + 8),
									 pack_cache_get_u32(b + 12)};
		if(!slot_shape_ok(target, k, &recs[k])) {
			why = "slot entry out of bounds";
			goto out;
		}
		payload += recs[k].bytes;
	}
	payload += (uint64_t)gw * gh * 4u + (uint64_t)fw * fh * 4u;
	if(payload != rd.remaining) {
		why = "payload length out of bounds";
		goto out;
	}
	for(size_t k = 0; k < SLOT_COUNT; k++) {
		if(!recs[k].present)
			continue;
		bufs[k] = tex_cache_texel_alloc(recs[k].bytes);
		if(!bufs[k]) {
			why = "no memory for a texture";
			goto out;
		}
		if(!read_into(&rd, bufs[k], recs[k].bytes)) {
			why = "short payload";
			goto out;
		}
	}
	if(gw) {
		grass = mem2_tag(mem2_alloc((size_t)gw * gh * 4u),
						 (size_t)gw * gh * 4u, CARVE_TEXTURE);
		if(!grass || !read_into(&rd, grass, (size_t)gw * gh * 4u)) {
			why = grass ? "short payload" : "no memory for a colormap";
			goto out;
		}
	}
	if(fw) {
		foliage = mem2_tag(mem2_alloc((size_t)fw * fh * 4u),
						   (size_t)fw * fh * 4u, CARVE_TEXTURE);
		if(!foliage || !read_into(&rd, foliage, (size_t)fw * fh * 4u)) {
			why = foliage ? "short payload" : "no memory for a colormap";
			goto out;
		}
	}
	why = pack_cache_read_finish(&rd);
	if(why)
		goto out;

	/* Validated end to end: install. A backend refusal part-way leaves
	   some slots replaced; the caller then runs the fresh load, which
	   replaces every slot again. */
	for(size_t k = 0; k < SLOT_COUNT; k++) {
		if(!recs[k].present) {
			tex_gfx_clear(s_slots[k].tex);
			continue;
		}
		void* b = bufs[k];
		bufs[k] = NULL; /* ownership passes on every path */
		if(!tex_gfx_restore(s_slots[k].tex, b, recs[k].bytes, recs[k].width,
							recs[k].height, s_slots[k].fmt, recs[k].lods,
							(recs[k].flags & 1u) != 0, s_slots[k].atlas)) {
			why = "the backend refused a restored texture";
			goto out;
		}
	}
	biome_color_import(grass, gw, gh, foliage, fw, fh);
	grass = foliage = NULL;
	memcpy(texture_sign_color, head + 40, 3);
	g_water_atlas_x = (int32_t)pack_cache_get_u32(head + 44);
	g_water_atlas_y = (int32_t)pack_cache_get_u32(head + 48);
	g_water_atlas_tile = (int32_t)pack_cache_get_u32(head + 52);
	tex_atlas_tables_import(tables, tables + TEXAT_MAX);
	why = NULL;
out:
	for(size_t k = 0; k < SLOT_COUNT; k++)
		tex_cache_texel_free(bufs[k]);
	if(grass)
		mem2_free(grass);
	if(foliage)
		mem2_free(foliage);
	free(deps);
	pack_cache_read_close(&rd);
	return why;
}

bool tex_cache_try_restore(void) {
	if(!session_begin())
		return false;
	uint32_t target = TEX_CACHE_HOST_TARGET;
	if(s_tool_targets) {
		s_state = "miss";
		s_reason = "host tool build";
		begin_recording();
		return false;
	}
	ptime_t t0 = time_get();
	struct pack_cache_record r[2];
	pack_cache_records(s_dir, TEX_CACHE_SCHEMA, r);
	int order[2];
	pack_cache_order(r, order);
	bool any_valid = false, any_key = false, hit = false;
	const char* why = NULL; /* the NEWEST matching generation's reason */
	for(int i = 0; i < 2 && !hit; i++) {
		const struct pack_cache_record* rec = &r[order[i]];
		if(!rec->valid)
			continue;
		any_valid = true;
		if(rec->key != s_key[target])
			continue;
		any_key = true;
		const char* w = read_generation(rec, target);
		hit = w == NULL;
		if(!why)
			why = w;
	}
	s_read_us = us_since(t0);
	if(hit) {
		s_state = "hit";
		s_reason = "hit";
		report();
		return true;
	}
	s_state = "miss";
	s_reason = any_key ? why
		: any_valid	   ? "content changed"
					   : "no valid commit record";
	begin_recording();
	return false;
}

/* ---- writing ---- */

/* The bytes of slot k for `target`, from the live state. `owned` = the
   returned buffer must be freed (a foreign-target encoding). */
static const uint8_t* slot_bytes(uint32_t target, size_t k,
								 struct slot_rec* r, bool* owned) {
	const struct tex_gfx* t = s_slots[k].tex;
	*owned = false;
	memset(r, 0, sizeof(*r));
	if(!t->data)
		return NULL;
	r->present = 1;
	r->fmt = (uint8_t)s_slots[k].fmt;
	r->width = (uint32_t)t->width;
	r->height = (uint32_t)t->height;
	if(target == TEX_CACHE_HOST_TARGET) {
		/* Fable M1: never publish a Wii atlas that fell back to a single
		   LOD under memory pressure (NULL here refuses the generation). */
		if(target == TEX_CACHE_TARGET_WII && atlas_mipmaps() && s_slots[k].atlas
		   && t->lods != TEX_GX_ATLAS_LODS
		   && tex_gx_atlas_bytes(t->width, t->height) != 0)
			return NULL;
		r->lods = t->lods;
		r->flags = t->linear ? 1u : 0u;
		r->bytes = (uint32_t)t->data_bytes;
		return t->data;
	}
#ifdef PLATFORM_WII
	return NULL; /* the console only produces its own target */
#else
	/* PC live state = the RGBA images the Wii converts: encode exactly as
	   wii/texture.c would (atlas: isolated opt-in chain, else nearest level
	   zero). */
	if(target != TEX_CACHE_TARGET_WII || t->fmt != s_slots[k].fmt)
		return NULL;
	size_t w = t->width, h = t->height;
	uint8_t* out = NULL;
	if(s_slots[k].atlas && atlas_mipmaps()) {
		size_t total = tex_gx_atlas_bytes(w, h);
		struct tex_atlas_grid grid;
		if(total && tex_atlas_grid_init(&grid, w, h)
		   && (out = malloc(total))
		   && tex_gx_encode_atlas(t->data, w, h, &grid, out)) {
			r->lods = TEX_GX_ATLAS_LODS;
			r->flags = 1;
			r->bytes = (uint32_t)total;
			*owned = true;
			return out;
		}
		free(out);
		if(total)
			return NULL; /* Fable M1: a chain that could exist failed (OOM) */
		out = NULL;
	}
	size_t bytes = tex_gx_bytes(s_slots[k].fmt, w, h);
	out = malloc(bytes);
	if(!out || !tex_gx_encode(s_slots[k].fmt, t->data, w, h, out)) {
		free(out);
		return NULL;
	}
	r->lods = 1;
	r->flags = !s_slots[k].atlas && t->linear ? 1u : 0u;
	r->bytes = (uint32_t)bytes;
	*owned = true;
	return out;
#endif
}

static bool write_target(uint32_t target, uint64_t dep_hash) {
	struct pack_cache_txn t;
	if(!pack_cache_begin(&t, s_dir, TEX_CACHE_SCHEMA))
		return false;
	const uint8_t *grass, *foliage;
	size_t gw, gh, fw, fh;
	biome_color_export(&grass, &gw, &gh, &foliage, &fw, &fh);

	uint8_t head[HEADER_BYTES];
	memset(head, 0, sizeof(head));
	memcpy(head, TEX_MAGIC, 4);
	pack_cache_put_u32(head + 4, TEX_CACHE_SCHEMA);
	pack_cache_put_u32(head + 8, target);
	pack_cache_put_u64(head + 12, s_key[target]);
	pack_cache_put_u32(head + 20, t.generation);
	pack_cache_put_u32(head + 24, (uint32_t)SLOT_COUNT);
	pack_cache_put_u32(head + 28, (uint32_t)s_deps_n);
	pack_cache_put_u64(head + 32, dep_hash);
	memcpy(head + 40, texture_sign_color, 3);
	pack_cache_put_u32(head + 44, (uint32_t)g_water_atlas_x);
	pack_cache_put_u32(head + 48, (uint32_t)g_water_atlas_y);
	pack_cache_put_u32(head + 52, (uint32_t)g_water_atlas_tile);
	pack_cache_put_u32(head + 56, TEXAT_MAX);
	pack_cache_put_u32(head + 60, (uint32_t)gw);
	pack_cache_put_u32(head + 64, (uint32_t)gh);
	pack_cache_put_u32(head + 68, (uint32_t)fw);
	pack_cache_put_u32(head + 72, (uint32_t)fh);
	pack_cache_put(&t, head, sizeof(head));

	uint8_t tables[2 * TEXAT_MAX];
	tex_atlas_tables_export(tables, tables + TEXAT_MAX);
	pack_cache_put(&t, tables, sizeof(tables));

	for(size_t k = 0; k < s_deps_n; k++) {
		size_t len = strlen(s_deps[k].name);
		uint8_t b[3] = {s_deps[k].tag, (uint8_t)len, (uint8_t)(len >> 8)};
		pack_cache_put(&t, b, 3);
		pack_cache_put(&t, s_deps[k].name, len);
	}

	/* Slot table, then payload. Foreign-target encodings are produced once
	   per slot and kept until the payload pass. */
	const uint8_t* data[SLOT_COUNT];
	bool owned[SLOT_COUNT];
	struct slot_rec recs[SLOT_COUNT];
	bool ok = true;
	for(size_t k = 0; k < SLOT_COUNT; k++) {
		data[k] = slot_bytes(target, k, &recs[k], &owned[k]);
		if(s_slots[k].tex->data && !data[k])
			ok = false; /* a present texture that cannot be serialized */
		uint8_t b[SLOT_REC_BYTES];
		b[0] = recs[k].present;
		b[1] = recs[k].fmt;
		b[2] = recs[k].lods;
		b[3] = recs[k].flags;
		pack_cache_put_u32(b + 4, recs[k].width);
		pack_cache_put_u32(b + 8, recs[k].height);
		pack_cache_put_u32(b + 12, recs[k].bytes);
		pack_cache_put(&t, b, sizeof(b));
	}
	for(size_t k = 0; k < SLOT_COUNT; k++)
		if(data[k])
			pack_cache_put(&t, data[k], recs[k].bytes);
	if(grass)
		pack_cache_put(&t, grass, gw * gh * 4);
	if(foliage)
		pack_cache_put(&t, foliage, fw * fh * 4);
	for(size_t k = 0; k < SLOT_COUNT; k++)
		if(owned[k])
			free((void*)data[k]);
	if(!ok) {
		pack_cache_abort(&t);
		return false;
	}
	return pack_cache_commit(&t, s_key[target]);
}

void tex_cache_after_load(bool loaded_ok) {
	if(!s_recording)
		return;
	s_recording = false;
	tex_set_read_observer(NULL);
	uint32_t targets = s_tool_targets ? s_tool_targets
									  : (1u << TEX_CACHE_HOST_TARGET);
	s_tool_targets = 0;
	bool slots_match = !s_touched_bad && s_touched_n == SLOT_COUNT;
	for(size_t k = 0; slots_match && k < SLOT_COUNT; k++)
		slots_match = s_touched[k] == (int)k;
	if(!loaded_ok || !slots_match || s_deps_overflow) {
		/* Never publish an incomplete or unaccounted load. */
		if(!slots_match)
			fprintf(stderr, "[texture] pack cache: load sites do not match "
							"the slot table; not published\n");
		return;
	}
	ptime_t t0 = time_get();
	uint64_t dep_hash = hash_deps(s_deps, s_deps_n);
	bool ok = true;
	for(uint32_t target = 1; target <= 2; target++)
		if(targets & (1u << target))
			ok = write_target(target, dep_hash) && ok;
	s_write_us = us_since(t0);
	s_state = ok ? "written" : "unwritable";
	report();
	if(!ok)
		fprintf(stderr, "[texture] pack cache: could not publish into "
						"\"%s\"; caching disabled for this load\n",
				s_dir);
}

const char* tex_cache_status(void) {
	return s_state ? s_state : "off";
}

const char* tex_cache_reason(void) {
	return s_reason;
}

void tex_cache_timings(unsigned* key_us, unsigned* read_us,
					   unsigned* write_us) {
	*key_us = s_key_us;
	*read_us = s_read_us;
	*write_us = s_write_us;
}

void tex_cache_tool_targets(uint32_t targets_mask) {
	s_tool_targets = targets_mask;
}

#ifdef CAVEWII_TESTING
bool tex_cache_test_slot_shape_ok(uint32_t target, size_t k, uint8_t lods,
								  uint32_t width, uint32_t height,
								  uint32_t bytes) {
	struct slot_rec r = {1, (uint8_t)s_slots[k].fmt, lods, 1u, width, height,
						 bytes};
	return k < SLOT_COUNT && slot_shape_ok(target, k, &r);
}

size_t tex_cache_test_slot_index(const struct tex_gfx* tex) {
	for(size_t k = 0; k < SLOT_COUNT; k++)
		if(s_slots[k].tex == tex)
			return k;
	return SIZE_MAX;
}
#endif

size_t tex_cache_slot_count(void) {
	return SLOT_COUNT;
}

struct tex_gfx* tex_cache_slot(size_t k) {
	return k < SLOT_COUNT ? s_slots[k].tex : NULL;
}
