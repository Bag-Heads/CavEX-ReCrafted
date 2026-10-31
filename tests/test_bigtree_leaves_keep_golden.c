/*
	Golden: a big oak's leaf discs never rewrite an existing leaf
	(source/worldgen/tree.c bt_circular_layer).

	Beta 1.7.3 (decompile 740c583), derived by hand -- never from CaveWii:
	  WorldGenBigTree.func_523_a (the leaf disc) [WorldGenBigTree.java:114-118]:
	    a cell that is neither air (0) nor leaves (18) is skipped;
	     otherwise it is set to the disc block.
	  World.setBlock -> Chunk.setBlockID [Chunk.java:277-281]: an unchanged id
	    returns false -- an unchanged id writes NOTHING: no metadata reset, no
	    onBlockRemoval, no lighting. So a big oak whose canopy discs cross an
	    existing birch canopy leaves those leaves birch (meta 2), and on the
	    live-world growth path (BlockSapling.growTree -> the same generator over
	    World) no leaf-over-leaf write happens, so no BlockLeaves.onBlockRemoval
	    fires for it -- the only removal flags are the ones wood-over-leaf writes
	    produce (trunk + branches over canopy).
	  The old CaveWii disc wrote set(LEAVES, 0) over leaves: species reset in
	  populate, and on the runtime path a same-id metadata change that the
	  set-block lifecycle treats as a removal -> extra flags around every
	  overlapped birch leaf.

	Fixture: the same birch (WorldGenForest via sapling meta 2) then the same
	oak sapling (meta 0; seeds scanned until BlockSapling.growTree's 1-in-10
	upgrade yields a WorldGenBigTree) grown from identical java_random streams
	on TWO worlds: (a) a flat array world whose set() mirrors the populate quad
	writer (wgen_leaves_on_removal on a non-leaf id over a leaf), and (b) the
	live superflat server world through server_world_set_block (the runtime
	rt_set path: full lifecycle, block_leaves.c onBlockRemoval). Pins: block
	ids AND metadata identical cell for cell between the two worlds, every
	surviving birch leaf keeps species bits 2, the java_random seed is
	identical after both growths, and at least one birch leaf lies inside the
	oak's canopy so the overlap is real. Block ids and the Random stream are
	unchanged by the fix (same id either way, no draw at the disc write), so
	test_oracle_worldgen stays byte-exact.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"
#include "worldgen/tree.h"

/* ---------------- (a) flat array world, populate-writer semantics ---------------- */
#define FW_W 32
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
};
static struct wgen_world* g_fw_view;
static int fw_index(int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128 || x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return 0;
	*idx = (x * FW_W + z) * 128 + y;
	return 1;
}
static uint8_t fw_get(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->ids[idx] : BLOCK_AIR;
}
static uint8_t fw_get_meta(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->meta[idx] : 0;
}
static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(!fw_index(x, y, z, &idx))
		return;
	uint8_t old = f->ids[idx];
	f->ids[idx] = id;
	f->meta[idx] = meta;
	if(old == BLOCK_LEAVES && id != BLOCK_LEAVES) /* quad_set mirror */
		wgen_leaves_on_removal(g_fw_view, 0, 0, x, y, z);
}

/* ---------------- (b) live server world, runtime lifecycle semantics ---------------- */
static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
/* tree_runtime.c rt_get/rt_set: out-of-world reads as air, writes go through
 * server_world_set_block (the full lifecycle, incl. block_leaves.c's
 * onBlockRemoval on a leaf being replaced). */
