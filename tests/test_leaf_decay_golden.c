/*
	Faithfulness golden for Beta 1.7.3 leaf decay (source/block/block_leaves.c).
	Decompile ref 740c583 -- every expected value below is derived by hand from
	the Java, never from running CaveWii:

	  BlockLeaves.onBlockRemoval  [BlockLeaves.java:33-50]  radius 1, every leaf
	      in the 3x3x3 gets metadata |8 (unconditional), gated on
	      World.checkChunksExist at radius 2.
	  BlockLog.onBlockRemoval     [BlockLog.java:23-42]     radius 4 (9x9x9),
	      only leaves with (meta & 8) == 0 are written, chunk gate radius 5.
	  BlockLeaves.updateTick      [BlockLeaves.java:52-144] acts ONLY when
	      (meta & 8) != 0; fills a 32^3 scratch over the 9x9x9 region
	      (0 = wood, -2 = leaves, -1 = other), then four level passes mark
	      leaves 6-adjacent to level d-1 with d (d = 1..4); centre >= 0 ->
	      setBlockMetadata(meta & -9) [no notify, no RNG]; else removeLeaves.
	      The scratch is NEVER cleared and the centre is read even when the
	      chunk gate fails [:135].
	  BlockLeaves.removeLeaves    [BlockLeaves.java:146-149] dropBlockAsItem
	      THEN setBlockWithNotify(0).
	  Block.dropBlockAsItemWithChance [Block.java:321-335] + dropBlockAsItem_do
	      [:337-347]: World.rand draws are quantityDropped = nextInt(20)
	      [BlockLeaves.java:151-153], then per dropped unit nextFloat() <= 1.0F
	      and three nextFloat() position offsets. So a decaying leaf consumes
	      exactly 1 draw (no sapling) or 1+1+3 = 5 draws (one sapling).
	  ItemLeaves.getPlacedBlockMetadata [ItemLeaves.java:10-12] `damage | 8`:
	      1.7.3 has NO player-placed / no-decay bit; placed leaves are flagged.
	  World.checkChunksExist      [World.java:327-348].

	Production path: real superflat server_world + the block registry; the
	registered onRandomTick / onBlockRemoval / onItemPlace hooks are called
	exactly as the random-tick driver and the set-block lifecycle call them.

	RED on the pre-fix rule ("no log in the 9x9x9 -> decay, even through air;
	any log in it -> live, flag ignored"): placed leaves are unflagged, log/leaf
	removal flags nothing, the reached leaf keeps its flag, the air-gap and
	diagonal leaves survive, the unflagged leaf decays.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

#include "block/block_leaves.h" /* BLOCK_LEAVES_DECAY_CHECK_BIT + the test counter */

#define FLAG BLOCK_LEAVES_DECAY_CHECK_BIT /* BlockLeaves.java:55 `(var6 & 8) != 0` */

/* The lookup-counter checks pin the Wii cost of one updateTick (729 block
 * lookups when the chunk gate opens, none when it fails). They ride on the
 * test pool's CAVEWII_TESTING define (the header exposes the counter under
 * it). The RED run against the base sources compiled without them (the
 * header did not exist yet); every Beta rule check is unconditional. */

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static volatile int g_drain_run = 1;
static int g_failures = 0;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* Soft check: print + count, keep going so a RED run lists every broken rule. */
#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if(cond) {                                                             \
			printf("  ok   ");                                                 \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
		} else {                                                               \
			printf("  FAIL ");                                                 \
			printf(__VA_ARGS__);                                               \
			printf("  [%s:%d]\n", __FILE__, __LINE__);                         \
			g_failures++;                                                      \
		}                                                                      \
	} while(0)

static void set(int x, int y, int z, int type, int meta) {
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = (uint8_t)(meta & 0x0F),
	};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}
static int gid(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int gmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F)
														  : -1;
}
/* One leaf random tick on the live cell, exactly as server_world_random_tick
 * dispatches it (a COPY of the cell, neighbours NULL). */
static void leaf_tick(int x, int y, int z) {
	struct block_data cd;
	assert(server_world_get_block(&s.world, x, y, z, &cd));
	if(cd.type != BLOCK_LEAVES)
		return; /* already gone (a wrong rule decayed it): the CHECK reports it */
	struct block_info info = {.block = &cd, .neighbours = NULL, .x = x, .y = y, .z = z};
	blocks[BLOCK_LEAVES]->onRandomTick(&s, &info);
}

