/* Copyright (c) 2026 CaveWii_v2 contributors
 * G9 R19 step 2 (ASTRA-G9-R19-IO): SAVE-IO DURABILITY of the deferred worker
 * (experimental.g9_save_io_int), against the synchronous reference.
 *
 * Every chunk the test tracks carries a REVISION byte at one fixed cell. A
 * revision is "acknowledged" only when a barrier that covers it returned
 * success (checkpoint + sync_regions) while the chunk was clean. After every
 * barrier, and after every fault, the save directory is COPIED and the copy is
 * reopened TWICE; each open must
 *   - succeed, with no dangling index (every non-zero header entry decodes),
 *   - give every tracked chunk a revision that was really written (old/new,
 *     never torn, never foreign) and never older than its acknowledged one
 *     (no lost acknowledged prefix, no early barrier success),
 *   - agree with the other open (recovery is idempotent).
 *
 * Cases (argv[1]):
 *   faults    -- one injected fault per round, then a clean retry round, in
 *                the inline reference AND the deferred arm: payload short
 *                write / ENOSPC, payload flush, journal write, header flush
 *                (rolled back from the journal), timestamp write, worker
 *                write / publish / fsync failures, op-allocation failure,
 *                sync failure. A failed round must not acknowledge anything
 *                it did not make durable, and the retry must converge.
 *   stalls    -- the worker HELD (deterministic stall): read-after-write of
 *                an evicted chunk served from its queued record; a stall past
 *                the next batch end (the server waits, counted, in order);
 *                queue exhaustion (backpressure waits, nothing dropped,
 *                coalesced or reordered); final bytes equal the reference.
 *   powercut  -- a power cut at EVERY persistence transition: the save
 *                directory is copied the instant each fsync / journal record
 *                / sector flip / retire / rollback / journal open or remove
 *                completes (on whichever thread performed it), in the inline
 *                reference and in the deferred arm. Every image is reopened
 *                twice with the rules above (acknowledged = acknowledged
 *                BEFORE that cut), and the sequence of states each region
 *                recovers to must be identical in both arms: the worker
 *                passes through exactly the reference's durable prefixes.
 *                Part 2 is the REL-12 child-process KILL witness for the
 *                shipped g9_save_io_int = 0 settings, with its exact
 *                durability boundary (see "powercut, part 2" below). */
#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"

#include "block/blocks.h"
#include "network/server_world.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "item/items.h"
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
#include "platform/time.h"
#include "cNBT/buffer.h"

#include <m-lib/m-string.h>

#define CLOCK_STAMP 0x5F5E1234u
#define REV_CELL (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT - 1)
#define REV_BASE 100

static unsigned failures;
static void check(bool cond, const char* label) {
	if(!cond) {
		printf("FAIL: %s\n", label);
		failures++;
	}
}

/* ---- tracked chunks ------------------------------------------------------ */
#define NTRACK 6
struct tracked {
	w_coord_t x, z;
	uint8_t mem;      /* revision in memory */
	uint8_t acked;    /* newest acknowledged durable revision (0 = none) */
	bool written[256];
};
static struct tracked T[NTRACK];
static const w_coord_t TRACK_XZ[NTRACK][2] = {
	{0, 0}, {1, 0}, {5, 3}, {40, 0}, {41, 1}, {70, 2}};

static char s_root[200], s_copy[220];
/* Exact encoded-record palette from every inline-reference crash image.
 * The existing MAX_CUTS bound is 600; two reopens only add distinct bytes. */
#define PREFIX_RECORDS 600
static struct buffer s_prefix[NTRACK][PREFIX_RECORDS];
static unsigned s_prefix_n[NTRACK];
static int s_prefix_arm = -1;
static void check_prefix_record(struct region_archive* ra, size_t k,
	w_coord_t x, w_coord_t z) {
	uint32_t offset = ra->offsets[(x & 31) + (z & 31) * 32];
	uint8_t framing[5];
	bool ok = fseek(ra->file, (long)(offset >> 8) * REGION_SECTOR_SIZE, SEEK_SET) == 0
		&& fread(framing, sizeof framing, 1, ra->file) == 1;
	uint32_t len = ok ? ((uint32_t)framing[0] << 24 | (uint32_t)framing[1] << 16
		| (uint32_t)framing[2] << 8 | framing[3]) : 0;
	ok &= len > 1 && len <= (offset & 255) * REGION_SECTOR_SIZE - 4 && framing[4] == 2;
	check(ok, "recovered encoded record has exact valid framing");
	if(!ok) return;
	len--;
	uint8_t* bytes = malloc(len);
	assert(bytes);
	ok = fread(bytes, len, 1, ra->file) == 1;
	check(ok, "complete recovered encoded record readable");
	bool found = false;
	for(unsigned j = 0; ok && j < s_prefix_n[k]; j++)
		found |= s_prefix[k][j].len == len && !memcmp(s_prefix[k][j].data, bytes, len);
	if(ok && s_prefix_arm == 0 && !found) {
		assert(s_prefix_n[k] < PREFIX_RECORDS);
		s_prefix[k][s_prefix_n[k]++] = (struct buffer){bytes, len, len};
		bytes = NULL;
	} else if(ok && s_prefix_arm != 0)
		check(found, "every recovered byte equals an allowed reference old/new record");
	free(bytes);
}


static struct server_chunk* load(struct server_world* w, w_coord_t x,
								 w_coord_t z) {
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	if(sc)
		return sc;
	assert(server_world_load_chunk(w, x, z, &sc) && sc);
	return sc;
}
static void bump(struct server_world* w, size_t k) {
	struct server_chunk* sc = load(w, T[k].x, T[k].z);
	T[k].mem = (uint8_t)(T[k].mem + 1);
	T[k].written[T[k].mem] = true;
	sc->ids[REV_CELL] = (uint8_t)(REV_BASE + T[k].mem);
	sc->modified = true;
}

/* Reopen the COPY of the save directory and read every tracked chunk's
 * revision straight from its region file; also walk every header entry of
 * those regions (no dangling index). Returns a digest of what it saw. */
static uint64_t reopen_once(const char* root, uint8_t out[NTRACK],
							const char* label) {
	string_t level;
	string_init_set_str(level, root);
	uint64_t digest = G9IO_FNV_BASIS;
	struct server_chunk sc;
	for(size_t k = 0; k < NTRACK; k++) {
		out[k] = 0;
		struct region_archive ra;
		memset(&ra, 0, sizeof ra);
		w_coord_t rx = CHUNK_REGION_COORD(T[k].x), rz = CHUNK_REGION_COORD(T[k].z);
		if(!region_archive_create(&ra, level, rx, rz, WORLD_DIM_OVERWORLD))
			continue; /* region never created: every revision absent */
		/* No dangling index anywhere in this region. */
		for(int lx = 0; lx < REGION_SIZE; lx++)
			for(int lz = 0; lz < REGION_SIZE; lz++) {
				w_coord_t cx = rx * REGION_SIZE + lx, cz = rz * REGION_SIZE + lz;
				bool exists = false;
				if(!region_archive_contains(&ra, cx, cz, &exists) || !exists)
					continue;
				memset(&sc, 0, sizeof sc);
				bool ok = region_archive_get_chunk(&ra, cx, cz, &sc, NULL, NULL);
				if(!ok)
					printf("  [%s] dangling index entry at chunk (%d,%d)\n",
						   label, (int)cx, (int)cz);
				check(ok, "no dangling index entry after reopen");
				if(ok) {
					if(cx == T[k].x && cz == T[k].z) {
						uint8_t v = sc.ids[REV_CELL];
						out[k] = v >= REV_BASE ? (uint8_t)(v - REV_BASE) : 0;
						if(s_prefix_arm >= 0) check_prefix_record(&ra, k, cx, cz);
					}
					server_chunk_arrays_free(&sc);
				}
			}
		check(region_archive_destroy(&ra), "reopened region closes");
		digest = g9io_fnv(digest, &out[k], 1);
	}
	string_clear(level);
	return digest;
}
/* Copy, reopen twice, check the revision rules. `barrier_ok` = the barrier
 * just returned success: every CLEAN chunk's memory revision is now
 * acknowledged and must be on disk. */
static void verify(struct server_world* w, bool barrier_ok, const char* label) {
	if(barrier_ok)
		for(size_t k = 0; k < NTRACK; k++) {
			struct server_chunk* sc
				= dict_server_chunks_get(w->chunks, S_CHUNK_ID(T[k].x, T[k].z));
			if(sc && !sc->modified)
				T[k].acked = T[k].mem;
		}
	test_fixture_rm_rf(s_copy);
	g9io_copy_tree(s_root, s_copy);
	uint8_t a[NTRACK], b[NTRACK];
	uint64_t da = reopen_once(s_copy, a, label);
	uint64_t db = reopen_once(s_copy, b, label);
	check(da == db && !memcmp(a, b, sizeof a),
		  "a second reopen recovers exactly what the first did");
	for(size_t k = 0; k < NTRACK; k++) {
		bool real = a[k] == 0 ? T[k].acked == 0 : T[k].written[a[k]];
		if(!real || a[k] < T[k].acked)
			printf("  [%s] chunk (%d,%d): disk rev %u, acked %u, mem %u\n",
				   label, (int)T[k].x, (int)T[k].z, a[k], T[k].acked, T[k].mem);
		check(real, "a recovered revision was really written");
		check(a[k] >= T[k].acked,
			  "no acknowledged revision is lost (no early barrier success)");
	}
	test_fixture_rm_rf(s_copy);
}

static void open_fixture(const char* id, struct server_world* w) {
	test_fixture_dir(id, s_root, sizeof s_root);
	G9IO_MKDIR(s_root);
	snprintf(s_copy, sizeof s_copy, "%s_copy", s_root);
	memset(T, 0, sizeof T);
	for(size_t k = 0; k < NTRACK; k++) {
		T[k].x = TRACK_XZ[k][0];
		T[k].z = TRACK_XZ[k][1];
	}
	string_t level;
	string_init_set_str(level, s_root);
	memset(w, 0, sizeof *w);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	/* F4-RNG: World.rand is runtime-seeded (new Random()); chunk population
	 * draws on it. Pin it so every arm generates the same chunk bytes. */
	jrand_set_seed(&w->rand, INT64_C(0x6910D0AB));
	string_clear(level);
}
static void close_fixture(struct server_world* w) {
	server_world_destroy(w);
	test_fixture_dir_cleanup(s_root);
}

/* One save round: bump every tracked chunk, save them in one batch window,
 * close it, checkpoint + sync. Returns the barrier result. */
static bool round_trip(struct server_world* w) {
	server_world_save_batch_begin(w);
	for(size_t k = 0; k < NTRACK; k++) {
		bump(w, k);
		(void)server_world_save_chunk(w, false, T[k].x, T[k].z);
	}
	bool end_ok = server_world_save_batch_end(w);
	bool ok = server_world_checkpoint_chunks(w);
	ok = server_world_sync_regions(w) && ok && end_ok;
	return ok;
}

/* ---- case: faults --------------------------------------------------------- */
static uint32_t read_be32(const uint8_t* p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
		| (uint32_t)p[2] << 8 | p[3];
}

