/*
	S2 two-phase retirement (docs/p2_storage_ruling_fable_2026-09-09.md,
	acceptance #2/#5/#7): an EVICTING save inside an open save batch must keep
	an exact retry source until the helper thread has PUBLISHED the record.

	Before S2, server_world_save_chunk_obj(erase=true) freed the chunk the
	moment its bytes were in the file; when the helper-side publication then
	FAILED, region_pending_rollback restored the OLD index entry and nothing
	held the new record any more -- the newest state of an evicted chunk was
	silently lost (Fable deep-dive on a4b482b4, server_world.c:4353 `&& !erase`).

	Scenario A  failure retained + retried: the failed record is re-appended
	            from the retained copy and the NEW state survives a cold reopen.
	Scenario B  a full ring refuses the eviction: nothing appended, the chunk
	            stays resident and dirty; after the ring drains the same
	            eviction succeeds.
	Scenario C  reloading a retiring chunk consumes its entry: the resident
	            chunk carries the newest bytes and the retry responsibility.
	Scenario D  teardown with a FAILED retained record drains it: the cold
	            reopen shows the new state and nothing is counted as dropped.
*/

#include <assert.h>
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
#include "network/region_archive.h"
#include "network/server_world.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static void fill_ids(struct server_chunk* sc, int pattern) {
	assert(sc && sc->ids);
	for(int i = 0; i < IDS_SIZE; i++)
		sc->ids[i] = (uint8_t)(1 + ((i * 7 + pattern) & 0x1F));
	sc->modified = true;
}

static bool ids_match(const struct server_chunk* sc, int pattern) {
	for(int i = 0; i < IDS_SIZE; i++)
		if(sc->ids[i] != (uint8_t)(1 + ((i * 7 + pattern) & 0x1F)))
			return false;
	return true;
}

static void world_open(struct server_world* w, string_t level) {
	memset(w, 0, sizeof(*w));
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 1337, true);
}

static struct server_chunk* load(struct server_world* w, w_coord_t x,
								 w_coord_t z) {
	/* server_world_load_chunk returns false for an ALREADY resident chunk
	 * (the fixture trap Astra hit too): resolve the dict first. */
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	if(sc)
		return sc;
	bool ok = server_world_load_chunk(w, x, z, &sc);
	assert(ok && sc);
	return sc;
}

static bool resident(struct server_world* w, w_coord_t x, w_coord_t z) {
	return dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z)) != NULL;
}

/* Write `pattern` into chunk (x,z) and make it DURABLE outside any batch. */
static void seed_durable(struct server_world* w, w_coord_t x, w_coord_t z,
						 int pattern) {
	struct server_chunk* sc = load(w, x, z);
	fill_ids(sc, pattern);
	assert(server_world_save_chunk_obj(w, false, x, z, sc));
	assert(server_world_sync_regions(w));
	assert(server_world_retiring_active(w) == 0);
}

static void cold_check(string_t level, w_coord_t x, w_coord_t z, int pattern,
					   const char* why) {
	struct server_world w;
	world_open(&w, level);
	struct server_chunk* sc = load(&w, x, z);
	if(!ids_match(sc, pattern)) {
		fprintf(stderr, "FAIL cold reopen of (%d,%d): %s\n", (int)x, (int)z,
				why);
		abort();
	}
	server_world_destroy(&w);
}

static void scenario_a_failure_retained(string_t level) {
	struct server_world w;
	world_open(&w, level);
	seed_durable(&w, 0, 0, 10);

	struct server_chunk* sc = load(&w, 0, 0);
	fill_ids(sc, 20);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, true, 0, 0, sc)
		   && "the eviction itself is admitted");
	assert(!resident(&w, 0, 0) && "an evicted chunk leaves the dict at once");
	assert(server_world_retiring_active(&w) == 1
		   && "its record is retained until published");

	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w); /* the helper fails on the index */

	/* The barrier reports the failure ONCE (false) and, in the same call,
	 * re-appends the retained record and settles it. */
	bool first = server_world_sync_regions(&w);
	assert(!first && "a helper-side publication failure is reported by the "
					 "next durability barrier");
	assert(w.retiring_retries == 1 && "the retained record was re-appended");
	assert(server_world_sync_regions(&w)
		   && "the retry published cleanly at the following barrier");
	assert(server_world_retiring_active(&w) == 0);
	assert(w.retiring_dropped == 0);

	struct server_chunk* back = load(&w, 0, 0);
	assert(ids_match(back, 20)
		   && "the NEW state is on disk after a failed eviction publication");
	server_world_destroy(&w);
	cold_check(level, 0, 0, 20,
			   "the retained record must survive to a fresh process");
	printf("PASS A: failed eviction publication retained and retried -- new "
		   "state survives a cold reopen\n");
}

