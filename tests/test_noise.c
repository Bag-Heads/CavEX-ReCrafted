/*
	Tests source/worldgen/noise.c against Beta 1.7.3's NoiseGeneratorPerlin.

	The Perlin math is canonical improved-noise (identical to Beta's grad/lerp/
	fade and corner ordering), so the part that must be pinned is that the
	generator is BUILT from java.util.Random in Beta's exact order — three
	nextDouble()*256 offsets, then a 0..255 shuffle. With java_random already
	proven byte-exact, matching that order means the whole noise field matches
	Beta for a seed. We also check the permutation is well-formed, the output is
	deterministic + bounded, and distinct seeds diverge.
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "worldgen/java_random.h"
#include "worldgen/noise.h"
#include "support/perlin_generate_reference.h"


static double noise_process_ms(void) {
#ifdef _WIN32
	FILETIME created, exited, kernel, user;
	assert(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
	ULARGE_INTEGER k = {.LowPart=kernel.dwLowDateTime, .HighPart=kernel.dwHighDateTime};
	ULARGE_INTEGER u = {.LowPart=user.dwLowDateTime, .HighPart=user.dwHighDateTime};
	return (double)(k.QuadPart + u.QuadPart) / 10000.0;
#else
	return 1000.0 * clock() / CLOCKS_PER_SEC;
#endif
}

static void test_perlin_grid_equiv(void) {
	struct java_random rng;
	struct perlin_noise p;
	double a[5*17*5], b[5*17*5];
	unsigned cases = 0;
	for(int seed = 0; seed < 24; seed++) {
		jrand_set_seed(&rng, seed * 7919 - 12345);
		perlin_init(&p, &rng);
		for(int dims = 0; dims < 3; dims++) {
			int sy = dims == 0 ? 1 : dims == 1 ? 2 : 17;
			for(int octave = 0; octave < 16; octave++) {
				double scale = ldexp(1.0, -octave);
				double off = seed % 2 ? -1048576.25 : 127.75;
				for(int i = 0; i < 5*sy*5; i++) a[i] = b[i] = (i%19-9)*0.125;
				noise_reference_perlin_generate(&p,a,off,-17.5,-off,5,sy,5,
					684.412*scale,8.55515*scale,684.412*scale,scale);
				perlin_generate(&p,b,off,-17.5,-off,5,sy,5,
					684.412*scale,8.55515*scale,684.412*scale,scale);
				assert(memcmp(a,b,sizeof(double)*5*sy*5)==0);
				cases++;
			}
		}
	}
	/* Exercise every hash's coordinate/sign choice at exact integer and
	 * signed-zero boundaries, not only random fractional offsets. */
	const double edge[] = {-0.0, 0.0, -1.0, 1.0, -.25, .25, 255.0, 256.0};
	p.xo = p.yo = p.zo = 0.0;
	for(int hash = 0; hash < 16; hash++) {
		memset(p.permutations,hash,sizeof(p.permutations));
		for(unsigned x = 0; x < sizeof(edge)/sizeof(*edge); x++)
			for(unsigned y = 0; y < sizeof(edge)/sizeof(*edge); y++)
				for(unsigned z = 0; z < sizeof(edge)/sizeof(*edge); z++) {
					double expected = noise_reference_perlin_sample(&p,edge[x],edge[y],edge[z]);
					double actual = perlin_sample(&p,edge[x],edge[y],edge[z]);
					assert(memcmp(&expected,&actual,sizeof(actual)) == 0);
				}
	}
	/* Restore a realistic shuffled permutation for the optional benchmark. */
	jrand_set_seed(&rng, 23 * 7919 - 12345);
	perlin_init(&p, &rng);
	printf("PASS: perlin_grid_equiv %u nonzero-field cases, including repeated Y cells and 2D\n", cases);
	if(getenv("CW_NOISE_BENCH")) {
		volatile double checksum = 0;
		for(int round = 0; round < 6; round++) {
			double ms[2];
			for(int order = 0; order < 2; order++) {
				int candidate = order ^ (round & 1);
				double begin = noise_process_ms();
				for(int k = 0; k < 100000; k++) {
					memset(a,0,sizeof(a));
					double scale = ldexp(1.0,-(k%16));
					if(candidate) perlin_generate(&p,a,k%31,0,-(k%17),5,17,5,684.412*scale,684.412*scale,684.412*scale,scale);
					else noise_reference_perlin_generate(&p,a,k%31,0,-(k%17),5,17,5,684.412*scale,684.412*scale,684.412*scale,scale);
					checksum += a[k%425];
				}
				ms[candidate] = noise_process_ms()-begin;
			}
			printf("noise CPU ms: reference=%.3f candidate=%.3f ratio=%.4f\n",ms[0],ms[1],ms[1]/ms[0]);
		}
		printf("benchmark checksum %.17g\n",checksum);
	}
}

