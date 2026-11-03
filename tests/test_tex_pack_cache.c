/*
	pack-cache-0927 (Astra astra-packcacheB-0927, astra-packkey-0927): the
	opt-in pre-baked TEXTURE cache.

	Every load below is compared against a FRESH load of the same tree (the
	cache off) by a 64-bit FNV-1a over the whole observable result: for every
	cache slot its width, height, format, LOD count, filter flag, byte count
	and EVERY byte the backend holds (PC: the RGBA image uploaded to GL, from
	which the atlas mips are rebuilt), the sign board colour, both biome
	colormaps, both atlas lookup tables and the live-water capture. Equal
	digest = the restored state is byte-identical to a fresh load.

	The fixture is synthetic (tests write their own PNGs): a bundled "assets"
	root with the required cores + a few optional files + the bundled-only
	controls.png, and a "pack" that overrides some of them.

	  T0 cache OFF (default): status "off", no .cavewii anywhere.
	  T1 unmanaged pack: miss -> fresh -> generation 1; then HIT == fresh.
	  T2 an unmanaged pack file changed: the recorded source list re-hash
	     catches it: miss "a source file changed" -> fresh; then HIT.
	  T3 helper manifest written (import): key changes -> miss "content
	     changed" -> fresh; then HIT.
	  T4 a managed pack file edited by hand, not re-imported: stale HIT (the
	     documented manifest limit); re-imported: miss -> fresh; then HIT.
	  T5 a bundled FALLBACK file the pack omits changed: miss "a source file
	     changed" -> fresh.
	  T6 the bundled-only controller sheet changed: miss -> fresh.
	  T7 corrupt payload byte: miss "payload checksum mismatch" -> fresh;
	     republished; then HIT.
	  T8 torn next generation + torn record in the free slot: ignored, HIT.
	  T9 host-tool Wii target: published beside the PC one; it carries the
	     exact GX bytes of the PC RGBA images (particle atlas 4-LOD chain,
	     RGB5A3 items, I8 font -- tex_gx_encode*, proven equal to the Wii's
	     own conversion by test_tex_gx_conv_equiv); the PC load still HITs
	     its own.
	  T10 bundled root: caches to .cavewii/bundled-textures, never inside
	     assets/; miss then HIT == fresh.
	  Fable review 2026-09-27:
	  T11 (M1) a Wii atlas slot whose mip chain COULD be built is never
	     accepted single-LOD (a memory-pressure fallback is not frozen); a
	     size whose chain cannot exist (336) keeps its single-LOD form.
	  T12 (M3) the key folds the source-tree identity: another build misses,
	     the original build still HITS its own generation.
	  T13 (M2) a manifest that cannot be READ (allocation failure) bypasses
	     the cache for that load -- it is not "unmanaged" (no key churn, no
	     write); the next readable load HITS the managed generation.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CAVEWII_TESTING
#define CAVEWII_TESTING /* the test hooks (compiled into the test pools) */
#endif
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define CHDIR(p) _chdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define CHDIR(p) chdir(p)
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "support/test_fixture_dir.h"
#include "config.h"
#include "game/game_state.h"
#include "graphics/biome_color.h"
#include "graphics/texture_atlas.h"
#include "lodepng/lodepng.h"
#include "platform/pack_cache.h"
#include "platform/pack_manifest.h"
#include "platform/tex_gx_conv.h"
#include "platform/texture.h"
#include "platform/texture_cache.h"

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static char s_fixture[128];

static void mkdir_p(const char* path) {
	char buf[400];
	snprintf(buf, sizeof(buf), "%.399s", path);
	for(char* p = buf + 1; *p; p++)
		if(*p == '/') {
			*p = '\0';
			MKDIR(buf);
			*p = '/';
		}
	MKDIR(buf);
}

static void put_png(const char* root, const char* rel, unsigned w, unsigned h,
					uint32_t seed) {
	char path[400];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	char dir[400];
	snprintf(dir, sizeof(dir), "%s", path);
	*strrchr(dir, '/') = '\0';
	mkdir_p(dir);
	uint8_t* img = malloc((size_t)w * h * 4);
	assert(img);
	uint32_t x = seed * 2654435761u + 7u;
	for(size_t k = 0; k < (size_t)w * h * 4; k++) {
		x = x * 1103515245u + 12345u;
		img[k] = (uint8_t)(x >> 16);
		if(k % 4 == 3 && seed % 3 == 0)
			img[k] = 255; /* some opaque images too */
	}
	assert(lodepng_encode32_file(path, img, w, h) == 0);
	free(img);
}

