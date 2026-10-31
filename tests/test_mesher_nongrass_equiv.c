/*
	DC-GRASS G2 T1 byte-exact proof (design-crit-1010, task G2 item 5).

	The grass fix (separate biome-tinted tile-38 side overlay, untinted grass
	atlas cells, fallback colour, Beta snow rules) must change GRASS output
	only. This test pins every NON-grass block's emitted chunk display-list
	bytes, and every non-grass atlas slot + cell texel, to a golden recorded
	from the pre-G2 base_ref sources (provenance file next to the golden):
	tests/fixtures/grass/mesher_nongrass_equiv.golden.

	PRODUCTION PATH (nothing copied): a synthetic terrain.png goes through the
	real tex_atlas_block (so atlas SLOT assignment -- which a mid-list atlas
	registration would shift for every later block -- is part of the proof),
	the real biome_color colormap lookup, and the real chunk_mesher worker
	(chunk_mesher_send -> worker -> chunk_mesher_receive). Every list of every
	meshed chunk is hashed (FNV-1a 64 over vertex count * stride bytes, plus
	the vertex count).

	SCENES (no grass block anywhere, except lod-grass below):
	  - sparse grid: every registered block id except grass, metadata 0..15,
	    one per even (x,y,z) cell so all faces are exposed (chunks x 0..3,
	    sections 4..5, biome-tinted columns);
	  - dense pseudo-random fill of the same ids with random metadata and
	    sky/torch light (occlusion, AO, connection-aware renderers), in a
	    tinted column and an UNtinted column (no section tint);
	  - the dense tinted chunk re-meshed with the colormap missing;
	  - lod-grass: a far-ring (lod != 0) skin mesh of stepped grass columns.
	    Every list's geometry bytes (positions, light, tint) are pinned, and
	    for every non-top list (the silhouette skirts) the 16x16 atlas texels
	    each quad samples are pinned too: G2 must keep the pre-G2 skirt look
	    (it now samples a different, texel-identical baked cell, so the UV
	    bytes alone may move). Top-list texels are deliberately NOT pinned --
	    the raw grass top cell is the G2 fix itself.
	INVARIANT checked in every non-grass scene: the TINTED double-sided list
	(CHUNK_MESH_PARTS + 12) is empty. chunk.c gives that list -- the G2 grass
	overlay list -- Beta's 0.1 alpha cut; this proves no non-grass geometry
	(e.g. leaves) is drawn under it.

	WHAT THIS DOES NOT PROVE: GPU draw state (alpha test/ref, blending, cull,
	tint enable) is not observable from the emitted bytes. Leaf draw state is
	unchanged by construction (chunk.c only brackets list PARTS + 12) and the
	invariant above shows leaves never land there; it is not measured here.
	render_item output is not covered. The G2 attempt-3 section below adds
	moving-grass (render_piston_moving) colour/overlay checks and textual
	checks of the draw-state lines; GPU execution itself stays unmeasured.

	Usage: test_mesher_nongrass_equiv <golden>                  (compare)
	       test_mesher_nongrass_equiv <golden> --record [note]  (write golden;
	       provenance lives in tests/fixtures/grass/
	       mesher_nongrass_equiv.provenance.txt, not in this binary)
*/

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
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
#include "block/piston_moving.h"
#include "graphics/biome_color.h"
#include "graphics/render_block.h"
#include "graphics/render_piston_moving.h"
#include "graphics/texture_atlas.h"
#include "lodepng/lodepng.h"
#include "network/piston_moving_transport.h"
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
/* enum tex_atlas_entry count at base_ref (its TEXAT_MAX), recorded in the
 * golden's "texat_max" line. Entries G2 appends after them are new and not
 * part of the golden. */
static int g_base_texat = TEXAT_MAX;

/* ---------------- synthetic pack ---------------- */

static uint8_t g_img[IMG * IMG * 4];

