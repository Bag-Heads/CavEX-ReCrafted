/*
	Microbenchmark for the Beta 1.7.3 worldgen phases, per 2x2 populate quadrant:
	  - terrain : worldgen_generate_chunk x4 (noise + surface + caves + initial
	              skylight, incl. the chunk allocations)
	  - populate: worldgen_populate (trees/big trees/flowers/springs/cacti) + the
	              RNG-free snow pass
	  - relight : worldgen_lighting(reset_torch=false) x4 -- the post-populate
	              heightmap+skylight recompute added by the relight fix

	This isolates the cost the relight added to the populate cascade (server_world
	runs four of these quadrants per ensure_populated, gated by the gen throttle),
	so we can judge whether the per-quadrant relight redundancy is worth deduping.
	Always exits 0; read stdout.

	Caveat: like find_spawn/find_lake this links the worldgen with blocks[]=NULL
	(no block table), so worldgen_lighting iterates every cell (the real cost) but
	skips the per-block opacity attenuation -- it slightly UNDER-counts relight,
	and the server-side block-light flood (lava springs) is not included here.
	Build with -O3 to mirror the real target (the CMake target sets it).
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "block/blocks.h"		 // struct block (opacity/can_see_through/luminance)
#include "block/blocks_data.h"
#include "network/server_world.h" // struct server_chunk
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"

#define LIDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * 16) * 128)

// MINIMAL real block table. The worldgen reads block opacity/can_see_through
// (heightmap + skylight) and luminance (lava block light); with blocks[]=NULL
// (as find_lake stubs) every heightmap stays 0, so populate's trees read height
// 0 and fail to place -- UNDER-measuring the expensive part. Give the terrain +
// tree blocks real light properties so trees actually generate and the relight
// does real work.
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_water = {.can_see_through = true, .opacity = 3};
static struct block b_lava
	= {.can_see_through = false, .opacity = 15, .luminance = 15};
struct block* blocks[256] = {0};
static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_GRASS] = blocks[BLOCK_DIRT]
		= blocks[BLOCK_COBBLESTONE] = blocks[BLOCK_BEDROCK] = blocks[BLOCK_SAND]
		= blocks[BLOCK_GRAVEL] = blocks[BLOCK_LOG] = blocks[BLOCK_SANDSTONE]
		= blocks[BLOCK_CLAY] = &b_solid;
	blocks[BLOCK_WATER_FLOW] = blocks[BLOCK_WATER_STILL] = blocks[BLOCK_ICE]
		= &b_water;
	blocks[BLOCK_LAVA_FLOW] = blocks[BLOCK_LAVA_STILL] = &b_lava;
	blocks[BLOCK_LEAVES] = &b_leaves;
}
uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}

struct qctx {
	int cx, cz;
	struct server_chunk* c[2][2];
	struct wgen_world* view; /* for wgen_leaves_on_removal (quad_set mirror) */
};
static struct server_chunk* qpick(struct qctx* q, int x, int z, int* lx,
								  int* lz) {
	int gx = x - q->cx * 16, gz = z - q->cz * 16;
	if(gx < 0 || gx >= 32 || gz < 0 || gz >= 32)
		return NULL;
	*lx = gx & 15;
	*lz = gz & 15;
	return q->c[gx >> 4][gz >> 4];
}
static uint8_t qget(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= 128)
		return 0;
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	return sc ? sc->ids[LIDX(lx, y, lz)] : 0;
}
static uint8_t qget_meta(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= 128)
		return 0;
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	return sc ? nibble_read(sc->metadata, LIDX(lx, y, lz)) : 0;
}
static void qset(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	if(y < 0 || y >= 128)
		return;
	int lx, lz;
	struct qctx* q = ctx;
	struct server_chunk* sc = qpick(q, x, z, &lx, &lz);
	if(sc) {
		uint8_t old = sc->ids[LIDX(lx, y, lz)];
		sc->ids[LIDX(lx, y, lz)] = id;
		nibble_write(sc->metadata, LIDX(lx, y, lz), meta);
		/* mirror of server_world.c quad_set: the populate-time leaf flag pass */
		if(old == 18 && id != 18 && q->view)
			wgen_leaves_on_removal(q->view, q->cx, q->cz, x, y, z);
	}
}
static int qheight(void* ctx, int x, int z) {
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	return sc ? sc->heightmap[lx + lz * 16] : 0;
}
static int qsky(void* ctx, int x, int y, int z) {
	(void)ctx;
	(void)x;
	(void)z;
	return y < 0 ? 0 : 15;
}

