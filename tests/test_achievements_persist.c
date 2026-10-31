/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_achievements_persist`:
 *
 *   write -> reload -> identical state; reload never re-queues a toast; bad
 *   crc / short file / identity mismatch -> moved to `.bad`, `.bak` used,
 *   status flag, no crash; newer version -> untouched, empty state, writes
 *   refused; fault injection at every write stage leaves the previous good
 *   file or the new one and keeps the state dirty; saturation at UINT32_MAX
 *   and 64-bit totals; unknown record letter skipped
 *
 * Every scenario uses its OWN profile id inside one per-process fixture
 * directory, so a quarantined `.bad` from one case can never change the
 * outcome of the next (that is itself one of the behaviours under test).
 *
 * "Keeps the state dirty" is asserted as "the writer does not touch the
 * caller's profile at all": the dirty flag lives with the caller (B2's
 * lifecycle), and a save that cannot modify the state cannot clear it.
 * Likewise "never re-queues a toast" is structural here -- B1 has no notice
 * state and no 0->1 transition hook, so a load can only produce counters; the
 * assertion is that unlock is read back purely as `count > 0`.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "support/test_fixture_dir.h"

#include "game/achievements/achievements_persist.h"
#include "game/achievements/achievements_state.h"

static char s_dir[128];

/* Three resident profiles (~57 KB each): static, never on the stack. */
static struct stats_profile g_src;
static struct stats_profile g_dst;
static struct stats_profile g_copy;

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

static uint32_t fixed_clock(void) {
	return 1789000000u;
}

static long read_file(const char* path, char* buf, size_t cap) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return -1;
	size_t n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = '\0';
	return (long)n;
}

static void write_file(const char* path, const char* data, size_t len) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, 1, len, f) == len);
	assert(fclose(f) == 0);
}

/* Compose a valid v1 file: `body` is every byte above the terminator. */
static void write_cwstats(const char* path, const char* body,
						  uint32_t records) {
	char out[8192];
	uint32_t crc = (uint32_t)crc32(0, (const Bytef*)body, (uInt)strlen(body));
	int n = snprintf(out, sizeof(out), "%send %lu %08lx\n", body,
					 (unsigned long)records, (unsigned long)crc);
	assert(n > 0 && (size_t)n < sizeof(out));
	write_file(path, out, (size_t)n);
}

static const char* path_of(const char* profile_id, const char* suffix) {
	static char bufs[4][CW_STATS_PATH_MAX];
	static int slot = -1;
	slot = (slot + 1) & 3;
	assert(stats_profile_path(bufs[slot], sizeof(bufs[slot]), s_dir, profile_id,
							  suffix));
	return bufs[slot];
}

static bool exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

/* Flip one digit of the committed file's crc: the cheapest corruption that is
 * indistinguishable from a torn write. */
static void corrupt_main(const char* id) {
	static char bytes[8192];
	long len = read_file(path_of(id, ""), bytes, sizeof(bytes));
	assert(len > 2);
	bytes[len - 2] = (bytes[len - 2] == 'a') ? 'b' : 'a';
	write_file(path_of(id, ""), bytes, (size_t)len);
}

static bool pair_eq(const struct stats_pair* a, const struct stats_pair* b) {
	return a->local == b->local && a->remote == b->remote;
}

static bool profiles_equal(const struct stats_profile* a,
						   const struct stats_profile* b) {
	if(strcmp(a->profile_id, b->profile_id) != 0)
		return false;
	for(int i = 0; i < CW_STAT_COUNT; i++)
		if(!pair_eq(&a->general[i], &b->general[i]))
			return false;
	for(int i = 0; i < CW_STAT_BLOCK_COUNT; i++)
		if(!pair_eq(&a->mine[i], &b->mine[i]))
			return false;
	for(int i = 0; i < ITEMS_MAX; i++)
		if(!pair_eq(&a->craft[i], &b->craft[i])
		   || !pair_eq(&a->use[i], &b->use[i])
		   || !pair_eq(&a->brk[i], &b->brk[i]))
			return false;
	for(int i = 0; i < CW_ACH_COUNT; i++)
		if(!pair_eq(&a->ach[i], &b->ach[i])
		   || a->ach_first[i] != b->ach_first[i])
			return false;
	return true;
}

static bool profile_is_empty(const struct stats_profile* p) {
	static struct stats_profile zero; /* zero-initialised by definition */
	stats_profile_reset(&zero, p->profile_id);
	return profiles_equal(p, &zero);
}

/* A deterministic mix that touches every array and both columns. */
static void fill(struct stats_profile* p, const char* id, uint32_t bump) {
	stats_profile_reset(p, id);
	int walk = stat_general_index(2000);
	int play = stat_general_index(1100);
	int deaths = stat_general_index(2022);
	assert(walk >= 0 && play >= 0 && deaths >= 0);
	stats_counter_add(&p->general[walk], STATS_COLUMN_LOCAL, 123456u + bump);
	stats_counter_add(&p->general[walk], STATS_COLUMN_REMOTE, 700u);
	stats_counter_add(&p->general[play], STATS_COLUMN_LOCAL, 24000u);
	stats_counter_add(&p->general[deaths], STATS_COLUMN_REMOTE, 3u);
	stats_counter_add(&p->mine[3], STATS_COLUMN_LOCAL, 40u + bump);
	stats_counter_add(&p->mine[17], STATS_COLUMN_LOCAL, 12u);
	stats_counter_add(&p->craft[58], STATS_COLUMN_LOCAL, 1u);
	stats_counter_add(&p->use[270], STATS_COLUMN_LOCAL, 9u);
	stats_counter_add(&p->brk[270], STATS_COLUMN_REMOTE, 2u);
	stats_counter_add(&p->ach[0], STATS_COLUMN_LOCAL, 5u);
	p->ach_first[0] = (uint8_t)STATS_FIRST_LOCAL;
	stats_counter_add(&p->ach[1], STATS_COLUMN_REMOTE, 1u);
	p->ach_first[1] = (uint8_t)STATS_FIRST_REMOTE;
}

/* ------------------------------------------------------------------ */

static void test_round_trip(void) {
	const char* id = "rt";
	fill(&g_src, id, 0);
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);

	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_OK);
	assert(profiles_equal(&g_src, &g_dst));
	assert(g_dst.progress_incomplete == 0 && g_dst.write_refused == 0);

	/* Unlock is read back as `count > 0` and nothing else: there is no
	 * transition state a reload could re-fire [design 4 last bullet]. */
	assert(ach_unlocked(&g_dst, 0) && ach_unlocked(&g_dst, 1));
	assert(!ach_unlocked(&g_dst, 2));
	assert(ach_can_unlock(&g_dst, 0)); /* no parent */
	assert(ach_can_unlock(&g_dst, 2)); /* parent 1 unlocked */
	assert(!ach_can_unlock(&g_dst, 3)); /* parent 2 still locked */

	/* Saving twice is idempotent and rotates the previous file to .bak. */
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	assert(exists(path_of(id, ".bak")));
	assert(!exists(path_of(id, ".tmp")));
	puts("PASS round trip: save -> load is byte-identical state, no toast state to re-fire");
}

static void test_ach_letter_derived(void) {
	const char* id = "letter";
	stats_profile_reset(&g_src, id);
	stats_counter_add(&g_src.ach[3], STATS_COLUMN_LOCAL, 1u);
	stats_counter_add(&g_src.ach[4], STATS_COLUMN_REMOTE, 1u);
	/* ach_first deliberately left unset (a profile filled before B2's rules
	 * layer exists): the writer derives the letter from the columns. */
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	stats_profile_load(s_dir, id, &g_dst, NULL);
	assert(g_dst.ach_first[3] == (uint8_t)STATS_FIRST_LOCAL);
	assert(g_dst.ach_first[4] == (uint8_t)STATS_FIRST_REMOTE);
	puts("PASS achievement provenance letter round-trips (and is derived when unset)");
}

static void test_fault_stages(void) {
	static const int stages[] = {
		STATS_IO_STAGE_TMP_OPEN, STATS_IO_STAGE_WRITE, STATS_IO_STAGE_FSYNC,
		STATS_IO_STAGE_RENAME_BAK, STATS_IO_STAGE_RENAME_MAIN,
	};
	static const enum stats_save_status want[] = {
		STATS_SAVE_ERR_TMP_OPEN,	STATS_SAVE_ERR_WRITE,
		STATS_SAVE_ERR_FSYNC,		STATS_SAVE_ERR_RENAME_BAK,
		STATS_SAVE_ERR_RENAME_MAIN,
	};

	for(size_t s = 0; s < sizeof(stages) / sizeof(stages[0]); s++) {
		char id[32];
		snprintf(id, sizeof(id), "fault%d", stages[s]);

		/* A committed "previous good" file. */
		fill(&g_src, id, 0);
		assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
		static char good[8192];
		long good_len = read_file(path_of(id, ""), good, sizeof(good));
		assert(good_len > 0);

		/* The failing write of DIFFERENT content. */
		fill(&g_src, id, 1000);
		memcpy(&g_copy, &g_src, sizeof(g_copy));
		stats_io_fail_at(stages[s]);
		assert(stats_profile_save(s_dir, &g_src) == want[s]);

		/* The caller's state is untouched, so whatever dirty flag it holds
		 * is still set and the next flush retries. */
		assert(memcmp(&g_copy, &g_src, sizeof(g_copy)) == 0);
		/* No torn file: the committed name still holds exactly the previous
		 * good bytes, and no stray .tmp survives. */
		static char now[8192];
		long now_len = read_file(path_of(id, ""), now, sizeof(now));
		assert(now_len == good_len && memcmp(now, good, (size_t)good_len) == 0);
		assert(!exists(path_of(id, ".tmp")));
		/* And it still LOADS -- not merely "same bytes", but usable. */
		enum stats_load_status st = STATS_LOAD_CORRUPT;
		stats_profile_load(s_dir, id, &g_dst, &st);
		assert(st == STATS_LOAD_OK);

		/* Disarmed, the same save succeeds and the new content commits. */
		assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
		stats_profile_load(s_dir, id, &g_dst, &st);
		assert(st == STATS_LOAD_OK && profiles_equal(&g_src, &g_dst));
	}
	/* The hook is one-shot: an armed stage that never fires is cleared by
	 * the next arm, and disarming is explicit. */
	stats_io_fail_at(STATS_IO_STAGE_NONE);
	puts("PASS every write-fault stage leaves the previous good file, never a torn one");
}

static void test_bad_crc_falls_back_to_bak(void) {
	const char* id = "crc";
	fill(&g_src, id, 0); /* generation 1 -> becomes .bak */
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	fill(&g_copy, id, 77); /* generation 2 -> the committed file */
	assert(stats_profile_save(s_dir, &g_copy) == STATS_SAVE_OK);

	static char bytes[8192];
	long len = read_file(path_of(id, ""), bytes, sizeof(bytes));
	assert(len > 2);
	bytes[len - 2] = (bytes[len - 2] == 'a') ? 'b' : 'a'; /* a crc digit */
	write_file(path_of(id, ""), bytes, (size_t)len);

	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_RECOVERED_BAK);
	assert(profiles_equal(&g_src, &g_dst)); /* generation 1 came back */
	assert(!exists(path_of(id, "")));		/* quarantined, not left in place */
	static char quarantined[8192];
	long qlen = read_file(path_of(id, ".bad"), quarantined, sizeof(quarantined));
	assert(qlen == len && memcmp(quarantined, bytes, (size_t)len) == 0);

	/* THE NEWEST quarantine wins (R1 HIGH 1). The old rule kept the FIRST
	 * `.bad` and therefore left the second corrupt main file in place, which
	 * is what let the next save rotate a known-corrupt file over the only good
	 * `.bak`; test_corrupt_main_never_rotated_over_bak() below is that whole
	 * sequence. Here: the second corruption replaces the evidence, and the
	 * damaged main file is NOT left behind. */
	fill(&g_copy, id, 9);
	assert(stats_profile_save(s_dir, &g_copy) == STATS_SAVE_OK);
	static char second[8192];
	long len2 = read_file(path_of(id, ""), second, sizeof(second));
	assert(len2 > 2);
	second[len2 - 2] = (second[len2 - 2] == 'a') ? 'b' : 'a';
	write_file(path_of(id, ""), second, (size_t)len2);
	stats_profile_load(s_dir, id, &g_dst, &st);
	static char again[8192];
	long qlen2 = read_file(path_of(id, ".bad"), again, sizeof(again));
	assert(qlen2 == len2 && memcmp(again, second, (size_t)len2) == 0);
	assert(qlen2 != qlen || memcmp(again, quarantined, (size_t)qlen) != 0);
	assert(!exists(path_of(id, "")));
	assert(g_dst.main_was_corrupt == 1);
	puts("PASS bad crc: quarantined to .bad (newest wins), .bak recovered");
}

/* R1 HIGH 1, the whole sequence that used to end with an empty profile:
 * corrupt, save, corrupt again, then lose the promote. `.bak` must still hold
 * the good state at every step, because it only ever receives a file THIS
 * writer produced. */
static void test_corrupt_main_never_rotated_over_bak(void) {
	const char* id = "h1";

	/* Two good saves: .bak = generation 1, main = generation 2. */
	fill(&g_src, id, 0);
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	fill(&g_copy, id, 42);
	assert(stats_profile_save(s_dir, &g_copy) == STATS_SAVE_OK);

	/* First corruption: quarantined, generation 1 recovered from .bak. */
	corrupt_main(id);
	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_RECOVERED_BAK && profiles_equal(&g_src, &g_dst));
	assert(g_dst.main_was_corrupt == 1);

	/* THE SAVE IN BETWEEN. It writes a new main file; whatever it does, the
	 * good .bak must survive it. */
	assert(stats_profile_save(s_dir, &g_dst) == STATS_SAVE_OK);
	static char bak_after[8192];
	long bak_len = read_file(path_of(id, ".bak"), bak_after, sizeof(bak_after));
	assert(bak_len > 0);

	/* Second corruption -- with a `.bad` ALREADY on disk. This is the state
	 * the old code could not get out of: it refused to replace the
	 * quarantine, so the corrupt main stayed put. */
	corrupt_main(id);
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_RECOVERED_BAK && profiles_equal(&g_src, &g_dst));
	assert(g_dst.main_was_corrupt == 1);

	/* And now the save whose promote fails -- the step that used to leave the
	 * corrupt file installed as BOTH main and backup. */
	stats_io_fail_at(STATS_IO_STAGE_RENAME_MAIN);
	assert(stats_profile_save(s_dir, &g_dst) == STATS_SAVE_ERR_RENAME_MAIN);
	stats_io_fail_at(STATS_IO_STAGE_NONE);

	/* THE CLAIM: .bak is byte-unchanged and still loads the good state. */
	static char bak_now[8192];
	long bak_len2 = read_file(path_of(id, ".bak"), bak_now, sizeof(bak_now));
	assert(bak_len2 == bak_len
		   && memcmp(bak_now, bak_after, (size_t)bak_len) == 0);
	stats_profile_load(s_dir, id, &g_copy, &st);
	assert(st == STATS_LOAD_OK || st == STATS_LOAD_RECOVERED_BAK);
	assert(profiles_equal(&g_src, &g_copy));
	puts("PASS a corrupt main file is never rotated over the good .bak");
}

static void test_short_file(void) {
	const char* id = "short";
	fill(&g_src, id, 0);
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	fill(&g_copy, id, 5);
	assert(stats_profile_save(s_dir, &g_copy) == STATS_SAVE_OK);

	static char bytes[8192];
	long len = read_file(path_of(id, ""), bytes, sizeof(bytes));
	assert(len > 40);
	write_file(path_of(id, ""), bytes, (size_t)len - 25); /* no `end` line */

	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_RECOVERED_BAK);
	assert(profiles_equal(&g_src, &g_dst));
	assert(exists(path_of(id, ".bad")));
	puts("PASS truncated file: no `end` record -> quarantined, .bak recovered");
}

static void test_identity_mismatch(void) {
	const char* id = "ident";
	/* No .bak at all, so the fallback finds nothing: empty state + CORRUPT. */
	write_cwstats(path_of(id, ""),
				  "cwstats 1\nprofile someone_else\nsaved 1\ns 2010 4 0\n", 1);
	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_CORRUPT);
	assert(profile_is_empty(&g_dst));
	assert(strcmp(g_dst.profile_id, id) == 0);
	assert(exists(path_of(id, ".bad")));
	puts("PASS identity mismatch: another profile's file is never adopted");
}

static void test_unsupported_version(void) {
	const char* id = "newer";
	write_cwstats(path_of(id, ""),
				  "cwstats 2\nprofile newer\nsaved 1\ns 2010 4 0\nq 1 2 3\n", 1);
	static char before[8192];
	long len = read_file(path_of(id, ""), before, sizeof(before));
	assert(len > 0);

	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_UNSUPPORTED_VERSION);
	assert(profile_is_empty(&g_dst));
	assert(g_dst.write_refused == 1);

	/* The save honours the refusal, and the newer file is byte-untouched. */
	stats_counter_add(&g_dst.general[stat_general_index(2010)],
					  STATS_COLUMN_LOCAL, 1u);
	assert(stats_profile_save(s_dir, &g_dst) == STATS_SAVE_REFUSED_NEWER);
	static char after[8192];
	long len2 = read_file(path_of(id, ""), after, sizeof(after));
	assert(len2 == len && memcmp(after, before, (size_t)len) == 0);
	assert(!exists(path_of(id, ".bad")) && !exists(path_of(id, ".tmp")));
	puts("PASS newer version: untouched on disk, empty in memory, writes refused");
}

static void test_unknown_record_skipped(void) {
	const char* id = "unknown";
	/* Exactly what a v2 writer's extra line looks like to a v1 reader: it is
	 * skipped, does NOT count toward `end <count>`, and changes no counter --
	 * but it IS kept, so this reader's next save does not delete the line it
	 * was told to ignore (R1 HIGH 2). */
	write_cwstats(path_of(id, ""),
				  "cwstats 1\nprofile unknown\nsaved 1789000000\n"
				  "z whatever the future wants\n"
				  "s 2010 7 0\n"
				  "a 5242880 1 0 L\n"
				  "q 4 5 6\n",
				  2);
	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_OK);
	assert(g_dst.general[stat_general_index(2010)].local == 7u);
	assert(ach_unlocked(&g_dst, 0));
	assert(g_dst.foreign_count == 2);
	assert(strcmp(g_dst.foreign[0], "z whatever the future wants") == 0);
	assert(strcmp(g_dst.foreign[1], "q 4 5 6") == 0);
	assert(g_dst.progress_incomplete == 1); /* "this file holds more than I do" */
	puts("PASS unknown record letters are skipped, never counted, and kept");
}

static void test_unrepresentable_id_is_flagged(void) {
	const char* id = "wideid";
	/* A crc-valid file carrying a counter this build has no slot for (an
	 * item id past ITEMS_MAX). The rest of the file still loads and the
	 * profile says so rather than the whole file being discarded. */
	write_cwstats(path_of(id, ""),
				  "cwstats 1\nprofile wideid\nsaved 1\n"
				  "s 2010 7 0\n"
				  "s 16842752 3 0\n", /* 0x1010000 + 0 is fine ... */
				  2);
	stats_profile_load(s_dir, id, &g_dst, NULL);
	assert(g_dst.craft[0].local == 3u && g_dst.progress_incomplete == 0);
	assert(g_dst.foreign_count == 0);

	write_cwstats(path_of(id, ""),
				  "cwstats 1\nprofile wideid\nsaved 1\n"
				  "s 2010 7 0\n"
				  "s 16875520 3 0\n", /* 0x1010000 + 32768: past ITEMS_MAX */
				  2);
	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_OK);
	assert(g_dst.general[stat_general_index(2010)].local == 7u);
	assert(g_dst.progress_incomplete == 1);
	puts("PASS an id this build cannot represent is flagged, not silently dropped");
}

/* R1 HIGH 2. The flag and the record both have to SURVIVE a save: before this,
 * the next flush rewrote the file without the counter it could not hold and
 * without any trace that it ever existed. */
static void test_foreign_records_survive_a_save(void) {
	const char* id = "foreign";
	write_cwstats(path_of(id, ""),
				  "cwstats 1\nprofile foreign\nsaved 1\n"
				  "s 2010 7 0\n"
				  "s 16875520 3 0\n"		/* craftItem past ITEMS_MAX   */
				  "a 5243000 1 0 L\n"		/* an achievement id past 15  */
				  "y a future record\n",	/* an unknown record letter   */
				  3); /* the `y` line is not a counter record, so it is not one */
	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_OK);
	assert(g_dst.progress_incomplete == 1 && g_dst.foreign_count == 3);

	/* Round trip: save what we loaded, then load it back. */
	assert(stats_profile_save(s_dir, &g_dst) == STATS_SAVE_OK);
	stats_profile_load(s_dir, id, &g_src, &st);
	assert(st == STATS_LOAD_OK);
	assert(g_src.general[stat_general_index(2010)].local == 7u);
	/* BYTE-IDENTICAL foreign lines, in load order ... */
	assert(g_src.foreign_count == g_dst.foreign_count);
	for(int i = 0; i < (int)g_dst.foreign_count; i++)
		assert(strcmp(g_src.foreign[i], g_dst.foreign[i]) == 0);
	assert(strcmp(g_src.foreign[0], "s 16875520 3 0") == 0);
	assert(strcmp(g_src.foreign[1], "a 5243000 1 0 L") == 0);
	assert(strcmp(g_src.foreign[2], "y a future record") == 0);
	/* ... and the flag persisted as the `incomplete 1` header. */
	assert(g_src.progress_incomplete == 1);
	static char bytes[8192];
	assert(read_file(path_of(id, ""), bytes, sizeof(bytes)) > 0);
	assert(strstr(bytes, "\nincomplete 1\n") != NULL);
	assert(strstr(bytes, "\ns 16875520 3 0\n") != NULL);
	assert(strstr(bytes, "\ny a future record\n") != NULL);

	/* A second round trip is byte-stable, which is what makes it a FORMAT and
	 * not a one-way migration. */
	static char again[8192];
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	assert(read_file(path_of(id, ""), again, sizeof(again)) > 0);
	assert(strcmp(again, bytes) == 0);

	/* Overflow fails CLOSED: a file with more foreign records than the list
	 * can hold is never partially rewritten. */
	{
		static char body[16384];
		size_t w = (size_t)snprintf(body, sizeof(body),
									"cwstats 1\nprofile ovfl\nsaved 1\n");
		for(int i = 0; i <= CW_STATS_FOREIGN_MAX; i++)
			w += (size_t)snprintf(body + w, sizeof(body) - w, "s %lu 1 0\n",
								  (unsigned long)(0x1010000u + 32768u + i));
		write_cwstats(path_of("ovfl", ""), body,
					  (uint32_t)(CW_STATS_FOREIGN_MAX + 1));
	}
	stats_profile_load(s_dir, "ovfl", &g_dst, &st);
	assert(st == STATS_LOAD_OK);
	assert(g_dst.foreign_count == CW_STATS_FOREIGN_MAX);
	assert(g_dst.write_refused == (uint8_t)STATS_WRITE_REFUSED_FOREIGN);
	assert(stats_profile_save(s_dir, &g_dst) == STATS_SAVE_REFUSED_FOREIGN);
	assert(!exists(path_of("ovfl", ".tmp")) && !exists(path_of("ovfl", ".bak")));
	puts("PASS records this build cannot hold survive a save, and overflow "
		 "fails closed");
}

/* R1 MED 3: one key rule, both directions. */
static void test_invalid_key_is_rejected_by_both(void) {
	const char* seventeen = "seventeen_chars_x"; /* 17 */
	assert(strlen(seventeen) == 17);

	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, seventeen, &g_dst, &st);
	assert(st == STATS_LOAD_BAD_ARG);
	/* The trap this closes: reset() used to TRUNCATE, so the profile held
	 * "seventeen_chars_" -- a key the writer accepts and the reader can never
	 * open again with the key it was given. Now it holds no key at all, and
	 * the save fails the same way the load did. */
	stats_profile_reset(&g_src, seventeen);
	assert(g_src.profile_id[0] == '\0');
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_ERR_BAD_ARG);
	assert(!exists(path_of("seventeen_chars_", "")));
	assert(!exists(path_of("seventeen_chars_", ".tmp")));

	/* Out-of-charset keys, both directions. */
	stats_profile_load(s_dir, "Bad-Key", &g_dst, &st);
	assert(st == STATS_LOAD_BAD_ARG);
	stats_profile_reset(&g_src, "Bad-Key");
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_ERR_BAD_ARG);
	stats_profile_load(s_dir, "", &g_dst, &st);
	assert(st == STATS_LOAD_BAD_ARG);
	stats_profile_load(s_dir, NULL, &g_dst, &st);
	assert(st == STATS_LOAD_BAD_ARG);

	assert(stats_profile_id_valid("player1") && stats_profile_id_valid("a"));
	assert(stats_profile_id_valid("sixteen_chars_ok"));
	assert(!stats_profile_id_valid(seventeen) && !stats_profile_id_valid(""));
	assert(!stats_profile_id_valid("has space") && !stats_profile_id_valid(NULL));
	puts("PASS an invalid profile key is rejected by load AND save, and writes "
		 "nothing");
}

/* R1 LOW 12: last-wins silently changes which value survives. */
static void test_duplicate_id_is_corrupt(void) {
	const char* id = "dup";
	write_cwstats(path_of(id, ""),
				  "cwstats 1\nprofile dup\nsaved 1\n"
				  "s 2010 7 0\n"
				  "s 2010 9 0\n",
				  2);
	enum stats_load_status st = STATS_LOAD_OK;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_CORRUPT);
	assert(profile_is_empty(&g_dst));
	assert(exists(path_of(id, ".bad")));

	/* The same rule for `a` records, and for an id neither kind can hold. */
	const char* id2 = "dupa";
	write_cwstats(path_of(id2, ""),
				  "cwstats 1\nprofile dupa\nsaved 1\n"
				  "a 5242880 1 0 L\n"
				  "a 5242880 2 0 R\n",
				  2);
	stats_profile_load(s_dir, id2, &g_dst, &st);
	assert(st == STATS_LOAD_CORRUPT);
	const char* id3 = "dupf";
	write_cwstats(path_of(id3, ""),
				  "cwstats 1\nprofile dupf\nsaved 1\n"
				  "s 16875520 1 0\n"
				  "s 16875520 2 0\n",
				  2);
	stats_profile_load(s_dir, id3, &g_dst, &st);
	assert(st == STATS_LOAD_CORRUPT);

	/* Two DIFFERENT ids that merely share a value are of course fine. */
	const char* ok = "nodup";
	write_cwstats(path_of(ok, ""),
				  "cwstats 1\nprofile nodup\nsaved 1\n"
				  "s 2010 7 0\n"
				  "s 2011 7 0\n",
				  2);
	stats_profile_load(s_dir, ok, &g_dst, &st);
	assert(st == STATS_LOAD_OK);
	puts("PASS a duplicate s/a id is corrupt, not last-wins");
}

/* R1 MED 6: the barrier exists, and it is reached ONLY after both renames. */
static void test_rename_sync_runs_after_both_renames(void) {
	const char* id = "rsync";
	fill(&g_src, id, 0);
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	static char first[8192];
	long first_len = read_file(path_of(id, ""), first, sizeof(first));
	assert(first_len > 0);

	/* (a) A save that never gets as far as the renames must NOT consume the
	 * stage. The failure is a real one -- a directory that does not exist --
	 * so nothing about the ordering is assumed. */
	stats_io_fail_at(STATS_IO_STAGE_RENAME_SYNC);
	assert(stats_profile_save(test_fixture_join(s_dir, "/nope"), &g_src)
		   == STATS_SAVE_ERR_TMP_OPEN);
	assert(stats_io_fail_armed() == STATS_IO_STAGE_RENAME_SYNC);

	/* (b) A save that completes both renames DOES reach it -- and a failing
	 * barrier still leaves the save successful, because the rename already
	 * committed the data. */
	fill(&g_copy, id, 7);
	assert(stats_profile_save(s_dir, &g_copy) == STATS_SAVE_OK);
	assert(stats_io_fail_armed() == STATS_IO_STAGE_NONE);

	/* Both renames had happened before the stage fired: main holds the NEW
	 * content and .bak holds what main held before it. */
	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, id, &g_dst, &st);
	assert(st == STATS_LOAD_OK && profiles_equal(&g_copy, &g_dst));
	static char bak[8192];
	long bak_len = read_file(path_of(id, ".bak"), bak, sizeof(bak));
	assert(bak_len == first_len && memcmp(bak, first, (size_t)first_len) == 0);
	puts("PASS the post-rename sync stage is reached only after both renames");
}

static void test_missing_file(void) {
	enum stats_load_status st = STATS_LOAD_CORRUPT;
	stats_profile_load(s_dir, "nothinghere", &g_dst, &st);
	assert(st == STATS_LOAD_MISSING);
	assert(profile_is_empty(&g_dst));

	/* Bad arguments never write and never crash. */
	assert(stats_profile_save(s_dir, NULL) == STATS_SAVE_ERR_BAD_ARG);
	stats_profile_reset(&g_dst, NULL);
	assert(stats_profile_save(s_dir, &g_dst) == STATS_SAVE_ERR_BAD_ARG);
	stats_profile_load(NULL, "x", &g_dst, &st);
	assert(st == STATS_LOAD_BAD_ARG);
	puts("PASS a missing file is a fresh profile; bad arguments fail closed");
}

static void test_saturation(void) {
	struct stats_pair p = {0, 0};
	stats_counter_add(&p, STATS_COLUMN_LOCAL, UINT32_MAX - 1u);
	stats_counter_add(&p, STATS_COLUMN_LOCAL, 1u);
	assert(p.local == UINT32_MAX);
	stats_counter_add(&p, STATS_COLUMN_LOCAL, 1000u); /* saturates, no wrap */
	assert(p.local == UINT32_MAX);
	stats_counter_add(&p, STATS_COLUMN_REMOTE, 5u);
	assert(p.remote == 5u);
	/* The displayed total is computed in 64 bits and saturated back. */
	assert(stats_counter_total(&p) == UINT32_MAX);
	struct stats_pair q = {7u, 11u};
	assert(stats_counter_total(&q) == 18u);
	stats_counter_add(&q, STATS_COLUMN_LOCAL, 0u); /* a zero amount is inert */
	assert(q.local == 7u);

	/* And a saturated counter survives the file round trip. */
	const char* id = "sat";
	stats_profile_reset(&g_src, id);
	g_src.general[stat_general_index(2000)] = p;
	assert(stats_profile_save(s_dir, &g_src) == STATS_SAVE_OK);
	stats_profile_load(s_dir, id, &g_dst, NULL);
	assert(g_dst.general[stat_general_index(2000)].local == UINT32_MAX);
	assert(g_dst.general[stat_general_index(2000)].remote == 5u);
	puts("PASS counters saturate at UINT32_MAX; totals are 64-bit then saturated");
}

static void test_canonical_blocks(void) {
	/* The alias rows [design 2]. */
	assert(stat_canonical_block(2) == 3);	/* grass    -> dirt */
	assert(stat_canonical_block(60) == 3);	/* farmland -> dirt */
	assert(stat_canonical_block(3) == 3);	/* dirt is its own canonical id */
	assert(stat_canonical_block(62) == 61); /* lit furnace -> furnace */
	assert(stat_canonical_block(74) == 73); /* glowing redstone ore */
	assert(stat_canonical_block(76) == 75); /* active redstone torch */
	assert(stat_canonical_block(40) == 39); /* red -> brown mushroom */
	assert(stat_canonical_block(43) == 44); /* double slab -> slab */
	assert(stat_canonical_block(91) == 86); /* pumpkin lantern -> pumpkin */

	/* disableStats() blocks have no counter at all, and so do the alias
	 * rows whose target is one of them (water, lava, repeaters). */
	assert(stat_canonical_block(7) == CW_STAT_BLOCK_NONE);	/* bedrock */
	assert(stat_canonical_block(18) == CW_STAT_BLOCK_NONE); /* leaves */
	assert(stat_canonical_block(8) == CW_STAT_BLOCK_NONE);	/* water -> still */
	assert(stat_canonical_block(9) == CW_STAT_BLOCK_NONE);
	assert(stat_canonical_block(11) == CW_STAT_BLOCK_NONE); /* the no-op pair */
	assert(stat_canonical_block(94) == CW_STAT_BLOCK_NONE); /* repeater */
	assert(stat_canonical_block(256) == CW_STAT_BLOCK_NONE); /* out of range */

	/* The ALIAS half alone, which is what the craftItem / useItem / breakItem
	 * arrays fold through: StatList runs replaceAllSimilarBlocks over all four
	 * arrays, but only mineBlock is built behind getEnableStats(), so a
	 * disableStats() block still HAS a craft/use counter (R1 lead item 2). */
	assert(stat_alias_block(2) == 3 && stat_alias_block(60) == 3);
	assert(stat_alias_block(62) == 61 && stat_alias_block(43) == 44);
	assert(stat_alias_block(3) == 3); /* not aliased */
	assert(stat_alias_block(64) == 64);	 /* wooden door: no mine stat ... */
	assert(stat_canonical_block(64) == CW_STAT_BLOCK_NONE); /* ... but craftable */
	assert(stat_alias_block(92) == 92 && stat_alias_block(26) == 26);
	assert(stat_alias_block(270) == 270); /* an item id has no alias */
	assert(stat_alias_block(2303) == 2303);

	/* The design's invariant behind "the alias is always: count as the target
	 * id" -- no alias row crosses the enabled/disabled boundary. Pinned here
	 * so a future row cannot quietly break the reasoning. */
	static const uint16_t disabled[] = {
#define CW_BLOCK_NO_STAT(b) (uint16_t)(b),
		CW_BLOCK_NO_STAT_ROWS
#undef CW_BLOCK_NO_STAT
	};
	assert(sizeof(disabled) / sizeof(disabled[0])
		   == (size_t)CW_BLOCK_NO_STAT_COUNT);
#define CW_BLOCK_ALIAS(from, to)                                               \
	{                                                                          \
		bool from_off = false, to_off = false;                                 \
		for(size_t i = 0; i < (size_t)CW_BLOCK_NO_STAT_COUNT; i++) {                   \
			if(disabled[i] == (uint16_t)(from))                                \
				from_off = true;                                               \
			if(disabled[i] == (uint16_t)(to))                                  \
				to_off = true;                                                 \
		}                                                                      \
		assert(from_off == to_off);                                            \
	}
	CW_BLOCK_ALIAS_ROWS
#undef CW_BLOCK_ALIAS

	/* The table itself matches design 1.1/1.4. */
	assert(CW_ACH_COUNT == 16 && CW_STAT_COUNT == 23);
	assert(ach_parent(0) == -1 && ach_is_independent(0));
	assert(ach_parent(15) == 14 && ach_is_special(15) && ach_is_special(11));
	assert(ach_icon_item(0) == 340 && ach_icon_item(11) == 66);
	assert(strcmp(ach_key(13), "killEnemy") == 0);
	assert(stat_general_id(0) == 1000
		   && stat_general_id(CW_STAT_COUNT - 1) == 2025);
	assert(stat_general_index(0x1000000) < 0); /* an array id is not general */
	puts("PASS canonical block folding, the disabled set and the id table");
}

int main(void) {
	test_fixture_dir("test_achievements_persist_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);
	stats_set_clock(fixed_clock);

	test_round_trip();
	test_ach_letter_derived();
	test_fault_stages();
	test_bad_crc_falls_back_to_bak();
	test_corrupt_main_never_rotated_over_bak();
	test_short_file();
	test_identity_mismatch();
	test_unsupported_version();
	test_unknown_record_skipped();
	test_unrepresentable_id_is_flagged();
	test_foreign_records_survive_a_save();
	test_invalid_key_is_rejected_by_both();
	test_duplicate_id_is_corrupt();
	test_rename_sync_runs_after_both_renames();
	test_missing_file();
	test_saturation();
	test_canonical_blocks();

	stats_set_clock(NULL);
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_persist");
	return 0;
}
