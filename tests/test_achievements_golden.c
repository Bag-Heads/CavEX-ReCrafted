/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_achievements_golden`:
 * "fixed event script + injected clock -> byte-exact `.cwstats` file".
 *
 * B1 has no event layer yet (that is B2), so the script here is the fixed
 * COUNTER fill the brief specifies, applied through the same
 * stat_canonical_block() fold a real mine event will use. Grass, farmland and
 * dirt all land on the dirt counter, the lit and unlit furnace share one, and
 * leaves (a Block.disableStats() block) produce no counter at all -- so the
 * golden shows only canonical ids, which is exactly what makes the fold
 * visible in a file diff [design 2].
 *
 * tests/golden/achievements/b1_golden.cwstats was written from the FORMAT
 * SPEC (design 4) rather than dumped from this code: its record set, its
 * ascending id order and its crc32 were composed independently, so a
 * regression in the writer shows up as a diff instead of being blessed.
 * Run from the repo root (the ctest WORKING_DIRECTORY property).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "support/test_fixture_dir.h"

#include "game/achievements/achievements_persist.h"
#include "game/achievements/achievements_state.h"

#define GOLDEN_PATH "tests/golden/achievements/b1_golden.cwstats"
#define PROFILE_ID "golden"

static char s_dir[128];
static struct stats_profile g_p;
static char g_written[4096];
static char g_golden[4096];

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

/* The `saved` stamp of design 4's own example, so the golden is stable. */
static uint32_t fixed_clock(void) {
	return 1789000000u;
}

static long read_all(const char* path, char* buf, size_t cap) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return -1;
	size_t n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = '\0';
	return (long)n;
}

/* Count a harvested block exactly the way the mine producer will: through the
 * canonical fold, and not at all when the block has no counter. */
static void mine(struct stats_profile* p, uint16_t block, uint32_t n) {
	uint16_t id = stat_canonical_block(block);
	if(id == CW_STAT_BLOCK_NONE)
		return;
	stats_counter_add(&p->mine[id], STATS_COLUMN_LOCAL, n);
}

static void add_general(struct stats_profile* p, uint32_t stat_id,
						enum stats_column col, uint32_t n) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	stats_counter_add(&p->general[i], col, n);
}

static void fill_fixed(struct stats_profile* p) {
	stats_profile_reset(p, PROFILE_ID);

	add_general(p, 1000, STATS_COLUMN_LOCAL, 3u);	  /* startGame */
	add_general(p, 1100, STATS_COLUMN_LOCAL, 36000u); /* playOneMinute */
	add_general(p, 2000, STATS_COLUMN_LOCAL, 123456u);
	add_general(p, 2000, STATS_COLUMN_REMOTE, 4321u); /* walkOneCm, both */
	add_general(p, 2022, STATS_COLUMN_REMOTE, 2u);	  /* deaths (server-owned) */

	stats_counter_add(&p->ach[0], STATS_COLUMN_LOCAL, 1u); /* openInventory */
	p->ach_first[0] = (uint8_t)STATS_FIRST_LOCAL;
	stats_counter_add(&p->ach[1], STATS_COLUMN_LOCAL, 1u); /* mineWood */
	p->ach_first[1] = (uint8_t)STATS_FIRST_LOCAL;
	stats_counter_add(&p->ach[11], STATS_COLUMN_REMOTE, 1u); /* onARail */
	p->ach_first[11] = (uint8_t)STATS_FIRST_REMOTE;

	mine(p, 1, 2u);	 /* stone: its own id */
	mine(p, 2, 5u);	 /* grass    -> dirt */
	mine(p, 3, 7u);	 /* dirt */
	mine(p, 60, 2u); /* farmland -> dirt, so dirt totals 14 */
	mine(p, 62, 3u); /* lit furnace -> furnace */
	mine(p, 61, 1u); /* furnace, so furnace totals 4 */
	mine(p, 18, 9u); /* leaves: disableStats(), no counter at all */
	mine(p, 8, 4u);	 /* water: aliased to still water, also disabled */

	stats_counter_add(&p->craft[58], STATS_COLUMN_LOCAL, 1u);  /* workbench */
	stats_counter_add(&p->craft[297], STATS_COLUMN_LOCAL, 4u); /* bread */
	stats_counter_add(&p->use[270], STATS_COLUMN_LOCAL, 12u); /* wood pickaxe */
	stats_counter_add(&p->brk[270], STATS_COLUMN_REMOTE, 1u);
}

int main(void) {
	test_fixture_dir("test_achievements_golden_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);
	stats_set_clock(fixed_clock);

	fill_fixed(&g_p);
	/* The fold landed where it should before a single byte is written. */
	assert(g_p.mine[3].local == 14u);  /* grass + dirt + farmland */
	assert(g_p.mine[61].local == 4u);  /* lit + unlit furnace */
	assert(g_p.mine[2].local == 0u);   /* grass has no counter of its own */
	assert(g_p.mine[60].local == 0u);  /* nor farmland */
	assert(g_p.mine[62].local == 0u);  /* nor the lit furnace */
	assert(g_p.mine[18].local == 0u);  /* leaves are never counted */
	assert(g_p.mine[8].local == 0u && g_p.mine[9].local == 0u);

	assert(stats_profile_save(s_dir, &g_p) == STATS_SAVE_OK);

	char path[CW_STATS_PATH_MAX];
	assert(stats_profile_path(path, sizeof(path), s_dir, PROFILE_ID, ""));
	long n = read_all(path, g_written, sizeof(g_written));
	long g = read_all(GOLDEN_PATH, g_golden, sizeof(g_golden));
	if(g < 0) {
		fprintf(stderr,
				"FAIL cannot open %s -- run this test from the repo root\n",
				GOLDEN_PATH);
		return 1;
	}
	if(n != g || memcmp(g_written, g_golden, (size_t)g) != 0) {
		fprintf(stderr, "FAIL golden mismatch (%ld written, %ld golden)\n", n,
				g);
		fprintf(stderr, "--- written ---\n%s--- golden ---\n%s", g_written,
				g_golden);
		return 1;
	}
	puts("PASS written .cwstats is byte-exact against the committed golden");

	/* Positive proof in the FILE, not only in memory: no aliased-away id and
	 * no disabled-block id ever appears. */
	assert(strstr(g_written, "s 16777218 ") == NULL); /* grass */
	assert(strstr(g_written, "s 16777276 ") == NULL); /* farmland */
	assert(strstr(g_written, "s 16777278 ") == NULL); /* lit furnace */
	assert(strstr(g_written, "s 16777234 ") == NULL); /* leaves */
	assert(strstr(g_written, "s 16777224 ") == NULL); /* water */
	assert(strstr(g_written, "s 16777225 ") == NULL); /* still water */
	assert(strstr(g_written, "s 16777219 14 0\n") != NULL); /* dirt */
	assert(strstr(g_written, "s 16777277 4 0\n") != NULL);	/* furnace */
	puts("PASS the file holds only canonical ids (grass/farmland/lit furnace folded away)");

	/* The golden also reads back: the format is a round trip, not just an
	 * output shape. */
	static struct stats_profile back;
	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, PROFILE_ID, &back, &st);
	assert(st == STATS_LOAD_OK);
	assert(back.mine[3].local == 14u && back.mine[61].local == 4u);
	assert(back.general[stat_general_index(2000)].local == 123456u);
	assert(back.general[stat_general_index(2000)].remote == 4321u);
	assert(stats_counter_total(&back.general[stat_general_index(2000)])
		   == 127777u);
	assert(back.ach_first[11] == (uint8_t)STATS_FIRST_REMOTE);
	assert(ach_unlocked(&back, 11));
	puts("PASS the golden file loads back to the same state");

	stats_set_clock(NULL);
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_golden");
	return 0;
}
