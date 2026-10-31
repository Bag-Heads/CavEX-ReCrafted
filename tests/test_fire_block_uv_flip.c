/* Golden for Beta RenderBlocks.renderBlockFire lines 795-950: emitted per-position U order,
   plus (F1b) the literal fire / fire+16 cell each emitted quad samples. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/redstone_power.h"
#include "graphics/render_block.h"
#include "graphics/texture_atlas.h" // TEXTURE_X / TEXTURE_Y
#include "platform/displaylist.h"
#include "platform/gfx.h"		   // enum gfx_blend / depth_func / cull_func, mat4
#include "platform/texture.h"	   // struct tex_gfx (texture_terrain)
#include "game/game_state.h"	   // struct game_state (gstate), struct window_container
#include "item/items.h"			   // struct item, item_get

// render_block_fire reads only the supplied neighbour array.
static bool world_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* out) {
	(void)ctx; (void)x; (void)y; (void)z;
	*out = (struct block_data) {0};
	return false;
}

// ---- leaf engine symbols render_block.c's OTHER (never-called-by-this-test) functions
//      reference. See test_redstone_render_count.c's identical rationale -- Windows bfd ld
//      crashes on --gc-sections for this target, so unused-here symbols are satisfied explicitly.
bool block_is_normal_cube(uint8_t type) { (void)type; return false; }
bool block_beta_material_is_solid(int id) { (void)id; return false; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry name) { (void)name; return 0; }
// Link-only: this test never calls render_block_redstone_wire.
void redstone_wire_connections(const struct redstone_power_world* w, int x, int y, int z,
							   bool conn[4]) {
	(void)w; (void)x; (void)y; (void)z;
	conn[0] = conn[1] = conn[2] = conn[3] = false;
}

// ---- captured emitted vertex stream.
struct cap_vtx {
	int16_t x, y, z;
	uint8_t light, s, t;
};
#define MAX_CAP 64
static struct cap_vtx g_cap[MAX_CAP];
static int g_cap_n;
void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t) {
	(void)l;
	if(g_cap_n < MAX_CAP)
		g_cap[g_cap_n++] = (struct cap_vtx) {x, y, z, light, s, t};
}
// ---- link-only stubs: symbols referenced ONLY by render_block.c's other (never-called)
//      functions -- same set as test_redstone_render_count.c.
struct game_state gstate;
struct tex_gfx texture_terrain;
void displaylist_init(struct displaylist* l, size_t v, size_t s) { (void)l; (void)v; (void)s; }
void displaylist_reset(struct displaylist* l) { (void)l; }
void displaylist_render_immediate(struct displaylist* l, uint16_t n) { (void)l; (void)n; }
void gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void gfx_blending(enum gfx_blend m) { (void)m; }
void gfx_depth_func(enum depth_func f) { (void)f; }
void gfx_cull_func(enum cull_func f) { (void)f; }
/* parity-render2-0924/G1: render_block_cracks_owned (never called here) now sets the
   alpha test and a depth-range bias like Beta's drawBlockBreaking. Link-only stubs. */
void gfx_alpha_test(bool enable) { (void)enable; }
void gfx_alpha_test_ref(bool enable, float ref) { (void)enable; (void)ref; }
void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	(void)color; (void)depth; (void)depth_test;
}
void gfx_depth_range(float near, float far) { (void)near; (void)far; }
void gfx_matrix_modelview(mat4 m) { (void)m; }
uint32_t hash_u32(uint32_t x) { return x; }
enum side blocks_side_opposite(enum side s) {
	// Real semantics needed here: render_block_stairs branches on
	// side == blocks_side_opposite(facing), and this test exercises exactly that branch.
	switch(s) {
		case SIDE_TOP: return SIDE_BOTTOM;
		case SIDE_BOTTOM: return SIDE_TOP;
		case SIDE_LEFT: return SIDE_RIGHT;
		case SIDE_RIGHT: return SIDE_LEFT;
		case SIDE_FRONT: return SIDE_BACK;
		case SIDE_BACK: return SIDE_FRONT;
		default: return s;
	}
}
void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	(void)s; if(x) *x = 0; if(y) *y = 0; if(z) *z = 0;
}
bool inventory_get_hotbar_item(struct inventory* inv, struct item_data* it) {
	(void)inv; (void)it; return false;
}
struct item* item_get(struct item_data* it) { (void)it; return NULL; }
/* push-0921/B18 (review B17 H1): block_cracks_texture -- never called by this
   test -- moved from tool_dig_delay_ms() to tool_dig_crack_stage(), which takes
   the dig's observed environment so the overlay and the dig share one delay.
   Link-only stub, same as the others above. */
