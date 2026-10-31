/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * g9-savemiss-1009 (T1): the checkpoint read-back memo. A save that must
 * merge with the stored record may take that record's UNCOMPRESSED bytes from
 * the memo instead of the SD read + inflate, but only while the archive entry
 * still names the very record the memo wrote. Proof: a memo arm (cached /
 * watched / plain saves, exactly as server_world issues them) against a
 * reference arm that never uses the memo (plain set_chunk throughout), in all
 * three IO modes. Complete save-directory bytes, ordered persistence traces
 * and every success flag must match at EVERY step, through hits, skip-
 * unchanged, foreign-tag recommits, plain/retain rewrites, watch moves,
 * injected write failures and close/reopen. Hit counts prove the fast path
 * ran, and that each invalidating event really invalidated it. A stale memo
 * is observable here: the recommitted foreign root tag must survive the merge
 * and an unchanged-skip over a different stored record would lose bytes.
 */
#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
#include "item/item_nbt.h"
#include "platform/thread.h"

static char root[2][220];
static struct region_archive ra[2];
static bool open_[2];
static struct g9io_trace traces[2];
static struct region_encode_cache* cache;
static struct server_chunk sc;
static uint8_t ids[32768], meta[16384], sky[16384], torch[16384], height[256];
static struct te_chest chest;
static struct te_furnace furnace;
static struct chunk_tile_entities te;
static struct chunk_entities entities;
static unsigned steps;