/* Inspect the physical header before a recovery open can salvage it. */
static void check_pending_index(void) {
	char path[256];
	snprintf(path, sizeof path, "%s/region/r.0.0.mcr", s_root);
	FILE* f = fopen(path, "rb");
	assert(f);
	assert(!fseek(f, 0, SEEK_END));
	long size = ftell(f);
	assert(size >= 2 * REGION_SECTOR_SIZE);
	rewind(f);
	uint8_t header[REGION_SECTOR_SIZE];
	assert(fread(header, sizeof header, 1, f) == 1);
	for(size_t i = 0; i < REGION_SECTOR_SIZE / 4; i++) {
		uint32_t entry = read_be32(header + i * 4);
		if(!entry) continue;
		uint32_t start = entry >> 8, sectors = entry & 255;
		bool valid = start >= 2 && sectors && (uint64_t)start * REGION_SECTOR_SIZE + 5 <= (uint64_t)size;
		check(valid, "physical index points inside complete payload framing");
		if(!valid) continue;
		uint8_t framing[5];
		assert(!fseek(f, (long)start * REGION_SECTOR_SIZE, SEEK_SET));
		assert(fread(framing, sizeof framing, 1, f) == 1);
		uint32_t len = read_be32(framing);
		check(len > 1 && len <= sectors * REGION_SECTOR_SIZE - 4
			&& (uint64_t)start * REGION_SECTOR_SIZE + 4 + len <= (uint64_t)size
			&& framing[4] == 2, "physical index never names a partial record");
		for(size_t j = 0; j < i; j++) {
			uint32_t prev = read_be32(header + j * 4);
			if(prev) check(start >= (prev >> 8) + (prev & 255)
				|| (prev >> 8) >= start + sectors, "indexed sectors never overlap");
		}
	}
	assert(!fclose(f));
}

/* ci-1101/42: a clean chunk still owns its pending non-evicting save.
 * Eviction must transfer that ownership before freeing its arrays. */
static void fail_pending_publication(void) {
	region_archive_test_rio_fail_next(2);
}

static void check_old_pending_revision(void) {
	g9io_copy_tree(s_root, s_copy);
	uint8_t rev[NTRACK];
	(void)reopen_once(s_copy, rev, "unpublished index stays OLD");
	check(rev[0] == T[0].acked,
		"unpublished NEW bytes never replace the durable OLD index");
	test_fixture_rm_rf(s_copy);
}

static void pending_clean_eviction(unsigned mode) {
	unsigned failures_before = failures;
	g9io_arm(2);
	struct server_world w;
	char id[64];
	snprintf(id, sizeof id, "g9io_pending_clean_evict_%u", mode);
	open_fixture(id, &w);
	bump(&w, 0);
	assert(server_world_checkpoint_chunks(&w));
	assert(server_world_sync_regions(&w));
	verify(&w, true, "pending eviction durable OLD");

	region_archive_test_io_hold(true);
	server_world_save_batch_begin(&w);
	bump(&w, 0);
	assert(server_world_save_chunk(&w, false, T[0].x, T[0].z));
	struct server_chunk* sc = load(&w, T[0].x, T[0].z);
	assert(!sc->modified && sc->save_pending);
	if(mode == 5)
		assert(server_world_save_batch_end(&w));
	if(mode == 3) {
		size_t bytes = w.retiring_bytes;
		w.retiring_bytes = SERVER_WORLD_RETIRING_MAX_BYTES;
		bool evicted = server_world_save_chunk(&w, true, T[0].x, T[0].z);
		check(!evicted, "a full retry ring refuses clean pending eviction");
		sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(T[0].x, T[0].z));
		check(sc && !sc->modified && sc->save_pending,
			"ring admission failure preserves the resident pending owner");
		w.retiring_bytes = bytes;
		if(!sc) sc = load(&w, T[0].x, T[0].z);
	}
	assert(server_world_save_chunk(&w, true, T[0].x, T[0].z));
	check(server_world_retiring_active(&w) == 1,
		"clean pending eviction transfers retry ownership");
	if(mode == 1 || mode == 4) {
		sc = load(&w, T[0].x, T[0].z);
		check(sc->ids[REV_CELL] == REV_BASE + T[0].mem,
			"pending retained reload sees the NEWER content");
		check(!sc->modified && sc->save_pending,
			"retained reload transfers the exact pending job without a rewrite");
		if(mode == 4) bump(&w, 0); /* supersede the consumed retry owner */
		assert(server_world_save_chunk(&w, true, T[0].x, T[0].z));
		check(server_world_retiring_active(&w) == 1,
			"second clean eviction transfers ownership back to the pipeline");
	}
	check_pending_index();
	verify(&w, false, "held pending eviction preserves OLD index");
	check_old_pending_revision();
	uint32_t warnings = region_save_failure_count();
	if(mode == 1)
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH);
	else
		region_archive_test_rio_fail_next(2);
	if(mode == 2)
		region_archive_test_payload_hook(fail_pending_publication);
	region_archive_test_io_hold(false);
	(void)server_world_save_batch_end(&w);
	check(!server_world_sync_regions(&w), "publication failure is reported");
	check_pending_index();
	verify(&w, false, "pending eviction failed publication");
	if(mode == 2) {
		check_old_pending_revision();
		for(unsigned attempt = 0; attempt < 3; attempt++) {
			uint32_t before = w.retiring_retries;
			check(!server_world_sync_regions(&w), "persistent failure returns to caller");
			check(w.retiring_retries - before <= 2,
				"each barrier bounds retries to two single-entry ring walks");
			check(server_world_retiring_active(&w) == 1 && !w.retiring_dropped,
				"exhausted barrier retries preserve the newest payload owner");
			check_pending_index();
		}
		region_archive_test_payload_hook(NULL);
		(void)server_world_sync_regions(&w); /* report the last failed attempt */
	}
	check(region_save_failure_count() > warnings,
		"publication failure reaches the visible save-fail warning path");
	check(server_world_sync_regions(&w), "pending eviction retry publishes");
	check_pending_index();
	uint8_t rev[NTRACK];
	/* Inspect an isolated crash-image copy, as verify() does, instead
	 * of reopening the active region a second time. */
	g9io_copy_tree(s_root, s_copy);
	(void)reopen_once(s_copy, rev, "pending eviction recovered NEW");
	test_fixture_rm_rf(s_copy);
	check(rev[0] == T[0].mem,
		"NEWER clean-evicted content survives publication failure and reopen");
	check(server_world_retiring_active(&w) == 0 && !w.retiring_dropped,
		"confirmed publication releases retry ownership without drops");
	printf("%s pending-clean-eviction mode=%u OLD=%u NEW=%u reopened=%u "
		"retries=%u warnings=%u owners=%zu dropped=%u\n",
		failures == failures_before ? "PASS" : "FAIL", mode, T[0].acked,
		T[0].mem, rev[0], w.retiring_retries,
		region_save_failure_count() - warnings,
		server_world_retiring_active(&w), w.retiring_dropped);
	close_fixture(&w);
}

/* ci-1101/58: the shipping helper may finish publication INSIDE the pending
 * record copy's region_io_wait. Hold it before index publication, then release
 * only once the server actually waits; no sleep determines the interleaving. */
static unsigned s_publish_held, s_publish_release, s_publish_saw_wait;
static uint32_t s_publish_waits_before;
static void pending_publish_hold(int op, const char* path, uint64_t a, uint64_t b) {
	(void)path; (void)a; (void)b;
	if(op != RT_SYNC_REGION) return;
	__atomic_store_n(&s_publish_held, 1, __ATOMIC_RELEASE);
	while(!__atomic_load_n(&s_publish_release, __ATOMIC_ACQUIRE))
		thread_msleep(1);
}
static void* pending_publish_release(void* arg) {
	(void)arg;
	ptime_t start = time_get();
	uint32_t stats[24];
	do {
		region_archive_journal_stats(stats);
		if(stats[22] != s_publish_waits_before) {
			__atomic_store_n(&s_publish_saw_wait, 1, __ATOMIC_RELEASE);
			break;
		}
		thread_msleep(1);
	} while(time_diff_ms(start, time_get()) < 5000);
	/* Timeout only prevents a hung test; missing the wait is a failure. */
	__atomic_store_n(&s_publish_release, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void pending_helper_eviction(unsigned mode) {
	unsigned failures_before = failures;
	g9io_arm(1); /* shipping default: deferred IO OFF, async publication ON */
	struct server_world w;
	char id[64];
	snprintf(id, sizeof id, "g9io_helper_evict_%u", mode);
	open_fixture(id, &w);
	bump(&w, 0);
	assert(server_world_checkpoint_chunks(&w));
	assert(server_world_sync_regions(&w));
	verify(&w, true, "helper eviction durable OLD");
	server_world_save_batch_begin(&w);
	bump(&w, 0);
	assert(server_world_save_chunk(&w, false, T[0].x, T[0].z));
	struct server_chunk* sc = load(&w, T[0].x, T[0].z);
	assert(!sc->modified && sc->save_pending);
	uint64_t job = sc->save_job;
	struct region_archive* ra = server_world_chunk_region(&w, T[0].x, T[0].z);
	assert(ra);
	__atomic_store_n(&s_publish_held, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&s_publish_release, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&s_publish_saw_wait, 0, __ATOMIC_RELEASE);
	region_archive_test_trace(pending_publish_hold);
	assert(server_world_save_batch_end(&w));
	ptime_t start = time_get();
	while(!__atomic_load_n(&s_publish_held, __ATOMIC_ACQUIRE)
		&& time_diff_ms(start, time_get()) < 5000)
		thread_msleep(1);
	assert(__atomic_load_n(&s_publish_held, __ATOMIC_ACQUIRE));
	assert(region_archive_save_state(ra, job) == REGION_SAVE_PENDING);
	if(mode == 1)
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH);
	if(mode == 2) bump(&w, 0); /* newer unsaved edit than the publishing job */
	uint32_t stats[24];
	region_archive_journal_stats(stats);
	s_publish_waits_before = stats[22];
	uint32_t warnings = region_save_failure_count();
	struct thread release;
	assert(thread_create(&release, pending_publish_release, NULL, 64));
	bool evicted = server_world_save_chunk(&w, true, T[0].x, T[0].z);
	thread_join(&release);
	region_archive_test_trace(NULL);
	check(__atomic_load_n(&s_publish_saw_wait, __ATOMIC_ACQUIRE),
		"helper stays held until eviction waits for publication");
	sc = dict_server_chunks_get(w.chunks, S_CHUNK_ID(T[0].x, T[0].z));
	if(mode == 1) {
		check(region_archive_save_state(ra, job) == REGION_SAVE_FAILED,
			"injected publication really failed");
		check(!evicted && sc && sc->save_pending,
			"failed pending copy refuses eviction and keeps the resident owner");
		check(region_save_failure_count() > warnings,
			"real failed copy still reports save failure");
	} else {
		check(evicted && !sc, "publication completing during eviction permits eviction");
		check(region_archive_save_state(ra, job) == REGION_SAVE_DURABLE,
			"the exact pending job became durable");
		check(region_save_failure_count() == warnings,
			"benign publication completion never bumps save_fail_n");
		if(mode == 0)
			check(!server_world_retiring_active(&w),
				"already durable eviction needs no retry record");
	}
	/* A failed job is recovered through its resident owner; a newer dirty
	 * revision must also survive, rather than evicting against the older job. */
	(void)server_world_sync_regions(&w); /* surface the injected async error */
	assert(server_world_checkpoint_chunks(&w));
	assert(server_world_sync_regions(&w));
	verify(&w, true, "helper eviction final barrier");
	g9io_copy_tree(s_root, s_copy);
	uint8_t rev[NTRACK];
	(void)reopen_once(s_copy, rev, "helper eviction recovered NEW");
	test_fixture_rm_rf(s_copy);
	check(rev[0] == T[0].mem, "newest revision survives eviction and reopen");
	printf("%s helper-pending-eviction mode=%u waited=%u evicted=%u "
		"warnings=%u newest=%u reopened=%u\n",
		failures == failures_before ? "PASS" : "FAIL", mode,
		__atomic_load_n(&s_publish_saw_wait, __ATOMIC_ACQUIRE), (unsigned)evicted,
		region_save_failure_count() - warnings, T[0].mem, rev[0]);
	close_fixture(&w);
}

struct fault {
	const char* name;
	int stage; /* region_archive_test_fail_stage, or 0 */
	int rio;   /* region_archive_test_rio_fail_next kind, or 0 */
	unsigned fsync; /* nth covering fsync of the warm stream, or 0 */
};
static const struct fault FAULTS[] = {
	{"payload short write / ENOSPC", REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE, 0, 0},
	{"payload flush", REGION_ARCHIVE_TEST_FAIL_PAYLOAD_FLUSH, 0, 0},
	{"journal record write", REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE, 0, 0},
	{"header-sector flush (journal rollback)", REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH, 0, 0},
	{"timestamp sector", REGION_ARCHIVE_TEST_FAIL_TIMESTAMP_WRITE, 0, 0},
	{"sync barrier", REGION_ARCHIVE_TEST_FAIL_SYNC, 0, 0},
	{"worker payload write", 0, 1, 0},
	{"worker publication", 0, 2, 0},
	{"worker fsync", 0, 3, 0},
	{"op allocation", 0, 4, 0},
	{"payload covering fsync", 0, 0, 1},
	{"journal covering fsync", 0, 0, 2},
	{"header covering fsync", 0, 0, 3},
	{"final covering fsync", 0, 0, 4},
	{"committed sequence retirement", 0, 5, 0},
};
static void run_faults_arm(int arm) {
	struct server_world w;
	char id[64];
	snprintf(id, sizeof id, "g9io_faults_%s", G9IO_ARM_NAME[arm]);
	g9io_arm(arm);
	region_archive_test_clock(CLOCK_STAMP);
	open_fixture(id, &w);
	check(round_trip(&w), "clean first round");
	verify(&w, true, "clean");
	unsigned failed_rounds = 0;
	for(size_t f = 0; f < sizeof FAULTS / sizeof *FAULTS; f++) {
		char label[96];
		snprintf(label, sizeof label, "%s: %s", G9IO_ARM_NAME[arm],
				 FAULTS[f].name);
		/* The deferred-only worker faults are inert in the reference. */
		if(FAULTS[f].stage)
			region_archive_test_fail_next(
				(enum region_archive_test_fail_stage)FAULTS[f].stage);
		if(FAULTS[f].rio)
			region_archive_test_rio_fail_next(FAULTS[f].rio);
		region_archive_test_fsync_fail_after(FAULTS[f].fsync);
		bool ok = round_trip(&w);
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_NONE);
		region_archive_test_rio_fail_next(0);
		region_archive_test_fsync_fail_after(0);
		failed_rounds += !ok;
		verify(&w, ok, label);
		/* The retry round must converge whatever the fault did. */
		bool retry = round_trip(&w);
		if(!retry)
			retry = round_trip(&w);
		check(retry, "the retry round after a fault succeeds");
		verify(&w, retry, label);
		bool clean = true;
		for(size_t k = 0; k < NTRACK; k++)
			clean = clean && T[k].acked == T[k].mem;
		check(clean, "after the retry every revision is acknowledged");
		printf("  %-8s %-40s round %s, retry %s\n", G9IO_ARM_NAME[arm],
			   FAULTS[f].name, ok ? "ok" : "FAILED (reported)",
			   retry ? "ok" : "FAILED");
	}
	close_fixture(&w);
	printf("  %s: %u fault rounds reported failure\n", G9IO_ARM_NAME[arm],
		   failed_rounds);
	region_archive_test_clock(0);
	g9io_arm(1);
}