int tool_dig_crack_stage(struct block* b, struct item* it, bool in_water,
						 bool on_ground, int elapsed_ms) {
	(void)b; (void)it; (void)in_water; (void)on_ground; (void)elapsed_ms;
	return 0;
}
struct inventory* windowc_get_latest(struct window_container* wc) { (void)wc; return NULL; }
ptime_t time_get(void) { ptime_t t; memset(&t, 0, sizeof t); return t; }
int32_t time_diff_ms(ptime_t f, ptime_t s) { (void)f; (void)s; return 0; }
struct block_data world_get_block(struct world* w, w_coord_t x, w_coord_t y, w_coord_t z) {
	(void)w; (void)x; (void)y; (void)z;
	struct block_data b = {0};
	return b;
}
/* A2-W: render_block.c reads the drawn view's world (world.c) */
struct world* world_view_current(void) {
	return NULL;
}

// Tile zero gives a known fire-cell U origin of 3.
static uint8_t tex_zero(struct block_info* bi, enum side side) {
	(void)bi; (void)side;
	return 0;
}
static struct block mock_block = {.getTextureIndex = tex_zero, .luminance = 0};
struct block* blocks[256];

static uint8_t LIGHT[24];
static struct block_data g_self;
static struct block_data g_neighbours[SIDE_MAX];
static struct block_info mk_into(struct block_data* store, w_coord_t x, w_coord_t y,
								 w_coord_t z, uint8_t type, uint8_t meta) {
	*store = (struct block_data) {.type = type, .metadata = meta};
	struct block_info bi = {0};
	bi.block = store;
	bi.neighbours = g_neighbours;
	bi.x = x;
	bi.y = y;
	bi.z = z;
	bi.get_block = world_get;
	bi.get_block_ctx = NULL;
	return bi;
}

static struct block_info mk(w_coord_t x, w_coord_t y, w_coord_t z, uint8_t type,
							uint8_t meta) {
	for(int i = 0; i < SIDE_MAX; i++)
		g_neighbours[i] = (struct block_data) {.type = BLOCK_AIR, .metadata = 0};
	return mk_into(&g_self, x, y, z, type, meta);
}

/* RenderBlocks.java:805-809 swaps var10/var12 only inside the floating-fire
   branch, after :798-803 chooses fire or fire+16. The four leaning-neighbour
   pairs at :811-853 all consume that swapped pair. The ceiling branch
   :855-897 resets U, and the eight free-standing planes :907-950 never read
   the half-coordinate flip. Java integer /2 truncates toward zero. */
static int beta_flip(int x, int y, int z) {
	return ((x / 2 + y / 2 + z / 2) & 1) != 0;
}

/* Beta :811-853 writes each side with the same U pair (u0 low, u1
   high), then its second four vertices reverse that pair. CaveWii emits one
   double-sided quad per neighbour: SIDE_LEFT faces Beta's +X attachment,
   SIDE_RIGHT its -X, SIDE_FRONT its +Z, SIDE_BACK its -Z. lean_hi records the
   Beta U (0 = u0, 1 = u1) at each CaveWii-emitted corner, before :805's
   u0/u1 exchange; exchanging the pair toggles every bit. This tests
   order, not just min/max U. Emitted corner order per side (render_block.c
   render_block_side_adv_v2): LEFT z0,z0,z1,z1; RIGHT z0,z1,z1,z0;
   FRONT x0,x1,x1,x0; BACK x0,x0,x1,x1.
     +X :823-826  z=0 -> u0, z=1 -> u1
     -X :812-815  z=1 -> u1, z=0 -> u0   (lo at z=0, hi at z=1)
     +Z :845-848  x=1 -> u0, x=0 -> u1
     -Z :834-837  x=0 -> u1, x=1 -> u0   (hi at x=0, lo at x=1)
   next-gaps-0924/R1a: the -X / -Z rows previously snapshotted CaveWii's
   mirrored order ({1,0,0,1} / {0,0,1,1}), not Beta. */
static const uint8_t lean_hi[SIDE_MAX][4] = {
	[SIDE_LEFT] = {0, 0, 1, 1},
	[SIDE_RIGHT] = {0, 1, 1, 0},
	[SIDE_FRONT] = {1, 0, 0, 1},
	[SIDE_BACK] = {1, 1, 0, 0},
};

