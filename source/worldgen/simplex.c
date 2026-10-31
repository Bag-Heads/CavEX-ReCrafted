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

#include <math.h>
#include <stdlib.h>

#include "jmath.h"
#include "simplex.h"

#ifdef CAVEWII_WORLDGEN_TEST
static void* (*s_simplex_octaves_alloc)(size_t) = malloc;
void simplex_octaves_test_set_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_simplex_octaves_alloc = alloc_fn ? alloc_fn : malloc;
}
#else
#define s_simplex_octaves_alloc malloc
#endif

// The 12 edge gradients Beta's simplex uses (3D table, only x/y read in 2D).
static const int SIMPLEX_GRAD[12][3] = {
	{1, 1, 0},	{-1, 1, 0},	 {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
	{1, 0, -1}, {-1, 0, -1}, {0, 1, 1},	 {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
};

void simplex_init(struct simplex_noise* s, struct java_random* r) {
	// Identical construction to the Perlin generator (Beta shares it).
	s->xo = jrand_next_double(r) * 256.0;
	s->yo = jrand_next_double(r) * 256.0;
	s->zo = jrand_next_double(r) * 256.0;

	for(int i = 0; i < 256; i++)
		s->permutations[i] = i;

	for(int i = 0; i < 256; i++) {
		int j = jrand_next_int_bound(r, 256 - i) + i;
		int tmp = s->permutations[i];
		s->permutations[i] = s->permutations[j];
		s->permutations[j] = tmp;
		s->permutations[i + 256] = s->permutations[i];
	}
}

// Beta floors with grad>0 ? (int)grad : (int)grad - 1, using the Java int cast.
static int simplex_wrap(double v) {
	return v > 0.0 ? java_double_to_int(v) : java_double_to_int(v) - 1;
}

static double simplex_dot(const int g[3], double x, double y) {
	return (double)g[0] * x + (double)g[1] * y;
}

static double simplex_corner(double t, const int g[3], double x, double y) {
	if(t < 0.0)
		return 0.0;
	t *= t;
	return t * t * simplex_dot(g, x, y);
}

void simplex_generate(const struct simplex_noise* s, double* field, double xoff,
					  double yoff, int width, int height, double xscale,
					  double yscale, double amplitude) {
	const double skewing = 0.5 * (sqrt(3.0) - 1.0);
	const double unskewing = (3.0 - sqrt(3.0)) / 6.0;
	int index = 0;

	for(int xi = 0; xi < width; xi++) {
		double xp = (xoff + (double)xi) * xscale + s->xo;

		for(int yi = 0; yi < height; yi++) {
			double yp = (yoff + (double)yi) * yscale + s->yo;

			double skew = (xp + yp) * skewing;
			int x0 = simplex_wrap(xp + skew);
			int y0 = simplex_wrap(yp + skew);
			double unskew = (double)(x0 + y0) * unskewing;
			double x0a = (double)x0 - unskew;
			double y0a = (double)y0 - unskew;
			double x0b = xp - x0a;
			double y0b = yp - y0a;

			int i1, j1;
			if(x0b > y0b) {
				i1 = 1;
				j1 = 0;
			} else {
				i1 = 0;
				j1 = 1;
			}

			double x1 = x0b - (double)i1 + unskewing;
			double y1 = y0b - (double)j1 + unskewing;
			double x2 = x0b - 1.0 + 2.0 * unskewing;
			double y2 = y0b - 1.0 + 2.0 * unskewing;

			int xint = x0 & 255;
			int yint = y0 & 255;
			int g0 = s->permutations[xint + s->permutations[yint]] % 12;
			int g1
				= s->permutations[xint + i1 + s->permutations[yint + j1]] % 12;
			int g2 = s->permutations[xint + 1 + s->permutations[yint + 1]] % 12;

			double n0 = simplex_corner(0.5 - x0b * x0b - y0b * y0b,
									   SIMPLEX_GRAD[g0], x0b, y0b);
			double n1 = simplex_corner(0.5 - x1 * x1 - y1 * y1,
									   SIMPLEX_GRAD[g1], x1, y1);
			double n2 = simplex_corner(0.5 - x2 * x2 - y2 * y2,
									   SIMPLEX_GRAD[g2], x2, y2);

			field[index++] += 70.0 * (n0 + n1 + n2) * amplitude;
		}
	}
}

bool simplex_octaves_init(struct simplex_octaves* o, struct java_random* r,
						  int count) {
	if(!o)
		return false;
	o->octaves = NULL;
	o->count = 0;
	if(!r || count <= 0
	   || (size_t)count > SIZE_MAX / sizeof(struct simplex_noise))
		return false;
	o->octaves
		= s_simplex_octaves_alloc(sizeof(struct simplex_noise) * (size_t)count);
	if(!o->octaves)
		return false;
	o->count = count;
	for(int i = 0; i < count; i++)
		simplex_init(&o->octaves[i], r);
	return true;
}

void simplex_octaves_free(struct simplex_octaves* o) {
	free(o->octaves);
	o->octaves = NULL;
	o->count = 0;
}

void simplex_octaves_generate(const struct simplex_octaves* o, double* field,
							  double xoff, double yoff, int width, int height,
							  double xscale, double yscale, double freq_mul) {
	xscale /= 1.5;
	yscale /= 1.5;

	int n = width * height;
	for(int i = 0; i < n; i++)
		field[i] = 0.0;

	double freq = 1.0; // Beta var18
	double amp = 1.0;  // Beta var21
	for(int i = 0; i < o->count; i++) {
		simplex_generate(&o->octaves[i], field, xoff, yoff, width, height,
						 xscale * freq, yscale * freq, 0.55 / amp);
		freq *= freq_mul;
		amp *= 0.5;
	}
}
