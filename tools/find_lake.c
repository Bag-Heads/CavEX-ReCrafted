/*
	Offline lake finder: runs the byte-exact Beta terrain + populate (water/lava
	lakes included) over a grid of chunk quadrants and reports the biggest
	EXPOSED surface water pond that sits ABOVE sea level -- i.e. a populate lake,
	not the ocean (Beta oceans are also water_still, but their surface is at
	y<=63; WATER_LEVEL is 64). Also returns a shore spawn standing on land at the
	pond's edge. Generation is deterministic, so the Wii reproduces the same
	pond and these coordinates are valid in-game.

	Usage: find_lake <seed> [radius_chunks] [min_surface_y]
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks_data.h"
#include "network/server_world.h" // struct server_chunk
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"

// server_chunk block index: y + (lz + lx*16)*128, local lx/lz in [0,16).
#define LIDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * 16) * 128)

// Stubbed externals worldgen.c/populate.c link against (same as find_spawn /
// test_worldgen). blocks[] NULL -> the sky-light pass leaves skylight at 15,
// which is irrelevant to where WATER is placed.
struct block;
struct block* blocks[256] = {0};
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

// A wgen_world view over the 2x2 chunks of a single populate quadrant.
struct qctx {
	int cx, cz;
	struct server_chunk* c[2][2];
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
static void qset(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	if(y < 0 || y >= 128)
		return;
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	if(sc) {
		sc->ids[LIDX(lx, y, lz)] = id;
		nibble_write(sc->metadata, LIDX(lx, y, lz), meta);
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
	return y < 0 ? 0 : 15; // blocks[]=NULL -> lighting is uniformly lit
}

static int is_land(uint8_t b) {
	return b == BLOCK_STONE || b == BLOCK_GRASS || b == BLOCK_DIRT
		|| b == BLOCK_SAND || b == BLOCK_GRAVEL;
}

// Top (highest non-air) block of a column, via the quad view.
static int column_top(struct qctx* q, int wx, int wz, uint8_t* out_id) {
	int lx, lz;
	struct server_chunk* sc = qpick(q, wx, wz, &lx, &lz);
	if(!sc)
		return -1;
	for(int y = 127; y >= 0; y--) {
		uint8_t b = sc->ids[LIDX(lx, y, lz)];
		if(b != BLOCK_AIR) {
			*out_id = b;
			return y;
		}
	}
	return -1;
}

int main(int argc, char** argv) {
	int64_t seed = (argc > 1) ? strtoll(argv[1], NULL, 10) : 1;
	int R = (argc > 2) ? atoi(argv[2]) : 10;
	int MIN_Y = (argc > 3) ? atoi(argv[3]) : 66; // above sea level => a lake

	struct worldgen* wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
	if(!wg) {
		printf("worldgen_create failed\n");
		return 1;
	}

	int best_count = 0, best_wx = 0, best_wy = 0, best_wz = 0;
	int best_sx = 0, best_sy = 0, best_sz = 0;

	for(int cx = -R; cx <= R; cx++) {
		for(int cz = -R; cz <= R; cz++) {
			struct qctx q = {cx, cz, {{0}}};
			struct server_chunk sc[2][2];
			memset(sc, 0, sizeof(sc));
			int ok = 1;
			for(int dx = 0; dx < 2; dx++)
				for(int dz = 0; dz < 2; dz++) {
					if(worldgen_generate_chunk(wg, &sc[dx][dz], cx + dx, cz + dz))
						q.c[dx][dz] = &sc[dx][dz];
					else
						ok = 0;
				}

			if(ok) {
				struct wgen_world view = {&q, qget, qset, qheight, qsky};
				// populate now needs the biome at the +16,+16 corner and the
				// tree-density noise at the origin (same as server_world.c).
				enum biome corner_biome
					= worldgen_biome_at(wg, cx * 16 + 16, cz * 16 + 16);
				double tree_density
					= worldgen_tree_density(wg, cx * 16, cz * 16);
				worldgen_populate(seed, cx, cz, &view, corner_biome,
								  tree_density);

				// Count exposed water columns whose surface is above sea level.
				int count = 0;
				long sumx = 0, sumz = 0, sumy = 0;
				for(int dx = 0; dx < 2; dx++)
					for(int dz = 0; dz < 2; dz++)
						for(int lx = 0; lx < 16; lx++)
							for(int lz = 0; lz < 16; lz++) {
								int wx = (cx + dx) * 16 + lx;
								int wz = (cz + dz) * 16 + lz;
								uint8_t topb;
								int top = column_top(&q, wx, wz, &topb);
								if(top >= MIN_Y && topb == BLOCK_WATER_STILL) {
									count++;
									sumx += wx;
									sumz += wz;
									sumy += top;
								}
							}

				if(count > best_count) {
					int wx = (int)(sumx / count);
					int wz = (int)(sumz / count);
					int wy = (int)(sumy / count);
					// Find a land shore column near the pond centre.
					int sh_x = 0, sh_y = -1, sh_z = 0;
					for(int rad = 1; rad <= 10 && sh_y < 0; rad++)
						for(int ddx = -rad; ddx <= rad && sh_y < 0; ddx++)
							for(int ddz = -rad; ddz <= rad && sh_y < 0; ddz++) {
								uint8_t tb;
								int t = column_top(&q, wx + ddx, wz + ddz, &tb);
								if(t >= wy - 1 && t <= wy + 4 && is_land(tb)) {
									sh_x = wx + ddx;
									sh_y = t + 1;
									sh_z = wz + ddz;
								}
							}
					if(sh_y >= 0) { // only accept ponds with a real shore
						best_count = count;
						best_wx = wx;
						best_wy = wy;
						best_wz = wz;
						best_sx = sh_x;
						best_sy = sh_y;
						best_sz = sh_z;
					}
				}
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
	}

	if(best_count > 0)
		printf("seed=%lld R=%d min_y=%d : pond=%d cols  water_surface=(%d,%d,%d)"
			   "  shore_spawn=(%d,%d,%d)\n",
			   (long long)seed, R, MIN_Y, best_count, best_wx, best_wy, best_wz,
			   best_sx, best_sy, best_sz);
	else
		printf("seed=%lld R=%d min_y=%d : no above-sea-level pond with a shore "
			   "found (try a larger radius or lower min_y)\n",
			   (long long)seed, R, MIN_Y);

	worldgen_destroy(wg);
	return 0;
}