/* The real production caller, not a test-added sync after a raw save. The
 * standalone world omits actor/transport services; roof/torch edits, trial,
 * submission, completion polling and admission backpressure run production code. */
static struct server_local stress;
static unsigned stress_retries;
static unsigned stress_this_retry;
static uint32_t stress_held_seed;
static void stress_retry(struct server_local* s) {
	stress_retries++;
	if(!stress_this_retry++)
		stress_held_seed = s->rand_src.seed; /* after the real fixture edits */
	check((s->light_queue_tick == 601 || s->light_queue_tick == 1801)
		&& s->rand_src.seed == stress_held_seed,
		"rejected admission holds fixture/time/RNG without replay");
	check(dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0)) != NULL,
		"failed checkpoint retains its world and column ownership");
}
static void stress_checkpoint(struct server_world* w) {
	/* world has address-sensitive state; never copy it into a different owner. */
	assert(w == &stress.worlds[0]);
	w->local = &stress;
	w->session = &stress;
	stress.scheduled_tick_budget = 128;
	stress.light_queue_bx = 0;
	stress.light_queue_by = 96;
	stress.light_queue_bz = 0;
	stress.light_queue_tick = 600;
	stress.world_time = 6000;
	stress.rand_src.seed = 0x9E11u;
	/* Torch support drops use Beta Math.random's independent stream. */
	server_local_test_seed_math_random(0x600);
	stress_this_retry = 0;
	server_local_test_light_queue_stress_drive(&stress);
	check(stress.light_queue_tick == 601, "one fixture admission, no replay on retry");
	if(stress_this_retry)
		check(stress.rand_src.seed == stress_held_seed,
			"retry completion does not advance held RNG");
}
static uint32_t stress_returned, stress_advanced;
static void* stress_release(void* arg) {
	(void)arg;
	thread_msleep(31000);
	check(__atomic_load_n(&stress_returned, __ATOMIC_ACQUIRE),
		"R24 real stress caller returned while fsync stalled beyond N+1");
	check(__atomic_load_n(&stress_advanced, __ATOMIC_ACQUIRE),
		"simulation and distinct N+1 snapshot advance during 31-s stall");
	region_archive_test_io_hold(false);
	return NULL;
}
static void stress_finish(struct server_world* w) {
	ptime_t start = time_get();
	while(w->checkpoint_count && time_diff_us(start, time_get()) < 5000000) {
		server_world_checkpoint_poll(w);
		thread_msleep(1);
	}
	check(w->checkpoint_count == 0, "all checkpoint tokens consumed by server polling");
	check(w->checkpoint_durable == w->checkpoint_sequence,
		"DURABLE accounts for every accepted sequence");
}
static void stress_snapshot_revision(uint8_t expected, const char* label) {
	test_fixture_rm_rf(s_copy);
	g9io_copy_tree(s_root, s_copy);
	uint8_t a[NTRACK], b[NTRACK];
	uint64_t da = reopen_once(s_copy, a, label), db = reopen_once(s_copy, b, label);
	check(da == db && !memcmp(a, b, sizeof a), "R24 two reopens are identical");
	check(a[0] == expected, "immutable original checkpoint revision, not subsequent live edits");
	test_fixture_rm_rf(s_copy);
}
static void stress_caller(bool faults) {
	for(int arm = 0; arm < 3; arm++)
		for(unsigned open = 0; open < 2; open++) {
			char id[64];
			snprintf(id, sizeof id, "g9io_stress_%d_%u", arm, open);
			memset(&stress, 0, sizeof stress);
			/* IMP-TE-CAPS: growable tile slabs (all rows free) */
			if(!server_local_te_tables_init(&stress))
				abort();
			dict_entity_init(stress.entities);
			g9io_arm(arm);
			region_archive_test_clock(CLOCK_STAMP);
			open_fixture(id, &stress.worlds[0]);
			stress.worlds[0].session = &stress;
			stress.worlds[0].local = &stress;
			check(round_trip(&stress.worlds[0]), "stress durable base");
			if(open) server_world_save_batch_begin(&stress.worlds[0]);
			stress_retries = 0;
			server_local_test_stress_checkpoint_retry_hook(stress_retry);
			if(faults) {
				if(arm == 2) {
					/* Completed failed legacy payload, with its batch still
					 * open, must not become a stranded parked R24 operation. */
					server_world_save_batch_begin(&stress.worlds[0]);
					bump(&stress.worlds[0], 1);
					region_archive_test_rio_fail_next(1);
					check(server_world_save_chunk(&stress.worlds[0], false, 1, 0), "legacy batch write accepted before first checkpoint");
					ptime_t until = time_get();
					while(region_archive_test_io_incomplete() && time_diff_ms(until, time_get()) < 5000)
						thread_msleep(1);
					check(!region_archive_test_io_incomplete(), "legacy failing write completed");
					stress_checkpoint(&stress.worlds[0]);
					check(stress_retries != 0, "failed legacy handoff explicitly rejects before capture");
					stress_finish(&stress.worlds[0]);
					verify(&stress.worlds[0], false, "legacy failure handoff retry");
				}
				unsigned before_oom = stress_retries;
				bump(&stress.worlds[0], 0);
				server_world_test_light_trial_fail(1);
				stress_checkpoint(&stress.worlds[0]);
				check(stress_retries > before_oom, "snapshot OOM rejects before capture, retains same logical request");
				stress_finish(&stress.worlds[0]);
				verify(&stress.worlds[0], false, "snapshot OOM retry");
				for(size_t f = 0; f < sizeof FAULTS / sizeof *FAULTS; f++) {
					if(FAULTS[f].rio && arm != 2) continue;
					/* Alternate real open/closed batch callers for every fault. */
					if(open) server_world_save_batch_begin(&stress.worlds[0]);
					else check(server_world_save_batch_end(&stress.worlds[0]), "closed batch fault setup");
					bump(&stress.worlds[0], 0);
					uint8_t snap = T[0].mem;
					uint32_t before[SERVER_CHECKPOINT_STATS], after[SERVER_CHECKPOINT_STATS];
					server_world_checkpoint_stats(before);
					if(FAULTS[f].stage) region_archive_test_fail_next(FAULTS[f].stage);
					if(FAULTS[f].rio) region_archive_test_rio_fail_next(FAULTS[f].rio);
					region_archive_test_fsync_fail_after(FAULTS[f].fsync);
					unsigned admissions = stress_retries;
					stress_checkpoint(&stress.worlds[0]);
					if(arm == 2 && FAULTS[f].rio != 4) {
						struct server_checkpoint_token* token = &stress.worlds[0].checkpoints[stress.worlds[0].checkpoint_head];
						if(FAULTS[f].rio == 1 || FAULTS[f].rio == 5) {
							/* A separate ownership barrier may reconcile/retry first.
							 * Poll must still consume the retained error, not hide it. */
							check(!region_archive_sync(token->archive[0]), "covering barrier reports retry error");
							check(token->state == REGION_CHECKPOINT_PENDING, "barrier cannot fabricate token acknowledgement");
						}
						ptime_t start = time_get();
						while(token->state != REGION_CHECKPOINT_FAILED
							   && time_diff_us(start, time_get()) < 5000000) {
							server_world_checkpoint_poll(&stress.worlds[0]);
							thread_msleep(1);
						}
						check(token->state == REGION_CHECKPOINT_FAILED,
							"every worker IO failure consumed as FAILED, never early DURABLE");
						check(stress.worlds[0].checkpoint_durable < token->sequence,
							"dependent durable prefix cannot overtake failed checkpoint");
						bump(&stress.worlds[0], 0); /* MUST NOT become retry's snapshot */
					} else check(stress_retries > admissions, "inline/submission failure explicitly rejected and retried");
					stress_finish(&stress.worlds[0]);
					server_world_checkpoint_stats(after);
					check(after[8] > before[8] || after[9] > before[9], "failure is latched in telemetry");
					stress_snapshot_revision(snap, FAULTS[f].name);
					check(server_world_light_drain(&stress.worlds[0]), "normal drain after failed persistence advances");
					check(load(&stress.worlds[0], 0, 0)->modified,
						"completion never clears newer dirty trial/edit revision");
					/* Ownership barriers still report carried errors once. */
					if(!server_world_sync_regions(&stress.worlds[0]))
						check(server_world_sync_regions(&stress.worlds[0]), "retry barrier after consumed error");
				}
			} else if(arm == 2) {
				bump(&stress.worlds[0], 0);
				region_archive_test_io_hold(true);
				__atomic_store_n(&stress_returned, 0, __ATOMIC_RELEASE);
				__atomic_store_n(&stress_advanced, 0, __ATOMIC_RELEASE);
				struct thread rel;
				assert(thread_create(&rel, stress_release, NULL, 64));
				ptime_t start = time_get();
				stress_checkpoint(&stress.worlds[0]);
				check(time_diff_us(start, time_get()) < 30000000,
					"pending caller does not hold for stalled fsync");
				__atomic_store_n(&stress_returned, 1, __ATOMIC_RELEASE);
				struct server_checkpoint_token* first = &stress.worlds[0].checkpoints[stress.worlds[0].checkpoint_head];
				uint32_t generation = first->generation, sequence = first->sequence;
				check(first->state == REGION_CHECKPOINT_PENDING, "N is explicitly PENDING");
				check(server_world_light_drain(&stress.worlds[0]), "ordinary post-checkpoint drain advances");
				bump(&stress.worlds[0], 0);
				/* Intervening edit, eviction/reload: archive RAM visibility is
				 * newest bytes; it is never authority for recovery. */
				server_world_save_batch_begin(&stress.worlds[0]);
				bump(&stress.worlds[0], 1);
				check(server_world_save_chunk(&stress.worlds[0], true, 1, 0), "stalled edit evicts into retained ownership");
				struct server_chunk* reload = NULL;
				check(server_world_load_chunk(&stress.worlds[0], 1, 0, &reload) && reload
					&& reload->ids[REV_CELL] == REV_BASE + T[1].mem, "reload sees newest queued revision");
				for(int t = 601; t <= 1200; t++) {
					stress.world_time++;
					server_local_test_light_queue_stress_drive(&stress);
					check(server_world_light_drain(&stress.worlds[0]), "matching logical-step normal drain advances");
				}
				check(first->sequence == sequence && first->generation == generation
					&& first->state == REGION_CHECKPOINT_PENDING, "N identity/storage remains owned beyond N+1");
				check(stress.worlds[0].checkpoint_count == 2, "N+1 distinct immutable transaction queued behind N");
				__atomic_store_n(&stress_advanced, 1, __ATOMIC_RELEASE);
				/* Fill the bounded stream until admission cannot reserve a
				 * checkpoint; real caller then applies measured backpressure. */
				struct region_archive* ra = first->archive[0];
				struct buffer record = {0};
				check(region_archive_set_chunk_retain(ra, 1, 0,
					load(&stress.worlds[0], 1, 0), NULL, NULL, &record), "queue exhaustion record");
				if(!record.len) { bump(&stress.worlds[0], 1); check(region_archive_set_chunk_retain(ra, 1, 0,
					load(&stress.worlds[0], 1, 0), NULL, NULL, &record), "changed exhaustion record"); }
				while(region_archive_test_io_incomplete() < 90)
					assert(region_archive_recommit_record(ra, 1, 0, &record));
				buffer_free(&record);
				uint32_t before[SERVER_CHECKPOINT_STATS], after[SERVER_CHECKPOINT_STATS];
				server_world_checkpoint_stats(before);
				stress.light_queue_tick = 1800;
				start = time_get();
				server_local_test_light_queue_stress_drive(&stress);
				check(time_diff_us(start, time_get()) > 1000000, "queue exhaustion real admission hold is counted");
				thread_join(&rel);
				check(server_local_checkpoint_drain(&stress), "production final drain consumes the accepted backlog");
				stress_finish(&stress.worlds[0]);
				server_world_checkpoint_stats(after);
				check(after[8] > before[8], "queue exhaustion reports REJECTED, no invented success");
				check(after[7] >= 30000000 && after[3] >= 2, "pending age/backlog/completion latency reported");
				check(server_world_sync_regions(&stress.worlds[0]), "stalled final covering drain");
				verify(&stress.worlds[0], false, "R24 advancing stalled checkpoints");
			}
			if(faults && arm == 2)
				check(stress.worlds[0].checkpoint_failed && !server_local_checkpoint_drain(&stress),
					"final lifecycle report retains failed attempts after durable retries");
			server_local_test_stress_checkpoint_retry_hook(NULL);
			close_fixture(&stress.worlds[0]);
			dict_entity_clear(stress.entities);
		}
	region_archive_test_clock(0);
	g9io_arm(1);
}

