/*
	Beta leaves follow the graphics level (render.fancy_graphics).

	BlockLeaves.setGraphicsLevel [BlockLeaves.java:181-184] picks the base
	tile 52 (fancy, cut-out) or 53 (fast, opaque); spruce is +80 (132 / 133)
	[:177-179]. isOpaqueCube() = !graphicsLevel [:173-175], and
	BlockLeavesBase.shouldSideBeRendered [BlockLeavesBase.java:15-18] culls a
	face toward the same id only when fast. The round-2 "spruce leaf noise"
	was a hybrid of neither mode: the fancy cut-out tile with fast culling, so
	the holes looked straight through the canopy.

	PART 1 (assertions, production atlas + mesher path):
	  - fast (default): opaque *_FAST atlas cells; two adjacent leaves emit 10
	    faces; a leaf against stone hides and is hidden (5 + 5);
	  - fancy: the cut-out cells; two leaves emit 12 faces;
	  - a pack with no fast tile (the E&A default ships none) gets the fancy
	    tile with its holes filled opaque.

	PART 2 (measurement, stdout only): a taiga canopy (WorldGenTaiga2-shaped
	spruces) and an oak forest on a 2x2-chunk column set, meshed fast and
	fancy: vertices, display-list bytes (PC storage, plus the Wii GX estimate
	9 B untinted / 12 B tinted per vertex) and the worker's per-build wall ms
	(PC proxy; the Wii cost is far_walk_6m / perf_stress_5m on hardware).
*/
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "block/block_leaves.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "config.h"
#include "game/game_state.h"
#include "graphics/biome_color.h"
#include "graphics/texture_atlas.h"
#ifndef CAVEWII_PISTON_RENDER_TEST
#define CAVEWII_PISTON_RENDER_TEST
#endif
#include "graphics/render_piston_moving.h"
#include "network/piston_moving_transport.h"
#include "lodepng/lodepng.h"
#include "network/client_interface.h"
#include "platform/displaylist.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "support/test_fixture_dir.h"
#include "world.h"

#define HANG_TIMEOUT_MS 10000
#define IMG 256
#define TILE 16

static uint8_t g_img[IMG * IMG * 4];

static void build_synthetic_terrain(void) {
	for(int y = 0; y < IMG; y++)
		for(int x = 0; x < IMG; x++) {
			int tx = x / TILE, ty = y / TILE, u = x % TILE, v = y % TILE;
			uint8_t* p = g_img + (y * IMG + x) * 4;
			p[0] = (uint8_t)(tx * 16 + u);
			p[1] = (uint8_t)(ty * 16 + v);
			p[2] = (uint8_t)((tx * 37 + ty * 11 + u * 3 + v * 5) & 0xFF);
			p[3] = ((u + v + tx) % 7 == 0) ? 0 : 255;
		}
}

static uint8_t* make_colormap(uint32_t seed) {
	uint8_t* m = mem2_alloc(256 * 256 * 4);
	assert(m);
	for(int i = 0; i < 256 * 256; i++) {
		seed = seed * 1103515245u + 12345u;
		m[i * 4 + 0] = (uint8_t)(seed >> 8);
		m[i * 4 + 1] = (uint8_t)(seed >> 16);
		m[i * 4 + 2] = (uint8_t)(seed >> 24);
		m[i * 4 + 3] = 255;
	}
	return m;
}

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static struct world w;

static bool wait_for_mesh(struct chunk* c) {
	ptime_t t0 = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		if(c->mesh_in_flight) {
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS)
				return false;
			thread_msleep(1);
		}
	}
	return true;
}

static bool mesh_chunk_and_wait(struct chunk* c) {
	c->dirty_generation++;
	c->mesh_in_flight = false;
	ptime_t t0 = time_get();
	while(!chunk_mesher_send(c)) {
		chunk_mesher_receive();
		thread_msleep(1);
		if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS)
			return false;
	}
	c->rebuild_displist = false;
	c->first_meshed = true;
	c->mesh_in_flight = true;
	return wait_for_mesh(c);
}

struct mesh_count {
	size_t verts, verts_tinted, pc_bytes;
};