int main(void) {
	test_perlin_grid_equiv();
	struct perlin_noise p;
	struct java_random r;

	// Offsets are the first three nextDouble()*256 of the same Random.
	jrand_set_seed(&r, 12345);
	perlin_init(&p, &r);
	struct java_random ref;
	jrand_set_seed(&ref, 12345);
	assert(fabs(p.xo - jrand_next_double(&ref) * 256.0) < 1e-9);
	assert(fabs(p.yo - jrand_next_double(&ref) * 256.0) < 1e-9);
	assert(fabs(p.zo - jrand_next_double(&ref) * 256.0) < 1e-9);

	// permutations[0..255] is a permutation of 0..255; upper half mirrors it.
	int seen[256] = {0};
	for(int i = 0; i < 256; i++) {
		assert(p.permutations[i] >= 0 && p.permutations[i] < 256);
		seen[p.permutations[i]]++;
		assert(p.permutations[i + 256] == p.permutations[i]);
	}
	for(int i = 0; i < 256; i++)
		assert(seen[i] == 1);

	// Deterministic + bounded + actually varying.
	double v0 = perlin_sample(&p, 1.5, 2.5, 3.5);
	assert(perlin_sample(&p, 1.5, 2.5, 3.5) == v0);
	int nonzero = 0;
	for(int i = 0; i < 2000; i++) {
		double v = perlin_sample(&p, i * 0.013, i * 0.027 + 0.5, i * 0.041 + 1.0);
		assert(v == v); // not NaN
		assert(v >= -1.5 && v <= 1.5);
		if(fabs(v) > 1e-6)
			nonzero++;
	}
	assert(nonzero > 1000);

	// Octave determinism.
	struct octave_noise o, o2;
	jrand_set_seed(&r, 999);
	assert(octave_init(&o, &r, 8));
	jrand_set_seed(&r, 999);
	assert(octave_init(&o2, &r, 8));
	for(int i = 0; i < 100; i++) {
		double a = octave_sample(&o, i * 0.1, 0.0, i * 0.07);
		double b = octave_sample(&o2, i * 0.1, 0.0, i * 0.07);
		assert(a == b);
		assert(a == a);
	}
	octave_free(&o);
	octave_free(&o2);

	// Distinct seeds diverge.
	struct perlin_noise pa, pb;
	struct java_random ra, rb;
	jrand_set_seed(&ra, 1);
	perlin_init(&pa, &ra);
	jrand_set_seed(&rb, 2);
	perlin_init(&pb, &rb);
	assert(perlin_sample(&pa, 3.3, 4.4, 5.5)
		   != perlin_sample(&pb, 3.3, 4.4, 5.5));

	// --- Bulk generator (perlin_generate / octave_generate) ---
	// The terrain uses Beta's bulk array fill, which must agree with the
	// single-sample path it was derived from. A 1-octave octave_generate at
	// amplitude 1 fills field[cell] with the Perlin value at that cell's world
	// coordinate; for grid points whose y lands in distinct integer cells the
	// bulk's per-y-cell gradient cache recomputes every step, so it must equal
	// perlin_sample exactly. This pins the whole bulk path to Beta.
	{
		struct perlin_noise single;
		struct java_random rs;
		jrand_set_seed(&rs, 4242);
		perlin_init(&single, &rs);

		struct octave_noise one;
		jrand_set_seed(&rs, 4242);
		assert(octave_init(&one, &rs, 1)); // identical generator to `single`

		const int SX = 3, SY = 5, SZ = 3;
		double field[3 * 5 * 3];
		const double xoff = 1.0, yoff = 2.0, zoff = 3.0;
		const double sx = 0.7, sy = 1.3, sz = 0.9; // sy*spacing => distinct cells
		octave_generate(&one, field, xoff, yoff, zoff, SX, SY, SZ, sx, sy, sz);

		int idx = 0;
		for(int ix = 0; ix < SX; ix++)
			for(int iz = 0; iz < SZ; iz++)
				for(int iy = 0; iy < SY; iy++) {
					double expect = perlin_sample(&single, (xoff + ix) * sx,
												  (yoff + iy) * sy,
												  (zoff + iz) * sz);
					assert(fabs(field[idx] - expect) < 1e-9);
					idx++;
				}
		octave_free(&one);
	}

	// Bulk octave determinism + the 2D (size_y == 1) path stays finite.
	{
		struct octave_noise oa, ob;
		struct java_random rr;
		jrand_set_seed(&rr, 13);
		assert(octave_init(&oa, &rr, 6));
		jrand_set_seed(&rr, 13);
		assert(octave_init(&ob, &rr, 6));

		double fa[5 * 1 * 5], fb[5 * 1 * 5];
		octave_generate(&oa, fa, 10.0, 0.0, -7.0, 5, 1, 5, 1.121, 1.0, 1.121);
		octave_generate(&ob, fb, 10.0, 0.0, -7.0, 5, 1, 5, 1.121, 1.0, 1.121);
		for(int i = 0; i < 25; i++) {
			assert(fa[i] == fa[i]);
			assert(fa[i] == fb[i]);
		}
		octave_free(&oa);
		octave_free(&ob);
	}

	printf("test_noise: all assertions passed (Beta Perlin construction + "
		   "math + bulk generators)\n");
	return 0;
}
