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
	Beta 1.7.3 cave generator, ported to match MapGenCaves byte-for-byte.

	IMPORTANT for faithfulness: the Random consumption order is load-bearing,
	and C (unlike Java) does NOT order the operands of `a() - b()` or
	`a()*2 + b()`, so every expression with more than one Random draw is split
	into explicit ordered temporaries. Two PRNGs are in play: the caver's
	`outer` Random selects caves per region chunk; each tunnel seeds a fresh
	local `r2` from outer.nextLong() and draws everything from r2.
*/

#include <stdbool.h>

#include "../block/blocks_data.h"
#ifdef CAVEWII_SERVER_LOCAL_TEST
#include "../lighting.h"
#else
#define G9B_BYTE(array, index, value) ((array)[index] = (value))
#endif
#include "beta_math.h"
#include "cave.h"
#include "java_random.h"
#include "jmath.h"

#define CV_WX 16
#define CV_WZ 16
#define CV_H 128
#define CV_EXTENT 8

// Beta casts PI to float in the carve.
#define CV_PI ((float)3.141592653589793)

// MathHelper.floor_double, reproduced exactly: Java (int) cast (truncate toward
// zero) then adjust down for negative non-integers.
static int cv_floor(double x) {
	int i = java_double_to_int(x);
	return x < (double)i ? i - 1 : i;
}

// Beta CarveCave (recursive tunnel). `outer` is the caver's member Random; this
// invocation seeds a fresh local r2 and draws everything from it. Offset is by
// value (Beta passes Vec3 by value, so a tunnel never moves its caller's).
static void cave_carve(struct java_random* outer, int cx, int cz, uint8_t* ids,
					   double ox, double oy, double oz, float tunnel_radius,
					   float carve_yaw, float carve_pitch, int tunnel_step,
					   int tunnel_length, double vertical_scale) {
	double center_x = (double)(cx * CV_WX + 8);
	double center_z = (double)(cz * CV_WZ + 8);
	float yaw_delta = 0.0f;	  // var21
	float pitch_delta = 0.0f; // var22

	struct java_random r2;
	jrand_set_seed(&r2, jrand_next_long(outer));

	if(tunnel_length <= 0) {
		int v24 = CV_EXTENT * 16 - 16; // 112
		tunnel_length = v24 - jrand_next_int_bound(&r2, v24 / 4);
	}

	bool single = false;
	if(tunnel_step == -1) {
		tunnel_step = tunnel_length / 2;
		single = true;
	}

	int branch = jrand_next_int_bound(&r2, tunnel_length / 2) + tunnel_length / 4;
	bool steep = jrand_next_int_bound(&r2, 6) == 0;

	for(; tunnel_step < tunnel_length; tunnel_step++) {
		double radius
			= 1.5
			+ (double)(mh_sin((float)tunnel_step * CV_PI / (float)tunnel_length)
					   * tunnel_radius * 1.0f);
		double vrad = radius * vertical_scale;
		float cp = mh_cos(carve_pitch);
		float sp = mh_sin(carve_pitch);
		ox += (double)(mh_cos(carve_yaw) * cp);
		oy += (double)sp;
		oz += (double)(mh_sin(carve_yaw) * cp);
		carve_pitch *= steep ? 0.92f : 0.7f;
		carve_pitch += pitch_delta * 0.1f;
		carve_yaw += yaw_delta * 0.1f;
		pitch_delta *= 0.9f;
		yaw_delta *= 12.0f / 16.0f;

		// Ordered draws (Java is left-to-right; C is not). Pitch delta first.
		float p1 = jrand_next_float(&r2);
		float p2 = jrand_next_float(&r2);
		float p3 = jrand_next_float(&r2);
		pitch_delta += (p1 - p2) * p3 * 2.0f;
		float y1 = jrand_next_float(&r2);
		float y2 = jrand_next_float(&r2);
		float y3 = jrand_next_float(&r2);
		yaw_delta += (y1 - y2) * y3 * 4.0f;

		if(!single && tunnel_step == branch && tunnel_radius > 1.0f) {
			float ra = jrand_next_float(&r2) * 0.5f + 0.5f;
			cave_carve(outer, cx, cz, ids, ox, oy, oz, ra,
					   carve_yaw - CV_PI * 0.5f, carve_pitch / 3.0f, tunnel_step,
					   tunnel_length, 1.0);
			float rb = jrand_next_float(&r2) * 0.5f + 0.5f;
			cave_carve(outer, cx, cz, ids, ox, oy, oz, rb,
					   carve_yaw + CV_PI * 0.5f, carve_pitch / 3.0f, tunnel_step,
					   tunnel_length, 1.0);
			return;
		}

		if(single || jrand_next_int_bound(&r2, 4) != 0) {
			double dx_c = ox - center_x;
			double dz_c = oz - center_z;
			double remain = (double)(tunnel_length - tunnel_step);
			double reach = (double)(tunnel_radius + 2.0f + 16.0f);
			if(dx_c * dx_c + dz_c * dz_c - remain * remain > reach * reach)
				return;

			if(ox >= center_x - 16.0 - radius * 2.0
			   && oz >= center_z - 16.0 - radius * 2.0
			   && ox <= center_x + 16.0 + radius * 2.0
			   && oz <= center_z + 16.0 + radius * 2.0) {
				int x_min = cv_floor(ox - radius) - cx * 16 - 1;
				int x_max = cv_floor(ox + radius) - cx * 16 + 1;
				int y_min = cv_floor(oy - vrad) - 1;
				int y_max = cv_floor(oy + vrad) + 1;
				int z_min = cv_floor(oz - radius) - cz * 16 - 1;
				int z_max = cv_floor(oz + radius) - cz * 16 + 1;
				if(x_min < 0)
					x_min = 0;
				if(x_max > 16)
					x_max = 16;
				if(y_min < 1)
					y_min = 1;
				if(y_max > 120)
					y_max = 120;
				if(z_min < 0)
					z_min = 0;
				if(z_max > 16)
					z_max = 16;

				// Skip the whole segment if it would carve into water.
				bool water = false;
				for(int bx = x_min; !water && bx < x_max; bx++) {
					for(int bz = z_min; !water && bz < z_max; bz++) {
						for(int by = y_max + 1; !water && by >= y_min - 1; by--) {
							int idx = (bx * CV_WZ + bz) * CV_H + by;
							if(by >= 0 && by < CV_H) {
								uint8_t t = ids[idx];
								if(t == BLOCK_WATER_FLOW || t == BLOCK_WATER_STILL)
									water = true;
								if(by != y_min - 1 && bx != x_min
								   && bx != x_max - 1 && bz != z_min
								   && bz != z_max - 1)
									by = y_min;
							}
						}
					}
				}

				if(!water) {
					for(int bx = x_min; bx < x_max; bx++) {
						double fx = ((double)(bx + cx * 16) + 0.5 - ox) / radius;
						for(int bz = z_min; bz < z_max; bz++) {
							double fz
								= ((double)(bz + cz * 16) + 0.5 - oz) / radius;
							// Beta starts the index at yMax (one above the first
							// tested y) and decrements; the carved block is thus
							// the tested y + 1 -- reproduced exactly.
							int idx = (bx * CV_WZ + bz) * CV_H + y_max;
							bool grass_above = false; // var47
							if(fx * fx + fz * fz < 1.0) {
								for(int by = y_max - 1; by >= y_min; by--) {
									double fy = ((double)by + 0.5 - oy) / vrad;
									if(fy > -0.7
									   && fx * fx + fy * fy + fz * fz < 1.0) {
										uint8_t t = ids[idx];
										if(t == BLOCK_GRASS)
											grass_above = true;
										if(t == BLOCK_STONE || t == BLOCK_DIRT
										   || t == BLOCK_GRASS) {
											if(by < 10) {
												G9B_BYTE(ids, idx, BLOCK_LAVA_FLOW);
											} else {
												G9B_BYTE(ids, idx, BLOCK_AIR);
												if(grass_above
												   && ids[idx - 1] == BLOCK_DIRT)
													G9B_BYTE(ids, idx - 1, BLOCK_GRASS);
											}
										}
									}
									idx--;
								}
							}
						}
					}

					if(single)
						break;
				}
			}
		}
	}
}