static void fixture_build(void) {
	put_png("assets", "terrain.png", 256, 256, 1);
	put_png("assets", "particles.png", 128, 128, 2);
	put_png("assets", "font/default.png", 128, 128, 3);
	put_png("assets", "environment/clouds.png", 256, 256, 4);
	put_png("assets", "environment/rain.png", 64, 64, 5);
	put_png("assets", "anim.png", 256, 256, 6);
	put_png("assets", "gui/items.png", 256, 256, 7);
	put_png("assets", "controls.png", 256, 256, 8);
	put_png("assets", "misc/grasscolor.png", 256, 256, 9);
	put_png("assets", "misc/foliagecolor.png", 256, 256, 10);
	put_png("assets", "terrain/sun.png", 32, 32, 11);
	put_png("pack", "terrain.png", 256, 256, 21);
	put_png("pack", "gui/items.png", 256, 256, 22);
	put_png("pack", "mob/pig.png", 64, 32, 23);
	put_png("pack", "item/arrows.png", 32, 32, 26);
	put_png("pack", "misc/grasscolor.png", 256, 256, 24);
	put_png("pack", "controls.png", 256, 256, 25); /* ignored: bundled-only */
}

static uint64_t state_hash(void) {
	uint64_t h = PACK_CACHE_FNV_INIT;
	for(size_t k = 0; k < tex_cache_slot_count(); k++) {
		const struct tex_gfx* t = tex_cache_slot(k);
		uint32_t f[6] = {(uint32_t)t->width, (uint32_t)t->height,
						 (uint32_t)t->fmt, t->lods, t->linear,
						 (uint32_t)t->data_bytes};
		h = pack_cache_fnv(h, f, sizeof(f));
		h = pack_cache_fnv_u32(h, t->data ? 1u : 0u);
		if(t->data)
			h = pack_cache_fnv(h, t->data, t->data_bytes);
	}
	h = pack_cache_fnv(h, texture_sign_color, 3);
	const uint8_t *g, *fo;
	size_t gw, gh, fw, fh;
	biome_color_export(&g, &gw, &gh, &fo, &fw, &fh);
	uint32_t d[4] = {(uint32_t)gw, (uint32_t)gh, (uint32_t)fw, (uint32_t)fh};
	h = pack_cache_fnv(h, d, sizeof(d));
	if(g)
		h = pack_cache_fnv(h, g, gw * gh * 4);
	if(fo)
		h = pack_cache_fnv(h, fo, fw * fh * 4);
	uint8_t tables[2 * TEXAT_MAX];
	tex_atlas_tables_export(tables, tables + TEXAT_MAX);
	h = pack_cache_fnv(h, tables, sizeof(tables));
	int32_t wa[3] = {g_water_atlas_x, g_water_atlas_y, g_water_atlas_tile};
	return pack_cache_fnv(h, wa, sizeof(wa));
}

static void set_cache(bool on) {
	assert(config_set_string(&gstate.config_user, "pack.cache",
							 on ? "true" : "false"));
}

static void set_pack(const char* pack) {
	assert(config_set_string(&gstate.config_user, "paths.texturepack", pack));
}

static bool load(void) {
	return tex_init();
}

/* A fresh load of the CURRENT tree (cache off), then back on. */
static uint64_t fresh_hash(void) {
	set_cache(false);
	assert(load());
	assert(strcmp(tex_cache_status(), "off") == 0);
	uint64_t h = state_hash();
	set_cache(true);
	return h;
}

static bool status_is(const char* s) {
	return strcmp(tex_cache_status(), s) == 0;
}

static bool reason_is(const char* s) {
	return strcmp(tex_cache_reason(), s) == 0;
}

