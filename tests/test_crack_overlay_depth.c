/*
	parity-render2-0924/G1 -- the dig-progress crack overlay must be visible
	across the WHOLE face of an alpha-tested block (glass, leaves, ...), the way
	Beta draws it, and must stay exactly as visible on an opaque block.

	THE DEFECT (tmp/jobs/push-0921/h1-capture/NOTES.md runs 2-4: a submerged
	bare-handed glass dig ran to completion, the block broke, and not one frame
	showed a crack): render_block_cracks_owned drew the overlay with the depth
	function EQUAL. EQUAL only passes where the depth buffer holds the FACE's
	own depth -- and an alpha-tested face never writes depth where its texels
	were discarded (the see-through middle of a glass pane), so the crack
	fragments there compared against whatever lay BEHIND the pane and were
	rejected. Only the pane's opaque 1-px rim could ever show a crack.

	WHAT BETA DOES (the spec this test is written against), decompile at
	<repo-analysis>/mcb173-retromcp/minecraft/
	src_original/net/minecraft/src/RenderGlobal.java drawBlockBreaking:

	  :1078  glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)      (= MODE_BLEND3 here)
	  :1086  glPolygonOffset(-3.0F, -3.0F)
	  :1087  glEnable(GL_POLYGON_OFFSET_FILL)            crack pulled TOWARD the eye
	  :1095  glEnable(GL_ALPHA_TEST)                      the tile's clear texels drop
	  :1099  renderBlockUsingTexture(.., 240 + stage)     the WHOLE block, every face
	  :1103  glPolygonOffset(0.0F, 0.0F)
	  :1104  glDisable(GL_POLYGON_OFFSET_FILL)            offset restored
	  :1106  glDepthMask(true)
	  and the depth FUNCTION is never touched: it stays GL_LEQUAL, the default
	  EntityRenderer never changes. So Beta's rule is "LEQUAL + a small
	  toward-the-eye bias", which passes on the face itself (bias >= 0), passes
	  on the discarded texels (what lies behind is farther), and still fails
	  against anything nearer than the bias (no cracks through walls).

	WHAT THIS ENGINE HAS: no polygon offset on either backend (GX has none), but
	one established cross-platform depth-offset mechanism -- gfx_depth_range,
	the remap both backends implement (glDepthRange / GX_SetViewport near-far)
	and the same tool the held item (screen_ingame.c) and the GUI layers
	(gui_util.c) already use to place a draw in front of what is under it.
	The crack pass therefore renders with LEQUAL inside a depth range shrunk by
	a tiny bias, then restores (0,1) and LEQUAL.

	HOW THIS TEST SEES IT: it links the real render_block.c (the same
	self-contained pattern as test_redstone_render_count.c: link-only stubs for
	the leaf engine symbols) against a RECORDING gfx sink that snapshots the
	depth function / depth range / blend / alpha-test / cull state at the
	moment displaylist_render_immediate is called, and then runs a small
	fixed-function depth-test oracle over that snapshot:

	    window_z(frag) = near + frag * (far - near)        (GL glDepthRange and
	                                                        GX viewport z scale
	                                                        + offset are this
	                                                        same affine map)
	    pass = (func == EQUAL) ? q(window_z) == q(stored)
	                           : q(window_z) <= q(stored)  with q() the 24-bit
	                                                        quantisation of the
	                                                        PC (GLFW default)
	                                                        and EFB depth buffers

	against three depth-buffer contents for a face at several depths:
	  (a) the face's OWN depth        -- an opaque texel: must pass (was passing)
	  (b) something FARTHER           -- a discarded (see-through) glass texel:
	                                     must pass. THIS IS THE RED CASE: EQUAL
	                                     rejects it.
	  (c) something NEARER            -- a wall in front: must NOT pass, before
	                                     and after (no cracks through walls).
	The face's stored depth in (a) is produced by the same map with the default
	range, because the chunk pass and the crack pass build the identical
	modelview (chunk.c:754 and render_block.c both glm_translate_to the chunk
	origin) -- that bit-identity is what let EQUAL work on opaque blocks at all.

	The opaque-block cases assert the same three outcomes so the fix can only
	ADD visibility on discarded texels, never change what an opaque block shows.
	Every state the pass touches must be back to its entry value afterwards
	(depth func LEQUAL, range (0,1), blend OFF, cull BACK, alpha test on) --
	the GX FIFO-safety reset discipline of render_block.c / render_entity.c.

	If you change the render-state sequence in render_block_cracks_owned,
	change the expectations below with a citation.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "this test must be compiled with asserts enabled"
#endif

#include "block/blocks.h"		   // struct block / block_info / block_data, enum side
#include "block/redstone_power.h"  // struct redstone_power_world (link-only stub below)
#include "game/game_state.h"	   // struct game_state (gstate), struct client_session
#include "graphics/render_block.h" // render_block_cracks_owned
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "item/tool.h"
#include "platform/displaylist.h"
#include "platform/gfx.h"
#include "platform/texture.h"

/* ---- the recording gfx sink ---------------------------------------------- */

