/*
	Microbenchmark for the mesher hot loops: reference vs optimized, in a
	tight single-threaded loop. This isolates COMPUTE cost — unlike the
	in-game overlay's wall-clock "mesher build" timer, which on the
	single-core Wii conflates compute with the worker thread being preempted
	by the render/server threads during the busy cold world-load. Use this
	(not the wall-clock max) to judge whether the AO and BFS rewrites
	actually reduce work.

	Prints per-call microseconds and the speedup for:
	  - chunk_mesher_vertex_light (smooth-lighting AO precompute)
	  - chunk_test_init (occlusion-culling reachability BFS)
	on a realistic mixed-density grid. Always exits 0 (it's a benchmark, not a
	pass/fail gate); read its stdout.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHUNK_SIZE 16
#define N CHUNK_SIZE
#define P (CHUNK_SIZE + 2)
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define BLK_INDEX(x, y, z) ((x) + ((z) + (y) * P) * P)
#define BLK_INDEX2(x, y, z) ((x) + ((z) + (y) * N) * N)
#define BLK_DATA(b, x, y, z) ((b)[BLK_INDEX((x) + 1, (y) + 1, (z) + 1)])
enum { SIDE_LEFT, SIDE_RIGHT, SIDE_BOTTOM, SIDE_TOP, SIDE_FRONT, SIDE_BACK };

struct bd_t {
	uint8_t type;
	uint8_t sky_light;
	uint8_t torch_light;
};

static uint8_t passes[256];   // light predicate
static uint8_t seethru[256];  // BFS predicate

/* ===================== vertex_light (AO) ===================== */

static void vl_ref(struct bd_t* bd, uint8_t* ld) {
	const int sh[5] = {0, 5, 3, 1, 0};
	for(uint32_t y = 0; y < P; y++)
		for(uint32_t z = 0; z < P; z++)
			for(uint32_t x = 0; x < P; x++) {
#define AVG(o, a, b, c, d)                                                     \
	do {                                                                       \
		struct bd_t s[4] = {a, b, c, d};                                       \
		int ss = 0, st = 0, ct = 0;                                            \
		for(int k = 0; k < 4; k++)                                             \
			if(passes[s[k].type]) {                                            \
				ss += s[k].sky_light;                                          \
				st += s[k].torch_light;                                        \
				ct++;                                                          \
			}                                                                  \
		st = ct > 0 ? (st + ct - 1) / ct : 0;                                  \
		ss = ct > 0 ? (ss + ct - 1) / ct : 0;                                  \
		st = MAX(st - sh[ct], 0);                                              \
		ss = MAX(ss - sh[ct], 0);                                              \
		ld[BLK_INDEX(x, y, z) * 3 + o] = (st << 4) | ss;                       \
	} while(0)
				if(x != P - 1 && z != P - 1)
					AVG(0, BLK_DATA(bd, x, y - 1, z),
						BLK_DATA(bd, x - 1, y - 1, z),
						BLK_DATA(bd, x, y - 1, z - 1),
						BLK_DATA(bd, x - 1, y - 1, z - 1));
				if(y != P - 1 && z != P - 1)
					AVG(1, BLK_DATA(bd, x - 1, y, z),
						BLK_DATA(bd, x - 1, y - 1, z),
						BLK_DATA(bd, x - 1, y, z - 1),
						BLK_DATA(bd, x - 1, y - 1, z - 1));
				if(x != P - 1 && y != P - 1)
					AVG(2, BLK_DATA(bd, x, y, z - 1),
						BLK_DATA(bd, x - 1, y, z - 1),
						BLK_DATA(bd, x, y - 1, z - 1),
						BLK_DATA(bd, x - 1, y - 1, z - 1));
#undef AVG
			}
}

static inline uint8_t ao_avg(const struct bd_t* s0, const struct bd_t* s1,
							 const struct bd_t* s2, const struct bd_t* s3) {
	static const int sh[5] = {0, 5, 3, 1, 0};
	const struct bd_t* s[4] = {s0, s1, s2, s3};
	int ss = 0, st = 0, ct = 0;
	for(int k = 0; k < 4; k++)
		if(passes[s[k]->type]) {
			ss += s[k]->sky_light;
			st += s[k]->torch_light;
			ct++;
		}
	st = ct > 0 ? (st + ct - 1) / ct : 0;
	ss = ct > 0 ? (ss + ct - 1) / ct : 0;
	st = MAX(st - sh[ct], 0);
	ss = MAX(ss - sh[ct], 0);
	return (uint8_t)((st << 4) | ss);
}