static struct mesh_count count_chunk(struct chunk* c) {
	struct mesh_count m = {0};
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		const struct displaylist* l = c->mesh + k;
		if(!c->has_displist[k] || !l->data)
			continue;
		m.verts += l->index;
		if(l->tinted)
			m.verts_tinted += l->index;
		m.pc_bytes += l->length;
	}
	return m;
}

static void set_blk(int x, int y, int z, uint8_t type, uint8_t meta) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = type,
										 .metadata = meta,
										 .sky_light = 15,
										 .torch_light = 0},
					false);
}

static struct chunk* chunk_at(int cx, int cy, int cz) {
	struct chunk* c = world_find_chunk(&w, cx * 16, cy * 16, cz * 16);
	assert(c && "SETUP: chunk not resident");
	return c;
}

static void clear_box(int x0, int y0, int z0, int x1, int y1, int z1) {
	for(int y = y0; y <= y1; y++)
		for(int z = z0; z <= z1; z++)
			for(int x = x0; x <= x1; x++)
				set_blk(x, y, z, BLOCK_AIR, 0);
}

static size_t mesh_verts(int cx, int cy, int cz) {
	struct chunk* c = chunk_at(cx, cy, cz);
	assert(mesh_chunk_and_wait(c) && "mesher hang");
	return count_chunk(c).verts;
}

/* ---- Part 1 -------------------------------------------------------------- */

static void test_adjacent_leaves(void) {
	/* chunk (0,4,0): world y 64..79, all air but the probe blocks. */
	clear_box(0, 64, 0, 15, 79, 15);
	set_blk(5, 70, 5, BLOCK_LEAVES, 1); /* spruce */
	set_blk(6, 70, 5, BLOCK_LEAVES, 1);

	block_leaves_set_fancy(false);
	size_t fast = mesh_verts(0, 4, 0);
	block_leaves_set_fancy(true);
	size_t fancy = mesh_verts(0, 4, 0);
	printf("  two spruce leaves: fast %zu verts, fancy %zu verts\n", fast,
		   fancy);
	assert(fast == 10 * 4);	 /* shared pair culled */
	assert(fancy == 12 * 4); /* every face drawn */

	/* Mixed species: Beta compares the block ID only. */
	set_blk(6, 70, 5, BLOCK_LEAVES, 0);
	block_leaves_set_fancy(false);
	assert(mesh_verts(0, 4, 0) == 10 * 4);
	block_leaves_set_fancy(true);
	assert(mesh_verts(0, 4, 0) == 12 * 4);

	/* Leaf against stone [Block.java:236]: fast leaves are an opaque cube,
	 * so the pair hides both shared faces (5 + 5). Fancy keeps the stone
	 * face (leaves are not opaque) and -- a pre-existing over-draw, outside
	 * this fix -- also the leaf face Beta hides (12, Beta 11). */
	set_blk(6, 70, 5, BLOCK_STONE, 0);
	block_leaves_set_fancy(false);
	size_t ls_fast = mesh_verts(0, 4, 0);
	block_leaves_set_fancy(true);
	size_t ls_fancy = mesh_verts(0, 4, 0);
	printf("  leaf + stone: fast %zu, fancy %zu verts\n", ls_fast, ls_fancy);
	assert(ls_fast == 10 * 4);
	assert(ls_fancy == 12 * 4);
	clear_box(0, 64, 0, 15, 79, 15);
	block_leaves_set_fancy(false);
	puts("PASS leaves: fast culls like an opaque cube, fancy draws every "
		 "leaf-to-leaf face");
}

/* The opaque fast cells: each has its own atlas slot; with
 * `expect_fallback`, every texel must be opaque (the fancy cell's cut-out
 * texels filled). */