struct gfx_state {
	enum depth_func func;
	float near, far;
	enum gfx_blend blend;
	bool alpha_test;
	enum cull_func cull;
	bool depth_write;
	float alpha_ref; /* -1 = the engine default cutoff (gfx_alpha_test) */
};

/* Entry state = what the frame leaves for render3D: world pass 0 exits with
   blending OFF and the alpha test ON (world.c:2271-2272), the default depth
   range (0,1) and LEQUAL (pc/gfx.c:294, wii/gfx.c:608), back-face culling. */
static const struct gfx_state ENTRY = {MODE_LEQUAL, 0.0f, 1.0f, MODE_OFF, true, MODE_BACK, true, -1.0f};
static struct gfx_state g_cur;

static int g_draws;
static struct gfx_state g_at_draw;
static uint16_t g_vtx_at_draw;
static int g_range_calls; // every gfx_depth_range call, so a no-op pass is visible

void gfx_depth_func(enum depth_func f) { g_cur.func = f; }
void gfx_depth_range(float near, float far) {
	g_cur.near = near;
	g_cur.far = far;
	g_range_calls++;
}
void gfx_blending(enum gfx_blend m) { g_cur.blend = m; }
void gfx_alpha_test(bool enable) { g_cur.alpha_test = enable; g_cur.alpha_ref = -1.0f; }
void gfx_alpha_test_ref(bool enable, float ref) { g_cur.alpha_test = enable; g_cur.alpha_ref = ref; }
void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	(void)color; (void)depth_test; g_cur.depth_write = depth;
}
void gfx_cull_func(enum cull_func f) { g_cur.cull = f; }
void gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void gfx_matrix_modelview(mat4 m) { (void)m; }
void displaylist_render_immediate(struct displaylist* l, uint16_t n) {
	(void)l;
	g_at_draw = g_cur;
	g_vtx_at_draw = n;
	g_draws++;
}

/* ---- link-only stubs (symbols referenced by render_block.c's OTHER,
        never-entered functions; the Windows bfd ld crashes on --gc-sections
        for this target, so they are satisfied explicitly) ----------------- */