static void build_synthetic_terrain(void) {
	for(int y = 0; y < IMG; y++)
		for(int x = 0; x < IMG; x++) {
			int tx = x / TILE, ty = y / TILE, u = x % TILE, v = y % TILE;
			uint8_t* p = g_img + (y * IMG + x) * 4;
			p[0] = (uint8_t)(tx * 16 + u);
			p[1] = (uint8_t)(ty * 16 + v);
			p[2] = (uint8_t)((tx * 37 + ty * 11 + u * 3 + v * 5) & 0xFF);
			/* some cut-out texels so alpha-dependent paths see real data */
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

/* ---------------- harness (same shape as test_grass_render) ---------------- */

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

static uint64_t fnv64(uint64_t h, const void* data, size_t n) {
	const uint8_t* p = data;
	for(size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}
#define FNV64_INIT 1469598103934665603ull

/* ---------------- record / compare ---------------- */

#define MAX_LINES 4096
static char* g_lines[MAX_LINES];
static int g_nlines = 0;

static void emit(const char* fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	assert(g_nlines < MAX_LINES);
	g_lines[g_nlines] = malloc(strlen(buf) + 1);
	assert(g_lines[g_nlines]);
	strcpy(g_lines[g_nlines], buf);
	g_nlines++;
}

/* The G2 grass-overlay list must stay empty for every non-grass block. */
static int g_invariant_bad = 0;
static void check_overlay_list_empty(const char* scene, struct chunk* c) {
	int k = CHUNK_MESH_PARTS + 12;
	if(c->has_displist[k] && c->mesh[k].index) {
		printf("INVARIANT FAIL %s: tinted double-sided list %d has %zu "
			   "vertices (non-grass geometry under the grass-overlay alpha "
			   "state)\n",
			   scene, k, c->mesh[k].index);
		g_invariant_bad++;
	}
}

static void hash_chunk(const char* scene, struct chunk* c) {
	size_t total = 0;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		const struct displaylist* l = c->mesh + k;
		size_t n = (c->has_displist[k] && l->data) ? l->index : 0;
		size_t stride = l->tinted ? 25u : 22u;
		uint64_t h = n ? fnv64(FNV64_INIT, l->data, n * stride) : 0;
		total += n;
		emit("mesh %s %d %d %d list %d verts %zu fnv %016" PRIx64, scene,
			 (int)c->x, (int)c->y, (int)c->z, k, n, h);
	}
	emit("mesh %s %d %d %d total %zu", scene, (int)c->x, (int)c->y,
		 (int)c->z, total);
	check_overlay_list_empty(scene, c);
}

static float rd_f(const uint8_t* p) {
	float f;
	memcpy(&f, p, sizeof f);
	return f;
}

/* lod-grass: geometry (pos + light + tint) per list; for non-top lists also
 * the texels each quad samples (cell at the quad's min UV). */
static void hash_lod_chunk(const char* scene, struct chunk* c,
						   const uint8_t* atlas, size_t aw) {
	for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
		const struct displaylist* l = c->mesh + k;
		size_t n = (c->has_displist[k] && l->data) ? l->index : 0;
		size_t stride = l->tinted ? 25u : 22u;
		size_t geo_len = l->tinted ? 17u : 14u; /* pos + light (+ tint) */
		uint64_t g = FNV64_INIT, t = FNV64_INIT;
		int side = k % CHUNK_MESH_PARTS;
		bool top = side == SIDE_TOP || side == SIDE_TOP + 6;
		for(size_t q = 0; q + 3 < n; q += 4) {
			float umin = 1e9F, vmin = 1e9F;
			for(int j = 0; j < 4; j++) {
				const uint8_t* b = (const uint8_t*)l->data + (q + j) * stride;
				g = fnv64(g, b, geo_len);
				float u = rd_f(b + geo_len) * (float)aw;
				float v = rd_f(b + geo_len + 4) * (float)aw;
				if(u < umin)
					umin = u;
				if(v < vmin)
					vmin = v;
			}
			if(top)
				continue;
			int x0 = (int)(umin + 1e-3F), y0 = (int)(vmin + 1e-3F);
			if(x0 < 0 || y0 < 0 || x0 + TILE > (int)aw || y0 + TILE > (int)aw) {
				t = fnv64(t, "oob", 3);
				continue;
			}
			for(int v = 0; v < TILE; v++)
				t = fnv64(t, atlas + ((y0 + v) * aw + x0) * 4, TILE * 4);
		}
		emit("lod %s list %d verts %zu geom %016" PRIx64 " cells %016" PRIx64,
			 scene, k, n, n ? g : 0, (n && !top) ? t : 0);
	}
}

/* Non-grass atlas proof: every base enum entry keeps its slot; every entry
 * except the two grass cells G2 deliberately changes keeps its texels. */
static void hash_atlas(const uint8_t* atlas, size_t aw, size_t ah) {
	emit("texat_max %d", g_base_texat);
	assert(g_base_texat <= TEXAT_MAX);
	for(int e = 0; e < g_base_texat; e++) {
		uint8_t slot = tex_atlas_lookup((enum tex_atlas_entry)e);
		uint64_t h = 0;
		if(e != TEXAT_GRASS_TOP && e != TEXAT_GRASS_SIDE) {
			float uv[4];
			assert(tex_atlas_tile_uv(aw, ah, slot, uv));
			int x0 = (int)(uv[0] * aw + 0.5F), y0 = (int)(uv[1] * ah + 0.5F);
			h = FNV64_INIT;
			for(int v = 0; v < TILE; v++)
				h = fnv64(h, atlas + ((y0 + v) * aw + x0) * 4, TILE * 4);
		}
		emit("atlas %d slot %d fnv %016" PRIx64, e, slot, h);
	}
}

/* ---------------- scenes ---------------- */

static uint8_t g_ids[256];
static int g_nids = 0;

static void set_blk(w_coord_t x, w_coord_t y, w_coord_t z, uint8_t type,
					uint8_t meta, uint8_t sky, uint8_t torch) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = type,
										 .metadata = meta,
										 .sky_light = sky,
										 .torch_light = torch},
					false);
}

static uint32_t g_rng = 0xC0FFEEu;
static uint32_t rnd(void) {
	g_rng = g_rng * 1664525u + 1013904223u;
	return g_rng >> 8;
}

static void fill_dense(int cx, int cy, int cz) {
	for(int y = 0; y < CHUNK_SIZE; y++)
		for(int z = 0; z < CHUNK_SIZE; z++)
			for(int x = 0; x < CHUNK_SIZE; x++) {
				uint32_t r = rnd();
				uint8_t type = (r % 100) < 45 ? BLOCK_AIR
											  : g_ids[(r >> 7) % g_nids];
				set_blk(cx * 16 + x, cy * 16 + y, cz * 16 + z, type,
						(uint8_t)(rnd() & 15), (uint8_t)(rnd() & 15),
						(uint8_t)(rnd() & 15));
			}
}

static void set_column_tint(int cx, int cz) {
	uint8_t gi[CHUNK_SIZE * CHUNK_SIZE], gj[CHUNK_SIZE * CHUNK_SIZE];
	for(int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; i++) {
		gi[i] = (uint8_t)rnd();
		gj[i] = (uint8_t)rnd();
	}
	world_set_section_tint(&w, cx * 16, cz * 16, gi, gj);
}

static struct chunk* chunk_at(int cx, int cy, int cz) {
	struct chunk* c = world_find_chunk(&w, cx * 16, cy * 16, cz * 16);
	assert(c && "SETUP: chunk not resident");
	return c;
}

static void make_dir(const char* path) {
#ifdef _WIN32
	assert(!_mkdir(path));
#else
	assert(!mkdir(path, 0755));
#endif
}

/* ======================================================================
	DC-GRASS G2 attempt-3 production-linked regressions. These emit NO
	golden lines (the golden above is unchanged); each failure is counted
	in g_g2_bad and fails the test.

	(1) overlay culling (chunk mesher + chunk.c): every eligible grass side
	    overlay lands in the tinted double-sided list (PARTS + 12) as an
	    exact copy of its base quad's corners IN THE SAME ORDER (same
	    winding), with the same facing convention as ordinary opaque cube
	    faces (stone), so under the opaque pass's back-face culling base and
	    overlay are front/back together for every camera: inside the cube,
	    outside on the face's side, outside opposite (a far face seen through
	    a transparent pack). Count: 4 vertices per visible eligible side, 0
	    for occluded or snowy sides. chunk.c's overlay draw sets no cull
	    mode and restores the alpha state (textual check).
	(2) overlay alpha comparison: production refs + the backends' own
	    comparison lines (resources/fragment.shader, pc/gfx.c,
	    wii/gfx.c), and the boundary outcomes they produce.
	(3) moving grass (render_piston_moving.c) through the PRODUCTION world
	    accessor: colour availability independent of RGB.
   ====================================================================== */

