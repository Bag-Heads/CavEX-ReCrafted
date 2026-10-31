/*
	Tests source/worldgen/simplex.c against Beta 1.7.3's NoiseGeneratorSimplex
	(used for the biome temperature/humidity/weirdness fields the terrain reads).

	As with the Perlin generator, the math is canonical simplex noise; the part
	that must be pinned to Beta is that the generator is BUILT from
	java.util.Random in Beta's exact order (three nextDouble()*256 offsets, then
	a 0..255 shuffle). With java_random proven byte-exact, matching that order
	reproduces Beta's field for a seed. We also check the permutation is
	well-formed and that output is finite, deterministic, varying, and
	seed-sensitive.
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "worldgen/java_random.h"
#include "worldgen/simplex.h"

int main(void) {
	struct simplex_noise s;
	struct java_random r;

	// Construction: offsets are the first three nextDouble()*256 of the same
	// Random; permutations[0..255] is a permutation of 0..255 mirrored above.
	jrand_set_seed(&r, 54321);
	simplex_init(&s, &r);
	struct java_random ref;
	jrand_set_seed(&ref, 54321);
	assert(fabs(s.xo - jrand_next_double(&ref) * 256.0) < 1e-9);
	assert(fabs(s.yo - jrand_next_double(&ref) * 256.0) < 1e-9);
	assert(fabs(s.zo - jrand_next_double(&ref) * 256.0) < 1e-9);

	int seen[256] = {0};
	for(int i = 0; i < 256; i++) {
		assert(s.permutations[i] >= 0 && s.permutations[i] < 256);
		seen[s.permutations[i]]++;
		assert(s.permutations[i + 256] == s.permutations[i]);
	}
	for(int i = 0; i < 256; i++)
		assert(seen[i] == 1);

	// Single-generator determinism + finiteness + variation. simplex_generate
	// ACCUMULATES into the field, so each run starts from a zeroed buffer.
	double a[256], b[256];
	for(int i = 0; i < 256; i++)
		a[i] = 0.0;
	simplex_generate(&s, a, 10.0, 20.0, 16, 16, 0.05, 0.05, 1.0);
	for(int i = 0; i < 256; i++)
		b[i] = 0.0;
	simplex_generate(&s, b, 10.0, 20.0, 16, 16, 0.05, 0.05, 1.0);

	int nonzero = 0;
	for(int i = 0; i < 256; i++) {
		assert(a[i] == a[i]); // not NaN
		assert(a[i] == b[i]); // deterministic
		if(fabs(a[i]) > 1e-9)
			nonzero++;
	}
	assert(nonzero > 64);

	// Octave generation: same seed is identical, different seed diverges.
	struct simplex_octaves o1, o2, o3;
	struct java_random r1, r2, r3;
	jrand_set_seed(&r1, 7);
	assert(simplex_octaves_init(&o1, &r1, 4));
	jrand_set_seed(&r2, 7);
	assert(simplex_octaves_init(&o2, &r2, 4));
	jrand_set_seed(&r3, 8);
	assert(simplex_octaves_init(&o3, &r3, 4));

	double f1[256], f2[256], f3[256];
	simplex_octaves_generate(&o1, f1, 0.0, 0.0, 16, 16, 0.025, 0.025, 0.25);
	simplex_octaves_generate(&o2, f2, 0.0, 0.0, 16, 16, 0.025, 0.025, 0.25);
	simplex_octaves_generate(&o3, f3, 0.0, 0.0, 16, 16, 0.025, 0.025, 0.25);

	int diff = 0;
	for(int i = 0; i < 256; i++) {
		assert(f1[i] == f1[i]);
		assert(f1[i] == f2[i]); // same seed -> identical
		if(fabs(f1[i] - f3[i]) > 1e-9)
			diff++; // different seed -> diverges
	}
	assert(diff > 64);

	simplex_octaves_free(&o1);
	simplex_octaves_free(&o2);
	simplex_octaves_free(&o3);

	printf("test_simplex: all assertions passed (Beta Simplex construction + "
		   "octaves)\n");
	return 0;
}
