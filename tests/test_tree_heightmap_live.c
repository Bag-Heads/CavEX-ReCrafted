/*
	laneC/TREE-HEIGHTMAP (2026-09-26): does the live server's tree-height read
	(server_world.c quad_height -> sc->heightmap) match Beta's?

	Beta 1.7.3 (decompile ../mcb173-retromcp/.../src_original):
	  ChunkProviderGenerate.populate: every tree attempt reads
	    y = world.getHeightValue(x, z) [World.java:603-614 ->
	    Chunk.getHeightValue:60-62 = heightMap[z<<4|x]].
	  heightMap is built by Chunk.generateHeightMap [:67-86] (top y whose block
	    y-1 has Block.lightOpacity != 0) and then MAINTAINED on every write:
	    Chunk.setBlockID[WithMetadata] [:236-273, :275-] calls func_1003_g
	    [:158-] (relight column) when an opacity!=0 block lands at/above the
	    height, or when an opacity-0 block replaces the top cell. That keeps the
	    invariant exact, so at any moment Beta's height == a fresh top-down scan
	    for the first lightOpacity!=0 block, over the CURRENT blocks -- which
	    includes every earlier write of the SAME populate (lakes, dungeons,
	    earlier trees' logs and leaves).
	  lightOpacity: Block.java:152 default isOpaqueCube()?255:0, overridden by
	    setLightOpacity (water 3, lava 255, leaves 1, web 1, ice 3, farmland /
	    stairs / step 255).

	The live server fills sc->heightmap at terrain time (worldgen_lighting) and
	only recomputes it AFTER a whole populate + snow pass
	(server_world_rebuild_lighting); quad_set never touches it, so quad_height
	is stale inside a populate. This test replays that exact sequence over real
	terrain with the hook returning the stale value (as quad_height does) and
	pins the result to the world populated with this file's independent Beta
	scan. RED on 64d7941f (worldgen_trees read the hook): 3 of 4 regions off.
	The hook-read counters are diagnostic only (0 once trees stop using it).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"
#include "worldgen/biome.h"
#include "worldgen/decorate.h"
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"

// Beta Block.lightOpacity[id] == 0, for every id Beta 1.7.3 registers
// (Block.java:152 isOpaqueCube()?255:0 with the setLightOpacity overrides).
// isOpaqueCube()==false overrides (hence opacity 0 unless re-set): sapling,
// glass (BlockBreakable), bed, rails, piston base/ext/moving, plants (tall
// grass, dead bush, flowers, mushrooms via BlockFlower), torches, fire,
// spawner, wire, crops, sign, doors, ladder, lever, plates, button, snow
// layer, cactus, reed, fence, portal, cake, repeaters, trapdoor.
static bool beta_opacity_zero(uint8_t id) {
	switch(id) {
		case 0: case 6: case 20: case 26: case 27: case 28: case 29: case 31:
		case 32: case 33: case 34: case 36: case 37: case 38: case 39: case 40:
		case 50: case 51: case 52: case 55: case 59: case 63: case 64: case 65:
		case 66: case 68: case 69: case 70: case 71: case 72: case 75: case 76:
		case 77: case 78: case 81: case 83: case 85: case 90: case 92: case 93:
		case 94: case 96:
			return true;
		default: return false;
	}
}

#define R 8
#define IDX(x, y, z)                                                           \
	((size_t)(y) + ((size_t)(z) + (size_t)(x) * CHUNK_SIZE) * WORLD_HEIGHT)

static struct server_chunk g_ch[R][R];
static int g_cx0, g_cz0;
static int g_px, g_pz;
static bool g_read_beta; // trees read the Beta-live height instead of stale

struct stats {
	long reads, stale_ne_beta, stale_ne_cwlive, cwlive_ne_beta;
	long stale_below, stale_above; // stale vs beta direction
};
static struct stats g_st;
static int g_first_printed;

static struct server_chunk* reg_chunk(int x, int z, int* lx, int* lz) {
	int cx = (x >> 4) - g_cx0, cz = (z >> 4) - g_cz0;
	if(cx < 0 || cz < 0 || cx >= R || cz >= R)
		return NULL;
	*lx = x & 15;
	*lz = z & 15;
	return &g_ch[cx][cz];
}
static uint8_t reg_get(void* ctx, int x, int y, int z) {
	(void)ctx;
	int lx, lz;
	if(y < 0 || y >= WORLD_HEIGHT)
		return BLOCK_AIR;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	return s ? s->ids[IDX(lx, y, lz)] : BLOCK_AIR;
}
static uint8_t reg_get_meta(void* ctx, int x, int y, int z) {
	(void)ctx;
	int lx, lz;
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	return s ? nibble_read(s->metadata, IDX(lx, y, lz)) : 0;
}
static int reg_skylight(void* ctx, int x, int y, int z) {
	(void)ctx;
	int lx, lz;
	if(y >= WORLD_HEIGHT)
		return 15;
	if(y < 0)
		return 0;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	return s ? nibble_read(s->lighting_sky, IDX(lx, y, lz)) : 0;
}
static void reg_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t m);
static struct wgen_world g_view;

// Mirror of server_world.c quad_set (raw write + leaf removal flag pass).
static void reg_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t m) {
	(void)ctx;
	int lx, lz;
	if(y < 0 || y >= WORLD_HEIGHT)
		return;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	if(!s)
		return;
	uint8_t old = s->ids[IDX(lx, y, lz)];
	s->ids[IDX(lx, y, lz)] = id;
	nibble_write(s->metadata, IDX(lx, y, lz), m);
	if(old == BLOCK_LEAVES && id != BLOCK_LEAVES)
		wgen_leaves_on_removal(&g_view, g_px, g_pz, x, y, z);
}

static int scan_beta(struct server_chunk* s, int lx, int lz) {
	int y = WORLD_HEIGHT - 1; // generateHeightMap: var4 = 127, scan var4-1
	while(y > 0 && beta_opacity_zero(s->ids[IDX(lx, y - 1, lz)]))
		y--;
	return y;
}
// worldgen_lighting's predicate (worldgen.c worldgen_is_solid_surface) on the
// CURRENT blocks: separates definition drift from staleness.
static int scan_cw(struct server_chunk* s, int lx, int lz) {
	for(int y = WORLD_HEIGHT - 1; y >= 0; y--) {
		uint8_t t = s->ids[IDX(lx, y, lz)];
		if(blocks[t] && (!blocks[t]->can_see_through || blocks[t]->opacity > 0))
			return y + 1;
	}
	return 0;
}

static int reg_height(void* ctx, int x, int z) {
	(void)ctx;
	int lx, lz;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	assert(s);
	int stale = s->heightmap[lx + lz * CHUNK_SIZE]; // == quad_height
	int beta = scan_beta(s, lx, lz);
	int cw = scan_cw(s, lx, lz);
	g_st.reads++;
	if(stale != beta) {
		g_st.stale_ne_beta++;
		if(stale < beta)
			g_st.stale_below++;
		else
			g_st.stale_above++;
		if(g_first_printed < 6 && !g_read_beta) {
			g_first_printed++;
			printf("    stale read: chunk populate (%d,%d) column (%d,%d) "
				   "server y=%d beta y=%d (top id at beta-1: %d)\n",
				   g_px, g_pz, x, z, stale, beta,
				   beta > 0 ? s->ids[IDX(lx, beta - 1, lz)] : -1);
		}
	}
	if(stale != cw)
		g_st.stale_ne_cwlive++;
	if(cw != beta)
		g_st.cwlive_ne_beta++;
	return g_read_beta ? beta : stale;
}

static uint64_t g_hash;
static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
	for(size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

static bool run_region(struct worldgen* wg, int64_t seed, int cx0, int cz0,
					   uint8_t* out_ids) {
	g_cx0 = cx0;
	g_cz0 = cz0;
	g_hash = 0xcbf29ce484222325ULL;
	for(int i = 0; i < R; i++)
		for(int j = 0; j < R; j++)
			if(!worldgen_generate_chunk(wg, &g_ch[i][j], cx0 + i, cz0 + j))
				return false;
	memset(&g_view, 0, sizeof g_view);
	g_view.get = reg_get;
	g_view.set = reg_set;
	g_view.height = reg_height;
	g_view.skylight = reg_skylight;
	g_view.get_meta = reg_get_meta;
	for(int i = 0; i < R - 1; i++)
		for(int j = 0; j < R - 1; j++) {
			g_px = cx0 + i;
			g_pz = cz0 + j;
			enum biome b
				= worldgen_biome_at(wg, g_px * 16 + 16, g_pz * 16 + 16);
			double td = worldgen_tree_density(wg, g_px * 16, g_pz * 16);
			worldgen_populate(seed, g_px, g_pz, &g_view, b, td);
			double temp[256];
			worldgen_temperature_grid(wg, g_px * 16 + 8, g_pz * 16 + 8, temp);
			worldgen_snow(&g_view, temp, g_px * 16, g_pz * 16);
			// server_world.c: post-populate server_world_rebuild_lighting on
			// the 2x2 quadrant == worldgen_lighting(sc, false, dim).
			for(int dx = 0; dx < 2; dx++)
				for(int dz = 0; dz < 2; dz++)
					worldgen_lighting(&g_ch[i + dx][j + dz], false,
									  WORLD_DIM_OVERWORLD);
		}
	size_t per = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	FILE* dump = NULL;
	const char* path = getenv("CW_TREE_DUMP");
	if(path) {
		dump = fopen(path, "ab");
		assert(dump);
	}
	for(int i = 0; i < R; i++)
		for(int j = 0; j < R; j++) {
			memcpy(out_ids + ((size_t)i * R + j) * per, g_ch[i][j].ids, per);
			g_hash = fnv(g_hash, g_ch[i][j].ids, per);
			g_hash = fnv(g_hash, g_ch[i][j].metadata, per / 2);
			if(dump) {
				assert(fwrite(g_ch[i][j].ids, per, 1, dump) == 1);
				assert(fwrite(g_ch[i][j].metadata, per / 2, 1, dump) == 1);
			}
			server_chunk_arrays_free(&g_ch[i][j]);
		}
	if(dump) assert(fclose(dump) == 0);
	return true;
}

static long count_ids(const uint8_t* a, size_t n, uint8_t id) {
	long c = 0;
	for(size_t i = 0; i < n; i++)
		c += a[i] == id;
	return c;
}

int main(void) {
	blocks_init();
	/* Forest-heavy regions (tree height reads 90 / 357 / 337 / 269). `beta`
	 * is the FNV-1a hash (ids then metadata, per chunk) of the region
	 * populated with trees fed THIS FILE's independent Beta scan (scan_beta)
	 * through the height() hook, captured on main 64d7941f BEFORE the fix,
	 * when worldgen_trees still read the hook. The server run feeds the hook
	 * the stale terrain heightmap (quad_height) and must now hash the same.
	 * Pre-fix the server run hashed 0xf954d6a7d96e0ebc (== beta: its 10
	 * stale reads all failed either way), 0x63610106ad42b01a,
	 * 0x94b584f4a0ff61db, 0xc1966352b26c4b36 -- 8617 / 3030 / 1710 cells
	 * off, with extra trees rooted on canopies' ground (logs 2194 vs 1925,
	 * 1242 vs 1095, 720 vs 612). */
	const struct {
		int64_t seed;
		int cx0, cz0;
		uint64_t beta;
	} regions[] = {
		/* L3 FAST re-pin: 11d73857e and c2f7c0ce1 reproduce the old
		 * four pins (f954d6a7d96e0ebc, 2ef0923dfe4a5bc6,
		 * 9637e029b284d24b, 2d5c235af44961af). The first changed
		 * ID byte, region0 offset259529, is reproduced by WIP356da47fe's
		 * density-axis correction ALONE (CFR NoiseGeneratorPerlin:91-92;
		 * independent original-JAR doubles in history_case.h). HEAD8/8
		 * full ID/meta captures match; both height modes still match
		 * every cell. D:/cavewii-evidence/l3/tree-{P-raw,axis-only}.bin
		 * and tree-raw-repeats.json retain the full evidence. */
		{12345, 12, 0, 0x2840a151bbaae371ULL},
		{12345, 20, 20, 0x425853c77cfd4fdcULL},
		/* Seed 42 pins re-derived at G4 assembly 2026-09-26: reed a3c02638
		 * (no natural cane on sand) removes reed cells in both seed-42
		 * regions, so the 64d7941f pins (0xca967aa37af6f4da,
		 * 0x86c7ee9092dd2f8c) describe a world without the reed fix. The new
		 * values were measured INDEPENDENTLY of this fix: the pre-fix tree
		 * code (tree.c/populate.h at 2eb72ae0, still reading the height()
		 * hook) with the reed fix, beta-hook run = 0x9637e029b284d24b /
		 * 0x2d5c235af44961af; the fixed server run hashes the same, 0 cells
		 * differ. With decorate.c reverted the old pins still pass. */
		{42, -4, -4, 0x373e5ca7bc16aee3ULL},
		{42, -5, -2, 0x74ab8f9c6769f2c6ULL},
	};
	size_t per = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	size_t n = per * R * R;
	uint8_t* a = malloc(n);
	uint8_t* b = malloc(n);
	assert(a && b);
	int failures = 0;
	for(size_t r = 0; r < sizeof regions / sizeof regions[0]; r++) {
		struct worldgen* wg
			= worldgen_create(regions[r].seed, WORLD_DIM_OVERWORLD);
		assert(wg);
		printf("region seed=%lld origin (%d,%d):\n",
			   (long long)regions[r].seed, regions[r].cx0, regions[r].cz0);
		memset(&g_st, 0, sizeof g_st);
		g_first_printed = 0;
		g_read_beta = false;
		assert(run_region(wg, regions[r].seed, regions[r].cx0, regions[r].cz0,
						  a));
		struct stats server = g_st;
		uint64_t hash_server = g_hash;
		memset(&g_st, 0, sizeof g_st);
		g_read_beta = true;
		assert(run_region(wg, regions[r].seed, regions[r].cx0, regions[r].cz0,
						  b));
		uint64_t hash_beta = g_hash;
		long diff = 0;
		for(size_t i = 0; i < n; i++)
			diff += a[i] != b[i];
		printf("  hook reads %ld (%ld stale); server hash 0x%016llx, "
			   "beta-hook hash 0x%016llx, pinned beta 0x%016llx; %ld cells "
			   "differ; logs %ld leaves %ld\n",
			   server.reads, server.stale_ne_beta,
			   (unsigned long long)hash_server, (unsigned long long)hash_beta,
			   (unsigned long long)regions[r].beta, diff,
			   count_ids(a, n, BLOCK_LOG), count_ids(a, n, BLOCK_LEAVES));
		if(hash_server != regions[r].beta || hash_beta != regions[r].beta
		   || diff != 0) {
			printf("  FAIL: trees did not read Beta's live getHeightValue\n");
			failures++;
		}
		worldgen_destroy(wg);
	}
	free(a);
	free(b);
	if(failures) {
		printf("FAIL: %d region(s) diverge from the Beta height rule\n",
			   failures);
		return 1;
	}
	printf("OK: tree heights follow Beta's per-write heightMap in every "
		   "region\n");
	return 0;
}