static int g_g2_bad = 0;
#define G2_CHECK(cond, ...)                                                    \
	do {                                                                       \
		if(!(cond)) {                                                          \
			printf("G2 FAIL (%s:%d): ", __FILE__, __LINE__);                   \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
			g_g2_bad++;                                                        \
		}                                                                      \
	} while(0)

struct g2_quad {
	float p[4][3];
	uint8_t tint[4][3];
	int cell_x, cell_y; /* atlas pixel of the quad's min UV */
};

/* Decode quad q of a PC display list with `stride`-byte vertices. */
static void g2_read_quad(const uint8_t* data, size_t stride, bool tinted,
						 size_t q, struct g2_quad* out) {
	float umin = 1e9F, vmin = 1e9F;
	for(int j = 0; j < 4; j++) {
		const uint8_t* b = data + (q * 4 + j) * stride;
		for(int k = 0; k < 3; k++)
			out->p[j][k] = rd_f(b + 4 * k);
		size_t off = 14;
		if(tinted) {
			memcpy(out->tint[j], b + 14, 3);
			off += 3;
		} else {
			memset(out->tint[j], 255, 3);
		}
		float u = rd_f(b + off) * 256.0F, v = rd_f(b + off + 4) * 256.0F;
		if(u < umin)
			umin = u;
		if(v < vmin)
			vmin = v;
	}
	out->cell_x = (int)(umin + 1e-3F);
	out->cell_y = (int)(vmin + 1e-3F);
}

static bool g2_quad_samples(const struct g2_quad* q, uint8_t slot) {
	return q->cell_x == TEX_OFFSET(TEXTURE_X(slot))
		&& q->cell_y == TEX_OFFSET(TEXTURE_Y(slot));
}

static bool g2_tint_all(const struct g2_quad* q, uint32_t rgb) {
	for(int j = 0; j < 4; j++)
		if(q->tint[j][0] != ((rgb >> 16) & 0xFF)
		   || q->tint[j][1] != ((rgb >> 8) & 0xFF)
		   || q->tint[j][2] != (rgb & 0xFF))
			return false;
	return true;
}

static void g2_normal(const struct g2_quad* q, float n[3]) {
	float a[3], b[3];
	for(int k = 0; k < 3; k++) {
		a[k] = q->p[1][k] - q->p[0][k];
		b[k] = q->p[2][k] - q->p[0][k];
	}
	n[0] = a[1] * b[2] - a[2] * b[1];
	n[1] = a[2] * b[0] - a[0] * b[2];
	n[2] = a[0] * b[1] - a[1] * b[0];
}

static float g2_dot_to(const struct g2_quad* q, const float n[3],
					   const float cam[3]) {
	return n[0] * (cam[0] - q->p[0][0]) + n[1] * (cam[1] - q->p[0][1])
		+ n[2] * (cam[2] - q->p[0][2]);
}

static void g2_centre(const struct g2_quad* q, float c[3]) {
	for(int k = 0; k < 3; k++)
		c[k] = (q->p[0][k] + q->p[1][k] + q->p[2][k] + q->p[3][k]) / 4.0F;
}

/* Sign of dot(normal, outward) of a full-cube face quad of the unit block
 * whose min corner is floor of the quad's own corner set: +1 or -1. */
static int g2_facing_sign(const struct g2_quad* q, const float blk_min[3]) {
	float n[3], c[3], o[3];
	g2_normal(q, n);
	g2_centre(q, c);
	for(int k = 0; k < 3; k++)
		o[k] = c[k] - (blk_min[k] + 0.5F);
	float d = n[0] * o[0] + n[1] * o[1] + n[2] * o[2];
	return d > 0 ? 1 : -1;
}

static bool g2_same_corners(const struct g2_quad* a, const struct g2_quad* b) {
	for(int j = 0; j < 4; j++)
		for(int k = 0; k < 3; k++)
			if(fabsf(a->p[j][k] - b->p[j][k]) > 1e-5F)
				return false;
	return true;
}

static bool g2_quad_in_block(const struct g2_quad* q, const float m[3]) {
	for(int j = 0; j < 4; j++)
		for(int k = 0; k < 3; k++)
			if(q->p[j][k] < m[k] - 1e-4F || q->p[j][k] > m[k] + 1 + 1e-4F)
				return false;
	return true;
}

/* Source tree root, derived at runtime from the golden path argument
 * (<root>/tests/fixtures/grass/<golden>) -- no compile-time seam. */
static char g_src_dir[400] = ".";
static void g2_set_src_dir(const char* golden) {
	snprintf(g_src_dir, sizeof g_src_dir, "%s", golden);
	/* CMake passes the golden with forward slashes */
	char* t = strstr(g_src_dir, "/tests/fixtures/grass/");
	if(t)
		*t = 0;
}

static char* g2_read_text(const char* rel) {
	char path[512];
	snprintf(path, sizeof path, "%s/%s", g_src_dir, rel);
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* s = malloc((size_t)n + 1);
	assert(s);
	size_t got = fread(s, 1, (size_t)n, f);
	s[got] = 0;
	fclose(f);
	return s;
}

/* Text of the function/brace block that starts at the first '{' at or after
 * `from` (balanced braces), or NULL. Caller frees. */
static char* g2_block_after(const char* from) {
	const char* o = strchr(from, '{');
	if(!o)
		return NULL;
	int depth = 0;
	const char* e = o;
	for(; *e; e++) {
		if(*e == '{')
			depth++;
		else if(*e == '}' && --depth == 0)
			break;
	}
	if(!*e)
		return NULL;
	size_t n = (size_t)(e - o + 1);
	char* s = malloc(n + 1);
	assert(s);
	memcpy(s, o, n);
	s[n] = 0;
	return s;
}

/* `a` occurs in `s`, then `b` after it, then `c` after that. */
static bool g2_in_order(const char* s, const char* a, const char* b,
						const char* c) {
	const char* pa = s ? strstr(s, a) : NULL;
	const char* pb = pa ? strstr(pa + strlen(a), b) : NULL;
	const char* pc = pb ? strstr(pb + strlen(b), c) : NULL;
	return pc != NULL;
}

/* ---------- (1) overlay culling / winding / count ---------- */

#define G2_CX 10
#define G2_Y 37 /* local y 5 in section cy 2 */