/* ---- case: stalls --------------------------------------------------------- */
static unsigned s_release_ms;
static void* releaser(void* arg) {
	(void)arg;
	thread_msleep(s_release_ms);
	region_archive_test_io_hold(false);
	return NULL;
}
static void release_later(struct thread* t, unsigned ms) {
	s_release_ms = ms;
	assert(thread_create(t, releaser, NULL, 64));
}

/* Hold the REAL worker inside RT_PAYLOAD, where it still owns s_rio_disk.
 * A timeout releases the regression rather than deadlocking the test. */
static uint32_t s_disk_hold;
static void disk_hold_trace(int op, const char* path, uint64_t a, uint64_t b) {
	(void)path; (void)a; (void)b;
	if(op != RT_PAYLOAD || __atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE))
		return;
	__atomic_store_n(&s_disk_hold, 1, __ATOMIC_RELEASE);
	while(__atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE) != 3)
		thread_msleep(1);
}
static void* disk_releaser(void* arg) {
	(void)arg;
	for(unsigned i = 0; i < 500; i++) {
		if(__atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE) == 2)
			break;
		thread_msleep(1);
	}
	__atomic_store_n(&s_disk_hold, 3, __ATOMIC_RELEASE);
	return NULL;
}
static void payload_hold(void) {
	disk_hold_trace(RT_PAYLOAD, NULL, 0, 0);
}

/* The worker is paused AFTER framing, BEFORE its slow payload fwrite on the
 * SAME archive. Save a different, already published NONCANONICAL record:
 * this necessarily exercises the preservation read, not the canonical skip
 * or the queued-byte shortcut. The global disk mutex must remain held when
 * the save returns. Compare every final byte to the synchronous reference. */
enum preserve_read_mode { PRESERVE_PUBLISHED, PRESERVE_ZERO, PRESERVE_UNPUBLISHED };
static void preserve_read_stall(enum preserve_read_mode mode) {
	bool zero_reserved = mode == PRESERVE_ZERO;
	bool unpublished = mode == PRESERVE_UNPUBLISHED;
	static struct g9io_snap final[2];
	static uint8_t ids[32768], meta[16384], sky[16384], torch[16384], height[256];
	struct server_chunk sc = {.ids = ids, .metadata = meta,
		.lighting_sky = sky, .lighting_torch = torch, .heightmap = height};
	struct te_furnace furnace = {.x = 17, .y = 64, .z = 2};
	struct chunk_tile_entities te = {.furnaces = &furnace, .n_furnace = 1,
		.legacy_flat_complete = true};
	struct chunk_entities entities = {0};
	for(unsigned arm = 0; arm < 2; arm++) {
		bool deferred = arm != 0;
		g9io_arm(deferred ? 2 : 0);
		region_archive_test_clock(CLOCK_STAMP);
		char root[200];
		test_fixture_dir("g9io_preserve_stall", root, sizeof root);
		assert(G9IO_MKDIR(root) == 0);
		assert(G9IO_MKDIR(test_fixture_join(root, "/region")) == 0);
		string_t level;
		string_init_set_str(level, root);
		struct region_archive ra;
		if(zero_reserved) {
			/* Existing uninitialised sector: NBT framing fails, then the
			 * all-zero scanner must also avoid the worker's disk mutex. */
			FILE* file = fopen(test_fixture_join(root, "/region/r.0.0.mcr"), "wb");
			assert(file);
			uint8_t zeros[REGION_SECTOR_SIZE] = {0};
			zeros[6] = 2; zeros[7] = 1; /* slot 1: offset 2, one sector */
			assert(fwrite(zeros, sizeof zeros, 1, file) == 1);
			memset(zeros, 0, sizeof zeros);
			assert(fwrite(zeros, sizeof zeros, 1, file) == 1);
			assert(fwrite(zeros, sizeof zeros, 1, file) == 1);
			assert(fclose(file) == 0);
			assert(region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
		} else
			assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
		memset(ids, BLOCK_STONE, sizeof ids);
		ids[(1 * 16 + 2) * 128 + 64] = BLOCK_FURNACE;
		if(unpublished)
			assert(region_archive_set_chunk(&ra, 0, 0, &sc, NULL, NULL));
		else if(!zero_reserved)
			assert(region_archive_set_chunk(&ra, 1, 0, &sc, &te, &entities));
		assert(region_archive_sync(&ra));
		region_archive_batch_begin(&ra);
		if(unpublished) {
			assert(region_archive_set_chunk(&ra, 1, 0, &sc, &te, &entities));
			if(deferred) {
				ptime_t ready = time_get();
				while(region_archive_test_io_incomplete()
					  && time_diff_ms(ready, time_get()) < 2000)
					thread_msleep(1);
				assert(!region_archive_test_io_incomplete());
				/* Retire the completed payload without publishing the batch:
				 * neither queued-byte reads nor fresh libfat filesize suffice. */
				region_archive_batch_begin(&ra);
				uint32_t value = ra.offsets[1];
				assert((uint64_t)((value >> 8) + (value & 255)) * REGION_SECTOR_SIZE
					> ra.rio_read_bytes);
			}
		}
		uint32_t st0[REGION_DEFERRED_STATS], st1[REGION_DEFERRED_STATS];
		region_archive_deferred_stats(st0);
		uint8_t data[32] = {1};
		struct buffer record = {data, sizeof data, sizeof data};
		__atomic_store_n(&s_disk_hold, 0, __ATOMIC_RELEASE);
		if(deferred)
			region_archive_test_payload_hook(payload_hold);
		assert(region_archive_recommit_record(&ra, 0, 0, &record));
		struct thread rel;
		if(deferred) {
			ptime_t ready = time_get();
			while(!__atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE)
				  && time_diff_ms(ready, time_get()) < 2000)
				thread_msleep(1);
			assert(__atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE) == 1);
			assert(thread_create(&rel, disk_releaser, NULL, 64));
		}
		ids[0] = BLOCK_DIRT;
		ptime_t start = time_get();
		check(region_archive_set_chunk(&ra, 1, 0, &sc, &te, &entities),
			"noncanonical save reads the published original mid-payload-write");
		if(deferred) {
			bool held = __atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE) == 1;
			printf("  preserve read (mode=%u): returned mid-write=%u, elapsed_us=%llu\n",
				(unsigned)mode, (unsigned)held,
				(unsigned long long)time_diff_us(start, time_get()));
			region_archive_deferred_stats(st1);
			check(st1[11] == st0[11], "record reads never acquire the global SD mutex");
			if(unpublished) {
				/* g9-savemiss-1009: a RETIRED payload whose range a fresh
				 * libfat handle cannot see yet is served from its own
				 * immutable bytes (retired-payload shadow), exactly like a
				 * queued one -- no handle drain, no SD mutex. Final bytes
				 * are still checked identical to the synchronous arm. */
				check(held && st1[5] == st0[5] && st1[10] > st0[10],
					"unpublished extension served from its retired payload (no drain)");
			} else
				check(held, "preservation read does not wait on the worker SD mutex");
			region_archive_test_payload_hook(NULL);
			__atomic_store_n(&s_disk_hold, 2, __ATOMIC_RELEASE);
			thread_join(&rel);
		}
		check(region_archive_batch_end(&ra) && region_archive_sync(&ra),
			"preservation publication and covering sync complete");
		check(region_archive_destroy(&ra), "preservation close");
		string_clear(level);
		g9io_snap_take(&final[arm], root);
		test_fixture_dir_cleanup(root);
	}
	check(g9io_snap_equal(&final[0], &final[1], "preserve-read stall"),
		"mid-write preservation synchronous/deferred bytes identical");
	region_archive_test_clock(0);
	g9io_arm(1);
}
static void partial_tail_stall(void) {
	static struct g9io_snap final[2];
	for(unsigned arm = 0; arm < 2; arm++) {
		bool deferred = arm != 0;
		g9io_arm(deferred ? 2 : 0);
		region_archive_test_clock(CLOCK_STAMP);
		char root[200], path[240];
		test_fixture_dir("g9io_partial_tail", root, sizeof root);
		assert(G9IO_MKDIR(root) == 0);
		snprintf(path, sizeof path, "%s/region", root);
		assert(G9IO_MKDIR(path) == 0);
		snprintf(path, sizeof path, "%s/region/r.0.0.mcr", root);
		FILE* f = fopen(path, "wb");
		assert(f);
		uint8_t zero[REGION_SECTOR_SIZE] = {0};
		/* A real indexed partial-tail record, already present at OPEN. Its
		 * framing must be checked while the worker writes a DIFFERENT slot. */
		zero[10] = 4; zero[11] = 1; /* slot 2: offset 4, one sector */
		assert(fwrite(zero, sizeof zero, 1, f) == 1);
		memset(zero, 0, sizeof zero);
		for(unsigned k = 1; k < 4; k++)
			assert(fwrite(zero, sizeof zero, 1, f) == 1);
		zero[3] = 4; zero[4] = 2; /* length=4, zlib marker, three payload bytes */
		assert(fwrite(zero, 17, 1, f) == 1 && fclose(f) == 0);
		string_t level;
		string_init_set_str(level, root);
		struct region_archive ra;
		assert(region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
		region_archive_batch_begin(&ra);
		/* Framing-only fixture: the allocator treats compressed bytes as opaque. */
		uint8_t data[32] = {1};
		struct buffer record = {data, sizeof data, sizeof data};
		__atomic_store_n(&s_disk_hold, 0, __ATOMIC_RELEASE);
		if(deferred)
			region_archive_test_trace(disk_hold_trace);
		assert(region_archive_recommit_record(&ra, 0, 0, &record));
		struct thread rel;
		if(deferred) {
			ptime_t ready = time_get();
			while(!__atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE)
				  && time_diff_ms(ready, time_get()) < 2000)
				thread_msleep(1);
			assert(__atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE) == 1);
			assert(thread_create(&rel, disk_releaser, NULL, 64));
		}
		ptime_t start = time_get();
		check(region_archive_recommit_record(&ra, 1, 0, &record),
			"partial-tail save accepted while worker holds disk mutex");
		if(deferred) {
			bool held = __atomic_load_n(&s_disk_hold, __ATOMIC_ACQUIRE) == 1;
			printf("  partial tail: save returned with SD mutex held=%u, elapsed_us=%llu\n",
				(unsigned)held, (unsigned long long)time_diff_us(start, time_get()));
			check(held, "bookkeeping does not wait for unrelated SD write");
			__atomic_store_n(&s_disk_hold, 2, __ATOMIC_RELEASE);
			thread_join(&rel);
			region_archive_test_trace(NULL);
		}
		check(region_archive_batch_end(&ra) && region_archive_sync(&ra),
			"partial-tail publication and covering sync complete");
		check(region_archive_destroy(&ra), "partial-tail close");
		string_clear(level);
		g9io_snap_take(&final[arm], root);
		test_fixture_dir_cleanup(root);
	}
	check(g9io_snap_equal(&final[0], &final[1], "partial-tail stall"),
		"partial-tail synchronous/deferred bytes identical at durable barrier");
	region_archive_test_clock(0);
	g9io_arm(1);
}