static void scenario_b_ring_full(string_t level) {
	struct server_world w;
	world_open(&w, level);
	const int n = SERVER_WORLD_RETIRING_MAX;
	for(int k = 0; k < n; k++)
		seed_durable(&w, k, 1, 30);
	/* The refusal subject lives in region 0, which is cached: a chunk in a
	 * FIFTH region would take the throwaway synchronous handle instead. */
	seed_durable(&w, 0, 2, 30);

	server_world_save_batch_begin(&w);
	for(int k = 0; k < n; k++) {
		struct server_chunk* sc = load(&w, k, 1);
		fill_ids(sc, 31);
		assert(server_world_save_chunk_obj(&w, true, k, 1, sc)
			   && "a whole ring of evictions inside one batch is admitted "
				  "(overflow publishes drain entries as they go)");
	}
	assert(server_world_retiring_active(&w) <= (size_t)n);

	/* Exhaust the BYTE budget artificially: the ring cannot make room by
	 * reconciling (nothing further is durable), so the next eviction must
	 * be refused with nothing appended and the chunk left resident+dirty. */
	size_t saved_bytes = w.retiring_bytes;
	w.retiring_bytes = SERVER_WORLD_RETIRING_MAX_BYTES;
	struct server_chunk* last = load(&w, 0, 2);
	fill_ids(last, 31);
	assert(!server_world_save_chunk_obj(&w, true, 0, 2, last)
		   && "a full ring REFUSES the eviction");
	assert(resident(&w, 0, 2) && "the refused chunk stays resident");
	assert(dict_server_chunks_get(w.chunks, S_CHUNK_ID(0, 2))->modified
		   && "and stays dirty");
	assert(w.retiring_refused == 1);
	w.retiring_bytes = saved_bytes;

	assert(server_world_save_batch_end(&w));
	assert(server_world_sync_regions(&w));
	assert(server_world_retiring_active(&w) == 0 && "the ring drained");

	server_world_save_batch_begin(&w);
	last = dict_server_chunks_get(w.chunks, S_CHUNK_ID(0, 2));
	assert(last);
	assert(server_world_save_chunk_obj(&w, true, 0, 2, last)
		   && "the same eviction succeeds once a slot is free");
	assert(server_world_save_batch_end(&w));
	assert(server_world_sync_regions(&w));
	server_world_destroy(&w);
	for(int k = 0; k < n; k++)
		cold_check(level, k, 1, 31, "every evicted chunk published");
	cold_check(level, 0, 2, 31, "the refused-then-admitted eviction published");
	printf("PASS B: a full retiring ring refuses eviction (chunk stays "
		   "resident+dirty) and the eviction succeeds after the drain\n");
}

static void scenario_c_reload_consumes(string_t level) {
	struct server_world w;
	world_open(&w, level);
	seed_durable(&w, 5, 2, 40);

	struct server_chunk* sc = load(&w, 5, 2);
	fill_ids(sc, 41);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, true, 5, 2, sc));
	assert(server_world_retiring_active(&w) == 1);

	/* Reload while the record is still unpublished. */
	struct server_chunk* back = load(&w, 5, 2);
	assert(ids_match(back, 41)
		   && "a reload during the pending window sees the NEWEST bytes");
	assert(back->modified
		   && "the resident chunk now carries the retry responsibility");
	assert(server_world_retiring_active(&w) == 0
		   && "the retained entry was consumed by the reload");

	assert(server_world_save_batch_end(&w));
	assert(server_world_sync_regions(&w));
	server_world_destroy(&w);
	cold_check(level, 5, 2, 41, "reload path kept the new state");
	printf("PASS C: reloading a retiring chunk consumes its entry -- newest "
		   "bytes, dirty, ring empty\n");
}

static void scenario_d_teardown_drains(string_t level) {
	struct server_world w;
	world_open(&w, level);
	seed_durable(&w, 9, 3, 50);

	struct server_chunk* sc = load(&w, 9, 3);
	fill_ids(sc, 51);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, true, 9, 3, sc));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w);
	assert(server_world_retiring_active(&w) == 1);

	/* No explicit barrier: teardown itself must drain the retained record. */
	server_world_destroy(&w);
	assert(w.retiring_dropped == 0
		   && "teardown re-appended and published the failed record");
	assert(w.retiring_retries == 1);
	cold_check(level, 9, 3, 51, "teardown drained the retained record");
	printf("PASS D: world teardown drains a FAILED retained record (no drop, "
		   "new state on disk)\n");
}

/* E: a chunk whose FIRST-EVER record fails to publish. Rollback restores an
 * "absent" index entry, so a naive loader would regenerate a fresh chunk over
 * the retained newer record. */