static void check_fast_cells(const uint8_t* atlas, size_t aw, size_t ah,
							 bool expect_fallback) {
	static const enum tex_atlas_entry fast[] = {TEXAT_LEAVES_OAK_FAST,
		TEXAT_LEAVES_SPRUCE_FAST, TEXAT_LEAVES_BIRCH_FAST};
	static const enum tex_atlas_entry fancy[] = {TEXAT_LEAVES_OAK,
		TEXAT_LEAVES_SPRUCE, TEXAT_LEAVES_BIRCH};
	for(int i = 0; i < 3; i++) {
		uint8_t slot = tex_atlas_lookup(fast[i]);
		assert(slot != tex_atlas_lookup(fancy[i]));
		float uv[4];
		assert(tex_atlas_tile_uv(aw, ah, slot, uv));
		size_t x0 = (size_t)(uv[0] * aw + 0.5F), y0 = (size_t)(uv[1] * ah + 0.5F);
		size_t x1 = (size_t)(uv[2] * aw + 0.5F), y1 = (size_t)(uv[3] * ah + 0.5F);
		assert(x1 > x0 && y1 > y0);
		size_t holes = 0;
		for(size_t y = y0; y < y1; y++)
			for(size_t x = x0; x < x1; x++)
				if(atlas[(y * aw + x) * 4 + 3] < 128)
					holes++;
		if(expect_fallback)
			assert(holes == 0 && "fallback must fill every cut-out texel");
	}
}

static float rd_f(const uint8_t* p) {
	float f;
	memcpy(&f, p, sizeof f);
	return f;
}

/* Atlas content rect (pixels) of an entry. */
static void cell_rect(size_t aw, size_t ah, enum tex_atlas_entry e,
					  size_t r[4]) {
	float uv[4];
	assert(tex_atlas_tile_uv(aw, ah, tex_atlas_lookup(e), uv));
	r[0] = (size_t)(uv[0] * aw + 0.5F);
	r[1] = (size_t)(uv[1] * ah + 0.5F);
	r[2] = (size_t)(uv[2] * aw + 0.5F);
	r[3] = (size_t)(uv[3] * ah + 0.5F);
}

/* Every vertex of a PC 25-byte (tinted) or 22-byte vertex list: tint at
 * byte 14 (tinted only), u/v floats after it. Checks the tint and that the
 * texel sampled lies in `cell`. */
static void check_list(const struct displaylist* l, size_t aw, size_t ah,
					   uint32_t want_tint, const size_t cell[4]) {
	size_t stride = l->tinted ? 25 : 22;
	for(size_t v = 0; v < l->index; v++) {
		const uint8_t* b = (const uint8_t*)l->data + v * stride;
		size_t off = 14;
		if(l->tinted) {
			assert(b[14] == ((want_tint >> 16) & 0xFF)
				   && b[15] == ((want_tint >> 8) & 0xFF)
				   && b[16] == (want_tint & 0xFF));
			off += 3;
		} else {
			assert(want_tint == 0xFFFFFFu);
		}
		float u = rd_f(b + off) * (float)aw, t = rd_f(b + off + 4) * (float)ah;
		assert(u >= (float)cell[0] - 0.01F && u <= (float)cell[2] + 0.01F);
		assert(t >= (float)cell[1] - 0.01F && t <= (float)cell[3] + 0.01F);
	}
}

/* BlockLeaves.colorMultiplier [BlockLeaves.java:19-29] checks meta & 1
 * FIRST (pine 0x619961), then meta & 2 (birch 0x80A755), else the biome
 * foliage colour; the spruce TILE only for (meta & 3) == 1 [:177-179].
 * Expected per face (fast and fancy):
 *   meta 1 -> baked spruce cell, no tint;  meta 2 -> baked birch, no tint;
 *   meta 3 -> RAW oak cell x pine;
 *   meta 0 -> RAW oak cell x biome foliage (tinted column),
 *             baked oak cell 0x48B518, no tint (column without a tint). */