struct game_state gstate;
struct tex_gfx texture_terrain;
void displaylist_init(struct displaylist* l, size_t v, size_t s) { (void)l; (void)v; (void)s; }
void displaylist_reset(struct displaylist* l) { (void)l; }
void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t) {
	(void)l; (void)x; (void)y; (void)z; (void)light; (void)s; (void)t;
}
bool block_is_normal_cube(uint8_t type) { (void)type; return false; }
bool block_beta_material_is_solid(int id) { (void)id; return false; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry name) { (void)name; return 0; }
uint32_t hash_u32(uint32_t x) { return x; }
enum side blocks_side_opposite(enum side s) { return s; }
void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	(void)s; if(x) *x = 0; if(y) *y = 0; if(z) *z = 0;
}
bool inventory_get_hotbar_item(struct inventory* inv, struct item_data* it) {
	(void)inv; (void)it; return false;
}
struct item* item_get(struct item_data* it) { (void)it; return NULL; }
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
	struct block_data b = {0}; // air neighbours: every face of the dug block is exposed
	return b;
}
/* A2-W: render_block.c reads the drawn view's world (world.c) */
struct world* world_view_current(void) {
	return NULL;
}
// Link-only: this test never calls render_block_redstone_wire.
void redstone_wire_connections(const struct redstone_power_world* w, int x, int y, int z,
							   bool conn[4]) {
	(void)w; (void)x; (void)y; (void)z;
	conn[0] = conn[1] = conn[2] = conn[3] = false;
}

/* ---- the blocks under dig ------------------------------------------------ */

/* One quad per face, like a full cube; the crack pass only counts the return
   value (its displaylist_vertex is the no-op stub above). What distinguishes
   glass from stone for THIS test is not geometry -- both are full cubes -- but
   what the depth buffer holds under the face, modelled in main(). */
static size_t one_quad(struct displaylist* d, struct block_info* this, enum side side,
					   struct block_info* it, uint8_t* light, bool count_only) {
	(void)d; (void)this; (void)side; (void)it; (void)light; (void)count_only;
	return 1;
}

static struct block mock_glass = {
	.name = "glass",
	.renderBlock = one_quad,
	.transparent = true, // block_glass.c: alpha-tested see-through texels
	.double_sided = false,
	.digging = {.hardness = 450},
};
static struct block mock_stone = {
	.name = "stone",
	.renderBlock = one_quad,
	.transparent = false,
	.double_sided = false,
	.digging = {.hardness = 2250},
};
static struct block mock_double = { // a double-sided block flips the cull mode
	.name = "double",
	.renderBlock = one_quad,
	.double_sided = true,
	.digging = {.hardness = 100},
};
struct block* blocks[256];
enum { ID_GLASS = 20, ID_STONE = 1, ID_DOUBLE = 18 };

/* ---- the fixed-function depth oracle ------------------------------------- */

#define Z24 16777215.0f // 24-bit depth buffer: GLFW's default on PC, the EFB's on Wii

static uint32_t q24(float z) { return (uint32_t)floorf(z * Z24 + 0.5f); }

static float window_z(const struct gfx_state* s, float frag) {
	return s->near + frag * (s->far - s->near);
}

/* Does a crack fragment whose face lies at `frag` (default-range window depth)
   survive the depth test against a buffer holding `stored`? */
static bool depth_pass(const struct gfx_state* s, float frag, float stored) {
	uint32_t zc = q24(window_z(s, frag));
	uint32_t zs = q24(stored);
	return s->func == MODE_EQUAL ? zc == zs : zc <= zs;
}

static long g_checks;
static void expect(bool cond, const char* what) {
	g_checks++;
	if(!cond) {
		fprintf(stderr, "FAIL: %s\n", what);
		fflush(stderr);
	}
	assert(cond);
}

static void run_pass(uint8_t id, const char* name) {
	struct block_data blk = {.type = id, .metadata = 0};
	mat4 view;
	glm_mat4_identity(view);
	g_cur = ENTRY;
	g_draws = 0;
	g_range_calls = 0;
	render_block_cracks_owned(&gstate.players[0], &blk, view, 5, 64, 7);
	printf("%-7s draw: func=%s range=(%.9g,%.9g) blend=%d alpha=%d cull=%d vtx=%u\n",
		   name, g_at_draw.func == MODE_EQUAL ? "EQUAL" : "LEQUAL", g_at_draw.near,
		   g_at_draw.far, (int)g_at_draw.blend, (int)g_at_draw.alpha_test,
		   (int)g_at_draw.cull, (unsigned)g_vtx_at_draw);
	fflush(stdout); // keep the recorded state visible even if an assert aborts next
	expect(g_draws == 1, "exactly one immediate draw per crack pass");
	expect(g_vtx_at_draw == 6 * 4, "a full cube: six quads = 24 vertices");
}