/* Beta's World.rand consumption for one removeLeaves, replayed on a shadow
 * copy of the server stream: quantityDropped nextInt(20) == 0 ? 1 : 0, then
 * per unit nextFloat (chance <= 1.0F) + 3 nextFloat (position). Returns the
 * sapling count so the caller can pin the entity delta too. */
/* F4-RNG (ASTRA-RNG2-RULING 1/2): the stream is the leaf's world's
 * java.util.Random World.rand (struct server_world.rand); the session
 * xorshift rand_src must not move at all. */
static int shadow_remove_leaves_draws(struct java_random* shadow) {
	int quantity = jrand_next_int_bound(shadow, 20) == 0 ? 1 : 0;
	for(int k = 0; k < quantity; k++) {
		(void)jrand_next_float(shadow); /* nextFloat() <= 1.0F -- always true */
		(void)jrand_next_float(shadow); /* dropBlockAsItem_do x */
		(void)jrand_next_float(shadow); /* y */
		(void)jrand_next_float(shadow); /* z */
	}
	return quantity;
}

/* Find a seed (from `from` upward) whose FIRST draw gives the wanted
 * nextInt(20)==0 outcome so both removeLeaves branches are exercised. */
static uint32_t seed_with_sapling_roll(uint32_t from, bool want_sapling) {
	for(uint32_t seed = from;; seed += 7919u) {
		struct java_random g;
		jrand_set_seed(&g, (int64_t)(seed | 1u));
		bool sapling = jrand_next_int_bound(&g, 20) == 0;
		if(sapling == want_sapling)
			return seed | 1u;
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* keep the per-rule listing on an abort */
	test_fixture_dir("test_leaf_decay_golden_tmp", s_fixture_TMP_WORLD,
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
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x9E3779B9u; /* fixed, nonzero (xorshift 0 is a fixed point) */
	dict_entity_init(s.entities); /* removeLeaves may spawn a sapling item */
	s.player.x = 1000.0; /* far from every placement -> canBlockBePlacedAt clear */
	s.player.y = 1000.0;
	s.player.z = 1000.0;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
	/* Loaded columns: x,z in 0..31. A leaf/log whose radius-5 box stays inside
	 * x,z in 5..26 passes checkChunksExist; anything nearer the edge fails it. */
	const int Y = 72; /* open air above the superflat surface */

	/* ================= (d1) ItemLeaves placement: damage | 8 ================= */
	printf("(d1) ItemLeaves.getPlacedBlockMetadata = damage | 8\n");
	{
		set(20, Y, 20, BLOCK_STONE, 0);
		struct block_data on_d = {.type = BLOCK_STONE, .metadata = 0};
		struct block_data where_d = {.type = BLOCK_AIR, .metadata = 0};
		struct block_info on = {.block = &on_d, .x = 20, .y = Y, .z = 20};
		struct block_info where = {.block = &where_d, .x = 20, .y = Y + 1, .z = 20};
		struct item_data it = {.id = BLOCK_LEAVES, .durability = 2, .count = 1};
		assert(blocks[BLOCK_LEAVES]->block_item.onItemPlace);
		bool placed = blocks[BLOCK_LEAVES]->block_item.onItemPlace(
			&s, &s.player, &it, &where, &on, SIDE_TOP);
		CHECK(placed && gid(20, Y + 1, 20) == BLOCK_LEAVES,
			  "placement writes a leaf block");
		CHECK(gmeta(20, Y + 1, 20) == (2 | FLAG),
			  "placed birch leaf metadata = 2 | 8 = 10 (got %d)",
			  gmeta(20, Y + 1, 20));
		set(20, Y + 1, 20, BLOCK_AIR, 0);
		set(20, Y, 20, BLOCK_AIR, 0);
	}

	/* ============ (d2) BlockLog.onBlockRemoval: radius 4, unflagged only ============ */
	printf("(d2) BlockLog.onBlockRemoval flags leaves in the 9x9x9 (radius 4)\n");
	{
		/* Log L1 at (12,Y,10), log L2 at (12,Y,15); leaf chain z = 11..14
		 * links them: from L2 the chain is 14 (d1), 13 (d2), 12 (d3), 11 (d4). */
		set(12, Y, 10, BLOCK_LOG, 0);
		set(12, Y, 15, BLOCK_LOG, 0);
		for(int z = 11; z <= 14; z++)
			set(12, Y, z, BLOCK_LEAVES, 0);
		set(12, Y + 4, 10, BLOCK_LEAVES, 1);	   /* dy = +4 -> in the cube */
		set(12, Y + 5, 10, BLOCK_LEAVES, 1);	   /* dy = +5 -> outside */
		set(8, Y, 10, BLOCK_LEAVES, 2);			   /* dx = -4 -> in */
		set(7, Y, 10, BLOCK_LEAVES, 2);			   /* dx = -5 -> outside */
		set(12, Y - 4, 10, BLOCK_LEAVES, FLAG | 0); /* already flagged: skipped */
		set(12, Y, 10, BLOCK_AIR, 0);				   /* remove L1 -> onBlockRemoval */
		CHECK(gmeta(12, Y + 4, 10) == (1 | FLAG), "dy=+4 pine leaf -> 1|8 = 9 (got %d)",
			  gmeta(12, Y + 4, 10));
		CHECK(gmeta(12, Y + 5, 10) == 1, "dy=+5 leaf untouched -> 1 (got %d)",
			  gmeta(12, Y + 5, 10));
		CHECK(gmeta(8, Y, 10) == (2 | FLAG), "dx=-4 birch leaf -> 2|8 = 10 (got %d)",
			  gmeta(8, Y, 10));
		CHECK(gmeta(7, Y, 10) == 2, "dx=-5 leaf untouched -> 2 (got %d)",
			  gmeta(7, Y, 10));
		CHECK(gmeta(12, Y - 4, 10) == FLAG, "pre-flagged leaf stays 8 (got %d)",
			  gmeta(12, Y - 4, 10));
		for(int z = 11; z <= 14; z++)
			CHECK(gmeta(12, Y, z) == FLAG, "chain leaf z=%d flagged -> 8 (got %d)", z,
				  gmeta(12, Y, z));

		/* (a) the clump is connected to ANOTHER tree's log within 4 leaf steps:
		 * ticking (12,Y,11) (d4 from L2) survives and is UNFLAGGED, drawing
		 * nothing from World.rand. */
		printf("(a) flagged clump reachable from the other log at 4 leaf steps survives\n");
		uint64_t seed_before = s.worlds[0].rand.seed;
		const uint32_t session_before = s.rand_src.seed;
		leaf_tick(12, Y, 11);
		CHECK(gid(12, Y, 11) == BLOCK_LEAVES, "d4 leaf survives");
		CHECK(gmeta(12, Y, 11) == 0, "d4 leaf unflagged: meta & -9 -> 0 (got %d)",
			  gmeta(12, Y, 11));
		CHECK(s.worlds[0].rand.seed == seed_before && s.rand_src.seed == session_before, "survive path draws nothing");
		/* the rest of the chain is still flagged (only the ticked cell is written) */
		CHECK(gmeta(12, Y, 12) == FLAG && gmeta(12, Y, 13) == FLAG
				  && gmeta(12, Y, 14) == FLAG,
			  "untouched chain cells keep their flag");
		/* clean up this cluster */
		set(12, Y, 15, BLOCK_AIR, 0); /* log removal flags the chain again (radius 4) */
		for(int z = 11; z <= 14; z++)
			set(12, Y, z, BLOCK_AIR, 0);
		set(12, Y + 4, 10, BLOCK_AIR, 0);
		set(12, Y + 5, 10, BLOCK_AIR, 0);
		set(8, Y, 10, BLOCK_AIR, 0);
		set(7, Y, 10, BLOCK_AIR, 0);
		set(12, Y - 4, 10, BLOCK_AIR, 0);
	}

	/* ============ (d3) BlockLeaves.onBlockRemoval: radius 1, unconditional ============ */
	printf("(d3) BlockLeaves.onBlockRemoval flags the 3x3x3 around a removed leaf\n");
	{
		set(20, Y + 8, 20, BLOCK_LEAVES, 0);
		set(21, Y + 8, 20, BLOCK_LEAVES, 0);	/* dx=+1 */
		set(20, Y + 9, 20, BLOCK_LEAVES, 1);	/* dy=+1 */
		set(19, Y + 7, 19, BLOCK_LEAVES, 2);	/* corner (-1,-1,-1): in the cube */
		set(22, Y + 8, 20, BLOCK_LEAVES, 0);	/* dx=+2: outside */
		set(20, Y + 8, 20, BLOCK_AIR, 0);		/* remove the centre leaf */
		CHECK(gmeta(21, Y + 8, 20) == FLAG, "dx=+1 -> 0|8 (got %d)", gmeta(21, Y + 8, 20));
		CHECK(gmeta(20, Y + 9, 20) == (1 | FLAG), "dy=+1 -> 1|8 = 9 (got %d)",
			  gmeta(20, Y + 9, 20));
		CHECK(gmeta(19, Y + 7, 19) == (2 | FLAG), "corner -> 2|8 = 10 (got %d)",
			  gmeta(19, Y + 7, 19));
		CHECK(gmeta(22, Y + 8, 20) == 0, "dx=+2 untouched (got %d)", gmeta(22, Y + 8, 20));
		set(21, Y + 8, 20, BLOCK_AIR, 0);
		set(20, Y + 9, 20, BLOCK_AIR, 0);
		set(19, Y + 7, 19, BLOCK_AIR, 0);
		set(22, Y + 8, 20, BLOCK_AIR, 0);
	}

	/* ================= tree T: trunk (16,Y..Y+3,16) ================= */
	for(int y = Y; y <= Y + 3; y++)
		set(16, y, 16, BLOCK_LOG, 0);
	const int TOP = Y + 3;

	/* (b) separated from the log by AIR: log two cells below, air between. The
	 * pre-fix rule keeps it (log inside the 9x9x9); Beta decays it. */
	printf("(b) flagged leaves with only air between them and the log decay\n");
	{
		set(16, TOP + 2, 16, BLOCK_LEAVES, FLAG | 0); /* (16,TOP+1,16) is air */
		size_t ents = dict_entity_size(s.entities);
		jrand_set_seed(&s.worlds[0].rand, (int64_t)seed_with_sapling_roll(0x1234567u, false));
		struct java_random shadow = s.worlds[0].rand;
		const uint32_t session_seed = s.rand_src.seed;
		int saplings = shadow_remove_leaves_draws(&shadow);
		assert(saplings == 0);
		leaf_tick(16, TOP + 2, 16);
		CHECK(gid(16, TOP + 2, 16) == BLOCK_AIR, "air-gap leaf removed");
		CHECK(s.worlds[0].rand.seed == shadow.seed && s.rand_src.seed == session_seed,
			  "removeLeaves drew exactly nextInt(20) (1 draw, no sapling)");
		CHECK(dict_entity_size(s.entities) == ents, "no sapling entity spawned");

		/* diagonal-only contact is NOT 6-adjacency: decays */
		set(17, TOP + 1, 17, BLOCK_LEAVES, FLAG | 1);
		jrand_set_seed(&s.worlds[0].rand, (int64_t)seed_with_sapling_roll(0x2345678u, true));
		shadow = s.worlds[0].rand;
		saplings = shadow_remove_leaves_draws(&shadow);
		assert(saplings == 1);
		ents = dict_entity_size(s.entities);
		leaf_tick(17, TOP + 1, 17);
		CHECK(gid(17, TOP + 1, 17) == BLOCK_AIR, "diagonal-only leaf removed");
		CHECK(s.worlds[0].rand.seed == shadow.seed && s.rand_src.seed == session_seed,
			  "removeLeaves drew nextInt(20) + chance + 3 position floats (5 draws)");
		CHECK(dict_entity_size(s.entities) == ents + 1, "one sapling item spawned");
	}

	/* (a') a 6-connected chain along +x from the trunk top: d1..d5 */
	printf("(a') chain d1..d4 survives, d5 decays, and its removal re-flags d4\n");
	{
		for(int d = 1; d <= 5; d++)
			set(16 + d, TOP, 16, BLOCK_LEAVES, FLAG | 1);
		uint64_t seed_before = s.worlds[0].rand.seed;
		const uint32_t session_before = s.rand_src.seed;
		leaf_tick(20, TOP, 16); /* d4 */
		CHECK(gid(20, TOP, 16) == BLOCK_LEAVES && gmeta(20, TOP, 16) == 1,
			  "d4 leaf survives and is unflagged -> meta 1 (got %d)", gmeta(20, TOP, 16));
		CHECK(s.worlds[0].rand.seed == seed_before && s.rand_src.seed == session_before, "survive path draws nothing");
		jrand_set_seed(&s.worlds[0].rand, (int64_t)seed_with_sapling_roll(0x3456789u, false));
		struct java_random shadow = s.worlds[0].rand;
		const uint32_t session_seed = s.rand_src.seed;
		(void)shadow_remove_leaves_draws(&shadow);
		leaf_tick(21, TOP, 16); /* d5: unreachable within 4 steps */
		CHECK(gid(21, TOP, 16) == BLOCK_AIR, "d5 leaf removed");
		CHECK(s.worlds[0].rand.seed == shadow.seed && s.rand_src.seed == session_seed, "d5 removal draw sequence");
		/* setBlockWithNotify(0) -> Chunk.setBlockID -> BlockLeaves.onBlockRemoval
		 * (radius 1) re-flags the d4 leaf that its own tick had just cleared. */
		CHECK(gmeta(20, TOP, 16) == (1 | FLAG),
			  "d4 leaf re-flagged by the d5 removal -> 9 (got %d)", gmeta(20, TOP, 16));
		/* the survivors closer to the trunk keep their flag (never ticked) */
		CHECK(gmeta(17, TOP, 16) == (1 | FLAG) && gmeta(19, TOP, 16) == (1 | FLAG),
			  "unticked chain cells still flagged");
	}

	/* (c) unflagged leaves never decay on a random tick, whatever surrounds them */
	printf("(c) unflagged leaves are inert on the random tick\n");
	{
		set(8, TOP + 4, 8, BLOCK_LEAVES, 2); /* birch, no flag, no log anywhere near */
		uint64_t seed_before = s.worlds[0].rand.seed;
		const uint32_t session_before = s.rand_src.seed;
		for(int k = 0; k < 500; k++)
			leaf_tick(8, TOP + 4, 8);
		CHECK(gid(8, TOP + 4, 8) == BLOCK_LEAVES && gmeta(8, TOP + 4, 8) == 2,
			  "500 ticks: still a birch leaf, meta 2 (got id %d meta %d)",
			  gid(8, TOP + 4, 8), gmeta(8, TOP + 4, 8));
		CHECK(s.worlds[0].rand.seed == seed_before && s.rand_src.seed == session_before, "no World.rand draw on the inert path");
	}

	/* (e) chunk gate: a flagged leaf whose radius-5 box leaves the loaded
	 * chunks skips the scan and is judged by the STALE scratch centre
	 * [BlockLeaves.java:66 gate, :135 unconditional read]. */
	printf("(e) checkChunksExist false -> verdict from the previous scan's centre\n");
	{
		/* last full scan above was the d5 decay -> centre = -2 -> removal */
		set(2, TOP, 2, BLOCK_LEAVES, FLAG | 0); /* x-5 = -3 -> chunk -1 unloaded */
		jrand_set_seed(&s.worlds[0].rand, (int64_t)seed_with_sapling_roll(0x4567890u, false));
		struct java_random shadow = s.worlds[0].rand;
		const uint32_t session_seed = s.rand_src.seed;
		(void)shadow_remove_leaves_draws(&shadow);
#ifdef CAVEWII_TESTING
		uint32_t lookups0 = block_leaves_test_scan_lookups();
#endif
		leaf_tick(2, TOP, 2);
		CHECK(gid(2, TOP, 2) == BLOCK_AIR, "gated leaf after a decay scan: removed");
		CHECK(s.worlds[0].rand.seed == shadow.seed && s.rand_src.seed == session_seed, "gated removal draw sequence");
#ifdef CAVEWII_TESTING
		CHECK(block_leaves_test_scan_lookups() == lookups0,
			  "gated tick performed zero block lookups");
#endif
		/* now a full scan that SURVIVES (d1 leaf beside the trunk), then a gated
		 * leaf is judged by that centre (>= 0): survives and is unflagged */
		set(16, TOP, 15, BLOCK_LEAVES, FLAG | 0);
#ifdef CAVEWII_TESTING
		lookups0 = block_leaves_test_scan_lookups();
#endif
		leaf_tick(16, TOP, 15);
		CHECK(gmeta(16, TOP, 15) == 0, "d1 leaf survives, unflagged");
#ifdef CAVEWII_TESTING
		CHECK(block_leaves_test_scan_lookups() == lookups0 + 9 * 9 * 9,
			  "one gated-open updateTick = 729 block lookups (worst case)");
#endif
		set(2, TOP + 1, 2, BLOCK_LEAVES, FLAG | 2);
		uint64_t seed_before = s.worlds[0].rand.seed;
		const uint32_t session_before = s.rand_src.seed;
		leaf_tick(2, TOP + 1, 2);
		CHECK(gid(2, TOP + 1, 2) == BLOCK_LEAVES && gmeta(2, TOP + 1, 2) == 2,
			  "gated leaf after a survive scan: kept, unflagged -> 2 (got id %d meta %d)",
			  gid(2, TOP + 1, 2), gmeta(2, TOP + 1, 2));
		CHECK(s.worlds[0].rand.seed == seed_before && s.rand_src.seed == session_before, "gated survive draws nothing");
	}

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_leaf_decay_golden: %d failure(s)\n", g_failures);
	assert(g_failures == 0 && "Beta 1.7.3 leaf decay golden");
	server_world_destroy(&s.world);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