static uint8_t sv_get(void* ctx, int x, int y, int z) {
	(void)ctx;
	struct block_data b;
	if(!server_world_get_block(&s.world, x, y, z, &b))
		return BLOCK_AIR;
	return b.type;
}
static void sv_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	(void)ctx;
	server_world_set_block(&s.worlds[0], x, y, z,
						   (struct block_data) {.type = id, .metadata = meta});
	clin_drain_discard();
}
#define GROUND 64 /* grass top on both worlds; dirt at 63 */
static void reset_worlds(struct flatworld* f) {
	memset(f, 0, sizeof *f);
	for(int x = 0; x < FW_W; x++)
		for(int z = 0; z < FW_W; z++) {
			f->ids[(x * FW_W + z) * 128 + GROUND - 1] = BLOCK_DIRT;
			f->ids[(x * FW_W + z) * 128 + GROUND] = BLOCK_GRASS;
			for(int y = GROUND - 1; y < 110; y++) {
				uint8_t id = y == GROUND - 1 ? BLOCK_DIRT : y == GROUND ? BLOCK_GRASS : BLOCK_AIR;
				server_world_set_block_silent(&s.worlds[0], x, y, z, (struct block_data) {.type = id});
			}
			clin_drain_discard();
		}
}

static bool is_big_oak(struct flatworld* f, int tx, int tz, int bx, int bz) {
	/* a big oak has logs off the trunk column (branches); the birch's own
	 * trunk column is not one of them */
	for(int x = tx - 6; x <= tx + 6; x++)
		for(int z = tz - 6; z <= tz + 6; z++) {
			if((x == tx && z == tz) || (x == bx && z == bz))
				continue;
			for(int y = GROUND + 1; y < GROUND + 30; y++)
				if(fw_get(f, x, y, z) == BLOCK_LOG)
					return true;
		}
	return false;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_bigtree_leaves_keep_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x9E3779B9u;
	dict_entity_init(s.entities);
	s.player.x = 1000.0;
	s.player.y = 1000.0;
	s.player.z = 1000.0;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	struct flatworld* f = malloc(sizeof *f);
	struct flatworld* birch_only = malloc(sizeof *birch_only);
	assert(f && birch_only);
	struct wgen_world wf = {.ctx = f, .get = fw_get, .set = fw_set, .get_meta = fw_get_meta};
	struct wgen_world ws = {.ctx = NULL, .get = sv_get, .set = sv_set};
	g_fw_view = &wf;
	/* trunks 4 apart: inside the big oak's reach, outside its trunk checks */
	const int BX = 14, BZ = 16, OX = 18, OZ = 16;

	int verified = 0, overlap_total = 0, flagged_total = 0;
	for(int64_t seed = 1; seed < 3000 && verified < 3; seed++) {
		reset_worlds(f);
		struct java_random rf, rs;
		jrand_set_seed(&rf, seed);
		rs = rf;
		fw_set(f, BX, GROUND + 1, BZ, BLOCK_SAPLING, 2);
		sv_set(NULL, BX, GROUND + 1, BZ, BLOCK_SAPLING, 2);
		bool bf = worldgen_grow_sapling(&wf, &rf, 2, BX, GROUND + 1, BZ);
		bool bs = worldgen_grow_sapling(&ws, &rs, 2, BX, GROUND + 1, BZ);
		assert(bf == bs && rf.seed == rs.seed);
		if(!bf)
			continue;
		memcpy(birch_only, f, sizeof *f);
		fw_set(f, OX, GROUND + 1, OZ, BLOCK_SAPLING, 0);
		sv_set(NULL, OX, GROUND + 1, OZ, BLOCK_SAPLING, 0);
		bool of = worldgen_grow_sapling(&wf, &rf, 0, OX, GROUND + 1, OZ);
		bool os = worldgen_grow_sapling(&ws, &rs, 0, OX, GROUND + 1, OZ);
		assert(of == os && rf.seed == rs.seed && "identical Random consumption on both paths");
		if(!of || !is_big_oak(f, OX, OZ, BX, BZ))
			continue;
		/* the TRUE overlap: re-grow the same oak (same Random) on a copy of the
		 * birch world whose birch leaves are air -- WorldGenBigTree treats air
		 * and leaves alike in every clearance check (:114-118, func_524_c), so
		 * the geometry is identical and the cells this run writes as LEAVES
		 * where a birch leaf stood are exactly the disc cells the old
		 * unconditional set(LEAVES, 0) rewrote. */
		int overlap = 0, flagged = 0;
		{
			struct flatworld* airy = malloc(sizeof *airy);
			assert(airy);
			memcpy(airy, birch_only, sizeof *airy);
			for(int i = 0; i < FW_W * FW_W * 128; i++)
				if(airy->ids[i] == BLOCK_LEAVES)
					airy->ids[i] = BLOCK_AIR;
			struct wgen_world wa = {.ctx = airy, .get = fw_get, .set = fw_set, .get_meta = fw_get_meta};
			struct wgen_world* saved = g_fw_view;
			g_fw_view = &wa;
			struct java_random ra;
			jrand_set_seed(&ra, seed);
			{ /* replay the birch draws so the oak starts from the same stream */
				struct flatworld* scratch = malloc(sizeof *scratch);
				assert(scratch);
				memset(scratch, 0, sizeof *scratch);
				for(int x = 0; x < FW_W; x++)
					for(int z = 0; z < FW_W; z++) {
						scratch->ids[(x * FW_W + z) * 128 + GROUND - 1] = BLOCK_DIRT;
						scratch->ids[(x * FW_W + z) * 128 + GROUND] = BLOCK_GRASS;
					}
				struct wgen_world wsc = {.ctx = scratch, .get = fw_get, .set = fw_set, .get_meta = fw_get_meta};
				g_fw_view = &wsc;
				fw_set(scratch, BX, GROUND + 1, BZ, BLOCK_SAPLING, 2);
				(void)worldgen_grow_sapling(&wsc, &ra, 2, BX, GROUND + 1, BZ);
				free(scratch);
				g_fw_view = &wa;
			}
			fw_set(airy, OX, GROUND + 1, OZ, BLOCK_SAPLING, 0);
			bool oa = worldgen_grow_sapling(&wa, &ra, 0, OX, GROUND + 1, OZ);
			assert(oa && ra.seed == rf.seed && "airified replay consumes the same Random");
			for(int i = 0; i < FW_W * FW_W * 128; i++)
				if(airy->ids[i] == BLOCK_LEAVES && birch_only->ids[i] == BLOCK_LEAVES)
					overlap++;
			g_fw_view = saved;
			free(airy);
		}
		for(int x = 0; x < FW_W; x++)
			for(int z = 0; z < FW_W; z++)
				for(int y = GROUND - 1; y < 110; y++) {
					int idx = (x * FW_W + z) * 128 + y;
					/* (1) both paths agree cell for cell: id AND metadata */
					struct block_data b;
					assert(server_world_get_block(&s.world, x, y, z, &b));
					if(b.type != f->ids[idx] || b.metadata != f->meta[idx]) {
						printf("FAIL seed %lld cell (%d,%d,%d): populate-path %d:%d vs runtime-path %d:%d\n",
							   (long long)seed, x, y, z, f->ids[idx], f->meta[idx], b.type, b.metadata);
						assert(0 && "runtime growth must equal populate growth (extra removal flags?)");
					}
					/* (2) a surviving birch leaf keeps species 2 */
					if(birch_only->ids[idx] == BLOCK_LEAVES && f->ids[idx] == BLOCK_LEAVES) {
						if((f->meta[idx] & 3) != 2) {
							printf("FAIL seed %lld: birch leaf (%d,%d,%d) now meta %d\n",
								   (long long)seed, x, y, z, f->meta[idx]);
							assert(0 && "big oak discs must not rewrite an existing leaf (Chunk.java:279)");
						}
					}
					if(f->ids[idx] == BLOCK_LEAVES && (f->meta[idx] & 8))
						flagged++;
				}
		if(overlap < 8)
			continue; /* the discs must really have crossed the birch canopy */
		verified++;
		overlap_total += overlap;
		flagged_total += flagged;
		printf("  seed %lld: big oak over birch, %d birch leaves under its discs keep meta 2, %d flagged leaves total, both paths identical\n",
			   (long long)seed, overlap, flagged);
	}
	printf("test_bigtree_leaves_keep_golden: %d big oaks verified, %d overlapped birch leaves kept species 2, %d flagged\n",
		   verified, overlap_total, flagged_total);
	assert(verified >= 3 && overlap_total >= 20);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	free(f);
	free(birch_only);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
