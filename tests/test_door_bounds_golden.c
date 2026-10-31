/* Door bounds golden (Bucket A block rules + the render that must sit on them).

   Maintainer report 10-28 (real Wii): "you couldn't click the door to open or
   close it, you had to find an invisible outline to the side". Beta has ONE
   door box per metadata; the selection box, the collision box, the ray-trace
   box AND the rendered slab are all that same box:

     BlockDoor.java:50-58   getSelectedBoundingBoxFromPool / getCollisionBoundingBoxFromPool
                            both call setBlockBoundsBasedOnState first
     BlockDoor.java:175-178 collisionRayTrace -> setBlockBoundsBasedOnState
     BlockDoor.java:60-62   setBlockBoundsBasedOnState -> setDoorRotation(getState(meta))
     BlockDoor.java:180-182 getState(m) = (m & 4) == 0 ? (m - 1) & 3 : m & 3
     BlockDoor.java:64-83   setDoorRotation, f = 3/16:
                              0: (0,0,0)-(1,1,f)      z in [0, 3/16]
                              1: (1-f,0,0)-(1,1,1)    x in [13/16, 1]
                              2: (0,0,1-f)-(1,1,1)    z in [13/16, 1]
                              3: (0,0,0)-(f,1,1)      x in [0, 3/16]
     RenderBlocks.java:2548  renderBlockDoor draws the six faces of the block's
                            CURRENT bounds (renderBlockByRenderType sets them via
                            setBlockBoundsBasedOnState first), so render == box.

   Expected values below are derived by hand from those lines, never from our
   code. Bit 3 (upper half) does not change the state: (8+d-1)&3 == (d-1)&3.
   World axes = Beta axes (+x east, +z south; SIDE_LEFT = -x, SIDE_FRONT = -z).

   Checks, for all 16 metadata values and both door types:
     - getBoundingBox(entity=false) (selection / ray-trace box) == Beta box
     - getBoundingBox(entity=true)  (collision box)             == Beta box
     - the vertex extent of every face render_block_door emits  == Beta box
*/
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/aabb.h"
#include "block/blocks.h"
#include "block/redstone_power.h"
#include "graphics/render_block.h"
#include "graphics/texture_atlas.h"
#include "platform/displaylist.h"
#include "platform/gfx.h"
#include "platform/texture.h"
#include "game/game_state.h"
#include "item/items.h"
#include "entity/entity.h"
#include "graphics/render_item.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "block/redstone_server.h"
#include "util.h"

extern struct block block_wooden_door;
extern struct block block_iron_door;

/* ---- captured vertex stream (render_block.c emits through this). */
#define MAX_CAP 256
static int16_t g_cap[MAX_CAP][3];
static uint8_t g_cap_st[MAX_CAP][2];
static int g_cap_n;
void displaylist_vertex(struct displaylist* l, int16_t x, int16_t y, int16_t z,
						uint8_t light, uint8_t s, uint8_t t) {
	(void)l; (void)light;
	if(g_cap_n < MAX_CAP) {
		g_cap[g_cap_n][0] = x;
		g_cap[g_cap_n][1] = y;
		g_cap[g_cap_n][2] = z;
		g_cap_st[g_cap_n][0] = s;
		g_cap_st[g_cap_n][1] = t;
		g_cap_n++;
	}
}