static void vl_opt(struct bd_t* bd, uint8_t* ld) {
	const int SX = 1, SZ = P, SY = P * P;
	for(uint32_t y = 0; y < P; y++)
		for(uint32_t z = 0; z < P; z++)
			for(uint32_t x = 0; x < P; x++) {
				size_t o = BLK_INDEX(x, y, z) * 3;
				if(x != P - 1 && z != P - 1) {
					struct bd_t* b = &BLK_DATA(bd, x, y - 1, z);
					ld[o + 0] = ao_avg(b, b - SX, b - SZ, b - SZ - SX);
				}
				if(y != P - 1 && z != P - 1) {
					struct bd_t* b = &BLK_DATA(bd, x - 1, y, z);
					ld[o + 1] = ao_avg(b, b - SY, b - SZ, b - SY - SZ);
				}
				if(x != P - 1 && y != P - 1) {
					struct bd_t* b = &BLK_DATA(bd, x, y, z - 1);
					ld[o + 2] = ao_avg(b, b - SX, b - SY, b - SY - SX);
				}
			}
}

/* ===================== reachability BFS ===================== */

static void bfs_ref_test(struct bd_t* bd, int* sx, int* sy, int* sz,
						 bool* vis, uint8_t* re, int x, int y, int z) {
	if(vis[BLK_INDEX2(x, y, z)] || !seethru[BLK_DATA(bd, x, y, z).type])
		return;
	int sp = 0;
	sx[sp] = x;
	sy[sp] = y;
	sz[sp] = z;
	sp++;
	vis[BLK_INDEX2(x, y, z)] = true;
	uint8_t rs = 0;
	static const int off[6][3]
		= {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	while(sp > 0) {
		sp--;
		int bx = sx[sp], by = sy[sp], bz = sz[sp];
		if(bx == 0)
			rs |= 1 << SIDE_LEFT;
		if(bx == N - 1)
			rs |= 1 << SIDE_RIGHT;
		if(by == 0)
			rs |= 1 << SIDE_BOTTOM;
		if(by == N - 1)
			rs |= 1 << SIDE_TOP;
		if(bz == 0)
			rs |= 1 << SIDE_FRONT;
		if(bz == N - 1)
			rs |= 1 << SIDE_BACK;
		for(int s = 0; s < 6; s++) {
			int nx = bx + off[s][0], ny = by + off[s][1], nz = bz + off[s][2];
			if(nx >= 0 && ny >= 0 && nz >= 0 && nx < N && ny < N && nz < N
			   && !vis[BLK_INDEX2(nx, ny, nz)]
			   && seethru[BLK_DATA(bd, nx, ny, nz).type]) {
				vis[BLK_INDEX2(nx, ny, nz)] = true;
				sx[sp] = nx;
				sy[sp] = ny;
				sz[sp] = nz;
				sp++;
			}
		}
	}
	for(int s = 0; s < 6; s++)
		if(rs & (1 << s))
			re[s] |= rs;
}

static void bfs_ref(struct bd_t* bd, uint8_t* re) {
	static bool vis[N * N * N];
	static int sx[N * N * N], sy[N * N * N], sz[N * N * N];
	memset(re, 0, 6);
	memset(vis, 0, sizeof(vis));
	for(int y = 0; y < N; y++)
		for(int x = 0; x < N; x++) {
			bfs_ref_test(bd, sx, sy, sz, vis, re, x, y, 0);
			bfs_ref_test(bd, sx, sy, sz, vis, re, x, 0, y);
			bfs_ref_test(bd, sx, sy, sz, vis, re, 0, x, y);
			bfs_ref_test(bd, sx, sy, sz, vis, re, x, y, N - 1);
			bfs_ref_test(bd, sx, sy, sz, vis, re, x, N - 1, y);
			bfs_ref_test(bd, sx, sy, sz, vis, re, N - 1, x, y);
		}
}

static void bfs_opt_test(struct bd_t* bd, int* stk, bool* vis, uint8_t* re,
						 int x, int y, int z) {
	const int VZ = N, VY = N * N, BZ = P, BY = P * P;
	int seed = x | (z << 4) | (y << 8);
	int sbi = (x + 1) + (z + 1) * BZ + (y + 1) * BY;
	if(vis[seed] || !seethru[bd[sbi].type])
		return;
	int sp = 0;
	stk[sp++] = seed;
	vis[seed] = true;
	uint8_t rs = 0;
	while(sp > 0) {
		int i = stk[--sp];
		int cx = i & 15, cz = (i >> 4) & 15, cy = (i >> 8) & 15;
		int bi = (cx + 1) + (cz + 1) * BZ + (cy + 1) * BY;
		if(cx == 0)
			rs |= 1 << SIDE_LEFT;
		if(cx == N - 1)
			rs |= 1 << SIDE_RIGHT;
		if(cy == 0)
			rs |= 1 << SIDE_BOTTOM;
		if(cy == N - 1)
			rs |= 1 << SIDE_TOP;
		if(cz == 0)
			rs |= 1 << SIDE_FRONT;
		if(cz == N - 1)
			rs |= 1 << SIDE_BACK;
#define TRY(ok, ni, nbi)                                                       \
	do {                                                                       \
		if((ok) && !vis[ni] && seethru[bd[nbi].type]) {                        \
			vis[ni] = true;                                                    \
			stk[sp++] = (ni);                                                  \
		}                                                                      \
	} while(0)
		TRY(cx > 0, i - 1, bi - 1);
		TRY(cx < N - 1, i + 1, bi + 1);
		TRY(cz > 0, i - VZ, bi - BZ);
		TRY(cz < N - 1, i + VZ, bi + BZ);
		TRY(cy > 0, i - VY, bi - BY);
		TRY(cy < N - 1, i + VY, bi + BY);
#undef TRY
	}
	for(int s = 0; s < 6; s++)
		if(rs & (1 << s))
			re[s] |= rs;
}

static void bfs_opt(struct bd_t* bd, uint8_t* re) {
	static bool vis[N * N * N];
	static int stk[N * N * N];
	memset(re, 0, 6);
	memset(vis, 0, sizeof(vis));
	for(int y = 0; y < N; y++)
		for(int x = 0; x < N; x++) {
			bfs_opt_test(bd, stk, vis, re, x, y, 0);
			bfs_opt_test(bd, stk, vis, re, x, 0, y);
			bfs_opt_test(bd, stk, vis, re, 0, x, y);
			bfs_opt_test(bd, stk, vis, re, x, y, N - 1);
			bfs_opt_test(bd, stk, vis, re, x, N - 1, y);
			bfs_opt_test(bd, stk, vis, re, N - 1, x, y);
		}
}

static double ms_per(clock_t dt, int iters) {
	return (double)dt / CLOCKS_PER_SEC * 1000.0 / iters;
}

int main(void) {
	static struct bd_t bd[P * P * P];
	static uint8_t ld[P * P * P * 3];
	uint8_t re[6];

	srand(12345);
	// Realistic-ish: ~half see-through types, ~60% air cells (an open-ish
	// chunk — close to the BFS worst case the overlay flagged, but with real
	// geometry so the emit/AO loops also do work).
	for(int t = 0; t < 256; t++) {
		passes[t] = (t < 160) ? 1 : 0;
		seethru[t] = (t < 160) ? 1 : 0;
	}
	for(int i = 0; i < P * P * P; i++) {
		bd[i].type = (rand() % 100 < 60) ? 0 : (uint8_t)(rand() & 0xFF);
		bd[i].sky_light = rand() % 16;
		bd[i].torch_light = rand() % 16;
	}

	const int VL_IT = 20000, BFS_IT = 20000;
	volatile uint8_t sink = 0;
	clock_t t0;

	t0 = clock();
	for(int i = 0; i < VL_IT; i++)
		vl_ref(bd, ld);
	clock_t vl_r = clock() - t0;
	sink ^= ld[7];

	t0 = clock();
	for(int i = 0; i < VL_IT; i++)
		vl_opt(bd, ld);
	clock_t vl_o = clock() - t0;
	sink ^= ld[7];

	t0 = clock();
	for(int i = 0; i < BFS_IT; i++)
		bfs_ref(bd, re);
	clock_t bf_r = clock() - t0;
	sink ^= re[0];

	t0 = clock();
	for(int i = 0; i < BFS_IT; i++)
		bfs_opt(bd, re);
	clock_t bf_o = clock() - t0;
	sink ^= re[0];

	printf("vertex_light: ref %.4f ms/call, opt %.4f ms/call, speedup %.2fx\n",
		   ms_per(vl_r, VL_IT), ms_per(vl_o, VL_IT),
		   (double)vl_r / (double)(vl_o ? vl_o : 1));
	printf("reachable BFS: ref %.4f ms/call, opt %.4f ms/call, speedup %.2fx\n",
		   ms_per(bf_r, BFS_IT), ms_per(bf_o, BFS_IT),
		   (double)bf_r / (double)(bf_o ? bf_o : 1));
	(void)sink;
	return 0;
}