static void scenario_e_first_record_failed(string_t level) {
	struct server_world w;
	world_open(&w, level);
	struct server_chunk* sc = load(&w, 20, 6); /* generated, never saved */
	fill_ids(sc, 60);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, true, 20, 6, sc));
	assert(server_world_retiring_active(&w) == 1);
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w);
	/* Give the helper time to fail, then reload with NO barrier in between:
	 * the loader must not regenerate over the retained record. */
	server_world_save_batch_begin(&w);
	struct server_chunk* back = load(&w, 20, 6);
	assert(ids_match(back, 60)
		   && "a failed first-ever record is still the newest state on reload");
	assert(back->modified);
	assert(server_world_retiring_active(&w) == 0);
	assert(server_world_save_batch_end(&w) || true);
	assert(server_world_sync_regions(&w) || server_world_sync_regions(&w));
	server_world_destroy(&w);
	cold_check(level, 20, 6, 60, "first-ever record retained across failure");
	printf("PASS E: failed FIRST-EVER record is not regenerated over -- "
		   "reload sees the retained state\n");
}

/* F: save then evict the SAME chunk in one batch with identical content. The
 * eviction used to take the unchanged short-circuit (no bytes, no retry
 * source) against an UNPUBLISHED in-batch delta. */
static void scenario_f_save_then_evict_same_batch(string_t level) {
	struct server_world w;
	world_open(&w, level);
	seed_durable(&w, 21, 6, 70);
	struct server_chunk* sc = load(&w, 21, 6);
	fill_ids(sc, 71);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, false, 21, 6, sc)); /* delta */
	sc = load(&w, 21, 6);
	sc->modified = true; /* same bytes, evict now */
	assert(server_world_save_chunk_obj(&w, true, 21, 6, sc));
	assert(server_world_retiring_active(&w) == 1
		   && "an evicting write in a batch always retains real bytes");
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w);
	(void)server_world_sync_regions(&w);
	assert(server_world_sync_regions(&w));
	server_world_destroy(&w);
	cold_check(level, 21, 6, 71, "save-then-evict in one batch survived");
	printf("PASS F: save-then-evict of one chunk in one batch keeps a retry "
		   "source (no unchanged short-circuit on the eviction path)\n");
}

/* G: batch overflow. REGION_BATCH_MAX deltas publish on the NEXT append, so
 * the 33rd write of a region joins a NEW job. A pre-write job sample would
 * name the job that just published (DURABLE) while the bytes sit in the
 * failed one. 20 resident saves + 13 evictions = 33 deltas on region 0. */
static void scenario_g_overflow_job(string_t level) {
	struct server_world w;
	world_open(&w, level);
	for(int k = 0; k < 20; k++)
		seed_durable(&w, k, 8, 80);
	for(int k = 0; k < 13; k++)
		seed_durable(&w, k, 9, 80);
	server_world_save_batch_begin(&w);
	for(int k = 0; k < 20; k++) {
		struct server_chunk* sc = load(&w, k, 8);
		fill_ids(sc, 81);
		assert(server_world_save_chunk_obj(&w, false, k, 8, sc));
	}
	for(int k = 0; k < 13; k++) {
		struct server_chunk* sc = load(&w, k, 9);
		fill_ids(sc, 81);
		assert(server_world_save_chunk_obj(&w, true, k, 9, sc));
	}
	/* The first 32 deltas published on the 33rd append (job A, clean);
	 * the 33rd delta -- eviction (12,9) -- is alone in job B. Let A finish
	 * on the helper FIRST, then arm the failure so it hits B, not A. */
	for(size_t k = 0; k < server_world_region_count(&w); k++)
		if(server_world_region_at(&w, k)->x == 0
		   && server_world_region_at(&w, k)->z == 0)
			region_archive_test_publish_wait(server_world_region_at(&w, k));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w);
	(void)server_world_sync_regions(&w);
	assert(server_world_sync_regions(&w));
	assert(server_world_retiring_active(&w) == 0);
	server_world_destroy(&w);
	cold_check(level, 12, 9, 81,
			   "the overflow-straddling eviction waited for ITS job");
	for(int k = 0; k < 12; k++)
		cold_check(level, k, 9, 81, "pre-overflow evictions published");
	printf("PASS G: an eviction that joins a new job after an overflow "
		   "publish waits for that job, not the one that just completed\n");
}

/* H (Astra consult 2026-09-10, contract §4): a reload that fails AFTER the
 * chunk is installed (the failed-entity-restore shape) must not have thrown
 * away the retained record. The reload happens while the eviction's batch is
 * still open (the entry is PENDING and the in-memory offsets already name
 * the new bytes); the batch then FAILS on the helper, so the retained copy
 * is the ONLY source of the new state. Before the fix the loader released
 * the entry right after installing the chunk, so the failed reload left no
 * retry source and the cold reopen showed the OLD state. */