/* ---- Beta golden: setDoorRotation(getState(meta)), BlockDoor.java:64-83/180-182. */
static int beta_get_state(int m) {
	return (m & 4) == 0 ? ((m - 1) & 3) : (m & 3);
}
static struct AABB beta_door_box(int meta) {
	const float f = 3.0F / 16.0F;
	switch(beta_get_state(meta)) {
		case 0: return (struct AABB) {0.0F, 0.0F, 0.0F, 1.0F, 1.0F, f};
		case 1: return (struct AABB) {1.0F - f, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
		case 2: return (struct AABB) {0.0F, 0.0F, 1.0F - f, 1.0F, 1.0F, 1.0F};
		default: return (struct AABB) {0.0F, 0.0F, 0.0F, f, 1.0F, 1.0F};
	}
}

static int g_fail;
static int g_broad_checked, g_edge_checked, g_edge_uv_gap;

/* ---- Beta face UVs, in texels of the door's 16x16 tile (tex_atlas_lookup is stubbed to
   tile 0, whose texel origin is TEX_OFFSET(0) = 3 on both axes).
   Texture choice / mirror: BlockDoor.getBlockTextureFromSideAndMetadata (BlockDoor.java:18-36)
     sides 0/1 -> base; else v = getState(meta):
       if ((v == 0 || v == 2) ^ side <= 3) -> base, unmirrored (the 3/16 edge faces);
       else (the two broad faces) mirrored iff (v/2 + ((side & 1) ^ v) + (meta & 4)/4) & 1;
     RenderBlocks.renderBlockDoor (:2591-2618 and the x faces after it) turns a negative
     texture into flipTexture = true for that one face.
   Per-vertex U/V of each face, bounds [lo,hi] in block units (RenderBlocks.java):
     renderEastFace  z=minZ (side 2, SIDE_FRONT) :2832-2903  U = 16*(minX+maxX-x)
     renderWestFace  z=maxZ (side 3, SIDE_BACK)  :2921-2992  U = 16*x
     renderNorthFace x=minX (side 4, SIDE_LEFT)  :3010-3081  U = 16*z
     renderSouthFace x=maxX (side 5, SIDE_RIGHT) :3099-3170  U = 16*(minZ+maxZ-z)
       all four: V = 16*(1-y); flipTexture swaps var12/var14, i.e. U -> 16*(lo+hi) - U.
     renderBottomFace :2668-2730 / renderTopFace :2750-2812  U = 16*x, V = 16*z.
   (Beta's -0.01 texel max inset is a sampling guard, not a texel choice; ignored.) */
static const int k_beta_side[SIDE_MAX] = {
	[SIDE_TOP] = 1, [SIDE_BOTTOM] = 0, [SIDE_FRONT] = 2,
	[SIDE_BACK] = 3, [SIDE_LEFT] = 4, [SIDE_RIGHT] = 5,
};
static bool beta_face_mirrored(int meta, int bside) {
	if(bside == 0 || bside == 1)
		return false;
	int v = (meta & 4) == 0 ? ((meta - 1) & 3) : (meta & 3);
	if((v == 0 || v == 2) ^ (bside <= 3))
		return false;
	int k = v / 2 + ((bside & 1) ^ v) + (meta & 4) / 4;
	return (k & 1) != 0;
}
/* Expected (u,v) texel for a vertex at block-local position p of face `side`. */
static void beta_face_uv(int meta, enum side side, struct AABB b, const float p[3],
						 int* u, int* v) {
	float fu, fv, lo, hi;
	switch(side) {
		case SIDE_FRONT: fu = 16.0F * (b.x1 + b.x2 - p[0]); lo = b.x1; hi = b.x2;
			fv = 16.0F * (1.0F - p[1]); break;
		case SIDE_BACK: fu = 16.0F * p[0]; lo = b.x1; hi = b.x2;
			fv = 16.0F * (1.0F - p[1]); break;
		case SIDE_LEFT: fu = 16.0F * p[2]; lo = b.z1; hi = b.z2;
			fv = 16.0F * (1.0F - p[1]); break;
		case SIDE_RIGHT: fu = 16.0F * (b.z1 + b.z2 - p[2]); lo = b.z1; hi = b.z2;
			fv = 16.0F * (1.0F - p[1]); break;
		default: fu = 16.0F * p[0]; fv = 16.0F * p[2]; lo = hi = 0.0F; break;
	}
	if(beta_face_mirrored(meta, k_beta_side[side]))
		fu = 16.0F * (lo + hi) - fu;
	*u = (int)lroundf(fu);
	*v = (int)lroundf(fv);
}
static bool near(float a, float b) { return fabsf(a - b) < 1e-5F; }
static bool box_eq(struct AABB a, struct AABB b) {
	return near(a.x1, b.x1) && near(a.y1, b.y1) && near(a.z1, b.z1)
		&& near(a.x2, b.x2) && near(a.y2, b.y2) && near(a.z2, b.z2);
}
static void report(const char* what, const char* name, int meta, struct AABB got,
				   struct AABB want) {
	printf("FAIL %s %s meta=%2d (state %d, open=%d, top=%d): got "
		   "x[%.4f,%.4f] y[%.4f,%.4f] z[%.4f,%.4f] want x[%.4f,%.4f] "
		   "y[%.4f,%.4f] z[%.4f,%.4f]\n",
		   name, what, meta, beta_get_state(meta), (meta & 4) != 0,
		   (meta & 8) != 0, got.x1, got.x2, got.y1, got.y2, got.z1, got.z2,
		   want.x1, want.x2, want.y1, want.y2, want.z1, want.z2);
	g_fail++;
}

static struct block_data g_neighbours[SIDE_MAX];
static bool world_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* out) {
	(void)ctx; (void)x; (void)y; (void)z;
	*out = (struct block_data) {0};
	return false;
}
static struct block_info mk(struct block_data* store, uint8_t type, uint8_t meta) {
	for(int i = 0; i < SIDE_MAX; i++)
		g_neighbours[i] = (struct block_data) {.type = BLOCK_AIR};
	*store = (struct block_data) {.type = type, .metadata = meta};
	struct block_info bi = {0};
	bi.block = store;
	bi.neighbours = g_neighbours;
	bi.x = 0;
	bi.y = 0;
	bi.z = 0;
	bi.get_block = world_get;
	return bi;
}

