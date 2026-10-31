/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>

#include "../daytime.h"
#include "../game/game_state.h"
#include "../platform/gfx.h"
#include "gfx_util.h"

// Beta 1.7.3 [740c583 RenderGlobal.renderStars / starGLCallList] bakes the
// whole star field ONCE (Random(10842L), 1500 rejection-sampled candidates,
// each quad given its own fixed random roll baked into an outward-facing
// basis derived from the star's own direction) into a GL_COMPILE display
// list; renderSky() just replays it every frame under a glColor4f brightness
// multiply. There is no view-matrix/camera term anywhere in Beta's method.
//
// This port used to recompute a camera-facing BILLBOARD (view_matrix-derived
// right/up) for all 1000 stars every single frame -- wrong per the decompile
// above (an undocumented deviation, see docs/deviations.md) and expensive
// (2000 rand() + ~4000 trig calls + 1000 separate GX_Begin/End draws each
// frame the stars are visible). Fixed here: orientation now matches Beta
// (fixed per-star roll, baked once, no camera dependency), which also makes
// the geometry itself frame-invariant -- it is computed once on first use
// and then only re-emitted through ONE gfx_draw_quads_flt call per frame.
//
// Scope note: this patch changes ORIENTATION DISCIPLINE only (billboard ->
// fixed bake), matching Beta's structure. It deliberately keeps this port's
// existing star VALUES unchanged (seed 42, count 1000, direct theta/phi
// sampling, fixed size 0.2) rather than adopting Beta's 10842/1500 rejection-
// sampling/0.25-0.5 varying size -- those are additional, separately flagged
// deviations (see docs/deviations.md) that are P1LGR1M's faithfulness call,
// not bundled into this fix.
//
// Mechanism note: Beta's baked list carries NO per-vertex color at all --
// brightness is a GL global-color multiply applied outside the display list.
// This port's shared `struct displaylist` GX abstraction (source/platform/
// displaylist.h, used by chunks/entities/items) has no equivalent: its CLR0
// slot is a block/sky LIGHT INDEX (0-15, no alpha channel at all -- see
// displaylist_color(), wii/displaylist.c:189 and pc/displaylist.c:120), so it
// cannot represent the day/night alpha fade. Reproducing Beta's exact
// mechanism would mean a GX constant-color register path (GX_SetChanMatColor
// + GX_SRC_REG) -- either extending that shared abstraction or writing
// bespoke raw GX list code, both real Wii/GX platform changes needing
// devkitPPC + real-hardware proof. Deferred as a documented follow-up (it
// composes cleanly on top of this patch's baked geometry). This patch instead
// keeps drawing through gfx_draw_quads_flt (VTXFMT1, F32 pos + RGBA8 color --
// already alpha-capable, already proven on both platforms): with billboarding
// gone the position/texcoord buffers are genuinely static forever after the
// first bake, and the color buffer is only rewritten on the ticks alpha
// actually changes (the ~2000-tick dawn/dusk windows), never during steady
// day/night -- still a single draw call, zero redundant rand()/trig work.
#define GUTIL_STAR_COUNT 1000
static bool s_star_baked = false;
static float s_star_verts[GUTIL_STAR_COUNT * 4 * 3];
static float s_star_texcoords[GUTIL_STAR_COUNT * 4 * 2];
static uint8_t s_star_colors[GUTIL_STAR_COUNT * 4 * 4];
// Sentinel outside uint8_t's normal range semantics (any real alpha 0-255 is
// valid) -- use a separate "have we baked a color buffer yet" flag instead of
// overloading a byte value, so alpha==0 (a legitimately re-bakeable state)
// never gets mistaken for "never baked".
static bool s_star_color_baked = false;
static uint8_t s_star_alpha_baked = 0;

