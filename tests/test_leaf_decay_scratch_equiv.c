/*
	Equivalence: the production leaf updateTick (source/block/block_leaves.c,
	an 11^3 int8_t scratch) against an independent transcription of Beta
	BlockLeaves.updateTick [740c583 BlockLeaves.java:52-144] on the ORIGINAL
	32^3 int[] scratch (var9 = 32, var10 = 1024, var11 = 16), driven over the
	same live server world with random and adversarial leaf/log layouts.

	Per trial both sides must agree on the verdict (inert / survive+unflag /
	remove), the resulting block + metadata, and the World.rand consumption
	(zero draws on inert/survive; nextInt(20) + [chance nextFloat + 3 position
	nextFloat] on remove -- Block.java:321-347). Both scratches are persistent
	and never cleared, so the trials deliberately interleave chunk-gate
	FAILURES (World.checkChunksExist false [World.java:327-348]) after survive
	and after remove verdicts: Beta then judges by the stale centre cell
	[BlockLeaves.java:135], and the port must too.

	The claim the 11^3 port rests on -- of Beta's 32 cells per axis only
	offsets -5..+5 are ever addressed, and the |offset| == 5 shell is never
	written (a shell cell is written only if it holds -2, and the fill never
	stores -2 outside -4..+4) -- is asserted on the 32^3 reference after every
	scan: every cell outside the -4..+4 cube is still 0.

	The reference's chunk-existence oracle is hard-wired to the fixture (chunks
	0..1 x 0..1 loaded), independent of server_world_is_chunk_loaded.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/block_leaves.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

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

/* ---------------- Beta reference: the 32^3 int[] transcription ---------------- */

enum ref_verdict { REF_INERT = 0, REF_SURVIVE = 1, REF_REMOVE = 2 };

static int32_t ref_adjacentTreeBlocks[32 * 32 * 32]; /* Java int[var9^3], zeroed once */

static int ref_block_id(int x, int y, int z) {
	struct block_data b;
	if(!server_world_get_block(&s.world, x, y, z, &b))
		return 0; /* World.getBlockId: 0 outside the world / unloaded */
	return b.type;
}

static int java_shr4(int v) { /* Java `>> 4` is arithmetic: floor(v / 16) */
	return v >= 0 ? v >> 4 : -((-v - 1) / 16) - 1;
}

/* World.checkChunksExist [World.java:327-348] over the FIXTURE's loaded set. */
static bool ref_checkChunksExist(int x1, int y1, int z1, int x2, int y2, int z2) {
	if(!(y2 >= 0 && y1 < 128))
		return false;
	for(int cx = java_shr4(x1); cx <= java_shr4(x2); cx++)
		for(int cz = java_shr4(z1); cz <= java_shr4(z2); cz++)
			if(!(cx >= 0 && cx <= 1 && cz >= 0 && cz <= 1))
				return false;
	return true;
}

