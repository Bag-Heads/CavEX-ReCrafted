/* Frozen pre-optimization implementation, 98314e8 + cadence changes.
 * Beta 740c583 NoiseGeneratorPerlin.func_805_a: preserve x/z/y order,
 * repeated-y gradient reuse and every floating-point operation.
 * Do not update this reference to follow optimized production code. */
#include "worldgen/jmath.h"
static double noise_reference_fade(double t) {
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static double noise_reference_lerp(double t, double a, double b) {
	return a + t * (b - a);
}

static double noise_reference_grad(int hash, double x, double y, double z) {
	int h = hash & 15;
	double u = h < 8 ? x : y;
	double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

static double noise_reference_grad2(int hash, double x, double z) {
	int h = hash & 15;
	double u = (double)(1 - ((h & 8) >> 3)) * x;
	double v = h < 4 ? 0.0 : (h == 12 || h == 14 ? x : z);
	return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

static void noise_reference_perlin_generate(const struct perlin_noise* p, double* field, double xoff,
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
			double u = noise_reference_fade(fx);

			for(int z = 0; z < size_z; z++) {
				double fz = (zoff + z) * scale_z + p->zo;
				int iz = java_double_to_int(fz);
				if(fz < (double)iz)
					iz--;
				int pz = iz & 255;
				fz -= iz;
				double w = noise_reference_fade(fz);

				int a = pm[px] + 0;
				int aa = pm[a] + pz;
				int b = pm[px + 1] + 0;
				int ba = pm[b] + pz;

				double x1 = noise_reference_lerp(u, noise_reference_grad2(pm[aa], fx, fz),
								 noise_reference_grad(pm[ba], fx - 1.0, 0.0, fz));
				double x2 = noise_reference_lerp(u, noise_reference_grad(pm[aa + 1], fx, 0.0, fz - 1.0),
								 noise_reference_grad(pm[ba + 1], fx - 1.0, 0.0, fz - 1.0));

				field[index++] += noise_reference_lerp(w, x1, x2) * inv_amp;
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
	// Per-y hoist: py, the fractional fy, noise_reference_fade(fy) and fy-1.0 depend ONLY on y,
	// but the original recomputed them inside the x/z loops -- size_x*size_z
	// (= 25 for the 5x17x5 density grid) times each, every one paying a
	// java_double_to_int floor + a noise_reference_fade(). Precompute them once per call. This is
	// BYTE-IDENTICAL: they are pure functions of y, so the stored doubles equal
	// the recomputed ones bit-for-bit, and the index order + last_py carry are
	// untouched (so octave accumulation is unchanged). Likewise fx-1.0/fz-1.0 and
	// pm[px]/pm[px+1] are loop-invariant in the inner loops and hoisted. Proven by
	// the worldgen golden-digest / determinism tests.
	int ypm[size_y];	   // py = floor((yoff+y)*scale_y + yo) & 255
	double yfrac[size_y];  // the fractional fy
	double yfrac1[size_y]; // fy - 1.0
	double yfade[size_y];  // noise_reference_fade(fy)
	for(int y = 0; y < size_y; y++) {
		double fy = (yoff + y) * scale_y + p->yo;
		int iy = java_double_to_int(fy);
		if(fy < (double)iy)
			iy--;
		ypm[y] = iy & 255;
		fy -= iy;
		yfrac[y] = fy;
		yfrac1[y] = fy - 1.0;
		yfade[y] = noise_reference_fade(fy);
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
		double u = noise_reference_fade(fx);
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
			double w = noise_reference_fade(fz);
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

					c_aa = noise_reference_lerp(u, noise_reference_grad(pm[AA], fx, fy, fz),
								noise_reference_grad(pm[BA], fx1, fy, fz));
					c_ab = noise_reference_lerp(u, noise_reference_grad(pm[AB], fx, fy1, fz),
								noise_reference_grad(pm[BB], fx1, fy1, fz));
					c_ba = noise_reference_lerp(u, noise_reference_grad(pm[AA + 1], fx, fy, fz1),
								noise_reference_grad(pm[BA + 1], fx1, fy, fz1));
					c_bb = noise_reference_lerp(u, noise_reference_grad(pm[AB + 1], fx, fy1, fz1),
								noise_reference_grad(pm[BB + 1], fx1, fy1, fz1));
				}

				double v = yfade[y];
				double i1 = noise_reference_lerp(v, c_aa, c_ab);
				double i2 = noise_reference_lerp(v, c_ba, c_bb);
				field[index++] += noise_reference_lerp(w, i1, i2) * inv_amp;
			}
		}
	}
}

static double noise_reference_perlin_sample(const struct perlin_noise* p, double x, double y, double z) {
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
	double u = noise_reference_fade(dx), v = noise_reference_fade(dy), w = noise_reference_fade(dz);

	const uint8_t* pm = p->permutations;
	int a = pm[xm] + ym;
	int aa = pm[a] + zm;
	int ab = pm[a + 1] + zm;
	int b = pm[xm + 1] + ym;
	int ba = pm[b] + zm;
	int bb = pm[b + 1] + zm;

	return noise_reference_lerp(w,
				noise_reference_lerp(v,
					 noise_reference_lerp(u, noise_reference_grad(pm[aa], dx, dy, dz),
						  noise_reference_grad(pm[ba], dx - 1.0, dy, dz)),
					 noise_reference_lerp(u, noise_reference_grad(pm[ab], dx, dy - 1.0, dz),
						  noise_reference_grad(pm[bb], dx - 1.0, dy - 1.0, dz))),
				noise_reference_lerp(v,
					 noise_reference_lerp(u, noise_reference_grad(pm[aa + 1], dx, dy, dz - 1.0),
						  noise_reference_grad(pm[ba + 1], dx - 1.0, dy, dz - 1.0)),
					 noise_reference_lerp(u, noise_reference_grad(pm[ab + 1], dx, dy - 1.0, dz - 1.0),
						  noise_reference_grad(pm[bb + 1], dx - 1.0, dy - 1.0, dz - 1.0))));
}