static void gutil_bake_star_geometry(void) {
	if(s_star_baked)
		return;

	const float scale = 0.2f;  // star size -- unchanged from the port's existing value
	const float radius = 90.0f;
	static const float texcoords[8] = {0.0f, 0.0f, 1.0f, 0.0f,
									   1.0f, 1.0f, 0.0f, 1.0f};

	srand(42);

	for(int i = 0; i < GUTIL_STAR_COUNT; i++) {
		float theta = glm_rad(rand() % 360);
		float phi = glm_rad(rand() % 180);

		float x = cosf(theta) * sinf(phi) * radius;
		float y = cosf(phi) * radius;
		float z = sinf(theta) * sinf(phi) * radius;

		// Outward-facing basis from the star's OWN (normalized) direction --
		// Beta's var20 (yaw, atan2 of x,z) / var26 (pitch, atan2 of the
		// horizontal magnitude and y) -- plus an independent per-star random
		// roll (Beta's var32), all fixed here at bake time. [740c583
		// RenderGlobal.renderStars]
		float dirlen = sqrtf(x * x + y * y + z * z);
		float ux = x / dirlen, uy = y / dirlen, uz = z / dirlen;
		float yaw = atan2f(ux, uz);
		float pitch = atan2f(sqrtf(ux * ux + uz * uz), uy);
		float roll = (rand() / (float)RAND_MAX) * (2.0f * GLM_PIf);

		float sy = sinf(yaw), cy = cosf(yaw);
		float sp = sinf(pitch), cp = cosf(pitch);
		float sr = sinf(roll), cr = cosf(roll);

		for(int c = 0; c < 4; c++) {
			// Corner local offsets before rotation -- Beta's (var41,var43)
			// pattern: (-s,-s),(-s,+s),(+s,+s),(+s,-s).
			float lx = ((c & 2) ? 1.0f : -1.0f) * scale;
			float lz = (((c + 1) & 2) ? 1.0f : -1.0f) * scale;

			float rx = lx * cr - lz * sr;  // Beta's rotated x
			float rz = lz * cr + lx * sr;  // Beta's rotated z
			float world_dy = rx * sp;      // Beta's world dy
			float t = -rx * cp;            // Beta's t
			float world_dx = t * sy - rz * cy;  // Beta's world dx
			float world_dz = rz * sy + t * cy;  // Beta's world dz

			int vi = (i * 4 + c) * 3;
			s_star_verts[vi + 0] = x + world_dx;
			s_star_verts[vi + 1] = y + world_dy;
			s_star_verts[vi + 2] = z + world_dz;

			int ti = (i * 4 + c) * 2;
			s_star_texcoords[ti + 0] = texcoords[c * 2 + 0];
			s_star_texcoords[ti + 1] = texcoords[c * 2 + 1];
		}
	}

	s_star_baked = true;
}

static void gutil_render_stars(mat4 view_matrix, float time) {
	(void)view_matrix;  // no longer billboard-facing; kept for call-site compat
	float fade = 0.0f;
	float day_ticks = fmodf(time, 24000.0f);

	if (day_ticks > 13000.0f && day_ticks < 14000.0f)
	    fade = (day_ticks - 13000.0f) / 1000.0f;  // fade-in
	else if (day_ticks >= 14000.0f && day_ticks <= 22000.0f)
	    fade = 1.0f;
	else if (day_ticks > 22000.0f && day_ticks < 23000.0f)
	    fade = (23000.0f - day_ticks) / 1000.0f;  // fade-out
	else
	    fade = 0.0f;

	uint8_t star_alpha = (uint8_t)(fade * 255.0f);
	if (star_alpha == 0)
		return;

	gutil_bake_star_geometry();

	if(!s_star_color_baked || s_star_alpha_baked != star_alpha) {
		uint8_t star_r = 180;
		uint8_t star_g = 190;
		uint8_t star_b = 255;
		for(int v = 0; v < GUTIL_STAR_COUNT * 4; v++) {
			s_star_colors[v * 4 + 0] = star_r;
			s_star_colors[v * 4 + 1] = star_g;
			s_star_colors[v * 4 + 2] = star_b;
			s_star_colors[v * 4 + 3] = star_alpha;
		}
		s_star_alpha_baked = star_alpha;
		s_star_color_baked = true;
	}

	gfx_texture(false);
	gfx_lighting(false);
	gfx_blending(MODE_BLEND2);
	gfx_alpha_test(false);
	gfx_cull_func(MODE_NONE);

	gfx_draw_quads_flt(GUTIL_STAR_COUNT * 4, s_star_verts, s_star_colors,
					   s_star_texcoords);

	gfx_alpha_test(true);
	gfx_blending(MODE_OFF);
	gfx_texture(true);
	gfx_cull_func(MODE_BACK);

}