#define NQUAD 96

int main(void) {
	const int64_t seed = 1;
	setup_blocks();
	struct worldgen* wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
	if(!wg) {
		printf("worldgen_create failed\n");
		return 0;
	}

	double terrain = 0, populate = 0, relight = 0;
	int counted = 0;
	long leaves = 0; // sanity: did trees actually generate?

	for(int i = 0; i < NQUAD; i++) {
		int cx = i * 3, cz = (i * 7) % 64; // spread coords -> varied terrain
		struct qctx q = {cx, cz, {{0}}, NULL};
		struct server_chunk sc[2][2];
		memset(sc, 0, sizeof(sc));

		clock_t t0 = clock();
		int ok = 1;
		for(int dx = 0; dx < 2; dx++)
			for(int dz = 0; dz < 2; dz++) {
				if(worldgen_generate_chunk(wg, &sc[dx][dz], cx + dx, cz + dz))
					q.c[dx][dz] = &sc[dx][dz];
				else
					ok = 0;
			}
		clock_t t1 = clock();

		if(ok) {
			struct wgen_world view = {&q, qget, qset, qheight, qsky};
			view.get_meta = qget_meta;
			q.view = &view;
			enum biome cb = worldgen_biome_at(wg, cx * 16 + 16, cz * 16 + 16);
			double td = worldgen_tree_density(wg, cx * 16, cz * 16);
			worldgen_populate(seed, cx, cz, &view, cb, td);
			clock_t t2 = clock();

			for(int dx = 0; dx < 2; dx++)
				for(int dz = 0; dz < 2; dz++)
					worldgen_lighting(&sc[dx][dz], false,
								  WORLD_DIM_OVERWORLD);
			clock_t t3 = clock();

			terrain += (double)(t1 - t0);
			populate += (double)(t2 - t1);
			relight += (double)(t3 - t2);
			counted++;
			for(int dx2 = 0; dx2 < 2; dx2++)
				for(int dz2 = 0; dz2 < 2; dz2++)
					for(size_t k = 0; k < (size_t)16 * 16 * 128; k++)
						if(sc[dx2][dz2].ids[k] == BLOCK_LEAVES)
							leaves++;
		}

		for(int dx = 0; dx < 2; dx++)
			for(int dz = 0; dz < 2; dz++)
				if(sc[dx][dz].ids) {
					free(sc[dx][dz].ids);
					free(sc[dx][dz].metadata);
					free(sc[dx][dz].lighting_sky);
					free(sc[dx][dz].lighting_torch);
					free(sc[dx][dz].heightmap);
				}
	}

	worldgen_destroy(wg);

	double k = 1000.0 / CLOCKS_PER_SEC / (counted ? counted : 1);
	double tt = terrain * k, tp = populate * k, tr = relight * k;
	printf("bench_worldgen: %d quadrants (2x2 chunks each), x86 -O3\n", counted);
	printf("  terrain  (gen x4)            : %.3f ms/quadrant\n", tt);
	printf("  populate (trees+decor+snow)  : %.3f ms/quadrant\n", tp);
	printf("  relight  (worldgen_lighting x4): %.3f ms/quadrant  (+%.0f%% over "
		   "terrain+populate)\n",
		   tr, (tt + tp) > 0 ? tr / (tt + tp) * 100.0 : 0.0);
	printf("  full quadrant                : %.3f ms\n", tt + tp + tr);
	printf("  trees generated: %.1f leaves/quadrant (real block table -> valid "
		   "heightmaps, so populate's trees actually place)\n",
		   counted ? (double)leaves / counted : 0.0);
	printf("  NOTE: server-side lava block-light flood is not timed here.\n");

	// Terrain hotspot breakdown (WORLDGEN_PROFILE). ph[] is cumulative ms over all
	// NQUAD*4 chunk gens; report ms/quadrant + share of the terrain bucket.
	double ph[6];
	worldgen_phase_ms(ph);
	double phtot = 0;
	for(int i = 0; i < 6; i++)
		phtot += ph[i];
	const char* pn[6]
		= {"biome", "density", "fill", "surface", "cave", "gen-light"};
	printf("  --- terrain sub-phases (ms/quadrant | %% of terrain gen) ---\n");
	for(int i = 0; i < 6; i++)
		printf("    %-9s : %.3f ms | %4.1f%%\n", pn[i], ph[i] / NQUAD,
			   phtot > 0 ? ph[i] / phtot * 100.0 : 0.0);
	return 0;
}
