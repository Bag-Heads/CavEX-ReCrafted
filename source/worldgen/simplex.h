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

#ifndef WORLDGEN_SIMPLEX_H
#define WORLDGEN_SIMPLEX_H

#include <stdbool.h>
#include <stddef.h>

#include "java_random.h"

// Minecraft Beta 1.7.3 NoiseGeneratorSimplex. Beta uses Simplex noise (NOT the
// Perlin generator) for the biome temperature/humidity/weirdness fields, and
// the terrain density couples to those (continentalness is scaled by a humidity
// term), so a faithful terrain requires this too. Construction matches Beta
// exactly: three nextDouble()*256 offsets then a Fisher-Yates shuffle of
// 0..255 -- identical to the Perlin setup -- but the sampling is 2D simplex.
struct simplex_noise {
	int permutations[512];
	double xo, yo, zo;
};

void simplex_init(struct simplex_noise* s, struct java_random* r);

// Bulk 2D fill (Beta NoiseGeneratorSimplex.generateNoise array overload).
// Samples a width*height grid; sample (xi,yi) is taken at
// ((xoff+xi)*xscale, (yoff+yi)*yscale). Results are ADDED into `field`, index
// order x-outer y-inner: field[xi*height + yi].
void simplex_generate(const struct simplex_noise* s, double* field, double xoff,
					  double yoff, int width, int height, double xscale,
					  double yscale, double amplitude);

// Beta NoiseGeneratorOctaves<Simplex>. `count` simplex octaves seeded in
// sequence from one Random.
struct simplex_octaves {
	struct simplex_noise* octaves;
	int count;
};

bool simplex_octaves_init(struct simplex_octaves* o, struct java_random* r,
						  int count);
void simplex_octaves_free(struct simplex_octaves* o);
#ifdef CAVEWII_WORLDGEN_TEST
void simplex_octaves_test_set_alloc_hook(void* (*alloc_fn)(size_t));
#endif

// Beta's simplex octave driver. Note the quirks reproduced here: the x/y scales
// are divided by 1.5 up front; per octave the frequency multiplies by
// `freq_mul` while the amplitude is 0.55 divided by a factor that halves each
// octave. `field` must hold width*height doubles.
void simplex_octaves_generate(const struct simplex_octaves* o, double* field,
							  double xoff, double yoff, int width, int height,
							  double xscale, double yscale, double freq_mul);

#endif