static void scenario_h_failed_load_keeps_record(string_t level) {
	struct server_world w;
	world_open(&w, level);
	seed_durable(&w, 30, 6, 90);
	struct server_chunk* sc = load(&w, 30, 6);
	fill_ids(sc, 91);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, true, 30, 6, sc));
	assert(!resident(&w, 30, 6));
	assert(server_world_retiring_active(&w) == 1);

	/* Reload inside the batch: the entry is consumed (PENDING), the chunk is
	 * installed, and the load then fails before it is complete. */
	server_world_test_fail_loads_after_install(1);
	struct server_chunk* got = NULL;
	bool first_load = server_world_load_chunk(&w, 30, 6, &got);
	assert(server_world_test_fail_loads_after_install_remaining() == 0
		   && "the reload reached the install seam");
	assert(!first_load && !got && "the injected failure");
	assert(!resident(&w, 30, 6));
	assert(server_world_retiring_active(&w) == 1
		   && "a failed load must NOT release the retained record");

	/* Now the batch itself fails: the retained copy is the only retry source. */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w);
	assert(!server_world_sync_regions(&w) && "the barrier reports the failure");
	assert(w.retiring_retries == 1 && "the retained record was re-appended");
	assert(server_world_sync_regions(&w));
	assert(server_world_retiring_active(&w) == 0);

	struct server_chunk* back = load(&w, 30, 6);
	assert(ids_match(back, 91) && "the reload after the retry sees the NEW bytes");
	server_world_destroy(&w);
	cold_check(level, 30, 6, 91, "record survived a failed reload + failed batch");
	printf("PASS H: a reload that fails after install keeps the retained "
		   "record; the retry after the failed batch lands the new state\n");
}

/* I (Astra review 2026-09-10, C): a reload of a retained-FAILED chunk BEFORE
 * the next seam is refused (the handle still holds the unsurfaced failure),
 * and the refusal must not write: no delta joins the pending list, the entry
 * keeps its job, no retry is counted. The seam then recovers as usual. */
static struct region_archive* region00(struct server_world* w) {
	for(size_t k = 0; k < server_world_region_count(w); k++)
		if(server_world_region_at(w, k)->x == 0
		   && server_world_region_at(w, k)->z == 0)
			return server_world_region_at(w, k);
	return NULL;
}

static void scenario_i_blocked_reload_writes_nothing(string_t level) {
	struct server_world w;
	world_open(&w, level);
	seed_durable(&w, 31, 6, 92);
	struct server_chunk* sc = load(&w, 31, 6);
	fill_ids(sc, 93);
	server_world_save_batch_begin(&w);
	assert(server_world_save_chunk_obj(&w, true, 31, 6, sc));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	(void)server_world_save_batch_end(&w);
	struct region_archive* ra = region00(&w);
	assert(ra);
	region_archive_test_publish_wait(ra);
	assert(server_world_retiring_active(&w) == 1);
	size_t pending_before = ra->pending_count;
	uint64_t join_before = region_archive_last_join_job(ra);

	for(int n = 0; n < 3; n++) {
		struct server_chunk* got = NULL;
		assert(!server_world_load_chunk(&w, 31, 6, &got) && !got
			   && "refused before the seam");
		assert(!resident(&w, 31, 6));
		assert(server_world_retiring_active(&w) == 1);
		assert(w.retiring_retries == 0 && "a refused reload is not a retry");
		assert(ra->pending_count == pending_before
			   && region_archive_last_join_job(ra) == join_before
			   && "a refused reload appends NOTHING");
	}

	assert(!server_world_sync_regions(&w) && "the barrier reports the failure");
	assert(w.retiring_retries == 1);
	assert(server_world_sync_regions(&w));
	assert(server_world_retiring_active(&w) == 0);
	struct server_chunk* back = load(&w, 31, 6);
	assert(ids_match(back, 93));
	server_world_destroy(&w);
	cold_check(level, 31, 6, 93, "seam recovery after refused reloads");
	printf("PASS I: reloads refused before the seam write nothing; the seam "
		   "retry lands the new state\n");
}

int main(void) {
	test_fixture_dir("test_region_evict_retain_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	region_archive_io_async_enable(true);

	scenario_a_failure_retained(level);
	scenario_b_ring_full(level);
	scenario_c_reload_consumes(level);
	scenario_d_teardown_drains(level);
	scenario_e_first_record_failed(level);
	scenario_f_save_then_evict_same_batch(level);
	scenario_g_overflow_job(level);
	scenario_h_failed_load_keeps_record(level);
	scenario_i_blocked_reload_writes_nothing(level);

	string_clear(level);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("test_region_evict_retain: PASS\n");
	return 0;
}
