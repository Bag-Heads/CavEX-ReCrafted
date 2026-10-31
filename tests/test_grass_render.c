/*
	DC-GRASS G1 (design-crit-1010, Astra plan astra-grass.log task G1):
	production-linked regressions for Beta 1.7.3 grass rendering.

	PRODUCTION PATH UNDER TEST (nothing copied):
	  - the real texture_atlas.c registration + compositing (tex_atlas_block,
	    reading a SYNTHETIC terrain.png through the real tex_read_atlas pack
	    resolver), and
	  - the real chunk_mesher.c worker (chunk_mesher_send -> worker ->
	    chunk_mesher_receive) emitting the grass/dirt faces into the chunk's
	    display lists, with the real biome_color.c colormap lookup.
	The test then COMPOSITES each block face from the emitted bytes: every quad
	lying exactly on the face, in chunk_render's draw order (chunk.c
	chunk_render/chunk_render_group at base_ref 796f2e43: opaque pass then
	transparent pass; untinted group then tinted group; bottom, top, left,
	right, front, back, double-sided), sampling the produced atlas at each
	texel centre through the quad's own UVs, BETA's alpha test (GL_GREATER
	0.1F, client Minecraft.java:267 -> RGBA8 alpha 26..255 passes; see
	BETA_ALPHA_PASS), colour = texel.rgb x per-corner vertex tint (bilinear).
	Equal-depth acceptance: a later quad on the same plane overwrites.
	Besides the composited texels, check_structure() asserts the emitted
	geometry itself: per face the exact quad count, which raw tile each quad
	samples, EVERY corner's tint, base-before-overlay order, opaque-pass
	lists, and identical overlay/base tile-local UVs and light bytes.
	Per-vertex light is not part of the comparison (Beta multiplies the same
	brightness into base and overlay).

	EXPECTATIONS (derived independently from the Beta 1.7.3 client decompile,
	../mcb173-retromcp/minecraft/src/net/minecraft/src/):
	  BlockGrass.java:12-21 getBlockTexture: top (side 1) = tile 0, bottom
	    (side 0) = tile 2, sides = 3, or 68 when the material above is snow OR
	    builtSnow.
	  BlockGrass.java:23-28 colorMultiplier = biome grass colour.
	  RenderBlocks.java:1954-1970 (AO) and 2236-2281 (flat): the top face is
	    multiplied by the colour multiplier ONCE; for Block.grass the side and
	    bottom colours are NOT multiplied (2240 `if(var1 != Block.grass)`;
	    AO: 1755 `blockIndexInTexture == 3` clears the side colour flags); a
	    side whose texture is 3 is drawn untinted, then, when fancyGrass
	    (default true) and no override texture, tile 38 = (6,2) is drawn over
	    it with the SAME face geometry multiplied by the colour multiplier
	    (2276-2281, 1755-1761). Tile 68 sides get no overlay.
	  So for biome colour B, at tile-local texel (u,v):
	    top    = tile0(u,v) x B
	    side   = tile38(u,v).a passes ? tile38(u,v) x B : tile3(u,v)
	    snowed = tile68(u,v)  (untinted, no overlay); top stays tile0 x B
	    bottom = tile2(u,v)   (untinted)
	  (d) Unavailable data (NOT Beta -- Beta always has a biome colour): when
	  the colormap is missing, or the column has no section tint, grass uses
	  the deterministic CaveWii fallback colour GRASS_FALLBACK, applied exactly
	  once, i.e. rendered identically to a biome whose colormap entry IS
	  GRASS_FALLBACK. GRASS_FALLBACK = (110,198,63), the static colour the
	  pre-fix atlas baked into grass (texture_atlas.c:463 at base_ref), so the
	  unavailable-data look is unchanged.

	SYNTHETIC DATA ONLY: terrain.png and the colormap are generated below and
	written to a pid-suffixed fixture dir; no Beta pack pixels are read or
	committed.

	Cases expected to FAIL before the G2 fix: (a) (b) (c) (d). In (d), the
	missing-colormap and missing-section-tint renders already match before G2
	(the atlas bakes exactly GRASS_FALLBACK); they guard G2 against a white
	(untinted) fallback. (d) fails before G2 on its once-only check: a biome
	whose colormap entry equals GRASS_FALLBACK must render identically to the
	fallback, which the baked atlas colour times the biome tint breaks.
	Regressions expected to PASS throughout: dirt untouched, grass bottom.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "config.h"
#include "game/game_state.h"
#include "graphics/biome_color.h"
#include "graphics/texture_atlas.h"
#include "lodepng/lodepng.h"
#include "network/client_interface.h"
#include "platform/displaylist.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "support/test_fixture_dir.h"
#include "world.h"

#define HANG_TIMEOUT_MS 5000
#define TILE 16
#define IMG 256
#define WY 66 /* block row, local y = 2 inside chunk cy = 4 */
/* Beta alpha test: glAlphaFunc(GL_GREATER, 0.1F) (client Minecraft.java:267):
 * an RGBA8 alpha a passes iff a/255 > 0.1, i.e. a >= 26. This is the ORACLE
 * rule and also the rule the CPU compositor applies (it reconstructs what
 * Beta's pipeline would show for the emitted geometry + atlas bytes). The
 * CaveWii PC backend default differs (fragment.shader:65 discards
 * a/255 < GFX_ALPHA_REF_DEFAULT 0.0625, i.e. passes a >= 16); that GPU state
 * is NOT observable from the mesh and is only reported as a note below. */
