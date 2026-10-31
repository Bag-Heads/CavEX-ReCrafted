/*
	Copyright (c) 2026 CaveWii_v2 contributors

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
#include <string.h>

#include "render_lightning.h"
#include "../platform/gfx.h"
#include "../util.h"

/* ---- EntityLightningBolt render lifecycle ------------------------------- */

void lightning_bolt_init(struct lightning_bolt* b, double x, double y, double z,
						 int64_t rng_seed) {
	assert(b);
	memset(b, 0, sizeof(*b));
	b->x = x;
	b->y = y;
	b->z = z;
	jrand_set_seed(&b->rng, rng_seed);
	/* EntityLightningBolt.java:13-15:
	 *   the bolt state starts at 2;
	 *   the render seed is one own-rand nextLong();
	 *   the living time is own-rand nextInt(3) + 1. */
	b->state = 2;
	b->render_seed = jrand_next_long(&b->rng);
	b->living_time = jrand_next_int_bound(&b->rng, 3) + 1;
}

bool lightning_bolt_tick(struct lightning_bolt* b, int32_t* flash) {
	assert(b && flash);
	/* java:38-41: the state-2 sound pair draws two nextFloat() pitches from
	 * this.rand. The sounds themselves are played by the Packet71 handler at
	 * spawn (same client tick); the draws keep this stream Beta-shaped. */
	if(b->state == 2) {
		(void)jrand_next_float(&b->rng);
		(void)jrand_next_float(&b->rng);
	}
	/* java:43-60 */
	b->state--;
	if(b->state < 0) {
		if(b->living_time == 0)
			return false; /* setEntityDead() */
		if(b->state < -jrand_next_int_bound(&b->rng, 10)) {
			b->living_time--;
			b->state = 1;
			b->render_seed = jrand_next_long(&b->rng);
			/* java:51-57 restrike fire: the server's (it owns the world). */
		}
	}
	/* java:62-72: onStruckByLightning is the server's; the flash is ours. */
	if(b->state >= 0)
		*flash = 2; /* this.worldObj.field_27172_i = 2; */
	return true;
}

bool lightning_bolt_visible(const struct lightning_bolt* b) {
	assert(b);
	return b->state >= 0; /* isInRangeToRenderVec3D, java:85-87 */
}

void lightning_fx_reset(struct lightning_fx* fx) {
	assert(fx);
	fx->count = 0;
	fx->flash = 0;
}

void lightning_fx_spawn(struct lightning_fx* fx, double x, double y, double z,
						int64_t rng_seed, uint32_t generation,
						int32_t dimension) {
	assert(fx);
	if(generation != fx->generation) {
		lightning_fx_reset(fx);
		fx->generation = generation;
	}
	if(fx->count == LIGHTNING_MAX_BOLTS) {
		/* Full pool: replace the oldest (index 0), counted. */
		memmove(&fx->bolts[0], &fx->bolts[1],
				sizeof(fx->bolts[0]) * (LIGHTNING_MAX_BOLTS - 1));
		fx->count--;
		fx->dropped++;
	}
	lightning_bolt_init(&fx->bolts[fx->count], x, y, z, rng_seed);
	fx->bolts[fx->count++].dimension = dimension;
	fx->spawned++;
}

void lightning_fx_spawn_state(struct lightning_fx* fx, double x, double y,
							  double z, int32_t living_time,
							  int64_t render_seed, uint64_t rng_state,
							  uint32_t generation, int32_t dimension) {
	assert(fx);
	/* Reuse the pool/generation bookkeeping, then overwrite the render
	 * state with the server's. */
	lightning_fx_spawn(fx, x, y, z, 0, generation, dimension);
	struct lightning_bolt* b = &fx->bolts[fx->count - 1];
	memset(&b->rng, 0, sizeof(b->rng));
	b->rng.seed = rng_state & ((1ULL << 48) - 1);
	b->render_seed = render_seed;
	b->living_time = living_time;
	b->state = 2;
}

void lightning_fx_tick(struct lightning_fx* fx, uint32_t generation,
					   int32_t player_dimension) {
	assert(fx);
	const bool dim_changed = fx->player_dimension_known
		&& player_dimension != fx->player_dimension;
	fx->player_dimension = player_dimension;
	fx->player_dimension_known = true;
	if(generation != fx->generation || dim_changed) {
		lightning_fx_reset(fx);
		fx->generation = generation;
		return;
	}
	/* Minecraft.java:1143-1147: decrement first, then updateEntities. */
	if(fx->flash > 0)
		fx->flash--;
	size_t write = 0;
	for(size_t i = 0; i < fx->count; i++) {
		if(lightning_bolt_tick(&fx->bolts[i], &fx->flash)) {
			/* the bolt that set field_27172_i owns it (its World's) */
			if(fx->bolts[i].state >= 0)
				fx->flash_dimension = fx->bolts[i].dimension;
			if(write != i)
				fx->bolts[write] = fx->bolts[i];
			write++;
		}
	}
	fx->count = write;
}