static void test_leaf_colours(const uint8_t* atlas, size_t aw, size_t ah) {
	uint8_t gi[CHUNK_SIZE * CHUNK_SIZE], gj[CHUNK_SIZE * CHUNK_SIZE];
	memset(gi, 77, sizeof gi);
	memset(gj, 140, sizeof gj);
	clear_box(16, 64, 0, 31, 79, 15);
	world_set_section_tint(&w, 16, 0, gi, gj); /* chunk (1,*,0) tinted */
	uint32_t biome = biome_foliage_color(77, 140);
	assert(biome != 0xFFFFFFu);
	for(int fancy = 0; fancy < 2; fancy++) {
		block_leaves_set_fancy(fancy != 0);
		enum tex_atlas_entry raw
			= fancy ? TEXAT_LEAVES_OAK_RAW : TEXAT_LEAVES_OAK_FAST_RAW;
		struct {
			uint8_t meta;
			bool tinted_col;
			uint32_t tint;
			enum tex_atlas_entry cell;
		} cases[] = {
			{0, true, biome, raw},
			{8, true, biome, raw},
			{1, true, 0xFFFFFFu,
			 fancy ? TEXAT_LEAVES_SPRUCE : TEXAT_LEAVES_SPRUCE_FAST},
			{8 | 1, true, 0xFFFFFFu,
			 fancy ? TEXAT_LEAVES_SPRUCE : TEXAT_LEAVES_SPRUCE_FAST},
			{2, true, 0xFFFFFFu,
			 fancy ? TEXAT_LEAVES_BIRCH : TEXAT_LEAVES_BIRCH_FAST},
			{3, true, BLOCK_LEAVES_PINE_RGB, raw},
			{0, false, 0xFFFFFFu,
			 fancy ? TEXAT_LEAVES_OAK : TEXAT_LEAVES_OAK_FAST},
			{3, false, BLOCK_LEAVES_PINE_RGB, raw},
		};
		for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			/* "No foliage colour": the pack has no foliage colormap (the
			 * mesher's foliage_known false), same column. */
			if(!cases[i].tinted_col)
				biome_color_import(make_colormap(0x1234u), 256, 256, NULL, 0,
								   0);
			int bx = 21, bz = 5, cx = 1, cz = 0;
			set_blk(bx, 70, bz, BLOCK_LEAVES, cases[i].meta);
			struct chunk* c = chunk_at(cx, 4, cz);
			assert(mesh_chunk_and_wait(c));
			struct mesh_count m = count_chunk(c);
			assert(m.verts == 6 * 4);
			printf("  colour case fancy=%d meta=%u col=%s: verts %zu tinted %zu\n",
				   fancy, cases[i].meta, cases[i].tinted_col ? "tinted" : "plain",
				   m.verts, m.verts_tinted);
			assert((m.verts_tinted != 0) == (cases[i].tint != 0xFFFFFFu));
			size_t cell[4];
			cell_rect(aw, ah, cases[i].cell, cell);
			for(int k = 0; k < CHUNK_MESH_LISTS; k++)
				if(c->has_displist[k] && c->mesh[k].data && c->mesh[k].index)
					check_list(c->mesh + k, aw, ah, cases[i].tint, cell);
			set_blk(bx, 70, bz, BLOCK_AIR, 0);
			if(!cases[i].tinted_col)
				biome_color_import(make_colormap(0x1234u), 256, 256,
								   make_colormap(0x9876u), 256, 256);
		}
	}
	block_leaves_set_fancy(false);
	/* The baked / raw cells: texel = source x colour / 255 (raw: x 1). */
	static const struct {
		enum tex_atlas_entry e;
		int tx, ty;
		uint32_t rgb;
	} cells[] = {{TEXAT_LEAVES_SPRUCE_FAST, 5, 8, 0x619961u},
		{TEXAT_LEAVES_SPRUCE, 4, 8, 0x619961u},
		{TEXAT_LEAVES_BIRCH_FAST, 5, 3, 0x80A755u},
		{TEXAT_LEAVES_BIRCH, 4, 3, 0x80A755u},
		{TEXAT_LEAVES_OAK, 4, 3, 0x48B518u},
		{TEXAT_LEAVES_OAK_FAST, 5, 3, 0x48B518u},
		{TEXAT_LEAVES_OAK_RAW, 4, 3, 0xFFFFFFu},
		{TEXAT_LEAVES_OAK_FAST_RAW, 5, 3, 0xFFFFFFu}};
	for(size_t i = 0; i < sizeof cells / sizeof cells[0]; i++) {
		size_t r[4];
		cell_rect(aw, ah, cells[i].e, r);
		const uint8_t* got = atlas + (r[1] * aw + r[0]) * 4;
		const uint8_t* src = g_img
			+ ((size_t)(cells[i].ty * TILE) * IMG + (size_t)cells[i].tx * TILE)
				* 4;
		assert(src[3] == 255); /* texel (0,0) of these cells is opaque */
		for(int k = 0; k < 3; k++) {
			unsigned col = (cells[i].rgb >> (16 - 8 * k)) & 0xFF;
			assert(got[k] == src[k] * col / 255);
		}
	}
	puts("PASS leaf colours: spruce/birch fixed, meta 3 pine on the oak tile, "
		 "oak = raw tile x biome (baked 0x48B518 without a tint)");
}