/* Free-standing fire (Beta RenderBlocks.renderBlockFire :907-950). CaveWii
   emits two quads per side: the EDGE quad (foot on the block edge, top 0.09
   in) and the INSET quad (foot 0.31 in, top 0.81 in). Geometrically they are
   Beta's two DIFFERENT planes of that axis, and the two planes carry OPPOSITE
   U orientations (each Beta plane writes u0/u1 at opposite ends):
     edge LEFT  (foot x=0)   = outer-X :935-938   z=0 -> u0, z=1 -> u1
     edge RIGHT (foot x=1)   = outer-X :939-942   z=0 -> u1, z=1 -> u0
     edge FRONT (foot z=0)   = outer-Z :951-954   x=0 -> u1, x=1 -> u0
     edge BACK  (foot z=1)   = outer-Z :947-950   x=0 -> u0, x=1 -> u1
     inset LEFT  (foot .31)  = inner-X :911-914   z=0 -> u1, z=1 -> u0
     inset RIGHT (foot .69)  = inner-X :907-910   z=0 -> u0, z=1 -> u1
     inset FRONT (foot .31)  = inner-Z :919-922   x=0 -> u0, x=1 -> u1
     inset BACK  (foot .69)  = inner-Z :923-926   x=0 -> u1, x=1 -> u0
   (decompile check: RetroMCP RenderBlocks.java renderBlockFire's else-branch,
   planes with feet 0.5+-0.2 then 0.5+-0.5.) Mapped to render_block_side_adv_v2's
   emitted corner order: LEFT z0,z0,z1,z1; RIGHT z0,z1,z1,z0; FRONT x0,x1,x1,x0;
   BACK x0,x0,x1,x1. free_inset_hi is the per-side inverse of free_edge_hi.
   next-gaps-0924/G1b: the inset rows previously copied the edge rows, which
   enshrined 4 of 8 free-standing quads U-mirrored against Beta.
   The ceiling branch is checked separately in check_at (W3-r1): Beta
   :855-897 parity-dependent planes, positions and UVs. */
static const uint8_t free_edge_hi[SIDE_MAX][4] = {
	[SIDE_LEFT] = {0, 0, 1, 1},
	[SIDE_RIGHT] = {1, 0, 0, 1},
	[SIDE_FRONT] = {1, 0, 0, 1},
	[SIDE_BACK] = {0, 0, 1, 1},
};
static const uint8_t free_inset_hi[SIDE_MAX][4] = {
	[SIDE_LEFT] = {1, 1, 0, 0},
	[SIDE_RIGHT] = {0, 1, 1, 0},
	[SIDE_FRONT] = {0, 1, 1, 0},
	[SIDE_BACK] = {1, 1, 0, 0},
};

static void check_quad(const uint8_t (*table)[4], const char* label, int x, int y,
				   int z, enum side side, int quad, int tile, int flip, int rotation) {
	const int first = quad * 4;
	const int u0 = 3 + tile * 54; /* tex_zero: fire cell col 0, second col +3. */
	for(int i = 0; i < 4; i++) {
		int hi = table[side][(i + rotation) & 3] ^ flip;
		int want = u0 + hi * 16;
		if(g_cap[first + i].s != want) {
			fprintf(stderr, "FAIL %s (%d,%d,%d) side=%d quad=%d vertex=%d U=%u want=%d\n",
					label, x, y, z, side, quad, i, g_cap[first + i].s, want);
			assert(g_cap[first + i].s == want);
		}
	}
}