static void compare(const char* label) {
	struct g9io_snap a, b;
	g9io_snap_take(&a, root[0]); g9io_snap_take(&b, root[1]);
	if(!g9io_snap_equal(&a, &b, label)) {
		printf("FAIL at step %u (%s)\n", steps, label);
		assert(false);
	}
	steps++;
}
static uint32_t hits(void) {
	uint32_t s[2]; region_archive_encode_cache_readback_stats(cache, s);
	return s[0];
}
static void fill(unsigned state) {
	memset(ids, 1, sizeof ids); memset(meta, state & 15, sizeof meta);
	memset(sky, 0x9f ^ state, sizeof sky); memset(torch, 0x12 + state, sizeof torch);
	memset(height, 80 + (state & 7), sizeof height);
	ids[((1u * 16u + 5u) * 128u) + 63u] = 54;
	ids[((6u * 16u + 9u) * 128u) + 64u] = 61;
	ids[((3u * 16u + 3u) * 128u) + 70u] = (uint8_t)(state * 7);
	sc.populated = true;
	chest.items[0] = (struct item_data){.id = 278, .count = 1, .durability = 1500 + state};
	furnace.burn_time = 100 + state;
}
static void open_both(void) {
	for(unsigned arm = 0; arm < 2; arm++) {
		string_t level; string_init_set_str(level, root[arm]);
		assert(region_archive_create(&ra[arm], level, 0, 0, WORLD_DIM_OVERWORLD));
		string_clear(level); open_[arm] = true;
	}
}
static void close_both(void) {
	for(unsigned arm = 0; arm < 2; arm++) if(open_[arm]) {
		assert(region_archive_destroy(&ra[arm])); open_[arm] = false;
	}
}
enum op { OP_CACHED, OP_WATCHED, OP_PLAIN, OP_RETAIN };
/* One save on both arms; the reference arm is ALWAYS the plain save. */
static bool save(enum op op, w_coord_t x, w_coord_t z, int expect_ok,
	enum region_archive_test_fail_stage fail, const char* label) {
	bool ok[2];
	/* The fixture TEs live in chunk (1,1); other chunks save without. */
	const struct chunk_tile_entities* t = x == 1 && z == 1 ? &te : NULL;
	for(unsigned arm = 0; arm < 2; arm++) {
		g9io_trace_begin(&traces[arm], root[arm]);
		if(fail) region_archive_test_fail_next(fail);
		if(!arm) ok[arm] = region_archive_set_chunk(&ra[arm], x, z, &sc, t, &entities);
		else if(op == OP_CACHED)
			ok[arm] = region_archive_set_chunk_cached(&ra[arm], x, z, &sc, t, &entities, &cache);
		else if(op == OP_WATCHED)
			ok[arm] = region_archive_set_chunk_watched(&ra[arm], x, z, &sc, t, &entities, &cache);
		else if(op == OP_PLAIN)
			ok[arm] = region_archive_set_chunk(&ra[arm], x, z, &sc, t, &entities);
		else {
			struct buffer kept;
			ok[arm] = region_archive_set_chunk_retain(&ra[arm], x, z, &sc, t, &entities, &kept);
			buffer_free(&kept);
		}
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_NONE);
		bool synced = region_archive_sync(&ra[arm]);
		(void)synced;
		g9io_trace_end();
	}
	assert(ok[0] == ok[1]);
	assert(expect_ok < 0 || ok[0] == (expect_ok > 0));
	assert(g9io_trace_equal(&traces[0], &traces[1], label));
	compare(label);
	return ok[0];
}
static void recommit_marker(int value) {
	for(unsigned arm = 0; arm < 2; arm++) {
		uint32_t sector = ra[arm].offsets[33] >> 8;
		FILE* f = ra[arm].file;
		assert(!fseek(f, (long)sector * REGION_SECTOR_SIZE, SEEK_SET));
		uint8_t h[5]; assert(fread(h, 1, 5, f) == 5 && h[4] == 2);
		size_t n = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
		uint8_t* bytes = malloc(n - 1); assert(bytes && fread(bytes, 1, n - 1, f) == n - 1);
		nbt_node* tree = nbt_parse_compressed(bytes, n - 1); free(bytes); assert(tree);
		nbt_node* marker = calloc(1, sizeof *marker); assert(marker);
		marker->type = TAG_INT;
		marker->name = malloc(5); assert(marker->name); memcpy(marker->name, "kept", 5);
		marker->payload.tag_int = value;
		assert(item_nbt_replace(tree, "kept", marker));
		struct buffer record = nbt_dump_compressed(tree, STRAT_INFLATE);
		assert(record.data && region_archive_recommit_record(&ra[arm], 1, 1, &record));
		buffer_free(&record); nbt_free(tree);
		assert(region_archive_sync(&ra[arm]));
	}
	compare("recommit foreign tag");
}
static void run_mode(int mode) {
	g9io_arm(mode);
	region_archive_encode_cache_free(&cache);
	for(unsigned arm = 0; arm < 2; arm++) {
		char name[64];
		snprintf(name, sizeof name, "g9_rbmemo_%s_%s", arm ? "memo" : "ref", G9IO_ARM_NAME[mode]);
		test_fixture_dir(name, root[arm], sizeof root[arm]);
		G9IO_MKDIR(root[arm]); G9IO_MKDIR(test_fixture_join(root[arm], "/region"));
		string_t level; string_init_set_str(level, root[arm]);
		assert(region_archive_create_new(&ra[arm], level, 0, 0, WORLD_DIM_OVERWORLD));
		string_clear(level); open_[arm] = true;
	}
	uint32_t h0;
	fill(1); save(OP_PLAIN, 1, 1, true, 0, "initial plain");
	fill(2); h0 = hits(); save(OP_CACHED, 1, 1, true, 0, "cold cached (disk read, arms)");
	assert(hits() == h0);
	fill(3); save(OP_CACHED, 1, 1, true, 0, "cached hit"); assert(hits() == h0 + 1);
	fill(4); save(OP_WATCHED, 1, 1, true, 0, "watched autosave hit"); assert(hits() == h0 + 2);
	save(OP_CACHED, 1, 1, true, 0, "unchanged (skip) hit"); assert(hits() == h0 + 3);
	save(OP_WATCHED, 1, 1, true, 0, "unchanged watched hit"); assert(hits() == h0 + 4);
	/* Foreign tag recommitted under the watch: must invalidate and survive. */
	recommit_marker(42); h0 = hits();
	fill(5); save(OP_CACHED, 1, 1, true, 0, "after recommit (disk)"); assert(hits() == h0);
	fill(6); save(OP_CACHED, 1, 1, true, 0, "hit with kept tag"); assert(hits() == h0 + 1);
	/* Stale-skip probe: a different record recommitted, then the memo's own
	 * content saved again. A stale memo would skip and keep the marker 7. */
	recommit_marker(7); h0 = hits();
	save(OP_CACHED, 1, 1, true, 0, "stale-skip probe"); assert(hits() == h0);
	/* ABA probe: two copy-on-write recommits can hand the entry back the
	 * SAME offset/sector value with different bytes; only the watch token
	 * (cleared by every index store) can tell. */
	fill(20); save(OP_CACHED, 1, 1, true, 0, "aba arm");
	const uint32_t armed = ra[1].offsets[33];
	unsigned aba = 0;
	do { recommit_marker(100 + (int)aba); aba++; }
	while(aba < 8 && ra[1].offsets[33] != armed);
	printf("aba: entry value %s after %u recommits\n",
		ra[1].offsets[33] == armed ? "RECURRED" : "did not recur", aba);
	h0 = hits();
	save(OP_CACHED, 1, 1, true, 0, "aba probe"); assert(hits() == h0);
	/* A plain (unwatched) rewrite and a retained rewrite both invalidate. */
	fill(7); save(OP_CACHED, 1, 1, true, 0, "re-arm"); h0 = hits();
	fill(8); save(OP_PLAIN, 1, 1, true, 0, "plain rewrite"); assert(hits() == h0);
	fill(9); save(OP_CACHED, 1, 1, true, 0, "after plain (disk)"); assert(hits() == h0);
	fill(10); save(OP_RETAIN, 1, 1, true, 0, "retain rewrite"); assert(hits() == h0);
	fill(11); save(OP_CACHED, 1, 1, true, 0, "after retain (disk)"); assert(hits() == h0);
	/* The watch moves with the newest cached save. */
	save(OP_CACHED, 2, 1, true, 0, "other chunk first save");
	fill(12); save(OP_CACHED, 2, 1, true, 0, "other chunk hit"); assert(hits() == h0 + 1);
	save(OP_CACHED, 1, 1, true, 0, "watch moved away (disk)"); assert(hits() == h0 + 1);
	/* Unrelated entries do not disturb the watch. */
	fill(13); save(OP_PLAIN, 2, 1, true, 0, "unrelated plain");
	fill(14); save(OP_CACHED, 1, 1, true, 0, "hit across unrelated"); assert(hits() == h0 + 2);
	/* Any failure, anywhere, invalidates (conservative epoch): the reference
	 * arm fails first, so even the memo arm's own read-back goes to disk. */
	fill(15); h0 = hits();
	bool ok = save(OP_CACHED, 1, 1, -1, REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE, "failed payload write");
	if(mode != 2) assert(!ok);
	assert(hits() == h0);
	fill(16); save(OP_CACHED, 1, 1, true, 0, "after failure (disk)"); assert(hits() == h0);
	/* Deferred mode keeps the failed handle conservative (blocked/poisoned
	 * state refuses the memo); the bytes above are the proof either way. */
	fill(17); save(OP_CACHED, 1, 1, true, 0, "re-armed hit");
	assert(hits() == h0 + 1 || (mode == 2 && hits() == h0));
	/* A new handle instance never inherits a watch. */
	close_both(); open_both(); h0 = hits();
	fill(18); save(OP_CACHED, 1, 1, true, 0, "after reopen (disk)"); assert(hits() == h0);
	fill(19); save(OP_CACHED, 1, 1, true, 0, "hit after reopen"); assert(hits() == h0 + 1);
	close_both();
	compare("closed");
	for(unsigned arm = 0; arm < 2; arm++) test_fixture_dir_cleanup(root[arm]);
	printf("mode %s: %u steps equal, memo hits %u\n", G9IO_ARM_NAME[mode], steps, hits());
}
/* r_memo3 first-checkpoint stall (real Wii: one 402 ms barrier wait inside
 * a 425 ms save). A plain save's payload write has completed and RETIRED,
 * but its publication/fsync has not run, so the range lies beyond the
 * conservative libfat-visible length while other ops of the handle are still
 * queued. The next read-back of that record used to drain the WHOLE handle.
 * Here the worker is HELD behind a queued op: the old code blocked until the
 * watchdog thread released it (a counted barrier wait); the fix reads the
 * retired bytes from the same stream with no handle drain. Bytes and ordered
 * persistence must still equal the inline reference. */