void render_piston_moving_test_world_access(
	struct world* world, struct piston_moving_render_access* out);

/* The piston-moving renderer applies the same rule (block_leaves_face_tint)
 * to a pushed leaf block. */
static void test_moving_leaf_colours(size_t aw, size_t ah) {
	struct piston_moving_render_access acc;
	render_piston_moving_test_world_access(&w, &acc);
	uint32_t biome = biome_foliage_color(77, 140);
	struct {
		uint8_t meta;
		uint32_t tint;
		enum tex_atlas_entry cell;
	} cases[] = {{0, biome, TEXAT_LEAVES_OAK_FAST_RAW},
		{1, 0xFFFFFFu, TEXAT_LEAVES_SPRUCE_FAST},
		{2, 0xFFFFFFu, TEXAT_LEAVES_BIRCH_FAST},
		{3, BLOCK_LEAVES_PINE_RGB, TEXAT_LEAVES_OAK_FAST_RAW}};
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		set_blk(21, 70, 5, BLOCK_PISTON_MOVING, 0);
		struct piston_moving_client_entry e = {
			.x = 21, .y = 70, .z = 5,
			.state = {.stored_id = BLOCK_LEAVES, .stored_meta = cases[i].meta,
					  .facing = 5, .flags = PISTON_MOVING_FLAG_EXTENDING,
					  .current_progress = 0.25F, .last_progress = 0.25F}};
		struct displaylist dl;
		displaylist_init(&dl, 64, 12);
		size_t q = render_piston_moving_test_emit(&dl, &e, 0.0F, &acc, false);
		printf("  moving leaf meta %u: %zu quads, %zu bytes\n", cases[i].meta, q,
			   dl.index);
		/* An immediate list: index counts BYTES (25 per tinted vertex). */
		assert(q == 6 && dl.tinted && dl.index == q * 4 * 25);
		size_t cell[4];
		cell_rect(aw, ah, cases[i].cell, cell);
		/* Every face is emitted into the one tinted list; white = no tint. */
		for(size_t v = 0; v < dl.index / 25; v++) {
			const uint8_t* b = (const uint8_t*)dl.data + v * 25;
			assert(b[14] == ((cases[i].tint >> 16) & 0xFF)
				   && b[15] == ((cases[i].tint >> 8) & 0xFF)
				   && b[16] == (cases[i].tint & 0xFF));
			float u = rd_f(b + 17) * (float)aw, t = rd_f(b + 21) * (float)ah;
			assert(u >= (float)cell[0] - 0.01F && u <= (float)cell[2] + 0.01F);
			assert(t >= (float)cell[1] - 0.01F && t <= (float)cell[3] + 0.01F);
		}
		displaylist_destroy(&dl);
		set_blk(21, 70, 5, BLOCK_AIR, 0);
	}
	puts("PASS pushed leaves keep the mesher's colour rule (piston-moving "
		 "renderer)");
}