static void stalls_script(int arm, struct g9io_snap* final) {
	struct server_world w;
	char id[64];
	snprintf(id, sizeof id, "g9io_stalls_%s", G9IO_ARM_NAME[arm]);
	g9io_arm(arm);
	region_archive_test_clock(CLOCK_STAMP);
	open_fixture(id, &w);
	check(round_trip(&w), "stall fixture first round");
	verify(&w, true, "stall base");
	bool deferred = arm == 2;
	uint32_t st0[REGION_DEFERRED_STATS], st1[REGION_DEFERRED_STATS];
	region_archive_deferred_stats(st0);

	/* S1: read-after-write while the worker is held -- an evicting save
	 * retains the record, the chunk leaves memory, and its reload must see
	 * the NEW revision (served from the queued bytes). */
	if(deferred)
		region_archive_test_io_hold(true);
	server_world_save_batch_begin(&w);
	bump(&w, 0);
	check(server_world_save_chunk(&w, true, T[0].x, T[0].z),
		  "evicting save while the worker is held");
	check(!dict_server_chunks_get(w.chunks, S_CHUNK_ID(T[0].x, T[0].z)),
		  "the evicted chunk left memory");
	struct server_chunk* back = load(&w, T[0].x, T[0].z);
	check(back && back->ids[REV_CELL] == REV_BASE + T[0].mem,
		  "read-after-write: the reload sees the queued newest revision");
	if(deferred)
		check(region_archive_test_io_incomplete() > 0,
			  "the write really is still queued");
	/* S2: stall past the NEXT batch end -- the previous window's publication
	 * is still queued, so the second batch_end must wait for it (in order),
	 * never overtake it. */
	bump(&w, 1);
	(void)server_world_save_chunk(&w, false, T[1].x, T[1].z);
	check(server_world_save_batch_end(&w), "first held batch end");
	server_world_save_batch_begin(&w);
	bump(&w, 1);
	(void)server_world_save_chunk(&w, false, T[1].x, T[1].z);
	struct thread rel;
	if(deferred)
		release_later(&rel, 300);
	ptime_t t0 = time_get();
	check(server_world_save_batch_end(&w), "second batch end");
	float waited = time_diff_s(t0, time_get());
	if(deferred) {
		thread_join(&rel);
		check(waited > 0.2f,
			  "the second batch end waited for the held previous publication");
	}
	check(server_world_checkpoint_chunks(&w) && server_world_sync_regions(&w),
		  "barrier after the stall");
	verify(&w, true, "after stall");

	/* S3: queue exhaustion -- far more ops than the bound while held. */
	if(deferred)
		region_archive_test_io_hold(true);
	server_world_save_batch_begin(&w);
	if(deferred)
		release_later(&rel, 300);
	for(int i = 0; i < 70; i++) {
		w_coord_t x = (w_coord_t)(i % 35), z = (w_coord_t)(8 + i / 35);
		struct server_chunk* sc = load(&w, x, z);
		sc->ids[REV_CELL - 1] = (uint8_t)(i + 1);
		sc->modified = true;
		check(server_world_save_chunk(&w, false, x, z), "exhaustion save");
		sc->ids[REV_CELL - 2] = (uint8_t)(i + 1);
		sc->modified = true;
		check(server_world_save_chunk(&w, false, x, z), "exhaustion resave");
	}
	if(deferred)
		thread_join(&rel);
	check(server_world_save_batch_end(&w), "exhaustion batch end");
	check(server_world_checkpoint_chunks(&w) && server_world_sync_regions(&w),
		  "barrier after exhaustion");
	verify(&w, true, "after exhaustion");
	region_archive_deferred_stats(st1);
	if(deferred) {
		printf("  stalls: backpressure waits %u, barrier waits %u, peak ops %u,"
			   " peak bytes %u, read-after-write hits %u\n",
			   st1[4] - st0[4], st1[5] - st0[5], st1[9], st1[8],
			   st1[10] - st0[10]);
		check(st1[4] > st0[4], "queue exhaustion really applied backpressure");
		check(st1[10] > st0[10], "the reload was served from a queued record");
	}
	/* every exhaustion chunk is durable with its LAST write */
	server_world_destroy(&w);
	g9io_snap_take(final, s_root);
	{
		string_t level;
		string_init_set_str(level, s_root);
		bool all = true;
		for(int i = 0; i < 70; i++) {
			w_coord_t x = (w_coord_t)(i % 35), z = (w_coord_t)(8 + i / 35);
			struct region_archive ra;
			memset(&ra, 0, sizeof ra);
			struct server_chunk sc;
			memset(&sc, 0, sizeof sc);
			bool ok = region_archive_create(&ra, level, CHUNK_REGION_COORD(x),
											CHUNK_REGION_COORD(z),
											WORLD_DIM_OVERWORLD)
				&& region_archive_get_chunk(&ra, x, z, &sc, NULL, NULL);
			all = all && ok && sc.ids[REV_CELL - 1] == (uint8_t)(i + 1)
				&& sc.ids[REV_CELL - 2] == (uint8_t)(i + 1);
			if(ok)
				server_chunk_arrays_free(&sc);
			if(ra.offsets)
				region_archive_destroy(&ra);
		}
		check(all, "no write was dropped, coalesced or reordered");
		string_clear(level);
	}
	test_fixture_dir_cleanup(s_root);
	region_archive_test_clock(0);
	g9io_arm(1);
}

/* ---- case: powercut ----------------------------------------------------- */
#define MAX_CUTS 600
static char s_cut_base[220];
static unsigned s_cuts;
static struct thread_mutex s_cut_lock;
static uint8_t s_cut_acked[MAX_CUTS][NTRACK];
static uint8_t s_acked_now[NTRACK]; /* under g9io_trace_lock */
static void cut_hook(int op, const char* rel) {
	(void)op;
	(void)rel;
	/* The shipping arm can reach trace callbacks on both threads. Serialize
	 * image numbering and the entire copy, not just trace/ack bookkeeping. */
	thread_mutex_lock(&s_cut_lock);
	if(s_cuts >= MAX_CUTS) {
		thread_mutex_unlock(&s_cut_lock);
		return;
	}
	unsigned k = s_cuts++;
	char dst[260];
	snprintf(dst, sizeof dst, "%s_%03u", s_cut_base, k);
	/* Only acknowledgements preceding this cut belong to its lower bound.
	 * A different file can finish while the multi-file image is copied. */
	thread_mutex_lock(&g9io_trace_lock);
	memcpy(s_cut_acked[k], s_acked_now, NTRACK);
	thread_mutex_unlock(&g9io_trace_lock);
	test_fixture_rm_rf(dst);
	g9io_copy_tree(s_root, dst);
	thread_mutex_unlock(&s_cut_lock);
}
static void publish_acks(void) {
	thread_mutex_lock(&g9io_trace_lock);
	for(size_t k = 0; k < NTRACK; k++)
		s_acked_now[k] = T[k].acked;
	thread_mutex_unlock(&g9io_trace_lock);
}
/* Per-cut, per-tracked-chunk recovered revision; returns the cut count. */
static struct g9io_trace* s_cut_trace[3];
static unsigned powercut_arm(int arm, uint8_t (*recovered)[NTRACK]) {
	memset(&stress, 0, sizeof stress);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&stress))
		abort();
	dict_entity_init(stress.entities);
	struct server_world* w = &stress.worlds[0];
	char id[64];
	snprintf(id, sizeof id, "g9io_cut_%s", G9IO_ARM_NAME[arm]);
	g9io_arm(arm);
	region_archive_test_clock(CLOCK_STAMP);
	open_fixture(id, w);
	w->session = &stress;
	w->local = &stress;
	snprintf(s_cut_base, sizeof s_cut_base, "%s_cut", s_root);
	s_cuts = 0;
	memset(s_acked_now, 0, sizeof s_acked_now);
	struct g9io_trace* tr = calloc(1, sizeof(*tr));
	assert(tr);
	g9io_trace_begin(tr, s_root);
	g9io_point_cb = cut_hook;
	for(int r = 0; r < 4; r++) {
		bool ok = round_trip(w);
		bump(w, 0);
		if(r & 1)
			server_world_save_batch_begin(w);
		stress_checkpoint(w);
		if(arm == 2) {
			/* Pending bytes may be lost. Keep acknowledged prefix at the
			 * previous barrier, then resolve this exact immutable snapshot. */
			publish_acks();
			stress_finish(w);
		}
		check(ok, "power-cut workload round");
		if(ok)
			for(size_t k = 0; k < NTRACK; k++) {
				struct server_chunk* sc = dict_server_chunks_get(
					w->chunks, S_CHUNK_ID(T[k].x, T[k].z));
				if(sc && !sc->modified)
					T[k].acked = T[k].mem;
			}
		publish_acks();
	}
	/* an evicting save inside a batch, then the exit barrier */
	server_world_save_batch_begin(w);
	bump(w, 3);
	check(server_world_save_chunk(w, true, T[3].x, T[3].z), "evicting save");
	bump(w, 4);
	(void)server_world_save_chunk(w, false, T[4].x, T[4].z);
	check(server_world_save_batch_end(w), "eviction batch end");
	server_world_destroy(w);
	dict_entity_clear(stress.entities);
	for(size_t k = 0; k < NTRACK; k++)
		T[k].acked = T[k].mem; /* destroy published + closed everything */
	publish_acks();
	g9io_point_cb = NULL;
	g9io_trace_end();
	unsigned cuts = s_cuts;
	printf("  %s: %u persistence transitions, %u traced mutations\n",
		   G9IO_ARM_NAME[arm], cuts, tr->total);
	s_cut_trace[arm] = tr;
	/* Reopen every image twice on the main thread. */
	for(unsigned c = 0; c < cuts; c++) {
		char dir[260], label[64];
		snprintf(dir, sizeof dir, "%s_%03u", s_cut_base, c);
		snprintf(label, sizeof label, "%s cut %u", G9IO_ARM_NAME[arm], c);
		uint8_t a[NTRACK], b[NTRACK];
		uint64_t da = reopen_once(dir, a, label);
		uint64_t db = reopen_once(dir, b, label);
		check(da == db && !memcmp(a, b, sizeof a),
			  "power-cut image: the second reopen agrees with the first");
		for(size_t k = 0; k < NTRACK; k++) {
			bool real = a[k] == 0 || T[k].written[a[k]];
			if(!real || a[k] < s_cut_acked[c][k])
				printf("  [%s] chunk (%d,%d): recovered %u, acked-before-cut %u\n",
					   label, (int)T[k].x, (int)T[k].z, a[k], s_cut_acked[c][k]);
			check(real, "power-cut image: only revisions really written");
			check(a[k] >= s_cut_acked[c][k],
				  "power-cut image: no acknowledged revision lost");
		}
		memcpy(recovered[c], a, NTRACK);
		test_fixture_rm_rf(dir);
	}
	test_fixture_dir_cleanup(s_root);
	region_archive_test_clock(0);
	g9io_arm(1);
	return cuts;
}
/* The distinct-state sequence one tracked chunk passes through. */
static size_t chunk_sequence(uint8_t (*rec)[NTRACK], unsigned cuts, size_t k,
							 uint8_t* out) {
	size_t n = 0;
	for(unsigned c = 0; c < cuts; c++)
		if(!n || out[n - 1] != rec[c][k])
			out[n++] = rec[c][k];
	return n;
}