/* BlockLeaves.updateTick [:52-144] minus the world writes: returns the verdict. */
static enum ref_verdict ref_updateTick(int var2, int var3, int var4, int var6) {
	if((var6 & 8) == 0)
		return REF_INERT;
	const int var7 = 4, var8 = var7 + 1, var9 = 32, var10 = var9 * var9,
			  var11 = var9 / 2;
	int32_t* a = ref_adjacentTreeBlocks;
	int var12;
	if(ref_checkChunksExist(var2 - var8, var3 - var8, var4 - var8, var2 + var8,
							var3 + var8, var4 + var8)) {
		for(var12 = -var7; var12 <= var7; ++var12)
			for(int var13 = -var7; var13 <= var7; ++var13)
				for(int var14 = -var7; var14 <= var7; ++var14) {
					int var15 = ref_block_id(var2 + var12, var3 + var13, var4 + var14);
					int idx = (var12 + var11) * var10 + (var13 + var11) * var9 + var14 + var11;
					if(var15 == BLOCK_LOG)
						a[idx] = 0;
					else if(var15 == BLOCK_LEAVES)
						a[idx] = -2;
					else
						a[idx] = -1;
				}
		for(var12 = 1; var12 <= 4; ++var12)
			for(int var13 = -var7; var13 <= var7; ++var13)
				for(int var14 = -var7; var14 <= var7; ++var14)
					for(int var15 = -var7; var15 <= var7; ++var15) {
						if(a[(var13 + var11) * var10 + (var14 + var11) * var9 + var15 + var11] != var12 - 1)
							continue;
						if(a[(var13 + var11 - 1) * var10 + (var14 + var11) * var9 + var15 + var11] == -2)
							a[(var13 + var11 - 1) * var10 + (var14 + var11) * var9 + var15 + var11] = var12;
						if(a[(var13 + var11 + 1) * var10 + (var14 + var11) * var9 + var15 + var11] == -2)
							a[(var13 + var11 + 1) * var10 + (var14 + var11) * var9 + var15 + var11] = var12;
						if(a[(var13 + var11) * var10 + (var14 + var11 - 1) * var9 + var15 + var11] == -2)
							a[(var13 + var11) * var10 + (var14 + var11 - 1) * var9 + var15 + var11] = var12;
						if(a[(var13 + var11) * var10 + (var14 + var11 + 1) * var9 + var15 + var11] == -2)
							a[(var13 + var11) * var10 + (var14 + var11 + 1) * var9 + var15 + var11] = var12;
						if(a[(var13 + var11) * var10 + (var14 + var11) * var9 + (var15 + var11 - 1)] == -2)
							a[(var13 + var11) * var10 + (var14 + var11) * var9 + (var15 + var11 - 1)] = var12;
						if(a[(var13 + var11) * var10 + (var14 + var11) * var9 + var15 + var11 + 1] == -2)
							a[(var13 + var11) * var10 + (var14 + var11) * var9 + var15 + var11 + 1] = var12;
					}
	}
	var12 = a[var11 * var10 + var11 * var9 + var11];
	return var12 >= 0 ? REF_SURVIVE : REF_REMOVE;
}

/* The 11^3 claim: nothing outside offsets -4..+4 is ever non-zero. */
static void ref_assert_outer_untouched(void) {
	for(int i = 0; i < 32; i++)
		for(int j = 0; j < 32; j++)
			for(int k = 0; k < 32; k++) {
				bool inner = abs(i - 16) <= 4 && abs(j - 16) <= 4 && abs(k - 16) <= 4;
				if(!inner)
					assert(ref_adjacentTreeBlocks[i * 1024 + j * 32 + k] == 0
						   && "a cell outside the -4..+4 cube was written");
			}
}

/* ---------------- fixture helpers ---------------- */

static void set(int x, int y, int z, int type, int meta) {
	if(y < 0 || y >= 128 || x < 0 || x > 31 || z < 0 || z > 31)
		return; /* outside the loaded chunks: leave it (reads as air/unloaded) */
	struct block_data expected = {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)};
	/* Raw write (no lifecycle, no light flood): the layout must carry exactly
	 * the flags set here, and the fixture must not pay a 729-lookup removal
	 * pass per log placed/cleared. Production's own removal on a REMOVE
	 * verdict still goes through the full server_world_set_block. */
	server_world_set_block_silent(&s.worlds[0], x, y, z, expected);
	/* Every write emits one CRPC_SET_BLOCK; drain the client queue inline so
	 * the fixture never blocks on a full queue behind a sleeping drain thread
	 * (that alone cost ~1 ms per write -- 335 s for the whole run). */
	clin_drain_discard();
}
static bool get(int x, int y, int z, struct block_data* b) {
	return server_world_get_block(&s.world, x, y, z, b);
}

/* BlockLeaves.java:151-152, Block.java:321-345: quantity on World.rand,
 * then chance and the three position draws for each emitted sapling.
 * F4-RNG (f049fa3d8) moved these draws off the session xorshift stream. */
static int shadow_remove_leaves_draws(struct java_random* shadow) {
	int quantity = jrand_next_int_bound(shadow, 20) == 0 ? 1 : 0;
	for(int k = 0; k < quantity; k++) {
		(void)jrand_next_float(shadow);
		(void)jrand_next_float(shadow);
		(void)jrand_next_float(shadow);
		(void)jrand_next_float(shadow);
	}
	return quantity;
}

/* test-private LCG, independent of the server stream */
static uint32_t g_lcg = 0x2545F491u;
static uint32_t lrand(void) {
	g_lcg = g_lcg * 1664525u + 1013904223u;
	return g_lcg >> 8;
}
static int lrange(int lo, int hi) { /* inclusive */
	return lo + (int)(lrand() % (uint32_t)(hi - lo + 1));
}