static void test_fast_tile_fallback(const char* fixture) {
	/* A pack WITHOUT fast tiles (E&A-shaped): cells (5,3) and (5,8) empty. */
	static uint8_t img2[IMG * IMG * 4];
	memcpy(img2, g_img, sizeof img2);
	static const int cells[][2] = {{5, 3}, {5, 8}};
	for(int c = 0; c < 2; c++)
		for(int v = 0; v < TILE; v++)
			memset(img2 + (((size_t)(cells[c][1] * TILE + v) * IMG)
						   + (size_t)cells[c][0] * TILE) * 4,
				   0, TILE * 4);
	char path[256];
	snprintf(path, sizeof path, "%s/terrain.png", fixture);
	assert(!lodepng_encode32_file(path, img2, IMG, IMG));
	size_t aw = 0, ah = 0;
	uint8_t* atlas = tex_atlas_block("terrain.png", &aw, &ah);
	assert(atlas);
	check_fast_cells(atlas, aw, ah, true);
	free(atlas);
	/* Restore the full pack for the mesher scenes. */
	assert(!lodepng_encode32_file(path, g_img, IMG, IMG));
	atlas = tex_atlas_block("terrain.png", &aw, &ah);
	assert(atlas);
	free(atlas);
	puts("PASS fast-leaf tile fallback: a pack without tile 53/133 gets the "
		 "fancy tile with its holes filled opaque");
}

/* ---- Part 2 -------------------------------------------------------------- */

/* A 2x2-chunk column set, world x/z 32..63, sections y 4..5 (64..95). */
#define SCENE_CX 2
#define SCENE_CZ 2

static void ground(void) {
	for(int z = SCENE_CZ * 16; z < SCENE_CZ * 16 + 32; z++)
		for(int x = SCENE_CX * 16; x < SCENE_CX * 16 + 32; x++) {
			for(int y = 64; y < 96; y++)
				set_blk(x, y, z, BLOCK_AIR, 0);
			set_blk(x, 64, z, BLOCK_DIRT, 0);
			set_blk(x, 65, z, BLOCK_GRASS, 0);
		}
}

/* WorldGenTaiga2's shape: trunk, then a cone whose radius grows 0,1,2 and
 * resets to 0..1 every few layers (here a fixed, deterministic cycle). */
static void spruce(int x, int z, int height) {
	int base = 66;
	for(int y = 0; y < height; y++)
		set_blk(x, base + y, z, BLOCK_LOG, 1);
	static const int radii[] = {0, 1, 1, 2, 1, 2, 2, 1, 2};
	int top = base + height;
	for(int i = 0; i < (int)(sizeof radii / sizeof radii[0]); i++) {
		int y = top - i, r = radii[i];
		for(int dz = -r; dz <= r; dz++)
			for(int dx = -r; dx <= r; dx++) {
				if(r > 0 && abs(dx) == r && abs(dz) == r)
					continue; /* corners clipped, as Beta's |dx|!=r|||dz|!=r */
				if(dx == 0 && dz == 0 && y < top)
					continue; /* the trunk */
				set_blk(x + dx, y, z + dz, BLOCK_LEAVES, 1);
			}
	}
}

/* WorldGenTrees' shape: two 5x5 layers and two 3x3 layers over a 5 trunk. */
static void oak(int x, int z) {
	int base = 66, h = 5;
	for(int y = 0; y < h; y++)
		set_blk(x, base + y, z, BLOCK_LOG, 0);
	for(int y = base + h - 3; y <= base + h; y++) {
		int r = (y >= base + h - 1) ? 1 : 2;
		for(int dz = -r; dz <= r; dz++)
			for(int dx = -r; dx <= r; dx++) {
				if(dx == 0 && dz == 0 && y < base + h)
					continue;
				set_blk(x + dx, y, z + dz, BLOCK_LEAVES, 0);
			}
	}
}

struct scene_result {
	struct mesh_count m;
	double build_ms;
};

static struct scene_result measure(int reps) {
	struct scene_result r = {0};
	double ms = 0;
	int n = 0;
	for(int cx = SCENE_CX; cx < SCENE_CX + 2; cx++)
		for(int cz = SCENE_CZ; cz < SCENE_CZ + 2; cz++)
			for(int cy = 4; cy <= 5; cy++) {
				struct chunk* c = chunk_at(cx, cy, cz);
				for(int k = 0; k < reps; k++) {
					assert(mesh_chunk_and_wait(c));
					float last = 0;
					chunk_mesher_build_ms(&last, NULL);
					ms += last;
					n++;
				}
				struct mesh_count m = count_chunk(c);
				r.m.verts += m.verts;
				r.m.verts_tinted += m.verts_tinted;
				r.m.pc_bytes += m.pc_bytes;
			}
	r.build_ms = n ? ms / n : 0;
	return r;
}