#define BETA_ALPHA_PASS(a) ((a) >= 26)
#define BACKEND_DEFAULT_ALPHA_PASS(a) ((a) >= 16)

struct rgb {
	int r, g, b;
};

static const struct rgb B1 = {210, 60, 40};
static const struct rgb B2 = {40, 90, 230};
static const struct rgb GRASS_FALLBACK = {110, 198, 63};
/* colormap pixels (ci, cj) holding B1 / B2 / GRASS_FALLBACK */
#define B1_CI 10
#define B1_CJ 20
#define B2_CI 200
#define B2_CJ 30
#define FB_CI 50
#define FB_CJ 150

static int g_failed_cases = 0;
static bool g_case_failed;

/* ---------------- synthetic terrain.png ---------------- */

static uint8_t g_img[IMG * IMG * 4];

static void synth_texel(int tx, int ty, int u, int v, uint8_t* p) {
	p[3] = 255;
	if(tx == 0 && ty == 0) { /* grass top: greyscale, asymmetric */
		int g = 90 + ((u * 11 + v * 7) % 140);
		p[0] = p[1] = p[2] = (uint8_t)g;
	} else if(tx == 3 && ty == 0) { /* grass side (dirt + fringe base) */
		p[0] = (uint8_t)(120 + u * 6);
		p[1] = (uint8_t)(70 + v * 5);
		p[2] = (uint8_t)(30 + ((u * 3 + v) % 9) * 4);
	} else if(tx == 6 && ty == 2) { /* tile 38: side overlay mask */
		int edge = 3 + ((u * 5) % 4); /* opaque fringe rows: v < edge */
		if(v < edge || v == edge) {
			int g = 120 + ((u * 13 + v * 3) % 120);
			p[0] = p[1] = p[2] = (uint8_t)g;
			/* the row just below the opaque fringe carries Beta alpha-test
			 * boundary / intermediate values: 25 (Beta rejects), 26 (Beta
			 * passes), 128 (intermediate, passes), 16 (Beta rejects, the
			 * current backend default would pass). */
			if(v == edge) {
				static const uint8_t edge_alpha[4] = {25, 26, 128, 16};
				p[3] = edge_alpha[u % 4];
			}
		} else {
			p[0] = 255; /* magenta under alpha 0: any bleed is visible */
			p[1] = 0;
			p[2] = 255;
			p[3] = 0;
		}
	} else if(tx == 4 && ty == 4) { /* tile 68: snowy grass side */
		p[0] = (uint8_t)(230 - u * 2);
		p[1] = (uint8_t)(235 - v * 3);
		p[2] = 250;
	} else if(tx == 2 && ty == 0) { /* tile 2: dirt */
		p[0] = (uint8_t)(110 + v * 4);
		p[1] = (uint8_t)(80 + u * 3);
		p[2] = 50;
	} else { /* every other tile: distinct, opaque */
		p[0] = (uint8_t)(tx * 16 + u);
		p[1] = (uint8_t)(ty * 16 + v);
		p[2] = 200;
	}
}

static void build_synthetic_terrain(void) {
	for(int y = 0; y < IMG; y++)
		for(int x = 0; x < IMG; x++)
			synth_texel(x / TILE, y / TILE, x % TILE, y % TILE,
						g_img + (y * IMG + x) * 4);
}

static const uint8_t* src_tile(int tx, int ty, int u, int v) {
	return g_img + ((ty * TILE + v) * IMG + (tx * TILE + u)) * 4;
}

/* ---------------- world / mesher harness ---------------- */

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

static void set(w_coord_t x, w_coord_t y, w_coord_t z, int type) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = 0,
										 .sky_light = 15,
										 .torch_light = 0},
					false);
}

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

/* ---------------- emitted-quad decoding ---------------- */

struct vtx {
	float p[3];
	float uv[2]; /* atlas pixels (256-wide atlas) */
	uint8_t light[2];
	uint8_t tint[3];
};

static float rd_f(const uint8_t* p) {
	float f;
	memcpy(&f, p, sizeof f);
	return f;
}

/* chunk_render's draw order of the 26 lists (see header comment). */
static int g_draw_order[CHUNK_MESH_LISTS];
static int g_draw_count = 0;
static void build_draw_order(void) {
	static const int sides[6] = {SIDE_BOTTOM, SIDE_TOP,   SIDE_LEFT,
								 SIDE_RIGHT,  SIDE_FRONT, SIDE_BACK};
	for(int pass = 0; pass < 2; pass++)
		for(int base = 0; base <= CHUNK_MESH_PARTS; base += CHUNK_MESH_PARTS) {
			for(int s = 0; s < 6; s++)
				g_draw_order[g_draw_count++] = base + sides[s] + (pass ? 6 : 0);
			if(!pass)
				g_draw_order[g_draw_count++] = base + 12;
		}
	assert(g_draw_count == CHUNK_MESH_LISTS);
}

/* Lists drawn in the opaque (unblended) pass: k % PARTS in 0..5 or 12. */
static bool list_is_opaque_pass(int k) {
	int s = k % CHUNK_MESH_PARTS;
	return s < 6 || s == 12;
}

static void read_vtx(const struct displaylist* l, size_t i, struct vtx* o) {
	size_t stride = l->tinted ? 25u : 22u;
	const uint8_t* b = (const uint8_t*)l->data + i * stride;
	for(int k = 0; k < 3; k++)
		o->p[k] = rd_f(b + 4 * k);
	o->light[0] = b[12];
	o->light[1] = b[13];
	size_t off = 12 + 2;
	if(l->tinted) {
		memcpy(o->tint, b + off, 3);
		off += 3;
	} else {
		o->tint[0] = o->tint[1] = o->tint[2] = 255;
	}
	o->uv[0] = rd_f(b + off) * 256.0F;
	o->uv[1] = rd_f(b + off + 4) * 256.0F;
}

