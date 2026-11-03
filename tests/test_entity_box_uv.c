/* test_entity_box_uv.c -- every face of a mob/player model box samples the
 * Beta 1.7.3 ModelRenderer.addBox UV rectangle, with Beta's orientation.
 *
 * Oracle (mcb173-retromcp, minecraft/src/net/minecraft/src):
 *   ModelRenderer.java:65-70  addBox builds six TexturedQuads. With texture
 *     offset (u,v) and box (w,h,d):
 *       faces[0] x-max side  rect (u+d+w,   v+d, d, h)
 *       faces[1] x-min side  rect (u,       v+d, d, h)
 *       faces[2] y-min (top) rect (u+d,     v,   w, d)
 *       faces[3] y-max (bot) rect (u+d+w,   v,   w, d)
 *       faces[4] z-min front rect (u+d,     v+d, w, h)
 *       faces[5] z-max back  rect (u+2d+w,  v+d, w, h)
 *   TexturedQuad.java:19-22   vertex 0 -> (u1,v0), 1 -> (u0,v0),
 *                             2 -> (u0,v1), 3 -> (u1,v1).
 *   RenderLiving.java:42      glScalef(-1,-1,1): Java model +X/+Y are
 *                             screen -X/-Y (Y-down model space).
 *
 * The CaveWii model frame is (x_j+8, 24-y_j, z_j+8) px (render_entity_anchor.h)
 * under body rotY(yaw+180); the result on screen matches Beta's
 * glScalef(-1,-1,1)/rotY(180-yaw) chain only when, in CaveWii's own frame,
 *   y-min  (bottom) rect faces[3], u grows with -x, v grows with +z
 *   y-max  (top)    rect faces[2], u grows with -x, v grows with -z
 *   z-max  (back)   rect faces[5], u grows with +x, v grows with -y
 *   z-min  (front)  rect faces[4], u grows with -x, v grows with -y
 *   x-min           rect faces[0], u grows with +z, v grows with -y
 *   x-max           rect faces[1], u grows with -z, v grows with -y
 * (worked from the corner orders above; the hardware-proven biped/pig leg
 * tables already follow exactly this slotting).
 *
 * The model code is compiled intact; displaylist/gfx calls are recording
 * doubles. The FIRST box each model emits after render_entity_init is its
 * head, which is the box every table under test feeds.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/displaylist.h"
#include "platform/gfx.h"
#include "platform/texture.h"
#include "graphics/render_entity.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "entity/entity.h"
#include "entity/combat.h"
#include "cglm/cglm.h"

/* ---- recording doubles (same shape as tests/test_mob_windup_render.py) ---- */
struct tex_gfx texture_armor_chain1, texture_armor_chain2, texture_armor_cloth1,
	texture_armor_cloth2, texture_armor_diamond1, texture_armor_diamond2,
	texture_armor_gold1, texture_armor_gold2, texture_armor_iron1,
	texture_armor_iron2, texture_chicken, texture_cow, texture_creeper,
	texture_ghast, texture_ghast_fire, texture_minecart, texture_mob_char,
	texture_pig, texture_pigzombie, texture_sheep, texture_sheep_fur,
	texture_skeleton, texture_slime, texture_spider, texture_spider_eyes,
	texture_squid, texture_wolf, texture_wolf_angry, texture_wolf_tame,
	texture_zombie;
struct item* items[ITEMS_MAX];

struct vert {
	int16_t x, y, z;
	uint8_t u, v;
	bool has_uv;
};
#define MAX_VERTS 4096
static struct vert recorded[MAX_VERTS];
static size_t nverts;
static int resets;

