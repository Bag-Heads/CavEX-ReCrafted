/*
	Equivalence test for the optimized occlusion-culling reachability BFS in
	source/chunk_mesher.c (chunk_test / chunk_test_init).

	The optimization swaps the generic 3-byte-memcpy stack for a plain int
	index stack, the per-neighbour blocks[type] deref for a see_through[256]
	table, and blocks_side_offset for inlined +-index arithmetic on packed
	BLK_INDEX2 cell indices. None of that may change the reachable[] result
	(it drives chunk culling — a behaviour change would make chunks wrongly
	vanish or render). This embeds the original algorithm and the optimized
	one, runs both over randomized grids (incl. all-air, all-solid, and mixed
	densities), and asserts identical reachable[6].
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE 16
#define N CHUNK_SIZE
#define P (CHUNK_SIZE + 2)

#define BLK_INDEX(x, y, z) ((x) + ((z) + (y) * P) * P)
#define BLK_INDEX2(x, y, z) ((x) + ((z) + (y) * N) * N)
#define BLK_DATA(b, x, y, z) ((b)[BLK_INDEX((x) + 1, (y) + 1, (z) + 1)])

enum { SIDE_LEFT, SIDE_RIGHT, SIDE_BOTTOM, SIDE_TOP, SIDE_FRONT, SIDE_BACK };

struct bd_t {
	uint8_t type;
};

static uint8_t see_through[256];

// ---- Reference: original algorithm (explicit coords, 6-offset loop). ----
static void ref_test(struct bd_t* bd, int* sx, int* sy, int* sz, bool* visited,
					 uint8_t* reachable, int x, int y, int z) {
	if(visited[BLK_INDEX2(x, y, z)] || !see_through[BLK_DATA(bd, x, y, z).type])
		return;
	int sp = 0;
	sx[sp] = x;
	sy[sp] = y;
	sz[sp] = z;
	sp++;
	visited[BLK_INDEX2(x, y, z)] = true;
	uint8_t reached = 0;
	static const int off[6][3]
		= {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	while(sp > 0) {
		sp--;
		int bx = sx[sp], by = sy[sp], bz = sz[sp];
		if(bx == 0)
			reached |= (1 << SIDE_LEFT);
		if(bx == N - 1)
			reached |= (1 << SIDE_RIGHT);
		if(by == 0)
			reached |= (1 << SIDE_BOTTOM);
		if(by == N - 1)
			reached |= (1 << SIDE_TOP);
		if(bz == 0)
			reached |= (1 << SIDE_FRONT);
		if(bz == N - 1)
			reached |= (1 << SIDE_BACK);
		for(int s = 0; s < 6; s++) {
			int nx = bx + off[s][0], ny = by + off[s][1], nz = bz + off[s][2];
			if(nx >= 0 && ny >= 0 && nz >= 0 && nx < N && ny < N && nz < N
			   && !visited[BLK_INDEX2(nx, ny, nz)]
			   && see_through[BLK_DATA(bd, nx, ny, nz).type]) {
				visited[BLK_INDEX2(nx, ny, nz)] = true;
				sx[sp] = nx;
				sy[sp] = ny;
				sz[sp] = nz;
				sp++;
			}
		}
	}
	for(int s = 0; s < 6; s++)
		if(reached & (1 << s))
			reachable[s] |= reached;
}

static void ref_init(struct bd_t* bd, uint8_t* reachable) {
	static bool visited[N * N * N];
	static int sx[N * N * N], sy[N * N * N], sz[N * N * N];
	memset(reachable, 0, 6);
	memset(visited, 0, sizeof(visited));
	for(int y = 0; y < N; y++)
		for(int x = 0; x < N; x++) {
			ref_test(bd, sx, sy, sz, visited, reachable, x, y, 0);
			ref_test(bd, sx, sy, sz, visited, reachable, x, 0, y);
			ref_test(bd, sx, sy, sz, visited, reachable, 0, x, y);
			ref_test(bd, sx, sy, sz, visited, reachable, x, y, N - 1);
			ref_test(bd, sx, sy, sz, visited, reachable, x, N - 1, y);
			ref_test(bd, sx, sy, sz, visited, reachable, N - 1, x, y);
		}
}

// ---- Optimized: packed-int stack (mirrors chunk_mesher.c). ----
static void opt_test(struct bd_t* bd, int* stk, bool* visited,
					 uint8_t* reachable, int x, int y, int z) {
	const int VZ = N, VY = N * N, BZ = P, BY = P * P;
	int seed = x | (z << 4) | (y << 8);
	int seed_bi = (x + 1) + (z + 1) * BZ + (y + 1) * BY;
	if(visited[seed] || !see_through[bd[seed_bi].type])
		return;
	int sp = 0;
	stk[sp++] = seed;
	visited[seed] = true;
	uint8_t reached = 0;
	while(sp > 0) {
		int i = stk[--sp];
		int cx = i & 15, cz = (i >> 4) & 15, cy = (i >> 8) & 15;
		int bi = (cx + 1) + (cz + 1) * BZ + (cy + 1) * BY;
		if(cx == 0)
			reached |= (1 << SIDE_LEFT);
		if(cx == N - 1)
			reached |= (1 << SIDE_RIGHT);
		if(cy == 0)
			reached |= (1 << SIDE_BOTTOM);
		if(cy == N - 1)
			reached |= (1 << SIDE_TOP);
		if(cz == 0)
			reached |= (1 << SIDE_FRONT);
		if(cz == N - 1)
			reached |= (1 << SIDE_BACK);
#define BFS_TRY(ok, ni, nbi)                                                   \
	do {                                                                       \
		if((ok) && !visited[ni] && see_through[bd[nbi].type]) {                 \
			visited[ni] = true;                                                \
			stk[sp++] = (ni);                                                  \
		}                                                                      \
	} while(0)
		BFS_TRY(cx > 0, i - 1, bi - 1);
		BFS_TRY(cx < N - 1, i + 1, bi + 1);
		BFS_TRY(cz > 0, i - VZ, bi - BZ);
		BFS_TRY(cz < N - 1, i + VZ, bi + BZ);
		BFS_TRY(cy > 0, i - VY, bi - BY);
		BFS_TRY(cy < N - 1, i + VY, bi + BY);
#undef BFS_TRY
	}
	for(int s = 0; s < 6; s++)
		if(reached & (1 << s))
			reachable[s] |= reached;
}

static void opt_init(struct bd_t* bd, uint8_t* reachable) {
	static bool visited[N * N * N];
	static int stk[N * N * N];
	memset(reachable, 0, 6);
	memset(visited, 0, sizeof(visited));
	for(int y = 0; y < N; y++)
		for(int x = 0; x < N; x++) {
			opt_test(bd, stk, visited, reachable, x, y, 0);
			opt_test(bd, stk, visited, reachable, x, 0, y);
			opt_test(bd, stk, visited, reachable, 0, x, y);
			opt_test(bd, stk, visited, reachable, x, y, N - 1);
			opt_test(bd, stk, visited, reachable, x, N - 1, y);
			opt_test(bd, stk, visited, reachable, N - 1, x, y);
		}
}

int main(void) {
	static struct bd_t bd[P * P * P];
	uint8_t r_ref[6], r_opt[6];

	for(int seed = 1; seed <= 40; seed++) {
		srand((unsigned)seed);
		// Vary see-through density across seeds: solid (low), mixed, airy.
		int thresh = (seed * 7) % 256;
		for(int t = 0; t < 256; t++)
			see_through[t] = (t < thresh) ? 1 : 0;
		// A couple of degenerate cases: all-air and all-solid.
		if(seed == 1)
			for(int t = 0; t < 256; t++)
				see_through[t] = 1; // all air -> worst-case flood
		if(seed == 2)
			for(int t = 0; t < 256; t++)
				see_through[t] = 0; // all solid

		for(int i = 0; i < P * P * P; i++)
			bd[i].type = (uint8_t)(rand() & 0xFF);

		ref_init(bd, r_ref);
		opt_init(bd, r_opt);
		assert(memcmp(r_ref, r_opt, 6) == 0);
	}

	return 0;
}
