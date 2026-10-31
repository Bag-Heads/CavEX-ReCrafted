/*
	Wii display-list SIZING guard for the redstone-wire + lever renderers.

	On the Wii the chunk display list is exactly sized: the count_only pass returns a
	quad/vertex count, the emit pass MUST write exactly that many. Under NDEBUG (asserts
	stripped) a count_only != emit mismatch stamps a GX_QUADS header claiming more/fewer
	quads than were written -> the GP parses garbage and can WEDGE (the frozen-no-dump
	hang) -- the same freeze family as the open piston-freeze and "redstone broken on
	real Wii" render-corruption bugs (see displaylist.h `truncated`, CLAUDE.md).

	render_block_redstone_wire returns `1 + nvface` quads (1 flat top quad + one vertical
	"climbing" quad per connected wall) and render_block_lever returns 2 for EVERY side (1
	cobblestone base-box quad, always present -- a proper 6-face box, one face per side call,
	same shape as render_block_button -- plus 1 handle quad, whichever of the handle's 6
	Beta-model faces lands on the requested side for the lever's current orientation; every
	orientation covers all 6 sides exactly once, see s_lever_handle_face_side in
	render_block.c). Both hold today only BY CONSTRUCTION (the count is computed before
	the `if(count_only) return` early-out, the emit path then draws exactly that). This test
	pins it: for every wire topology (dot / line / L / T / + and 0..4 climbing faces) and
	every lever orientation x on/off x side, it drives BOTH passes against a mock displaylist
	and asserts count == emit AND emit-return*4 == the actual displaylist_vertex call count.

	It is the render-side analogue of test_displaylist_vertex_equiv, at the per-block
	render-count level the project's Wii-sizing rule demands. The renderer is linked for real
	(render_block.c); only the leaf engine symbols the two functions touch are mocked, and
	--gc-sections drops render_block.c's other functions (and their gfx/gstate/inventory refs).

	conn index (redstone_wire_connections): 0=west(-X) 1=east(+X) 2=north(-Z) 3=south(+Z).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"		   // struct block / block_info / block_data, enum side, BLOCK_*
#include "block/redstone_power.h"  // RSP_* ids
#include "graphics/render_block.h" // render_block_redstone_wire / render_block_lever
#include "graphics/texture_atlas.h"
#include "platform/displaylist.h"
#include "platform/gfx.h"		   // enum gfx_blend / depth_func / cull_func, mat4
#include "platform/texture.h"	   // struct tex_gfx (texture_terrain)
#include "game/game_state.h"	   // struct game_state (gstate), struct window_container
#include "item/items.h"			   // struct item, item_get
#include "item/tool.h"			   // tool_dig_crack_stage

// ---- mock world: a 16^3 id/meta grid, same convention as test_redstone_wire_render_connections.
#define G 16
#define STONE_ID 1 // a normal opaque cube (floor / wall / wire support)
static uint8_t g_id[G][G][G];
static uint8_t g_meta[G][G][G];

static bool in_world(int x, int y, int z) {
	return x >= 0 && x < G && y >= 0 && y < G && z >= 0 && z < G;
}
static void clear_world(void) {
	memset(g_id, 0, sizeof g_id);
	memset(g_meta, 0, sizeof g_meta);
}
static void put(int x, int y, int z, int id, int meta) {
	g_id[x][y][z] = (uint8_t)id;
	g_meta[x][y][z] = (uint8_t)meta;
}
// block_info.get_block backing (the diagonal-aware accessor the wire connection probe uses).
static bool world_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* out) {
	(void)ctx;
	if(!in_world(x, y, z)) {
		*out = (struct block_data) {0};
		return false; // outside the world == not available
	}
	*out = (struct block_data) {.type = g_id[x][y][z], .metadata = g_meta[x][y][z]};
	return true;
}

// ---- leaf engine symbols the two renderers reference (everything else is --gc-section'd).
bool block_is_normal_cube(uint8_t type) {
	return type == STONE_ID; // only the mock stone is a full opaque cube
}
/* Link-only dependency of render_block_fluid; this redstone test never calls it. */
bool block_beta_material_is_solid(int id) { return id == STONE_ID; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry name) {
	(void)name;
	return 0; // 0 != the wire's getTextureIndex sentinel below -> wire_tex_is_break()==false
}
static int g_verts; // displaylist_vertex call count == vertices the emit pass wrote
void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t) {
	(void)l; (void)x; (void)y; (void)z; (void)light; (void)s; (void)t;
	g_verts++;
}