static void g2_check_overlay_culling(void) {
	/* G_A exposed (4 overlays), G_B with stone on +x (3), G_C snow layer
	 * above (0), G_D snow block above (0); a lone stone for the facing
	 * convention of ordinary opaque cube faces. */
	const int gx[4] = {2, 6, 10, 13};
	for(int i = 0; i < 4; i++)
		set_blk(G2_CX * 16 + gx[i], G2_Y, 2, BLOCK_GRASS, 0, 15, 0);
	set_blk(G2_CX * 16 + 7, G2_Y, 2, BLOCK_STONE, 0, 15, 0);
	set_blk(G2_CX * 16 + 10, G2_Y + 1, 2, BLOCK_SNOW, 0, 15, 0);
	set_blk(G2_CX * 16 + 13, G2_Y + 1, 2, BLOCK_SNOW_BLOCK, 0, 15, 0);
	set_blk(G2_CX * 16 + 2, G2_Y, 10, BLOCK_STONE, 0, 15, 0);
	set_column_tint(G2_CX, 0);
	struct chunk* c = chunk_at(G2_CX, 2, 0);
	assert(mesh_chunk_and_wait(c) && "SETUP: G2 culling mesh hung");

	const uint8_t ov_slot = tex_atlas_lookup(TEXAT_GRASS_SIDE_OVERLAY);
	const uint8_t side_slot = tex_atlas_lookup(TEXAT_GRASS_SIDE);
	int ov_list = CHUNK_MESH_PARTS + 12;
	const struct displaylist* ol = c->mesh + ov_list;
	size_t ov_quads = (c->has_displist[ov_list] && ol->data) ? ol->index / 4 : 0;
	G2_CHECK(ol->tinted, "overlay list %d is not a tinted list", ov_list);
	G2_CHECK(ov_quads == 7,
			 "overlay quads = %zu, want 7 (4 exposed + 3 with one occluded "
			 "side + 0 under snow layer + 0 under snow block)",
			 ov_quads);

	/* Facing convention of ordinary opaque cube faces (stone at local
	 * (2,5,10)) in the untinted side lists. */
	int stone_sign = 0, stone_faces = 0;
	float stone_min[3] = {2, 5, 10};
	for(int s = SIDE_LEFT; s <= SIDE_BACK; s++) {
		const struct displaylist* l = c->mesh + s;
		size_t nq = (c->has_displist[s] && l->data) ? l->index / 4 : 0;
		for(size_t q = 0; q < nq; q++) {
			struct g2_quad sq;
			g2_read_quad(l->data, 22, false, q, &sq);
			if(!g2_quad_in_block(&sq, stone_min))
				continue;
			int sg = g2_facing_sign(&sq, stone_min);
			if(!stone_faces)
				stone_sign = sg;
			G2_CHECK(sg == stone_sign, "stone faces disagree on winding");
			stone_faces++;
		}
	}
	G2_CHECK(stone_faces == 4, "stone side faces = %d, want 4", stone_faces);

	int per_block[4] = {0, 0, 0, 0}, sides_seen[4][6];
	memset(sides_seen, 0, sizeof sides_seen);
	for(size_t q = 0; q < ov_quads && q < 64; q++) {
		struct g2_quad oq;
		g2_read_quad(ol->data, 25, true, q, &oq);
		G2_CHECK(g2_quad_samples(&oq, ov_slot),
				 "overlay quad %zu does not sample the tile-38 cell", q);
		int bi = -1;
		float bmin[3] = {0, 5, 2};
		for(int i = 0; i < 4; i++) {
			bmin[0] = (float)gx[i];
			if(g2_quad_in_block(&oq, bmin)) {
				bi = i;
				break;
			}
		}
		G2_CHECK(bi == 0 || bi == 1, "overlay quad %zu on block %d (want an "
				 "unsnowed grass block)", q, bi);
		if(bi < 0)
			continue;
		bmin[0] = (float)gx[bi];
		per_block[bi]++;
		/* its base: same corners, same order, untinted side list */
		int base_side = -1;
		struct g2_quad bq;
		for(int s = SIDE_LEFT; s <= SIDE_BACK && base_side < 0; s++) {
			const struct displaylist* l = c->mesh + s;
			size_t nq = (c->has_displist[s] && l->data) ? l->index / 4 : 0;
			for(size_t k = 0; k < nq; k++) {
				g2_read_quad(l->data, 22, false, k, &bq);
				if(g2_same_corners(&bq, &oq)) {
					base_side = s;
					break;
				}
			}
		}
		G2_CHECK(base_side >= 0, "overlay quad %zu has no base quad with the "
				 "same corners in the same order (winding)", q);
		if(base_side < 0)
			continue;
		sides_seen[bi][base_side]++;
		G2_CHECK(g2_quad_samples(&bq, side_slot) && g2_tint_all(&bq, 0xFFFFFFu),
				 "base of overlay %zu is not the untinted tile-3 side", q);
		/* winding/facing: identical to ordinary cube faces */
		int sg = g2_facing_sign(&oq, bmin);
		G2_CHECK(sg == stone_sign, "overlay quad %zu winding (facing sign %d) "
				 "differs from opaque cube faces (%d): back-face culling would "
				 "hide it from outside", q, sg, stone_sign);
		/* camera classification: inside, own side, opposite side */
		float n[3], cen[3], cams[3][3];
		g2_normal(&oq, n);
		g2_centre(&oq, cen);
		for(int k = 0; k < 3; k++) {
			float out = cen[k] - (bmin[k] + 0.5F);
			cams[0][k] = bmin[k] + 0.5F;          /* inside the cube */
			cams[1][k] = bmin[k] + 0.5F + 4 * out; /* outside, face side */
			cams[2][k] = bmin[k] + 0.5F - 4 * out; /* outside, far side */
		}
		bool want_front[3] = {false, true, false};
		float bn[3];
		g2_normal(&bq, bn);
		for(int v = 0; v < 3; v++) {
			bool ofront = (g2_dot_to(&oq, n, cams[v]) > 0) == (stone_sign > 0);
			bool bfront = (g2_dot_to(&bq, bn, cams[v]) > 0) == (stone_sign > 0);
			G2_CHECK(ofront == bfront && ofront == want_front[v],
					 "overlay %zu view %d: front=%d base front=%d want %d", q,
					 v, ofront, bfront, want_front[v]);
		}
	}
	G2_CHECK(per_block[0] == 4 && per_block[1] == 3,
			 "overlays per block exposed=%d (want 4) occluded-side=%d (want 3)",
			 per_block[0], per_block[1]);
	for(int s = SIDE_LEFT; s <= SIDE_BACK; s++) {
		G2_CHECK(sides_seen[0][s] == 1, "exposed grass side %d overlays %d", s,
				 sides_seen[0][s]);
		G2_CHECK(sides_seen[1][s] == (s == SIDE_RIGHT ? 0 : 1),
				 "grass with stone on +x: side %d overlays %d", s,
				 sides_seen[1][s]);
	}
	/* snowy grass: sides sample tile 68, untinted, and have no overlay */
	const uint8_t snow_slot = tex_atlas_lookup(TEXAT_GRASS_SIDE_SNOW);
	for(int i = 2; i < 4; i++) {
		float bmin[3] = {(float)gx[i], 5, 2};
		int snowy = 0;
		for(int s = SIDE_LEFT; s <= SIDE_BACK; s++) {
			const struct displaylist* l = c->mesh + s;
			size_t nq = (c->has_displist[s] && l->data) ? l->index / 4 : 0;
			for(size_t k = 0; k < nq; k++) {
				struct g2_quad bq;
				g2_read_quad(l->data, 22, false, k, &bq);
				if(g2_quad_in_block(&bq, bmin) && g2_quad_samples(&bq, snow_slot))
					snowy++;
			}
		}
		G2_CHECK(snowy == 4, "snowy grass %d: %d tile-68 sides, want 4", i,
				 snowy);
	}

	/* count-only and emission agree for the overlay emitter */
	struct block_data gb = {.type = BLOCK_GRASS, .sky_light = 15};
	struct block_data nb[SIDE_MAX];
	memset(nb, 0, sizeof nb);
	struct block_info info = {.block = &gb, .neighbours = nb, .x = 1, .y = 1, .z = 1};
	uint8_t light[24];
	memset(light, 0xF0, sizeof light);
	struct displaylist dl;
	displaylist_init(&dl, 8, 12);
	displaylist_set_tinted(&dl, true);
	G2_CHECK(render_block_full_tex(NULL, &info, SIDE_LEFT, light, ov_slot, true)
				 == 1,
			 "count-only overlay != 1 quad");
	G2_CHECK(render_block_full_tex(&dl, &info, SIDE_LEFT, light, ov_slot, false)
					 == 1
				 && dl.index == 4 * 25,
			 "emitted overlay != 1 quad / 4 vertices (index %zu bytes)",
			 dl.index);
	displaylist_destroy(&dl);

	/* chunk.c: the overlay draw keeps the pass's back-face culling (sets no
	 * cull mode), applies the overlay alpha ref, restores the default test;
	 * the non-grass double-sided list keeps cull NONE/BACK bracketing. */
	char* chunk_c = g2_read_text("source/chunk.c");
	G2_CHECK(chunk_c != NULL, "cannot read source/chunk.c");
	const char* grp = chunk_c ? strstr(chunk_c, "if(grass_overlay) {") : NULL;
	char* ov_blk = grp ? g2_block_after(grp) : NULL;
	G2_CHECK(ov_blk != NULL, "chunk.c overlay draw block not found");
	if(ov_blk) {
		G2_CHECK(g2_in_order(ov_blk,
							 "gfx_alpha_test_ref(true, "
							 "RENDER_GRASS_OVERLAY_ALPHA_REF);",
							 "displaylist_render(c->mesh + base + 12);",
							 "gfx_alpha_test(true);"),
				 "chunk.c overlay draw: alpha ref -> render -> restore order");
		G2_CHECK(!strstr(ov_blk, "gfx_cull_func"),
				 "chunk.c overlay draw changes the cull mode (must stay BACK)");
		const char* els = strstr(grp, "} else");
		char* nb_blk = els ? g2_block_after(els) : NULL;
		G2_CHECK(nb_blk && g2_in_order(nb_blk, "gfx_cull_func(MODE_NONE);",
									   "displaylist_render(c->mesh + base + 12);",
									   "gfx_cull_func(MODE_BACK);"),
				 "chunk.c non-grass double-sided draw lost its cull bracket");
		free(nb_blk);
	}
	free(ov_blk);
	free(chunk_c);
}

