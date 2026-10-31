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

#include <stdlib.h>

#include "jmath.h"
#include "noise.h"

#ifdef CAVEWII_WORLDGEN_TEST
static void* (*s_octave_alloc)(size_t) = malloc;
void octave_test_set_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_octave_alloc = alloc_fn ? alloc_fn : malloc;
}
#else
#define s_octave_alloc malloc
#endif

void perlin_init(struct perlin_noise* p, struct java_random* r) {
	// Beta builds the offsets first, then the permutation, all from one Random.
	p->xo = jrand_next_double(r) * 256.0;
	p->yo = jrand_next_double(r) * 256.0;
	p->zo = jrand_next_double(r) * 256.0;

	for(int i = 0; i < 256; i++)
		p->permutations[i] = i;

	for(int i = 0; i < 256; i++) {
		int j = jrand_next_int_bound(r, 256 - i) + i;
		int tmp = p->permutations[i];
		p->permutations[i] = p->permutations[j];
		p->permutations[j] = tmp;
		p->permutations[i + 256] = p->permutations[i];
	}
}

static double fade(double t) {
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static double lerp(double t, double a, double b) {
	return a + t * (b - a);
}

// Beta NoiseGeneratorPerlin.grad [740c583]. T1 candidate: table-select the
// SAME two coordinates and signs, eliminating data-dependent branches.
// Multiplication by exactly +1/-1 preserves each finite IEEE double, including
// signed zero; the final addition and every surrounding lerp stay in order.
// test_noise compares whole fields and signed-zero/boundary samples bytewise
// against the frozen scalar reference. No RNG, SIMD or approximation change.
static double grad(int hash, double x, double y, double z) {
	static const unsigned char vi[16] = {1,1,1,1,2,2,2,2,2,2,2,2,0,2,0,2};
	static const double sign[2] = {1.0,-1.0};
	int h = hash & 15;
	const double coord[3] = {x,y,z};
	double u = coord[h >> 3];
	double v = coord[vi[h]];
	return u * sign[h & 1] + v * sign[(h >> 1) & 1];
}

double perlin_sample(const struct perlin_noise* p, double x, double y, double z) {
	double dx = x + p->xo, dy = y + p->yo, dz = z + p->zo;

	// floor (Beta does an (int) cast then decrements if it rounded up).
	int ix = java_double_to_int(dx), iy = java_double_to_int(dy),
		iz = java_double_to_int(dz);
	if(dx < (double)ix)
		ix--;
	if(dy < (double)iy)
		iy--;
	if(dz < (double)iz)
		iz--;

	int xm = ix & 255, ym = iy & 255, zm = iz & 255;
	dx -= ix;
	dy -= iy;
	dz -= iz;
	double u = fade(dx), v = fade(dy), w = fade(dz);

	const uint8_t* pm = p->permutations;
	int a = pm[xm] + ym;
	int aa = pm[a] + zm;
	int ab = pm[a + 1] + zm;
	int b = pm[xm + 1] + ym;
	int ba = pm[b] + zm;
	int bb = pm[b + 1] + zm;

	return lerp(w,
				lerp(v,
					 lerp(u, grad(pm[aa], dx, dy, dz),
						  grad(pm[ba], dx - 1.0, dy, dz)),
					 lerp(u, grad(pm[ab], dx, dy - 1.0, dz),
						  grad(pm[bb], dx - 1.0, dy - 1.0, dz))),
				lerp(v,
					 lerp(u, grad(pm[aa + 1], dx, dy, dz - 1.0),
						  grad(pm[ba + 1], dx - 1.0, dy, dz - 1.0)),
					 lerp(u, grad(pm[ab + 1], dx, dy - 1.0, dz - 1.0),
						  grad(pm[bb + 1], dx - 1.0, dy - 1.0, dz - 1.0))));
}

bool octave_init(struct octave_noise* o, struct java_random* r, int count) {
	if(!o)
		return false;
	o->octaves = NULL;
	o->count = 0;
	if(!r || count <= 0
	   || (size_t)count > SIZE_MAX / sizeof(struct perlin_noise))
		return false;
	o->octaves = s_octave_alloc(sizeof(struct perlin_noise) * (size_t)count);
	if(!o->octaves)
		return false;
	o->count = count;
	for(int i = 0; i < count; i++)
		perlin_init(&o->octaves[i], r);
	return true;
}

void octave_free(struct octave_noise* o) {
	free(o->octaves);
	o->octaves = NULL;
	o->count = 0;
}

double octave_sample(const struct octave_noise* o, double x, double y, double z) {
	double sum = 0.0;
	double d = 1.0;
	for(int i = 0; i < o->count; i++) {
		sum += perlin_sample(&o->octaves[i], x * d, y * d, z * d) / d;
		d /= 2.0;
	}
	return sum;
}

// Beta's 2-argument gradient (NoiseGeneratorPerlin uses a separate grad for the
// first corner of the 2D path). It is provably identical to grad(hash, x, 0, z)
// -- expand both and they reduce to the same expression -- but kept distinct to
// mirror the Beta source one-to-one.
static double grad2(int hash, double x, double z) {
	int h = hash & 15;
	double u = (double)(1 - ((h & 8) >> 3)) * x;
	double v = h < 4 ? 0.0 : (h == 12 || h == 14 ? x : z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

void perlin_generate(const struct perlin_noise* p, double* field, double xoff,
					 double yoff, double zoff, int size_x, int size_y,
					 int size_z, double scale_x, double scale_y, double scale_z,
					 double amplitude) {
	const uint8_t* pm = p->permutations;
	double inv_amp = 1.0 / amplitude;
	int index = 0;

	if(size_y == 1) {
		// 2D path: a flat (x,z) grid. y is ignored entirely.
		for(int x = 0; x < size_x; x++) {
			double fx = (xoff + x) * scale_x + p->xo;
			int ix = java_double_to_int(fx);
			if(fx < (double)ix)
				ix--;
			int px = ix & 255;
			fx -= ix;
			double u = fade(fx);

			for(int z = 0; z < size_z; z++) {
				double fz = (zoff + z) * scale_z + p->zo;
				int iz = java_double_to_int(fz);
				if(fz < (double)iz)
					iz--;
				int pz = iz & 255;
				fz -= iz;
				double w = fade(fz);

				int a = pm[px] + 0;
				int aa = pm[a] + pz;
				int b = pm[px + 1] + 0;
				int ba = pm[b] + pz;

				double x1 = lerp(u, grad2(pm[aa], fx, fz),
								 grad(pm[ba], fx - 1.0, 0.0, fz));
				double x2 = lerp(u, grad(pm[aa + 1], fx, 0.0, fz - 1.0),
								 grad(pm[ba + 1], fx - 1.0, 0.0, fz - 1.0));

				field[index++] += lerp(w, x1, x2) * inv_amp;
			}
		}
		return;
	}

	// 3D path. The four x-interpolated corner pairs are cached and only
	// recomputed when the integer y-cell changes -- exactly Beta's
	// optimization, which (because the cached corner gradients keep the
	// first-in-cell fractional y) is observable in the output for
	// high-frequency octaves, so it must be reproduced rather than
	// "corrected".
	//
	// Per-y hoist: py, the fractional fy, fade(fy) and fy-1.0 depend ONLY on y,
	// but the original recomputed them inside the x/z loops -- size_x*size_z
	// (= 25 for the 5x17x5 density grid) times each, every one paying a
	// java_double_to_int floor + a fade(). Precompute them once per call. This is
	// BYTE-IDENTICAL: they are pure functions of y, so the stored doubles equal
	// the recomputed ones bit-for-bit, and the index order + last_py carry are
	// untouched (so octave accumulation is unchanged). Likewise fx-1.0/fz-1.0 and
	// pm[px]/pm[px+1] are loop-invariant in the inner loops and hoisted. Proven by
	// the worldgen golden-digest / determinism tests.
	int ypm[size_y];	   // py = floor((yoff+y)*scale_y + yo) & 255
	double yfrac[size_y];  // the fractional fy
	double yfrac1[size_y]; // fy - 1.0
	double yfade[size_y];  // fade(fy)
	for(int y = 0; y < size_y; y++) {
		double fy = (yoff + y) * scale_y + p->yo;
		int iy = java_double_to_int(fy);
		if(fy < (double)iy)
			iy--;
		ypm[y] = iy & 255;
		fy -= iy;
		yfrac[y] = fy;
		yfrac1[y] = fy - 1.0;
		yfade[y] = fade(fy);
	}

	int last_py = -1;
	double c_aa = 0.0, c_ab = 0.0, c_ba = 0.0, c_bb = 0.0;

	for(int x = 0; x < size_x; x++) {
		double fx = (xoff + x) * scale_x + p->xo;
		int ix = java_double_to_int(fx);
		if(fx < (double)ix)
			ix--;
		int px = ix & 255;
		fx -= ix;
		double u = fade(fx);
		double fx1 = fx - 1.0;		// invariant in the z and y loops
		int pm_px = pm[px];
		int pm_px1 = pm[px + 1];

		for(int z = 0; z < size_z; z++) {
			double fz = (zoff + z) * scale_z + p->zo;
			int iz = java_double_to_int(fz);
			if(fz < (double)iz)
				iz--;
			int pz = iz & 255;
			fz -= iz;
			double w = fade(fz);
			double fz1 = fz - 1.0;	// invariant in the y loop

			for(int y = 0; y < size_y; y++) {
				int py = ypm[y];

				if(y == 0 || py != last_py) {
					last_py = py;
					double fy = yfrac[y];
					double fy1 = yfrac1[y];
					int A = pm_px + py;
					int AA = pm[A] + pz;
					int AB = pm[A + 1] + pz;
					int B = pm_px1 + py;
					int BA = pm[B] + pz;
					int BB = pm[B + 1] + pz;

					c_aa = lerp(u, grad(pm[AA], fx, fy, fz),
								grad(pm[BA], fx1, fy, fz));
					c_ab = lerp(u, grad(pm[AB], fx, fy1, fz),
								grad(pm[BB], fx1, fy1, fz));
					c_ba = lerp(u, grad(pm[AA + 1], fx, fy, fz1),
								grad(pm[BA + 1], fx1, fy, fz1));
					c_bb = lerp(u, grad(pm[AB + 1], fx, fy1, fz1),
								grad(pm[BB + 1], fx1, fy1, fz1));
				}

				double v = yfade[y];
				double i1 = lerp(v, c_aa, c_ab);
				double i2 = lerp(v, c_ba, c_bb);
				field[index++] += lerp(w, i1, i2) * inv_amp;
			}
		}
	}
}

void octave_generate(const struct octave_noise* o, double* field, double xoff,
					 double yoff, double zoff, int size_x, int size_y,
					 int size_z, double scale_x, double scale_y,
					 double scale_z) {
	int n = size_x * size_y * size_z;
	for(int i = 0; i < n; i++)
		field[i] = 0.0;

	double scale = 1.0;
	for(int i = 0; i < o->count; i++) {
		perlin_generate(&o->octaves[i], field, xoff, yoff, zoff, size_x, size_y,
						size_z, scale_x * scale, scale_y * scale, scale_z * scale,
						scale);
		scale /= 2.0;
	}
}