/* The face depths swept: near, mid, and the far-compressed tail where nearly
   every world fragment lives under a perspective projection. */
static const float FACE_Z[] = {0.05f, 0.5f, 0.9f, 0.99f, 0.999f, 0.9999f};
#define N_FACE (sizeof FACE_Z / sizeof FACE_Z[0])

static void check_face_visibility(const char* name, bool alpha_tested) {
	char what[160];
	for(size_t i = 0; i < N_FACE; i++) {
		float z = FACE_Z[i];
		float own = window_z(&ENTRY, z); // what the chunk pass wrote for this face
		float behind = fminf(1.0f, z + 0.001f);
		float nearer = z - 0.001f;

		snprintf(what, sizeof what, "%s: crack passes on the face's own texel at z=%g", name, z);
		expect(depth_pass(&g_at_draw, z, own), what);

		snprintf(what, sizeof what,
				 "%s: crack passes where the face wrote NO depth (see-through texel, "
				 "something farther underneath) at z=%g", name, z);
		if(alpha_tested)
			expect(depth_pass(&g_at_draw, z, behind), what); // RED under EQUAL
		else
			expect(depth_pass(&g_at_draw, z, behind), what); // same rule: Beta does
															   // not branch on block
		snprintf(what, sizeof what, "%s: crack passes against the far plane at z=%g", name, z);
		expect(depth_pass(&g_at_draw, z, 1.0f), what);

		snprintf(what, sizeof what, "%s: crack does NOT draw through a nearer wall at z=%g",
				 name, z);
		expect(!depth_pass(&g_at_draw, z, nearer), what);

		/* The bias: toward the eye (never pushes the crack BEHIND its own
		   face) and small (at most 64 LSB of the 24-bit buffer -- Beta's
		   -3/-3 polygon offset is a few units; a bias large enough to punch
		   through a neighbouring block would be a new defect). */
		float bias = own - window_z(&g_at_draw, z);
		snprintf(what, sizeof what, "%s: bias is toward the eye (>= 0) at z=%g", name, z);
		expect(bias >= 0.0f, what);
		snprintf(what, sizeof what, "%s: bias is bounded (<= 64 LSB) at z=%g", name, z);
		expect(bias <= 64.0f / Z24, what);
	}
	/* And it IS a bias (Beta's polygon offset is non-zero): at the far tail
	   the crack must land strictly nearer than the face it is drawn on. */
	expect(q24(window_z(&g_at_draw, 0.99f)) < q24(window_z(&ENTRY, 0.99f)),
		   "the crack is drawn strictly nearer than its face (non-zero bias)");
}

static void check_draw_state(const char* name, enum cull_func cull_expected) {
	char what[160];
	snprintf(what, sizeof what, "%s: depth func at draw is LEQUAL (Beta never changes it)", name);
	expect(g_at_draw.func == MODE_LEQUAL, what);
	snprintf(what, sizeof what, "%s: blend at draw is DST_COLOR/SRC_COLOR (RenderGlobal:1078)", name);
	expect(g_at_draw.blend == MODE_BLEND3, what);
	snprintf(what, sizeof what, "%s: alpha test ON at draw (RenderGlobal:1095)", name);
	expect(g_at_draw.alpha_test, what);
	/* Opus review F1: Beta's damage pass runs with depth writes OFF
	 * (RenderGlobal.java:1106 restores glDepthMask(true)); a writing crack pass
	 * would hide water/mobs behind glass and cull the later ice pass. */
	expect(!g_at_draw.depth_write, "crack pass must not write depth");
	/* F2: Beta's world alpha cutoff is GL_GREATER 0.1F (EntityRenderer.java:773),
	 * not the engine's 16/255 default. */
	expect(g_at_draw.alpha_ref == 0.1f, "crack alpha cutoff must be Beta's 0.1");
	snprintf(what, sizeof what, "%s: cull mode at draw", name);
	expect(g_at_draw.cull == cull_expected, what);
	snprintf(what, sizeof what, "%s: depth range was actually set for the pass", name);
	expect(g_range_calls >= 1, what);
}