/* ---------- (2) alpha comparison ---------- */

static bool g2_pc_pass(float a, float ref) {
	/* resources/fragment.shader: discard if gl_FragColor.a < alpha_ref */
	return !(a < ref);
}

static bool g2_wii_pass(uint8_t a8, float ref) {
	/* wii/gfx.c gfx_alpha_test_ref: GX_GREATER, (uint8_t)roundf(ref*255.0f) */
	uint8_t byte_ref = (uint8_t)roundf(ref * 255.0f);
	return a8 > byte_ref;
}

static void g2_check_alpha(void) {
	/* production refs (render_block.h) */
	float pc = RENDER_GRASS_OVERLAY_ALPHA_REF_PC;
	G2_CHECK(pc == nextafterf(0.1F, 1.0F), "PC ref %.9g != nextafterf(0.1F,1)",
			 pc);
	G2_CHECK(RENDER_GRASS_OVERLAY_ALPHA_BYTE_WII == 25, "Wii byte ref != 25");
	G2_CHECK((uint8_t)roundf(RENDER_GRASS_OVERLAY_ALPHA_REF_WII * 255.0f) == 25,
			 "Wii ref does not convert to GX byte 25");
	G2_CHECK(RENDER_GRASS_OVERLAY_ALPHA_REF == pc,
			 "this PC build does not use the PC ref");

	/* Beta: survive iff a > 0.1F (strict). PC, fractional (filtered) alpha. */
	const float fa[] = {0.0995F, nextafterf(0.1F, 0.0F), 0.1F,
						nextafterf(0.1F, 1.0F), 0.1005F, 25.0F / 255.0F,
						26.0F / 255.0F, 0.0F, 1.0F};
	for(size_t i = 0; i < sizeof fa / sizeof fa[0]; i++) {
		bool beta = fa[i] > 0.1F;
		G2_CHECK(g2_pc_pass(fa[i], pc) == beta,
				 "PC alpha %.9g: pass=%d, Beta (a > 0.1F) = %d", fa[i],
				 g2_pc_pass(fa[i], pc), beta);
	}
	G2_CHECK(!g2_pc_pass(0.0995F, pc), "PC: 0.0995 must fail");
	/* Wii: integer alpha > 25 == Beta's a/255 > 0.1 on every byte */
	for(int a = 0; a < 256; a++) {
		bool beta = (float)a / 255.0F > 0.1F;
		G2_CHECK(g2_wii_pass((uint8_t)a, RENDER_GRASS_OVERLAY_ALPHA_REF_WII)
					 == beta,
				 "Wii alpha %d: pass != Beta", a);
	}
	G2_CHECK(!g2_wii_pass(25, RENDER_GRASS_OVERLAY_ALPHA_REF_WII)
				 && g2_wii_pass(26, RENDER_GRASS_OVERLAY_ALPHA_REF_WII),
			 "Wii: 25 must fail, 26 must pass");
	/* the defaults restored afterwards would NOT be Beta's (proves the ref
	 * matters): PC default 0.0625 lets 0.0995 through; Wii GEQUAL 16. */
	G2_CHECK(g2_pc_pass(0.0995F, 0.0625F), "default PC ref sanity");

	/* The comparisons modelled above ARE the production lines. */
	char* sh = g2_read_text("resources/fragment.shader");
	G2_CHECK(sh && strstr(sh, "if(enable_alpha && gl_FragColor.a < alpha_ref)")
				 && strstr(sh, "discard;"),
			 "fragment.shader alpha comparison changed (model is `a < ref` "
			 "discard)");
	free(sh);
	char* pcg = g2_read_text("source/platform/pc/gfx.c");
	const char* f1 = pcg ? strstr(pcg, "void gfx_alpha_test_ref(bool enable, "
										"float ref) {")
						 : NULL;
	char* f1b = f1 ? g2_block_after(f1) : NULL;
	G2_CHECK(f1b && strstr(f1b, "glUniform1f(glGetUniformLocation(shader_prog, "
								"\"alpha_ref\"), ref);"),
			 "pc/gfx.c gfx_alpha_test_ref no longer uploads ref unchanged");
	const char* f2 = pcg ? strstr(pcg, "void gfx_alpha_test(bool enable) {")
						 : NULL;
	char* f2b = f2 ? g2_block_after(f2) : NULL;
	G2_CHECK(f2b && strstr(f2b, "GFX_ALPHA_REF_DEFAULT"),
			 "pc/gfx.c gfx_alpha_test no longer re-uploads the default ref "
			 "(restoration)");
	free(f1b);
	free(f2b);
	free(pcg);
	char* wg = g2_read_text("source/platform/wii/gfx.c");
	const char* w1 = wg ? strstr(wg, "void gfx_alpha_test_ref(bool enable, "
									 "float ref) {")
						: NULL;
	char* w1b = w1 ? g2_block_after(w1) : NULL;
	G2_CHECK(w1b && strstr(w1b, "(uint8_t)roundf(ref * 255.0f)")
				 && strstr(w1b, "GX_SetAlphaCompare(GX_GREATER, byte_ref"),
			 "wii/gfx.c gfx_alpha_test_ref conversion/compare changed");
	const char* w2 = wg ? strstr(wg, "void gfx_alpha_test(bool enable) {")
						: NULL;
	char* w2b = w2 ? g2_block_after(w2) : NULL;
	G2_CHECK(w2b && strstr(w2b, "GX_SetAlphaCompare(GX_GEQUAL, 16"),
			 "wii/gfx.c gfx_alpha_test(true) no longer restores GEQUAL 16");
	free(w1b);
	free(w2b);
	free(wg);
	/* restoration at both production call sites */
	char* pm = g2_read_text("source/graphics/render_piston_moving.c");
	G2_CHECK(pm && g2_in_order(pm,
							   "gfx_alpha_test_ref(true, "
							   "RENDER_GRASS_OVERLAY_ALPHA_REF);",
							   "displaylist_render_immediate(",
							   "gfx_alpha_test(true);"),
			 "render_piston_moving.c: alpha ref -> draw -> restore order");
	free(pm);
}