/* ---- powercut, part 2: CHILD-PROCESS KILL witness (REL-12) -------------
 * Shipped synchronous-save settings only (experimental.g9_save_io_int = 0):
 * arm 0 = every publication inline, arm 1 = the shipping default helper.
 *
 * The images above are COPIES taken inside one live process. This witness
 * instead lets a real child process be KILLED (SIGKILL / TerminateProcess:
 * no atexit, no stdio flush, no destructor) right after a NAMED persistence
 * transition (enum region_trace_op, reported through the existing
 * region_archive_test_trace seam, counted per file) of the production
 * save-and-quit barrier server_local_checkpoint_live_world, and a FRESH
 * process then reopens the world with the production readers.
 *
 *   gen 1 = the last COMPLETE save: chest + furnace + revision byte in chunk
 *           (0,0), three neighbouring chunks (1,0) (0,1) (1,1), player
 *           inventory / position / health / dimension (Overworld).
 *   gen 2 = the IN-FLIGHT transaction: the same chest, furnace, chunk and
 *           player fields all changed (dimension -> Nether); the
 *           neighbours are NOT touched.
 *   gen 3 = the SUBSEQUENT save made by the resumed child on the killed
 *           image (must succeed and must read back exactly).
 *
 * A census child runs gen 1 + gen 2 unkilled and lists every traced
 * transition of the gen-2 barrier; one kill child per listed transition.
 * The kill child's gen-1 image must equal the census gen-1 image byte for
 * byte (path + size + FNV-1a-64 of every file), so every kill starts from
 * the identical committed state.
 *
 * EXACT DURABILITY BOUNDARY this pins (what is, and is NOT, promised):
 *   - PER FILE, never torn: level.dat recovers wholly gen 1 or wholly gen 2
 *     (inventory, position, health and dimension always from ONE
 *     generation); chunk (0,0) recovers wholly gen 1 or wholly gen 2
 *     (chunk bytes, chest and furnace from ONE record, complete marker set).
 *   - level.dat flips to gen 2 exactly at the RT_LEVEL_ROTATE of level.dat;
 *     chunk (0,0) flips to gen 2 exactly at the first RT_SECTORS (the
 *     journalled header flip) of r.0.0.mcr. Before its own flip each file
 *     recovers gen 1, from its flip on gen 2.
 *   - NO cross-file atomicity: the barrier publishes level.dat BEFORE the
 *     chunks (server_local_checkpoint_live_world), so a kill between the two
 *     flips reopens gen-2 player/inventory/dimension with gen-1 chest /
 *     furnace / chunk. That mixed state is the expected, recorded outcome,
 *     not a pass waiver; there is no world-wide transaction to promise.
 *   - Untouched neighbouring chunks keep their gen-1 encoded record byte
 *     for byte at every kill point and after the subsequent save.
 *   - The region stays usable: no dangling index entry after the kill, two
 *     fresh reopens agree, and the resumed child's gen-3 save succeeds and
 *     reads back exactly.
 *   - A process kill keeps whatever reached the OS (fflush'd). It is NOT a
 *     power cut: fsync ordering under power loss is part 1's job above. */
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "network/level_archive.h"
#include "network/server_furnace.h"
#include "network/tile_entities_store.h"
#include "game/difficulty.h"

#define KW_KILL_RC 137
#define KW_MAX_OPS 256
#define KW_CELL(x, y, z) ((((x) * CHUNK_SIZE) + (z)) * WORLD_HEIGHT + (y))
#define KW_CHEST_X 2
#define KW_CHEST_Z 2
#define KW_FURN_X 5
#define KW_FURN_Z 5
#define KW_TE_Y 10
#define KW_NEIGH_REV 50
static const w_coord_t KW_NEIGH[3][2] = {{1, 0}, {0, 1}, {1, 1}};
static const char* const KW_OP_NAME[] = {"?", "RT_PAYLOAD", "RT_FLUSH",
	"RT_SYNC_REGION", "RT_JOURNAL", "RT_SYNC_JOURNAL", "RT_SECTORS",
	"RT_RETIRE", "RT_ROLLBACK", "RT_JOURNAL_OPEN", "RT_JOURNAL_REMOVE",
	"RT_CLOSE", "RT_LEVEL_WRITE", "RT_LEVEL_SYNC", "RT_LEVEL_ROTATE"};
static const char* kw_op_name(int op) {
	return op > 0 && op <= RT_LEVEL_ROTATE ? KW_OP_NAME[op] : "?";
}
static const char* s_self;
static struct server_local KW;

/* One traced transition of the gen-2 barrier: op, root-relative file, and
 * which occurrence of that (op, file) pair it is -- per-file order is
 * deterministic in both arms, the cross-thread interleaving of arm 1 is not. */
struct kw_op {
	int op;
	char rel[64];
	unsigned nth;
};
static struct thread_mutex s_kw_lock;
static char s_kw_root[200];
static struct kw_op s_kw_ops[KW_MAX_OPS];
static unsigned s_kw_nops;
static struct kw_op s_kw_target; /* op == 0: census, no kill */

static void kw_die(void) {
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), KW_KILL_RC);
#else
	kill(getpid(), SIGKILL);
#endif
	for(;;)
		thread_msleep(1000);
}
static void kw_hook(int op, const char* path, uint64_t a, uint64_t b) {
	(void)a;
	(void)b;
	size_t root_len = strlen(s_kw_root);
	const char* rel = strncmp(path, s_kw_root, root_len) ? path : path + root_len;
	thread_mutex_lock(&s_kw_lock);
	unsigned nth = 1;
	for(unsigned k = 0; k < s_kw_nops; k++)
		nth += s_kw_ops[k].op == op && !strcmp(s_kw_ops[k].rel, rel);
	if(s_kw_nops < KW_MAX_OPS) {
		struct kw_op* o = &s_kw_ops[s_kw_nops++];
		o->op = op;
		snprintf(o->rel, sizeof o->rel, "%s", rel);
		o->nth = nth;
	}
	if(s_kw_target.op == op && s_kw_target.nth == nth
	   && !strcmp(s_kw_target.rel, rel))
		kw_die(); /* the lock dies with the process */
	thread_mutex_unlock(&s_kw_lock);
}

static struct item_data kw_item(uint16_t id, unsigned count) {
	return (struct item_data) {.id = id, .count = (uint8_t)count};
}
static struct item_data kw_chest_slot(int g, int k) {
	return k == 0 ? kw_item(ITEM_DIAMOND, (unsigned)g)
		: k == 26 ? kw_item(BLOCK_COBBLESTONE, 10u + (unsigned)g)
				  : kw_item(0, 0);
}
static struct item_data kw_furnace_slot(int g, int k) {
	return k == 0 ? kw_item(BLOCK_IRON_ORE, (unsigned)g)
		: k == 1 ? kw_item(ITEM_COAL, (unsigned)g)
				 : kw_item(0, 0);
}
static struct item_data kw_inv_slot(int g, int k) {
	return k == INVENTORY_SLOT_MAIN ? kw_item(ITEM_DIAMOND, 10u + (unsigned)g)
		: k == INVENTORY_SLOT_MAIN + 1 ? kw_item(BLOCK_COBBLESTONE, (unsigned)g)
									   : kw_item(0, 0);
}
static bool kw_item_eq(struct item_data a, struct item_data b) {
	if(!b.count)
		return a.id == 0 || a.count == 0;
	return a.id == b.id && a.count == b.count && a.durability == b.durability;
}
static double kw_player_x(int g) { return 8.5 + g; }
static int16_t kw_health(int g) { return (int16_t)(10 + g); }
static enum world_dim kw_dim(int g) {
	return g == 2 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
}

static void kw_open(struct server_local* s, const char* root, bool fresh) {
	memset(s, 0, sizeof *s);
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t level;
	string_init_set_str(level, root);
	if(fresh)
		assert(level_archive_create_new(&s->level, level, 4242,
										(vec3) {8, 4, 8}));
	else
		assert(level_archive_create(&s->level, level));
	string_set(s->level_name, level);
	assert(server_world_create_with_populate_scratch(
		&s->worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true,
		s->world_populate_scratch));
	string_clear(level);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	jrand_set_seed(&s->worlds[0].rand, INT64_C(0x6910D0AB));
	s->rand_src.seed = 0x9E11u;
	s->world_time = 6000;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.y = 20.0;
	s->player.z = 8.5;
	s->player.oxygen = MAX_OXYGEN;
	s->render_demo_mob = -1;
	server_local_test_set_last_played(INT64_C(1791532400000));
	server_local_request_difficulty(0);
}
/* Install generation g in memory: chunk (0,0) revision + chest + furnace,
 * the player record, and (gen 1 only) the neighbours. */
static void kw_apply(struct server_local* s, int g) {
	struct server_world* w = &s->worlds[0];
	struct server_chunk* a = load(w, 0, 0);
	a->ids[REV_CELL] = (uint8_t)(REV_BASE + g);
	a->ids[KW_CELL(KW_CHEST_X, KW_TE_Y, KW_CHEST_Z)] = BLOCK_CHEST;
	a->ids[KW_CELL(KW_FURN_X, KW_TE_Y, KW_FURN_Z)] = BLOCK_FURNACE;
	a->modified = true;
	struct te_store st = server_world_tile_store(w);
	bool existed = false;
	int row = te_store_claim(&st, TE_KIND_CHEST, KW_CHEST_X, KW_TE_Y,
							 KW_CHEST_Z, &existed);
	assert(row >= 0);
	for(int k = 0; k < MAX_CHEST_SLOTS; k++)
		s->chest_items[row][k] = kw_chest_slot(g, k);
	assert(furnace_te_register_w(w, KW_FURN_X, KW_TE_Y, KW_FURN_Z) >= 0);
	struct furnace_te* fu = furnace_te_lookup_w(w, KW_FURN_X, KW_TE_Y, KW_FURN_Z);
	assert(fu);
	for(int k = 0; k < 3; k++)
		fu->items[k] = kw_furnace_slot(g, k);
	fu->burn_time = 100 * g;
	fu->cook_time = 10 * g;
	if(g == 1)
		for(size_t k = 0; k < 3; k++) {
			struct server_chunk* n = load(w, KW_NEIGH[k][0], KW_NEIGH[k][1]);
			n->ids[REV_CELL] = (uint8_t)(REV_BASE + KW_NEIGH_REV + k);
			n->modified = true;
		}
	for(int k = 0; k < INVENTORY_SIZE; k++)
		s->player.inventory.items[k] = kw_inv_slot(g, k);
	s->player.x = kw_player_x(g);
	s->player.health = kw_health(g);
	s->player.dimension = kw_dim(g);
}

