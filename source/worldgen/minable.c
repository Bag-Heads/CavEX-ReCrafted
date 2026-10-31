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
	Beta 1.7.3 WorldGenMinable, ported byte-for-byte.

	IMPORTANT: BetrockServer's GenerateMinable has axis-swapped bound variables
	(minX/maxX/... assigned from the wrong centres, loops indexing {x,z,y}) and
	does NOT reproduce Java's geometry. This port instead follows the real
	WorldGenMinable -- a clean x/y/z ellipsoid swept along a line -- while
	keeping BetrockServer's (faithful) PRNG order.

	As with the cave carver, the Random consumption order is load-bearing and C
	does not order the operands of multi-call expressions like Java does, so any
	expression drawing more than once uses explicit ordered temporaries. Here
	the only such case is the two nextInt(3) y-endpoint draws, split into
	separate statements (Java evaluates them in two sequential assignments).
*/

#include "../block/blocks_data.h"
#include "beta_math.h"
#include "java_random.h"
#include "jmath.h"
#include "populate.h"

// Beta casts Math.PI to float in WorldGenMinable.
#define MN_PI ((float)3.141592653589793)

// MathHelper.floor_double: Java (int) cast (truncate toward zero) then adjust
// down for negative non-integers. Identical to the cave carver's cv_floor.
static int mn_floor(double x) {
	int i = java_double_to_int(x);
	return x < (double)i ? i - 1 : i;
}

void worldgen_minable(struct wgen_world* w, struct java_random* rand, int i,
					  int j, int k, int size, uint8_t ore_id, uint8_t ore_meta) {
	beta_math_init();

	// Endpoints of the vein's spine: centred at (i+8, k+8), swung by `angle`
	// and reaching +/- size/8 in the horizontal plane. All float arithmetic,
	// matching Java's (double)((float)(i+8) + sin(f)*(float)size/8F).
	float angle = jrand_next_float(rand) * MN_PI;
	float si = mh_sin(angle);
	float co = mh_cos(angle);
	float fi8 = (float)(i + 8);
	float fk8 = (float)(k + 8);
	float sterm = si * (float)size / 8.0f;
	float cterm = co * (float)size / 8.0f;
	double x_start = (double)(fi8 + sterm);
	double x_end = (double)(fi8 - sterm);
	double z_start = (double)(fk8 + cterm);
	double z_end = (double)(fk8 - cterm);

	// Two y endpoints: j + nextInt(3) + 2, drawn in order.
	int ry0 = jrand_next_int_bound(rand, 3);
	double y_start = (double)(j + ry0 + 2);
	int ry1 = jrand_next_int_bound(rand, 3);
	double y_end = (double)(j + ry1 + 2);

	for(int l = 0; l <= size; l++) {
		double cx = x_start + (x_end - x_start) * (double)l / (double)size;
		double cy = y_start + (y_end - y_start) * (double)l / (double)size;
		double cz = z_start + (z_end - z_start) * (double)l / (double)size;
		double scale = jrand_next_double(rand) * (double)size / 16.0;
		// Sphere radius peaks at the spine's middle. xz and y radius are equal
		// (Beta computes the same expression into both d10 and d11).
		float bell = mh_sin((float)l * MN_PI / (float)size) + 1.0f;
		double rxz = (double)bell * scale + 1.0;
		double ry = (double)bell * scale + 1.0;

		int x_lo = mn_floor(cx - rxz / 2.0);
		int x_hi = mn_floor(cx + rxz / 2.0);
		int y_lo = mn_floor(cy - ry / 2.0);
		int y_hi = mn_floor(cy + ry / 2.0);
		int z_lo = mn_floor(cz - rxz / 2.0);
		int z_hi = mn_floor(cz + rxz / 2.0);

		for(int bx = x_lo; bx <= x_hi; bx++) {
			double dx = ((double)bx + 0.5 - cx) / (rxz / 2.0);
			if(dx * dx < 1.0) {
				for(int by = y_lo; by <= y_hi; by++) {
					double dy = ((double)by + 0.5 - cy) / (ry / 2.0);
					if(dx * dx + dy * dy < 1.0) {
						for(int bz = z_lo; bz <= z_hi; bz++) {
							double dz = ((double)bz + 0.5 - cz) / (rxz / 2.0);
							if(dx * dx + dy * dy + dz * dz < 1.0
							   && w->get(w->ctx, bx, by, bz) == BLOCK_STONE)
								{
								if(ore_meta) w->set(w->ctx, bx, by, bz, ore_id, ore_meta);
								else wgen_set_plain(w, bx, by, bz, ore_id);
							}
						}
					}
				}
			}
		}
	}
}
