/*
	test_flame_fx_two_instances -- Beta 1.7.3 registers TWO TextureFlamesFX.

	Beta: Minecraft.java:282-283 registers flame instance zero before
	instance one; TextureFlamesFX.java:7-9 selects the fire tile for zero and
	the tile sixteen positions later for one (the fire cell and the cell one
	row below it). Each instance owns its own field_1133_g / field_1132_h arrays
	(TextureFlamesFX.java:4-5), but both draw from the ONE global Math.random()
	stream (TextureFlamesFX.java:33), in registration order: RenderEngine
	.registerTextureFX (RenderEngine.java:346-349) ticks each instance once as it
	is registered, and updateDynamicTextures (RenderEngine.java:363-366) ticks the
	list in order every client tick -- instance 0 then instance 1.

	CaveWii's flame_fx port replaces Math.random() with its documented
	self-contained LCG (flame_fx.h: render-layer, non-parity), so the golden here
	is an INDEPENDENT straight-line transcription of TextureFlamesFX.onTick
	(Java pointer swap, Java's left-to-right ten-factor blue product) driven by
	that same LCG as ONE shared stream consumed instance 0 then instance 1.

	(a1) pair init == Beta registration: each instance ticked once, 0 then 1.
	(a2) every tick, each instance's 16x16 RGBA bytes AND 16x20 field are
	     byte-exact to the reference, for 600 ticks.
	(a3) the instances are independent: different pixels after N ticks, and
	     instance 0 of the pair is NOT a copy of instance 1 at any tick >= 1.
	(a4) the single-instance math is unchanged: pair instance 0 after its
	     registration tick equals a lone flame_fx seeded the same and ticked once.
	(b)  every consumer picks instance 1 (Beta's fire+16 cell) exactly where
	     Beta does, and that cell resolves to anim.png's second strip (cols 6 &
	     13) through the same 28-frame scroll as the first (cols 3 & 10):
	       RenderBlocks.renderBlockFire  :798-803 side quads iff (x+y+z)&1,
	                                     :865-874 the SECOND ceiling quad,
	                                     :907-944 free-standing planes 3-6;
	       ItemRenderer.renderFireInFirstPerson :385 the second quad;
	       Render.renderEntityOnFire     :49-64 the odd-numbered layers.
*/

#include "graphics/fire_overlay.h"
#include "graphics/flame_fx.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SEED 0x1234567u
#define TICKS 600

// --- reference: TextureFlamesFX.onTick, transcribed line by line -----------
struct ref_rng {
	uint32_t s;
};

// The CaveWii flame RNG (flame_fx.h: self-contained LCG, [0,1) from the top 24
// bits) standing in for Math.random(); one instance of this struct is SHARED by
// both reference flames, like Java's one global Math.random(). Returned as a
// DOUBLE like Math.random() (24-bit values, so exact in either type).
static double ref_random(struct ref_rng* r) {
	r->s = r->s * 1664525u + 1013904223u;
	return (double)(r->s >> 8) / (double)(1u << 24);
}

struct ref_flame {
	float a[320], b[320];
	float* g; // field_1133_g
	float* h; // field_1132_h
	uint8_t image[1024];
};

static void ref_init(struct ref_flame* f) {
	memset(f, 0, sizeof(*f));
	f->g = f->a;
	f->h = f->b;
}