static void check_door(const char* name, struct block* b, uint8_t type) {
	static uint8_t light[24];
	for(int meta = 0; meta < 16; meta++) {
		struct AABB want = beta_door_box(meta);
		struct block_data store;
		struct block_info bi = mk(&store, type, (uint8_t)meta);

		/* Selection / ray-trace box and collision box. */
		for(int entity = 0; entity < 2; entity++) {
			struct AABB got;
			memset(&got, 0, sizeof got);
			assert(b->getBoundingBox(&bi, entity != 0, &got) == 1);
			if(!box_eq(got, want))
				report(entity ? "collision" : "selection", name, meta, got, want);
		}

		/* Rendered slab: union of every emitted face's vertices (1/256 units). */
		g_cap_n = 0;
		for(int side = 0; side < SIDE_MAX; side++) {
			int first = g_cap_n;
			render_block_door(NULL, &bi, (enum side)side, NULL, light, false);
			/* Face UV check, ASSERTED on every face: every vertex must sample Beta's
			   texel. The broad faces carry the hinge mirror; the 3/16 edge strips and
			   top/bottom sample the bounds-clipped columns (e.g. 13..16). Card P3
			   DOOR-EDGE-UV closed the old gap (critique-1028 RETURN-DOOR.md: texels
			   0..3 on every strip, bottom face rotated 180 degrees). */
			int bside = k_beta_side[side];
			int st = beta_get_state(meta);
			bool broad = bside >= 2 && !((st == 0 || st == 2) ^ (bside <= 3));
			for(int i = first; i < g_cap_n; i++) {
				float p[3] = {g_cap[i][0] / 256.0F, g_cap[i][1] / 256.0F,
							  g_cap[i][2] / 256.0F};
				int eu, ev;
				beta_face_uv(meta, (enum side)side, want, p, &eu, &ev);
				int gu = (int)g_cap_st[i][0] - 3, gv = (int)g_cap_st[i][1] - 3;
				if(gu == eu && gv == ev)
					continue;
				if(!broad)
					g_edge_uv_gap++;
				printf("FAIL %s uv meta=%2d (state %d, open=%d) side %d "
					   "(beta %d, mirrored=%d) vtx (%.4f,%.4f,%.4f): got "
					   "u%d v%d want u%d v%d\n",
					   name, meta, st, (meta & 4) != 0, side, bside,
					   beta_face_mirrored(meta, bside), p[0], p[1], p[2], gu, gv,
					   eu, ev);
				g_fail++;
			}
			if(broad)
				g_broad_checked += g_cap_n - first;
			else
				g_edge_checked += g_cap_n - first;
		}
		assert(g_cap_n > 0 && g_cap_n < MAX_CAP);
		int lo[3] = {32767, 32767, 32767}, hi[3] = {-32768, -32768, -32768};
		for(int i = 0; i < g_cap_n; i++)
			for(int a = 0; a < 3; a++) {
				if(g_cap[i][a] < lo[a]) lo[a] = g_cap[i][a];
				if(g_cap[i][a] > hi[a]) hi[a] = g_cap[i][a];
			}
		struct AABB rend = {lo[0] / 256.0F, lo[1] / 256.0F, lo[2] / 256.0F,
							hi[0] / 256.0F, hi[1] / 256.0F, hi[2] / 256.0F};
		if(!box_eq(rend, want))
			report("render", name, meta, rend, want);
	}
}