/* What a fresh reopen recovers. gen 0 = matched no single generation. */
struct kw_obs {
	int chunk_gen, level_gen;
	uint64_t a_hash, n_hash[3];
	unsigned entries;
	bool ok;
};
static bool kw_record_hash(struct region_archive* ra, w_coord_t x, w_coord_t z,
						   uint64_t* out) {
	uint32_t offset = ra->offsets[(x & 31) + (z & 31) * 32];
	uint8_t framing[4];
	if(!offset || fseek(ra->file, (long)(offset >> 8) * REGION_SECTOR_SIZE, SEEK_SET)
	   || fread(framing, sizeof framing, 1, ra->file) != 1)
		return false;
	uint32_t len = (uint32_t)framing[0] << 24 | (uint32_t)framing[1] << 16
		| (uint32_t)framing[2] << 8 | framing[3];
	if(len < 1 || len > (offset & 255) * REGION_SECTOR_SIZE - 4)
		return false;
	uint8_t* bytes = malloc(len);
	assert(bytes);
	bool ok = fread(bytes, len, 1, ra->file) == 1;
	*out = g9io_fnv(g9io_fnv(G9IO_FNV_BASIS, framing, 4), bytes, len);
	free(bytes);
	return ok;
}
static int kw_chunk_gen(const struct server_chunk* sc,
						const struct chunk_tile_entities* te) {
	int g = sc->ids[REV_CELL] - REV_BASE;
	if(g < 1 || g > 3 || !te->legacy_flat_complete || te->n_chest != 1
	   || te->n_furnace != 1
	   || sc->ids[KW_CELL(KW_CHEST_X, KW_TE_Y, KW_CHEST_Z)] != BLOCK_CHEST
	   || sc->ids[KW_CELL(KW_FURN_X, KW_TE_Y, KW_FURN_Z)] != BLOCK_FURNACE)
		return 0;
	const struct te_chest* c = &te->chests[0];
	const struct te_furnace* f = &te->furnaces[0];
	bool same = c->x == KW_CHEST_X && c->y == KW_TE_Y && c->z == KW_CHEST_Z
		&& f->x == KW_FURN_X && f->y == KW_TE_Y && f->z == KW_FURN_Z
		&& f->burn_time == 100 * g && f->cook_time == 10 * g;
	for(int k = 0; same && k < TE_CHEST_SLOTS; k++)
		same = kw_item_eq(c->items[k], kw_chest_slot(g, k));
	for(int k = 0; same && k < TE_FURNACE_SLOTS; k++)
		same = kw_item_eq(f->items[k], kw_furnace_slot(g, k));
	return same ? g : 0;
}
static int kw_level_gen(const struct item_data* items, double x, int16_t health,
						enum world_dim dim) {
	for(int g = 1; g <= 3; g++) {
		bool same = x == kw_player_x(g) && health == kw_health(g)
			&& dim == kw_dim(g);
		for(int k = 0; same && k < INVENTORY_SIZE; k++)
			same = kw_item_eq(items[k], kw_inv_slot(g, k));
		if(same)
			return g;
	}
	return 0;
}
/* Fresh production reopen of `dir` (mutates it: journal replay / salvage --
 * callers pass a copy). */
static void kw_reopen(const char* dir, struct kw_obs* o, const char* label) {
	memset(o, 0, sizeof *o);
	o->ok = true;
	string_t level;
	string_init_set_str(level, dir);
	struct level_archive la;
	memset(&la, 0, sizeof la);
	if(level_archive_create(&la, level)) {
		struct item_data items[INVENTORY_SIZE] = {{0}};
		struct inventory inv = {.items = items, .capacity = INVENTORY_SIZE};
		vec3 pos = {0};
		enum world_dim dim = WORLD_DIM_OVERWORLD;
		int16_t health = -1;
		if(level_archive_read_inventory(&la, &inv)
		   && level_archive_read_player(&la, pos, NULL, NULL, &dim)
		   && level_archive_read_player_health(&la, &health))
			o->level_gen = kw_level_gen(items, pos[0], health, dim);
		level_archive_discard(&la);
	}
	struct region_archive ra;
	memset(&ra, 0, sizeof ra);
	if(!region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD)) {
		printf("  [%s] region r.0.0 does not reopen\n", label);
		o->ok = false;
		string_clear(level);
		return;
	}
	for(int lx = 0; lx < REGION_SIZE; lx++)
		for(int lz = 0; lz < REGION_SIZE; lz++) {
			bool exists = false;
			if(!region_archive_contains(&ra, lx, lz, &exists) || !exists)
				continue;
			o->entries++;
			struct server_chunk sc;
			struct chunk_tile_entities te;
			struct chunk_entities ents;
			memset(&sc, 0, sizeof sc);
			memset(&te, 0, sizeof te);
			memset(&ents, 0, sizeof ents);
			if(!region_archive_get_chunk(&ra, lx, lz, &sc, &te, &ents)) {
				printf("  [%s] dangling index entry at chunk (%d,%d)\n", label,
					   lx, lz);
				o->ok = false;
				continue;
			}
			if(lx == 0 && lz == 0) {
				o->chunk_gen = kw_chunk_gen(&sc, &te);
				o->ok &= kw_record_hash(&ra, 0, 0, &o->a_hash);
			}
			for(size_t k = 0; k < 3; k++)
				if(lx == KW_NEIGH[k][0] && lz == KW_NEIGH[k][1]) {
					o->ok &= sc.ids[REV_CELL] == REV_BASE + KW_NEIGH_REV + k;
					o->ok &= kw_record_hash(&ra, lx, lz, &o->n_hash[k]);
				}
			tile_entities_parse_free(&te);
			chunk_entities_free(&ents);
			server_chunk_arrays_free(&sc);
		}
	o->ok &= region_archive_destroy(&ra);
	string_clear(level);
}
/* Copy, reopen twice; both reopens must agree. */
static void kw_reopen_copy(const char* dir, struct kw_obs* o, const char* label) {
	char copy[260];
	snprintf(copy, sizeof copy, "%s_copy", dir);
	test_fixture_rm_rf(copy);
	g9io_copy_tree(dir, copy);
	struct kw_obs second;
	kw_reopen(copy, o, label);
	kw_reopen(copy, &second, label);
	check(o->chunk_gen == second.chunk_gen && o->level_gen == second.level_gen
		&& o->a_hash == second.a_hash && o->entries == second.entries
		&& !memcmp(o->n_hash, second.n_hash, sizeof o->n_hash),
		"kill witness: a second fresh reopen recovers exactly what the first did");
	test_fixture_rm_rf(copy);
}

/* ---- the children ---- */
static int kw_child(int argc, char** argv) {
	/* kw_child census|kill <arm> <root> [<op> <rel> <nth>] */
	if(argc < 5)
		return 2;
	bool census = !strcmp(argv[2], "census");
	int arm = atoi(argv[3]);
	snprintf(s_kw_root, sizeof s_kw_root, "%s", argv[4]);
	memset(&s_kw_target, 0, sizeof s_kw_target);
	if(!census) {
		if(argc < 8)
			return 2;
		s_kw_target.op = atoi(argv[5]);
		snprintf(s_kw_target.rel, sizeof s_kw_target.rel, "%s", argv[6]);
		s_kw_target.nth = (unsigned)atoi(argv[7]);
	}
	assert(thread_mutex_init(&s_kw_lock));
	g9io_arm(arm);
	region_archive_test_clock(CLOCK_STAMP);
	assert(G9IO_MKDIR(s_kw_root) == 0);
	struct server_local* s = &KW;
	kw_open(s, s_kw_root, true);
	kw_apply(s, 1);
	if(!server_local_test_checkpoint_live_world(s, "rel12_gen1"))
		return 4;
	char img[260];
	snprintf(img, sizeof img, "%s_gen1", s_kw_root);
	g9io_copy_tree(s_kw_root, img);
	kw_apply(s, 2);
	region_archive_test_trace(kw_hook);
	bool ok = server_local_test_checkpoint_live_world(s, "rel12_gen2");
	region_archive_test_trace(NULL);
	if(!census)
		return 3; /* the named transition never happened */
	if(!ok)
		return 5;
	snprintf(img, sizeof img, "%s_gen2", s_kw_root);
	g9io_copy_tree(s_kw_root, img);
	snprintf(img, sizeof img, "%s_ops.txt", s_kw_root);
	FILE* f = fopen(img, "w");
	if(!f)
		return 6;
	for(unsigned k = 0; k < s_kw_nops; k++)
		fprintf(f, "%d %s %u\n", s_kw_ops[k].op, s_kw_ops[k].rel, s_kw_ops[k].nth);
	return fclose(f) || s_kw_nops >= KW_MAX_OPS ? 6 : 0;
}
static int kw_resume(int argc, char** argv) {
	/* kw_resume <arm> <root> <chunk gen> <level gen> */
	if(argc < 6)
		return 2;
	g9io_arm(atoi(argv[2]));
	region_archive_test_clock(CLOCK_STAMP);
	int chunk_gen = atoi(argv[4]), level_gen = atoi(argv[5]);
	struct server_local* s = &KW;
	kw_open(s, argv[3], false);
	/* The production readers a world load uses. */
	vec3 pos = {0};
	enum world_dim dim = WORLD_DIM_OVERWORLD;
	int16_t health = -1;
	bool read = level_archive_read_inventory(&s->level, &s->player.inventory)
		&& level_archive_read_player(&s->level, pos, NULL, NULL, &dim)
		&& level_archive_read_player_health(&s->level, &health);
	check(read && kw_level_gen(s->player.inventory.items, pos[0], health, dim)
		== level_gen, "resumed process loads the recovered player record");
	struct server_world* w = &s->worlds[0];
	struct server_chunk* a = load(w, 0, 0);
	check(a->ids[REV_CELL] == REV_BASE + chunk_gen,
		  "resumed process loads the recovered chunk revision");
	int row = server_local_chest_row_w(w, KW_CHEST_X, KW_TE_Y, KW_CHEST_Z);
	bool chest = row >= 0;
	for(int k = 0; chest && k < MAX_CHEST_SLOTS; k++)
		chest = kw_item_eq(s->chest_items[row][k], kw_chest_slot(chunk_gen, k));
	check(chest, "resumed process loads the recovered chest contents");
	struct furnace_te* fu = furnace_te_lookup_w(w, KW_FURN_X, KW_TE_Y, KW_FURN_Z);
	bool furnace = fu && fu->burn_time == 100 * chunk_gen
		&& fu->cook_time == 10 * chunk_gen;
	for(int k = 0; furnace && k < 3; k++)
		furnace = kw_item_eq(fu->items[k], kw_furnace_slot(chunk_gen, k));
	check(furnace, "resumed process loads the recovered furnace");
	kw_apply(s, 3);
	check(server_local_test_checkpoint_live_world(s, "rel12_gen3"),
		  "the subsequent save on the killed image succeeds");
	return failures ? 1 : 0;
}