static void ref_on_tick(struct ref_flame* f, struct ref_rng* r) {
	int var2;
	float var4;
	int var5, var6;
	for(int var1 = 0; var1 < 16; ++var1) {
		for(var2 = 0; var2 < 20; ++var2) {
			int var3 = 18;
			var4 = f->g[var1 + (var2 + 1) % 20 * 16] * (float)var3;
			for(var5 = var1 - 1; var5 <= var1 + 1; ++var5) {
				for(var6 = var2; var6 <= var2 + 1; ++var6) {
					if(var5 >= 0 && var6 >= 0 && var5 < 16 && var6 < 20)
						var4 += f->g[var5 + var6 * 16];
					++var3;
				}
			}
			f->h[var1 + var2 * 16] = var4 / ((float)var3 * 1.06F);
			if(var2 >= 19) {
				// TextureFlamesFX.java:33, evaluated in DOUBLE then cast:
				// (float)(r*r*r*4.0D + r*(double)0.1F + (double)0.2F).
				double r1 = ref_random(r), r2 = ref_random(r);
				double r3 = ref_random(r), r4 = ref_random(r);
				f->h[var1 + var2 * 16] = (float)(r1 * r2 * r3 * 4.0
					+ r4 * (double)0.1F + (double)0.2F);
			}
		}
	}
	float* var12 = f->h; // Java swaps the array references
	f->h = f->g;
	f->g = var12;
	for(var2 = 0; var2 < 256; ++var2) {
		float var13 = f->g[var2] * 1.8F;
		if(var13 > 1.0F)
			var13 = 1.0F;
		if(var13 < 0.0F)
			var13 = 0.0F;
		var5 = (int)(var13 * 155.0F + 100.0F);
		var6 = (int)(var13 * var13 * 255.0F);
		int var7 = (int)(var13 * var13 * var13 * var13 * var13 * var13 * var13
						 * var13 * var13 * var13 * 255.0F);
		int var8 = 255;
		if(var13 < 0.5F)
			var8 = 0;
		f->image[var2 * 4 + 0] = (uint8_t)var5;
		f->image[var2 * 4 + 1] = (uint8_t)var6;
		f->image[var2 * 4 + 2] = (uint8_t)var7;
		f->image[var2 * 4 + 3] = (uint8_t)var8;
	}
}

// RenderEngine.updateDynamicTextures order: FlamesFX(0) then FlamesFX(1).
static void ref_tick_pair(struct ref_flame f[2], struct ref_rng* r) {
	ref_on_tick(&f[0], r);
	ref_on_tick(&f[1], r);
}

static void expect_equal(const struct flame_fx_pair* p,
						 const struct ref_flame ref[2], int tick) {
	for(int i = 0; i < FLAME_FX_INSTANCES; i++) {
		if(memcmp(flame_fx_pair_pixels(p, i), ref[i].image, FLAME_FX_BYTES)
		   != 0) {
			fprintf(stderr, "instance %d pixels diverge at tick %d\n", i, tick);
			assert(0);
		}
		if(memcmp(p->fx[i].field, ref[i].g, sizeof(p->fx[i].field)) != 0) {
			fprintf(stderr, "instance %d field diverges at tick %d\n", i, tick);
			assert(0);
		}
	}
}

// --- (b) consumers --------------------------------------------------------
static float cell_u0(int col, int frame) {
	return (float)((col + (frame / 14) * 7) * 18 + 3) / 256.0f;
}
static float cell_v0(int frame) {
	return (float)((frame % 14) * 18 + 3) / 256.0f;
}

static void check_consumers(void) {
	// Strip geometry: fx[0] -> cols 3 & 10, fx[1] -> cols 6 & 13.
	assert(FLAME_FX_ANIM_COL0 == 3 && FLAME_FX_ANIM_COL1 == 6);
	for(int frame = 0; frame < 28; frame++)
		for(int cell = 0; cell < 2; cell++) {
			float u, v;
			fire_anim_cell_origin(frame, cell, &u, &v);
			assert(u == cell_u0(cell ? 6 : 3, frame) && v == cell_v0(frame));
		}

	// RenderBlocks :798-803 -- the side-catching branch: every side quad takes
	// the second fire tile when the coordinate sum is odd, using two's
	// complement & for negatives, same as C).
	for(int x = -3; x <= 3; x++)
		for(int y = 0; y <= 2; y++)
			for(int z = -3; z <= 3; z++)
				assert(flame_fx_block_side_cell(x, y, z) == ((x + y + z) & 1));
	assert(flame_fx_block_side_cell(-1, 0, 0) == 1);
	assert(flame_fx_block_side_cell(-2, 0, 0) == 0);
	// :865-874 -- under a flammable ceiling: first quad fire, second fire+16.
	assert(flame_fx_block_ceiling_cell(0) == 0);
	assert(flame_fx_block_ceiling_cell(1) == 1);
	// :907-944 -- free-standing, 8 planes in Beta order: inner-X pair (1-2)
	// fire; inner-Z pair (3-4) fire+16; outer-X pair (5-6) STILL fire+16 (no
	// reset before them); outer-Z pair (7-8) reset to fire.
	assert(flame_fx_block_free_cell(false, false) == 0); // planes 1-2
	assert(flame_fx_block_free_cell(true, false) == 1);  // planes 3-4
	assert(flame_fx_block_free_cell(false, true) == 1);  // planes 5-6
	assert(flame_fx_block_free_cell(true, true) == 0);   // planes 7-8

	// ItemRenderer :385 -- quad zero uses fire; quad one uses the tile sixteen positions later.
	assert(flame_fx_overlay_cell(0) == 0 && flame_fx_overlay_cell(1) == 1);
	for(int frame = 0; frame < 28; frame++)
		for(int side = 0; side < 2; side++) {
			struct fire_overlay_quad q = fire_overlay_quad(frame, side);
			float u0 = cell_u0(side ? 6 : 3, frame), v0 = cell_v0(frame);
			float u1 = u0 + 15.99f / 256.0f, v1 = v0 + 15.99f / 256.0f;
			// Beta visits the UV corners: high/high, low/high, low/low, high/low.
			assert(q.uv[0] == u1 && q.uv[1] == v1 && q.uv[2] == u0
				   && q.uv[3] == v1 && q.uv[4] == u0 && q.uv[5] == v0
				   && q.uv[6] == u1 && q.uv[7] == v0);
		}

	// Render :49-64 -- even layers fire, odd layers fire+16.
	for(size_t layer = 0; layer < 12; layer++)
		assert(flame_fx_entity_layer_cell(layer) == (int)(layer % 2));
}