/* ---------- (3) moving grass colour availability ---------- */

void render_piston_moving_test_world_access(
	struct world* world, struct piston_moving_render_access* out);

#define G2_PX 12 /* chunk column with section tint */
#define G2_PX_NOTINT 14 /* column whose section tint is never set */

static void g2_emit_moving_at(int wx, int wy, int wz, uint8_t above,
							  bool place,
							  struct piston_moving_render_access* acc,
							  uint32_t want_col, bool want_overlays,
							  const char* label) {
	if(place) {
		set_blk(wx, wy, wz, BLOCK_PISTON_MOVING, 0, 15, 0);
		set_blk(wx, wy + 1, wz, above, 0, 15, 0);
	}
	struct piston_moving_client_entry e = {
		.x = wx, .y = wy, .z = wz,
		.state = {.stored_id = BLOCK_GRASS, .stored_meta = 0, .facing = 5,
				  .flags = PISTON_MOVING_FLAG_EXTENDING,
				  .current_progress = 0.25F, .last_progress = 0.25F}};
	size_t want_q = want_overlays ? 10 : 6;
	size_t cq = render_piston_moving_test_emit(NULL, &e, 0.0F, acc, true);
	struct displaylist dl;
	displaylist_init(&dl, 64, 12);
	size_t eq = render_piston_moving_test_emit(&dl, &e, 0.0F, acc, false);
	G2_CHECK(cq == want_q && eq == want_q && dl.index == eq * 4 * 25,
			 "%s: count-only %zu / emitted %zu quads (%zu bytes), want %zu",
			 label, cq, eq, dl.index, want_q);
	uint8_t top = tex_atlas_lookup(TEXAT_GRASS_TOP);
	uint8_t side = tex_atlas_lookup(TEXAT_GRASS_SIDE);
	uint8_t snow = tex_atlas_lookup(TEXAT_GRASS_SIDE_SNOW);
	uint8_t ov = tex_atlas_lookup(TEXAT_GRASS_SIDE_OVERLAY);
	size_t nq = dl.index / (4 * 25);
	int overlays = 0, tops = 0;
	for(size_t q = 0; q < nq && q < 16; q++) {
		struct g2_quad mq;
		g2_read_quad(dl.data, 25, true, q, &mq);
		if(g2_quad_samples(&mq, top)) {
			tops++;
			G2_CHECK(g2_tint_all(&mq, want_col), "%s: top tint (%d,%d,%d) "
					 "want %06x", label, mq.tint[0][0], mq.tint[0][1],
					 mq.tint[0][2], (unsigned)want_col);
		} else if(g2_quad_samples(&mq, ov)) {
			overlays++;
			G2_CHECK(g2_tint_all(&mq, want_col), "%s: overlay tint (%d,%d,%d) "
					 "want %06x", label, mq.tint[0][0], mq.tint[0][1],
					 mq.tint[0][2], (unsigned)want_col);
			struct g2_quad prev;
			memset(&prev, 0, sizeof prev);
			if(q > 0)
				g2_read_quad(dl.data, 25, true, q - 1, &prev);
			G2_CHECK(q > 0 && g2_quad_samples(&prev, side)
						 && g2_tint_all(&prev, 0xFFFFFFu)
						 && g2_same_corners(&prev, &mq),
					 "%s: overlay %zu not right after its untinted tile-3 "
					 "base with the same corners", label, q);
		} else if(g2_quad_samples(&mq, side) || g2_quad_samples(&mq, snow)) {
			G2_CHECK(g2_tint_all(&mq, 0xFFFFFFu), "%s: side base tinted",
					 label);
			G2_CHECK(want_overlays == g2_quad_samples(&mq, side),
					 "%s: side cell is %s", label,
					 want_overlays ? "snowy" : "plain tile 3 under snow");
		}
	}
	G2_CHECK(tops == 1 && overlays == (want_overlays ? 4 : 0),
			 "%s: tops %d overlays %d", label, tops, overlays);
	displaylist_destroy(&dl);
}

