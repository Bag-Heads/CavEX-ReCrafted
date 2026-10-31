/*
	Golden test for FH-04/FH-05/FH-06 (docs/fork_hack_audit_2026-08-21.md): three render_block.c
	renderers (portal, stairs corner-riser, fence post/rail) that either culled faces on the wrong
	predicate or sampled a bare, un-derived texture-tile origin instead of the Beta-faithful
	bounds-proportional crop. This pins the FIXED behaviour so neither regresses back to the old
	fork-inherited hack.

	FH-04 -- render_block_portal must implement Beta BlockPortal.shouldSideBeRendered EXACTLY
	[decompile 740c583]:
	  shouldSideBeRendered(world, x, y, z, side):
	      false when id(x,y,z) is portal itself;
	      w1 = id(x-1,y,z)==portal && id(x-2,y,z)!=portal;
	      e1 = id(x+1,y,z)==portal && id(x+2,y,z)!=portal;
	      n1 = id(x,y,z-1)==portal && id(x,y,z-2)!=portal;
	      s1 = id(x,y,z+1)==portal && id(x,y,z+2)!=portal;
	      alongX = w1||e1, alongZ = n1||s1;
	      alongX && side==4 -> true;   // -X
	      else alongX && side==5 -> true; // +X
	      else alongZ && side==2 -> true; // -Z
	      else alongZ && side==3.           // +Z
	  }
	For a valid Beta portal frame (always exactly 2 wide x 3 tall) this reduces to: the face facing
	directly into another portal cell (interior) NEVER renders, the frame-edge face along the WIDTH
	axis never renders either (alongX/alongZ both resolve false there), and the two faces along the
	THICKNESS axis (the visible pane surface, front AND back) ALWAYS render. This test proves that
	reduction against the real predicate, not just the mirrored formula, by driving
	render_block_portal itself over a tiny world grid.

	FH-05/FH-06 -- render_block_stairs' corner-riser quad and render_block_fence's post TOP/BOTTOM
	faces and fence_always's rail quads must sample a UV origin cropped proportionally to the box's
	OWN bounds, per RenderBlocks.renderEastFace/renderNorthFace/renderTopFace/renderBottomFace
	[decompile 740c583]:
	  u = tileBaseU + minAxis*16          (X for Z-fixed faces / top-bottom; Z for X-fixed faces)
	  v = tileBaseV + 16 - maxY*16        (side faces only; top/bottom crop v = minZ*16, NOT inverted)
	  and Beta's own overflow guard when a bound exceeds [0,1] -- e.g. renderEastFace:
	    a box minX < 0 or maxX > 1 sets u0 = base+0, u1 = base+15.99;
	  -- falls back to the UNSHIFTED full tile, which is exactly the CaveWii fence rail's case (its
	  long axis deliberately spans a whole inter-post gap, >1 block).
	This test derives the expected origin independently from each quad's known geometry (documented
	inline, NOT copied from the render_block.c fix) and reads back the ACTUAL emitted texcoords via
	a captured displaylist_vertex stream, so it exercises the real, wired-up renderer.

	Linking: render_block.c only (matches test_redstone_render_count.c's precedent); this test
	never calls the wire/lever renderers so their one real dependency (redstone_wire_connections)
	is a link-only stub, same rationale as that file's block_beta_material_is_solid stub.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"		   // struct block / block_info / block_data, enum side, BLOCK_*
#include "block/redstone_power.h" // struct redstone_power_world (link-only stub signature)
#include "graphics/render_block.h" // render_block_portal / _stairs / _fence / _fence_always
#include "graphics/texture_atlas.h" // TEXTURE_X / TEXTURE_Y
#include "platform/displaylist.h"
#include "platform/gfx.h"		   // enum gfx_blend / depth_func / cull_func, mat4
#include "platform/texture.h"	   // struct tex_gfx (texture_terrain)
#include "game/game_state.h"	   // struct game_state (gstate), struct window_container
#include "item/items.h"			   // struct item, item_get

// ---- mock world: a 16^3 id grid, same convention as test_redstone_wire_render_connections /
//      test_redstone_render_count. Only the portal predicate needs world lookups.
#define G 16
static uint8_t g_id[G][G][G];

static bool in_world(int x, int y, int z) {
	return x >= 0 && x < G && y >= 0 && y < G && z >= 0 && z < G;
}
static void clear_world(void) { memset(g_id, 0, sizeof g_id); }
static void put(int x, int y, int z, int id) { g_id[x][y][z] = (uint8_t)id; }
static bool world_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* out) {
	(void)ctx;
	if(!in_world(x, y, z)) {
		*out = (struct block_data) {0};
		return false;
	}
	*out = (struct block_data) {.type = g_id[x][y][z], .metadata = 0};
	return true;
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

// ---- captured vertex stream: unlike test_redstone_render_count's count-only stub, this test
//      needs the actual emitted texcoords to check the UV-origin fix.
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
// A quad's 4 corners span [u0, u0+span] x [v0, v0+span] regardless of the per-side vertex/
// rotation order (render_block_side_adv_v2's tex_coords table), so MIN(s)/MIN(t) recovers the
// origin robustly.
static void quad_uv_origin(int start, uint8_t* u0, uint8_t* v0) {
	uint8_t mu = g_cap[start].s, mv = g_cap[start].t;
	for(int i = start + 1; i < start + 4; i++) {
		if(g_cap[i].s < mu)
			mu = g_cap[i].s;
		if(g_cap[i].t < mv)
			mv = g_cap[i].t;
	}
	*u0 = mu;
	*v0 = mv;
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

// blocks[]: a single deterministic mock (getTextureIndex always returns atlas tile 0, so every
// face's tile-base is TEX_OFFSET(TEXTURE_X(0)) == TEX_OFFSET(TEXTURE_Y(0)) == 3 -- known,
// reproducible, and irrelevant to the UV-ORIGIN math this test checks).
static uint8_t tex_zero(struct block_info* bi, enum side side) {
	(void)bi; (void)side;
	return 0;
}
static struct block mock_block = {.getTextureIndex = tex_zero, .luminance = 0};
struct block* blocks[256];

static uint8_t LIGHT[24];
static struct block_data g_self;
static struct block_data g_neighbours[SIDE_MAX];
// A SEPARATE backing store for check_portal's `it` (the tested neighbour): render_block_portal
// still reads `this->block->type` in its emit-pass getTextureIndex call, so `it` must NOT alias
// `this`'s storage -- constructing `it` via mk() into a shared global would clobber `this`'s
// block type out from under the later render_block_portal(..., bi, ...) emit call (NULL-deref
// via blocks[stale/wrong type] the moment a "should render" case actually draws).
static struct block_data g_it_self;

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

// ============================================================================================
// FH-04: portal shouldSideBeRendered
// ============================================================================================

static void check_portal(const char* name, struct block_info* bi, w_coord_t nx, w_coord_t ny,
						 w_coord_t nz, enum side side, size_t want) {
	struct block_info it = mk_into(&g_it_self, nx, ny, nz, g_id[nx][ny][nz], 0);
	size_t qc = render_block_portal(NULL, bi, side, &it, LIGHT, true);
	g_cap_n = 0;
	size_t qe = render_block_portal(NULL, bi, side, &it, LIGHT, false);
	if(qc != want || qe != want || (size_t)(g_cap_n / 4) != qe) {
		fprintf(stderr, "FAIL %s: count=%zu emit=%zu verts=%d, want %zu\n", name, qc, qe,
				g_cap_n, want);
		assert(0);
	}
}

static void test_portal(void) {
	blocks[BLOCK_PORTAL] = &mock_block;

	// X-oriented 2-wide portal: columns at x=8 and x=9, obsidian frame at x=7/x=10.
	clear_world();
	put(8, 8, 8, BLOCK_PORTAL);
	put(9, 8, 8, BLOCK_PORTAL);
	put(7, 8, 8, BLOCK_OBSIDIAN);
	put(10, 8, 8, BLOCK_OBSIDIAN);
	struct block_info bi = mk(8, 8, 8, BLOCK_PORTAL, 0);

	// Interior face (neighbour is the OTHER portal column): early-out, never renders.
	check_portal("X-portal interior (E, portal neighbour)", &bi, 9, 8, 8, SIDE_RIGHT, 0);
	// Frame-edge face (neighbour is the obsidian frame): var10 resolves false for an exact
	// 2-wide frame (Beta's own dead-code branch), so this ALSO never renders.
	check_portal("X-portal frame edge (W, obsidian neighbour)", &bi, 7, 8, 8, SIDE_LEFT, 0);
	// Thickness axis (both Z faces, open air beyond): var11 true, always renders.
	check_portal("X-portal thickness front (open air)", &bi, 8, 8, 7, SIDE_FRONT, 1);
	check_portal("X-portal thickness back (open air)", &bi, 8, 8, 9, SIDE_BACK, 1);
	// Top/bottom: Beta's override never branches on side 0/1 -- always false.
	check_portal("X-portal top", &bi, 8, 9, 8, SIDE_TOP, 0);
	check_portal("X-portal bottom", &bi, 8, 7, 8, SIDE_BOTTOM, 0);

	// Z-oriented 2-wide portal (the other axis): columns at z=8/z=9, obsidian at z=7/z=10.
	clear_world();
	put(8, 8, 8, BLOCK_PORTAL);
	put(8, 8, 9, BLOCK_PORTAL);
	put(8, 8, 7, BLOCK_OBSIDIAN);
	put(8, 8, 10, BLOCK_OBSIDIAN);
	bi = mk(8, 8, 8, BLOCK_PORTAL, 0);
	// Now the THICKNESS axis is X: both X faces (open air) always render.
	check_portal("Z-portal thickness left (open air)", &bi, 7, 8, 8, SIDE_LEFT, 1);
	check_portal("Z-portal thickness right (open air)", &bi, 9, 8, 8, SIDE_RIGHT, 1);
	// The frame-width axis (Z) never renders: interior (portal neighbour) and edge (obsidian).
	check_portal("Z-portal interior (S, portal neighbour)", &bi, 8, 8, 9, SIDE_BACK, 0);
	check_portal("Z-portal frame edge (N, obsidian neighbour)", &bi, 8, 8, 7, SIDE_FRONT, 0);

	// get_block == NULL (the world-less item/inventory-icon path, e.g. render_item.c's bare
	// BLOCK_AIR neighbour with no accessor -- unreachable for portal in practice since it has
	// no ItemBlock, but the fallback branch must still degrade sanely, not go fully invisible).
	// `it_dummy` is built into its OWN storage (mk_into, not mk()) so it doesn't alias/clobber
	// bi_noworld's block_data -- see g_it_self's comment; this exercises the emit pass too
	// (count_only=false), which DOES read this->block->type via getTextureIndex.
	struct block_info bi_noworld = mk(8, 8, 8, BLOCK_PORTAL, 0);
	bi_noworld.get_block = NULL;
	struct block_info it_dummy = mk_into(&g_it_self, 9, 8, 8, BLOCK_AIR, 0);
	assert(render_block_portal(NULL, &bi_noworld, SIDE_FRONT, &it_dummy, LIGHT, true) == 1);
	assert(render_block_portal(NULL, &bi_noworld, SIDE_BACK, &it_dummy, LIGHT, true) == 1);
	assert(render_block_portal(NULL, &bi_noworld, SIDE_LEFT, &it_dummy, LIGHT, true) == 0);
	assert(render_block_portal(NULL, &bi_noworld, SIDE_RIGHT, &it_dummy, LIGHT, true) == 0);
	assert(render_block_portal(NULL, &bi_noworld, SIDE_TOP, &it_dummy, LIGHT, true) == 0);
	g_cap_n = 0;
	assert(render_block_portal(NULL, &bi_noworld, SIDE_FRONT, &it_dummy, LIGHT, false) == 1
		   && g_cap_n == 4);

	printf("  portal (FH-04): interior/frame-edge suppressed, thickness-axis pane renders both "
		   "sides, both frame orientations, top/bottom always off, get_block==NULL fallback -- "
		   "OK\n");
}

// ============================================================================================
// FH-05: stairs corner-riser UV
// ============================================================================================

// Independently re-derives the expected corner-riser UV origin from Beta's
// renderEastFace/renderNorthFace proportional crop (u = tileBase + minAxis*16), NOT by
// referencing render_block.c's off[][] table: the corner riser always occupies the "tall box"
// half of the footprint on the block's `facing` side (Y always [0.5,1], so V is always the tile
// top, +0) and, on the in-plane horizontal axis, either the near half [0,0.5] (u +0) or far half
// [0.5,1] (u +8 texels of a 16-texel tile) depending purely on which side of the block is tall.
static void check_stairs_corner(const char* name, uint8_t meta, enum side perp_side,
								uint8_t expect_u_off) {
	g_cap_n = 0;
	struct block_info bi = mk(8, 8, 8, BLOCK_WOODEN_STAIRS, meta);
	struct block_info it = bi; // unused by render_block_stairs
	size_t k = render_block_stairs(NULL, &bi, perp_side, &it, LIGHT, false);
	// side != facing/opposite(facing)/BOTTOM/TOP draws BOTH the low-slab half-height quad (the
	// first "render slab" branch) and the corner-riser quad (the second branch) -- 2 quads.
	assert(k == 2 && g_cap_n == 8);
	uint8_t u0, v0;
	quad_uv_origin(4, &u0, &v0); // vertices 4..7 = the SECOND quad = the corner riser
	uint8_t base = TEX_OFFSET(TEXTURE_X(0));
	uint8_t expect_u = (uint8_t)(base + expect_u_off);
	uint8_t expect_v = base; // Y:[0.5,1] -> v = base + 16 - maxY*16 = base + 0, always
	if(u0 != expect_u || v0 != expect_v) {
		fprintf(stderr, "FAIL %s: corner UV (%u,%u), want (%u,%u)\n", name, u0, v0, expect_u,
				expect_v);
		assert(0);
	}
}

static void test_stairs(void) {
	blocks[BLOCK_WOODEN_STAIRS] = &mock_block;

	// facing metadata 0 = SIDE_RIGHT -> tall box on the RIGHT/+X half -> X:[0.5,1] -> u +8.
	check_stairs_corner("facing RIGHT, corner on FRONT", 0, SIDE_FRONT, 8);
	check_stairs_corner("facing RIGHT, corner on BACK", 0, SIDE_BACK, 8);
	// facing metadata 1 = SIDE_LEFT -> tall box on the LEFT/-X half -> X:[0,0.5] -> u +0.
	check_stairs_corner("facing LEFT, corner on FRONT", 1, SIDE_FRONT, 0);
	check_stairs_corner("facing LEFT, corner on BACK", 1, SIDE_BACK, 0);
	// facing metadata 2 = SIDE_BACK -> tall box on the BACK/+Z half -> Z:[0.5,1] -> u +8.
	check_stairs_corner("facing BACK, corner on LEFT", 2, SIDE_LEFT, 8);
	check_stairs_corner("facing BACK, corner on RIGHT", 2, SIDE_RIGHT, 8);
	// facing metadata 3 = SIDE_FRONT -> tall box on the FRONT/-Z half -> Z:[0,0.5] -> u +0.
	check_stairs_corner("facing FRONT, corner on LEFT", 3, SIDE_LEFT, 0);
	check_stairs_corner("facing FRONT, corner on RIGHT", 3, SIDE_RIGHT, 0);

	printf("  stairs corner-riser (FH-05): bounds-derived UV (near half +0 / far half +8 texels) "
		   "over all 4 facings x both perpendicular sides -- OK\n");
}

// ============================================================================================
// FH-06: fence post + rail UV
// ============================================================================================

static void test_fence_post(void) {
	blocks[BLOCK_FENCE] = &mock_block;
	uint8_t base = TEX_OFFSET(TEXTURE_X(0));

	struct block_info bi = mk(8, 8, 8, BLOCK_FENCE, 0);
	struct block_info it = bi;

	// Post bounds X/Z:[0.375,0.625] (6..10 sixteenths), Y full. TOP/BOTTOM crop u=minX*16=6,
	// v=minZ*16=6 (renderTopFace/renderBottomFace do NOT invert v, unlike the side faces).
	g_cap_n = 0;
	assert(render_block_fence(NULL, &bi, SIDE_TOP, &it, LIGHT, false) == 1 && g_cap_n == 4);
	uint8_t u0, v0;
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == (uint8_t)(base + 6) && v0 == (uint8_t)(base + 6));

	g_cap_n = 0;
	assert(render_block_fence(NULL, &bi, SIDE_BOTTOM, &it, LIGHT, false) == 1 && g_cap_n == 4);
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == (uint8_t)(base + 6) && v0 == (uint8_t)(base + 6));

	// LEFT (a fixed-X face): u crops by Z bounds (minZ*16=6), v crops by Y bounds inverted --
	// full height Y:[0,1] -> v = 16 - 16 = 0. Was already correct pre-fix; pinned as a
	// regression guard alongside the TOP/BOTTOM fix.
	g_cap_n = 0;
	assert(render_block_fence(NULL, &bi, SIDE_LEFT, &it, LIGHT, false) == 1 && g_cap_n == 4);
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == (uint8_t)(base + 6) && v0 == base);

	printf("  fence post (FH-06): TOP/BOTTOM bounds-derived UV (6,6), LEFT regression-pinned -- "
		   "OK\n");
}

static void test_fence_rail(void) {
	blocks[BLOCK_FENCE] = &mock_block;
	uint8_t base = TEX_OFFSET(TEXTURE_X(0));

	struct block_info bi = mk(8, 8, 8, BLOCK_FENCE, 0);
	struct block_info it = bi;

	// SIDE_LEFT/RIGHT rail (connect_pos_z): a fixed-X face whose in-plane axes are Z (rail
	// length, this port's own geometry: z-offset 10/16, width 3/4 block -> spans [10,22]/16,
	// i.e. OUT of [0,1] -- Beta's own renderEastFace overflow guard therefore falls back to the
	// UNSHIFTED tile, u = base+0) and Y (rail height, which stays IN [0,1] and crops normally:
	// upper rail Y:[12,15]/16 -> v = 16-15 = 1; lower rail Y:[6,9]/16 -> v = 16-9 = 7).
	g_neighbours[SIDE_BACK] = (struct block_data) {.type = BLOCK_FENCE, .metadata = 0};
	g_cap_n = 0;
	assert(render_block_fence_always(NULL, &bi, SIDE_LEFT, &it, LIGHT, false) == 2
		   && g_cap_n == 8);
	uint8_t u0, v0;
	quad_uv_origin(0, &u0, &v0); // upper rail
	assert(u0 == base && v0 == (uint8_t)(base + 1));
	quad_uv_origin(4, &u0, &v0); // lower rail
	assert(u0 == base && v0 == (uint8_t)(base + 7));

	// SIDE_RIGHT is the mirrored face of the same rail -- identical Z/Y bounds, identical UV.
	g_cap_n = 0;
	assert(render_block_fence_always(NULL, &bi, SIDE_RIGHT, &it, LIGHT, false) == 2
		   && g_cap_n == 8);
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == base && v0 == (uint8_t)(base + 1));
	quad_uv_origin(4, &u0, &v0);
	assert(u0 == base && v0 == (uint8_t)(base + 7));

	// SIDE_BOTTOM, connect_pos_x only: this rail's in-plane axes are X (length, OUT of [0,1] --
	// same overflow fallback, u = base+0) and Z (the rail's cross-section, IN bounds: Z:[7,9]/16
	// -> v = minZ*16 = 7, NOT inverted for the top/bottom face formula).
	memset(g_neighbours, 0, sizeof g_neighbours);
	for(int i = 0; i < SIDE_MAX; i++)
		g_neighbours[i].type = BLOCK_AIR;
	g_neighbours[SIDE_RIGHT] = (struct block_data) {.type = BLOCK_FENCE, .metadata = 0};
	g_cap_n = 0;
	assert(render_block_fence_always(NULL, &bi, SIDE_BOTTOM, &it, LIGHT, false) == 2
		   && g_cap_n == 8);
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == base && v0 == (uint8_t)(base + 7));
	quad_uv_origin(4, &u0, &v0);
	assert(u0 == base && v0 == (uint8_t)(base + 7));

	// SIDE_TOP mirrors SIDE_BOTTOM's UV for the same connect_pos_x rail.
	g_cap_n = 0;
	assert(render_block_fence_always(NULL, &bi, SIDE_TOP, &it, LIGHT, false) == 2
		   && g_cap_n == 8);
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == base && v0 == (uint8_t)(base + 7));
	quad_uv_origin(4, &u0, &v0);
	assert(u0 == base && v0 == (uint8_t)(base + 7));

	// SIDE_BOTTOM, connect_pos_z only: axes swap -- X is the cross-section (X:[7,9]/16 ->
	// u = minX*16 = 7) and Z is the overflowing length axis (v = base+0).
	for(int i = 0; i < SIDE_MAX; i++)
		g_neighbours[i].type = BLOCK_AIR;
	g_neighbours[SIDE_BACK] = (struct block_data) {.type = BLOCK_FENCE, .metadata = 0};
	g_cap_n = 0;
	assert(render_block_fence_always(NULL, &bi, SIDE_BOTTOM, &it, LIGHT, false) == 2
		   && g_cap_n == 8);
	quad_uv_origin(0, &u0, &v0);
	assert(u0 == (uint8_t)(base + 7) && v0 == base);
	quad_uv_origin(4, &u0, &v0);
	assert(u0 == (uint8_t)(base + 7) && v0 == base);

	printf("  fence rails (FH-06): bounds-derived UV for LEFT/RIGHT (upper v+1/lower v+7, "
		   "overflow-fallback u+0) and BOTTOM/TOP connect_pos_x (u+0,v+7) / connect_pos_z "
		   "(u+7,v+0) -- OK\n");
}

int main(void) {
	test_portal();
	test_stairs();
	test_fence_post();
	test_fence_rail();

	printf("test_render_block_beta_uv_golden: OK (FH-04 portal shouldSideBeRendered predicate + "
		   "FH-05 stairs corner-riser UV + FH-06 fence post/rail UV all bounds-derived per the "
		   "real Beta decompile, verified against the actual emitted displaylist vertices)\n");
	return 0;
}