// ---- link-only stubs: symbols referenced ONLY by render_block.c's other (never-called)
//      functions -- block_cracks_texture / render_block_cross / _stairs / render_block_init.
//      The default Windows bfd ld crashes on --gc-sections for this target, so rather than
//      rely on dead-code elimination we satisfy these explicitly; this test never enters the
//      code paths that use them.
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
enum side blocks_side_opposite(enum side s) { return s; }
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

// blocks[]: only WIRE + LEVER, whose getTextureIndex returns a non-break sentinel so the
// wire takes its real shape/connection path (not the flat block-cracks quad).
static uint8_t tex_sentinel(struct block_info* bi, enum side side) {
	(void)bi; (void)side;
	return 99; // any value tex_atlas_lookup(TEXAT_BREAK_*) never returns here (we return 0)
}
static struct block mock_wire = {.getTextureIndex = tex_sentinel, .luminance = 0};
static struct block mock_lever = {.getTextureIndex = tex_sentinel, .luminance = 0};
struct block* blocks[256];

static uint8_t LIGHT[24];
static struct block_data g_self;

static struct block_info mk(int x, int y, int z, uint8_t type, uint8_t meta) {
	g_self = (struct block_data) {.type = type, .metadata = meta};
	struct block_info bi = {0};
	bi.block = &g_self;
	bi.neighbours = NULL;
	bi.x = x; bi.y = y; bi.z = z;
	bi.get_block = world_get;
	bi.get_block_ctx = NULL;
	return bi;
}

// count_only == emit, AND emit-return * 4 == vertices actually emitted.
static void assert_balanced(const char* what, size_t qc, size_t qe) {
	if(qc != qe || (int)(qe * 4) != g_verts) {
		fprintf(stderr,
				"FAIL %s: count=%zu emit=%zu vertices=%d (want count==emit and "
				"vertices==emit*4)\n",
				what, qc, qe, g_verts);
		assert(0);
	}
}

// `want` = expected quad count (1 flat top quad + nvface vertical climbs). Pinning it proves
// the climb path is actually EXERCISED (a trivially-1-quad pass would otherwise hide it).
static void check_wire(const char* name, size_t want) {
	struct block_info bi = mk(8, 8, 8, BLOCK_REDSTONE_WIRE, 0);
	struct block_info it = bi; // unused by the wire renderer, but pass a valid one
	size_t qc = render_block_redstone_wire(NULL, &bi, SIDE_TOP, &it, LIGHT, true);
	g_verts = 0;
	size_t qe = render_block_redstone_wire(NULL, &bi, SIDE_TOP, &it, LIGHT, false);
	assert_balanced(name, qc, qe);
	if(qe != want) {
		fprintf(stderr, "FAIL %s: %zu quads, expected %zu\n", name, qe, want);
		assert(0);
	}
}

// centre wire on a stone floor (baseline each case)
static void base_wire(void) {
	clear_world();
	put(8, 7, 8, STONE_ID, 0);				// floor under the centre wire
	put(8, 8, 8, BLOCK_REDSTONE_WIRE, 0);	// the wire under test
}
static void wire_at(int x, int y, int z) { // a floored wire neighbour at the same level
	put(x, y - 1, z, STONE_ID, 0);
	put(x, y, z, BLOCK_REDSTONE_WIRE, 0);
}
// a stone cube carrying wire on top (a vertical "stair-up climb" neighbour)
static void climb_at(int x, int y, int z) {
	put(x, y, z, STONE_ID, 0);
	put(x, y + 1, z, BLOCK_REDSTONE_WIRE, 0);
}