static void check_at(int x, int y, int z) {
	for(enum side side = SIDE_LEFT; side <= SIDE_BACK; side++) {
		if(side == SIDE_TOP || side == SIDE_BOTTOM)
			continue;
		struct block_info bi = mk(x, y, z, BLOCK_FIRE, 0);
		g_neighbours[blocks_side_opposite(side)].type = BLOCK_PLANKS;
		g_cap_n = 0;
		assert(render_block_fire(NULL, &bi, side, NULL, LIGHT, true) == 1);
		assert(render_block_fire(NULL, &bi, side, NULL, LIGHT, false) == 1);
		assert(g_cap_n == 4);
		check_quad(lean_hi, "leaning", x, y, z, side, 0,
				   (x + y + z) & 1, beta_flip(x, y, z), 0);
	}

	/* Ceiling uses its own pair of tiles and a 90-degree second quad. Neither
	   inherits the half-coordinate swap from the side branch. */
	struct block_info bi = mk(x, y, z, BLOCK_FIRE, 0);
	g_neighbours[SIDE_TOP].type = BLOCK_PLANKS;
	g_cap_n = 0;
	assert(render_block_fire(NULL, &bi, SIDE_BOTTOM, NULL, LIGHT, true) == 2);
	assert(render_block_fire(NULL, &bi, SIDE_BOTTOM, NULL, LIGHT, false) == 2);
	assert(g_cap_n == 8);
	/* RenderBlocks.java:855-897, Beta's LITERAL vertex list per parity arm
	   (after ++var3; var18 = -0.2F -> 0.8 block = 205/256; the even arm leans
	   along X, the odd arm along Z). Per vertex: x, y, z (1/256 of the block),
	   U hi (var12), V hi (var16). Beta: hi,hi,lo,lo U; top,bottom,bottom,top V. */
	static const int beta_ceiling[2][2][4][5] = {
		{ /* even: :871-880 */
			{{0, 205, 0, 1, 0}, {256, 256, 0, 1, 1}, {256, 256, 256, 0, 1}, {0, 205, 256, 0, 0}},
			{{256, 205, 256, 1, 0}, {0, 256, 256, 1, 1}, {0, 256, 0, 0, 1}, {256, 205, 0, 0, 0}},
		},
		{ /* odd: :882-891 */
			{{0, 205, 256, 1, 0}, {0, 256, 0, 1, 1}, {256, 256, 0, 0, 1}, {256, 205, 256, 0, 0}},
			{{256, 205, 0, 1, 0}, {256, 256, 256, 1, 1}, {0, 256, 256, 0, 1}, {0, 205, 0, 0, 0}},
		},
	};
	/* WINDING RULE (W3-r1b, Fable W3 review): CaveWii's front-face winding is
	   the mirror of Beta's -- Beta renderBottomFace emits (minX,maxZ)->
	   (minX,minZ)->(maxX,minZ)->(maxX,maxZ), CaveWii SIDE_BOTTOM (x,z)->(x,z+h)
	   ->(x+w,z+h)->(x+w,z) (render_block_side_adv_v2), with glFrontFace(GL_CW)
	   (pc/gfx.c) / GX_CULL_BACK (wii/gfx.c). Fire is single-sided
	   (block_fire.c double_sided = false), so a quad in Beta's literal order
	   faces UP and is culled from below. Emitted slot i must be Beta vertex
	   3 - i, position and UV together. Light is per CORNER, like SIDE_BOTTOM:
	   (x,z) -> vertex_light[0], (x+1,z) 1, (x+1,z+1) 2, (x,z+1) 3; main() sets
	   LIGHT[c] low nibble = c + 1 (fire luminance 0 here). */
	const int parity = (x + y + 1 + z) & 1;
	const int bx = (x & 15) * 256, by = (y & 15) * 256, bz = (z & 15) * 256;
	for(int quad = 0; quad < 2; quad++)
		for(int i = 0; i < 4; i++) {
			const int* bv = beta_ceiling[parity][quad][3 - i];
			const struct cap_vtx* q = &g_cap[quad * 4 + i];
			int corner = bv[2] ? (bv[0] ? 2 : 3) : (bv[0] ? 1 : 0);
			int want_s = 3 + quad * 54 + bv[3] * 16;
			int want_t = 3 + bv[4] * 16; /* tex_zero: row 0 */
			if(q->x != bx + bv[0] || q->y != by + bv[1] || q->z != bz + bv[2]
			   || q->s != want_s || q->t != want_t || (q->light & 0x0F) != corner + 1) {
				fprintf(stderr,
						"FAIL ceiling (%d,%d,%d) quad=%d slot=%d got (%d,%d,%d) U=%u V=%u "
						"light=%02x want (%d,%d,%d) U=%d V=%d light corner %d\n",
						x, y, z, quad, i, q->x - bx, q->y - by, q->z - bz, q->s, q->t,
						q->light, bv[0], bv[1], bv[2], want_s, want_t, corner);
				assert(0);
			}
		}
	/* And the winding really faces DOWN under CaveWii's rule: SIDE_BOTTOM's
	   own first three corners (x,z)->(x,z+h)->(x+w,z+h) give a cross product
	   with -Y; the emitted ceiling quads must match its sign. */
	for(int quad = 0; quad < 2; quad++) {
		const struct cap_vtx* q = &g_cap[quad * 4];
		long ax = q[1].x - q[0].x, az = q[1].z - q[0].z;
		long cx = q[2].x - q[0].x, cz = q[2].z - q[0].z;
		long ny = az * cx - ax * cz; /* (a x c).y */
		/* SIDE_BOTTOM reference: a = (0,0,h), c = (w,0,h) -> ny = h*w > 0. */
		assert(ny > 0 && "ceiling quad winding faces up (culled from below)");
	}

	/* A supporting block takes the free-standing branch. Its two planes per
	   side retain their fixed Beta tile choice at every coordinate. */
	for(enum side side = SIDE_LEFT; side <= SIDE_BACK; side++) {
		if(side == SIDE_TOP || side == SIDE_BOTTOM)
			continue;
		bi = mk(x, y, z, BLOCK_FIRE, 0);
		g_neighbours[SIDE_BOTTOM].type = BLOCK_PLANKS;
		g_cap_n = 0;
		assert(render_block_fire(NULL, &bi, side, NULL, LIGHT, true) == 2);
		assert(render_block_fire(NULL, &bi, side, NULL, LIGHT, false) == 2);
		assert(g_cap_n == 8);
		int z_facing = side == SIDE_FRONT || side == SIDE_BACK;
		check_quad(free_edge_hi, "free outer", x, y, z, side, 0, !z_facing, 0, 0);
		check_quad(free_inset_hi, "free inner", x, y, z, side, 1, z_facing, 0, 0);
	}
}