struct placed { int x, y, z; };
static struct placed g_placed[4096];
static int g_nplaced;
static void place(int x, int y, int z, int type, int meta) {
	if(y < 0 || y >= 128 || x < 0 || x > 31 || z < 0 || z > 31)
		return;
	set(x, y, z, type, meta);
	assert(g_nplaced < 4096);
	g_placed[g_nplaced++] = (struct placed) {x, y, z};
}
static void clear_placed(void) {
	for(int i = 0; i < g_nplaced; i++)
		set(g_placed[i].x, g_placed[i].y, g_placed[i].z, BLOCK_AIR, 0);
	g_nplaced = 0;
}

/* One trial: the layout is already in the world and the centre leaf carries
 * `meta`. Compare the reference verdict with what production does. */
static int g_counts[3];
static int g_gated;
static double g_t_layout, g_t_trial, g_t_clear;
static void run_trial(int cx, int cy, int cz, int meta) {
	struct block_data before;
	assert(get(cx, cy, cz, &before) && before.type == BLOCK_LEAVES
		   && before.metadata == (meta & 0xF));
	bool gate = ref_checkChunksExist(cx - 5, cy - 5, cz - 5, cx + 5, cy + 5, cz + 5);
	if(!gate && (meta & 8))
		g_gated++;

	enum ref_verdict v = ref_updateTick(cx, cy, cz, meta);
	ref_assert_outer_untouched();

	struct java_random shadow = s.worlds[0].rand;
	struct random_gen session_before = s.rand_src;
	int saplings = 0;
	if(v == REF_REMOVE)
		saplings = shadow_remove_leaves_draws(&shadow);
	size_t ents = dict_entity_size(s.entities);
#ifdef CAVEWII_TESTING
	uint32_t lookups0 = block_leaves_test_scan_lookups();
#endif

	struct block_data cd = before;
	struct block_info info = {.block = &cd, .neighbours = NULL, .x = cx, .y = cy, .z = cz};
	blocks[BLOCK_LEAVES]->onRandomTick(&s, &info);

	struct block_data after;
	assert(get(cx, cy, cz, &after));
	switch(v) {
		case REF_INERT:
			assert(after.type == BLOCK_LEAVES && after.metadata == before.metadata);
			break;
		case REF_SURVIVE:
			assert(after.type == BLOCK_LEAVES && after.metadata == (before.metadata & 7));
			break;
		case REF_REMOVE:
			assert(after.type == BLOCK_AIR);
			break;
	}
	assert(s.worlds[0].rand.seed == shadow.seed && "World.rand consumption differs");
	assert(s.worlds[0].rand.have_next_gaussian == shadow.have_next_gaussian);
	assert(s.worlds[0].rand.next_gaussian_cached == shadow.next_gaussian_cached);
	assert(!memcmp(&s.rand_src, &session_before, sizeof(session_before))
		&& "leaf decay consumed the unrelated session stream");
	assert(dict_entity_size(s.entities) == ents + (size_t)saplings);
#ifdef CAVEWII_TESTING
	uint32_t looked = block_leaves_test_scan_lookups() - lookups0;
	assert(looked == (uint32_t)((v != REF_INERT && gate) ? 729 : 0));
#endif
	g_counts[v]++;
}

/* Random layout in the +-5 box around the centre; `density` in 0..100. */
static void layout_random(int cx, int cy, int cz, int leaf_pct, int log_pct, int other_pct) {
	for(int dx = -5; dx <= 5; dx++)
		for(int dy = -5; dy <= 5; dy++)
			for(int dz = -5; dz <= 5; dz++) {
				if(!dx && !dy && !dz)
					continue;
				int r = lrange(0, 99);
				if(r < leaf_pct)
					place(cx + dx, cy + dy, cz + dz, BLOCK_LEAVES, lrange(0, 2) | (lrange(0, 1) ? 8 : 0));
				else if(r < leaf_pct + log_pct)
					place(cx + dx, cy + dy, cz + dz, BLOCK_LOG, lrange(0, 2));
				else if(r < leaf_pct + log_pct + other_pct)
					place(cx + dx, cy + dy, cz + dz, BLOCK_STONE, 0);
			}
}

/* Adversarial: a straight leaf chain of `len` from the centre along an axis
 * ending at a log (so the log sits at distance len+1), plus optional decoys:
 * a log shell at |offset| == 5 (outside the region), a leaf shell at 5. */