void displaylist_init(struct displaylist* l, size_t n, size_t size) {
	(void)l; (void)n; (void)size;
}
void displaylist_reset(struct displaylist* l) { (void)l; resets++; }
void displaylist_pos(struct displaylist* l, int16_t x, int16_t y, int16_t z) {
	(void)l;
	assert(nverts < MAX_VERTS);
	recorded[nverts].x = x; recorded[nverts].y = y; recorded[nverts].z = z;
	recorded[nverts].has_uv = false;
	nverts++;
}
void displaylist_color(struct displaylist* l, uint8_t i) { (void)l; (void)i; }
void displaylist_texcoord(struct displaylist* l, uint8_t u, uint8_t v) {
	(void)l;
	assert(nverts > 0);
	recorded[nverts - 1].u = u; recorded[nverts - 1].v = v;
	recorded[nverts - 1].has_uv = true;
}
void displaylist_render_immediate(struct displaylist* l, uint16_t n) { (void)l; (void)n; }
void gfx_matrix_modelview(mat4 mv) { (void)mv; }
void gfx_bind_texture(struct tex_gfx* tex) { (void)tex; }
void gfx_lighting(bool b) { (void)b; }
void gfx_alpha_test(bool b) { (void)b; }
void gfx_blending(enum gfx_blend b) { (void)b; }
void gfx_cull_func(enum cull_func f) { (void)f; }
void gfx_entity_modulate(float r, float g, float b) { (void)r; (void)g; (void)b; }
void gfx_entity_tint(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void gfx_draw_quads(size_t n, const int16_t* p, const uint8_t* c, const uint16_t* uv) {
	(void)n; (void)p; (void)c; (void)uv;
}
void gfx_draw_quads_flt(size_t n, const float* p, const uint8_t* c, const float* uv) {
	(void)n; (void)p; (void)c; (void)uv;
}
void render_item_flat(struct item* item, struct item_data* stack, mat4 mv, bool ui,
					  enum render_item_env env) {
	(void)item; (void)stack; (void)mv; (void)ui; (void)env;
}
void render_item_update_light(uint8_t l) { (void)l; }
void entity_shadow(struct entity* e, struct AABB* a, mat4 view) { (void)e; (void)a; (void)view; }
int underwater_overlay_sample_y(float p, float h, float o) { (void)h; (void)o; return (int)p; }
float light_brightness_current(uint8_t light) { (void)light; return 1.0f; }

/* ---- the oracle ---- */
struct rect { int u, v, w, h; };

/* Beta addBox rects (ModelRenderer.java:65-70), indexed by Java faces[] slot. */
static struct rect beta_face(int u, int v, int w, int h, int d, int slot) {
	switch(slot) {
	case 0: return (struct rect) {u + d + w, v + d, d, h};
	case 1: return (struct rect) {u, v + d, d, h};
	case 2: return (struct rect) {u + d, v, w, d};
	case 3: return (struct rect) {u + d + w, v, w, d};
	case 4: return (struct rect) {u + d, v + d, w, h};
	default: return (struct rect) {u + 2 * d + w, v + d, w, h};
	}
}

/* Which CaveWii-frame side a quad lies on. */
enum box_side { S_YMIN, S_YMAX, S_ZMAX, S_ZMIN, S_XMIN, S_XMAX };
static const char* side_name[6]
	= {"bottom(y-min)", "top(y-max)", "back(z-max)", "front(z-min)", "x-min", "x-max"};
static const int side_slot[6] = {3, 2, 5, 4, 0, 1};

static int failures;
#define CHECK(cond, ...)                                                     \
	do {                                                                     \
		if(!(cond)) {                                                        \
			failures++;                                                      \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
			printf(__VA_ARGS__);                                             \
			printf("\n");                                                    \
		}                                                                    \
	} while(0)

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* Check recorded cube number `box` (24 vertices each, in emission order)
 * against addBox(u,v,w,h,d). */
static void check_cube_at(const char* name, size_t box, int u, int v, int w,
						  int h, int d) {
	const int sw = 256 / 64, sh = 256 / 32; /* render_entity_emit_cube scale */
	const size_t need = (box + 1) * 24;
	CHECK(nverts >= need, "%s: only %zu vertices recorded, need %zu", name,
		  nverts, need);
	if(nverts < need)
		return;
	const struct vert* verts = recorded + box * 24;
	int bx0 = 32767, bx1 = -32768, by0 = 32767, by1 = -32768, bz0 = 32767, bz1 = -32768;
	for(int i = 0; i < 24; i++) {
		CHECK(verts[i].has_uv, "%s: vertex %d has no texcoord", name, i);
		bx0 = imin(bx0, verts[i].x); bx1 = imax(bx1, verts[i].x);
		by0 = imin(by0, verts[i].y); by1 = imax(by1, verts[i].y);
		bz0 = imin(bz0, verts[i].z); bz1 = imax(bz1, verts[i].z);
	}
	CHECK(bx1 - bx0 == w * 16 && by1 - by0 == h * 16 && bz1 - bz0 == d * 16,
		  "%s: box is %dx%dx%d px, expected %dx%dx%d", name, (bx1 - bx0) / 16,
		  (by1 - by0) / 16, (bz1 - bz0) / 16, w, h, d);

	bool seen[6] = {false};
	for(int f = 0; f < 6; f++) {
		const struct vert* q = &verts[f * 4];
		int side = -1;
		if(q[0].y == q[1].y && q[1].y == q[2].y && q[2].y == q[3].y)
			side = q[0].y == by0 ? S_YMIN : S_YMAX;
		else if(q[0].z == q[1].z && q[1].z == q[2].z && q[2].z == q[3].z)
			side = q[0].z == bz0 ? S_ZMIN : S_ZMAX;
		else if(q[0].x == q[1].x && q[1].x == q[2].x && q[2].x == q[3].x)
			side = q[0].x == bx0 ? S_XMIN : S_XMAX;
		CHECK(side >= 0, "%s: quad %d is not axis-aligned", name, f);
		if(side < 0)
			continue;
		CHECK(!seen[side], "%s: side %s emitted twice", name, side_name[side]);
		seen[side] = true;

		struct rect want = beta_face(u, v, w, h, d, side_slot[side]);
		int ulo = 255, uhi = 0, vlo = 255, vhi = 0;
		for(int i = 0; i < 4; i++) {
			ulo = imin(ulo, q[i].u); uhi = imax(uhi, q[i].u);
			vlo = imin(vlo, q[i].v); vhi = imax(vhi, q[i].v);
		}
		/* Compare in the emitter's 8-bit texcoord units. The right/bottom edge
		 * saturates at 255 (render_entity_emit_cube UV_MAX: texel 64 would be
		 * 256) -- a pre-existing emitter precision limit, not a table slot, so
		 * the expectation carries the same clamp. */
		int want_ulo = want.u * sw, want_vlo = want.v * sh;
		int want_uhi = imin(255, (want.u + want.w) * sw);
		int want_vhi = imin(255, (want.v + want.h) * sh);
		CHECK(ulo == want_ulo && vlo == want_vlo && uhi == want_uhi && vhi == want_vhi,
			  "%s %s: rect {%d,%d,%d,%d} texels, Beta faces[%d] wants {%d,%d,%d,%d}", name,
			  side_name[side], ulo / sw, vlo / sh, (uhi - ulo) / sw, (vhi - vlo) / sh,
			  side_slot[side], want.u, want.v, want.w, want.h);

		/* Orientation: which corner of the box carries (u-lo, v-lo) and how
		 * u/v grow across the face (see the header for the derivation). */
		for(int i = 0; i < 4; i++) {
			int eu = 0, ev = 0;
			switch(side) {
			case S_YMIN: eu = q[i].x == bx1 ? ulo : uhi; ev = q[i].z == bz0 ? vlo : vhi; break;
			case S_YMAX: eu = q[i].x == bx1 ? ulo : uhi; ev = q[i].z == bz1 ? vlo : vhi; break;
			case S_ZMAX: eu = q[i].x == bx0 ? ulo : uhi; ev = q[i].y == by1 ? vlo : vhi; break;
			case S_ZMIN: eu = q[i].x == bx1 ? ulo : uhi; ev = q[i].y == by1 ? vlo : vhi; break;
			case S_XMIN: eu = q[i].z == bz0 ? ulo : uhi; ev = q[i].y == by1 ? vlo : vhi; break;
			case S_XMAX: eu = q[i].z == bz1 ? ulo : uhi; ev = q[i].y == by1 ? vlo : vhi; break;
			}
			CHECK(q[i].u == eu && q[i].v == ev,
				  "%s %s: vertex (%d,%d,%d) has uv (%d,%d), Beta orientation wants (%d,%d)",
				  name, side_name[side], q[i].x, q[i].y, q[i].z, q[i].u, q[i].v, eu, ev);
		}
	}
	for(int s = 0; s < 6; s++)
		CHECK(seen[s], "%s: side %s never emitted", name, side_name[s]);
}

/* The first cube a model emits (its head, for every table under test). */
static void check_cube(const char* name, int u, int v, int w, int h, int d) {
	check_cube_at(name, 0, u, v, w, h, d);
}

static void begin(void) {
	nverts = 0;
	resets = 0;
}

int main(void) {
	mat4 view;
	glm_mat4_identity(view);
	render_entity_init();
	render_entity_update_light(172);
	render_entity_set_limb_swing(0.0f);

	/* ModelWolf.java:21-22  wolfHeadMain = ModelRenderer(0,0); addBox(-3,-3,-2, 6,6,4) */
	begin();
	render_entity_wolf(view, 0.0f, 0.0f, 0, 0, 8, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	check_cube("wolf head", 0, 0, 6, 6, 4);

	/* ModelBiped.java:33-34  bipedHead = ModelRenderer(0,0); addBox(-4,-8,-4, 8,8,8)
	 * (shared by player/zombie/skeleton/pigzombie through render_entity_biped). */
	begin();
	render_entity_zombie(view, 0.0f, 0.0f, 0);
	check_cube("biped head (zombie)", 0, 0, 8, 8, 8);
	/* ModelBiped.java:39-40  bipedBody = ModelRenderer(16,16); addBox(-4,0,-2, 8,12,4)
	 * is the SECOND box render_entity_biped emits, right after the head. */
	check_cube_at("biped body (zombie)", 1, 16, 16, 8, 12, 4);

	begin();
	render_entity_player(view, 0.0f, 0.0f, 0.0f, 0, 0, false, 0.0f, false, NULL);
	check_cube("biped head (player)", 0, 0, 8, 8, 8);
	check_cube_at("biped body (player)", 1, 16, 16, 8, 12, 4);

	/* ModelCreeper.java:18-19  head = ModelRenderer(0,0); addBox(-4,-8,-4, 8,8,8) */
	begin();
	render_entity_creeper(view, 0.0f, 0.0f, 0, 0, 0.0f);
	check_cube("creeper head", 0, 0, 8, 8, 8);

	/* ModelQuadruped.java:4,12  head = ModelRenderer(0,0); addBox(-4,-4,-8, 8,8,8) */
	begin();
	render_entity_pig(view, 0.0f, 0.0f, 0);
	check_cube("pig head", 0, 0, 8, 8, 8);

	/* ModelSpider.java:19-20  spiderHead = ModelRenderer(32,4); addBox(-4,-4,-8, 8,8,8) */
	begin();
	render_entity_spider(view, 0.0f, 0.0f, 0);
	check_cube("spider head", 32, 4, 8, 8, 8);

	if(failures) {
		printf("FAIL test_entity_box_uv: %d mismatches\n", failures);
		return 1;
	}
	printf("PASS test_entity_box_uv: wolf/biped/creeper/pig/spider head boxes and the biped body sample Beta addBox UVs\n");
	return 0;
}