float lightning_fx_flash(const struct lightning_fx* fx, float partial,
						 int32_t dimension) {
	assert(fx);
	if(fx->flash_dimension != dimension)
		return 0.0F;
	return lightning_flash_amount(fx->flash, partial);
}

bool lightning_bolt_drawn_in(const struct lightning_bolt* b, int32_t dimension) {
	assert(b);
	return lightning_bolt_visible(b) && b->dimension == dimension;
}

/* ---- World.func_4079_a flash term (World.java:1050-1060) ---------------- */

float lightning_flash_amount(int32_t flash, float partial) {
	if(flash <= 0)
		return 0.0F;
	float f = (float)flash - partial; /* var14 = field_27172_i - var2 */
	if(f > 1.0F)
		f = 1.0F;
	return f * 0.45F;
}

void lightning_flash_apply(float amount, float rgb[3]) {
	assert(rgb);
	if(amount <= 0.0F)
		return;
	rgb[0] = rgb[0] * (1.0F - amount) + 0.8F * amount;
	rgb[1] = rgb[1] * (1.0F - amount) + 0.8F * amount;
	rgb[2] = rgb[2] * (1.0F - amount) + 1.0F * amount;
}

/* ---- RenderLightningBolt.func_27002_a geometry (java:13-92) ------------- */

size_t lightning_bolt_geometry(int64_t seed,
							   float verts_out[LIGHTNING_VERTS_PER_BOLT][3]) {
	assert(verts_out);
	double v11[8], v12[8];
	double v13 = 0.0, v15 = 0.0;
	struct java_random r17;
	jrand_set_seed(&r17, seed);
	/* java:19-24: the trunk's branch start offsets. */
	for(int i = 7; i >= 0; --i) {
		v11[i] = v13;
		v12[i] = v15;
		v13 += (double)(jrand_next_int_bound(&r17, 11) - 5);
		v15 += (double)(jrand_next_int_bound(&r17, 11) - 5);
	}

	size_t n = 0;
	for(int layer = 0; layer < LIGHTNING_LAYERS; ++layer) {
		struct java_random r46; /* java:27, re-seeded per layer */
		jrand_set_seed(&r46, seed);
		for(int br = 0; br < 3; ++br) {
			int v20 = 7, v21 = 0;
			if(br > 0) {
				v20 = 7 - br;
				v21 = v20 - 2;
			}
			double d22 = v11[v20] - v13;
			double d24 = v12[v20] - v15;
			for(int k = v20; k >= v21; --k) {
				double d27 = d22, d29 = d24;
				if(br == 0) {
					d22 += (double)(jrand_next_int_bound(&r46, 11) - 5);
					d24 += (double)(jrand_next_int_bound(&r46, 11) - 5);
				} else {
					d22 += (double)(jrand_next_int_bound(&r46, 31) - 15);
					d24 += (double)(jrand_next_int_bound(&r46, 31) - 15);
				}
				double w32 = 0.1 + (double)layer * 0.2; /* top half-width */
				if(br == 0)
					w32 *= (double)k * 0.1 + 1.0;
				double w34 = 0.1 + (double)layer * 0.2; /* bottom half-width */
				if(br == 0)
					w34 *= (double)(k - 1) * 0.1 + 1.0;

				/* java:67-90: the 10-vertex strip, bottom/top per corner. */
				float strip[10][3];
				for(int c = 0; c < 5; ++c) {
					double x37 = 0.5 - w32, z39 = 0.5 - w32;
					if(c == 1 || c == 2)
						x37 += w32 * 2.0;
					if(c == 2 || c == 3)
						z39 += w32 * 2.0;
					double x41 = 0.5 - w34, z43 = 0.5 - w34;
					if(c == 1 || c == 2)
						x41 += w34 * 2.0;
					if(c == 2 || c == 3)
						z43 += w34 * 2.0;
					strip[c * 2][0] = (float)(x41 + d22);
					strip[c * 2][1] = (float)(k * 16);
					strip[c * 2][2] = (float)(z43 + d24);
					strip[c * 2 + 1][0] = (float)(x37 + d27);
					strip[c * 2 + 1][1] = (float)((k + 1) * 16);
					strip[c * 2 + 1][2] = (float)(z39 + d29);
				}
				/* Strip triangles (2j,2j+1,2j+2)+(2j+1,2j+2,2j+3) == planar
				 * quad (2j, 2j+1, 2j+3, 2j+2), same winding. */
				for(int j = 0; j < 4; ++j) {
					static const int order[4] = {0, 1, 3, 2};
					for(int q = 0; q < 4; ++q) {
						assert(n < LIGHTNING_VERTS_PER_BOLT);
						memcpy(verts_out[n++], strip[j * 2 + order[q]],
							   sizeof(float) * 3);
					}
				}
			}
		}
	}
	assert(n == LIGHTNING_VERTS_PER_BOLT);
	return n;
}

