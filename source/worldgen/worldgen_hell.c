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

/*
	Beta 1.7.3 Nether (Hell) terrain generator, ported to match
	ChunkProviderHell + MapGenCavesHell byte-for-byte.

	Pipeline per chunk (exactly Beta ChunkProviderHell.provideChunk's order):
	  1. reseed the surface RNG from the weighted sum of chunk coordinates --
	     note NO world-seed term (a faithful Beta quirk; the terrain SHAPE is
	     seed-dependent through the octaves, but the surface-material roughness
	     RNG depends only on chunk coords). generateNetherTerrain itself draws
	     no RNG.
	  2. density field (func_4057_a): a 5x17x5 low-res grid built from the low/
	     high Perlin fields blended by a selector, biased by a per-Y cosine table
	     that carves the Nether's solid roof + floor.
	  3. block fill (generateNetherTerrain): trilinearly interpolate the grid to
	     16x128x16 and emit lavaStill (y < 32) / netherrack (density > 0) / air.
	  4. surface (func_4058_b): bedrock ceiling + floor (both roughed by nextInt
	     (5)), a soulsand/gravel skin in y in [60..65], lava fill below y=64.
	  5. caves (MapGenCavesHell): the overworld cave carver with Nether tuning
	     (bigger/lower caves, lava-abort, carve netherrack/dirt/grass -> air).
	  6. return with the newly allocated height/light arrays still zero.

	FAITHFULNESS NOTES
	  - The seven octave generators are NOT rebuilt here: Beta's ChunkProviderHell
	    constructs its first seven NoiseGeneratorOctaves using the world seed
	    in the SAME order and with the SAME octave counts as
	    ChunkProviderGenerate, so wg->{low,high,selector,sand_gravel,stone,
	    continentalness,depth} ARE the Hell provider's field_4169_i..field_4176_b,
	    byte-for-byte. We reuse them (and the per-chunk scratch buffers) directly.
	  - The density's continentalness/depth 2D samples and the var17/var19/var21
	    scalars are DEAD in Beta (never read into the grid, never consume RNG) but
	    are retained here for comparison with the corresponding steps in the
	    decompile; see hell_density.
	  - populate() is a SEPARATE, deliberately-scoped pass -- see
	    worldgen_hell_populate.
*/

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "../block/blocks.h"
#include "../block/blocks_data.h"
#include "../lighting.h"
#include "../network/server_world.h"
#include "beta_math.h"
#include "java_random.h"
#include "jmath.h"
#include "noise.h"
#include "populate.h"
#include "worldgen_internal.h"

// ---------------------------------------------------------------------------
// Terrain: density (ChunkProviderHell.func_4057_a)
// ---------------------------------------------------------------------------