int main(void) {
	blocks[BLOCK_REDSTONE_WIRE] = &mock_wire;
	blocks[BLOCK_LEVER] = &mock_lever;

	// --- wire: flat topologies (nvface == 0 -> exactly 1 quad) ---------------------------
	base_wire();									check_wire("dot", 1);
	base_wire(); wire_at(7,8,8); wire_at(9,8,8);	check_wire("line E-W", 1);
	base_wire(); wire_at(8,8,7); wire_at(8,8,9);	check_wire("line N-S", 1);
	base_wire(); wire_at(9,8,8); wire_at(8,8,9);	check_wire("L-corner", 1);
	base_wire(); wire_at(7,8,8); wire_at(9,8,8); wire_at(8,8,9);	check_wire("T-junction", 1);
	base_wire(); wire_at(7,8,8); wire_at(9,8,8); wire_at(8,8,7); wire_at(8,8,9);
	check_wire("plus cross", 1);

	// --- wire: vertical climbing faces (nvface 1..4 -> 1 + nvface quads). Needs an OPEN
	//     ceiling over the centre wire (no block at 8,9,8) + a stone cube neighbour with wire
	//     on top on each climbed side. The expected counts (2..5) PROVE the climb path runs. --
	base_wire(); climb_at(9,8,8);								check_wire("climb x1 (E)", 2);
	base_wire(); climb_at(7,8,8); climb_at(9,8,8);				check_wire("climb x2 (W+E)", 3);
	base_wire(); climb_at(7,8,8); climb_at(9,8,8); climb_at(8,8,7);	check_wire("climb x3", 4);
	base_wire(); climb_at(7,8,8); climb_at(9,8,8); climb_at(8,8,7); climb_at(8,8,9);
	check_wire("climb x4 (all sides)", 5);

	// closed ceiling over the centre suppresses ALL climbs -> back to 1 quad
	base_wire(); climb_at(9,8,8); put(8,9,8,STONE_ID,0);		check_wire("climb suppressed by ceiling", 1);

	// non-top sides emit nothing in either pass
	{
		struct block_info bi = mk(8,8,8, BLOCK_REDSTONE_WIRE, 0), it = bi;
		for(enum side s = SIDE_TOP; s < SIDE_MAX; s++) {
			if(s == SIDE_TOP) continue;
			g_verts = 0;
			size_t qe = render_block_redstone_wire(NULL, &bi, s, &it, LIGHT, false);
			size_t qc = render_block_redstone_wire(NULL, &bi, s, &it, LIGHT, true);
			assert(qc == 0 && qe == 0 && g_verts == 0);
		}
	}

	// --- lever: every orientation (meta&7 = 1..6) x ON/OFF x side ------------------------
	for(int orient = 1; orient <= 6; orient++) {
		for(int on = 0; on <= 1; on++) {
			uint8_t meta = (uint8_t)(orient | (on ? 8 : 0));
			for(enum side s = SIDE_TOP; s < SIDE_MAX; s++) {
				struct block_info bi = mk(8,8,8, BLOCK_LEVER, meta), it = bi;
				size_t qc = render_block_lever(NULL, &bi, s, &it, LIGHT, true);
				g_verts = 0;
				size_t qe = render_block_lever(NULL, &bi, s, &it, LIGHT, false);
				char nm[64];
				snprintf(nm, sizeof nm, "lever o%d %s side%d", orient, on ? "on" : "off", (int)s);
				assert_balanced(nm, qc, qe);
				// every side draws exactly 2 quads: the cobblestone base-box face (always
				// present) plus the handle face that lands on this side for this orientation
				assert(qe == 2);
			}
		}
	}

	printf("test_redstone_render_count: OK (wire count==emit for dot/line/L/T/+ and 0..4 "
		   "climbing faces; lever 2 quads/side (base box + handle) across all orientations+on/off; "
		   "vertices==emit*4 -- the Wii display-list sizing invariant)\n");
	return 0;
}