static void report(const char* scene, int reps) {
	block_leaves_set_fancy(false);
	struct scene_result before = measure(reps);
	block_leaves_set_fancy(true);
	struct scene_result after = measure(reps);
	block_leaves_set_fancy(false);
	size_t wii_b = before.m.verts_tinted * 12
		+ (before.m.verts - before.m.verts_tinted) * 9;
	size_t wii_a = after.m.verts_tinted * 12
		+ (after.m.verts - after.m.verts_tinted) * 9;
	printf("MEASURE %-6s fast(default) verts %7zu  pc_bytes %8zu  wii_est %8zu  "
		   "build %.3f ms/chunk\n",
		   scene, before.m.verts, before.m.pc_bytes, wii_b, before.build_ms);
	printf("MEASURE %-6s fancy(opt-in) verts %7zu  pc_bytes %8zu  wii_est %8zu  "
		   "build %.3f ms/chunk\n",
		   scene, after.m.verts, after.m.pc_bytes, wii_a, after.build_ms);
	printf("MEASURE %-6s fancy-vs-fast verts %+.1f%%  pc_bytes %+.1f%%  wii_est "
		   "%+.1f%%  build %+.1f%%\n",
		   scene,
		   100.0 * ((double)after.m.verts / (double)before.m.verts - 1.0),
		   100.0 * ((double)after.m.pc_bytes / (double)before.m.pc_bytes - 1.0),
		   100.0 * ((double)wii_a / (double)wii_b - 1.0),
		   100.0 * (after.build_ms / before.build_ms - 1.0));
	/* Fancy only ADDS faces over fast. */
	assert(after.m.verts > before.m.verts);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	char fixture[160], path[256];
	test_fixture_dir("test_leaves_graphics_level", fixture, sizeof fixture);
#ifdef _WIN32
	_mkdir(fixture);
#else
	mkdir(fixture, 0755);
#endif
	build_synthetic_terrain();
	snprintf(path, sizeof path, "%s/terrain.png", fixture);
	assert(!lodepng_encode32_file(path, g_img, IMG, IMG));
	snprintf(path, sizeof path, "%s/config.json", fixture);
	FILE* cf = fopen(path, "wb");
	assert(cf);
	fputs("{}", cf);
	fclose(cf);
	assert(config_create(&gstate.config_user, path));
	assert(config_set_string(&gstate.config_user, "paths.texturepack", fixture));
	size_t aw = 0, ah = 0;
	uint8_t* atlas = tex_atlas_block("terrain.png", &aw, &ah);
	assert(atlas && "SETUP: atlas did not build");

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	world_create(&w);
	assert(chunk_mesher_init() && "SETUP: mesher worker did not start");
	biome_color_import(make_colormap(0x1234u), 256, 256, make_colormap(0x9876u),
					   256, 256);
	assert(!block_leaves_fancy() && "default must be Beta fast leaves");
	check_fast_cells(atlas, aw, ah, false);

	test_adjacent_leaves();
	test_leaf_colours(atlas, aw, ah);
	test_moving_leaf_colours(aw, ah);
	test_fast_tile_fallback(fixture);

	ground();
	for(int z = SCENE_CZ * 16 + 3; z < SCENE_CZ * 16 + 30; z += 5)
		for(int x = SCENE_CX * 16 + 3; x < SCENE_CX * 16 + 30; x += 5)
			spruce(x, z, 6 + ((x + z) % 4));
	report("taiga", 10);

	ground();
	for(int z = SCENE_CZ * 16 + 3; z < SCENE_CZ * 16 + 30; z += 6)
		for(int x = SCENE_CX * 16 + 3; x < SCENE_CX * 16 + 30; x += 6)
			oak(x, z);
	report("forest", 10);

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(fixture);
	puts("PASS test_leaves_graphics_level");
	return 0;
}