static bool feq(float a, float b) {
	return fabsf(a - b) < 1e-4F;
}

/* face = axis (0 x, 1 y, 2 z) at plane value, spanning the block's unit
 * square on the two other axes. */
struct face {
	int axis;
	float plane;
	float lo[3]; /* block min corner (local chunk coords) */
	const char* name;
};

/* One emitted full-face quad, corners indexed [a][b] in {0,1} on the face. */
struct fquad {
	int list;
	float cu[2][2], cv[2][2]; /* atlas-pixel UV per corner */
	uint8_t tint[2][2][3];
	uint8_t light[2][2][2];
	int cell_x, cell_y; /* atlas pixel origin of the sampled 16x16 cell */
};

#define MAX_FACE_QUADS 8

/* Every quad lying exactly on face f, in chunk_render draw order. Quads on
 * the plane inside the face that do not cover it are counted in *partial. */
static int collect_face(struct chunk* c, const struct face* f,
						struct fquad* out, int* partial) {
	int n = 0;
	*partial = 0;
	int a0 = (f->axis + 1) % 3, a1 = (f->axis + 2) % 3;
	for(int oi = 0; oi < g_draw_count; oi++) {
		int k = g_draw_order[oi];
		if(!c->has_displist[k])
			continue;
		const struct displaylist* l = c->mesh + k;
		if(!l->data)
			continue;
		for(size_t q = 0; q + 3 < l->index; q += 4) {
			struct vtx v[4];
			bool on = true, inside = true;
			for(int j = 0; j < 4; j++) {
				read_vtx(l, q + j, v + j);
				if(!feq(v[j].p[f->axis], f->plane))
					on = false;
				float da = v[j].p[a0] - f->lo[a0], db = v[j].p[a1] - f->lo[a1];
				if(da < -1e-4F || da > 1 + 1e-4F || db < -1e-4F
				   || db > 1 + 1e-4F)
					inside = false;
			}
			if(!on || !inside)
				continue;
			struct fquad fq;
			memset(&fq, 0, sizeof fq);
			fq.list = k;
			bool seen[2][2] = {{false}};
			float umin = 1e9F, vmin = 1e9F;
			bool corners = true;
			for(int j = 0; j < 4; j++) {
				float da = v[j].p[a0] - f->lo[a0], db = v[j].p[a1] - f->lo[a1];
				if(!(feq(da, 0) || feq(da, 1)) || !(feq(db, 0) || feq(db, 1))) {
					corners = false;
					break;
				}
				int ia = feq(da, 1), ib = feq(db, 1);
				seen[ia][ib] = true;
				fq.cu[ia][ib] = v[j].uv[0];
				fq.cv[ia][ib] = v[j].uv[1];
				memcpy(fq.tint[ia][ib], v[j].tint, 3);
				memcpy(fq.light[ia][ib], v[j].light, 2);
				if(v[j].uv[0] < umin)
					umin = v[j].uv[0];
				if(v[j].uv[1] < vmin)
					vmin = v[j].uv[1];
			}
			if(!corners
			   || !(seen[0][0] && seen[0][1] && seen[1][0] && seen[1][1])) {
				(*partial)++;
				continue;
			}
			fq.cell_x = (int)floorf(umin + 1e-3F);
			fq.cell_y = (int)floorf(vmin + 1e-3F);
			if(n < MAX_FACE_QUADS)
				out[n] = fq;
			n++;
		}
	}
	return n;
}

static float bilerp(float c00, float c10, float c01, float c11, float a,
					float b) {
	return (1 - a) * (1 - b) * c00 + a * (1 - b) * c10 + (1 - a) * b * c01
		+ a * b * c11;
}

struct composite {
	bool drawn[TILE][TILE];
	struct rgb px[TILE][TILE];
	int lu[TILE][TILE], lv[TILE][TILE]; /* base quad tile-local texel */
	int quads, partial;
	struct fquad q[MAX_FACE_QUADS];
};

/* Paint the face quads in draw order: Beta alpha test, colour = texel x
 * per-vertex tint (interpolated per corner), equal depth = later quad wins. */
static void composite_face(struct chunk* c, const uint8_t* atlas,
						   const struct face* f, struct composite* out) {
	memset(out, 0, sizeof(*out));
	out->quads = collect_face(c, f, out->q, &out->partial);
	int n = out->quads < MAX_FACE_QUADS ? out->quads : MAX_FACE_QUADS;
	for(int qi = 0; qi < n; qi++) {
		const struct fquad* fq = out->q + qi;
		for(int i = 0; i < TILE; i++)
			for(int jj = 0; jj < TILE; jj++) {
				float a = (i + 0.5F) / TILE, b = (jj + 0.5F) / TILE;
				float u = bilerp(fq->cu[0][0], fq->cu[1][0], fq->cu[0][1],
								 fq->cu[1][1], a, b);
				float vv = bilerp(fq->cv[0][0], fq->cv[1][0], fq->cv[0][1],
								  fq->cv[1][1], a, b);
				int ax = (int)floorf(u), ay = (int)floorf(vv);
				if(qi == 0) {
					out->lu[i][jj] = ax - fq->cell_x;
					out->lv[i][jj] = ay - fq->cell_y;
				}
				if(ax < 0 || ay < 0 || ax >= IMG || ay >= IMG)
					continue;
				const uint8_t* t = atlas + (ay * IMG + ax) * 4;
				if(!BETA_ALPHA_PASS(t[3]))
					continue;
				int tint[3];
				for(int ch = 0; ch < 3; ch++)
					tint[ch] = (int)lroundf(bilerp(
						fq->tint[0][0][ch], fq->tint[1][0][ch],
						fq->tint[0][1][ch], fq->tint[1][1][ch], a, b));
				out->drawn[i][jj] = true;
				out->px[i][jj] = (struct rgb) {(t[0] * tint[0] + 127) / 255,
											   (t[1] * tint[1] + 127) / 255,
											   (t[2] * tint[2] + 127) / 255};
			}
	}
}