/* ---- F1b (Opus F1 review, MED): per-quad CELL selection golden ----------
   The flip golden above derives each quad's tile from the same formulas the
   renderer's helpers encode. This block pins the literal U ORIGIN of every
   emitted quad for the REAL fire texture (block_fire.c TEXTURE_INDEX(3,0)):
   57 = TEX_OFFSET(3) = anim.png cols 3/10, Beta's fire tile (var9);
   111 = TEX_OFFSET(6) = cols 6/13, Beta's fire+16 tile (var9+16, the second
   TextureFlamesFX). The origin is min U over the quad's 4 corners, so it does
   not depend on the U flip; the flip only decides WHICH corners carry +16,
   and every quad must carry exactly two at origin and two at origin+16.
   Hand-written expectations, not computed from flame_fx_block_*_cell:
     RenderBlocks.java:798-803 (clinging to flammable neighbours): every side
       quad is fire+16 iff (x+y+z & 1) == 1 -> parity 0 = 57, parity 1 = 111.
     :865-874 (flammable ceiling): quad 0 fire (57), quad 1 fire+16 (111).
     :907-944 (free-standing): inner-X fire, inner-Z fire+16, outer-X still
       fire+16 (no reset), outer-Z fire again. CaveWii emits per side (outer
       edge quad, inset inner quad); LEFT/RIGHT are the X planes, FRONT/BACK
       the Z planes -> X sides {111, 57}, Z sides {57, 111}. A swap of
       tex_x_outer_x/tex_x_inner_x (or the _z pair) in render_block_fire
       turns these red. */
static uint8_t tex_fire(struct block_info* bi, enum side side) {
	(void)bi; (void)side;
	return TEXTURE_INDEX(3, 0); // block_fire.c getTextureIndex
}
#define U_FIRE 57	 // TEX_OFFSET(3): anim.png col 3 (frames 14.. at col 10)
#define U_FIRE16 111 // TEX_OFFSET(6): anim.png col 6 (frames 14.. at col 13)

static void check_origin(const char* label, int x, int y, int z, enum side side,
						 int quad, int want) {
	int lo = 255, n_lo = 0, n_hi = 0;
	for(int i = 0; i < 4; i++)
		if(g_cap[quad * 4 + i].s < lo)
			lo = g_cap[quad * 4 + i].s;
	for(int i = 0; i < 4; i++) {
		int s = g_cap[quad * 4 + i].s;
		n_lo += s == lo;
		n_hi += s == lo + 16;
	}
	if(lo != want || n_lo != 2 || n_hi != 2) {
		fprintf(stderr,
				"FAIL cell %s (%d,%d,%d) side=%d quad=%d U origin=%d want=%d "
				"(corners at origin %d, at origin+16 %d)\n",
				label, x, y, z, side, quad, lo, want, n_lo, n_hi);
		assert(lo == want && n_lo == 2 && n_hi == 2);
	}
}