static void layout_chain(int cx, int cy, int cz, int axis, int dir, int len,
						 bool log_shell, bool leaf_shell) {
	for(int d = 1; d <= len; d++) {
		int x = cx + (axis == 0 ? d * dir : 0), y = cy + (axis == 1 ? d * dir : 0),
			z = cz + (axis == 2 ? d * dir : 0);
		place(x, y, z, BLOCK_LEAVES, 0);
	}
	{
		int d = len + 1;
		place(cx + (axis == 0 ? d * dir : 0), cy + (axis == 1 ? d * dir : 0),
			  cz + (axis == 2 ? d * dir : 0), BLOCK_LOG, 0);
	}
	if(log_shell || leaf_shell)
		for(int dx = -5; dx <= 5; dx++)
			for(int dy = -5; dy <= 5; dy++)
				for(int dz = -5; dz <= 5; dz++) {
					if(abs(dx) != 5 && abs(dy) != 5 && abs(dz) != 5)
						continue;
					struct block_data b;
					if(get(cx + dx, cy + dy, cz + dz, &b) && b.type != BLOCK_AIR)
						continue;
					place(cx + dx, cy + dy, cz + dz, log_shell ? BLOCK_LOG : BLOCK_LEAVES, 0);
				}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_leaf_decay_scratch_equiv_tmp", s_fixture_TMP_WORLD,
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
	jrand_set_seed(&s.worlds[0].rand, 4242);
	server_local_test_seed_math_random(1357);
	dict_entity_init(s.entities);
	s.player.x = 1000.0;
	s.player.y = 1000.0;
	s.player.z = 1000.0;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	const int TRIALS = 320;
	for(int t = 0; t < TRIALS; t++) {
		/* centre: interior most of the time, every 4th trial on a chunk edge
		 * so the radius-5 gate fails (x or z within 4 of the loaded border) */
		int cx, cz;
		if(t % 4 == 3) {
			cx = lrange(0, 1) ? lrange(1, 4) : lrange(27, 30);
			cz = lrange(1, 30);
		} else {
			cx = lrange(5, 26);
			cz = lrange(5, 26);
		}
		int cy = lrange(20, 110);
		int kind = t % 8;
		clock_t c0 = clock();
		switch(kind) {
			case 0: layout_random(cx, cy, cz, 30, 3, 5); break;
			case 1: layout_random(cx, cy, cz, 60, 1, 2); break;
			case 2: layout_random(cx, cy, cz, 12, 8, 30); break;
			case 3: layout_random(cx, cy, cz, 45, 0, 0); break; /* no logs at all */
			case 4: layout_chain(cx, cy, cz, lrange(0, 2), lrange(0, 1) ? 1 : -1, 3, false, false); break; /* log at 4: reached */
			case 5: layout_chain(cx, cy, cz, lrange(0, 2), lrange(0, 1) ? 1 : -1, 4, true, false); break;  /* log at 5: not */
			case 6: layout_chain(cx, cy, cz, lrange(0, 2), lrange(0, 1) ? 1 : -1, 2, false, true); break;  /* leaf shell at 5 */
			default: layout_random(cx, cy, cz, 25, 2, 10); layout_chain(cx, cy, cz, lrange(0, 2), 1, lrange(1, 5), lrange(0, 1), lrange(0, 1)); break;
		}
		/* the centre leaf: flagged 7 times in 8 */
		int meta = lrange(0, 2) | ((t % 8 == 2) ? 0 : 8);
		place(cx, cy, cz, BLOCK_LEAVES, meta);
		clock_t c1 = clock();
		run_trial(cx, cy, cz, meta);
		clock_t c2 = clock();
		clear_placed();
		clock_t c3 = clock();
		g_t_layout += (double)(c1 - c0) / CLOCKS_PER_SEC;
		g_t_trial += (double)(c2 - c1) / CLOCKS_PER_SEC;
		g_t_clear += (double)(c3 - c2) / CLOCKS_PER_SEC;
	}
	printf("  phases: layout %.1fs trial %.1fs clear %.1fs\n", g_t_layout, g_t_trial, g_t_clear);
	printf("test_leaf_decay_scratch_equiv: %d trials -- inert %d, survive %d, remove %d, "
		   "chunk-gated (stale-centre) %d: 11^3 port == 32^3 Beta reference\n",
		   TRIALS, g_counts[0], g_counts[1], g_counts[2], g_gated);
	assert(g_counts[1] > 20 && g_counts[2] > 20 && g_gated > 20);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