/* ---------------- expectations ---------------- */

enum expect_kind { EXP_TOP, EXP_GRASS_SIDE, EXP_PLAIN };

static struct rgb mul(const uint8_t* t, struct rgb c) {
	return (struct rgb) {(t[0] * c.r + 127) / 255, (t[1] * c.g + 127) / 255,
						 (t[2] * c.b + 127) / 255};
}

static struct rgb expected_texel(enum expect_kind k, int tx, int ty,
								 struct rgb tint, int u, int v) {
	switch(k) {
		case EXP_TOP: return mul(src_tile(0, 0, u, v), tint);
		case EXP_GRASS_SIDE: {
			const uint8_t* ov = src_tile(6, 2, u, v);
			if(BETA_ALPHA_PASS(ov[3]))
				return mul(ov, tint);
			const uint8_t* b = src_tile(3, 0, u, v);
			return (struct rgb) {b[0], b[1], b[2]};
		}
		default: {
			const uint8_t* b = src_tile(tx, ty, u, v);
			return (struct rgb) {b[0], b[1], b[2]};
		}
	}
}

static bool close_rgb(struct rgb a, struct rgb b) {
	return abs(a.r - b.r) <= 1 && abs(a.g - b.g) <= 1 && abs(a.b - b.b) <= 1;
}

static const uint8_t* g_atlas; /* the production atlas under test */

/* Byte-exact (RGBA) match of the atlas cell at pixel origin (x0,y0) against
 * the raw synthetic source tile (tx,ty). */
static bool cell_at_is_raw(int x0, int y0, int tx, int ty) {
	if(x0 < 0 || y0 < 0 || x0 + TILE > IMG || y0 + TILE > IMG)
		return false;
	for(int v = 0; v < TILE; v++)
		if(memcmp(g_atlas + ((y0 + v) * IMG + x0) * 4, src_tile(tx, ty, 0, v),
				  TILE * 4))
			return false;
	return true;
}

static bool quad_tint_is(const struct fquad* q, struct rgb c) {
	for(int a = 0; a < 2; a++)
		for(int b = 0; b < 2; b++)
			if(q->tint[a][b][0] != c.r || q->tint[a][b][1] != c.g
			   || q->tint[a][b][2] != c.b)
				return false;
	return true;
}

static void print_quad(const char* role, const struct fquad* q) {
	printf("      %s: list %d (%s pass), cell px (%d,%d), corner tints "
		   "(%d,%d,%d) (%d,%d,%d) (%d,%d,%d) (%d,%d,%d)\n",
		   role, q->list, list_is_opaque_pass(q->list) ? "opaque" : "blended",
		   q->cell_x, q->cell_y, q->tint[0][0][0], q->tint[0][0][1],
		   q->tint[0][0][2], q->tint[1][0][0], q->tint[1][0][1],
		   q->tint[1][0][2], q->tint[0][1][0], q->tint[0][1][1],
		   q->tint[0][1][2], q->tint[1][1][0], q->tint[1][1][1],
		   q->tint[1][1][2]);
}

#define SMISS(...)                                                             \
	do {                                                                       \
		printf(bad++ ? "    ...  %s %s: " : "    MISS %s %s: ", label,         \
			   f->name);                                                       \
		printf(__VA_ARGS__);                                                   \
		printf("\n");                                                          \
	} while(0)

/* Emitted-geometry contract (Beta RenderBlocks.java:2263-2281, 1954-1970;
 * BlockGrass.java:12-28; Block.java:463 getRenderBlockPass() == 0, so the
 * grass base and overlay are drawn in the unblended opaque pass):
 *   EXP_TOP:        exactly one quad, raw tile 0, every corner tint == B.
 *   EXP_PLAIN:      exactly one quad, raw tile (tx,ty), every corner white;
 *                   in particular NO tile-38 overlay (snow sides, dirt).
 *   EXP_GRASS_SIDE: exactly two quads, in draw order: base = raw tile 3,
 *                   every corner white; then overlay = raw tile 38, every
 *                   corner == B; both opaque-pass; identical corner
 *                   positions (by construction), tile-local UVs and light. */