// Fill wg->density (the 5x17x5 low-res grid). Transcribed from
// ChunkProviderHell.func_4057_a, sampled at four times the chunk coordinates.
// The grid stores Y contiguously, followed by Z and then X, and its layout
// matches WG_GRID_* and the overworld generator.
static void hell_density(struct worldgen* wg, w_coord_t chunk_x,
						 w_coord_t chunk_z) {
	const int var5 = WG_GRID_X; // 5  (size_x)
	const int var6 = WG_GRID_Y; // 17 (size_y)
	const int var7 = WG_GRID_Z; // 5  (size_z)
	double nx = (double)((w_coord_t)(chunk_x * 4)); // var2
	double nz = (double)((w_coord_t)(chunk_z * 4)); // var4
	const double var8 = 684.412;
	const double var10 = 2053.236; // NOTE 3x the horizontal scale (overworld

								   // uses the same value for both axes)
	// field_4172_f / field_4171_g: Beta samples these (continentalness at scale
	// (1,0,1), depth at (100,0,100)) but -- see the inner loop -- their derived
	// scalars var17/var21 are never read into the density and consume no RNG, so
	// the two fields are DEAD. Computed anyway for a line-exact port; a future
	// T1 could drop these two calls + the scalars with an equivalence proof.
	octave_generate(&wg->continentalness, wg->f_cont, nx, 0.0, nz, var5, 1, var7,
					1.0, 0.0, 1.0);
	octave_generate(&wg->depth, wg->f_depth, nx, 0.0, nz, var5, 1, var7, 100.0,
					0.0, 100.0);
	octave_generate(&wg->selector, wg->f_sel, nx, 0.0, nz, var5, var6, var7,
					var8 / 80.0, var10 / 60.0, var8 / 80.0);
	octave_generate(&wg->low, wg->f_low, nx, 0.0, nz, var5, var6, var7, var8,
					var10, var8);
	octave_generate(&wg->high, wg->f_high, nx, 0.0, nz, var5, var6, var7, var8,
					var10, var8);

	// var14[]: per-Y cosine bias -- this is what gives the Nether its solid
	// ceiling and floor (the overworld density has no such term).
	double y_bias[WG_GRID_Y]; // var14
	for(int var15 = 0; var15 < var6; var15++) {
		y_bias[var15]
			= cos((double)var15 * 3.141592653589793 * 6.0 / (double)var6) * 2.0;
		double var16 = (double)var15;
		if(var15 > var6 / 2)
			var16 = (double)(var6 - 1 - var15);
		if(var16 < 4.0) {
			var16 = 4.0 - var16;
			y_bias[var15] -= var16 * var16 * var16 * 10.0;
		}
	}

	int var12 = 0; // 3D grid index (selector/low/high/density)
	int var13 = 0; // 2D grid index (continentalness/depth)
	for(int var15 = 0; var15 < var5; var15++) {
		for(int var36 = 0; var36 < var7; var36++) {
			// var17/var19/var21: computed exactly as Beta does, but DEAD -- the
			// inner var23 loop never reads them, and none of this draws RNG.
			double var17 = (wg->f_cont[var13] + 256.0) / 512.0;
			if(var17 > 1.0)
				var17 = 1.0;
			double var19 = 0.0;
			double var21 = wg->f_depth[var13] / 8000.0;
			if(var21 < 0.0)
				var21 = -var21;
			var21 = var21 * 3.0 - 3.0;
			if(var21 < 0.0) {
				var21 /= 2.0;
				if(var21 < -1.0)
					var21 = -1.0;
				var21 /= 1.4;
				var21 /= 2.0;
				var17 = 0.0;
			} else {
				if(var21 > 1.0)
					var21 = 1.0;
				var21 /= 6.0;
			}
			var17 += 0.5;
			var21 = var21 * (double)var6 / 16.0;
			(void)var17; // Beta-dead (documented above)
			(void)var19;
			(void)var21;
			var13++;

			for(int var23 = 0; var23 < var6; var23++) {
				double var24 = 0.0;
				double var26 = y_bias[var23];
				double var28 = wg->f_low[var12] / 512.0;
				double var30 = wg->f_high[var12] / 512.0;
				double var32 = (wg->f_sel[var12] / 10.0 + 1.0) / 2.0;
				if(var32 < 0.0)
					var24 = var28;
				else if(var32 > 1.0)
					var24 = var30;
				else
					var24 = var28 + (var30 - var28) * var32;
				var24 -= var26;
				if(var23 > var6 - 4) {
					double var34
						= (double)((float)(var23 - (var6 - 4)) / 3.0f);
					var24 = var24 * (1.0 - var34) + -10.0 * var34;
				}
				// Beta's second fade is below a zero-height threshold and
				// cannot affect any nonnegative grid height -- omitted (dead).
				wg->density[var12] = var24;
				var12++;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Terrain: block fill (ChunkProviderHell.generateNetherTerrain)
// ---------------------------------------------------------------------------

// Trilinearly interpolate wg->density to full resolution and emit
// lavaStill / netherrack / air. Beta stores Y contiguously, then Z, then X;
// WG_IDX uses the same layout: 2048 cells per X step, 128 per Z step.
static void hell_fill_blocks(struct worldgen* wg, struct server_chunk* sc) {
	const int var7 = WG_GRID_Y; // 17
	const int var8 = WG_GRID_Z; // 5
	const int lava_level = 32;	// var5

	for(int var9 = 0; var9 < 4; var9++) {			// macro X (4 blocks each)
		for(int var10 = 0; var10 < 4; var10++) {	// macro Z
			for(int var11 = 0; var11 < 16; var11++) { // macro Y (8 blocks each)
				const double var12 = 0.125;
				double var14
					= wg->density[((var9 + 0) * var8 + var10 + 0) * var7 + var11 + 0];
				double var16
					= wg->density[((var9 + 0) * var8 + var10 + 1) * var7 + var11 + 0];
				double var18
					= wg->density[((var9 + 1) * var8 + var10 + 0) * var7 + var11 + 0];
				double var20
					= wg->density[((var9 + 1) * var8 + var10 + 1) * var7 + var11 + 0];
				double var22
					= (wg->density[((var9 + 0) * var8 + var10 + 0) * var7 + var11 + 1]
					   - var14)
					* var12;
				double var24
					= (wg->density[((var9 + 0) * var8 + var10 + 1) * var7 + var11 + 1]
					   - var16)
					* var12;
				double var26
					= (wg->density[((var9 + 1) * var8 + var10 + 0) * var7 + var11 + 1]
					   - var18)
					* var12;
				double var28
					= (wg->density[((var9 + 1) * var8 + var10 + 1) * var7 + var11 + 1]
					   - var20)
					* var12;

				for(int var30 = 0; var30 < 8; var30++) {
					const double var31 = 0.25;
					double var33 = var14;
					double var35 = var16;
					double var37 = (var18 - var14) * var31;
					double var39 = (var20 - var16) * var31;

					for(int var41 = 0; var41 < 4; var41++) {
						int bx = var41 + var9 * 4;
						int by = var11 * 8 + var30;
						const double var44 = 0.25;
						double var46 = var33;
						double var48 = (var35 - var33) * var44;

						for(int var50 = 0; var50 < 4; var50++) {
							int bz = var10 * 4 + var50;
							int var51 = BLOCK_AIR;
							if(by < lava_level)
								var51 = BLOCK_LAVA_STILL;
							if(var46 > 0.0)
								var51 = BLOCK_NETHERRACK;
							G9B_BYTE(sc->ids, WG_IDX(bx, by, bz), (uint8_t)var51);
							var46 += var48;
						}

						var33 += var37;
						var35 += var39;
					}

					var14 += var22;
					var16 += var24;
					var18 += var26;
					var20 += var28;
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Terrain: surface (ChunkProviderHell.func_4058_b)
// ---------------------------------------------------------------------------

// Bedrock ceiling+floor, soulsand/gravel skin, lava-below-64. Beta names kept;
// Block storage uses the same Y-contiguous, then Z, then X layout as WG_IDX.
static void hell_surface(struct worldgen* wg, struct server_chunk* sc,
						 w_coord_t chunk_x, w_coord_t chunk_z,
						 struct java_random* rng) {
	const int var4 = 64;
	const double var5 = 0.03125;
	double bx = (double)((w_coord_t)(chunk_x * 16));
	double bz = (double)((w_coord_t)(chunk_z * 16));

	// field_4162_p (soulsand), gravelNoise, field_4160_r (netherrack depth).
	octave_generate(&wg->sand_gravel, wg->sand_noise, bx, bz, 0.0, 16, 16, 1,
					var5, var5, 1.0);
	octave_generate(&wg->sand_gravel, wg->gravel_noise, bx, 109.0134, bz, 16, 1,
					16, var5, 1.0, var5);
	octave_generate(&wg->stone, wg->stone_noise, bx, bz, 0.0, 16, 16, 1,
					var5 * 2.0, var5 * 2.0, var5 * 2.0);

	// Beta's hellRNG at this point holds setSeed(cx*.. + cz*..) from
	// provideChunk; generateNetherTerrain drew nothing, so the surface stream
	// starts fresh here (NO world-seed term -- see the file header).
	jrand_set_seed(rng,
				   (int64_t)chunk_x * 341873128712LL
					   + (int64_t)chunk_z * 132897987541LL);

	for(int var7 = 0; var7 < 16; var7++) {
		for(int var8 = 0; var8 < 16; var8++) {
			// Ordered draws (Java left-to-right; C is not): soulsand, gravel,
			// then the netherrack-depth roll.
			double d1 = jrand_next_double(rng);
			bool var9 = wg->sand_noise[var7 + var8 * 16] + d1 * 0.2 > 0.0;
			double d2 = jrand_next_double(rng);
			bool var10 = wg->gravel_noise[var7 + var8 * 16] + d2 * 0.2 > 0.0;
			double d3 = jrand_next_double(rng);
			int var11 = java_double_to_int(
				wg->stone_noise[var7 + var8 * 16] / 3.0 + 3.0 + d3 * 0.25);
			int var12 = -1;
			uint8_t var13 = BLOCK_NETHERRACK; // top
			uint8_t var14 = BLOCK_NETHERRACK; // filler

			for(int var15 = 127; var15 >= 0; var15--) {
				size_t var16 = WG_IDX(var8, var15, var7);

				// Beta always draws the ceiling bedrock roll before checking its height;
				// the floor roll is reached only if the ceiling check fails. Java
				// short-circuit evaluation determines that draw order. The
				// `continue` below reproduces that draw pattern exactly.
				int r_ceil = jrand_next_int_bound(rng, 5);
				if(var15 >= 127 - r_ceil) {
					G9B_BYTE(sc->ids, var16, BLOCK_BEDROCK);
					continue;
				}
				int r_floor = jrand_next_int_bound(rng, 5);
				if(var15 <= 0 + r_floor) {
					G9B_BYTE(sc->ids, var16, BLOCK_BEDROCK);
					continue;
				}

				uint8_t var17 = sc->ids[var16];
				if(var17 == BLOCK_AIR) {
					var12 = -1;
				} else if(var17 == BLOCK_NETHERRACK) {
					if(var12 == -1) {
						if(var11 <= 0) {
							var13 = BLOCK_AIR;
							var14 = BLOCK_NETHERRACK;
						} else if(var15 >= var4 - 4 && var15 <= var4 + 1) {
							var13 = BLOCK_NETHERRACK;
							var14 = BLOCK_NETHERRACK;
							if(var10)
								var13 = BLOCK_GRAVEL;
							if(var10)
								var14 = BLOCK_NETHERRACK;
							if(var9)
								var13 = BLOCK_SOULSAND;
							if(var9)
								var14 = BLOCK_SOULSAND;
						}

						if(var15 < var4 && var13 == BLOCK_AIR)
							var13 = BLOCK_LAVA_STILL;

						var12 = var11;
						if(var15 >= var4 - 1)
							G9B_BYTE(sc->ids, var16, var13);
						else
							G9B_BYTE(sc->ids, var16, var14);
					} else if(var12 > 0) {
						var12--;
						G9B_BYTE(sc->ids, var16, var14);
					}
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Caves (MapGenCavesHell : MapGenBase, range 8)
// ---------------------------------------------------------------------------

#define HL_RANGE 8
#define HL_PI ((float)3.141592653589793)

// MathHelper.floor_double.
static int hell_floor(double x) {
	int i = java_double_to_int(x);
	return x < (double)i ? i - 1 : i;
}

// MapGenCavesHell.generateCaveNode (the recursive tunnel). Identical geometry
// to the overworld MapGenCaves carve EXCEPT: aborts on lava (not water); carves
// netherrack/dirt/grass -> air with no lava floor and no grass restoration.
// `var15` is the vertical scale (0.5 for the tuned Nether nodes).
static void hell_cave_node(struct java_random* outer, int var1, int var2,
						   uint8_t* var3, double var4, double var6, double var8,
						   float var10, float var11, float var12, int var13,
						   int var14, double var15) {
	double var17 = (double)(var1 * 16 + 8);
	double var19 = (double)(var2 * 16 + 8);
	float var21 = 0.0f; // yaw delta
	float var22 = 0.0f; // pitch delta

	struct java_random var23;
	jrand_set_seed(&var23, jrand_next_long(outer));

	if(var14 <= 0) {
		int var24 = HL_RANGE * 16 - 16; // 112
		var14 = var24 - jrand_next_int_bound(&var23, var24 / 4);
	}

	bool var51 = false;
	if(var13 == -1) {
		var13 = var14 / 2;
		var51 = true;
	}

	int var25 = jrand_next_int_bound(&var23, var14 / 2) + var14 / 4;
	bool var26 = jrand_next_int_bound(&var23, 6) == 0;

	for(; var13 < var14; var13++) {
		double var27
			= 1.5
			+ (double)(mh_sin((float)var13 * HL_PI / (float)var14) * var10
					   * 1.0f);
		double var29 = var27 * var15;
		float var31 = mh_cos(var12);
		float var32 = mh_sin(var12);
		var4 += (double)(mh_cos(var11) * var31);
		var6 += (double)var32;
		var8 += (double)(mh_sin(var11) * var31);
		if(var26)
			var12 *= 0.92f;
		else
			var12 *= 0.7f;
		var12 += var22 * 0.1f;
		var11 += var21 * 0.1f;
		var22 *= 0.9f;
		var21 *= 0.75f;

		// Ordered draws (pitch delta first).
		float p1 = jrand_next_float(&var23);
		float p2 = jrand_next_float(&var23);
		float p3 = jrand_next_float(&var23);
		var22 += (p1 - p2) * p3 * 2.0f;
		float y1 = jrand_next_float(&var23);
		float y2 = jrand_next_float(&var23);
		float y3 = jrand_next_float(&var23);
		var21 += (y1 - y2) * y3 * 4.0f;

		if(!var51 && var13 == var25 && var10 > 1.0f) {
			float ra = jrand_next_float(&var23) * 0.5f + 0.5f;
			hell_cave_node(outer, var1, var2, var3, var4, var6, var8, ra,
						   var11 - 1.5707964f, var12 / 3.0f, var13, var14, 1.0);
			float rb = jrand_next_float(&var23) * 0.5f + 0.5f;
			hell_cave_node(outer, var1, var2, var3, var4, var6, var8, rb,
						   var11 + 1.5707964f, var12 / 3.0f, var13, var14, 1.0);
			return;
		}

		if(var51 || jrand_next_int_bound(&var23, 4) != 0) {
			double var33 = var4 - var17;
			double var35 = var8 - var19;
			double var37 = (double)(var14 - var13);
			double var39 = (double)(var10 + 2.0f + 16.0f);
			if(var33 * var33 + var35 * var35 - var37 * var37 > var39 * var39)
				return;

			if(var4 >= var17 - 16.0 - var27 * 2.0
			   && var8 >= var19 - 16.0 - var27 * 2.0
			   && var4 <= var17 + 16.0 + var27 * 2.0
			   && var8 <= var19 + 16.0 + var27 * 2.0) {
				int var52 = hell_floor(var4 - var27) - var1 * 16 - 1;
				int var34 = hell_floor(var4 + var27) - var1 * 16 + 1;
				int var53 = hell_floor(var6 - var29) - 1;
				int var36 = hell_floor(var6 + var29) + 1;
				int var54 = hell_floor(var8 - var27) - var2 * 16 - 1;
				int var38 = hell_floor(var8 + var27) - var2 * 16 + 1;
				if(var52 < 0)
					var52 = 0;
				if(var34 > 16)
					var34 = 16;
				if(var53 < 1)
					var53 = 1;
				if(var36 > 120)
					var36 = 120;
				if(var54 < 0)
					var54 = 0;
				if(var38 > 16)
					var38 = 16;

				// Abort the whole segment if it would open into lava.
				bool var55 = false;
				for(int var40 = var52; !var55 && var40 < var34; var40++) {
					for(int var41 = var54; !var55 && var41 < var38; var41++) {
						for(int var42 = var36 + 1; !var55 && var42 >= var53 - 1;
							var42--) {
							int var43 = (var40 * 16 + var41) * 128 + var42;
							if(var42 >= 0 && var42 < 128) {
								if(var3[var43] == BLOCK_LAVA_FLOW
								   || var3[var43] == BLOCK_LAVA_STILL)
									var55 = true;
								if(var42 != var53 - 1 && var40 != var52
								   && var40 != var34 - 1 && var41 != var54
								   && var41 != var38 - 1)
									var42 = var53;
							}
						}
					}
				}

				if(!var55) {
					for(int var40 = var52; var40 < var34; var40++) {
						double var56
							= ((double)(var40 + var1 * 16) + 0.5 - var4) / var27;
						for(int var43 = var54; var43 < var38; var43++) {
							double var44
								= ((double)(var43 + var2 * 16) + 0.5 - var8)
								/ var27;
							int var46 = (var40 * 16 + var43) * 128 + var36;
							for(int var47 = var36 - 1; var47 >= var53; var47--) {
								double var48
									= ((double)var47 + 0.5 - var6) / var29;
								if(var48 > -0.7
								   && var56 * var56 + var48 * var48
										   + var44 * var44
									   < 1.0) {
									uint8_t var50 = var3[var46];
									if(var50 == BLOCK_NETHERRACK
									   || var50 == BLOCK_DIRT
									   || var50 == BLOCK_GRASS)
										G9B_BYTE(var3, var46, BLOCK_AIR);
								}
								var46--;
							}
						}
					}

					if(var51)
						break;
				}
			}
		}
	}
}

// MapGenCavesHell.recursiveGenerate: per region chunk, choose a cave count and
// spawn tunnels. Differs from the overworld: count nextInt(nextInt(nextInt(10)+
// 1)+1), a 1-in-5 keep gate, y = nextInt(128) (not the overworld's nested
// draw), radius doubled, vertical scale 0.5.
static void hell_cave_spawn(struct java_random* rand, int var2, int var3,
							int var4, int var5, uint8_t* var6) {
	int n0 = jrand_next_int_bound(rand, 10);
	int n1 = jrand_next_int_bound(rand, n0 + 1);
	int var7 = jrand_next_int_bound(rand, n1 + 1);
	if(jrand_next_int_bound(rand, 5) != 0)
		var7 = 0;

	for(int var8 = 0; var8 < var7; var8++) {
		double var9
			= (double)(var2 * 16 + jrand_next_int_bound(rand, 16));
		double var11 = (double)jrand_next_int_bound(rand, 128);
		double var13
			= (double)(var3 * 16 + jrand_next_int_bound(rand, 16));
		int var15 = 1;
		if(jrand_next_int_bound(rand, 4) == 0) {
			// generateLargeCaveNode: radius 1..7, step/length -1, vscale 0.5.
			hell_cave_node(rand, var4, var5, var6, var9, var11, var13,
						   1.0f + jrand_next_float(rand) * 6.0f, 0.0f, 0.0f, -1,
						   -1, 0.5);
			var15 += jrand_next_int_bound(rand, 4);
		}
		for(int var16 = 0; var16 < var15; var16++) {
			float var17 = jrand_next_float(rand) * HL_PI * 2.0f;
			float var18 = (jrand_next_float(rand) - 0.5f) * 2.0f / 8.0f;
			// radius = nextFloat()*2 + nextFloat(), ordered.
			float ra = jrand_next_float(rand);
			float rb = jrand_next_float(rand);
			float var19 = ra * 2.0f + rb;
			hell_cave_node(rand, var4, var5, var6, var9, var11, var13,
						   var19 * 2.0f, var17, var18, 0, 0, 0.5);
		}
	}
}

// MapGenBase.generate (range 8): seed from the world seed, then per region
// chunk in [-8,+8]^2 reseed and recurse. Identical driver to the overworld.
static void hell_cave_generate(int64_t seed, int32_t chunk_x, int32_t chunk_z,
							   uint8_t* ids) {
	beta_math_init();

	struct java_random rand;
	jrand_set_seed(&rand, seed);
	int64_t var7 = jrand_next_long(&rand) / 2LL * 2LL + 1LL;
	int64_t var9 = jrand_next_long(&rand) / 2LL * 2LL + 1LL;

	for(int var11 = chunk_x - HL_RANGE; var11 <= chunk_x + HL_RANGE; var11++) {
		for(int var12 = chunk_z - HL_RANGE; var12 <= chunk_z + HL_RANGE;
			var12++) {
			uint64_t s = ((uint64_t)(int64_t)var11 * (uint64_t)var7
						  + (uint64_t)(int64_t)var12 * (uint64_t)var9)
				^ (uint64_t)seed;
			jrand_set_seed(&rand, (int64_t)s);
			hell_cave_spawn(&rand, var11, var12, chunk_x, chunk_z, ids);
		}
	}
}

// CFR server Chunk.java:61,67-73 + ChunkProviderHell.java:202-209:
// provideChunk returns newly allocated zero height/light arrays after terrain,
// surface and caves. It does not call generateHeightMap/skylight generation.
// In particular raw generated lava stays dark until actual Beta light work
// reaches it. worldgen_alloc_chunk already supplies those zero arrays.

bool worldgen_hell_generate_chunk(struct worldgen* wg, struct server_chunk* sc,
								  w_coord_t chunk_x, w_coord_t chunk_z,
								  struct java_random* rng) {
	// CFR ChunkProviderHell.java:119-184,202-209,300-347: retain surface's
	// actual terminal state for population, without replaying its draws.
	struct java_random standalone;
	if(!rng)
		rng = &standalone;
	// sc is already allocated + AIR-cleared by worldgen_generate_chunk. Beta
	// provideChunk order: (reseed handled inside hell_surface) terrain, surface,
	// caves, with the allocated height/light arrays left untouched.
	hell_density(wg, chunk_x, chunk_z);
	hell_fill_blocks(wg, sc);
	hell_surface(wg, sc, chunk_x, chunk_z, rng);
	hell_cave_generate(wg->seed, chunk_x, chunk_z, sc->ids);
	sc->modified = true;
	return true;
}

// ---------------------------------------------------------------------------
// populate() -- decoration pass (lava springs, fire, glowstone, mushrooms)
//
// SCOPE + DETERMINISM (read before trusting this for parity):
//   Beta's ChunkProviderHell.populate does NOT reseed the RNG (unlike the
//   overworld, whose chunk seed combines coordinate weights and world seed); it uses one
//   world-global hellRNG whose state depends on chunk generation order. So hell
//   populate is INHERENTLY non-deterministic per (seed, chunk) -- a faithful
//   Beta property, not a bug here -- and CANNOT be pinned by a per-seed golden.
//   This function therefore takes the RNG explicitly; the caller owns the
//   shared-RNG policy. Given a fixed RNG state, the draw ORDER, feature COUNTS,
//   and block PLACEMENTS below are byte-exact.
//     WorldGenFlowers (mushroom): FH-16 (docs/fork_hack_audit_2026-08-21.md:192)
//         -- canBlockStay requires full block light below 13 [740c583
//         BlockMushroom.canBlockStay] was previously unevaluated (only the
//         support-block opacity was checked). RESOLVED: see
//         hell_lit_13_or_more below the mushroom placer. mc173 has no Nether
//         generator (src/gen/nether.rs is a stub -- no oracle vector exists for
//         this), so the decompile is the sole authority for this change, same
//         as the rest of this file.
//   The terrain above remains byte-exact.
// ---------------------------------------------------------------------------

static inline uint8_t hw_get(struct wgen_world* w, int x, int y, int z) {
	return w->get(w->ctx, x, y, z);
}
static inline bool hw_air(struct wgen_world* w, int x, int y, int z) {
	return hw_get(w, x, y, z) == BLOCK_AIR;
}
static inline void hw_set(struct wgen_world* w, int x, int y, int z,
						  uint8_t id) {
	/* CFR WorldGenHellLava.java:61, WorldGenFire.java:12,
	 * WorldGenGlowStone1/2.java:12,53, WorldGenFlowers.java:18 all use
	 * plain World.setBlock[WithNotify], never the metadata setter. */
	if(id == BLOCK_FIRE || id == BLOCK_GLOWSTONE)
		wgen_set_plain_notify(w, x, y, z, id);
	else wgen_set_plain(w, x, y, z, id);
}

// Beta opaqueCubeLookup, for the mushroom canBlockStay ground check: a full
// solid cube. Uses the block table when present (real server) and falls back to
// the Nether-relevant opaque set (netherrack/soulsand/bedrock) so this stays
// correct in the block-table-free unit test.
static bool hell_is_opaque_cube(uint8_t id) {
	if(blocks[id])
		return !blocks[id]->can_see_through;
	return id == BLOCK_NETHERRACK || id == BLOCK_SOULSAND || id == BLOCK_BEDROCK
		|| id == BLOCK_STONE || id == BLOCK_DIRT;
}

// WorldGenHellLava.generate. Beta places the moving source, sets
// scheduledUpdatesAreImmediate, calls updateTick before returning, then clears
// the flag. The optional world-view hook performs that immediate cascade on the
// live raw quadrant without per-block render/lighting traffic.
static void hell_feat_lava(struct wgen_world* w, struct java_random* r, int x,
						   int y, int z) {
	if(hw_get(w, x, y + 1, z) != BLOCK_NETHERRACK)
		return;
	uint8_t here = hw_get(w, x, y, z);
	if(here != BLOCK_AIR && here != BLOCK_NETHERRACK)
		return;

	int var6 = 0; // netherrack neighbours (x+-1, z+-1, y-1)
	if(hw_get(w, x - 1, y, z) == BLOCK_NETHERRACK)
		var6++;
	if(hw_get(w, x + 1, y, z) == BLOCK_NETHERRACK)
		var6++;
	if(hw_get(w, x, y, z - 1) == BLOCK_NETHERRACK)
		var6++;
	if(hw_get(w, x, y, z + 1) == BLOCK_NETHERRACK)
		var6++;
	if(hw_get(w, x, y - 1, z) == BLOCK_NETHERRACK)
		var6++;

	int var7 = 0; // air neighbours (same five)
	if(hw_air(w, x - 1, y, z))
		var7++;
	if(hw_air(w, x + 1, y, z))
		var7++;
	if(hw_air(w, x, y, z - 1))
		var7++;
	if(hw_air(w, x, y, z + 1))
		var7++;
	if(hw_air(w, x, y - 1, z))
		var7++;

	if(var6 == 4 && var7 == 1) {
		hw_set(w, x, y, z, BLOCK_LAVA_FLOW);
		if(w->tick_fluid_immediate)
			w->tick_fluid_immediate(w->ctx, r, x, y, z, BLOCK_LAVA_FLOW);
	}
}

// WorldGenFire.generate: 64 tries, air over netherrack -> fire.
static void hell_feat_fire(struct wgen_world* w, struct java_random* r, int x,
						   int y, int z) {
	for(int i = 0; i < 64; i++) {
		int ax = jrand_next_int_bound(r, 8);
		int bx = jrand_next_int_bound(r, 8);
		int var7 = x + ax - bx;
		int ay = jrand_next_int_bound(r, 4);
		int by = jrand_next_int_bound(r, 4);
		int var8 = y + ay - by;
		int az = jrand_next_int_bound(r, 8);
		int bz = jrand_next_int_bound(r, 8);
		int var9 = z + az - bz;
		if(hw_air(w, var7, var8, var9)
		   && hw_get(w, var7, var8 - 1, var9) == BLOCK_NETHERRACK)
			hw_set(w, var7, var8, var9, BLOCK_FIRE);
	}
}

// WorldGenGlowStone1 / WorldGenGlowStone2.generate (identical Beta code): a
// glowstone seed under a netherrack ceiling, then 1500 tries growing a hanging
// cluster where a cell has EXACTLY one glowstone neighbour.
static void hell_feat_glowstone(struct wgen_world* w, struct java_random* r,
								int x, int y, int z) {
	if(!hw_air(w, x, y, z))
		return;
	if(hw_get(w, x, y + 1, z) != BLOCK_NETHERRACK)
		return;
	hw_set(w, x, y, z, BLOCK_GLOWSTONE);

	for(int i = 0; i < 1500; i++) {
		int ax = jrand_next_int_bound(r, 8);
		int bx = jrand_next_int_bound(r, 8);
		int var7 = x + ax - bx;
		int var8 = y - jrand_next_int_bound(r, 12);
		int az = jrand_next_int_bound(r, 8);
		int bz = jrand_next_int_bound(r, 8);
		int var9 = z + az - bz;
		if(hw_get(w, var7, var8, var9) == BLOCK_AIR) {
			int var10 = 0;
			for(int var11 = 0; var11 < 6; var11++) {
				uint8_t var12 = BLOCK_AIR;
				if(var11 == 0)
					var12 = hw_get(w, var7 - 1, var8, var9);
				if(var11 == 1)
					var12 = hw_get(w, var7 + 1, var8, var9);
				if(var11 == 2)
					var12 = hw_get(w, var7, var8 - 1, var9);
				if(var11 == 3)
					var12 = hw_get(w, var7, var8 + 1, var9);
				if(var11 == 4)
					var12 = hw_get(w, var7, var8, var9 - 1);
				if(var11 == 5)
					var12 = hw_get(w, var7, var8, var9 + 1);
				if(var12 == BLOCK_GLOWSTONE)
					var10++;
			}
			if(var10 == 1)
				hw_set(w, var7, var8, var9, BLOCK_GLOWSTONE);
		}
	}
}

/* CFR BlockMushroom.java:53-55 / World.java:getFullBlockLightValue:
 * canBlockStay reads STORED light at the call's actual phase. Generated
 * emitters do not imply propagated light before the deferred queue drains.
 * The original-JAR full-domain witness has a mushroom at (-135,67,37)
 * despite nearby emitter IDs; an eager two-hop flood incorrectly rejected it.
 * Buffer-only Nether fixtures without saved-light storage start unlit. */
static bool hell_lit_13_or_more(struct wgen_world* w, int x, int y, int z) {
	return w->full_light && w->full_light(w->ctx, x, y, z) >= 13;
}

// Public wrapper -- see worldgen.h's declaration for why this is exposed
// standalone (an RNG-independent unit test for hell_lit_13_or_more).
bool worldgen_hell_mushroom_light_gate_blocks(struct wgen_world* w, int x,
											  int y, int z) {
	return hell_lit_13_or_more(w, x, y, z);
}

// WorldGenFlowers.generate for a Nether mushroom: 64 tries, air with an opaque
// cube below and the light gate clear -> mushroom. `plant_id` is
// BLOCK_BROWM_MUSHROOM or BLOCK_RED_MUSHROOM.
static void hell_feat_mushroom(struct wgen_world* w, struct java_random* r,
							   int x, int y, int z, uint8_t plant_id) {
	for(int i = 0; i < 64; i++) {
		int ax = jrand_next_int_bound(r, 8);
		int bx = jrand_next_int_bound(r, 8);
		int var7 = x + ax - bx;
		int ay = jrand_next_int_bound(r, 4);
		int by = jrand_next_int_bound(r, 4);
		int var8 = y + ay - by;
		int az = jrand_next_int_bound(r, 8);
		int bz = jrand_next_int_bound(r, 8);
		int var9 = z + az - bz;
		if(hw_air(w, var7, var8, var9) && var8 >= 0 && var8 < 128
		   && hell_is_opaque_cube(hw_get(w, var7, var8 - 1, var9))
		   && !hell_lit_13_or_more(w, var7, var8, var9))
			hw_set(w, var7, var8, var9, plant_id);
	}
}

void worldgen_hell_populate(struct java_random* rng, int chunk_x, int chunk_z,
							struct wgen_world* w) {
	int var4 = chunk_x * 16;
	int var5 = chunk_z * 16;

	// 8 lava springs.
	for(int var6 = 0; var6 < 8; var6++) {
		int var7 = var4 + jrand_next_int_bound(rng, 16) + 8;
		int var8 = jrand_next_int_bound(rng, 120) + 4;
		int var9 = var5 + jrand_next_int_bound(rng, 16) + 8;
		hell_feat_lava(w, rng, var7, var8, var9);
	}

	// Fire: count nextInt(nextInt(10)+1) + 1.
	int fire_n = jrand_next_int_bound(rng, jrand_next_int_bound(rng, 10) + 1) + 1;
	for(int var7 = 0; var7 < fire_n; var7++) {
		int var8 = var4 + jrand_next_int_bound(rng, 16) + 8;
		int var9 = jrand_next_int_bound(rng, 120) + 4;
		int var10 = var5 + jrand_next_int_bound(rng, 16) + 8;
		hell_feat_fire(w, rng, var8, var9, var10);
	}

	// Glowstone pass 1: count nextInt(nextInt(10)+1) -- NOTE no +1 (unlike fire).
	int glow_n = jrand_next_int_bound(rng, jrand_next_int_bound(rng, 10) + 1);
	for(int var7 = 0; var7 < glow_n; var7++) {
		int var8 = var4 + jrand_next_int_bound(rng, 16) + 8;
		int var9 = jrand_next_int_bound(rng, 120) + 4;
		int var10 = var5 + jrand_next_int_bound(rng, 16) + 8;
		hell_feat_glowstone(w, rng, var8, var9, var10);
	}

	// Glowstone pass 2: exactly 10, y = nextInt(128).
	for(int var7 = 0; var7 < 10; var7++) {
		int var8 = var4 + jrand_next_int_bound(rng, 16) + 8;
		int var9 = jrand_next_int_bound(rng, 128);
		int var10 = var5 + jrand_next_int_bound(rng, 16) + 8;
		hell_feat_glowstone(w, rng, var8, var9, var10);
	}

	// Brown then red mushrooms: each bound-one random check succeeds but STILL draws
	// (advances the RNG one step) -- reproduce the draw.
	if(jrand_next_int_bound(rng, 1) == 0) {
		int var7 = var4 + jrand_next_int_bound(rng, 16) + 8;
		int var8 = jrand_next_int_bound(rng, 128);
		int var9 = var5 + jrand_next_int_bound(rng, 16) + 8;
		hell_feat_mushroom(w, rng, var7, var8, var9, BLOCK_BROWM_MUSHROOM);
	}
	if(jrand_next_int_bound(rng, 1) == 0) {
		int var7 = var4 + jrand_next_int_bound(rng, 16) + 8;
		int var8 = jrand_next_int_bound(rng, 128);
		int var9 = var5 + jrand_next_int_bound(rng, 16) + 8;
		hell_feat_mushroom(w, rng, var7, var8, var9, BLOCK_RED_MUSHROOM);
	}
}