/* ---- client glue -------------------------------------------------------- */

static struct lightning_fx s_fx;
static struct java_random s_seed_rng; /* stands in for new Random() seeds */
static bool s_seed_rng_init;

void lightning_client_spawn(double x, double y, double z, uint32_t generation,
							int32_t dimension) {
	if(!s_seed_rng_init) {
		jrand_set_seed(&s_seed_rng, (int64_t)0x6C8E9CF570932BD5LL);
		s_seed_rng_init = true;
	}
	/* Beta's entity Random is time-seeded (unsynced, cosmetic); a running
	 * client-side stream gives each bolt a fresh shape the same way. */
	lightning_fx_spawn(&s_fx, x, y, z, jrand_next_long(&s_seed_rng),
					   generation, dimension);
}

void lightning_client_spawn_state(int32_t x, int32_t y, int32_t z,
								  int32_t living_time, int64_t render_seed,
								  uint64_t rng_state, uint32_t generation,
								  int32_t dimension) {
	lightning_fx_spawn_state(&s_fx, (double)x, (double)y, (double)z,
							 living_time, render_seed, rng_state, generation,
							 dimension);
}

void lightning_client_tick(uint32_t generation, int32_t player_dimension) {
	if(s_fx.count == 0 && s_fx.flash == 0 && generation == s_fx.generation) {
		s_fx.player_dimension = player_dimension; /* nothing to drop */
		s_fx.player_dimension_known = true;
		return;
	}
	lightning_fx_tick(&s_fx, generation, player_dimension);
}

float lightning_client_flash(float partial, int32_t dimension) {
	return lightning_fx_flash(&s_fx, partial, dimension);
}

const struct lightning_fx* lightning_client_state(void) {
	return &s_fx;
}

void render_lightning_bolts(mat4 view, const vec3 cam, int32_t dimension) {
	bool any = false;
	for(size_t i = 0; i < s_fx.count; i++)
		any = any || lightning_bolt_drawn_in(&s_fx.bolts[i], dimension);
	if(!any)
		return;

	/* Static scratch: one bolt at a time, no per-frame allocation. */
	static float verts[LIGHTNING_VERTS_PER_BOLT][3];
	static uint8_t colors[LIGHTNING_VERTS_PER_BOLT][4];
	static float uv[LIGHTNING_VERTS_PER_BOLT][2]; /* texturing off */
	static bool colors_init;
	if(!colors_init) {
		for(size_t k = 0; k < LIGHTNING_VERTS_PER_BOLT; k++) {
			colors[k][0] = LIGHTNING_COLOR_R;
			colors[k][1] = LIGHTNING_COLOR_G;
			colors[k][2] = LIGHTNING_COLOR_B;
			colors[k][3] = LIGHTNING_COLOR_A;
		}
		colors_init = true;
	}

	/* Camera-relative vertices under a rotation-only view (float precision
	 * far from the origin), as render_weather.c's precip pass does. */
	mat4 rotation_view;
	glm_mat4_copy(view, rotation_view);
	rotation_view[3][0] = 0.0F;
	rotation_view[3][1] = 0.0F;
	rotation_view[3][2] = 0.0F;
	gfx_matrix_modelview(rotation_view);

	/* RenderLightningBolt.java:9-12: no texture, no lighting, blend
	 * SRC_ALPHA/ONE (MODE_BLEND2). gfx_lighting(false) is also the
	 * CLR0=DIRECT precondition for gfx_draw_quads_flt on GX. Depth test and
	 * write stay as Beta's entity pass leaves them (on). */
	gfx_texture(false);
	gfx_lighting(false);
	gfx_blending(MODE_BLEND2);
	gfx_alpha_test(false);

	for(size_t i = 0; i < s_fx.count; i++) {
		const struct lightning_bolt* b = &s_fx.bolts[i];
		if(!lightning_bolt_drawn_in(b, dimension))
			continue;
		size_t n = lightning_bolt_geometry(b->render_seed, verts);
		float ox = (float)(b->x - (double)cam[0]);
		float oy = (float)(b->y - (double)cam[1]);
		float oz = (float)(b->z - (double)cam[2]);
		for(size_t k = 0; k < n; k++) {
			verts[k][0] += ox;
			verts[k][1] += oy;
			verts[k][2] += oz;
		}
		gfx_draw_quads_flt(n, &verts[0][0], &colors[0][0], &uv[0][0]);
	}

	/* RenderLightningBolt.java:97-99 + this engine's world/entity contract. */
	gfx_cull_func(MODE_BACK);
	gfx_blending(MODE_OFF);
	gfx_alpha_test(true);
	gfx_lighting(true);
	gfx_texture(true);
	gfx_matrix_modelview(view);
}