static int check_structure(const char* label, const struct face* f,
						   const struct composite* cp, enum expect_kind k,
						   int tx, int ty, struct rgb tint) {
	int bad = 0;
	const struct rgb white = {255, 255, 255};
	int want = k == EXP_GRASS_SIDE ? 2 : 1;
	if(cp->partial)
		SMISS("%d partial (non-full-face) quad(s) on the face plane",
			  cp->partial);
	if(cp->quads != want) {
		SMISS("%d full-face quad(s) emitted, want %d", cp->quads, want);
		for(int i = 0; i < cp->quads && i < MAX_FACE_QUADS; i++)
			print_quad("quad", cp->q + i);
	}
	int n = cp->quads < MAX_FACE_QUADS ? cp->quads : MAX_FACE_QUADS;
	for(int i = 0; i < n; i++)
		if(k != EXP_GRASS_SIDE
		   && cell_at_is_raw(cp->q[i].cell_x, cp->q[i].cell_y, 6, 2))
			SMISS("a grass overlay (tile 38) quad is emitted on this face");
	if(!n)
		return bad;
	const struct fquad* q0 = cp->q;
	switch(k) {
		case EXP_TOP:
			if(!cell_at_is_raw(q0->cell_x, q0->cell_y, 0, 0))
				SMISS("top quad does not sample the raw tile 0");
			if(!quad_tint_is(q0, tint)) {
				SMISS("top quad corner tints != biome (%d,%d,%d)", tint.r,
					  tint.g, tint.b);
				print_quad("top", q0);
			}
			break;
		case EXP_PLAIN:
			if(!cell_at_is_raw(q0->cell_x, q0->cell_y, tx, ty))
				SMISS("quad does not sample the raw tile (%d,%d)", tx, ty);
			if(!quad_tint_is(q0, white)) {
				SMISS("quad is tinted; want untinted (white) at every corner");
				print_quad("quad", q0);
			}
			break;
		case EXP_GRASS_SIDE: {
			if(!cell_at_is_raw(q0->cell_x, q0->cell_y, 3, 0))
				SMISS("first-drawn quad is not the raw tile 3 base");
			if(!quad_tint_is(q0, white)) {
				SMISS("base quad is tinted; want white at every corner");
				print_quad("base", q0);
			}
			if(!list_is_opaque_pass(q0->list))
				SMISS("base quad is in a blended-pass list (%d)", q0->list);
			if(n < 2) {
				SMISS("no separate tile-38 overlay quad drawn after the base");
				print_quad("base", q0);
				break;
			}
			const struct fquad* q1 = cp->q + 1;
			if(!cell_at_is_raw(q1->cell_x, q1->cell_y, 6, 2))
				SMISS("second-drawn quad is not the raw tile 38 overlay");
			if(!quad_tint_is(q1, tint)) {
				SMISS("overlay corner tints != biome (%d,%d,%d)", tint.r,
					  tint.g, tint.b);
				print_quad("overlay", q1);
			}
			if(!list_is_opaque_pass(q1->list))
				SMISS("overlay quad is in a blended-pass list (%d)", q1->list);
			for(int a = 0; a < 2; a++)
				for(int b = 0; b < 2; b++) {
					if(!feq(q0->cu[a][b] - q0->cell_x, q1->cu[a][b] - q1->cell_x)
					   || !feq(q0->cv[a][b] - q0->cell_y,
							   q1->cv[a][b] - q1->cell_y))
						SMISS("corner %d,%d: overlay tile-local UV differs "
							  "from the base UV",
							  a, b);
					if(memcmp(q0->light[a][b], q1->light[a][b], 2))
						SMISS("corner %d,%d: overlay light (%d,%d) != base "
							  "(%d,%d)",
							  a, b, q1->light[a][b][0], q1->light[a][b][1],
							  q0->light[a][b][0], q0->light[a][b][1]);
				}
			break;
		}
	}
	return bad;
}

static bool g_backend_alpha_noted = false;

/* True when the face matches both the emitted-geometry contract and the
 * composited Beta texels; prints the misses. */
static bool check_face(const char* label, struct chunk* c, const uint8_t* atlas,
					   const struct face* f, enum expect_kind k, int tx, int ty,
					   struct rgb tint) {
	struct composite cp;
	composite_face(c, atlas, f, &cp);
	if(!cp.quads) {
		printf("    MISS %s %s: no quad emitted on this face\n", label, f->name);
		return false;
	}
	int sbad = check_structure(label, f, &cp, k, tx, ty, tint);
	int bad = 0;
	for(int i = 0; i < TILE; i++)
		for(int j = 0; j < TILE; j++) {
			int u = cp.lu[i][j], v = cp.lv[i][j];
			if(u < 0 || v < 0 || u >= TILE || v >= TILE) {
				if(!bad++)
					printf("    MISS %s %s: base quad samples outside one "
						   "16x16 cell (local %d,%d)\n",
						   label, f->name, u, v);
				continue;
			}
			struct rgb want = expected_texel(k, tx, ty, tint, u, v);
			if(!cp.drawn[i][j] || !close_rgb(cp.px[i][j], want)) {
				if(!bad++)
					printf("    MISS %s %s: texel (%d,%d) got %s(%d,%d,%d) "
						   "want (%d,%d,%d) [%d quads]\n",
						   label, f->name, u, v,
						   cp.drawn[i][j] ? "" : "UNDRAWN ", cp.px[i][j].r,
						   cp.px[i][j].g, cp.px[i][j].b, want.r, want.g, want.b,
						   cp.quads);
			}
			if(k == EXP_GRASS_SIDE && !g_backend_alpha_noted) {
				const uint8_t* ov = src_tile(6, 2, u, v);
				if(BACKEND_DEFAULT_ALPHA_PASS(ov[3])
				   && !BETA_ALPHA_PASS(ov[3])) {
					g_backend_alpha_noted = true;
					printf("    note: overlay texel alpha %d is rejected by "
						   "Beta (GL_GREATER 0.1) but passes the PC backend "
						   "default ref 0.0625; the overlay draw needs the "
						   "Beta ref (GPU state, not observable here)\n",
						   ov[3]);
				}
			}
		}
	if(bad)
		printf("    %s %s: %d / 256 texels differ\n", label, f->name, bad);
	return bad == 0 && sbad == 0;
}