static bool exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static long file_size(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void gen_file(char* out, size_t size, const char* dir, unsigned g) {
	snprintf(out, size, "%s/g%08u.bin", dir, g);
}

static void flip_byte(const char* path, long at) {
	FILE* f = fopen(path, "r+b");
	assert(f);
	assert(fseek(f, at, SEEK_SET) == 0);
	int c = fgetc(f);
	assert(c != EOF);
	assert(fseek(f, at, SEEK_SET) == 0);
	fputc(c ^ 0x5A, f);
	fclose(f);
}

static void write_bytes(const char* path, const char* data, size_t n) {
	FILE* f = fopen(path, "wb");
	assert(f);
	fwrite(data, 1, n, f);
	fclose(f);
}

static uint8_t* slurp(const char* path, long* n) {
	*n = file_size(path);
	if(*n <= 0)
		return NULL;
	uint8_t* b = malloc((size_t)*n);
	FILE* f = fopen(path, "rb");
	assert(b && f);
	assert(fread(b, 1, (size_t)*n, f) == (size_t)*n);
	fclose(f);
	return b;
}

static bool contains(const uint8_t* hay, long n, const uint8_t* needle,
					 size_t m) {
	for(long k = 0; k + (long)m <= n; k++)
		if(hay[k] == needle[0] && memcmp(hay + k, needle, m) == 0)
			return true;
	return false;
}

#define DIR_TEX "pack/.cavewii/textures"

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_tex_pack_cache");
	assert(glfwInit());
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* gl = glfwCreateWindow(32, 32, "tex pack cache", NULL, NULL);
	assert(gl);
	glfwMakeContextCurrent(gl);
	assert(glewInit() == GLEW_OK);

	test_fixture_dir("test_tex_pack_cache_tmp", s_fixture, sizeof(s_fixture));
	MKDIR(s_fixture);
	assert(CHDIR(s_fixture) == 0);
	fixture_build();
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	set_pack("pack");

	/* T0 */
	check(load(), "T0 setup: the synthetic fixture loads completely");
	uint64_t h0 = state_hash();
	check(status_is("off") && !exists("pack/.cavewii") && !exists(".cavewii"),
		  "T0 cache off: status off, no directory anywhere");
	check(texture_pig.data && texture_anim.data && texture_rain.data
			  && !texture_zombie.data,
		  "T0 the pack overrides, the bundled fallbacks and the absent "
		  "optionals all took their paths");

	check(texture_arrow.data && texture_arrow.width == 32
		  && texture_arrow.height == 32, "T0 arrow sheet loads from selected pack");
	/* T1 */
	set_cache(true);
	check(load() && status_is("written")
			  && reason_is("no valid commit record")
			  && exists(DIR_TEX "/commit-a.rec"),
		  "T1 first load misses, loads fresh, publishes generation 1");
	check(state_hash() == h0, "T1 the fresh load == cache-off");
	check(load() && status_is("hit") && state_hash() == h0,
		  "T1 next load HITS and is byte-identical to a fresh load");

	/* T2: unmanaged pack file changed */
	put_png("pack", "mob/pig.png", 64, 32, 33);
	uint64_t h1 = fresh_hash();
	check(h1 != h0, "T2 setup: the change moves the fresh digest");
	check(load() && status_is("written") && reason_is("a source file changed")
			  && state_hash() == h1,
		  "T2 an unmanaged pack edit is caught by the source re-hash");
	check(load() && status_is("hit") && state_hash() == h1, "T2 then HITS");

	/* T3: helper manifest */
	struct pack_manifest man;
	check(pack_manifest_write("pack", &man) && man.trusted,
		  "T3 setup: the import step writes a trusted manifest");
	check(load() && status_is("written") && reason_is("content changed")
			  && state_hash() == h1,
		  "T3 the managed key misses once, loads fresh");
	check(load() && status_is("hit") && state_hash() == h1, "T3 then HITS");

	/* T4: manual edit without re-import */
	put_png("pack", "gui/items.png", 256, 256, 44);
	uint64_t h2 = fresh_hash();
	check(h2 != h1, "T4 setup: the edit moves the fresh digest");
	check(load() && status_is("hit") && state_hash() == h1,
		  "T4 a manual edit of a managed pack is NOT detected without "
		  "re-import (stale hit, the documented limit)");
	assert(pack_manifest_write("pack", &man));
	check(load() && status_is("written") && state_hash() == h2,
		  "T4 re-imported: miss, fresh load");
	check(load() && status_is("hit") && state_hash() == h2, "T4 then HITS");

	/* T5: a bundled fallback changed */
	put_png("assets", "anim.png", 256, 256, 55);
	uint64_t h3 = fresh_hash();
	check(load() && status_is("written") && reason_is("a source file changed")
			  && state_hash() == h3,
		  "T5 a changed bundled fallback misses, loads fresh");

	/* T6: the bundled-only controller sheet changed */
	put_png("assets", "controls.png", 256, 256, 66);
	uint64_t h4 = fresh_hash();
	check(h4 != h3 && load() && status_is("written")
			  && reason_is("a source file changed") && state_hash() == h4,
		  "T6 a changed bundled-only sheet misses, loads fresh");

	/* T7: corrupt the newest generation */
	check(load() && status_is("hit"), "T7 setup: hit");
	struct pack_cache_record r[2];
	pack_cache_records(DIR_TEX, TEX_CACHE_SCHEMA, r);
	int order[2];
	pack_cache_order(r, order);
	char g[512];
	gen_file(g, sizeof(g), DIR_TEX, r[order[0]].generation);
	flip_byte(g, file_size(g) - 100);
	bool t7 = load();
	check(t7 && status_is("written")
			  && reason_is("payload checksum mismatch") && state_hash() == h4,
		  "T7 a corrupt payload falls back to a fresh load and republishes");
	check(load() && status_is("hit") && state_hash() == h4, "T7 then HITS");

	/* T8: torn publication of the next generation */
	pack_cache_records(DIR_TEX, TEX_CACHE_SCHEMA, r);
	pack_cache_order(r, order);
	unsigned newest = r[order[0]].generation;
	char gnext[512];
	gen_file(gnext, sizeof(gnext), DIR_TEX, newest + 1);
	write_bytes(gnext, "CWTX\x01\x00", 6);
	char free_rec[512];
	snprintf(free_rec, sizeof(free_rec), "%s/commit-%c.rec", DIR_TEX,
			 order[0] == 0 ? 'b' : 'a');
	write_bytes(free_rec, "CWAR\x01\x00\x00\x00\x09", 9);
	check(load() && status_is("hit") && state_hash() == h4,
		  "T8 a torn next generation + torn record are ignored: HIT");

	/* T9: the host tool's Wii target */
	tex_cache_tool_targets(1u << TEX_CACHE_TARGET_WII);
	check(load() && status_is("written") && state_hash() == h4,
		  "T9 the host tool publishes a Wii-target generation");
	pack_cache_records(DIR_TEX, TEX_CACHE_SCHEMA, r);
	pack_cache_order(r, order);
	gen_file(g, sizeof(g), DIR_TEX, r[order[0]].generation);
	long gn = 0;
	uint8_t* gb = slurp(g, &gn);
	/* The particle atlas (the terrain atlas's LOD 0 is re-animated in
	   place by the water FX after the load, so its live mirror is not the
	   published pre-FX image) and two single-LOD formats. */
	size_t total = tex_gx_bytes(TEX_FMT_RGBA16, texture_particles.width,
								texture_particles.height);
	struct tex_atlas_grid grid;
	assert(tex_atlas_grid_init(&grid, texture_particles.width,
							   texture_particles.height));
	uint8_t* want = malloc(total);
	assert(gb && want && total
		   && tex_gx_encode(TEX_FMT_RGBA16, texture_particles.data,
							texture_particles.width, texture_particles.height,
							want));
	size_t ib = tex_gx_bytes(TEX_FMT_RGBA16, texture_items.width,
							 texture_items.height);
	size_t fb = tex_gx_bytes(TEX_FMT_I8, texture_font.width,
							 texture_font.height);
	uint8_t* wi = malloc(ib);
	uint8_t* wf = malloc(fb);
	assert(wi && wf
		   && tex_gx_encode(TEX_FMT_RGBA16, texture_items.data,
							texture_items.width, texture_items.height, wi)
		   && tex_gx_encode(TEX_FMT_I8, texture_font.data, texture_font.width,
							texture_font.height, wf));
	check(gb[8] == TEX_CACHE_TARGET_WII && contains(gb, gn, want, total)
			  && contains(gb, gn, wi, ib) && contains(gb, gn, wf, fb),
		  "T9 the Wii generation carries the exact GX bytes: the particle "
		  "default nearest level zero, RGB5A3 items, I8 font");
	free(want);
	free(wi);
	free(wf);
	free(gb);
	check(load() && status_is("hit") && state_hash() == h4,
		  "T9 the PC load ignores the Wii generation and HITS its own");

	/* T10: the bundled root */
	set_pack("assets");
	uint64_t hb = fresh_hash();
	check(load() && status_is("written")
			  && exists(".cavewii/bundled-textures/commit-a.rec")
			  && !exists("assets/.cavewii"),
		  "T10 the bundled root caches beside the app, never inside assets/");
	check(load() && status_is("hit") && state_hash() == hb, "T10 then HITS");

	/* T11: default and opt-in Wii cache shapes agree with fresh uploads. */
	size_t ti = tex_cache_test_slot_index(&texture_terrain);
	assert(ti != SIZE_MAX);
	check(tex_cache_test_slot_shape_ok(
			  TEX_CACHE_TARGET_WII, ti, 1, 256, 256,
			  (uint32_t)tex_gx_bytes(TEX_FMT_RGBA16, 256, 256))
			  && !tex_cache_test_slot_shape_ok(
				  TEX_CACHE_TARGET_WII, ti, TEX_GX_ATLAS_LODS, 256, 256,
				  (uint32_t)tex_gx_atlas_bytes(256, 256)),
		  "T11 default accepts single level and rejects a stale mip chain");
	JSON_Object* cfg = json_object(gstate.config_user.root);
	assert(json_object_dotset_boolean(cfg, "render.mipmaps", 1) == JSONSuccess);
	check(!tex_cache_test_slot_shape_ok(
			  TEX_CACHE_TARGET_WII, ti, 1, 256, 256,
			  (uint32_t)tex_gx_bytes(TEX_FMT_RGBA16, 256, 256))
			  && tex_cache_test_slot_shape_ok(
				  TEX_CACHE_TARGET_WII, ti, TEX_GX_ATLAS_LODS, 256, 256,
				  (uint32_t)tex_gx_atlas_bytes(256, 256))
			  && tex_cache_test_slot_shape_ok(
				  TEX_CACHE_TARGET_WII, ti, 1, 336, 336,
				  (uint32_t)tex_gx_bytes(TEX_FMT_RGBA16, 336, 336)),
		  "T11 opt-in accepts isolated chain/unsafe-grid fallback, rejects OOM "
		  "fallback");
	assert(json_object_dotset_boolean(cfg, "render.mipmaps", 0) == JSONSuccess);

	/* T12 (M3) */
	check(load() && status_is("hit"), "T12 setup: the bundled root HITS");
	pack_cache_test_set_code_identity("some-other-build");
	check(load() && status_is("written") && reason_is("content changed")
			  && state_hash() == hb,
		  "T12 another build's key misses (never serves this build's cache)");
	pack_cache_test_set_code_identity(NULL);
	check(load() && status_is("hit") && state_hash() == hb,
		  "T12 the original build still HITS its own generation");

	/* T13 (M2) */
	set_pack("pack");
	check(load() && status_is("hit"), "T13 setup: the managed pack HITS");
	pack_cache_records(DIR_TEX, TEX_CACHE_SCHEMA, r);
	unsigned gen_a = r[0].generation, gen_b = r[1].generation;
	pack_manifest_test_fail_alloc(true);
	bool t13 = load();
	pack_manifest_test_fail_alloc(false);
	pack_cache_records(DIR_TEX, TEX_CACHE_SCHEMA, r);
	check(t13 && status_is("bypass") && state_hash() == h4
			  && r[0].generation == gen_a && r[1].generation == gen_b,
		  "T13 an unreadable manifest bypasses (no unmanaged key, no write)");
	check(load() && status_is("hit") && state_hash() == h4,
		  "T13 the next readable load HITS the managed generation");

	/* T14: optional arrow lifetime across selected, absent and fallback packs. */
	assert(remove("pack/item/arrows.png") == 0);
	assert(pack_manifest_write("pack", &man));
	uint64_t no_arrow = fresh_hash();
	check(!texture_arrow.data && load() && status_is("written")
		  && !texture_arrow.data && state_hash() == no_arrow,
		  "T14 missing in both packs clears former arrow image, fresh == cache");
	check(load() && status_is("hit") && !texture_arrow.data
		  && state_hash() == no_arrow, "T14 absent arrow survives cache HIT");
	put_png("assets", "item/arrows.png", 32, 32, 126);
	uint64_t bundled_arrow = fresh_hash();
	check(texture_arrow.data && bundled_arrow != no_arrow
		  && load() && state_hash() == bundled_arrow,
		  "T14 selected pack miss uses bundled arrow with byte-identical cache");
	check(load() && status_is("hit") && state_hash() == bundled_arrow,
		  "T14 bundled arrow fallback survives cache HIT");
	assert(remove("assets/item/arrows.png") == 0);
	check(load() && !texture_arrow.data && state_hash() == no_arrow,
		  "T14 removed fallback clears the cached arrow image");

	/* NPOT native grids must accept nearest caches and reject old mip records. */
	assert(json_object_dotset_boolean(cfg, "render.mipmaps", 1) == JSONSuccess);
	check(tex_cache_test_slot_shape_ok(TEX_CACHE_TARGET_WII, ti, 1, 768, 768,
			(uint32_t)tex_gx_bytes(TEX_FMT_RGBA16, 768, 768))
		  && !tex_cache_test_slot_shape_ok(TEX_CACHE_TARGET_WII, ti,
			TEX_GX_ATLAS_LODS, 768, 768, 768 * 768 * 2 + 384 * 384 * 2)
		  && !tex_cache_test_slot_shape_ok(TEX_CACHE_TARGET_WII, ti,
			TEX_GX_ATLAS_LODS, 768, 768, 0),
		  "NPOT 768 cache accepts nearest and rejects legacy mip chain");
	assert(json_object_dotset_boolean(cfg, "render.mipmaps", 0) == JSONSuccess);

	/* T15: changing the setting invalidates both cache producers.
	 * Native particles exercise the chain; T9 covered the 128px fallback. */
	put_png("assets", "particles.png", 256, 256, 2);
	assert(json_object_dotset_boolean(cfg, "render.mipmaps", 1) == JSONSuccess);
	check(load() && status_is("written") && reason_is("content changed"),
		  "T15 enabling mips invalidates the default cache");
	uint64_t hm = state_hash();
	check(load() && status_is("hit") && state_hash() == hm,
		  "T15 enabled cache hit equals enabled fresh load");
	GLint min_filter = 0, max_level = 0;
	glBindTexture(GL_TEXTURE_2D, texture_terrain.id);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &max_level);
	check(min_filter == GL_LINEAR_MIPMAP_LINEAR && max_level == 1,
		  "T15 real GL restored terrain has isolated mip sampler");
	tex_cache_tool_targets(1u << TEX_CACHE_TARGET_WII);
	check(load() && status_is("written"), "T15 bake enabled Wii generation");
	pack_cache_records(DIR_TEX, TEX_CACHE_SCHEMA, r);
	pack_cache_order(r, order);
	gen_file(g, sizeof(g), DIR_TEX, r[order[0]].generation);
	gb = slurp(g, &gn);
	total
		= tex_gx_atlas_bytes(texture_particles.width, texture_particles.height);
	want = malloc(total);
	assert(want
		   && tex_atlas_grid_init(&grid, texture_particles.width,
								  texture_particles.height));
	assert(tex_gx_encode_atlas(texture_particles.data, texture_particles.width,
							   texture_particles.height, &grid, want));
	check(gb && gb[8] == TEX_CACHE_TARGET_WII && contains(gb, gn, want, total),
		  "T15 enabled Wii cache contains exact isolated GX chain");
	free(gb);
	free(want);
	put_png("assets", "particles.png", 128, 128, 2);
	assert(json_object_dotset_boolean(cfg, "render.mipmaps", 0) == JSONSuccess);
	check(load() && state_hash() == no_arrow,
		  "T15 disabling restores byte-identical default metadata and pixels");
	glBindTexture(GL_TEXTURE_2D, texture_terrain.id);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &max_level);
	check(min_filter == GL_NEAREST && max_level == 0,
		  "T15 disabling restores real GL nearest level-zero sampler");

	unsigned key_us, read_us, write_us;
	tex_cache_timings(&key_us, &read_us, &write_us);
	printf("last hit: source re-hash %u us, read %u us\n", key_us, read_us);

	glfwDestroyWindow(gl);
	glfwTerminate();
	/* Fable LOW-5: leave no per-PID fixture tree behind. */
	assert(CHDIR("..") == 0);
	test_fixture_dir_cleanup(s_fixture);
	printf("%s test_tex_pack_cache (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