static void check_restored(const char* name) {
	char what[160];
	snprintf(what, sizeof what, "%s: depth func restored to LEQUAL", name);
	expect(g_cur.func == ENTRY.func, what);
	snprintf(what, sizeof what, "%s: depth range restored to (0,1)", name);
	expect(g_cur.near == ENTRY.near && g_cur.far == ENTRY.far, what);
	snprintf(what, sizeof what, "%s: blending restored to OFF", name);
	expect(g_cur.blend == ENTRY.blend, what);
	snprintf(what, sizeof what, "%s: alpha test left ON (Beta :1105 re-enables it)", name);
	expect(g_cur.alpha_test == ENTRY.alpha_test, what);
	snprintf(what, sizeof what, "%s: cull restored to BACK", name);
	expect(g_cur.cull == ENTRY.cull, what);
	expect(g_cur.depth_write == ENTRY.depth_write, "depth writes restored");
	expect(g_cur.alpha_ref == ENTRY.alpha_ref, "default alpha cutoff restored");
}

int main(void) {
	blocks[ID_GLASS] = &mock_glass;
	blocks[ID_STONE] = &mock_stone;
	blocks[ID_DOUBLE] = &mock_double;

	/* Oracle sanity on the ENTRY (pre-fix) state itself: EQUAL passes only on
	   the face's own depth -- the mechanism of the defect, stated once. */
	{
		struct gfx_state old = ENTRY;
		old.func = MODE_EQUAL;
		expect(depth_pass(&old, 0.9f, window_z(&ENTRY, 0.9f)), "oracle: EQUAL passes on own depth");
		expect(!depth_pass(&old, 0.9f, 0.901f), "oracle: EQUAL rejects a farther stored depth");
		expect(!depth_pass(&old, 0.9f, 0.899f), "oracle: EQUAL rejects a nearer stored depth");
	}

	/* --- glass: the alpha-tested block that showed no cracks -------------- */
	run_pass(ID_GLASS, "glass");
	check_face_visibility("glass", true);
	check_draw_state("glass", MODE_BACK);
	check_restored("glass");

	/* --- stone: an opaque block must show exactly what it showed ---------- */
	run_pass(ID_STONE, "stone");
	check_face_visibility("stone", false);
	check_draw_state("stone", MODE_BACK);
	check_restored("stone");

	/* --- a double-sided block: cull NONE at draw, BACK restored ----------- */
	run_pass(ID_DOUBLE, "double");
	check_face_visibility("double", true);
	check_draw_state("double", MODE_NONE);
	check_restored("double");

	/* --- a block with no hardness (undiggable) draws nothing and touches no
	       state at all ------------------------------------------------------ */
	{
		static struct block bedrock = {.name = "bedrock", .renderBlock = one_quad,
									   .digging = {.hardness = 0}};
		blocks[7] = &bedrock;
		struct block_data blk = {.type = 7, .metadata = 0};
		mat4 view;
		glm_mat4_identity(view);
		g_cur = ENTRY;
		g_draws = 0;
		g_range_calls = 0;
		render_block_cracks_owned(&gstate.players[0], &blk, view, 5, 64, 7);
		expect(g_draws == 0 && g_range_calls == 0, "undiggable block: no draw, no state change");
		expect(memcmp(&g_cur, &ENTRY, sizeof g_cur) == 0, "undiggable block: state untouched");
	}

	printf("test_crack_overlay_depth: %ld checks passed\n", g_checks);
	return 0;
}