static void mesh(struct block_info* bi, enum side side, size_t quads) {
	g_cap_n = 0;
	assert(render_block_fire(NULL, bi, side, NULL, LIGHT, true) == quads);
	assert(render_block_fire(NULL, bi, side, NULL, LIGHT, false) == quads);
	assert(g_cap_n == (int)quads * 4);
}

static void check_cell_selection(void) {
	static struct block fire_real = {.getTextureIndex = tex_fire, .luminance = 15};
	struct block* saved = blocks[BLOCK_FIRE];
	blocks[BLOCK_FIRE] = &fire_real;
	static const enum side sides[4] = {SIDE_LEFT, SIDE_RIGHT, SIDE_FRONT, SIDE_BACK};
	int checked = 0;

	/* :798-803, floating: parity 0 and parity 1 (incl. a negative coordinate,
	   Java two's-complement & like C). */
	static const struct { int x, y, z, want; } floating[] = {
		{0, 0, 0, U_FIRE}, {1, 1, 0, U_FIRE}, {2, 0, 0, U_FIRE},
		{1, 0, 0, U_FIRE16}, {0, 1, 2, U_FIRE16}, {-1, 0, 0, U_FIRE16},
	};
	for(size_t p = 0; p < sizeof floating / sizeof floating[0]; p++)
		for(int i = 0; i < 4; i++) {
			struct block_info bi = mk(floating[p].x, floating[p].y, floating[p].z,
									  BLOCK_FIRE, 0);
			g_neighbours[blocks_side_opposite(sides[i])].type = BLOCK_PLANKS;
			mesh(&bi, sides[i], 1);
			check_origin("floating", floating[p].x, floating[p].y, floating[p].z,
						 sides[i], 0, floating[p].want);
			checked++;
		}

	/* :865-874, ceiling: quad 0 fire, quad 1 fire+16, at either parity. */
	for(int x = 0; x < 2; x++) {
		struct block_info bi = mk(x, 0, 0, BLOCK_FIRE, 0);
		g_neighbours[SIDE_TOP].type = BLOCK_PLANKS;
		mesh(&bi, SIDE_BOTTOM, 2);
		check_origin("ceiling", x, 0, 0, SIDE_BOTTOM, 0, U_FIRE);
		check_origin("ceiling", x, 0, 0, SIDE_BOTTOM, 1, U_FIRE16);
		checked += 2;
	}

	/* :907-944, free-standing: {outer, inner} per side, at either parity. */
	static const int free_want[SIDE_MAX][2] = {
		[SIDE_LEFT] = {U_FIRE16, U_FIRE},  // outer-X fire+16, inner-X fire
		[SIDE_RIGHT] = {U_FIRE16, U_FIRE},
		[SIDE_FRONT] = {U_FIRE, U_FIRE16}, // outer-Z fire, inner-Z fire+16
		[SIDE_BACK] = {U_FIRE, U_FIRE16},
	};
	for(int x = 0; x < 2; x++)
		for(int i = 0; i < 4; i++) {
			struct block_info bi = mk(x, 0, 0, BLOCK_FIRE, 0);
			g_neighbours[SIDE_BOTTOM].type = BLOCK_PLANKS;
			mesh(&bi, sides[i], 2);
			check_origin("free outer", x, 0, 0, sides[i], 0, free_want[sides[i]][0]);
			check_origin("free inner", x, 0, 0, sides[i], 1, free_want[sides[i]][1]);
			checked += 2;
		}

	blocks[BLOCK_FIRE] = saved;
	printf("PASS fire cell selection golden (%d quads: 57 fire / 111 fire+16)\n",
		   checked);
}

int main(void) {
	static struct block flammable = {.getTextureIndex = tex_zero, .flammable = true};
	blocks[BLOCK_FIRE] = &mock_block;
	blocks[BLOCK_PLANKS] = &flammable;
	for(int c = 0; c < 4; c++)
		LIGHT[c] = (uint8_t)(c + 1); /* per-corner ceiling light check */
	check_cell_selection();
	const int pos[][3] = {
		{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0},
		{0, 2, 0}, {0, 0, 2}, {2, 2, 2}, {-1, 0, 0}, {-2, 0, 0}, {-3, 0, 0},
	};
	for(size_t i = 0; i < sizeof pos / sizeof pos[0]; i++)
		check_at(pos[i][0], pos[i][1], pos[i][2]);
	puts("PASS fire block UV flip golden");
	return 0;
}