void gutil_clouds(mat4 view_matrix, float brightness) {
	gutil_clouds_owned(view_matrix, brightness, &gstate.players[0].camera, time_get());
}

void gutil_clouds_owned(mat4 view_matrix, float brightness, const struct camera* camera, ptime_t now) {
	assert(view_matrix);

	float cloud_pos = (gstate.world_time
					   + time_diff_s(gstate.world_time_start, now)
						   * 1000.0F / DAY_TICK_MS)
		* 12.0F / 400.0F;

	vec3 shift = {roundf(camera->x / 12.0F) * 12.0F
					  - (cloud_pos - roundf(cloud_pos / 12.0F) * 12.0F),
				  108.5F, roundf(camera->z / 12.0F) * 12.0F};

	mat4 model_view;
	glm_translate_to(view_matrix, shift, model_view);
	gfx_matrix_modelview(model_view);

	gfx_fog(true);
	gfx_fog_pos(shift[0] - camera->x, shift[2] - camera->z,
				gstate.config.fog_distance * 1.5F);
	gfx_texture(false);
	gfx_blending(MODE_BLEND);
	gfx_lighting(false);
	gfx_cull_func(MODE_NONE);
	gfx_write_buffers(false, true, true);

	int ox = roundf(camera->x / 12.0F) + roundf(cloud_pos / 12.0F);
	int oy = roundf(camera->z / 12.0F);

	uint8_t shade[6] = {
		roundf(brightness * 1.0F * 255),  roundf(brightness * 0.75F * 255),
		roundf(brightness * 0.84F * 255), roundf(brightness * 0.84F * 255),
		roundf(brightness * 0.92F * 255), roundf(brightness * 0.92F * 255),
	};

	// FACE-CULLING (2026-06-13 cloud reduction, wf_906f4fa3): the old code emitted
	// an unconditional 24-vert 6-face solid cube for EVERY opaque cell of a 33x33
	// grid, TWICE = up to ~52k immediate verts/frame (the GP-saturation freeze's
	// dominant producer). Beta's own renderCloudsFancy culls per-face -- interior
	// cells draw only top+bottom; side faces only at exposed edges. Match that:
	// precompute the opacity grid ONCE (was a texture lookup per cell PER pass),
	// then per cell emit top+bottom (always) + each side ONLY where the abutting
	// cell is transparent. Removes the occluded interior side faces -- a faithful
	// cut (the dropped faces were never visible), not a shortcut. ~2-3x fewer
	// verts; the silhouette is identical (only shared interior faces vanish).
	#define CLOUD_GRID 33 // x,y in [-16,16] inclusive
	uint8_t op[CLOUD_GRID * CLOUD_GRID];
	for(int y = -16; y <= 16; y++)
		for(int x = -16; x <= 16; x++) {
			uint8_t c[4];
			tex_gfx_lookup(&texture_clouds, ox + x, oy + y, c);
			op[(y + 16) * CLOUD_GRID + (x + 16)] = c[3] >= 128;
		}
	// Out-of-grid neighbours read NOT opaque, so the field perimeter keeps its
	// side faces (an off-bitmap edge must never be culled).
	#define CLOUD_OP(X, Y)                                                       \
		((X) >= -16 && (X) <= 16 && (Y) >= -16 && (Y) <= 16                       \
		 && op[((Y) + 16) * CLOUD_GRID + ((X) + 16)])

	// Each face: 4 verts encoded (xb,yb,zb) selecting min/box per axis
	// (X=min[0]/box[0], Y=min[2]/box[2] the 0..4 thickness, Z=min[1]/box[1]);
	// shade index; (dx,dy) of the neighbour cell that occludes it (8,8 = never).
	static const struct {
		uint8_t v[4][3];
		uint8_t shade;
		int8_t dx, dy;
	} FACES[6] = {
		{{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}, 0, 8, 8},  // top
		{{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}, 1, 8, 8},  // bottom
		{{{0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}}, 2, 0, -1}, // -Z (back)
		{{{0, 1, 1}, {1, 1, 1}, {1, 0, 1}, {0, 0, 1}}, 3, 0, 1},  // +Z (front)
		{{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}, 4, 1, 0},  // +X (right)
		{{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}, 5, -1, 0}, // -X (left)
	};

	for(int k = 0; k < 2; k++) {
		if(k == 1)
			gfx_write_buffers(true, false, true);

		for(int y = -16; y <= 16; y++) {
			for(int x = -16; x <= 16; x++) {
				if(!CLOUD_OP(x, y))
					continue;

				int16_t ax[2] = {x * 12, x * 12 + 12};   // min[0], box[0]
				int16_t ay[2] = {0, 4};                  // min[2], box[2]
				int16_t az[2] = {y * 12, y * 12 + 12};   // min[1], box[1]

				int16_t verts[6 * 4 * 3];
				uint8_t cols[6 * 4 * 4];
				uint16_t tex[6 * 4 * 2] = {0}; // texturing off; dead but VTXFMT2-shaped
				int nv = 0;

				for(int f = 0; f < 6; f++) {
					if(FACES[f].dx != 8
					   && CLOUD_OP(x + FACES[f].dx, y + FACES[f].dy))
						continue; // shared interior face -- occluded, skip
					uint8_t s = shade[FACES[f].shade];
					for(int i = 0; i < 4; i++) {
						verts[nv * 3 + 0] = ax[FACES[f].v[i][0]];
						verts[nv * 3 + 1] = ay[FACES[f].v[i][1]];
						verts[nv * 3 + 2] = az[FACES[f].v[i][2]];
						cols[nv * 4 + 0] = s;
						cols[nv * 4 + 1] = s;
						cols[nv * 4 + 2] = s;
						cols[nv * 4 + 3] = 0xBF;
						nv++;
					}
				}

				if(nv)
					gfx_draw_quads(nv, verts, cols, tex);
			}
		}
	}
	#undef CLOUD_OP
	#undef CLOUD_GRID

	gfx_write_buffers(true, true, true);
	gfx_blending(MODE_OFF);
	gfx_texture(true);
	gfx_cull_func(MODE_BACK);
	gfx_fog(false);
	// Mandatory restore (gutil_sky_box precedent, 95159f6, same file): the
	// gfx_lighting(false) at this function's top was never paired with a
	// restore, leaving CLR0 in DIRECT for whatever draws next. gutil_clouds
	// is called exactly once per frame (main.c, overworld only) and is
	// IMMEDIATELY followed by gfx_mode_gui(), which itself unconditionally
	// re-asserts gfx_lighting(false) -- so this omission produces no
	// observable symptom in the current call graph (verified: the only
	// caller has no draw call between gutil_clouds and gfx_mode_gui). That is
	// exactly the accidental-ordering dependency gutil_sky_box had before
	// 95159f6 fixed it: correctness rode on what happens to run next, not on
	// this function's own contract. Restore explicitly so a future caller
	// (e.g. a debug/HUD draw inserted between clouds and gfx_mode_gui) can't
	// silently reintroduce the CLR0-desync hang class. Restored last, matching
	// the established idiom (gutil_sky_box's own tail, render_weather.c).
	gfx_lighting(true);
}