#define g2_emit_moving(x, y, z, above, acc, col, ov, label) g2_emit_moving_at(x, y, z, above, true, acc, col, ov, label)

static void g2_check_moving_grass(void) {
	/* colormap: (ci,cj) = (1,1) white, (2,2) nonwhite */
	uint8_t* m = make_colormap(0x5555u);
	uint8_t* wp = m + (1 * 256 + 1) * 4;
	wp[0] = wp[1] = wp[2] = 255;
	uint8_t* g = m + (2 * 256 + 2) * 4;
	g[0] = 30;
	g[1] = 140;
	g[2] = 90;
	biome_color_import(m, 256, 256, make_colormap(0x7777u), 256, 256);
	uint8_t gi[CHUNK_SIZE * CHUNK_SIZE], gj[CHUNK_SIZE * CHUNK_SIZE];
	memset(gi, 2, sizeof gi);
	memset(gj, 2, sizeof gj);
	gi[3 * CHUNK_SIZE + 3] = gj[3 * CHUNK_SIZE + 3] = 1; /* column (3,3) white */
	/* the section must be resident before its tint can be stored */
	set_blk(G2_PX * 16 + 1, 40, 1, BLOCK_STONE, 0, 15, 0);
	world_set_section_tint(&w, G2_PX * 16, 0, gi, gj);
	uint8_t ci, cj;
	G2_CHECK(world_get_section_tint_at(&w, G2_PX * 16 + 3, 3, &ci, &cj)
				 && ci == 1 && cj == 1,
			 "SETUP: section tint for the moving-grass column not stored");
	/* make the untinted column's section resident without a tint store:
	 * a block there creates it only if world_set_block does; check it */
	set_blk(G2_PX_NOTINT * 16 + 5, 40, 5, BLOCK_STONE, 0, 15, 0);
	uint8_t ti, tj;
	bool notint_has = world_get_section_tint_at(&w, G2_PX_NOTINT * 16 + 5, 5,
												&ti, &tj);

	struct piston_moving_render_access acc;
	render_piston_moving_test_world_access(&w, &acc);
	const uint32_t green = (30u << 16) | (140u << 8) | 90u;

	g2_emit_moving(G2_PX * 16 + 5, 40, 5, BLOCK_AIR, &acc, green, true,
				   "moving nonwhite");
	g2_emit_moving(G2_PX * 16 + 3, 40, 3, BLOCK_AIR, &acc, 0xFFFFFFu, true,
				   "moving valid WHITE (stays white)");
	g2_emit_moving(G2_PX * 16 + 7, 40, 7, BLOCK_SNOW, &acc, green, false,
				   "moving under snow layer");
	g2_emit_moving(G2_PX * 16 + 9, 40, 9, BLOCK_SNOW_BLOCK, &acc, green, false,
				   "moving under snow block");
	/* A resident section always carries a tint store (the mesher then has
	 * has_tint == true too), so "missing section tint" = no section: a column
	 * nothing was ever written to (g2_emit_moving's place == false). */
	G2_CHECK(notint_has, "SETUP: resident column reports no section tint");
	uint8_t ui, uj;
	G2_CHECK(!world_get_section_tint_at(&w, 40 * 16 + 7, 7, &ui, &uj),
			 "SETUP: column 40 unexpectedly resident");
	g2_emit_moving_at(40 * 16 + 7, 40, 7, BLOCK_AIR, false, &acc,
					  BIOME_GRASS_FALLBACK, true,
					  "moving, missing section tint -> fallback");
	/* missing colormap, section tint present */
	biome_color_import(NULL, 0, 0, NULL, 0, 0);
	g2_emit_moving(G2_PX * 16 + 3, 40, 3, BLOCK_AIR, &acc, BIOME_GRASS_FALLBACK,
				   true, "moving, missing colormap -> fallback");
	g2_emit_moving(G2_PX * 16 + 11, 40, 11, BLOCK_SNOW, &acc,
				   BIOME_GRASS_FALLBACK, false,
				   "moving, missing colormap, snow layer");
}

static int g2_run_checks(void) {
	g2_check_overlay_culling();
	g2_check_alpha();
	g2_check_moving_grass();
	printf("G2 checks: %s (%d failure(s))\n", g_g2_bad ? "FAIL" : "OK",
		   g_g2_bad);
	return g_g2_bad;
}