// Beta GenerateCaves: per region chunk, choose a cave count + spawn tunnels.
static void cave_spawn(struct java_random* rand, int chunk_off_x,
					   int chunk_off_z, int cx, int cz, uint8_t* ids) {
	// numberOfCaves = nextInt(nextInt(nextInt(40)+1)+1) -- ordered innermost
	// first.
	int n0 = jrand_next_int_bound(rand, 40);
	int n1 = jrand_next_int_bound(rand, n0 + 1);
	int num_caves = jrand_next_int_bound(rand, n1 + 1);
	if(jrand_next_int_bound(rand, 15) != 0)
		num_caves = 0;

	for(int ci = 0; ci < num_caves; ci++) {
		double ox = (double)(chunk_off_x * CV_WX
							 + jrand_next_int_bound(rand, CV_WX));
		int oy_inner = jrand_next_int_bound(rand, 120);
		double oy = (double)jrand_next_int_bound(rand, oy_inner + 8);
		double oz = (double)(chunk_off_z * CV_WZ
							 + jrand_next_int_bound(rand, CV_WZ));
		int num_nodes = 1;
		if(jrand_next_int_bound(rand, 4) == 0) {
			cave_carve(rand, cx, cz, ids, ox, oy, oz,
					   1.0f + jrand_next_float(rand) * 6.0f, 0.0f, 0.0f, -1, -1,
					   0.5);
			num_nodes += jrand_next_int_bound(rand, 4);
		}
		for(int ni = 0; ni < num_nodes; ni++) {
			float yaw = jrand_next_float(rand) * CV_PI * 2.0f;
			float pitch = (jrand_next_float(rand) - 0.5f) * 2.0f / 8.0f;
			// radius = nextFloat()*2 + nextFloat(), ordered.
			float r_a = jrand_next_float(rand);
			float r_b = jrand_next_float(rand);
			float radius = r_a * 2.0f + r_b;
			cave_carve(rand, cx, cz, ids, ox, oy, oz, radius, yaw, pitch, 0, 0,
					   1.0);
		}
	}
}

void cave_generate(int64_t seed, int32_t chunk_x, int32_t chunk_z,
				   uint8_t* ids) {
	beta_math_init();

	struct java_random rand;
	jrand_set_seed(&rand, seed);
	int64_t x_off = jrand_next_long(&rand) / 2LL * 2LL + 1LL;
	int64_t z_off = jrand_next_long(&rand) / 2LL * 2LL + 1LL;

	for(int co_x = chunk_x - CV_EXTENT; co_x <= chunk_x + CV_EXTENT; co_x++) {
		for(int co_z = chunk_z - CV_EXTENT; co_z <= chunk_z + CV_EXTENT;
			co_z++) {
			// (cX*xOffset + cZ*zOffset) ^ seed in Java long (wrapping) math;
			// use unsigned to keep the wrap well-defined in C.
			uint64_t s = ((uint64_t)(int64_t)co_x * (uint64_t)x_off
						  + (uint64_t)(int64_t)co_z * (uint64_t)z_off)
				^ (uint64_t)seed;
			jrand_set_seed(&rand, (int64_t)s);
			cave_spawn(&rand, co_x, co_z, chunk_x, chunk_z, ids);
		}
	}
}