int main(void) {
	blocks[BLOCK_DOOR_WOOD] = &block_wooden_door;
	blocks[BLOCK_DOOR_IRON] = &block_iron_door;
	check_door("wood", &block_wooden_door, BLOCK_DOOR_WOOD);
	check_door("iron", &block_iron_door, BLOCK_DOOR_IRON);
	if(g_fail) {
		printf("test_door_bounds_golden: %d mismatches vs Beta (%d of them edge-strip/"
			   "top/bottom UV vertices)\n",
			   g_fail, g_edge_uv_gap);
		return 1;
	}
	assert(g_broad_checked > 0 && g_edge_checked > 0 && g_edge_uv_gap == 0);
	printf("test_door_bounds_golden: PASS (2 door types x 16 metadata x "
		   "selection+collision+render == BlockDoor.setDoorRotation(getState(meta)); "
		   "%d broad-face vertices sample Beta's hinge-mirrored texel; "
		   "%d edge-strip/top/bottom vertices sample Beta's bounds-clipped texel; "
		   "0 UV mismatches)\n",
		   g_broad_checked, g_edge_checked);
	return 0;
}

/* ---- link-only stubs for symbols the never-called parts of render_block.c and
   block_door.c reference (same rationale as test_fire_block_uv_flip.c). */
struct block* blocks[256];
bool block_is_normal_cube(uint8_t type) { (void)type; return false; }
bool block_beta_material_is_solid(int id) { (void)id; return false; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry name) { (void)name; return 0; }
void redstone_wire_connections(const struct redstone_power_world* w, int x, int y, int z,
							   bool conn[4]) {
	(void)w; (void)x; (void)y; (void)z;
	conn[0] = conn[1] = conn[2] = conn[3] = false;
}
struct game_state gstate;
struct tex_gfx texture_terrain;
void displaylist_init(struct displaylist* l, size_t v, size_t s) { (void)l; (void)v; (void)s; }
void displaylist_reset(struct displaylist* l) { (void)l; }
void displaylist_render_immediate(struct displaylist* l, uint16_t n) { (void)l; (void)n; }
void gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void gfx_blending(enum gfx_blend m) { (void)m; }
void gfx_depth_func(enum depth_func f) { (void)f; }
void gfx_cull_func(enum cull_func f) { (void)f; }
void gfx_alpha_test(bool enable) { (void)enable; }
void gfx_alpha_test_ref(bool enable, float ref) { (void)enable; (void)ref; }
void gfx_write_buffers(bool color, bool depth, bool depth_test) {
	(void)color; (void)depth; (void)depth_test;
}
void gfx_depth_range(float near_, float far_) { (void)near_; (void)far_; }
void gfx_matrix_modelview(mat4 m) { (void)m; }
uint32_t hash_u32(uint32_t x) { return x; }
enum side blocks_side_opposite(enum side s) {
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
struct world* world_view_current(void) { return NULL; }

/* block_door.c: server-side handlers (never called -- only getBoundingBox is). */
bool block_place_default(struct server_local* s, struct server_player* actor,
						 struct item_data* it, struct block_info* where,
						 struct block_info* on, enum side on_side) {
	(void)s; (void)actor; (void)it; (void)where; (void)on; (void)on_side;
	return false;
}
struct server_world* block_world(struct server_local* s) { (void)s; return NULL; }
bool entity_local_player_block_collide(vec3 pos, struct block_info* blk_info) {
	(void)pos; (void)blk_info; return false;
}
struct face_occlusion* face_occlusion_empty() { return NULL; }
float rand_gen_flt(struct random_gen* g) { (void)g; return 0.0F; }
bool redstone_id_can_provide_power(int id) { (void)id; return false; }
bool redstone_server_indirectly_getting_powered(struct server_local* s, int x, int y, int z) {
	(void)s; (void)x; (void)y; (void)z; return false;
}
void render_item_flat(struct item* item, struct item_data* stack, mat4 view,
					  bool fullbright, enum render_item_env env) {
	(void)item; (void)stack; (void)view; (void)fullbright; (void)env;
}
void server_local_emit_sound(uint16_t sound_id, vec3 pos, float vol, float pitch) {
	(void)sound_id; (void)pos; (void)vol; (void)pitch;
}
struct random_gen* server_local_sound_rng(void) { return NULL; }
void server_local_spawn_block_drops(struct server_local* s, struct block_info* blk_info) {
	(void)s; (void)blk_info;
}
bool server_world_get_block(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z, struct block_data* blk) {
	(void)w; (void)x; (void)y; (void)z; (void)blk; return false;
}
uint8_t server_world_notify_origin(void) { return 0; }
bool server_world_set_block(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
							struct block_data blk) {
	(void)w; (void)x; (void)y; (void)z; (void)blk; return false;
}
