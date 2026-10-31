/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_stats_counters` (LCE-02,
 * brief B3):
 *
 *   every general stat and the per-id arrays: exact Beta rounding
 *   (round(sqrt*100), the > 25 fly threshold, `f >= 2.0` fall), craft +=
 *   stackSize, harvest-only mine stat, canonical-id aliasing goldens
 *
 * The statistics half of the module, pinned to Beta rather than to itself:
 *
 *   - every one of the CW_STAT_ROWS general ids is driven by the ONE event
 *     design 1.4 says feeds it, and nothing else moves (a table-driven sweep
 *     that also fails if a row is added without a case);
 *   - the same rows again in REMOTE_SERVER mode, to prove the column is
 *     chosen by the bind mode and never by the producer;
 *   - source/game/stats/stats_beta.c against hand-computed Beta values, its
 *     two boundaries (fly > 25, fall >= 2.0F) and a motion where the float
 *     chain and the double chain genuinely disagree;
 *   - craftItem += stackSize, a disableStats() harvest counting nothing, and
 *     saturation at UINT32_MAX inside a per-id array;
 *   - and the four-array alias derivation of
 *     tests/golden/achievements/b3_alias_table.txt, cell by cell, plus the
 *     named pairs read back through the B4 screen rows.
 *
 * Each case runs against a FRESH profile id inside one per-process fixture
 * directory, so no case inherits a file another case wrote.
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "entity/monsters_object.h"
#include "game/achievements/achievements.h"
#include "game/achievements/achievements_api.h"
#include "game/stats/stats_beta.h"

static char s_dir[128];
static uint64_t s_gen = 5u;
static int s_case;

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

/* ---------------------------------------------------------------- helpers */

static const struct stats_profile* p0(void) {
	return achievements_debug_profile(0);
}

static struct ach_status status0(void) {
	struct ach_status st;
	memset(&st, 0, sizeof(st));
	assert(achievements_debug_status(0, &st));
	return st;
}

static uint32_t general_local(uint32_t stat_id) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	return p0()->general[i].local;
}

static uint32_t general_remote(uint32_t stat_id) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	return p0()->general[i].remote;
}

/* A fresh module + a profile id no earlier case used. */
static void fresh(enum ach_bind_mode mode) {
	char id[CW_PROFILE_ID_MAX + 1];
	snprintf(id, sizeof(id), "sc%d", s_case++);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, mode) == ACH_BIND_OK);
}

/* ------------------------------------------------- the general-stat matrix */

/* One row per CW_STAT_ROWS id: the event design 1.4 names as its trigger, the
 * authority Beta's owner column implies, the payload, and what the counter
 * must read afterwards. `also` is the second id an event legitimately bumps
 * (Minecraft.startWorld counts startGame AND createWorld/loadWorld on the
 * same call); 0 means "only `id` moves". */
struct feed {
	uint32_t id;
	uint32_t also;
	uint8_t kind;
	uint8_t sub;
	uint8_t authority;
	int32_t payload; /* cm / damage amount; 0 where the kind has none */
	uint32_t want;
};

static const struct feed FEEDS[] = {
	/* Minecraft.startWorld, both branches [design 1.4 rows 1000-1002]. */
	{1000u, 1001u, GE_GAME_STARTED, GE_WORLD_NEW, GE_AUTH_CLIENT, 0, 1u},
	{1001u, 1000u, GE_GAME_STARTED, GE_WORLD_NEW, GE_AUTH_CLIENT, 0, 1u},
	{1002u, 1000u, GE_GAME_STARTED, GE_WORLD_LOADED, GE_AUTH_CLIENT, 0, 1u},
	{1003u, 0u, GE_MP_JOINED, 0, GE_AUTH_CLIENT, 0, 1u},
	{1004u, 0u, GE_GAME_LEFT, 0, GE_AUTH_CLIENT, 0, 1u},
	/* EntityPlayer.onUpdate: +1 per tick. */
	{1100u, 0u, GE_TICK, 0, GE_AUTH_CLIENT, 0, 1u},
	/* addMovementStat's five arms; the payload is already the centimetre the
	 * producer rounded with stats_beta.c. */
	{2000u, 0u, GE_MOVED, GE_MOVE_WALK, GE_AUTH_CLIENT, 120, 120u},
	{2001u, 0u, GE_MOVED, GE_MOVE_SWIM, GE_AUTH_CLIENT, 7, 7u},
	{2002u, 0u, GE_FELL, 0, GE_AUTH_CLIENT, 250, 250u}, /* fall(float) */
	{2003u, 0u, GE_MOVED, GE_MOVE_CLIMB, GE_AUTH_CLIENT, 11, 11u},
	{2004u, 0u, GE_MOVED, GE_MOVE_FLY, GE_AUTH_CLIENT, 30, 30u},
	{2005u, 0u, GE_MOVED, GE_MOVE_DIVE, GE_AUTH_CLIENT, 9, 9u},
	/* addMountedMovementStat's three arms. */
	{2006u, 0u, GE_RIDE, GE_MOUNT_MINECART, GE_AUTH_CLIENT, 64, 64u},
	{2007u, 0u, GE_RIDE, GE_MOUNT_BOAT, GE_AUTH_CLIENT, 65, 65u},
	{2008u, 0u, GE_RIDE, GE_MOUNT_PIG, GE_AUTH_CLIENT, 66, 66u},
	{2010u, 0u, GE_JUMPED, 0, GE_AUTH_CLIENT, 0, 1u},
	{2011u, 0u, GE_DROPPED, 0, GE_AUTH_CLIENT, 0, 1u},
	/* The six server-owned rows: `+= damage`, not `+= 1`, for the two damage
	 * rows [design 1.4 rows 2020/2021]. */
	{2020u, 0u, GE_DAMAGE_DEALT, 0, GE_AUTH_SERVER, 6, 6u},
	{2021u, 0u, GE_DAMAGE_TAKEN, 0, GE_AUTH_SERVER, 3, 3u},
	{2022u, 0u, GE_DIED, 0, GE_AUTH_SERVER, 0, 1u},
	{2023u, 0u, GE_KILLED, MONSTER_ZOMBIE, GE_AUTH_SERVER, 0, 1u},
	{2024u, 0u, GE_KILLED_PLAYER, 0, GE_AUTH_SERVER, 0, 1u},
	{2025u, 0u, GE_FISH_CAUGHT, 0, GE_AUTH_SERVER, 0, 1u},
};
#define FEED_COUNT ((int)(sizeof(FEEDS) / sizeof(FEEDS[0])))

static void emit_feed(const struct feed* f) {
	struct game_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = f->kind;
	ev.authority = f->authority;
	ev.sub = f->sub;
	ev.who.slot = 0;
	ev.who.generation = s_gen;
	switch(f->kind) {
		case GE_MOVED:
		case GE_FELL: ev.u.move.cm = f->payload; break;
		case GE_RIDE: ev.u.ride.cm = f->payload; break;
		case GE_DAMAGE_DEALT:
		case GE_DAMAGE_TAKEN: ev.u.n.amount = f->payload; break;
		default: break;
	}
	game_event_emit(&ev);
}

/* Design 1.4: every general id, exactly one trigger, and NOTHING ELSE moves.
 * The sweep is the coverage proof too -- a CW_STAT_ROWS row with no case here
 * fails the `seen` check rather than quietly going untested. */
static void test_every_general_stat(void) {
	bool seen[CW_STAT_COUNT];
	memset(seen, 0, sizeof(seen));

	for(int r = 0; r < FEED_COUNT; r++) {
		const struct feed* f = &FEEDS[r];
		int idx = stat_general_index(f->id);
		assert(idx >= 0);
		seen[idx] = true;

		fresh(ACH_MODE_LOCAL_WORLD);
		emit_feed(f);
		achievements_tick();

		for(int i = 0; i < CW_STAT_COUNT; i++) {
			uint32_t id = stat_general_id(i);
			uint32_t got = p0()->general[i].local;
			uint32_t want = 0u;
			if(id == f->id)
				want = f->want;
			else if(f->also != 0u && id == f->also)
				want = 1u;
			if(got != want) {
				fprintf(stderr,
						"FAIL feed %lu: stat %lu = %lu, want %lu\n",
						(unsigned long)f->id, (unsigned long)id,
						(unsigned long)got, (unsigned long)want);
				assert(0);
			}
			/* LOCAL_WORLD never touches the remote column. */
			assert(p0()->general[i].remote == 0u);
		}
		achievements_unbind(0, s_gen);
	}

	for(int i = 0; i < CW_STAT_COUNT; i++)
		if(!seen[i]) {
			fprintf(stderr, "FAIL general stat %lu has no feed case\n",
					(unsigned long)stat_general_id(i));
			assert(0);
		}
	puts("PASS every general stat is driven by its Beta trigger and no other");
}

/* Design 5.2/6: the COLUMN comes from the bind mode, never from the producer.
 * The same client-owned events, emitted identically, land in `remote` on a
 * remote server -- Beta's EntityClientPlayerMP.addStat counts them locally on
 * the client either way, and the session is what makes them remote activity. */
static void test_columns_follow_the_bind_mode(void) {
	fresh(ACH_MODE_REMOTE_SERVER);
	for(int r = 0; r < FEED_COUNT; r++)
		if(FEEDS[r].authority == GE_AUTH_CLIENT)
			emit_feed(&FEEDS[r]);
	achievements_tick();

	assert(general_remote(2010u) == 1u && general_local(2010u) == 0u);
	assert(general_remote(2000u) == 120u && general_local(2000u) == 0u);
	assert(general_remote(1100u) == 1u && general_local(1100u) == 0u);
	/* A server-owned row cannot be reached by a client emit at all, so it is
	 * still zero on BOTH sides (the full matrix is test_stats_provenance). */
	assert(general_remote(2022u) == 0u && general_local(2022u) == 0u);
	achievements_unbind(0, s_gen);
	puts("PASS the bind mode picks the column, the producer never does");
}

/* ------------------------------------------------------- the Beta rounding */

/* EntityPlayer.addMovementStat / fall(), reproduced in stats_beta.c. Every
 * expectation here is hand-computed from the decompiled formula, not read
 * back out of the implementation. */
static void test_beta_rounding(void) {
	/* round(sqrt(dx^2+dz^2) * 100). A 3-4-5 triangle is 5 blocks = 500 cm. */
	assert(stats_beta_horizontal_cm(3.0, 4.0) == 500);
	assert(stats_beta_horizontal_cm(-3.0, -4.0) == 500);
	assert(stats_beta_horizontal_cm(0.0, 0.0) == 0);
	assert(stats_beta_horizontal_cm(0.1, 0.0) == 10);
	/* Half-up, not half-even: 0.125 blocks is 12.5 cm and rounds to 13. */
	assert(stats_beta_horizontal_cm(0.125, 0.0) == 13);
	/* 3-D adds the vertical leg: 1,2,2 is exactly 3 blocks. */
	assert(stats_beta_3d_cm(1.0, 2.0, 2.0) == 300);
	assert(stats_beta_3d_cm(3.0, 0.0, 4.0)
		   == stats_beta_horizontal_cm(3.0, 4.0));

	/* climbOneCm is a DOUBLE chain -- no sqrt, no float. */
	assert(stats_beta_climb_cm(1.0) == 100);
	assert(stats_beta_climb_cm(0.125) == 13);
	/* Beta gates the arm on dy > 0.0D and then adds the rounded value with no
	 * check of its own, so a sub-half-centimetre climb legitimately adds 0. */
	assert(stats_beta_climb_cm(0.004) == 0);

	/* THE CHAINS ARE NOT INTERCHANGEABLE, and this pair is the proof.
	 *
	 * 0.145 blocks of motion is 15 cm through the horizontal chain and 14 cm
	 * through the climb chain. Horizontal: sqrt gives the float 0.14500000f,
	 * * 100F lands on exactly 14.5f, + 0.5f = 15.0f, floor 15. Climb: the
	 * double 0.145 is 0.14499999999999999, * 100D is 14.499999999999998 --
	 * BELOW the half -- so floor(14.999999999999998) is 14.
	 *
	 * And it goes the other way at 0.265, so this is a chain difference and
	 * not a "float always rounds up" bias: the double product is exactly 26.5
	 * and rounds to 27, while the nearest float to 0.265 is 0.26499999, whose
	 * float product 26.499998f rounds to 26.
	 *
	 * Copying either chain to the other emit site would be a silent
	 * one-centimetre drift per step, which is why lane A calls these and does
	 * not re-type them. */
	assert(stats_beta_horizontal_cm(0.145, 0.0) == 15);
	assert(stats_beta_climb_cm(0.145) == 14);
	assert(stats_beta_horizontal_cm(0.265, 0.0) == 26);
	assert(stats_beta_climb_cm(0.265) == 27);

	/* fall(f): `if(f >= 2.0F)`, then round((double)f * 100D). The boundary is
	 * INCLUSIVE, and one ulp below it yields nothing at all. */
	assert(stats_beta_fall_cm(2.0F) == 200);
	assert(stats_beta_fall_cm(1.9999999F) == 0);
	assert(stats_beta_fall_cm(0.0F) == 0);
	assert(stats_beta_fall_cm(3.5F) == 350);
	assert(stats_beta_fall_cm(-5.0F) == 0);

	/* The two emit gates. flyOneCm is the only arm with a threshold: `l > 25`
	 * [addMovementStat], so 25 cm of hop is not flight and 26 is. */
	assert(!stats_beta_fly_counts(25));
	assert(stats_beta_fly_counts(26));
	assert(!stats_beta_fly_counts(0));
	assert(!stats_beta_walk_counts(0));
	assert(stats_beta_walk_counts(1));

	/* ---- TOTALITY: Java's conversions, not C's casts (R2 LOW 10) --------
	 *
	 * `(int)floorf(x)` is UNDEFINED in C for a NaN or an out-of-range value;
	 * Java's conversions are defined for every input and Beta's numbers are
	 * Java's. These cases pin the three JLS 5.1.3 rules through the two public
	 * chains, including the place where the chains legitimately DISAGREE.
	 *
	 * NaN -> 0 (both chains; Math.round documents it, and the conversion
	 * itself specifies it). */
	assert(stats_beta_horizontal_cm((double)NAN, 0.0) == 0);
	assert(stats_beta_3d_cm((double)NAN, 0.0, 0.0) == 0);
	assert(stats_beta_climb_cm((double)NAN) == 0);
	/* The fall gate rejects a NaN before the arithmetic: `!(f >= 2.0F)`. */
	assert(stats_beta_fall_cm((float)NAN) == 0);

	/* +/- infinity. Math.round(float) RETURNS AN INT, so the float chain
	 * saturates: (int)+inf == Integer.MAX_VALUE. (dx*dx is +inf for either
	 * sign, so the horizontal chain cannot produce -inf; the climb chain can.)
	 */
	assert(stats_beta_horizontal_cm((double)INFINITY, 0.0) == INT32_MAX);
	assert(stats_beta_horizontal_cm(-(double)INFINITY, 0.0) == INT32_MAX);
	assert(stats_beta_3d_cm(0.0, (double)INFINITY, 0.0) == INT32_MAX);
	/* Math.round(double) returns a LONG and Beta narrows it with its own
	 * `(int)`, which DISCARDS the high bits rather than saturating:
	 *   (int)Math.round(+inf) = (int)Long.MAX_VALUE = -1
	 *   (int)Math.round(-inf) = (int)Long.MIN_VALUE =  0
	 * Reproduced as written -- the decompile is the spec, and "fixing" it to
	 * saturate would be a number Beta never produces. */
	assert(stats_beta_climb_cm((double)INFINITY) == -1);
	assert(stats_beta_climb_cm(-(double)INFINITY) == 0);

	/* 3e9 centimetres: past int, inside long. The same two rules, on a finite
	 * value, so the divergence is not an infinity artefact.
	 *   float chain : Math.round(3e9f) saturates  -> 2147483647
	 *   double chain: (int)3000000000L wraps      -> -1294967296 */
	assert(stats_beta_horizontal_cm(3.0e7, 0.0) == INT32_MAX);
	assert(stats_beta_climb_cm(3.0e7) == -1294967296);
	assert(stats_beta_fall_cm(3.0e7F) == -1294967296);
	/* And the mirror below zero, for the float chain -- the climb chain is the
	 * only one that can take a negative, and Beta gates it on dy > 0. */
	assert(stats_beta_climb_cm(-3.0e7) == 1294967296);

	puts("PASS Beta's three rounding chains, both emit gates, and Java's "
		 "NaN/infinity/out-of-range conversions (no UB)");
}

/* --------------------------------------------------- the per-id arrays */

/* ItemStack.onCrafting: `craftItem[id] += stackSize`, not += 1. */
static void test_craft_counts_the_stack(void) {
	struct game_event ev;
	fresh(ACH_MODE_LOCAL_WORLD);

	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_CRAFT_TAKEN;
	ev.authority = GE_AUTH_SERVER;
	ev.who.generation = s_gen;
	ev.u.item.id = 5u; /* planks, four to a log */
	ev.u.item.count = 4u;
	game_event_emit(&ev);
	ev.u.item.count = 4u;
	game_event_emit(&ev);
	achievements_tick();

	assert(p0()->craft[5].local == 8u);
	/* The furnace take is the same `+= stackSize` through
	 * SlotFurnace.onPickupFromSlot. */
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_FURNACE_TAKEN;
	ev.authority = GE_AUTH_SERVER;
	ev.who.generation = s_gen;
	ev.u.item.id = 20u; /* glass, smelted from sand */
	ev.u.item.count = 3u;
	game_event_emit(&ev);
	achievements_tick();
	assert(p0()->craft[20].local == 3u);

	achievements_unbind(0, s_gen);
	puts("PASS craftItem counts the stack size, not the click");
}

/* Block.<clinit> disableStats(): leaves (18) have no mineBlock counter at all,
 * even though BlockLeaves.harvestBlock calls addStat [design 2]. */
static void test_no_stat_harvest_counts_nothing(void) {
	struct game_event ev;
	fresh(ACH_MODE_LOCAL_WORLD);

	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_BLOCK_HARVESTED;
	ev.authority = GE_AUTH_SERVER;
	ev.who.generation = s_gen;
	ev.u.item.id = 18u;
	ev.u.item.count = 1u;
	game_event_emit(&ev);
	achievements_tick();

	assert(p0()->mine[18].local == 0u);
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(p0()->mine[b].local == 0u && p0()->mine[b].remote == 0u);
	{
		struct stat_cell mined, crafted, used;
		assert(stats_block_row(0, 18u, &mined, &crafted, &used));
		assert(mined.total == 0u);
	}
	achievements_unbind(0, s_gen);
	puts("PASS a disableStats() harvest reaches no counter");
}

/* Design 2: the counters saturate at UINT32_MAX instead of wrapping like
 * Beta's Java int, and the displayed total is computed in 64-bit. Driven
 * through the real Packet 200 path so the clamp in add_u64() is under test
 * too, not just stats_counter_add(). */
static void test_saturation_in_a_per_id_array(void) {
	struct game_event ev;
	fresh(ACH_MODE_REMOTE_SERVER);

	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_REMOTE_STAT;
	ev.authority = GE_AUTH_PACKET200;
	ev.who.generation = s_gen;
	ev.u.remote.stat_id = (int32_t)(CW_STAT_MINE_BASE + 3u); /* dirt */
	ev.u.remote.amount = INT32_MAX;
	for(int i = 0; i < 3; i++)
		game_event_emit(&ev);
	achievements_tick();

	assert(p0()->mine[3].remote == UINT32_MAX);
	assert(p0()->mine[3].local == 0u);
	assert(stats_counter_total(&p0()->mine[3]) == UINT32_MAX);
	{
		/* One more must neither wrap nor move it. */
		game_event_emit(&ev);
		achievements_tick();
		assert(p0()->mine[3].remote == UINT32_MAX);
	}
	achievements_unbind(0, s_gen);
	puts("PASS a per-id counter saturates at UINT32_MAX and never wraps");
}

/* ----------------------------------------------- the four-array alias table */

#define ALIAS_GOLDEN "tests/golden/achievements/b3_alias_table.txt"

static uint16_t parse_cell(const char* tok) {
	if(tok[0] == '-' && tok[1] == '\0')
		return CW_STAT_BLOCK_NONE;
	return (uint16_t)atoi(tok);
}

/* Every cell of the golden, against the module. The golden's header carries
 * the decompile derivation (StatList.replaceAllSimilarBlocks plus each
 * array's registration loop); this only has to prove the code agrees with
 * it, for all four arrays and all 256 ids. */
static void test_alias_table_matches_the_golden(void) {
	FILE* f = fopen(ALIAS_GOLDEN, "rb");
	char line[256];
	int rows = 0;
	bool seen[CW_STAT_BLOCK_COUNT];

	if(!f) {
		fprintf(stderr, "FAIL cannot open %s (working dir must be the repo "
						"root -- CMake sets WORKING_DIRECTORY)\n",
				ALIAS_GOLDEN);
		assert(0);
	}
	memset(seen, 0, sizeof(seen));

	while(fgets(line, sizeof(line), f)) {
		char c_id[16], c_mine[16], c_craft[16], c_use[16], c_brk[16];
		if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		if(sscanf(line, "%15s %15s %15s %15s %15s", c_id, c_mine, c_craft,
				  c_use, c_brk)
		   != 5) {
			fprintf(stderr, "FAIL malformed golden line: %s", line);
			assert(0);
		}
		{
			int id = atoi(c_id);
			uint16_t want_mine = parse_cell(c_mine);
			uint16_t want_craft = parse_cell(c_craft);
			uint16_t want_use = parse_cell(c_use);
			uint16_t want_brk = parse_cell(c_brk);
			uint16_t got_mine = stat_canonical_block((uint16_t)id);
			uint16_t got_craft
				= stat_alias_block_in(CW_STAT_ARRAY_CRAFT, (uint16_t)id);
			uint16_t got_use
				= stat_alias_block_in(CW_STAT_ARRAY_USE, (uint16_t)id);
			uint16_t got_brk
				= stat_alias_block_in(CW_STAT_ARRAY_BREAK, (uint16_t)id);
			assert(id >= 0 && id < CW_STAT_BLOCK_COUNT);
			assert(!seen[id]);
			seen[id] = true;
			if(got_mine != want_mine || got_craft != want_craft
			   || got_use != want_use || got_brk != want_brk) {
				fprintf(stderr,
						"FAIL alias id %d: got %u/%u/%u/%u, golden %u/%u/%u/%u"
						" (mine/craft/use/break)\n",
						id, got_mine, got_craft, got_use, got_brk, want_mine,
						want_craft, want_use, want_brk);
				assert(0);
			}
			rows++;
		}
	}
	fclose(f);
	assert(rows == CW_STAT_BLOCK_COUNT);
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(seen[b]);
	/* stat_alias_block() is the useItem direction by definition. */
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(stat_alias_block((uint16_t)b)
			   == stat_alias_block_in(CW_STAT_ARRAY_USE, (uint16_t)b));
	puts("PASS the four-array alias table matches its Beta-derived golden");
}

#define ITEM_GOLDEN "tests/golden/achievements/b4_item_registration.txt"

/* Every ITEM id (>= 256), against the hand-derived registration golden
 * (R2 MED 8). The golden's header carries the decompile derivation --
 * StatList.func_25157_c's recipe/smelting HashSet for craftItem and
 * func_25149_b's isDamagable() for breakItem; this proves the code agrees
 * with it for all three item arrays and every id in 256..ITEMS_MAX-1, in both
 * directions: registered ids keep their own counter, and EVERY other id has
 * none. It also pins the two X-macro tables in stats_table.h against the same
 * file, so a row added to the header without a golden line (or the reverse)
 * fails here rather than silently changing what counts. */
static void test_item_registration_matches_the_golden(void) {
	static bool want_craft[ITEMS_MAX];
	static bool want_break[ITEMS_MAX];
	FILE* f = fopen(ITEM_GOLDEN, "rb");
	char line[256];
	int craft_rows = 0, break_rows = 0;
	int table_craft = 0, table_break = 0;

	if(!f) {
		fprintf(stderr, "FAIL cannot open %s (working dir must be the repo "
						"root -- CMake sets WORKING_DIRECTORY)\n",
				ITEM_GOLDEN);
		assert(0);
	}
	memset(want_craft, 0, sizeof(want_craft));
	memset(want_break, 0, sizeof(want_break));
	while(fgets(line, sizeof(line), f)) {
		char tag[16];
		int id = -1;
		if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		if(sscanf(line, "%15s %d", tag, &id) != 2) {
			fprintf(stderr, "FAIL malformed item golden line: %s", line);
			assert(0);
		}
		assert(id >= CW_ITEM_FIRST && id < ITEMS_MAX);
		if(strcmp(tag, "craft") == 0) {
			assert(!want_craft[id]); /* no duplicate rows */
			want_craft[id] = true;
			craft_rows++;
		} else if(strcmp(tag, "break") == 0) {
			assert(!want_break[id]);
			want_break[id] = true;
			break_rows++;
		} else {
			fprintf(stderr, "FAIL unknown item golden section: %s", tag);
			assert(0);
		}
	}
	fclose(f);
	assert(craft_rows == 82 && break_rows == 48);

	for(int id = CW_ITEM_FIRST; id < ITEMS_MAX; id++) {
		uint16_t got_craft
			= stat_alias_block_in(CW_STAT_ARRAY_CRAFT, (uint16_t)id);
		uint16_t got_use
			= stat_alias_block_in(CW_STAT_ARRAY_USE, (uint16_t)id);
		uint16_t got_brk
			= stat_alias_block_in(CW_STAT_ARRAY_BREAK, (uint16_t)id);
		uint16_t want_c
			= want_craft[id] ? (uint16_t)id : (uint16_t)CW_STAT_BLOCK_NONE;
		uint16_t want_b
			= want_break[id] ? (uint16_t)id : (uint16_t)CW_STAT_BLOCK_NONE;
		if(got_craft != want_c || got_brk != want_b
		   || got_use != (uint16_t)id) {
			fprintf(stderr,
					"FAIL item id %d: got craft %u use %u break %u, golden "
					"craft %u use %u break %u\n",
					id, got_craft, got_use, got_brk, want_c, (unsigned)id,
					want_b);
			assert(0);
		}
		/* An item never folds: no replaceSimilarBlocks row names one. */
		assert(got_craft == CW_STAT_BLOCK_NONE || got_craft == (uint16_t)id);
		assert(got_brk == CW_STAT_BLOCK_NONE || got_brk == (uint16_t)id);
	}

	/* The X-macro tables say exactly what the golden says, id for id. */
#define CW_ITEM_CRAFT_OUTPUT(i)                                                \
	table_craft++;                                                             \
	assert(want_craft[i]);
	CW_ITEM_CRAFT_OUTPUT_ROWS
#undef CW_ITEM_CRAFT_OUTPUT
#define CW_ITEM_DAMAGABLE(i)                                                   \
	table_break++;                                                             \
	assert(want_break[i]);
	CW_ITEM_DAMAGABLE_ROWS
#undef CW_ITEM_DAMAGABLE
	assert(table_craft == CW_ITEM_CRAFT_OUTPUT_COUNT && table_craft == 82);
	assert(table_break == CW_ITEM_DAMAGABLE_COUNT && table_break == 48);

	/* The named cases the finding calls out, spelled so a diff names them:
	 * an apple is not a recipe output and a snowball is not damagable, so
	 * neither has a craft or a break counter -- both used to. */
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 260)  /* apple      */
		   == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, 260) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 332)  /* snowball   */
		   == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, 332) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 261) == 261); /* bow    */
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, 261)          /* NOT    */
		   == CW_STAT_BLOCK_NONE);            /* damagable in b1.7.3        */
	assert(stat_alias_block_in(CW_STAT_ARRAY_USE, 260) == 260);   /* always */
	puts("PASS every item id matches the hand-derived registration golden "
		 "(82 craft outputs, 48 damagable, useItem ungated)");
}

/* A craft inversion changes a pair's DIRECTION; it may never invent a pair
 * the alias table does not have. The preprocessor cannot cross the two row
 * sets, so the test does. */
static void test_craft_inversions_name_real_alias_pairs(void) {
	int alias_pairs = 0;
	int inverted = 0;
	bool ok_91_86 = false, ok_76_75 = false;
#define CW_BLOCK_ALIAS(f, t)                                                   \
	alias_pairs++;                                                             \
	if((f) == 91 && (t) == 86)                                                 \
		ok_91_86 = true;                                                       \
	if((f) == 76 && (t) == 75)                                                 \
		ok_76_75 = true;
	CW_BLOCK_ALIAS_ROWS
#undef CW_BLOCK_ALIAS
#define CW_BLOCK_CRAFT_INVERT(f, t)                                            \
	inverted++;                                                                \
	assert(((f) == 91 && (t) == 86) || ((f) == 76 && (t) == 75));
	CW_BLOCK_CRAFT_INVERT_ROWS
#undef CW_BLOCK_CRAFT_INVERT

	assert(alias_pairs == CW_BLOCK_ALIAS_COUNT && alias_pairs == 11);
	assert(inverted == CW_BLOCK_CRAFT_INVERT_COUNT && inverted == 2);
	assert(ok_91_86 && ok_76_75);
	puts("PASS both craft inversions name real alias pairs");
}

/* ------------------------------------------------ the registration gates (F3)
 *
 * A per-id counter exists in an array exactly where Beta's registration loop
 * for THAT array made a StatBase. The golden above pins all 1024 cells; these
 * cases pin the three gates as RULES, so a wrong cell fails with the name of
 * the gate it broke rather than as one anonymous diff line. */
static void test_gate_craft_is_recipe_outputs_only(void) {
	int registered = 0;
	int with_counter = 0;

	/* (a) StatList.func_25157_c registers the HashSet of CraftingManager
	 * recipe outputs and FurnaceRecipes smelting outputs, and nothing else:
	 * every output is its own counter unless a fold moved it. */
#define CW_BLOCK_CRAFT_OUTPUT(b)                                               \
	registered++;                                                              \
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, (b))                       \
		   != CW_STAT_BLOCK_NONE);
	CW_BLOCK_CRAFT_OUTPUT_ROWS
#undef CW_BLOCK_CRAFT_OUTPUT
	assert(registered == CW_BLOCK_CRAFT_OUTPUT_COUNT && registered == 39);

	/* A block nobody can craft has NO craftItem counter -- not a folded one,
	 * not one of its own. Dirt, grass, farmland, cobblestone, a wooden door,
	 * bedrock, a mob spawner. */
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 3) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 2) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 60) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 4) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 64) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 7) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 52) == CW_STAT_BLOCK_NONE);

	/* The four ids an alias row REACHES even though they register nothing:
	 * the two inverting pairs, and the two whose target is the output. */
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 86) == 91);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 75) == 76);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 62) == 61);
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, 43) == 44);

	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		if(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, (uint16_t)b)
		   != CW_STAT_BLOCK_NONE)
			with_counter++;
	assert(with_counter == 43); /* the 39 outputs + 86, 75, 62, 43 */

	/* The two inversions CW_BLOCK_CRAFT_INVERT_ROWS documents are now DERIVED
	 * from the output set rather than hard-coded, so the rows are an
	 * expectation this pins: both sides of an inverted pair answer the
	 * SOURCE's id, and no other pair inverts. */
#define CW_BLOCK_CRAFT_INVERT(from, to)                                        \
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, (from)) == (from));        \
	assert(stat_alias_block_in(CW_STAT_ARRAY_CRAFT, (to)) == (from));
	CW_BLOCK_CRAFT_INVERT_ROWS
#undef CW_BLOCK_CRAFT_INVERT
	puts("PASS gate (a): craftItem exists for the 39 recipe/smelting outputs "
		 "and the four ids a fold reaches");
}

static void test_gate_break_has_no_block_counter(void) {
	/* (b) func_25149_b registers only `Item.itemsList[l].isDamagable()`
	 * (StatList.java:134) and isDamagable() is `maxDamage > 0 && !hasSubtypes`
	 * (Item.java:115-118). No block item sets a maxDamage, so the whole
	 * breakItem column of 0..255 is "no counter". */
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, (uint16_t)b)
			   == CW_STAT_BLOCK_NONE);
	/* A DAMAGABLE ITEM keeps its counter, which is what makes this a gate and
	 * not a switch-off -- and an item that is not damagable does not (R2 MED
	 * 8: item ids used to be waved through, so the top of the range answered
	 * with itself no matter what it was). */
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, 270) == 270); /* pickaxe */
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, 359) == 359); /* shears  */
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, 280)          /* stick   */
		   == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_BREAK, ITEMS_MAX - 1)
		   == CW_STAT_BLOCK_NONE);
	puts("PASS gate (b): no block id has a breakItem counter, damagable items "
		 "keep theirs and nothing else does");
}

static void test_gate_no_beta_block_no_statistic(void) {
	static const enum cw_stat_array all[] = {
		CW_STAT_ARRAY_MINE, CW_STAT_ARRAY_CRAFT, CW_STAT_ARRAY_USE,
		CW_STAT_ARRAY_BREAK};
	/* (c) All four loops need a Beta block at the id (Block.blocksList /
	 * Item.itemsList), populated for 1..96 only. */
	for(size_t k = 0; k < sizeof(all) / sizeof(all[0]); k++) {
		assert(stat_alias_block_in(all[k], 0) == CW_STAT_BLOCK_NONE); /* air */
		for(int b = CW_BLOCK_BETA_LAST + 1; b < CW_STAT_BLOCK_COUNT; b++)
			assert(stat_alias_block_in(all[k], (uint16_t)b)
				   == CW_STAT_BLOCK_NONE);
	}
	assert(stat_canonical_block(0) == CW_STAT_BLOCK_NONE);
	/* The two CaveWii-only registrations the lead ruling names. */
	assert(stat_canonical_block(97) == CW_STAT_BLOCK_NONE);	 /* tree2d */
	assert(stat_canonical_block(200) == CW_STAT_BLOCK_NONE); /* iron chest */
	assert(stat_alias_block_in(CW_STAT_ARRAY_USE, 97) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_USE, 200) == CW_STAT_BLOCK_NONE);
	/* Inside the range useItem registers every id, gate or no gate. */
	for(int b = CW_BLOCK_BETA_FIRST; b <= CW_BLOCK_BETA_LAST; b++)
		assert(stat_alias_block_in(CW_STAT_ARRAY_USE, (uint16_t)b)
			   != CW_STAT_BLOCK_NONE);
	puts("PASS gate (c): an id with no Beta block has no statistic in any of "
		 "the four arrays");
}

/* The three gates on the real event path: a gated event applies nothing and
 * is not counted as accepted, and a Packet 200 for a gated id is an unknown
 * stat rather than a write. */
static void test_gates_on_the_event_path(void) {
	struct game_event ev;
	struct ach_status before, after;

	fresh(ACH_MODE_LOCAL_WORLD);
	before = status0();
	memset(&ev, 0, sizeof(ev));
	ev.authority = GE_AUTH_SERVER;
	ev.who.generation = s_gen;
	ev.u.item.count = 1u;
#define EMIT(k, what)                                                          \
	do {                                                                       \
		ev.kind = (k);                                                         \
		ev.u.item.id = (uint16_t)(what);                                       \
		game_event_emit(&ev);                                                  \
	} while(0)
	EMIT(GE_CRAFT_TAKEN, 3);	  /* (a) dirt is not a recipe output */
	EMIT(GE_CRAFT_TAKEN, 4);	  /* (a) nor is cobblestone */
	EMIT(GE_ITEM_BROKEN, 61);	  /* (b) a block is never damagable */
	EMIT(GE_ITEM_BROKEN, 44);	  /* (b) not even a craftable one */
	EMIT(GE_BLOCK_HARVESTED, 97); /* (c) CaveWii-only block */
	EMIT(GE_ITEM_USED, 97);
	EMIT(GE_CRAFT_TAKEN, 200);
	EMIT(GE_ITEM_USED, 200);
#undef EMIT
	achievements_tick();

	after = status0();
	assert(after.accepted == before.accepted); /* nothing landed */
	for(int r = 0; r < ACH_REJECT_COUNT; r++)
		assert(after.rejected[r] == before.rejected[r]); /* nor was rejected */
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(p0()->mine[b].local == 0u && p0()->craft[b].local == 0u
			   && p0()->use[b].local == 0u && p0()->brk[b].local == 0u);
	achievements_unbind(0, s_gen);

	/* The Packet 200 side: a remote counter for a gated id is an id this
	 * build has no counter for, exactly as a disableStats() mineBlock id is,
	 * so it is REJECTED as an unknown stat and writes nothing. */
	fresh(ACH_MODE_REMOTE_SERVER);
	before = status0();
	memset(&ev, 0, sizeof(ev));
	ev.kind = GE_REMOTE_STAT;
	ev.authority = GE_AUTH_PACKET200;
	ev.who.generation = s_gen;
	ev.u.remote.amount = 5;
	ev.u.remote.stat_id = (int32_t)(CW_STAT_CRAFT_BASE + 3u); /* dirt */
	game_event_emit(&ev);
	ev.u.remote.stat_id = (int32_t)(CW_STAT_BREAK_BASE + 44u); /* a block */
	game_event_emit(&ev);
	ev.u.remote.stat_id = (int32_t)(CW_STAT_USE_BASE + 200u); /* CaveWii-only */
	game_event_emit(&ev);
	achievements_tick();
	after = status0();
	assert(after.accepted == before.accepted);
	assert(after.rejected[ACH_REJECT_UNKNOWN_STAT]
		   == before.rejected[ACH_REJECT_UNKNOWN_STAT] + 3u);
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(p0()->craft[b].remote == 0u && p0()->brk[b].remote == 0u
			   && p0()->use[b].remote == 0u);
	/* An UNgated id in the same arrays still arrives, so the rejection above
	 * is the gate and not a dead Packet 200 path. */
	ev.u.remote.stat_id = (int32_t)(CW_STAT_CRAFT_BASE + 61u); /* furnace */
	game_event_emit(&ev);
	achievements_tick();
	assert(p0()->craft[61].remote == 5u);
	achievements_unbind(0, s_gen);
	puts("PASS the three gates drop the event and the Packet 200 without "
		 "counting either as accepted");
}

/* The named goldens design 9 asks for by name, end to end: the counter is
 * written through the fold AND read back through the B4 screen rows, so a
 * pumpkin row and a jack-o-lantern row always show the one shared count. */
static void test_named_alias_pairs_end_to_end(void) {
	struct game_event ev;
	struct stat_cell mined, crafted, used;

	fresh(ACH_MODE_LOCAL_WORLD);
	memset(&ev, 0, sizeof(ev));
	ev.authority = GE_AUTH_SERVER;
	ev.who.generation = s_gen;
	ev.u.item.count = 1u;
#define HARVEST(what)                                                          \
	do {                                                                       \
		ev.kind = GE_BLOCK_HARVESTED;                                          \
		ev.u.item.id = (uint16_t)(what);                                       \
		game_event_emit(&ev);                                                  \
	} while(0)
#define CRAFT(what)                                                            \
	do {                                                                       \
		ev.kind = GE_CRAFT_TAKEN;                                              \
		ev.u.item.id = (uint16_t)(what);                                       \
		game_event_emit(&ev);                                                  \
	} while(0)
#define USE(what)                                                              \
	do {                                                                       \
		ev.kind = GE_ITEM_USED;                                                \
		ev.u.item.id = (uint16_t)(what);                                       \
		game_event_emit(&ev);                                                  \
	} while(0)

	/* grass / farmland / dirt -> dirt's mineBlock [replaceSimilarBlocks
	 * (a, grass, dirt) and (a, tilledField, dirt)]. */
	HARVEST(2);
	HARVEST(60);
	HARVEST(3);
	/* lit furnace -> furnace; glowing redstone ore -> redstone ore; active
	 * redstone torch -> idle; red mushroom -> brown; double slab -> slab. */
	HARVEST(62);
	HARVEST(74);
	HARVEST(76);
	HARVEST(40);
	HARVEST(43);
	/* The two disableStats() pairs: water and the repeaters mine nothing. */
	HARVEST(8);
	HARVEST(9);
	HARVEST(93);
	HARVEST(94);
	/* The craftItem inversions: the recipe output is the lantern and the
	 * ACTIVE torch, and Beta's counter for the whole pair is that id. */
	CRAFT(91);
	CRAFT(76);
	USE(9); /* still water folds onto MOVING water in useItem */
	achievements_tick();

	assert(p0()->mine[3].local == 3u);
	assert(p0()->mine[2].local == 0u && p0()->mine[60].local == 0u);
	assert(p0()->mine[61].local == 1u && p0()->mine[62].local == 0u);
	assert(p0()->mine[73].local == 1u && p0()->mine[74].local == 0u);
	assert(p0()->mine[75].local == 1u && p0()->mine[76].local == 0u);
	assert(p0()->mine[39].local == 1u && p0()->mine[40].local == 0u);
	assert(p0()->mine[44].local == 1u && p0()->mine[43].local == 0u);
	assert(p0()->mine[8].local == 0u && p0()->mine[9].local == 0u);
	assert(p0()->mine[93].local == 0u && p0()->mine[94].local == 0u);
	/* The two INVERTED craft pairs: the counter is the recipe output's id, so
	 * the pumpkin (86) and the idle torch (75) hold nothing of their own. */
	assert(p0()->craft[91].local == 1u && p0()->craft[86].local == 0u);
	assert(p0()->craft[76].local == 1u && p0()->craft[75].local == 0u);
	assert(p0()->use[8].local == 1u && p0()->use[9].local == 0u);

	/* Read back through the screen rows: both sides of a pair show the shared
	 * counter, whichever id the count was stored under. */
	assert(stats_block_row(0, 2u, &mined, &crafted, &used) && mined.total == 3u);
	assert(stats_block_row(0, 60u, &mined, &crafted, &used)
		   && mined.total == 3u);
	assert(stats_block_row(0, 86u, &mined, &crafted, &used)
		   && crafted.total == 1u);
	assert(stats_block_row(0, 91u, &mined, &crafted, &used)
		   && crafted.total == 1u);
	assert(stats_block_row(0, 75u, &mined, &crafted, &used)
		   && crafted.total == 1u);
	assert(stats_block_row(0, 76u, &mined, &crafted, &used)
		   && crafted.total == 1u);
	assert(stats_block_row(0, 9u, &mined, &crafted, &used) && used.total == 1u);
	assert(stats_block_row(0, 8u, &mined, &crafted, &used) && used.total == 1u);

#undef HARVEST
#undef CRAFT
#undef USE
	achievements_unbind(0, s_gen);
	puts("PASS the named alias pairs share one counter, written and read");
}

int main(void) {
	test_fixture_dir("test_stats_counters_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);

	test_beta_rounding();
	test_alias_table_matches_the_golden();
	test_item_registration_matches_the_golden();
	test_craft_inversions_name_real_alias_pairs();
	test_every_general_stat();
	test_columns_follow_the_bind_mode();
	test_craft_counts_the_stack();
	test_no_stat_harvest_counts_nothing();
	test_saturation_in_a_per_id_array();
	test_gate_craft_is_recipe_outputs_only();
	test_gate_break_has_no_block_counter();
	test_gate_no_beta_block_no_statistic();
	test_gates_on_the_event_path();
	test_named_alias_pairs_end_to_end();

	achievements_shutdown();
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_stats_counters");
	return 0;
}