static volatile bool s_released;
static void* release_later(void* arg) {
	(void)arg;
	thread_msleep(3000);
	s_released = true;
	region_archive_test_io_hold(false);
	return NULL;
}
static void backlog_sequence(int mode, unsigned arm) {
	g9io_arm(mode);
	char name[64];
	snprintf(name, sizeof name, "g9_rbmemo_backlog_%s", G9IO_ARM_NAME[mode]);
	test_fixture_dir(name, root[arm], sizeof root[arm]);
	G9IO_MKDIR(root[arm]); G9IO_MKDIR(test_fixture_join(root[arm], "/region"));
	string_t level; string_init_set_str(level, root[arm]);
	struct region_archive* a = &ra[arm];
	assert(region_archive_create_new(a, level, 0, 0, WORLD_DIM_OVERWORLD));
	string_clear(level);
	g9io_trace_begin(&traces[arm], root[arm]);
	fill(30); assert(region_archive_set_chunk(a, 1, 1, &sc, &te, &entities));
	assert(region_archive_sync(a));
	region_archive_batch_begin(a);
	fill(31); assert(region_archive_set_chunk(a, 1, 1, &sc, &te, &entities));
	if(mode == 2) { /* the payload write completes; its publication waits */
		for(unsigned k = 0; k < 2000 && region_archive_test_io_incomplete(); k++)
			thread_msleep(1);
		assert(!region_archive_test_io_incomplete());
		region_archive_test_io_hold(true);
	}
	fill(32); assert(region_archive_set_chunk(a, 2, 1, &sc, NULL, &entities));
	uint32_t before[REGION_DEFERRED_STATS], after[REGION_DEFERRED_STATS];
	region_archive_deferred_stats(before);
	struct thread rel;
	s_released = false;
	if(mode == 2) {
		assert(region_archive_test_io_incomplete() == 1);
		assert(thread_create(&rel, release_later, NULL, 64));
	}
	fill(33); assert(region_archive_set_chunk(a, 1, 1, &sc, &te, &entities));
	region_archive_deferred_stats(after);
	if(mode == 2) {
		const bool stalled = s_released;
		printf("backlog read-back: barrier waits +%u, retired-payload reads +%u, "
			"SD-mutex reads +%u, %s\n",
			after[5] - before[5], after[10] - before[10], after[11] - before[11],
			stalled ? "STALLED until the worker was released" : "no stall");
		assert(!stalled && after[5] == before[5] && after[10] > before[10]
			&& after[11] == before[11]);
		region_archive_test_io_hold(false);
		thread_join(&rel);
	}
	assert(region_archive_batch_end(a));
	assert(region_archive_sync(a));
	assert(region_archive_destroy(a));
	g9io_trace_end();
}
static void run_backlog(void) {
	backlog_sequence(0, 0);
	backlog_sequence(2, 1);
	assert(g9io_trace_equal(&traces[0], &traces[1], "backlog persistence order"));
	compare("backlog read-back vs inline");
	for(unsigned arm = 0; arm < 2; arm++) test_fixture_dir_cleanup(root[arm]);
	g9io_arm(0);
}
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	region_archive_test_clock(0x5F5E1234u);
	sc.ids = ids; sc.metadata = meta; sc.lighting_sky = sky;
	sc.lighting_torch = torch; sc.heightmap = height;
	chest = (struct te_chest){.x = 17, .y = 63, .z = 21};
	furnace = (struct te_furnace){.x = 22, .y = 64, .z = 25};
	furnace.items[0] = (struct item_data){.id = 15, .count = 8};
	te = (struct chunk_tile_entities){.chests = &chest, .n_chest = 1,
		.furnaces = &furnace, .n_furnace = 1, .legacy_flat_complete = true};
	int only = argc > 1 ? atoi(argv[1]) : -1;
	for(int mode = 0; mode < 3; mode++)
		if(only < 0 || only == mode) run_mode(mode);
	if(only < 0 || only == 3) run_backlog();
	region_archive_encode_cache_free(&cache);
	g9io_arm(0);
	puts("test_g9_readback_memo_equiv: PASS");
	return 0;
}