static int kw_system(const char* args) {
	char cmd[700];
#ifdef _WIN32
	snprintf(cmd, sizeof cmd, "\"\"%s\" %s\"", s_self, args);
#else
	snprintf(cmd, sizeof cmd, "\"%s\" %s", s_self, args);
#endif
	int rc = system(cmd);
#ifndef _WIN32
	if(rc != -1 && WIFSIGNALED(rc))
		return 128 + WTERMSIG(rc);
	if(rc != -1 && WIFEXITED(rc))
		return WEXITSTATUS(rc);
#endif
	return rc;
}
static void kw_cleanup(const char* root) {
	static const char* const sfx[] = {"", "_gen1", "_gen2", "_copy", "_ops.txt"};
	for(size_t k = 0; k < sizeof sfx / sizeof *sfx; k++) {
		char p[260];
		snprintf(p, sizeof p, "%s%s", root, sfx[k]);
		test_fixture_rm_rf(p);
	}
}
static void kill_witness_arm(int arm) {
	char base[200], root[230], args[600], img[260];
	snprintf(args, sizeof args, "g9io_kw_%s", G9IO_ARM_NAME[arm]);
	test_fixture_dir(args, base, sizeof base);
	/* 1. census: gen 1 + gen 2 unkilled, list the gen-2 transitions. */
	snprintf(root, sizeof root, "%s_census", base);
	kw_cleanup(root);
	snprintf(args, sizeof args, "kw_child census %d %s", arm, root);
	int rc = kw_system(args);
	check(rc == 0, "kill witness: census child completes both saves");
	static struct kw_op ops[KW_MAX_OPS];
	unsigned nops = 0;
	snprintf(img, sizeof img, "%s_ops.txt", root);
	FILE* f = fopen(img, "r");
	while(f && nops < KW_MAX_OPS
		  && fscanf(f, "%d %63s %u", &ops[nops].op, ops[nops].rel,
					&ops[nops].nth) == 3)
		nops++;
	if(f)
		fclose(f);
	static struct g9io_snap ref_gen1, snap;
	snprintf(img, sizeof img, "%s_gen1", root);
	g9io_snap_take(&ref_gen1, img);
	struct kw_obs g1, g2;
	kw_reopen_copy(img, &g1, "census gen1");
	snprintf(img, sizeof img, "%s_gen2", root);
	g9io_snap_take(&snap, img);
	kw_reopen_copy(img, &g2, "census gen2");
	check(g1.ok && g1.chunk_gen == 1 && g1.level_gen == 1,
		  "kill witness: the gen-1 save is complete");
	check(g2.ok && g2.chunk_gen == 2 && g2.level_gen == 2
		  && !memcmp(g1.n_hash, g2.n_hash, sizeof g1.n_hash),
		  "kill witness: the unkilled gen-2 save is complete, neighbours untouched");
	printf("  kill %s: gen1 image %016" PRIx64 " (%zu files), gen2 image %016"
		   PRIx64 " (%zu files), chunk(0,0) record gen1 %016" PRIx64
		   " gen2 %016" PRIx64 ", neighbours %016" PRIx64 " %016" PRIx64
		   " %016" PRIx64 ", %u region entries, %u gen-2 transitions\n",
		   G9IO_ARM_NAME[arm], g9io_snap_digest(&ref_gen1), ref_gen1.n,
		   g9io_snap_digest(&snap), snap.n, g1.a_hash, g2.a_hash, g1.n_hash[0],
		   g1.n_hash[1], g1.n_hash[2], g1.entries, nops);
	bool has_rotate = false, has_sectors = false;
	for(unsigned k = 0; k < nops; k++) {
		has_rotate |= ops[k].op == RT_LEVEL_ROTATE && !strcmp(ops[k].rel, "/level.dat");
		has_sectors |= ops[k].op == RT_SECTORS
			&& !strcmp(ops[k].rel, "/region/r.0.0.mcr");
	}
	check(nops > 0 && has_rotate && has_sectors,
		  "kill witness: the gen-2 barrier rotates level.dat and flips r.0.0 sectors");
	kw_cleanup(root);
	/* 2. one killed child per transition. */
	unsigned mixed = 0, kills = 0;
	bool level_new = false, chunk_new = false;
	for(unsigned i = 0; i < nops; i++) {
		level_new |= ops[i].op == RT_LEVEL_ROTATE && !strcmp(ops[i].rel, "/level.dat");
		chunk_new |= ops[i].op == RT_SECTORS && !strcmp(ops[i].rel, "/region/r.0.0.mcr");
		int want_level = level_new ? 2 : 1, want_chunk = chunk_new ? 2 : 1;
		char label[160];
		snprintf(label, sizeof label, "%s kill after %s %s #%u",
				 G9IO_ARM_NAME[arm], kw_op_name(ops[i].op), ops[i].rel, ops[i].nth);
		snprintf(root, sizeof root, "%s_k%03u", base, i);
		kw_cleanup(root);
		snprintf(args, sizeof args, "kw_child kill %d %s %d %s %u", arm, root,
				 ops[i].op, ops[i].rel, ops[i].nth);
		rc = kw_system(args);
		if(rc != KW_KILL_RC)
			printf("  [%s] child exit %d, want %d\n", label, rc, KW_KILL_RC);
		check(rc == KW_KILL_RC, "kill witness: the child dies at the named transition");
		kills += rc == KW_KILL_RC;
		snprintf(img, sizeof img, "%s_gen1", root);
		g9io_snap_take(&snap, img);
		check(g9io_snap_equal(&ref_gen1, &snap, label),
			  "kill witness: every kill starts from the identical committed image");
		g9io_snap_take(&snap, root);
		uint64_t killed_digest = g9io_snap_digest(&snap);
		struct kw_obs o;
		kw_reopen_copy(root, &o, label);
		bool exact = o.ok && o.chunk_gen == want_chunk && o.level_gen == want_level
			&& o.a_hash == (want_chunk == 2 ? g2.a_hash : g1.a_hash)
			&& !memcmp(o.n_hash, g1.n_hash, sizeof o.n_hash)
			&& o.entries == g1.entries;
		if(!exact)
			printf("  [%s] recovered chunk gen %d (want %d) level gen %d (want %d)"
				   " ok %u entries %u (want %u)\n", label, o.chunk_gen, want_chunk,
				   o.level_gen, want_level, (unsigned)o.ok, o.entries, g1.entries);
		check(exact, "kill witness: fresh reopen recovers exactly the boundary state");
		mixed += o.level_gen != o.chunk_gen;
		/* 3. the subsequent save, by a fresh process, on the killed image. */
		snprintf(args, sizeof args, "kw_resume %d %s %d %d", arm, root,
				 want_chunk, want_level);
		int resumed = kw_system(args);
		check(resumed == 0, "kill witness: resumed child reloads and saves");
		struct kw_obs r;
		kw_reopen_copy(root, &r, label);
		check(r.ok && r.chunk_gen == 3 && r.level_gen == 3
			  && !memcmp(r.n_hash, g1.n_hash, sizeof r.n_hash)
			  && r.entries == g1.entries,
			  "kill witness: the subsequent save reads back exactly");
		g9io_snap_take(&snap, root);
		printf("  %-8s kill %02u after %-17s %-18s #%u: exit %d, image %016" PRIx64
			   ", recovered chunk gen%d level gen%d, resumed exit %d -> gen%d/gen%d"
			   " %016" PRIx64 "\n", G9IO_ARM_NAME[arm], i, kw_op_name(ops[i].op),
			   ops[i].rel, ops[i].nth, rc, killed_digest, o.chunk_gen, o.level_gen,
			   resumed, r.chunk_gen, r.level_gen, g9io_snap_digest(&snap));
		kw_cleanup(root);
	}
	printf("  kill %s: %u/%u kills at named transitions, %u reopened with the "
		   "documented cross-file mix (gen-2 level.dat, gen-1 chunk)\n",
		   G9IO_ARM_NAME[arm], kills, nops, mixed);
	check(mixed > 0, "kill witness: the level.dat-before-chunks window was exercised");
	test_fixture_dir_cleanup(base);
}

static void* drain_entry(void* arg) {
	(void)arg;
	for(;;) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init() && svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	const char* which = argc > 1 ? argv[1] : "faults";
	s_self = argv[0];
	if(!strcmp(which, "kw_child"))
		return kw_child(argc, argv);
	if(!strcmp(which, "kw_resume"))
		return kw_resume(argc, argv);
	if(!strcmp(which, "faults")) {
		for(unsigned mode = 0; mode < 3; mode++) pending_helper_eviction(mode);
		pending_clean_eviction(0);
		pending_clean_eviction(1);
		pending_clean_eviction(2);
		pending_clean_eviction(3);
		pending_clean_eviction(4);
		pending_clean_eviction(5);
		run_faults_arm(0);
		run_faults_arm(2);
		stress_caller(true);
	} else if(!strcmp(which, "helper_evict")) {
		for(unsigned mode = 0; mode < 3; mode++) pending_helper_eviction(mode);
	} else if(!strcmp(which, "stalls")) {
		stress_caller(false);
		preserve_read_stall(PRESERVE_PUBLISHED);
		preserve_read_stall(PRESERVE_ZERO);
		preserve_read_stall(PRESERVE_UNPUBLISHED);
		partial_tail_stall();
		static struct g9io_snap ref, def;
		stalls_script(0, &ref);
		stalls_script(2, &def);
		bool same = g9io_snap_equal(&ref, &def, "stalls final");
		printf("  stalls final bytes: inline %016" PRIx64 " deferred %016" PRIx64
			   " %s\n", g9io_snap_digest(&ref), g9io_snap_digest(&def),
			   same ? "EQUAL" : "DIFFERENT");
		check(same, "the stalled deferred run ends byte-identical to the "
					"reference");
	} else if(!strcmp(which, "powercut")) {
		assert(thread_mutex_init(&s_cut_lock));
		/* All three arms. The per-file mutation trace of the deferred worker
		 * must equal the SHIPPING helper's: both leave a batch publication
		 * pending at batch end, so a teardown right after an evicting batch
		 * takes the S2 retiring-ring drain (one more fsync of each region)
		 * that the fully-inline reference does not need -- an extra barrier,
		 * never a different byte. The recovered-state sequence of every
		 * chunk must equal across all three arms. */
		static uint8_t rec[3][MAX_CUTS][NTRACK];
		unsigned n[3];
		for(int arm = 0; arm < 3; arm++) {
			s_prefix_arm = arm;
			n[arm] = powercut_arm(arm, rec[arm]);
			check(n[arm] > 20, "the workload has persistence transitions");
			check(n[arm] < MAX_CUTS, "every transition was imaged");
		}
		check(g9io_trace_equal(s_cut_trace[1], s_cut_trace[2], "powercut"),
			  "the deferred arm performs the shipping helper's per-file "
			  "mutations, in order");
		for(int arm = 0; arm < 3; arm++)
			free(s_cut_trace[arm]);
		for(size_t k = 0; k < NTRACK; k++) {
			uint8_t sq[3][MAX_CUTS];
			size_t len[3];
			for(int arm = 0; arm < 3; arm++)
				len[arm] = chunk_sequence(rec[arm], n[arm], k, sq[arm]);
			bool same = len[0] == len[1] && len[0] == len[2]
				&& !memcmp(sq[0], sq[1], len[0]) && !memcmp(sq[0], sq[2], len[0]);
			printf("  chunk (%d,%d): recovered states inline %zu / helper %zu / "
				   "deferred %zu %s\n", (int)T[k].x, (int)T[k].z, len[0], len[1],
				   len[2], same ? "IDENTICAL" : "DIFFERENT");
			if(!same) for(int arm = 0; arm < 3; arm++) {
				printf("    %s sequence:", G9IO_ARM_NAME[arm]);
				for(size_t j = 0; j < len[arm]; j++) printf(" %u", sq[arm][j]);
				printf("\n");
			}
			check(same, "each chunk recovers through exactly the reference's "
						"durable prefixes");
		}
		s_prefix_arm = -1;
		for(size_t k = 0; k < NTRACK; k++)
			for(unsigned j = 0; j < s_prefix_n[k]; j++) buffer_free(&s_prefix[k][j]);
		/* Part 2: real child-process kills, shipped g9_save_io_int = 0. */
		kill_witness_arm(0);
		kill_witness_arm(1);
	} else {
		printf("unknown case %s\n", which);
		return 2;
	}
	if(failures) {
		printf("test_g9_save_io_durability[%s]: %u FAILED\n", which, failures);
		return 1;
	}
	printf("test_g9_save_io_durability[%s]: PASS\n", which);
	return 0;
}