void gutil_sky_box(mat4 view_matrix, float celestial_angle, vec3 color_top,
				   vec3 color_bottom) {
	gutil_sky_box_owned(view_matrix, celestial_angle, color_top, color_bottom,
		&gstate.players[0].camera);
}

void gutil_sky_box_owned(mat4 view_matrix, float celestial_angle, vec3 color_top,
	vec3 color_bottom, const struct camera* camera) {
	assert(view_matrix && color_top && color_bottom);
	/* Celestial quads reach 109 blocks from the eye. Split terrain can
	 * clip at 96: using that projection clips a centred sun/moon while
	 * leaving it visible off-axis. Sky writes no depth, so extend only
	 * this pass and restore the owner's terrain projection below. */
	mat4 sky_projection;
	glm_mat4_copy((vec4*)camera->projection, sky_projection);
	if(gstate.config.render_distance < 128.0F)
		glm_persp_move_far(sky_projection, 128.0F - gstate.config.render_distance);
	gfx_matrix_projection(sky_projection, true);

	gfx_alpha_test(false);
	gfx_write_buffers(true, false, false);
	gfx_blending(MODE_OFF);
	gfx_lighting(false);
	gfx_texture(false);
	gfx_fog(true);

	mat4 model_view;
	glm_translate_to(view_matrix,
					 (vec3) {camera->x, camera->y, camera->z},
					 model_view);
	gfx_matrix_modelview(model_view);
	gfx_fog_pos(0, 0, gstate.config.fog_distance);

	// render a bit larger for possible inaccuracy
	float size = gstate.config.fog_distance + 4;

	gfx_draw_quads(4,
				   (int16_t[]) {-size, 16, -size, -size, 16, size, size, 16,
								size, size, 16, -size},
				   (uint8_t[]) {color_top[0], color_top[1], color_top[2], 0xFF,
								color_top[0], color_top[1], color_top[2], 0xFF,
								color_top[0], color_top[1], color_top[2], 0xFF,
								color_top[0], color_top[1], color_top[2], 0xFF},
				   (uint16_t[]) {0, 0, 0, 0, 0, 0, 0, 0});

	gfx_draw_quads(
		4,
		(int16_t[]) {-size, -32, -size, size, -32, -size, size, -32, size,
					 -size, -32, size},
		(uint8_t[]) {color_bottom[0], color_bottom[1], color_bottom[2], 0xFF,
					 color_bottom[0], color_bottom[1], color_bottom[2], 0xFF,
					 color_bottom[0], color_bottom[1], color_bottom[2], 0xFF,
					 color_bottom[0], color_bottom[1], color_bottom[2], 0xFF},
		(uint16_t[]) {0, 0, 0, 0, 0, 0, 0, 0});

	gfx_fog(false);
	gfx_texture(true);

	gutil_render_stars(view_matrix, daytime_get_time());

	gfx_blending(MODE_BLEND2);

	mat4 tmp;
	glm_translate_to(view_matrix,
					 (vec3) {camera->x, camera->y, camera->z},
					 tmp);
	glm_rotate_x(tmp, glm_rad(celestial_angle * 360.0F), model_view);
	gfx_matrix_modelview(model_view);

	gfx_bind_texture(&texture_sun);
	gfx_draw_quads(
		4, (int16_t[]) {-30, 100, -30, -30, 100, 30, 30, 100, 30, 30, 100, -30},
		(uint8_t[]) {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		(uint16_t[]) {0, 0, 0, 256, 256, 256, 256, 0});

	gfx_bind_texture(&texture_moon);
	gfx_draw_quads(4,
				   (int16_t[]) {-20, -100, -20, 20, -100, -20, 20, -100, 20,
								-20, -100, 20},
				   (uint8_t[]) {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
								0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
				   (uint16_t[]) {0, 0, 0, 256, 256, 256, 256, 0});


	gfx_blending(MODE_OFF);
	gfx_write_buffers(true, true, true);
	gfx_alpha_test(true);
	gfx_matrix_projection((vec4*)camera->projection, true);
	// Mandatory restore (render_name_tag_draw_pass precedent): gfx_lighting(false)
	// at the top of this function was NEVER paired with a restore, so every
	// frame's sky box left CLR0 in DIRECT mode for whatever drew next
	// (world_render only happened to survive it by setting its own state) --
	// exactly the CLR0-desync class that has hung the Wii before (render_item.c,
	// render_weather.c). Restore lighting last, matching that idiom.
	gfx_lighting(true);
}

void gutil_block_selection(mat4 view_matrix, struct block_info* this) {
	assert(view_matrix && this);

	int pad = 1;

	if(!blocks[this->block->type])
		return;

	size_t count = blocks[this->block->type]->getBoundingBox(this, false, NULL);

	if(!count)
		return;

	struct AABB bbox[count];
	blocks[this->block->type]->getBoundingBox(this, false, bbox);

	gfx_fog(false);
	gfx_lighting(false);
	gfx_blending(MODE_BLEND);
	gfx_texture(false);

	mat4 model_view;
	glm_translate_to(view_matrix, (vec3) {this->x, this->y, this->z},
					 model_view);
	gfx_matrix_modelview(model_view);

	for(size_t k = 0; k < count; k++) {
		struct AABB* box = bbox + k;
		gfx_draw_lines(
			24,
			(int16_t[]) {
				// bottom
				-pad + box->x1 * 256, -pad + box->y1 * 256,
				-pad + box->z1 * 256, box->x2 * 256 + pad, -pad + box->y1 * 256,
				-pad + box->z1 * 256,

				-pad + box->x1 * 256, -pad + box->y1 * 256,
				-pad + box->z1 * 256, -pad + box->x1 * 256,
				-pad + box->y1 * 256, box->z2 * 256 + pad,

				box->x2 * 256 + pad, -pad + box->y1 * 256, box->z2 * 256 + pad,
				box->x2 * 256 + pad, -pad + box->y1 * 256, -pad + box->z1 * 256,

				box->x2 * 256 + pad, -pad + box->y1 * 256, box->z2 * 256 + pad,
				-pad + box->x1 * 256, -pad + box->y1 * 256, box->z2 * 256 + pad,

				// top
				-pad + box->x1 * 256, box->y2 * 256 + pad, -pad + box->z1 * 256,
				box->x2 * 256 + pad, box->y2 * 256 + pad, -pad + box->z1 * 256,

				-pad + box->x1 * 256, box->y2 * 256 + pad, -pad + box->z1 * 256,
				-pad + box->x1 * 256, box->y2 * 256 + pad, box->z2 * 256 + pad,

				box->x2 * 256 + pad, box->y2 * 256 + pad, box->z2 * 256 + pad,
				box->x2 * 256 + pad, box->y2 * 256 + pad, -pad + box->z1 * 256,

				box->x2 * 256 + pad, box->y2 * 256 + pad, box->z2 * 256 + pad,
				-pad + box->x1 * 256, box->y2 * 256 + pad, box->z2 * 256 + pad,

				// vertical
				-pad + box->x1 * 256, -pad + box->y1 * 256,
				-pad + box->z1 * 256, -pad + box->x1 * 256, box->y2 * 256 + pad,
				-pad + box->z1 * 256,

				box->x2 * 256 + pad, -pad + box->y1 * 256, -pad + box->z1 * 256,
				box->x2 * 256 + pad, box->y2 * 256 + pad, -pad + box->z1 * 256,

				-pad + box->x1 * 256, -pad + box->y1 * 256, box->z2 * 256 + pad,
				-pad + box->x1 * 256, box->y2 * 256 + pad, box->z2 * 256 + pad,

				box->x2 * 256 + pad, -pad + box->y1 * 256, box->z2 * 256 + pad,
				box->x2 * 256 + pad, box->y2 * 256 + pad, box->z2 * 256 + pad},
			(uint8_t[]) {0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,
						 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153,
						 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,
						 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153,
						 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,
						 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153,
						 0, 0,	 0, 153, 0, 0,	 0, 153, 0, 0,	 0, 153});
	}

	gfx_texture(true);
	gfx_lighting(true);
}

void gutil_entity_selection(mat4 view_matrix, const struct entity *e) {
    assert(view_matrix && e);
    // Fix (painting-slice capture, 2026-09-01): screen_ingame.c's render3D
    // calls this for EVERY crosshair-hit entity with type != LOCAL_PLAYER/
    // ITEM -- which includes ENTITY_PAINTING, the one entity kind in this
    // engine whose e->getBoundingBox is intentionally NULL (Beta's own
    // EntityPainting has no getEntityBoundingBox() override either -- see
    // entity_painting.c's entity_painting() assignment and its long
    // "findings #1/#2" comment). The old unconditional assert was compiled
    // out under NDEBUG (release), which would have turned this into a raw
    // e->getBoundingBox(...) call through a NULL function pointer the
    // instant a player looked at a placed painting -- undefined behavior in
    // exactly the release build real hardware ships. A painting genuinely
    // has no bounding box to outline here (Beta shows no such highlight for
    // it either); skip drawing one instead of crashing/UB-ing.
    if(!e->getBoundingBox)
        return;

    struct AABB box;
    size_t count = e->getBoundingBox(e, &box);
    if (count == 0) {
        return;
    }

    gfx_fog(false);
    gfx_lighting(false);
    gfx_blending(MODE_BLEND);
    gfx_texture(false);


    int base_x = (int)floorf(box.x1);
    int base_y = (int)floorf(box.y1);
    int base_z = (int)floorf(box.z1);

    float rel_x1 = box.x1 - (float)base_x;
    float rel_y1 = box.y1 - (float)base_y;
    float rel_z1 = box.z1 - (float)base_z;
    float rel_x2 = box.x2 - (float)base_x;
    float rel_y2 = box.y2 - (float)base_y;
    float rel_z2 = box.z2 - (float)base_z;

    mat4 model_view;
    glm_translate_to(view_matrix,
                     (vec3){(float)base_x, (float)base_y, (float)base_z},
                     model_view);
    gfx_matrix_modelview(model_view);

    int pad = 1;
    int16_t sx1 = (int16_t)(rel_x1 * 256.0f) - pad;
    int16_t sy1 = (int16_t)(rel_y1 * 256.0f) - pad;
    int16_t sz1 = (int16_t)(rel_z1 * 256.0f) - pad;
    int16_t sx2 = (int16_t)(rel_x2 * 256.0f) + pad;
    int16_t sy2 = (int16_t)(rel_y2 * 256.0f) + pad;
    int16_t sz2 = (int16_t)(rel_z2 * 256.0f) + pad;

    int16_t pts[24 * 3];
    int idx = 0;
    #define ADD_EDGE(x0,y0,z0, x1,y1,z1) \
        do {                             \
            pts[idx++] = (x0);           \
            pts[idx++] = (y0);           \
            pts[idx++] = (z0);           \
            pts[idx++] = (x1);           \
            pts[idx++] = (y1);           \
            pts[idx++] = (z1);           \
        } while (0)

    // Bottom face (y = sy1)
    ADD_EDGE(sx1, sy1, sz1,  sx2, sy1, sz1);
    ADD_EDGE(sx2, sy1, sz1,  sx2, sy1, sz2);
    ADD_EDGE(sx2, sy1, sz2,  sx1, sy1, sz2);
    ADD_EDGE(sx1, sy1, sz2,  sx1, sy1, sz1);

    // Top face (y = sy2)
    ADD_EDGE(sx1, sy2, sz1,  sx2, sy2, sz1);
    ADD_EDGE(sx2, sy2, sz1,  sx2, sy2, sz2);
    ADD_EDGE(sx2, sy2, sz2,  sx1, sy2, sz2);
    ADD_EDGE(sx1, sy2, sz2,  sx1, sy2, sz1);

    // Vertical edges
    ADD_EDGE(sx1, sy1, sz1,  sx1, sy2, sz1);
    ADD_EDGE(sx2, sy1, sz1,  sx2, sy2, sz1);
    ADD_EDGE(sx2, sy1, sz2,  sx2, sy2, sz2);
    ADD_EDGE(sx1, sy1, sz2,  sx1, sy2, sz2);

    #undef ADD_EDGE

    uint8_t colors[24 * 4];
    for (int i = 0; i < 24; i++) {
        colors[i * 4 + 0] = 0;   // R
        colors[i * 4 + 1] = 0;   // G
        colors[i * 4 + 2] = 0;   // B
        colors[i * 4 + 3] = 153; // A
    }

    gfx_draw_lines(24, pts, colors);

    gfx_texture(true);
    gfx_lighting(true);
}