int main(void) {
	// Tile identity: Beta's two cells (TextureFlamesFX.java:8).
	assert(FLAME_FX_INSTANCES == 2);

	struct flame_fx_pair pair;
	flame_fx_pair_init(&pair, SEED);

	struct ref_flame ref[2];
	struct ref_rng rng = {SEED};
	ref_init(&ref[0]);
	ref_init(&ref[1]);
	// (a1) registerTextureFX ticks each instance as it is registered, 0 then 1.
	ref_tick_pair(ref, &rng);
	expect_equal(&pair, ref, 0);

	// (a4) instance 0 drew first from the fresh stream: a lone port instance
	// with the same seed, ticked once, is byte-identical.
	struct flame_fx solo;
	flame_fx_init(&solo, SEED);
	flame_fx_tick(&solo);
	assert(memcmp(flame_fx_pixels(&solo), flame_fx_pair_pixels(&pair, 0),
				  FLAME_FX_BYTES) == 0);
	// ...and instance 1 is NOT that lone instance (it drew the NEXT 64 values).
	assert(memcmp(solo.field, pair.fx[1].field, sizeof(solo.field)) != 0);

	int pixel_diff_ticks = 0, first_diff = -1;
	for(int t = 1; t <= TICKS; t++) {
		flame_fx_pair_tick(&pair);
		ref_tick_pair(ref, &rng);
		expect_equal(&pair, ref, t); // (a2)
		// (a3) independent state every tick.
		assert(memcmp(pair.fx[0].field, pair.fx[1].field,
					  sizeof(pair.fx[0].field))
			   != 0);
		if(memcmp(flame_fx_pair_pixels(&pair, 0), flame_fx_pair_pixels(&pair, 1),
				  FLAME_FX_BYTES)
		   != 0) {
			if(first_diff < 0)
				first_diff = t;
			pixel_diff_ticks++;
		}
	}
	// (a3) visibly different pictures (not just different floats). The top 16
	// rows start flat (zero field) until the flame front climbs into them; from
	// the first tick the two pictures differ, they must differ on EVERY tick.
	printf("pictures first differ at tick %d; differ on %d of %d ticks\n",
		   first_diff, pixel_diff_ticks, TICKS);
	assert(first_diff > 0 && first_diff < 20);
	assert(pixel_diff_ticks == TICKS - first_diff + 1);

	// Determinism: a second pair with the same seed is byte-identical.
	struct flame_fx_pair again;
	flame_fx_pair_init(&again, SEED);
	for(int t = 1; t <= TICKS; t++)
		flame_fx_pair_tick(&again);
	for(int i = 0; i < FLAME_FX_INSTANCES; i++)
		assert(memcmp(flame_fx_pair_pixels(&again, i),
					  flame_fx_pair_pixels(&pair, i), FLAME_FX_BYTES)
			   == 0);

	check_consumers();

	printf("test_flame_fx_two_instances: PASS (%d ticks, 2 instances, shared "
		   "stream 0 then 1)\n",
		   TICKS);
	return 0;
}