static struct face top_face(int bx, int by, int bz) {
	return (struct face) {1, (float)(by + 1), {bx, by, bz}, "top"};
}
static struct face bottom_face(int bx, int by, int bz) {
	return (struct face) {1, (float)by, {bx, by, bz}, "bottom"};
}
static void side_faces(int bx, int by, int bz, struct face out[4]) {
	out[0] = (struct face) {0, (float)bx, {bx, by, bz}, "side -x"};
	out[1] = (struct face) {0, (float)(bx + 1), {bx, by, bz}, "side +x"};
	out[2] = (struct face) {2, (float)bz, {bx, by, bz}, "side -z"};
	out[3] = (struct face) {2, (float)(bz + 1), {bx, by, bz}, "side +z"};
}

/* Does atlas cell `cell` hold the raw, untinted source tile (tx,ty)? */
static bool cell_is_raw_tile(const uint8_t* atlas, size_t aw, size_t ah,
							 uint8_t cell, int tx, int ty, const char* label) {
	float uv[4];
	assert(tex_atlas_tile_uv(aw, ah, cell, uv));
	int x0 = (int)(uv[0] * aw + 0.5F), y0 = (int)(uv[1] * ah + 0.5F);
	int bad = 0;
	for(int v = 0; v < TILE; v++)
		for(int u = 0; u < TILE; u++) {
			const uint8_t* a = atlas + ((y0 + v) * IMG + x0 + u) * 4;
			const uint8_t* s = src_tile(tx, ty, u, v);
			if(memcmp(a, s, 4) && !bad++)
				printf("    MISS %s atlas cell texel (%d,%d) = (%d,%d,%d,%d), "
					   "raw tile (%d,%d) = (%d,%d,%d,%d)\n",
					   label, u, v, a[0], a[1], a[2], a[3], tx, ty, s[0], s[1],
					   s[2], s[3]);
		}
	if(bad)
		printf("    %s: %d / 256 atlas texels differ from raw tile (%d,%d)\n",
			   label, bad, tx, ty);
	return bad == 0;
}

/* The production grass block's top texture with `above` on top of it. */
static uint8_t grass_top_texture(uint8_t above) {
	struct block_data self = {.type = BLOCK_GRASS, .sky_light = 15};
	struct block_data nb[SIDE_MAX];
	memset(nb, 0, sizeof nb);
	nb[SIDE_TOP].type = above;
	struct block_info bi = {.block = &self, .neighbours = nb};
	return blocks[BLOCK_GRASS]->getTextureIndex(&bi, SIDE_TOP);
}

static void case_begin(const char* name) {
	g_case_failed = false;
	printf("CASE %s\n", name);
}
static void case_expect(bool ok) {
	if(!ok)
		g_case_failed = true;
}
static void case_end(const char* name) {
	if(g_case_failed) {
		g_failed_cases++;
		printf("FAIL %s\n", name);
	} else {
		printf("PASS %s\n", name);
	}
}

static bool same_composite(struct chunk* ca, const struct face* fa,
						   struct chunk* cb, const struct face* fb,
						   const uint8_t* atlas, const char* label) {
	struct composite x, y;
	composite_face(ca, atlas, fa, &x);
	composite_face(cb, atlas, fb, &y);
	if(!x.quads || !y.quads) {
		printf("    MISS %s %s: face not emitted\n", label, fa->name);
		return false;
	}
	for(int i = 0; i < TILE; i++)
		for(int j = 0; j < TILE; j++)
			if(x.drawn[i][j] != y.drawn[i][j]
			   || (x.drawn[i][j] && !close_rgb(x.px[i][j], y.px[i][j]))) {
				printf("    MISS %s %s: texel %d,%d (%d,%d,%d) vs (%d,%d,%d)\n",
					   label, fa->name, i, j, x.px[i][j].r, x.px[i][j].g,
					   x.px[i][j].b, y.px[i][j].r, y.px[i][j].g, y.px[i][j].b);
				return false;
			}
	return true;
}

/* ---------------- colormap ---------------- */

static uint8_t* make_colormap(void) {
	uint8_t* m = mem2_alloc(256 * 256 * 4);
	assert(m);
	for(int i = 0; i < 256 * 256; i++) {
		m[i * 4 + 0] = 60;
		m[i * 4 + 1] = 61;
		m[i * 4 + 2] = 62;
		m[i * 4 + 3] = 255;
	}
	struct {
		int ci, cj;
		struct rgb c;
	} e[3] = {{B1_CI, B1_CJ, B1}, {B2_CI, B2_CJ, B2}, {FB_CI, FB_CJ, GRASS_FALLBACK}};
	for(int k = 0; k < 3; k++) {
		uint8_t* p = m + (e[k].cj * 256 + e[k].ci) * 4; /* map[cj<<8|ci] */
		p[0] = (uint8_t)e[k].c.r;
		p[1] = (uint8_t)e[k].c.g;
		p[2] = (uint8_t)e[k].c.b;
	}
	return m;
}

/* Layout: blocks on the chunk diagonal (x == z) so a column's tint index is
 * the same whichever of the two 16x16 layouts (x*16+z or x+z*16) a reader
 * uses; diagonal neighbours never occlude a face. Local y = 2. */
#define P_G1 1 /* grass, biome B1 */
#define P_G2 3 /* grass, biome B2 */
#define P_D 5  /* dirt */
#define P_S1 7 /* grass, snow LAYER above, biome B1 */
#define P_S2 9 /* grass, snow BLOCK above, biome B2 */
#define P_F 11 /* grass, colormap entry == GRASS_FALLBACK */
#define LY (WY - 64)

