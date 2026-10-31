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

#ifndef WORLDGEN_NOISE_H
#define WORLDGEN_NOISE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "java_random.h"

// Minecraft Beta 1.7.3 NoiseGeneratorPerlin: Ken Perlin's improved 3D noise with
// a 512-entry permutation table and a (xo,yo,zo) offset, all built from a
// java.util.Random in the exact order Beta uses (three nextDouble()*256 offsets,
// then a Fisher-Yates shuffle of 0..255 via nextInt(256-i)+i). With the PRNG
// matched byte-for-byte (java_random) this reproduces Beta's noise field.
struct perlin_noise {
	// Values are 0..255, so a byte table is 4x smaller (512 B vs 2 KB) -> far more
	// of the 16-octave working set fits L1, and the random pm[hash] lookups in the
	// hot Perlin loop miss less. Reads promote uint8_t->int, so pm[a]+ym etc. is the
	// SAME integer arithmetic -> byte-identical noise (the determinism test proves it).
	uint8_t permutations[512];
	double xo, yo, zo;
};

void perlin_init(struct perlin_noise* p, struct java_random* r);

// Single sample, equivalent to Beta NoiseGeneratorPerlin.generateNoise(x,y,z).
double perlin_sample(const struct perlin_noise* p, double x, double y, double z);

// Beta NoiseGeneratorOctaves: `count` independent Perlin generators (each seeded
// from the same Random in sequence). octave_sample sums them as Beta does — the
// i-th octave is sampled at frequency d (d = 1, 1/2, 1/4, ...) and contributes
// amplitude 1/d, so low-frequency octaves dominate the shape.
//
// NOTE: this is the single-sample summation. The terrain generator uses Beta's
// bulk generateNoiseOctaves (with the large-coordinate floor/modulo precision
// trick); that lives with the terrain code where it is sampled.
struct octave_noise {
	struct perlin_noise* octaves;
	int count;
};

bool octave_init(struct octave_noise* o, struct java_random* r, int count);
void octave_free(struct octave_noise* o);
#ifdef CAVEWII_WORLDGEN_TEST
void octave_test_set_alloc_hook(void* (*alloc_fn)(size_t));
#endif
double octave_sample(const struct octave_noise* o, double x, double y, double z);

// Bulk noise fill, equivalent to Beta NoiseGeneratorPerlin.generateNoise (the
// array overload) and NoiseGeneratorOctaves.generateNoiseOctaves. This is what
// the terrain generator actually uses: it samples a regular grid in one call,
// reusing per-cell gradients down each column the way Beta does.
//
// CRITICAL faithfulness contract (mirrors Beta exactly):
//   - The sample at grid cell (ix,iy,iz) is taken at world coordinate
//     ((xoff+ix)*scale_x, (yoff+iy)*scale_y, (zoff+iz)*scale_z), i.e. the
//     offset is applied BEFORE scaling and there is no large-coordinate modulo
//     fold (see jmath.h on the farlands).
//   - Results are ADDED into `field` (the caller zeroes it first) so octaves
//     accumulate.
//   - Index order is x-outer, z-middle, y-inner: field[(ix*size_z+iz)*size_y+iy].
//   - When size_y == 1 Beta takes a distinct 2D branch (a flat z-grid); pass
//     size_y = 1 to get it. Several Beta noise calls are "2D" in exactly this
//     way and even rely on the resulting x/z transpose, so callers must match
//     Beta's argument order verbatim.
void perlin_generate(const struct perlin_noise* p, double* field, double xoff,
					 double yoff, double zoff, int size_x, int size_y,
					 int size_z, double scale_x, double scale_y, double scale_z,
					 double amplitude);

// Beta NoiseGeneratorOctaves.generateNoiseOctaves: zero `field`, then for each
// of the `count` octaves sample at frequency `scale * 2^-i` with amplitude
// `2^i` (octave 0 = base scale, amplitude 1) and accumulate. `field` must hold
// size_x*size_y*size_z doubles.
void octave_generate(const struct octave_noise* o, double* field, double xoff,
					 double yoff, double zoff, int size_x, int size_y,
					 int size_z, double scale_x, double scale_y,
					 double scale_z);

#endif