static bool read_golden(const char* path, char*** out, int* n) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	static char* lines[MAX_LINES];
	int k = 0;
	char buf[512];
	while(fgets(buf, sizeof buf, f)) {
		size_t len = strlen(buf);
		while(len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			buf[--len] = 0;
		if(!len || buf[0] == '#')
			continue;
		assert(k < MAX_LINES);
		lines[k] = malloc(len + 1);
		strcpy(lines[k], buf);
		k++;
	}
	fclose(f);
	*out = lines;
	*n = k;
	return true;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc < 2) {
		fprintf(stderr, "usage: %s <golden> [--record]\n", argv[0]);
		return 2;
	}
	bool record = argc > 2 && !strcmp(argv[2], "--record");
	g2_set_src_dir(argv[1]);
	char** gold = NULL;
	int ngold = 0;
	if(!record) {
		if(!read_golden(argv[1], &gold, &ngold)) {
			printf("FAIL: cannot read golden %s\n", argv[1]);
			return 1;
		}
		for(int i = 0; i < ngold; i++)
			if(sscanf(gold[i], "texat_max %d", &g_base_texat) == 1)
				break;
	}

	char fixture[160], path[256];
	test_fixture_dir("test_mesher_nongrass_equiv", fixture, sizeof fixture);
	make_dir(fixture);
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
	assert(atlas && aw == IMG && ah == IMG && "SETUP: atlas did not build");

	/* Leaves mesh in the default Beta FAST mode (block_leaves.h): opaque
	 * tile 53/133 and culled like an opaque cube. Re-recorded 2026-10-30
	 * (spruce-leaf-1030) for that change; every non-leaf block is unchanged. */
	block_leaves_set_fancy(false);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	world_create(&w);
	assert(chunk_mesher_init() && "SETUP: mesher worker did not start");
	biome_color_import(make_colormap(0x1234u), 256, 256, make_colormap(0x9876u),
					   256, 256);

	for(int id = 1; id < 256; id++)
		if(blocks[id] && id != BLOCK_GRASS)
			g_ids[g_nids++] = (uint8_t)id;
	printf("SETUP: %d non-grass block ids\n", g_nids);

	hash_atlas(atlas, aw, ah);

	/* sparse grid: chunks x 0..3, sections 4..5 (z chunk 0) */
	int slot = 0;
	for(int i = 0; i < g_nids; i++)
		for(int meta = 0; meta < 16; meta++, slot++) {
			int cell = slot % 512, chunk = slot / 512;
			assert(chunk < 8 && "SETUP: sparse grid out of chunks");
			int cx = chunk % 4, cy = 4 + chunk / 4;
			int x = (cell % 8) * 2, z = ((cell / 8) % 8) * 2, y = (cell / 64) * 2;
			set_blk(cx * 16 + x, cy * 16 + y, z, g_ids[i], (uint8_t)meta, 15,
					(uint8_t)(meta & 7));
		}
	int sparse_chunks = (slot + 511) / 512;
	/* dense: tinted column (cx 0, cy 2) and untinted column (cx 6, cy 2) */
	fill_dense(0, 2, 0);
	fill_dense(6, 2, 0);
	for(int cx = 0; cx < 4; cx++)
		set_column_tint(cx, 0);

	for(int k = 0; k < sparse_chunks; k++) {
		struct chunk* c = chunk_at(k % 4, 4 + k / 4, 0);
		assert(mesh_chunk_and_wait(c) && "SETUP: mesh build hung");
		hash_chunk("sparse", c);
	}
	struct chunk* dt = chunk_at(0, 2, 0);
	assert(mesh_chunk_and_wait(dt) && "SETUP: mesh build hung");
	hash_chunk("dense-tinted", dt);
	/* lod-grass: stepped grass-on-dirt columns, tinted column cx 8 */
	set_column_tint(8, 0);
	for(int z = 0; z < CHUNK_SIZE; z++)
		for(int x = 0; x < CHUNK_SIZE; x++) {
			int h = 2 + (x * 3 + z * 5) % 9;
			for(int y = 0; y < h; y++)
				set_blk(8 * 16 + x, 32 + y, z, BLOCK_DIRT, 0, 0, 0);
			set_blk(8 * 16 + x, 32 + h, z, BLOCK_GRASS, 0, 15, 0);
		}

	struct chunk* du = chunk_at(6, 2, 0);
	assert(mesh_chunk_and_wait(du) && "SETUP: mesh build hung");
	hash_chunk("dense-untinted", du);
	biome_color_import(NULL, 0, 0, NULL, 0, 0);
	assert(mesh_chunk_and_wait(dt) && "SETUP: mesh build hung");
	hash_chunk("dense-no-colormap", dt);
	/* colormap back for the lod-grass scene (tinted tops, as in play) */
	biome_color_import(make_colormap(0x1234u), 256, 256, make_colormap(0x9876u),
					   256, 256);
	struct chunk* lg = chunk_at(8, 2, 0);
	lg->lod = 1;
	assert(mesh_chunk_and_wait(lg) && "SETUP: lod mesh build hung");
	hash_lod_chunk("grass-skirts", lg, atlas, aw);

	/* DC-GRASS G2 attempt-3 regressions (no golden lines) */
	int g2_bad = g2_run_checks();

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);
	free(atlas);
	config_destroy(&gstate.config_user);
	test_fixture_dir_cleanup(fixture);

	if(record) {
		FILE* f = fopen(argv[1], "wb");
		assert(f && "cannot write golden");
		fprintf(f, "# test_mesher_nongrass_equiv golden: non-grass chunk "
				   "display lists, atlas slots/cells, lod grass skirts\n"
				   "# provenance: tests/fixtures/grass/"
				   "mesher_nongrass_equiv.provenance.txt\n");
		if(argc > 3)
			fprintf(f, "# recorder note: %s\n", argv[3]);
		for(int i = 0; i < g_nlines; i++)
			fprintf(f, "%s\n", g_lines[i]);
		fclose(f);
		printf("RECORDED %d lines -> %s\n", g_nlines, argv[1]);
		return (g_invariant_bad || g2_bad) ? 1 : 0;
	}

	int bad = g_invariant_bad + g2_bad;
	if(ngold != g_nlines) {
		printf("FAIL: %d lines produced, golden has %d\n", g_nlines, ngold);
		bad++;
	}
	int n = ngold < g_nlines ? ngold : g_nlines;
	for(int i = 0; i < n; i++)
		if(strcmp(gold[i], g_lines[i])) {
			if(bad < 20)
				printf("MISMATCH line %d\n  golden: %s\n  now:    %s\n", i,
					   gold[i], g_lines[i]);
			bad++;
		}
	if(bad) {
		printf("test_mesher_nongrass_equiv: %d mismatch(es) -- output "
			   "differs from the recorded golden\n",
			   bad);
		return 1;
	}
	printf("test_mesher_nongrass_equiv: OK (%d lines identical to the "
		   "recorded golden; overlay-list invariant holds)\n",
		   g_nlines);
	return 0;
}