static void set_tints(void) {
	uint8_t gi[CHUNK_SIZE * CHUNK_SIZE], gj[CHUNK_SIZE * CHUNK_SIZE];
	memset(gi, 0, sizeof gi);
	memset(gj, 0, sizeof gj);
	struct {
		int p, ci, cj;
	} t[] = {{P_G1, B1_CI, B1_CJ}, {P_G2, B2_CI, B2_CJ}, {P_D, B1_CI, B1_CJ},
			 {P_S1, B1_CI, B1_CJ}, {P_S2, B2_CI, B2_CJ}, {P_F, FB_CI, FB_CJ}};
	for(size_t k = 0; k < sizeof t / sizeof t[0]; k++) {
		gi[t[k].p * CHUNK_SIZE + t[k].p] = (uint8_t)t[k].ci;
		gj[t[k].p * CHUNK_SIZE + t[k].p] = (uint8_t)t[k].cj;
	}
	world_set_section_tint(&w, 0, 0, gi, gj);
}

static void make_dir(const char* path) {
#ifdef _WIN32
	assert(!_mkdir(path));
#else
	assert(!mkdir(path, 0755));
#endif
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	build_draw_order();

	/* --- SETUP: synthetic pack -> real tex_read_atlas -> real atlas --- */
	char fixture[160], path[256];
	test_fixture_dir("test_grass_render", fixture, sizeof fixture);
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
	assert(atlas && aw == IMG && ah == IMG
		   && "SETUP: production atlas did not build from the synthetic pack");
	g_atlas = atlas;

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	world_create(&w);
	assert(chunk_mesher_init() && "SETUP: mesher worker did not start");

	set(P_G1, WY, P_G1, BLOCK_GRASS);
	set(P_G2, WY, P_G2, BLOCK_GRASS);
	set(P_D, WY, P_D, BLOCK_DIRT);
	set(P_S1, WY, P_S1, BLOCK_GRASS);
	set(P_S1, WY + 1, P_S1, BLOCK_SNOW);
	set(P_S2, WY, P_S2, BLOCK_GRASS);
	set(P_S2, WY + 1, P_S2, BLOCK_SNOW_BLOCK);
	set(P_F, WY, P_F, BLOCK_GRASS);
	/* second column (chunk x = 1) for the missing-section-tint case */
	set(16 + 3, WY, 3, BLOCK_GRASS);
	struct chunk* c = world_find_chunk(&w, P_G1, WY, P_G1);
	struct chunk* c2 = world_find_chunk(&w, 16 + 3, WY, 3);
	assert(c && c2 && c != c2);
	set_tints();
	biome_color_import(make_colormap(), 256, 256, NULL, 0, 0);

	assert(mesh_chunk_and_wait(c) && "SETUP: mesh build hung");
	puts("SETUP OK: synthetic 256x256 terrain.png -> tex_atlas_block; "
		 "chunk meshed by the real worker");

	struct face sides[4];
	struct face f;

	/* (a) atlas: TEXAT_GRASS_TOP cell is the raw, untinted tile 0 */
	case_begin("(a) TEXAT_GRASS_TOP atlas cell == raw tile (0,0), untinted");
	{
		case_expect(cell_is_raw_tile(atlas, aw, ah,
									 tex_atlas_lookup(TEXAT_GRASS_TOP), 0, 0,
									 "TEXAT_GRASS_TOP"));
		f = top_face(P_G1, LY, P_G1);
		case_expect(check_face("grass@B1", c, atlas, &f, EXP_TOP, 0, 0, B1));
		f = top_face(P_G2, LY, P_G2);
		case_expect(check_face("grass@B2", c, atlas, &f, EXP_TOP, 0, 0, B2));
	}
	case_end("(a) TEXAT_GRASS_TOP atlas cell == raw tile (0,0), untinted");

	/* (b) sides: tile 3 untinted + tile 38 overlay x biome colour */
	case_begin("(b) grass sides = tile 3 untinted + tile 38 x biome (B1 vs B2)");
	side_faces(P_G1, LY, P_G1, sides);
	for(int s = 0; s < 4; s++)
		case_expect(check_face("grass@B1", c, atlas, sides + s, EXP_GRASS_SIDE,
							   0, 0, B1));
	side_faces(P_G2, LY, P_G2, sides);
	for(int s = 0; s < 4; s++)
		case_expect(check_face("grass@B2", c, atlas, sides + s, EXP_GRASS_SIDE,
							   0, 0, B2));
	case_end("(b) grass sides = tile 3 untinted + tile 38 x biome (B1 vs B2)");

	/* (c) snow / built snow above */
	case_begin("(c) snow above: sides tile 68, no overlay; top stays tile 0");
	side_faces(P_S1, LY, P_S1, sides);
	for(int s = 0; s < 4; s++)
		case_expect(check_face("grass+snow-layer", c, atlas, sides + s,
							   EXP_PLAIN, 4, 4, B1));
	side_faces(P_S2, LY, P_S2, sides);
	for(int s = 0; s < 4; s++)
		case_expect(check_face("grass+snow-block", c, atlas, sides + s,
							   EXP_PLAIN, 4, 4, B2));
	/* Top texture stays tile 0 (BlockGrass.java:13-14, side 1 -> 0, no snow
	 * test), read from the production block with snow / built snow above. */
	case_expect(cell_is_raw_tile(atlas, aw, ah, grass_top_texture(BLOCK_SNOW),
								 0, 0, "grass top under snow layer"));
	case_expect(cell_is_raw_tile(atlas, aw, ah,
								 grass_top_texture(BLOCK_SNOW_BLOCK), 0, 0,
								 "grass top under snow block"));
	/* Beta draws that top face under a snow LAYER: BlockSnow is not an
	 * opaque cube (BlockSnow.java:17), so Block.java:236 renders the grass
	 * top (side 1) -- tile 0 x biome, once. A built-snow block above is
	 * opaque and may occlude it. */
	f = top_face(P_S1, LY, P_S1);
	case_expect(
		check_face("grass+snow-layer", c, atlas, &f, EXP_TOP, 0, 0, B1));
	case_end("(c) snow above: sides tile 68, no overlay; top stays tile 0");

	/* regressions (expected to pass before and after G2) */
	case_begin("(r1) dirt untouched: every face raw tile 2");
	side_faces(P_D, LY, P_D, sides);
	for(int s = 0; s < 4; s++)
		case_expect(
			check_face("dirt", c, atlas, sides + s, EXP_PLAIN, 2, 0, B1));
	f = top_face(P_D, LY, P_D);
	case_expect(check_face("dirt", c, atlas, &f, EXP_PLAIN, 2, 0, B1));
	f = bottom_face(P_D, LY, P_D);
	case_expect(check_face("dirt", c, atlas, &f, EXP_PLAIN, 2, 0, B1));
	case_end("(r1) dirt untouched: every face raw tile 2");

	case_begin("(r2) grass bottom face = raw tile 2, untinted");
	f = bottom_face(P_G1, LY, P_G1);
	case_expect(check_face("grass@B1", c, atlas, &f, EXP_PLAIN, 2, 0, B1));
	f = bottom_face(P_G2, LY, P_G2);
	case_expect(check_face("grass@B2", c, atlas, &f, EXP_PLAIN, 2, 0, B2));
	case_end("(r2) grass bottom face = raw tile 2, untinted");

	/* (d) unavailable data -> GRASS_FALLBACK exactly once */
	case_begin("(d) missing colormap / missing section tint -> fallback once");
	{
		/* d.3 first (colormap present): a biome whose colour IS the
		 * fallback renders as tile0 x GRASS_FALLBACK, once. */
		struct face top_f = top_face(P_F, LY, P_F);
		case_expect(check_face("grass@colormap==fallback", c, atlas, &top_f,
							   EXP_TOP, 0, 0, GRASS_FALLBACK));
		side_faces(P_F, LY, P_F, sides);
		for(int s = 0; s < 4; s++)
			case_expect(check_face("grass@colormap==fallback", c, atlas,
								   sides + s, EXP_GRASS_SIDE, 0, 0,
								   GRASS_FALLBACK));

		/* d.1 missing colormap (section tint present) */
		biome_color_import(NULL, 0, 0, NULL, 0, 0);
		assert(mesh_chunk_and_wait(c) && "SETUP: remesh hung");
		struct face g1_top = top_face(P_G1, LY, P_G1);
		case_expect(check_face("no-colormap", c, atlas, &g1_top, EXP_TOP, 0, 0,
							   GRASS_FALLBACK));
		side_faces(P_G1, LY, P_G1, sides);
		for(int s = 0; s < 4; s++)
			case_expect(check_face("no-colormap", c, atlas, sides + s,
								   EXP_GRASS_SIDE, 0, 0, GRASS_FALLBACK));
		f = bottom_face(P_G1, LY, P_G1);
		case_expect(
			check_face("no-colormap", c, atlas, &f, EXP_PLAIN, 2, 0, B1));

		/* d.2 missing section tint (colormap present again): the column's
		 * section is gone, so world_get_section_tint reports no tint. The
		 * chunk is kept alive by our own reference. */
		biome_color_import(make_colormap(), 256, 256, NULL, 0, 0);
		chunk_ref(c2);
		world_unload_section(&w, 1, 0);
		uint8_t gi[CHUNK_SIZE * CHUNK_SIZE], gj[CHUNK_SIZE * CHUNK_SIZE];
		assert(!world_get_section_tint(&w, c2->x, c2->z, gi, gj)
			   && "SETUP: section tint still present");
		assert(mesh_chunk_and_wait(c2) && "SETUP: detached remesh hung");
		bool any = false;
		for(int k = 0; k < CHUNK_MESH_LISTS; k++)
			any |= c2->has_displist[k];
		assert(any && "SETUP: no mesh produced for the no-section-tint chunk");
		struct face t2 = top_face(3, LY, 3);
		case_expect(check_face("no-section-tint", c2, atlas, &t2, EXP_TOP, 0, 0,
							   GRASS_FALLBACK));
		side_faces(3, LY, 3, sides);
		for(int s = 0; s < 4; s++)
			case_expect(check_face("no-section-tint", c2, atlas, sides + s,
								   EXP_GRASS_SIDE, 0, 0, GRASS_FALLBACK));
		/* exactly once: unavailable data renders like a biome == fallback */
		case_expect(same_composite(c2, &t2, c, &g1_top, atlas,
								   "no-section-tint vs no-colormap"));
	}
	case_end("(d) missing colormap / missing section tint -> fallback once");

	chunk_mesher_shutdown();
	g_drain_run = 0;
	thread_join(&drain);
	chunk_unref(c2);
	world_destroy(&w);
	free(atlas);
	config_destroy(&gstate.config_user);
	test_fixture_dir_cleanup(fixture);

	if(g_failed_cases) {
		printf("test_grass_render: %d case(s) FAILED\n", g_failed_cases);
		return 1;
	}
	puts("test_grass_render: OK");
	return 0;
}
