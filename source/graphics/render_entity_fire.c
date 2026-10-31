#include "render_entity_fire.h"
#include "fire_overlay.h" /* fire_anim_cell_origin (anim.png fire strips) */
#include "flame_fx.h"

#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "../entity/entity.h"
#include "../entity/combat.h" /* PLAYER_EYE_HEIGHT for the split partner body. */
#include "../game/camera.h"
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include "../platform/time.h"
#include "../world.h"

/* Beta Render.java:30-88. Preserve its float update order, including the
 * 0.02F INITIAL depth term and separate 0.03F PER-LAYER depth step. */
size_t render_entity_fire_build(bool on_fire, float width, float height,
	float pos_y_minus_min_y, float camera_yaw, struct entity_fire_quad* out,
	size_t capacity) {
	if(!on_fire || width <= 0.0F || height <= 0.0F)
		return 0;
	float scale = width * 1.4F;
	float half = 0.5F, horizontal = 0.0F;
	float remaining = height / scale;
	float origin_y = pos_y_minus_min_y;
	float depth = 0.0F;
	float depth_offset = -0.3F + (float)((int)remaining) * 0.02F;
	/* Beta rotates by -playerViewY; playerViewY is -camera.rx here. */
	float cs = cosf(camera_yaw), sn = sinf(camera_yaw);
	size_t count = 0;
	while(remaining > 0.0F) {
		if(out && count < capacity) {
			struct entity_fire_quad* q = &out[count];
			float u0 = 240.0F / 256.0F;
			float u1 = (240.0F + 15.99F) / 256.0F;
			float v0 = (16.0F + (count % 2 ? 16.0F : 0.0F)) / 256.0F;
			float v1 = (16.0F + (count % 2 ? 16.0F : 0.0F) + 15.99F) / 256.0F;
			if((count / 2) % 2 == 0) {
				float t = u0; u0 = u1; u1 = t;
			}
			/* Walk the same four Beta corners clockwise for this engine's
			 * GL_CW / GX_CULL_BACK convention; UVs follow their corners. */
			float xx[4] = {half - horizontal, half - horizontal,
				-half - horizontal, -half - horizontal};
			float yy[4] = {0.0F - origin_y, 1.4F - origin_y,
				1.4F - origin_y, 0.0F - origin_y};
			float uu[4] = {u1, u1, u0, u0};
			float vv[4] = {v1, v0, v0, v1};
			for(int k = 0; k < 4; k++) {
				q->v[k].x = (xx[k] * cs + (depth + depth_offset) * sn) * scale;
				q->v[k].y = yy[k] * scale;
				q->v[k].z = (-xx[k] * sn + (depth + depth_offset) * cs) * scale;
				q->v[k].u = uu[k];
				q->v[k].v = vv[k];
			}
		}
		remaining -= 0.45F;
		origin_y -= 0.45F;
		half *= 0.9F;
		depth += 0.03F;
		count++;
	}
	return count;
}

void render_entity_fire_draw(const struct entity* e, const struct camera* c,
	float tick_delta) {
	if(!e || !c || !e->on_fire || !texture_anim.data)
		return;
	float origin_y = e->y_offset - e->y_size;
	size_t n = render_entity_fire_build(true, e->width, e->height,
		origin_y, c->rx, NULL, 0);
	if(!n) return;
	struct entity_fire_quad local[16];
	struct entity_fire_quad* q = n <= 16 ? local : malloc(n * sizeof(*q));
	if(!q) return;
	render_entity_fire_build(true, e->width, e->height, origin_y,
		c->rx, q, n);

	/* The block fire pass scrolls the same two 28-frame anim.png strips
	 * (world.c:2300-2307): Beta's fire cell = cols 3 & 10, its fire+16 cell =
	 * cols 6 & 13 (flame_fx.h). Render.java:49-64 picks fire+16 for the odd
	 * layers; flame_fx_entity_layer_cell encodes exactly that. */
	const struct world* w = e->world;
	int frame = w ? (int)((int64_t)time_diff_ms(w->anim_timer, time_get()) * 7
		/ 800 % 28) : 0;
	if(frame < 0) frame += 28;
	float base_u[2], base_v[2];
	for(int cell = 0; cell < 2; cell++)
		fire_anim_cell_origin(frame, cell, &base_u[cell], &base_v[cell]);
	vec3 pos;
	for(int axis = 0; axis < 3; axis++)
		pos[axis] = e->pos_old[axis]
			+ (e->pos[axis] - e->pos_old[axis]) * tick_delta;
	/* Local player positions are eye-based; the partner's visible model is
	 * lowered by the same amount in entity_local_player_render_other. */
	if(e->type == ENTITY_LOCAL_PLAYER) pos[1] -= PLAYER_EYE_HEIGHT;
	mat4 model, mv, view;
	glm_translate_make(model, pos);
	memcpy(view, c->view, sizeof(view));
	glm_mat4_mul(view, model, mv);
	gfx_matrix_modelview(mv);
	gfx_bind_texture(&texture_anim);
	gfx_lighting(false); /* GX CLR0=DIRECT for gfx_draw_quads_flt. */
	for(size_t i = 0; i < n; i++) {
		float xyz[12], uv[8];
		uint8_t rgba[16];
		int cell = flame_fx_entity_layer_cell(i);
		for(int k = 0; k < 4; k++) {
			xyz[k * 3] = q[i].v[k].x;
			xyz[k * 3 + 1] = q[i].v[k].y;
			xyz[k * 3 + 2] = q[i].v[k].z;
			/* Beta logical tile x=240..255.99, row y=16 (fire) or 32
			 * (fire+16); each maps onto its own strip's frame cell. */
			uv[k * 2] = base_u[cell] + (q[i].v[k].u - 240.0F / 256.0F);
			uv[k * 2 + 1] = base_v[cell] + (q[i].v[k].v
				- (cell ? 32.0F : 16.0F) / 256.0F);
			for(int j = 0; j < 4; j++) rgba[k * 4 + j] = 255;
		}
		gfx_draw_quads_flt(4, xyz, rgba, uv);
	}
	/* GX vertex descriptor reset is mandatory before the next entity/world
	 * draw (render_entity.c:2218 and render_weather.c:578). */
	gfx_lighting(true);
	gfx_matrix_modelview(GLM_MAT4_IDENTITY);
	gfx_bind_texture(&texture_terrain);
	if(q != local) free(q);
}
