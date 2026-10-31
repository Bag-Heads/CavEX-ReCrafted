/*
	SAVE BATCH equivalence + crash-point coverage (2026-09-07).

	region_archive_batch_begin/_end amortize the journaled region-header
	publication over a batch of chunk writes instead of paying a file create, a
	rename, a remove and three fsyncs per chunk (the ea0de5b3 cost that starved
	chunk generation on the real-hardware far walk). This test pins the two
	things that must not move:

	  1. EQUIVALENCE. For the same scripted save sequence, the batched path and
	     the per-write path produce
	       (a) byte-identical .mcr files -- offset table AND payload sectors --
	           whenever the batch does not reuse space it freed inside itself
	           (the append-only case: every chunk written once, no rewrite), and
	       (b) byte-identical CHUNK RECORDS for every chunk in every case,
	           including rewrite-heavy sequences. (b) is the weaker claim
	           because reserving an in-batch chunk's OLD range -- mandatory: the
	           on-disk index still names it until the batch publishes -- can
	           push a later payload in the same batch past a gap the per-write
	           path had already freed. That moves WHERE a payload sits inside
	           the container; it never changes what any chunk reads back, and
	           copy-on-write already moves every rewritten payload by design
	           (see test_region_archive_safety's "a same-size rewrite must still
	           move"). The difference is documented, not silent.
	       (c) neither path may leave a .idx or .idx_new sidecar behind.

	  1b. THE EVICTION PATH (round 2, 2026-09-07). An LRU region eviction
	     (server_world_chunk_region) syncs and destroys a handle that may be
	     carrying an open batch, and a world teardown closes one. Those deltas
	     can never join a later batch -- the handle is gone -- so the sync (and,
	     failing that, the destroy) must publish them. Covered inside a batch,
	     outside a batch, and with the same chunk saved twice in one batch
	     before the eviction.

	  2. CRASH POINTS at the batch's header-flip boundary. The invariant the
	     journal exists to guarantee is per chunk: an on-disk index entry never
	     names a payload that is not fully on disk, so recovery yields the
	     COMPLETE old state or the COMPLETE new state, never a mix. The three
	     reachable on-disk states around the flip are manufactured directly --
	     journal present with the sector still holding the before-image, the
	     sector holding the after-image, and the sector TORN half and half --
	     and each is reopened and every chunk of the batch read back.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

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
#include "network/tile_entities_nbt.h"
#include "network/entity_archive.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "item/item_nbt.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

/* Must match region_archive.c's private definitions. Duplicated ON PURPOSE:
 * the point of the crash-point cases is to prove that recovery honours the
 * DOCUMENTED record layout, so the test encodes it independently rather than
 * borrowing the writer's own helper. */
#define IDX_SECTOR 512
/* v1: magic, sector, before, after, crc (round 1, still readable).
 * v2: magic, count, count x {sector, before, after}, crc (round 2 -- one
 *     journal for every header sector the batch touches). */
#define IDX_RECORD (12 + 2 * IDX_SECTOR + 4)
#define IDX_ENTRY (4 + 2 * IDX_SECTOR)
#define IDX_RECORD_V2(n) (12 + (size_t)(n) * IDX_ENTRY + 4)
/* v3 (round 3): the journal is opened once per handle and rewritten IN PLACE,
 * retired by a sequence number instead of an unlink.
 *   [0,8) "CWIDX003"  [8,12) published_seq  [12,16) reserved
 *   [16,20) "CWRC"    [20,24) count  [24,28) seq  [28,..) entries  [..,+4) crc
 * published_seq is deliberately OUTSIDE the crc'd region so retiring a record
 * cannot invalidate the record it retires. */
#define IDX3_HEAD 16
#define IDX3_REC_HEAD 12
#define IDX3_RECORD(n) (IDX3_HEAD + IDX3_REC_HEAD + (size_t)(n) * IDX_ENTRY + 4)

static char s_fixture_root[128];

static const char* world_dir(int which) {
	return test_fixture_join(s_fixture_root, which == 0 ? "/a" : "/b");
}

static const char* region_path(int which) {
	return test_fixture_join(world_dir(which), "/region/r.0.0.mcr");
}

static void make_world(int which) {
	MKDIR(s_fixture_root);
	MKDIR(world_dir(which));
	MKDIR(test_fixture_join(world_dir(which), "/region"));
}

static void alloc_chunk(struct server_chunk* sc) {
	memset(sc, 0, sizeof(*sc));
	sc->ids = calloc(1, IDS_SIZE);
	sc->metadata = calloc(1, NIB_SIZE);
	sc->lighting_sky = calloc(1, NIB_SIZE);
	sc->lighting_torch = calloc(1, NIB_SIZE);
	sc->heightmap = calloc(1, HM_SIZE);
	assert(sc->ids && sc->metadata && sc->lighting_sky && sc->lighting_torch
		   && sc->heightmap);
	sc->populated = true;
}

static void free_chunk(struct server_chunk* sc) {
	/* A chunk read back through region_archive_get_blocks owns ONE pooled
	 * slot (server_chunk_arrays_alloc, 2026-09-11); a test-built one owns
	 * five calloc arrays. */
	if(sc->arrays_slot) {
		server_chunk_arrays_free(sc);
		return;
	}
	free(sc->ids);
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

/* A deterministic, compressible-but-not-trivial body so two runs producing the
 * same NBT produce the same zlib record byte for byte. */
static void fill_chunk(struct server_chunk* sc, unsigned seed) {
	for(size_t k = 0; k < IDS_SIZE; k++)
		sc->ids[k] = (uint8_t)((k * 31u + seed * 7u) & 0x3F);
	for(size_t k = 0; k < NIB_SIZE; k++)
		sc->metadata[k] = (uint8_t)((k + seed) & 0xFF);
	sc->ids[0] = (uint8_t)(1 + (seed & 0x1F));
}

static uint8_t* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long end = ftell(f);
	assert(end > 0);
	assert(fseek(f, 0, SEEK_SET) == 0);
	uint8_t* data = malloc((size_t)end);
	assert(data);
	assert(fread(data, (size_t)end, 1, f) == 1);
	assert(fclose(f) == 0);
	*size = (size_t)end;
	return data;
}

static void write_file(const char* path, const uint8_t* data, size_t size) {
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fwrite(data, size, 1, f) == 1);
	assert(fclose(f) == 0);
}

/* Overwrite the FIRST `size` bytes of an EXISTING file WITHOUT truncating (or
 * otherwise touching) whatever sits past them -- the in-place rewrite
 * production code performs, and the only way to manufacture a stale tail left
 * behind by a longer record the new one overwrote. `path` must already exist
 * (r+b does not create). */
static void write_file_inplace(const char* path, const uint8_t* data,
							   size_t size) {
	FILE* f = fopen(path, "r+b");
	assert(f);
	assert(fwrite(data, size, 1, f) == 1);
	assert(fclose(f) == 0);
}

/* read_file for a file that is allowed to be absent. */
static uint8_t* read_file_opt(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;
	assert(fseek(f, 0, SEEK_END) == 0);
	long end = ftell(f);
	assert(end >= 0);
	assert(fseek(f, 0, SEEK_SET) == 0);
	uint8_t* data = malloc((size_t)end + 1);
	assert(data);
	assert(end == 0 || fread(data, (size_t)end, 1, f) == 1);
	assert(fclose(f) == 0);
	*size = (size_t)end;
	return data;
}

static bool file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

static uint32_t get_be32(const uint8_t* in) {
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16)
		| ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static void put_u32(uint32_t v, uint8_t* out) {
	out[0] = (uint8_t)(v >> 24);
	out[1] = (uint8_t)(v >> 16);
	out[2] = (uint8_t)(v >> 8);
	out[3] = (uint8_t)v;
}

/* The whole indexed record of local chunk `index`, or NULL when absent. */
static uint8_t* record_of(const uint8_t* file, size_t size, size_t index,
						  size_t* out_len) {
	uint32_t entry = get_be32(file + index * 4);
	if(entry == 0)
		return NULL;
	uint32_t offset = entry >> 8;
	uint32_t sectors = entry & 0xFF;
	size_t start = (size_t)offset * REGION_SECTOR_SIZE;
	assert(start + 4 <= size);
	size_t len = get_be32(file + start) + 4u;
	assert(len <= (size_t)sectors * REGION_SECTOR_SIZE);
	assert(start + len <= size);
	*out_len = len;
	uint8_t* copy = malloc(len);
	assert(copy);
	memcpy(copy, file + start, len);
	return copy;
}

static void assert_same_records(const char* why) {
	size_t sa, sb;
	uint8_t* a = read_file(region_path(0), &sa);
	uint8_t* b = read_file(region_path(1), &sb);
	int present = 0;
	for(size_t k = 0; k < REGION_SIZE * REGION_SIZE; k++) {
		size_t la = 0, lb = 0;
		uint8_t* ra = record_of(a, sa, k, &la);
		uint8_t* rb = record_of(b, sb, k, &lb);
		assert((ra != NULL) == (rb != NULL) && why);
		if(ra) {
			assert(la == lb && memcmp(ra, rb, la) == 0 && why);
			present++;
		}
		free(ra);
		free(rb);
	}
	assert(present > 0);
	free(a);
	free(b);
}

/* Byte-identity of everything EXCEPT the wall-clock timestamp table (file
 * bytes 4096..8191, sector 1 of the McRegion header). */
static void assert_same_bytes_modulo_timestamps(const char* why) {
	size_t sa, sb;
	uint8_t* a = read_file(region_path(0), &sa);
	uint8_t* b = read_file(region_path(1), &sb);
	assert(sa == sb && why);
	assert(memcmp(a, b, REGION_SECTOR_SIZE) == 0
		   && "the published offset table must be byte-identical");
	assert(sa > 2 * REGION_SECTOR_SIZE);
	assert(memcmp(a + 2 * REGION_SECTOR_SIZE, b + 2 * REGION_SECTOR_SIZE,
				  sa - 2 * REGION_SECTOR_SIZE)
			   == 0
		   && why);
	free(a);
	free(b);
}

/* After a CLOSED handle: nothing at all is left behind. region_archive_destroy
 * removes the in-place journal once its record is retired, so the round-2
 * property survives the round-3 mechanism change. */
static void assert_no_journal(int which) {
	assert(!file_exists(test_fixture_join(region_path(which), ".idx"))
		   && "a closed handle must leave no journal behind");
	assert(!file_exists(test_fixture_join(region_path(which), ".idx_new"))
		   && "an unpublished journal sidecar must never survive");
}

/* While the handle is still OPEN the journal file legitimately exists -- that
 * is the whole round-3 win, one create per handle instead of one per
 * publication. What must hold is the INVARIANT the unlink used to express:
 * nothing in the file is replayable, i.e. either there is no record or its seq
 * is not newer than published_seq. */
static void assert_journal_retired(int which) {
	assert(!file_exists(test_fixture_join(region_path(which), ".idx_new"))
		   && "an unpublished journal sidecar must never survive");
	size_t size = 0;
	uint8_t* j = read_file_opt(test_fixture_join(region_path(which), ".idx"),
							   &size);
	if(!j)
		return;
	assert(size >= IDX3_HEAD && !memcmp(j, "CWIDX003", 8)
		   && "an open handle's journal must be a v3 in-place journal");
	uint32_t published = get_be32(j + 8);
	assert((published ^ get_be32(j + 12)) == 0xFFFFFFFFu
		   && "published_seq must carry its one's-complement guard");
	if(size >= IDX3_HEAD + IDX3_REC_HEAD && !memcmp(j + IDX3_HEAD, "CWRC", 4)) {
		uint32_t seq = get_be32(j + IDX3_HEAD + 8);
		assert(seq <= published
			   && "a completed batch must retire its journal record");
	}
	free(j);
}

static void open_world(string_t name, int which) {
	string_init_printf(name, "%s", world_dir(which));
}

/* Read chunk `index` back through the production loader and return its first
 * block id (fill_chunk's per-seed fingerprint), or -1 when absent. */
static int loaded_fingerprint(struct region_archive* ra, w_coord_t x,
							  w_coord_t z) {
	bool exists = false;
	if(!region_archive_contains(ra, x, z, &exists) || !exists)
		return -1;
	struct server_chunk loaded = {0};
	if(!region_archive_get_blocks(ra, x, z, &loaded, NULL))
		return -2;
	int id = loaded.ids[0];
	server_chunk_arrays_free(&loaded);
	return id;
}

/* --- 1. equivalence -------------------------------------------------------
 * `which` 0 writes each chunk with its own publish (the path every non-batched
 * caller still takes); `which` 1 wraps the whole sequence in one batch. */
static void run_sequence(int which, bool batched, const int* seq, size_t count,
						 struct server_chunk* sc) {
	remove(region_path(which));
	struct region_archive ra;
	string_t world;
	open_world(world, which);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	if(batched)
		region_archive_batch_begin(&ra);
	for(size_t k = 0; k < count; k++) {
		fill_chunk(sc, (unsigned)(100 + k));
		assert(region_archive_set_blocks(&ra, seq[k] % REGION_SIZE,
										 seq[k] / REGION_SIZE, sc, NULL));
	}
	if(batched) {
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
}

static void check_append_only_is_byte_identical(struct server_chunk* sc) {
	/* Every chunk written exactly once: nothing is freed inside the batch, so
	 * the allocator makes the identical placement decisions on both paths and
	 * the two containers must agree byte for byte. */
	const int seq[] = {0, 1, 2, 3, 4, 5, 6, 7};
	run_sequence(0, false, seq, sizeof(seq) / sizeof(*seq), sc);
	run_sequence(1, true, seq, sizeof(seq) / sizeof(*seq), sc);
	assert_same_bytes_modulo_timestamps(
		"an append-only batch must produce a byte-identical region file");
	assert_no_journal(0);
	assert_no_journal(1);
}

static void check_rewrites_preserve_every_record(struct server_chunk* sc) {
	/* Rewrites free space mid-batch, which the batch must keep reserved. Every
	 * chunk still reads back the identical record. */
	const int seq[] = {0, 1, 0, 2, 1, 0, 3, 2, 33, 64, 65, 33};
	run_sequence(0, false, seq, sizeof(seq) / sizeof(*seq), sc);
	run_sequence(1, true, seq, sizeof(seq) / sizeof(*seq), sc);
	assert_same_records(
		"a rewrite-heavy batch must store the identical chunk records");
	assert_no_journal(0);
	assert_no_journal(1);
}

static void check_multi_sector_batch(struct server_chunk* sc) {
	/* A 512-byte header sector spans 128 chunk indices; index 0 and index 200
	 * therefore land in different sectors and force TWO journal transactions
	 * inside one batch. */
	const int seq[] = {0, 200, 1, 201, 400, 900};
	run_sequence(0, false, seq, sizeof(seq) / sizeof(*seq), sc);
	run_sequence(1, true, seq, sizeof(seq) / sizeof(*seq), sc);
	assert_same_bytes_modulo_timestamps(
		"a multi-sector append-only batch must still be byte-identical");
	assert_no_journal(1);
}

static void check_batch_survives_overflow(struct server_chunk* sc) {
	/* More chunks than REGION_BATCH_MAX: the archive publishes what it has and
	 * keeps going, so the cap is a cadence bound, never a write limit. */
	int seq[REGION_BATCH_MAX + 9];
	for(size_t k = 0; k < sizeof(seq) / sizeof(*seq); k++)
		seq[k] = (int)k;
	run_sequence(0, false, seq, sizeof(seq) / sizeof(*seq), sc);
	run_sequence(1, true, seq, sizeof(seq) / sizeof(*seq), sc);
	assert_same_bytes_modulo_timestamps(
		"a batch past REGION_BATCH_MAX must still write every chunk");
	assert_no_journal(1);
}

/* --- 1b. the eviction path ------------------------------------------------
 * region_archive_sync is the LRU eviction barrier (server_world_chunk_region)
 * and the world-teardown barrier. A handle reaching it with an open batch is
 * about to disappear, so the pending deltas have nowhere to defer to: sync must
 * publish them, and destroy must publish anything sync did not. */

/* Write `count` chunks (indices from `seq`, fingerprint seed 200+k), then leave
 * the handle through `exit_via`: 0 = sync + destroy (the LRU eviction), 1 =
 * destroy alone (a close that skipped the barrier), 2 = batch_end + destroy
 * (the ordinary cadence). Every route must land the same published state. */
static void run_eviction(int which, bool batched, const int* seq, size_t count,
						 int exit_via, struct server_chunk* sc) {
	remove(region_path(which));
	struct region_archive ra;
	string_t world;
	open_world(world, which);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	if(batched)
		region_archive_batch_begin(&ra);
	for(size_t k = 0; k < count; k++) {
		fill_chunk(sc, (unsigned)(200 + k));
		assert(region_archive_set_blocks(&ra, seq[k] % REGION_SIZE,
										 seq[k] / REGION_SIZE, sc, NULL));
	}
	if(exit_via == 2) {
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
	}
	if(exit_via == 0) {
		assert(region_archive_sync(&ra));
		/* The eviction barrier published everything: the batch it interrupted
		 * has nothing left, so closing it is a no-op that still succeeds. */
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
		assert_journal_retired(which);
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(which);
}

/* Every chunk of `seq` must read back its own fingerprint through a FRESH
 * open -- i.e. the on-disk index names the new payloads, not the old ones. */
static void assert_eviction_published(int which, const int* seq, size_t count) {
	struct region_archive ra;
	string_t world;
	open_world(world, which);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(size_t k = 0; k < count; k++)
		assert(loaded_fingerprint(&ra, seq[k] % REGION_SIZE,
								  seq[k] / REGION_SIZE)
			   == 1 + (int)((200 + k) & 0x1F));
	assert(region_archive_destroy(&ra));
	string_clear(world);
}

static void check_evict_inside_batch(struct server_chunk* sc) {
	/* Two header sectors (index 200 lives in sector 1), so the eviction
	 * publishes a MULTI-sector batch through the single journal. */
	const int seq[] = {0, 1, 200, 201, 400};
	const size_t n = sizeof(seq) / sizeof(*seq);
	run_eviction(1, true, seq, n, 0, sc);
	assert_eviction_published(1, seq, n);
	/* Same sequence, same handle lifetime, but the batch is closed the
	 * ordinary way: the two must agree chunk record for chunk record. */
	run_eviction(0, true, seq, n, 2, sc);
	assert_same_records("an eviction mid-batch must publish exactly what "
						"batch_end would have published");
}

static void check_evict_outside_batch(struct server_chunk* sc) {
	/* No batch open: every write already published itself, so the eviction
	 * barrier has nothing pending and must still leave the identical file. */
	const int seq[] = {0, 1, 200, 201, 400};
	const size_t n = sizeof(seq) / sizeof(*seq);
	run_eviction(1, false, seq, n, 0, sc);
	assert_eviction_published(1, seq, n);
	run_eviction(0, false, seq, n, 1, sc);
	assert_same_bytes_modulo_timestamps(
		"an eviction outside a batch changes nothing on disk");
}

static void check_destroy_publishes_unclosed_batch(struct server_chunk* sc) {
	/* A handle closed with an open batch and no sync: destroy is the last
	 * chance to publish, and it must take it (otherwise the payloads are on
	 * disk with nothing naming them and the chunks silently revert). */
	const int seq[] = {0, 1, 200, 201, 400};
	const size_t n = sizeof(seq) / sizeof(*seq);
	run_eviction(1, true, seq, n, 1, sc);
	assert_eviction_published(1, seq, n);
}

static void check_mixed_save_and_evict_same_chunk(struct server_chunk* sc) {
	/* The same chunk saved twice in ONE batch, then evicted mid-batch. The
	 * chunk must read back the LAST write, and the range the on-disk index
	 * still named when the batch opened must not have been handed to any write
	 * in it -- the reservation has to survive an eviction publish just as it
	 * survives batch_end. */
	remove(region_path(1));
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 3);
	assert(region_archive_set_blocks(&ra, 5, 0, sc, NULL));

	size_t size;
	uint8_t* image = read_file(region_path(1), &size);
	uint32_t published = get_be32(image + 5 * 4);
	free(image);
	uint32_t reserved_start = published >> 8;
	uint32_t reserved_end = reserved_start + (published & 0xFF);
	assert(reserved_end > reserved_start);

	region_archive_batch_begin(&ra);
	fill_chunk(sc, 40);
	assert(region_archive_set_blocks(&ra, 5, 0, sc, NULL)); /* rewrite */
	fill_chunk(sc, 41);
	assert(region_archive_set_blocks(&ra, 6, 0, sc, NULL));
	fill_chunk(sc, 42);
	assert(region_archive_set_blocks(&ra, 8, 6, sc, NULL)); /* index 200 */
	fill_chunk(sc, 43);
	assert(region_archive_set_blocks(&ra, 5, 0, sc, NULL)); /* rewrite again */

	/* The eviction barrier, mid-batch. */
	assert(region_archive_sync(&ra));
	assert_journal_retired(1);
	assert(loaded_fingerprint(&ra, 5, 0) == 1 + (43 & 0x1F));
	assert(loaded_fingerprint(&ra, 6, 0) == 1 + (41 & 0x1F));
	assert(loaded_fingerprint(&ra, 8, 6) == 1 + (42 & 0x1F));
	assert(region_archive_destroy(&ra));
	string_clear(world);

	image = read_file(region_path(1), &size);
	const int touched[] = {5, 6, 200};
	for(size_t k = 0; k < sizeof(touched) / sizeof(*touched); k++) {
		uint32_t entry = get_be32(image + (size_t)touched[k] * 4);
		uint32_t start = entry >> 8;
		uint32_t end = start + (entry & 0xFF);
		assert(entry != 0);
		assert((end <= reserved_start || start >= reserved_end)
			   && "an eviction publish must honour the same old-range "
				  "reservation batch_end does");
	}
	free(image);

	/* And it survives a fresh open. */
	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(loaded_fingerprint(&ra, 5, 0) == 1 + (43 & 0x1F));
	assert(loaded_fingerprint(&ra, 6, 0) == 1 + (41 & 0x1F));
	assert(loaded_fingerprint(&ra, 8, 6) == 1 + (42 & 0x1F));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
}

/* --- 2. crash points ------------------------------------------------------ */

static void build_journal(uint8_t* record, uint32_t sector,
						  const uint8_t* before, const uint8_t* after) {
	memcpy(record, "CWIDX001", 8);
	put_u32(sector, record + 8);
	memcpy(record + 12, before, IDX_SECTOR);
	memcpy(record + 12 + IDX_SECTOR, after, IDX_SECTOR);
	put_u32((uint32_t)crc32(0, record, IDX_RECORD - 4), record + IDX_RECORD - 4);
}

/* Reopen world 1 and assert every chunk resolves to a COMPLETE state: either
 * every one of `n` batch chunks carries its new fingerprint, or every one
 * carries its old one. Never a mix, and never an unreadable record. */
static void assert_complete_state(const int* ids, size_t n, int old_fp,
								  int new_fp, const char* why) {
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD) && why);
	int first = loaded_fingerprint(&ra, ids[0] % REGION_SIZE,
								   ids[0] / REGION_SIZE);
	assert((first == old_fp || first == new_fp) && why);
	for(size_t k = 1; k < n; k++) {
		int fp = loaded_fingerprint(&ra, ids[k] % REGION_SIZE,
									ids[k] / REGION_SIZE);
		assert(fp == first
			   && "recovery must yield the complete old OR the complete new "
				  "state for the whole batch's sector -- never a mix");
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
}

static void check_crash_points(struct server_chunk* sc) {
	const int ids[] = {5, 6, 7};
	const size_t n = sizeof(ids) / sizeof(*ids);
	const int old_fp = 1 + (11 & 0x1F);
	const int new_fp = 1 + (22 & 0x1F);
	assert(old_fp != new_fp);

	/* Seed a committed "old" state. */
	remove(region_path(1));
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 11);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	size_t old_size;
	uint8_t* old_image = read_file(region_path(1), &old_size);

	/* Batch-write the "new" state and capture the published image. */
	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	region_archive_batch_begin(&ra);
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 22);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	assert(region_archive_destroy(&ra));
	string_clear(world);
	size_t new_size;
	uint8_t* new_image = read_file(region_path(1), &new_size);
	assert(new_size >= old_size);
	assert(memcmp(old_image, new_image, IDX_SECTOR) != 0
		   && "the batch must actually have flipped header sector 0");
	assert_complete_state(ids, n, old_fp, new_fp,
						  "a published batch reads back as the new state");

	/* All three manufactured states keep the NEW payloads on disk (the batch
	 * wrote them before touching the header) and differ only in the header
	 * sector plus the journal, exactly as a power cut inside publish would. */
	uint8_t* scratch = malloc(new_size);
	assert(scratch);
	uint8_t record[IDX_RECORD];

	/* A. crash after the journal, before the header flip: sector still holds
	 *    the before-image -> the complete OLD state. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	build_journal(record, 0, old_image, new_image);
	write_file(region_path(1), scratch, new_size);
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   sizeof(record));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "crash before the header flip -> complete old state");
	{
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, old_image, IDX_SECTOR) == 0
			   && "the before-image must be restored verbatim");
		free(now);
	}

	/* B. crash after the header flip, before the journal is retired: the
	 *    sector already holds the after-image -> the complete NEW state, and
	 *    the stale journal is cleaned up rather than replayed backwards. */
	memcpy(scratch, new_image, new_size);
	build_journal(record, 0, old_image, new_image);
	write_file(region_path(1), scratch, new_size);
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   sizeof(record));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "crash after the header flip -> complete new state");
	{
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, new_image, IDX_SECTOR) == 0
			   && "a committed sector must never be rolled back");
		free(now);
	}

	/* C. the sector itself was TORN across the batch's OWN entries: chunk
	 *    ids[0]'s new entry landed, ids[1] and ids[2] still hold their old
	 *    ones. That is the mixed state the journal exists to forbid; neither
	 *    image matches, so recovery restores the before-image -- the complete
	 *    OLD state. (Tearing the sector's unused tail would not test anything:
	 *    both images are zero out there, so the live sector would compare equal
	 *    to the after-image and recovery would rightly call it committed.) */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	assert(memcmp(scratch, old_image, IDX_SECTOR) != 0
		   && memcmp(scratch, new_image, IDX_SECTOR) != 0
		   && "the manufactured tear must match neither image");
	build_journal(record, 0, old_image, new_image);
	write_file(region_path(1), scratch, new_size);
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   sizeof(record));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "a torn header sector -> complete old state");
	{
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, old_image, IDX_SECTOR) == 0
			   && "a torn sector must be repaired from the before-image");
		free(now);
	}

	/* D. an unpublished batch (crash before ANY of the above) leaves the
	 *    payloads on disk unreferenced: the complete old state, no journal. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	write_file(region_path(1), scratch, new_size);
	remove(test_fixture_join(region_path(1), ".idx"));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "an unpublished batch -> complete old state");

	free(scratch);
	free(old_image);
	free(new_image);
}

/* --- 2b. crash points around the MULTI-SECTOR journal (round 2) ------------
 * One journal record now carries the before/after image of EVERY header sector
 * the batch touches, so a power cut can land between two sector writes. The
 * guarantee is per CHUNK, not per batch: each sector independently resolves to
 * its complete before-image or its complete after-image, and every chunk in it
 * reads back a complete record either way. */

static void build_journal_v2(uint8_t* record, const uint32_t* sectors,
							 size_t n, const uint8_t* before_file,
							 const uint8_t* after_file) {
	memcpy(record, "CWIDX002", 8);
	put_u32((uint32_t)n, record + 8);
	for(size_t i = 0; i < n; i++) {
		uint8_t* e = record + 12 + i * IDX_ENTRY;
		put_u32(sectors[i], e);
		memcpy(e + 4, before_file + sectors[i] * IDX_SECTOR, IDX_SECTOR);
		memcpy(e + 4 + IDX_SECTOR, after_file + sectors[i] * IDX_SECTOR,
			   IDX_SECTOR);
	}
	size_t len = IDX_RECORD_V2(n);
	put_u32((uint32_t)crc32(0, record, len - 4), record + len - 4);
}

/* Every id must read back a COMPLETE record whose fingerprint is `expect[k]`
 * (old_fp or new_fp) -- never absent, never unreadable, never a third value. */
static void assert_each_complete(const int* ids, const int* expect, size_t n,
								 const char* why) {
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD) && why);
	for(size_t k = 0; k < n; k++) {
		int fp = loaded_fingerprint(&ra, ids[k] % REGION_SIZE,
									ids[k] / REGION_SIZE);
		assert(fp == expect[k] && why);
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
}

static void check_multi_sector_crash_points(struct server_chunk* sc) {
	/* 5 and 6 live in header sector 0; 200 and 201 in sector 1. */
	const int ids[] = {5, 6, 200, 201};
	const size_t n = sizeof(ids) / sizeof(*ids);
	const uint32_t sectors[] = {0, 1};
	const int old_fp = 1 + (11 & 0x1F);
	const int new_fp = 1 + (22 & 0x1F);
	assert(old_fp != new_fp);

	remove(region_path(1));
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 11);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	size_t old_size;
	uint8_t* old_image = read_file(region_path(1), &old_size);

	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	region_archive_batch_begin(&ra);
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 22);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	assert(region_archive_destroy(&ra));
	string_clear(world);
	size_t new_size;
	uint8_t* new_image = read_file(region_path(1), &new_size);
	assert(new_size >= old_size);
	for(size_t i = 0; i < 2; i++)
		assert(memcmp(old_image + sectors[i] * IDX_SECTOR,
					  new_image + sectors[i] * IDX_SECTOR, IDX_SECTOR)
				   != 0
			   && "the batch must have flipped BOTH header sectors");

	uint8_t* scratch = malloc(new_size);
	uint8_t* record = malloc(IDX_RECORD_V2(2));
	assert(scratch && record);
	/* test_fixture_join hands back one of four rotating static buffers, so a
	 * path that must survive other calls has to be copied. */
	char journal[384];
	snprintf(journal, sizeof(journal), "%s", test_fixture_join(region_path(1),
															  ".idx"));
	build_journal_v2(record, sectors, 2, old_image, new_image);

	/* E. crash after the journal, before ANY sector flip -> both sectors hold
	 *    their before-image -> the complete OLD state everywhere. */
	{
		const int expect[] = {old_fp, old_fp, old_fp, old_fp};
		memcpy(scratch, new_image, new_size);
		memcpy(scratch, old_image, 2 * IDX_SECTOR);
		write_file(region_path(1), scratch, new_size);
		write_file(journal, record, IDX_RECORD_V2(2));
		assert_each_complete(ids, expect, n,
							 "crash before any flip -> complete old state");
	}

	/* F. crash BETWEEN the two sector writes: sector 0 already carries the
	 *    after-image, sector 1 does not. Recovery must keep the committed
	 *    sector and restore only the uncommitted one -- a per-chunk-consistent
	 *    mixed outcome, which is exactly what a per-sector journal produced in
	 *    round 1 when it crashed between two of its transactions. */
	{
		const int expect[] = {new_fp, new_fp, old_fp, old_fp};
		memcpy(scratch, new_image, new_size);
		memcpy(scratch + IDX_SECTOR, old_image + IDX_SECTOR, IDX_SECTOR);
		write_file(region_path(1), scratch, new_size);
		write_file(journal, record, IDX_RECORD_V2(2));
		assert_each_complete(ids, expect, n,
							 "crash between two sector writes -> each sector "
							 "resolves completely, no chunk is a mix");
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, new_image, IDX_SECTOR) == 0
			   && "a committed sector must never be rolled back");
		assert(memcmp(now + IDX_SECTOR, old_image + IDX_SECTOR, IDX_SECTOR) == 0
			   && "the uncommitted sector must be restored verbatim");
		free(now);
	}

	/* G. sector 1 TORN across the batch's own entries while sector 0 committed:
	 *    the torn sector matches neither image, so it is restored; the
	 *    committed one is kept. */
	{
		const int expect[] = {new_fp, new_fp, old_fp, old_fp};
		memcpy(scratch, new_image, new_size);
		memcpy(scratch + IDX_SECTOR, old_image + IDX_SECTOR, IDX_SECTOR);
		memcpy(scratch + (size_t)ids[2] * 4, new_image + (size_t)ids[2] * 4, 4);
		assert(memcmp(scratch + IDX_SECTOR, old_image + IDX_SECTOR, IDX_SECTOR)
				   != 0
			   && memcmp(scratch + IDX_SECTOR, new_image + IDX_SECTOR,
						 IDX_SECTOR)
				   != 0
			   && "the manufactured tear must match neither image");
		write_file(region_path(1), scratch, new_size);
		write_file(journal, record, IDX_RECORD_V2(2));
		assert_each_complete(ids, expect, n,
							 "a torn sector inside a multi-sector journal -> "
							 "that sector's complete old state");
	}

	/* H. a TRUNCATED multi-sector record (the crash landed inside the journal
	 *    write itself). Nothing on disk was mutated yet, so the record must be
	 *    refused rather than half-replayed -- and the open must fail closed
	 *    rather than silently salvage a header from a record it cannot trust. */
	{
		memcpy(scratch, new_image, new_size);
		memcpy(scratch, old_image, 2 * IDX_SECTOR);
		write_file(region_path(1), scratch, new_size);
		write_file(journal, record, IDX_RECORD_V2(2) - 100);
		struct region_archive bad;
		string_t w2;
		open_world(w2, 1);
		assert(!region_archive_create(&bad, w2, 0, 0, WORLD_DIM_OVERWORLD)
			   && "a truncated journal must be refused, not guessed at");
		string_clear(w2);
		assert(remove(journal) == 0);
	}

	/* I. ROUND-1 COMPATIBILITY: a v1 (CWIDX001) single-sector record left by
	 *    the previous build must still replay. */
	{
		const int expect[] = {old_fp, old_fp, new_fp, new_fp};
		uint8_t v1[IDX_RECORD];
		memcpy(scratch, new_image, new_size);
		memcpy(scratch, old_image, IDX_SECTOR);
		build_journal(v1, 0, old_image, new_image);
		write_file(region_path(1), scratch, new_size);
		write_file(journal, v1, sizeof(v1));
		assert_each_complete(ids, expect, n,
							 "a round-1 v1 journal must still recover");
	}

	free(record);
	free(scratch);
	free(old_image);
	free(new_image);
}

/* --- 3. the batch's own new invariant -------------------------------------
 * While a batch is open, a chunk's OLD range is still what the ON-DISK index
 * names, so no later write in the same batch may be placed on top of it. */
static void check_in_batch_old_range_reserved(struct server_chunk* sc) {
	remove(region_path(1));
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 3);
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));

	size_t size;
	uint8_t* image = read_file(region_path(1), &size);
	uint32_t published = get_be32(image);
	free(image);
	uint32_t reserved_start = published >> 8;
	uint32_t reserved_end = reserved_start + (published & 0xFF);
	assert(reserved_end > reserved_start);

	region_archive_batch_begin(&ra);
	/* Rewrite chunk 0 (freeing its old range in memory only), then write two
	 * more chunks that the per-write allocator would happily place into it. */
	fill_chunk(sc, 4);
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));
	for(int k = 1; k <= 3; k++) {
		fill_chunk(sc, (unsigned)(4 + k));
		assert(region_archive_set_blocks(&ra, k, 0, sc, NULL));
	}
	/* The on-disk index STILL names the old range at this point; nothing the
	 * batch wrote may have landed inside it. */
	image = read_file(region_path(1), &size);
	assert(get_be32(image) == published
		   && "an open batch must not have published the header yet");
	free(image);
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);

	image = read_file(region_path(1), &size);
	for(int k = 0; k <= 3; k++) {
		uint32_t entry = get_be32(image + (size_t)k * 4);
		uint32_t start = entry >> 8;
		uint32_t end = start + (entry & 0xFF);
		assert(entry != 0);
		assert((end <= reserved_start || start >= reserved_end)
			   && "no in-batch write may land on a range the on-disk index "
				  "still named");
	}
	free(image);

	/* Every chunk still reads back what was written last. */
	for(int k = 0; k <= 3; k++)
		assert(loaded_fingerprint(&ra, k, 0) == 1 + ((4 + (k ? k : 0)) & 0x1F));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
}

/* --- 3. the v3 sequence gate (round 3) -------------------------------------
 * The in-place journal replaced "the record is retired by unlinking the file"
 * with "the record is retired by publishing its seq". Three outcomes have to be
 * pinned, because with an unlink they were structurally impossible:
 *   STALE   seq <= published_seq  -> ignored, not a rollback
 *   NEWER   seq >  published_seq  -> applied exactly as v1/v2 were
 *   TORN    crc mismatch          -> refused before any sector is touched
 * Each case manufactures the on-disk state directly (the writer never leaves a
 * stale or torn record behind on purpose) and reopens through the production
 * loader. */
static void build_journal_v3(uint8_t* file, uint32_t published, uint32_t seq,
							 const uint32_t* sectors, size_t n,
							 const uint8_t* before_file,
							 const uint8_t* after_file) {
	memset(file, 0, IDX3_RECORD(n));
	memcpy(file, "CWIDX003", 8);
	put_u32(published, file + 8);
	/* published_seq is stored with its one's complement beside it: retirement
	 * writes those bytes with no barrier, and a torn pair must degrade to 0
	 * (replay, the safe direction) rather than to a spuriously high seq. */
	put_u32(~published, file + 12);
	uint8_t* rec = file + IDX3_HEAD;
	memcpy(rec, "CWRC", 4);
	put_u32((uint32_t)n, rec + 4);
	put_u32(seq, rec + 8);
	for(size_t i = 0; i < n; i++) {
		uint8_t* e = rec + IDX3_REC_HEAD + i * IDX_ENTRY;
		put_u32(sectors[i], e);
		memcpy(e + 4, before_file + sectors[i] * IDX_SECTOR, IDX_SECTOR);
		memcpy(e + 4 + IDX_SECTOR, after_file + sectors[i] * IDX_SECTOR,
			   IDX_SECTOR);
	}
	size_t body = IDX3_REC_HEAD + n * IDX_ENTRY;
	put_u32((uint32_t)crc32(0, rec, body), rec + body);
}

static void check_journal_seq_gate(struct server_chunk* sc) {
	const int ids[] = {5, 6, 7};
	const size_t n = sizeof(ids) / sizeof(*ids);
	const int old_fp = 1 + (11 & 0x1F);
	const int new_fp = 1 + (22 & 0x1F);
	const uint32_t sectors[] = {0};
	assert(old_fp != new_fp);

	/* Committed OLD state, then a published NEW state -- the same two images
	 * check_crash_points uses, so "applied" and "ignored" are distinguishable
	 * by a chunk fingerprint rather than by a byte diff alone. */
	remove(region_path(1));
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 11);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	size_t old_size;
	uint8_t* old_image = read_file(region_path(1), &old_size);

	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	/* Nothing published yet: the journal is opened lazily, so a read-only
	 * handle never touches the directory at all. */
	assert_no_journal(1);
	region_archive_batch_begin(&ra);
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 22);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	/* The publication wrote a record IN PLACE and retired it by seq -- the file
	 * stays open and reusable for the rest of the handle's life. */
	assert(file_exists(test_fixture_join(region_path(1), ".idx"))
		   && "the v3 journal is opened once per handle, not per publication");
	{
		size_t js;
		uint8_t* j = read_file_opt(test_fixture_join(region_path(1), ".idx"),
								   &js);
		assert(j && js >= IDX3_HEAD + IDX3_REC_HEAD);
		assert(!memcmp(j, "CWIDX003", 8) && !memcmp(j + IDX3_HEAD, "CWRC", 4)
			   && "the record must still be there -- retirement is a seq, "
				  "not an unlink");
		assert(get_be32(j + IDX3_HEAD + 8) == get_be32(j + 8)
			   && get_be32(j + 8) >= 1
			   && "published_seq must have caught up with the record");
		free(j);
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	size_t new_size;
	uint8_t* new_image = read_file(region_path(1), &new_size);
	assert(memcmp(old_image, new_image, IDX_SECTOR) != 0);

	uint8_t* scratch = malloc(new_size);
	uint8_t* record = malloc(IDX3_RECORD(1));
	assert(scratch && record);

	/* STALE: the record would roll the header back to the old image, but its
	 * seq is not newer than published_seq. The header must survive untouched
	 * and every chunk must still read the NEW state. */
	memcpy(scratch, new_image, new_size);
	write_file(region_path(1), scratch, new_size);
	build_journal_v3(record, 9, 4, sectors, 1, old_image, new_image);
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   IDX3_RECORD(1));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "a stale record must be ignored, not replayed");
	{
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, new_image, IDX_SECTOR) == 0
			   && "seq <= published_seq must leave the header alone");
		free(now);
	}

	/* NEWER: identical record, one greater seq. Now it IS the authority and
	 * the header goes back to the before-image (the complete old state). */
	memcpy(scratch, new_image, new_size);
	write_file(region_path(1), scratch, new_size);
	build_journal_v3(record, 4, 9, sectors, 1, old_image, new_image);
	/* Force the flip: with live == after the replay is a no-op by design, so
	 * tear the sector the way a power cut would to prove the record is read. */
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	write_file(region_path(1), scratch, new_size);
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   IDX3_RECORD(1));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "a newer record must be applied");
	{
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, old_image, IDX_SECTOR) == 0
			   && "seq > published_seq must repair the torn sector from the "
				  "before-image");
		free(now);
	}

	/* TORN: same newer record, one flipped byte inside the crc'd body. The
	 * record must be refused and the torn sector left exactly as found -- the
	 * loader may not salvage a header from a record it does not fully trust. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	write_file(region_path(1), scratch, new_size);
	build_journal_v3(record, 4, 9, sectors, 1, old_image, new_image);
	record[IDX3_HEAD + IDX3_REC_HEAD + 4 + 8] ^= 0xFF;
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   IDX3_RECORD(1));
	{
		struct region_archive probe;
		string_t w;
		open_world(w, 1);
		assert(region_archive_create(&probe, w, 0, 0, WORLD_DIM_OVERWORLD));
		assert(region_archive_destroy(&probe));
		string_clear(w);
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, scratch, IDX_SECTOR) == 0
			   && "a torn record must be refused before any sector is touched");
		free(now);
	}

	/* TORN published_seq: a stale record plus a published_seq whose complement
	 * guard does not match. The gate must fall back to 0 -- i.e. REPLAY, the
	 * conservative direction -- rather than trust a possibly-inflated seq and
	 * skip a record that still had a torn sector to repair. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	write_file(region_path(1), scratch, new_size);
	build_journal_v3(record, 9, 4, sectors, 1, old_image, new_image);
	put_u32(0, record + 12); /* break the complement guard */
	write_file(test_fixture_join(region_path(1), ".idx"), record,
			   IDX3_RECORD(1));
	assert_complete_state(ids, n, old_fp, new_fp,
						  "a torn published_seq must fall back to replaying");
	{
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, old_image, IDX_SECTOR) == 0
			   && "an untrustworthy published_seq must not suppress a replay");
		free(now);
	}

	free(record);
	free(scratch);
	free(old_image);
	free(new_image);
	printf("PASS journal seq gate: stale ignored, newer applied, "
		   "torn record refused, torn published_seq replays\n");
}

/* --- 4. in-place record-LENGTH changes (round 3 fix round, item 2) --------
 * The v3 journal rewrites its ONE record in place and never truncates the
 * file (region_journal_open's own doc comment: "Every later write is in
 * place"). A record with FEWER sectors than the one it physically overwrote
 * therefore leaves that longer record's own trailing bytes -- its extra entry
 * and its own crc -- sitting untouched past the new record's shorter length,
 * a stale tail. Two properties have to hold for that to be safe:
 *   (1) a torn record -- shorter OR longer than the one resident in the file
 *       -- is refused by ITS OWN crc, walked over ITS OWN freshly-written
 *       declared count, and leaves EVERY live sector untouched, including one
 *       named only by a stale tail the crc walk never reads.
 *   (2) a CLEAN shorter record validates and applies using ONLY its own
 *       declared count: the stale tail left by the longer record it
 *       overwrote must never be read into the crc walk or mistaken for a
 *       second entry that would flip a sector the new record never named.
 * Reuses check_multi_sector_crash_points' fixture: chunks 5/6 live in header
 * sector 0, 200/201 in sector 1. */
static void check_journal_record_length_tear(struct server_chunk* sc) {
	const int ids[] = {5, 6, 200, 201};
	const size_t n = sizeof(ids) / sizeof(*ids);
	const uint32_t sectors[] = {0, 1};

	remove(region_path(1));
	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 11);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	size_t old_size;
	uint8_t* old_image = read_file(region_path(1), &old_size);

	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	region_archive_batch_begin(&ra);
	for(size_t k = 0; k < n; k++) {
		fill_chunk(sc, 22);
		assert(region_archive_set_blocks(&ra, ids[k] % REGION_SIZE,
										 ids[k] / REGION_SIZE, sc, NULL));
	}
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	assert(region_archive_destroy(&ra));
	string_clear(world);
	size_t new_size;
	uint8_t* new_image = read_file(region_path(1), &new_size);
	assert(new_size >= old_size);
	for(size_t i = 0; i < 2; i++)
		assert(memcmp(old_image + sectors[i] * IDX_SECTOR,
					  new_image + sectors[i] * IDX_SECTOR, IDX_SECTOR)
				   != 0
			   && "the batch must have flipped BOTH header sectors");

	char journal[384];
	snprintf(journal, sizeof(journal), "%s",
			 test_fixture_join(region_path(1), ".idx"));

	uint8_t* scratch = malloc(new_size);
	uint8_t* long_record = malloc(IDX3_RECORD(2));
	uint8_t* short_record = malloc(IDX3_RECORD(1));
	assert(scratch && long_record && short_record);

	/* (1a) a TORN shorter record overwrites a resident LONGER one: sector 0
	 * is left torn/mixed (so a correct refusal has something to preserve),
	 * sector 1 is left at new_image so a spurious touch is detectable. */
	build_journal_v3(long_record, 0, 1, sectors, 2, old_image, new_image);
	write_file(journal, long_record, IDX3_RECORD(2));
	build_journal_v3(short_record, 0, 2, sectors, 1, old_image, new_image);
	short_record[IDX3_HEAD + IDX3_REC_HEAD + 4 + 8] ^= 0xFF; /* tear it */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	write_file(region_path(1), scratch, new_size);
	write_file_inplace(journal, short_record, IDX3_RECORD(1));
	{
		size_t js;
		uint8_t* j = read_file(journal, &js);
		assert(js == IDX3_RECORD(2)
			   && "the in-place overwrite must not have truncated the file "
				  "-- the stale tail has to still be physically there for "
				  "this test to mean anything");
		free(j);
	}
	{
		struct region_archive probe;
		string_t w;
		open_world(w, 1);
		assert(region_archive_create(&probe, w, 0, 0, WORLD_DIM_OVERWORLD));
		assert(region_archive_destroy(&probe));
		string_clear(w);
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, scratch, 2 * IDX_SECTOR) == 0
			   && "a torn shorter record over a longer resident one must be "
				  "refused, leaving every live sector -- including the one "
				  "only the stale tail could name -- untouched");
		free(now);
		assert_no_journal(1);
	}

	/* (1b) the other direction: a TORN longer record overwrites a resident
	 * SHORTER one. The file only grows here (no stale tail), but refusal and
	 * untouched-sectors must hold exactly the same way. */
	build_journal_v3(short_record, 0, 1, sectors, 1, old_image, new_image);
	write_file(journal, short_record, IDX3_RECORD(1));
	build_journal_v3(long_record, 0, 2, sectors, 2, old_image, new_image);
	long_record[IDX3_HEAD + IDX3_REC_HEAD + 4 + 8] ^= 0xFF; /* tear it */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	write_file(region_path(1), scratch, new_size);
	write_file_inplace(journal, long_record, IDX3_RECORD(2));
	{
		struct region_archive probe;
		string_t w;
		open_world(w, 1);
		assert(region_archive_create(&probe, w, 0, 0, WORLD_DIM_OVERWORLD));
		assert(region_archive_destroy(&probe));
		string_clear(w);
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, scratch, 2 * IDX_SECTOR) == 0
			   && "a torn longer record over a shorter resident one must be "
				  "refused, leaving every live sector untouched");
		free(now);
		assert_no_journal(1);
	}

	/* (2) a CLEAN shorter record, written in place over a resident longer
	 * one, must validate and apply using ONLY its own freshly-written
	 * declared count. Sector 0 is left torn/mixed (forcing an actual
	 * restore-from-before, so applying is observable); sector 1 is left at
	 * new_image, deliberately DIFFERENT from what the stale tail's own
	 * before-image would restore it to (old_image) -- if a regression ever
	 * made the crc walk or the entry loop read past the short record's own
	 * count into the stale tail, sector 1 would flip to old_image here. */
	build_journal_v3(long_record, 0, 1, sectors, 2, old_image, new_image);
	write_file(journal, long_record, IDX3_RECORD(2));
	build_journal_v3(short_record, 0, 2, sectors, 1, old_image, new_image);
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, old_image, IDX_SECTOR);
	memcpy(scratch + (size_t)ids[0] * 4, new_image + (size_t)ids[0] * 4, 4);
	write_file(region_path(1), scratch, new_size);
	write_file_inplace(journal, short_record, IDX3_RECORD(1));
	{
		size_t js;
		uint8_t* j = read_file(journal, &js);
		assert(js == IDX3_RECORD(2)
			   && "the clean overwrite must not truncate the file either -- "
				  "the stale tail is still physically there while this "
				  "record validates and applies");
		free(j);
	}
	{
		struct region_archive probe;
		string_t w;
		open_world(w, 1);
		assert(region_archive_create(&probe, w, 0, 0, WORLD_DIM_OVERWORLD));
		assert(region_archive_destroy(&probe));
		string_clear(w);
		size_t got;
		uint8_t* now = read_file(region_path(1), &got);
		assert(memcmp(now, old_image, IDX_SECTOR) == 0
			   && "sector 0, named by the short record's own declared count, "
				  "must be restored from ITS before-image");
		assert(memcmp(now + IDX_SECTOR, new_image + IDX_SECTOR, IDX_SECTOR)
				   == 0
			   && "sector 1, named only by the stale tail left by the "
				  "longer record it overwrote, must NOT be touched -- proving "
				  "the replay walked exactly the short record's own declared "
				  "count and never validated the stale bytes past it");
		free(now);
		assert_no_journal(1);
	}

	free(short_record);
	free(long_record);
	free(scratch);
	free(old_image);
	free(new_image);
	printf("PASS journal record-length tear: torn shorter/longer refused "
		   "with every sector untouched; a clean shorter record applies by "
		   "its own declared count only, never validating a longer record's "
		   "stale tail\n");
}

/* --- 5. region_archive_create_new must not inherit a stale journal --------
 * (round 3 fix round, item 5, pre-existing gap.) create_new is reached
 * whenever region_archive_create's own open failed -- including "the .mcr was
 * removed (by a repair tool, a migration, a test reset) but its .idx sidecar
 * was left behind". create_new writes a brand-new, all-zero header and falls
 * through to region_archive_create, whose recovery has no way to know a
 * journal it finds predates the file it is about to open: left alone, a
 * stale-but-otherwise-valid record would replay its after-image onto header
 * bytes that have nothing to do with it. */
static void check_create_new_removes_stale_journal(void) {
	remove(region_path(1));
	char journal[384];
	snprintf(journal, sizeof(journal), "%s",
			 test_fixture_join(region_path(1), ".idx"));
	remove(journal);

	/* A stale, otherwise-valid v3 record naming sector 0, with a NON-ZERO
	 * after-image: if it were ever replayed onto the fresh all-zero header
	 * create_new is about to write, sector 0 would stop being all-zero. */
	uint8_t before[IDX_SECTOR], after[IDX_SECTOR];
	memset(before, 0, sizeof(before));
	memset(after, 0x77, sizeof(after));
	const uint32_t sectors[] = {0};
	uint8_t* record = malloc(IDX3_RECORD(1));
	assert(record);
	build_journal_v3(record, 0, 1, sectors, 1, before, after);
	write_file(journal, record, IDX3_RECORD(1));
	free(record);
	assert(file_exists(journal)
		   && "the stale journal must be on disk before create_new runs, or "
			  "this test proves nothing");

	struct region_archive ra;
	string_t world;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_destroy(&ra));
	string_clear(world);

	size_t got;
	uint8_t* now = read_file(region_path(1), &got);
	uint8_t zero[IDX_SECTOR];
	memset(zero, 0, sizeof(zero));
	assert(memcmp(now, zero, IDX_SECTOR) == 0
		   && "a stale journal left by a previous archive at this path must "
			  "never be replayed onto the fresh file create_new just wrote");
	free(now);
	assert_no_journal(1);
	printf("PASS create_new stale journal: a leftover .idx from a prior "
		   "archive at this path is removed before the new file can be "
		   "recovered against it\n");
}

/* --- 6. THE CANONICAL RECORD FLAG (round 4, 2026-09-08) -------------------
 * region_archive_set_chunk skips its pre-save READ-BACK when the stored record
 * is flagged canonical (bit 31 of the per-chunk timestamp entry). Three things
 * have to hold and all three are proved here against the SAME scripted saves:
 *
 *   (a) EQUIVALENCE. The records the skipping path stores are byte-identical
 *       to the ones the preservation merge stores. World 0 runs normally;
 *       world 1 has the flag stripped out of its timestamp table between every
 *       save, so it takes the read-back path every single time -- which is
 *       also exactly what a world written by Beta, or by a CaveWii build older
 *       than this one, looks like on disk.
 *   (b) ZERO READS. A canonical save reads no chunk record at all: the archive
 *       io census must show the same bytes_read as a save into an EMPTY slot,
 *       and strictly fewer than the read-back path's.
 *   (c) THE FLAG IS EARNED, NOT ASSUMED. A save that stores a tile entity is
 *       not canonical, so it must CLEAR the flag and send the following save
 *       back through the read-back path. */

/* Clear bit 31 of every per-chunk timestamp entry -- i.e. manufacture the
 * on-disk state of a region this feature never touched. */
static void strip_canon_flags(int which) {
	size_t size = 0;
	uint8_t* file = read_file(region_path(which), &size);
	assert(size >= 2 * REGION_SECTOR_SIZE);
	for(size_t k = 0; k < REGION_SIZE * REGION_SIZE; k++)
		file[REGION_SECTOR_SIZE + k * 4] &= 0x7F;
	write_file_inplace(region_path(which), file, 2 * REGION_SECTOR_SIZE);
	free(file);
}

/* A tile-entity snapshot the archive treats as a COMPLETE image, which is what
 * region_archive_set_chunk writes CaveWiiTileEntitiesComplete=1 for. */
static struct chunk_tile_entities complete_te(void) {
	struct chunk_tile_entities te = {0};
	te.legacy_flat_complete = true;
	return te;
}

static void canon_run(int which, bool strip, struct server_chunk* sc,
					  struct te_sign* sign_at_round, unsigned rounds) {
	remove(region_path(which));
	remove(test_fixture_join(region_path(which), ".idx"));
	string_t world;
	open_world(world, which);
	for(unsigned round = 0; round < rounds; round++) {
		struct region_archive ra;
		if(round == 0)
			assert(region_archive_create_new(&ra, world, 0, 0,
											 WORLD_DIM_OVERWORLD));
		else
			assert(region_archive_create(&ra, world, 0, 0,
										 WORLD_DIM_OVERWORLD));
		struct chunk_tile_entities te = complete_te();
		if(sign_at_round && round == 2) {
			te.signs = sign_at_round;
			te.n_sign = 1;
		}
		struct chunk_entities entities = {0};
		fill_chunk(sc, 300u + round);
		region_archive_batch_begin(&ra);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
		assert(region_archive_sync(&ra));
		assert(region_archive_destroy(&ra));
		if(strip)
			strip_canon_flags(which);
	}
	string_clear(world);
}

static void check_canonical_records_are_identical(struct server_chunk* sc) {
	canon_run(0, false, sc, NULL, 4);
	canon_run(1, true, sc, NULL, 4);
	assert_same_records(
		"a save that skipped the read-back must store the identical record to "
		"one that ran the whole preservation merge");
	assert_no_journal(0);
	assert_no_journal(1);

	/* The same claim with a tile entity in the middle of the sequence: that
	 * save is NOT canonical, so the flag has to drop and round 3 has to go
	 * back through the merge -- and still store the identical record. */
	struct te_sign sign = {0};
	sign.x = 3;
	sign.y = 64;
	sign.z = 5;
	memcpy(sign.text, "canon", 5);
	canon_run(0, false, sc, &sign, 4);
	canon_run(1, true, sc, &sign, 4);
	assert_same_records(
		"a non-canonical save in the middle of the sequence must clear the "
		"flag and still leave both paths byte-identical");
	printf("PASS canonical equivalence: records stored by the read-back-free "
		   "path are byte-identical to the preservation merge's, with and "
		   "without a tile entity breaking the canonical run\n");
}

static void check_canonical_read_counters(struct server_chunk* sc) {
	remove(region_path(0));
	remove(test_fixture_join(region_path(0), ".idx"));
	string_t world;
	open_world(world, 0);
	struct region_archive ra;
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct chunk_tile_entities te = complete_te();
	struct chunk_entities entities = {0};

	uint32_t rj0[24], rj1[24];
	uint32_t fopen_n, fread_n;
	uint64_t empty_bytes, canon_bytes, merge_bytes;

	/* 1. The FIRST save of a slot: nothing exists, so nothing can be read
	 * back. This is the zero-read reference point. */
	fill_chunk(sc, 400);
	region_archive_io_stats_reset();
	region_archive_journal_stats(rj0);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	assert(region_archive_sync(&ra));
	region_archive_journal_stats(rj1);
	region_archive_io_stats(&fopen_n, &fread_n, &empty_bytes);
	assert(rj1[15] == rj0[15] && rj1[16] == rj0[16]
		   && "a first write neither reads back nor claims a canonical skip");
	assert(rj1[17] == rj0[17] + 1
		   && "a plain first write publishes a CANONICAL record");

	/* 2. The SECOND save over that canonical record: the skip fires and the
	 * archive reads exactly as many bytes as the empty-slot write did. */
	fill_chunk(sc, 401);
	region_archive_io_stats_reset();
	region_archive_journal_stats(rj0);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	assert(region_archive_sync(&ra));
	region_archive_journal_stats(rj1);
	region_archive_io_stats(&fopen_n, &fread_n, &canon_bytes);
	assert(rj1[16] == rj0[16] + 1 && rj1[15] == rj0[15]
		   && "a save over a canonical record must skip the read-back");
	assert(canon_bytes == empty_bytes
		   && "a canonical save must read no more than a write into an empty "
			  "slot -- i.e. it must not read the stored record at all");
	assert(region_archive_destroy(&ra));

	/* 3. The SAME save with the flag stripped -- an old world, a Beta world,
	 * or a world an external tool touched. The read-back returns, and with it
	 * tens of kilobytes of record reads. */
	strip_canon_flags(0);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 402);
	region_archive_io_stats_reset();
	region_archive_journal_stats(rj0);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	assert(region_archive_sync(&ra));
	region_archive_journal_stats(rj1);
	region_archive_io_stats(&fopen_n, &fread_n, &merge_bytes);
	assert(rj1[15] == rj0[15] + 1 && rj1[16] == rj0[16]
		   && "an index with no canonical flag must take the read-back path");
	fprintf(stderr, "DBG empty=%llu canon=%llu merge=%llu\n",
			(unsigned long long)empty_bytes, (unsigned long long)canon_bytes,
			(unsigned long long)merge_bytes);
	assert(merge_bytes >= canon_bytes + REGION_SECTOR_SIZE
		   && "the read-back path must actually read the stored record");
	assert(region_archive_destroy(&ra));
	string_clear(world);
	printf("PASS canonical read census: first write %llu bytes, canonical "
		   "rewrite %llu bytes (identical), stripped-flag rewrite %llu bytes\n",
		   (unsigned long long)empty_bytes, (unsigned long long)canon_bytes,
		   (unsigned long long)merge_bytes);
}

static void check_pending_canonical_rewrite(struct server_chunk* sc) {
	remove(region_path(0));
	string_t world;
	open_world(world, 0);
	struct region_archive ra;
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct chunk_tile_entities te = complete_te();
	struct chunk_entities entities = {0};
	uint32_t before[24], after[24];
	region_archive_batch_begin(&ra);
	fill_chunk(sc, 910);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	region_archive_journal_stats(before);
	fill_chunk(sc, 911);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	region_archive_journal_stats(after);
	assert(after[16] == before[16] + 1 && after[15] == before[15]
		   && "second canonical write in one open batch must trust latest pending record");
	assert(region_archive_batch_end(&ra));
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	remove(region_path(1));
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	region_archive_batch_begin(&ra);
	fill_chunk(sc, 910);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	/* A second handle disarms the certificate, forcing the read-back control. */
	struct region_archive second;
	assert(region_archive_create(&second, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_destroy(&second));
	fill_chunk(sc, 911);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	assert(region_archive_batch_end(&ra));
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_same_records("pending canonical rewrite matches forced-preserve chunk bytes");
}

static void check_pending_noncanonical_overrides_published(struct server_chunk* sc) {
	remove(region_path(0));
	string_t world;
	open_world(world, 0);
	struct region_archive ra;
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct chunk_tile_entities te = complete_te();
	struct chunk_entities entities = {0};
	fill_chunk(sc, 920);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	assert(region_archive_sync(&ra));
	struct te_sign sign = {0};
	sign.x = 3; sign.y = 64; sign.z = 5;
	memcpy(sign.text, "pending", 7);
	te.signs = &sign;
	te.n_sign = 1;
	region_archive_batch_begin(&ra);
	fill_chunk(sc, 921);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	uint32_t before[24], after[24];
	region_archive_journal_stats(before);
	fill_chunk(sc, 922);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	region_archive_journal_stats(after);
	assert(after[15] == before[15] + 1 && after[16] == before[16]
		   && "latest pending noncanonical record overrides published canonical flag");
	assert(region_archive_batch_end(&ra));
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
}

static void check_pending_foreign_record_preserved(struct server_chunk* sc) {
	for(int which = 0; which < 1; which++) {
		remove(region_path(which));
		string_t world;
		open_world(world, which);
		struct region_archive ra;
		assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
		struct chunk_tile_entities te = complete_te();
		struct chunk_entities entities = {0};
		struct buffer canonical = BUFFER_INIT;
		fill_chunk(sc, 940);
		assert(region_archive_set_chunk_retain(&ra, 0, 0, sc, &te, &entities,
				&canonical) && canonical.data);
		assert(region_archive_sync(&ra));
		nbt_node* parsed = nbt_parse_compressed(canonical.data, canonical.len);
		assert(parsed);
		struct buffer plain = nbt_dump_binary(parsed);
		assert(plain.data && plain.len > 1 && plain.data[plain.len - 1] == 0);
		/* Insert a fixed unknown root TAG_Int before the root compound's END. */
		const char marker[] = "PendingForeign";
		const uint8_t prefix[] = {TAG_INT, 0, sizeof(marker) - 1};
		const uint8_t value[] = {0, 0, 0, 42};
		struct buffer altered = BUFFER_INIT;
		assert(!buffer_append(&altered, plain.data, plain.len - 1)
				&& !buffer_append(&altered, prefix, sizeof(prefix))
				&& !buffer_append(&altered, marker, sizeof(marker) - 1)
				&& !buffer_append(&altered, value, sizeof(value))
				&& !buffer_append(&altered, plain.data + plain.len - 1, 1));
		nbt_node* foreign_tree = nbt_parse(altered.data, altered.len);
		assert(foreign_tree);
		struct buffer foreign = nbt_dump_compressed(foreign_tree, STRAT_INFLATE);
		assert(foreign.data);
		region_archive_batch_begin(&ra);
		assert(region_archive_recommit_record(&ra, 0, 0, &foreign));
		uint32_t before[24], after[24];
		region_archive_journal_stats(before);
		fill_chunk(sc, 941);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
		region_archive_journal_stats(after);
		assert(after[15] == before[15] + 1 && after[16] == before[16]
				&& "pending foreign record forces readback even if published flag is canonical");
		assert(region_archive_batch_end(&ra));
		assert(region_archive_sync(&ra));
		assert(region_archive_destroy(&ra));
		string_clear(world);
		buffer_free(&foreign); nbt_free(foreign_tree); buffer_free(&altered);
		buffer_free(&plain); nbt_free(parsed); buffer_free(&canonical);
	}
	for(int which = 0; which < 1; which++) {
		size_t len = 0;
		uint8_t* bytes = read_file(region_path(which), &len);
		size_t off = (((size_t)bytes[0] << 16) | ((size_t)bytes[1] << 8)
				| bytes[2]) * REGION_SECTOR_SIZE;
		assert(off + 5 <= len);
		uint32_t n = get_be32(bytes + off);
		assert(n > 1 && off + 4 + n <= len);
		nbt_node* final = nbt_parse_compressed(bytes + off + 5, n - 1);
		assert(final);
		nbt_node* kept = nbt_find_by_path(final, ".PendingForeign");
		assert(kept && kept->type == TAG_INT && kept->payload.tag_int == 42);
		nbt_free(final);
		free(bytes);
	}
}

static void check_failed_join_keeps_pending_certificate(struct server_chunk* sc) {
	remove(region_path(0));
	string_t world;
	open_world(world, 0);
	struct region_archive ra;
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct chunk_tile_entities te = complete_te();
	struct chunk_entities entities = {0};
	struct te_sign sign = {0};
	sign.x = 3; sign.y = 64; sign.z = 5;
	memcpy(sign.text, "failure", 7);
	te.signs = &sign; te.n_sign = 1;
	region_archive_batch_begin(&ra);
	fill_chunk(sc, 930);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	te.signs = NULL; te.n_sign = 0;
	fill_chunk(sc, 931);
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_REBUILD_VALIDATION);
	assert(!region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	uint32_t before[24], after[24];
	region_archive_journal_stats(before);
	fill_chunk(sc, 932);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	region_archive_journal_stats(after);
	assert(after[15] == before[15] + 1 && after[16] == before[16]
		   && "failed joined write must leave prior noncanonical certificate");
	assert(region_archive_batch_end(&ra));
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
}

/* --- 7. THE CANONICAL FLAG'S DURABILITY (round-4 fix round, item 2) -------
 * The flag lives in bit 31 of a per-chunk TIMESTAMP entry, and the write that
 * CLEARS it -- the pessimistic clear a non-canonical record forces -- is four
 * bytes inside a 512-byte device sector. Ordering alone therefore was not
 * enough: a power cut during that read-modify-write can leave the sector TORN
 * with bit 31 still set, and nothing would ever notice; the next save would
 * skip the read-back over a record that really does hold foreign NBT.
 *
 * The fix pulls the timestamp sector of every cleared slot into the SAME
 * journal record as the batch's offset sectors, so the clear has a crc'd
 * before-image. These cases manufacture each reachable on-disk state around
 * that publication from the journal record the writer ACTUALLY produced (read
 * out of the open handle's .idx, then un-retired by rewriting published_seq,
 * which lives outside the crc), reopen through the production loader, and
 * assert on the io census that the following save really does take the
 * read-back rather than trusting a flag it must not trust.
 *
 * Chunk (0,0) is local slot 0: offset sector 0, timestamp sector 8. */
#define CANON_STAMP_SECTOR \
	((uint32_t)((size_t)REGION_SIZE * REGION_SIZE * 4 / IDX_SECTOR))

static const uint8_t* canon_record_entry(const uint8_t* j, size_t jsize,
										 uint32_t sector, bool want_after) {
	assert(jsize >= IDX3_HEAD + IDX3_REC_HEAD);
	uint32_t count = get_be32(j + IDX3_HEAD + 4);
	assert(jsize >= IDX3_RECORD(count));
	for(uint32_t i = 0; i < count; i++) {
		const uint8_t* e
			= j + IDX3_HEAD + IDX3_REC_HEAD + (size_t)i * IDX_ENTRY;
		if(get_be32(e) == sector)
			return want_after ? e + 4 + IDX_SECTOR : e + 4;
	}
	return NULL;
}

/* Bit 31 of local slot `index`'s timestamp entry, as it sits on disk. */
static bool canon_flag_at(int which, size_t index) {
	size_t size = 0;
	uint8_t* file = read_file(region_path(which), &size);
	assert(size > REGION_SECTOR_SIZE + index * 4);
	bool set = (file[REGION_SECTOR_SIZE + index * 4] & 0x80) != 0;
	free(file);
	return set;
}
static bool canon_flag_on_disk(int which) {
	return canon_flag_at(which, 0);
}

/* Save chunk (0,0) once through a freshly opened handle and require that the
 * archive took the READ-BACK path -- i.e. that it did NOT trust a canonical
 * flag. The census counters are the evidence: slot 15 counts read-backs, slot
 * 16 the skips the flag bought. */
static void assert_next_save_reads_back_at(struct server_chunk* sc, w_coord_t x,
										   w_coord_t z, unsigned seed,
										   const char* why) {
	string_t world;
	open_world(world, 1);
	struct region_archive ra;
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD) && why);
	struct chunk_tile_entities te = complete_te();
	struct chunk_entities entities = {0};
	uint32_t rj0[24], rj1[24];
	fill_chunk(sc, seed);
	region_archive_journal_stats(rj0);
	assert(region_archive_set_chunk(&ra, x, z, sc, &te, &entities) && why);
	region_archive_journal_stats(rj1);
	assert(rj1[15] == rj0[15] + 1 && rj1[16] == rj0[16] && why);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
}
static void assert_next_save_reads_back(struct server_chunk* sc, unsigned seed,
										const char* why) {
	assert_next_save_reads_back_at(sc, 0, 0, seed, why);
}

/* Reopen and read chunk (0,0)'s fingerprint back through the loader, which is
 * what actually proves which of the two records recovery resolved to. */
static int canon_reopen_fingerprint(void) {
	string_t world;
	open_world(world, 1);
	struct region_archive ra;
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	int fp = loaded_fingerprint(&ra, 0, 0);
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	return fp;
}

static void check_canonical_flag_durability(struct server_chunk* sc) {
	const int old_fp = 1 + (500 & 0x1F);
	const int new_fp = 1 + (501 & 0x1F);
	assert(old_fp != new_fp);
	string_t world;
	struct region_archive ra;
	struct chunk_entities entities = {0};
	char journal[384];
	snprintf(journal, sizeof(journal), "%s",
			 test_fixture_join(region_path(1), ".idx"));

	/* (0) A canonical record for slot 0. Bit 31 must be SET on disk -- this is
	 * the state the whole vulnerability is about. */
	remove(region_path(1));
	remove(journal);
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	{
		struct chunk_tile_entities te = complete_te();
		fill_chunk(sc, 500);
		region_archive_batch_begin(&ra);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
	}
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	assert(canon_flag_on_disk(1)
		   && "a canonical record must leave bit 31 set on disk");
	size_t old_size;
	uint8_t* old_image = read_file(region_path(1), &old_size);

	/* (1) A NON-canonical save over it (a sign is foreign NBT the merge has to
	 * preserve). It must clear the flag AND -- the fix -- journal the timestamp
	 * sector that carries it, with the clear already present in BOTH images. */
	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct te_sign sign = {0};
	sign.x = 3;
	sign.y = 64;
	sign.z = 5;
	memcpy(sign.text, "tear", 4);
	{
		struct chunk_tile_entities te = complete_te();
		te.signs = &sign;
		te.n_sign = 1;
		fill_chunk(sc, 501);
		region_archive_batch_begin(&ra);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
	}
	/* The v3 journal is per HANDLE: its record is still on disk (retired by
	 * seq, not unlinked) until the handle closes, so capture it here. */
	size_t jsize;
	uint8_t* jrec = read_file(journal, &jsize);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	assert(!canon_flag_on_disk(1)
		   && "a non-canonical save must clear bit 31 on disk");
	size_t new_size;
	uint8_t* new_image = read_file(region_path(1), &new_size);
	assert(new_size >= old_size);

	assert(jsize >= IDX3_HEAD + IDX3_REC_HEAD && !memcmp(jrec, "CWIDX003", 8)
		   && !memcmp(jrec + IDX3_HEAD, "CWRC", 4));
	const uint8_t* ts_before
		= canon_record_entry(jrec, jsize, CANON_STAMP_SECTOR, false);
	const uint8_t* ts_after
		= canon_record_entry(jrec, jsize, CANON_STAMP_SECTOR, true);
	const uint8_t* off_before = canon_record_entry(jrec, jsize, 0, false);
	assert(off_before && "the offset sector is always journalled");
	assert(ts_before && ts_after
		   && "a publication that CLEARS a canonical flag must journal the "
			  "timestamp sector carrying it -- otherwise a torn write to that "
			  "sector has no before-image to be repaired from");
	assert(!(ts_before[0] & 0x80) && !(ts_after[0] & 0x80)
		   && "both images of the journalled timestamp sector must already "
			  "carry the CLEARED flag, so either crash outcome is safe");

	/* Un-retire the captured record: published_seq (and its one's complement)
	 * sit outside the crc precisely so this is a legal, writer-reachable
	 * state -- it is what a crash before the four-byte retire leaves. */
	uint8_t* jcopy = malloc(jsize);
	uint8_t* scratch = malloc(new_size);
	assert(jcopy && scratch);
	memcpy(jcopy, jrec, jsize);
	uint32_t seq = get_be32(jrec + IDX3_HEAD + 8);
	assert(seq >= 1);
	put_u32(seq - 1, jcopy + 8);
	put_u32(~(seq - 1), jcopy + 12);

	/* (A) Crash after the payload write, BEFORE the header flip: both
	 * journalled sectors still hold their before-images. Recovery is a no-op,
	 * the OLD record is live, and the flag is clear -- so the next save reads
	 * back rather than trusting anything. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, off_before, IDX_SECTOR);
	memcpy(scratch + (size_t)CANON_STAMP_SECTOR * IDX_SECTOR, ts_before,
		   IDX_SECTOR);
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == old_fp
		   && "a crash before the header flip must leave the OLD record");
	assert(!canon_flag_on_disk(1));
	assert_next_save_reads_back(sc, 502,
								"crash before the flip -> the flag is cleared "
								"and the read-back is taken");

	/* (B) Crash after the flip, BEFORE the journal retire: every journalled
	 * sector already equals its after-image, so recovery keeps them. The NEW
	 * (non-canonical) record is live and the flag is clear -- consistent. */
	memcpy(scratch, new_image, new_size);
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == new_fp
		   && "a crash after the flip must keep the NEW record");
	assert(!canon_flag_on_disk(1));
	assert_next_save_reads_back(sc, 503,
								"crash after the flip, before the retire -> "
								"the new record is not canonical and the "
								"read-back is taken");

	/* (C) THE CASE THE FIX EXISTS FOR. The offset sector committed, but the
	 * timestamp sector is TORN: its read-modify-write kept the OLD high byte,
	 * so bit 31 is set again over a record that now holds a sign. Without a
	 * journalled before-image nothing could detect this and the next save
	 * would skip the read-back, silently dropping the sign. With it, recovery
	 * sees live != after-image and restores the crc'd before-image. */
	memcpy(scratch, new_image, new_size);
	scratch[(size_t)CANON_STAMP_SECTOR * IDX_SECTOR] |= 0x80;
	assert(memcmp(scratch + (size_t)CANON_STAMP_SECTOR * IDX_SECTOR, ts_after,
				  IDX_SECTOR)
			   != 0
		   && "the manufactured tear must not match the after-image");
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == new_fp
		   && "the committed offset sector must not be rolled back");
	assert(!canon_flag_on_disk(1)
		   && "a torn timestamp sector must be repaired from the journal's "
			  "before-image, never left with bit 31 set");
	assert_next_save_reads_back(sc, 504,
								"a torn timestamp sector leaves the flag "
								"untrusted and the read-back is taken");

	/* (D) THE OLD-FORMAT REGION: bit 31 clear everywhere is what Beta, an
	 * older CaveWii build, or any external tool leaves. No format version, no
	 * migration -- just the read-back. */
	strip_canon_flags(1);
	assert(!canon_flag_on_disk(1));
	assert_next_save_reads_back(sc, 505,
								"an old-format region (bit 31 clear) must "
								"take the read-back path");

	/* (E) THE INJECTED STAMP FAILURE on the pessimistic clear. The clear is no
	 * longer forensic metadata -- it gates the next save's read-back -- so a
	 * failure there FAILS the publication (rolled back in memory and on disk,
	 * old payload still live) instead of committing under a stale flag, and
	 * the in-memory flag degrades to "unknown" so the following save in the
	 * same session reads back. (A stamp failure on the SET half stays
	 * best-effort and Beta-faithful; test_region_timestamps pins that.) */
	remove(region_path(1));
	remove(journal);
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	{
		struct chunk_tile_entities te = complete_te();
		fill_chunk(sc, 500);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	}
	assert(canon_flag_on_disk(1));
	{
		struct chunk_tile_entities te = complete_te();
		te.signs = &sign;
		te.n_sign = 1;
		fill_chunk(sc, 501);
		region_archive_test_fail_next(
			REGION_ARCHIVE_TEST_FAIL_TIMESTAMP_WRITE);
		assert(!region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities)
			   && "a failed pessimistic CLEAR must fail the publication, not "
				  "commit a record under a flag that still claims canonical");
	}
	{
		uint32_t rj0[24], rj1[24];
		struct chunk_tile_entities te = complete_te();
		fill_chunk(sc, 506);
		region_archive_journal_stats(rj0);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
		region_archive_journal_stats(rj1);
		assert(rj1[15] == rj0[15] + 1 && rj1[16] == rj0[16]
			   && "after a failed clear the in-memory flag must degrade to "
				  "unknown, i.e. to a read-back");
	}
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);

	free(jcopy);
	free(scratch);
	free(jrec);
	free(old_image);
	free(new_image);
	printf("PASS canonical flag durability: the clearing publication journals "
		   "the timestamp sector; crash before the flip, after the flip, a "
		   "TORN timestamp sector, an old-format region and an injected stamp "
		   "failure all leave the flag untrusted and the read-back taken\n");
}

/* --- 7b. THE CANONICAL SET'S DURABILITY (round-4 fix round 2) -------------
 * Fix round 1 journalled only the pessimistic CLEAR. The per-write stamp and
 * the 0 -> 1 SET stayed plain 512-byte sector writes, and a timestamp sector
 * holds 128 chunk slots -- so a tear inside one of them could leave bit 31
 * readable as SET on a NEIGHBOUR slot whose record really does hold foreign
 * NBT, and that neighbour's next save would skip its read-back and drop it.
 *
 * Now every timestamp sector the batch touches rides the SAME crc'd record as
 * its offset sectors, and a timestamp sector is all-or-nothing on replay: its
 * after-image is kept only if EVERY entry of the record was found committed,
 * because a flag is only true of whichever record the offset entry names.
 *
 * Local slot 0 is chunk (0,0) and local slot 1 is chunk (1,0): different
 * offset entries, the SAME 512-byte timestamp sector. Slot 1 is the neighbour
 * this case exists for. */
static void check_canonical_set_durability(struct server_chunk* sc) {
	/* A flag only ever goes 0 -> 1 on a write that did NOT read a record back:
	 * a save over an existing record merges `original` in and is therefore not
	 * canonical (check_canonical_read_counters pins that). So the SET under
	 * test is slot 0's FIRST write, and the old state is "absent", which
	 * loaded_fingerprint reports as -1. */
	const int absent_fp = -1;
	const int new_fp = 1 + (601 & 0x1F);
	string_t world;
	struct region_archive ra;
	struct chunk_entities entities = {0};
	char journal[384];
	snprintf(journal, sizeof(journal), "%s",
			 test_fixture_join(region_path(1), ".idx"));
	/* The sign has to sit inside the chunk that carries it: world block x
	 * 16..31 for chunk (1,0). */
	struct te_sign sign = {0};
	sign.x = 16 + 3;
	sign.y = 64;
	sign.z = 5;
	memcpy(sign.text, "keep", 4);

	/* (0) The NEIGHBOUR, local slot 1, holds a sign the merge has to preserve,
	 * so its flag is clear and must stay clear. Slot 0 is left empty, so the
	 * publication below is a genuine 0 -> 1 SET. */
	remove(region_path(1));
	remove(journal);
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	{
		/* Slot 0 stays UNWRITTEN. */
		struct chunk_tile_entities te = complete_te();
		te.signs = &sign;
		te.n_sign = 1;
		fill_chunk(sc, 700);
		assert(region_archive_set_chunk(&ra, 1, 0, sc, &te, &entities));
	}
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert(!canon_flag_at(1, 0) && !canon_flag_at(1, 1)
		   && "a record carrying foreign NBT must leave bit 31 clear");
	size_t old_size;
	uint8_t* old_image = read_file(region_path(1), &old_size);

	/* (1) THE SET: a canonical record for slot 0, published in a batch. The
	 * timestamp sector must be in the record with the flag CLEAR in its
	 * before-image and SET in its after-image. */
	open_world(world, 1);
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	{
		struct chunk_tile_entities te = complete_te();
		fill_chunk(sc, 601);
		region_archive_batch_begin(&ra);
		assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
	}
	size_t jsize;
	uint8_t* jrec = read_file(journal, &jsize);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert(canon_flag_at(1, 0) && "a canonical record must set bit 31");
	assert(!canon_flag_at(1, 1)
		   && "a publication must not touch a neighbour slot's flag");
	size_t new_size;
	uint8_t* new_image = read_file(region_path(1), &new_size);
	assert(new_size >= old_size);

	assert(jsize >= IDX3_HEAD + IDX3_REC_HEAD && !memcmp(jrec, "CWIDX003", 8)
		   && !memcmp(jrec + IDX3_HEAD, "CWRC", 4));
	const uint8_t* off_before = canon_record_entry(jrec, jsize, 0, false);
	const uint8_t* off_after = canon_record_entry(jrec, jsize, 0, true);
	const uint8_t* ts_before
		= canon_record_entry(jrec, jsize, CANON_STAMP_SECTOR, false);
	const uint8_t* ts_after
		= canon_record_entry(jrec, jsize, CANON_STAMP_SECTOR, true);
	assert(off_before && off_after);
	assert(ts_before && ts_after
		   && "a publication that SETS a canonical flag -- and any that merely "
			  "restamps -- must journal the timestamp sector it writes");
	assert(!(ts_before[0] & 0x80) && (ts_after[0] & 0x80)
		   && "the record must carry the flag's cleared BEFORE-image and its "
			  "set AFTER-image, so either crash outcome is decidable");

	uint8_t* jcopy = malloc(jsize);
	uint8_t* scratch = malloc(new_size);
	assert(jcopy && scratch);
	memcpy(jcopy, jrec, jsize);
	uint32_t seq = get_be32(jrec + IDX3_HEAD + 8);
	assert(seq >= 1);
	put_u32(seq - 1, jcopy + 8);
	put_u32(~(seq - 1), jcopy + 12);

	/* (A) Crash after the journal record is durable, before EITHER sector
	 * flips. The journal is an UNDO log, so replay does not roll forward: it
	 * restores both before-images, i.e. the COMPLETE old state -- old offset
	 * entry, old (clear) flag. Never a set flag over a record it does not
	 * describe. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, off_before, IDX_SECTOR);
	memcpy(scratch + (size_t)CANON_STAMP_SECTOR * IDX_SECTOR, ts_before,
		   IDX_SECTOR);
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == absent_fp
		   && "a crash before either flip must leave the OLD state -- here, no "
			  "record for slot 0 at all");
	assert(!canon_flag_at(1, 0) && !canon_flag_at(1, 1));
	assert_next_save_reads_back_at(sc, 1, 0, 602,
								   "crash before either flip -> the complete "
								   "old state, every flag in the sector clear, "
								   "the neighbour's read-back taken");

	/* (B) Crash after the OFFSET flip, before the stamp flip. The offset
	 * sector is self-contained and stays committed; the timestamp sector is
	 * not, so it goes back to its before-image and the new record is live
	 * WITHOUT the flag. The flag can never lead the record it describes -- the
	 * cost is one read-back. */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch + (size_t)CANON_STAMP_SECTOR * IDX_SECTOR, ts_before,
		   IDX_SECTOR);
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == new_fp
		   && "a committed offset sector must not be rolled back");
	assert(!canon_flag_at(1, 0)
		   && "an unflipped timestamp sector must not be rolled FORWARD");
	assert(!canon_flag_at(1, 1));
	assert_next_save_reads_back(sc, 603,
								"crash after the offset flip, before the stamp "
								"flip -> the flag is clear and the read-back "
								"is taken");

	/* (C) THE NEIGHBOUR TEAR -- the case fix round 2 exists for. The stamp
	 * sector's write tore and left bit 31 set on slot 1, whose record holds a
	 * sign. It matches neither image, so the record is not fully committed and
	 * every timestamp sector in it is restored from its crc'd before-image. */
	memcpy(scratch, new_image, new_size);
	scratch[(size_t)CANON_STAMP_SECTOR * IDX_SECTOR + 4] |= 0x80;
	assert(memcmp(scratch + (size_t)CANON_STAMP_SECTOR * IDX_SECTOR, ts_after,
				  IDX_SECTOR)
			   != 0
		   && memcmp(scratch + (size_t)CANON_STAMP_SECTOR * IDX_SECTOR,
					 ts_before, IDX_SECTOR)
			   != 0
		   && "the manufactured tear must match neither image");
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == new_fp
		   && "the committed offset sector must survive a torn stamp sector");
	assert(!canon_flag_at(1, 1)
		   && "a tear that sets a NEIGHBOUR slot's bit 31 must be repaired "
			  "from the journal's before-image -- otherwise that neighbour's "
			  "next save skips the read-back over the sign it still holds");
	assert(!canon_flag_at(1, 0));
	assert_next_save_reads_back_at(sc, 1, 0, 604,
								   "a torn timestamp sector leaves the "
								   "NEIGHBOUR's flag untrusted, so its next "
								   "save reads its record back");

	/* (D) THE MIX THE ALL-OR-NOTHING RULE EXISTS FOR: the stamp sector reached
	 * the card and the offset sector did not. Nothing in the sector-flip phase
	 * forbids that order -- it sits inside ONE fsync -- and a per-sector replay
	 * would keep the set flag over an offset entry that still names the old
	 * state. Because a timestamp sector is all-or-nothing, replay restores it:
	 * the flag never outruns the record. (With today's writer a SET can only
	 * follow a first write, so this particular mix is not yet a data-loss path;
	 * the rule is what makes the invariant structural rather than contingent on
	 * that remaining true.) */
	memcpy(scratch, new_image, new_size);
	memcpy(scratch, off_before, IDX_SECTOR);
	write_file(region_path(1), scratch, new_size);
	write_file(journal, jcopy, jsize);
	assert(canon_reopen_fingerprint() == absent_fp
		   && "an unflipped offset sector must still be rolled back");
	assert(!canon_flag_at(1, 0)
		   && "a timestamp sector must never be kept over an offset sector "
			  "that did not commit -- the flag would then claim a record the "
			  "index does not name");
	assert(!canon_flag_at(1, 1));

	free(jcopy);
	free(scratch);
	free(jrec);
	free(old_image);
	free(new_image);
	printf("PASS canonical SET durability: the SET and the per-write stamp are "
		   "journalled with the offset sectors; crash before either flip, "
		   "crash between them in either order, and a tear that sets a "
		   "NEIGHBOUR slot's bit 31 all resolve to a flag no stronger than "
		   "the record it describes\n");
}

/* --- 7c. THE FSYNC CENSUS (round-4 fix round 2) ---------------------------
 * Folding the timestamp sectors into the record must not buy their durability
 * with another barrier. A publication is still payload / journal / sectors --
 * three region_durable calls -- however many sectors of either table it now
 * touches. Census slot 0 counts publications, slot 2 the header sectors
 * written and slot 4 the fsyncs. The handle is warmed up first so the
 * once-per-writing-handle journal open (slot 13, its own fsync) is outside the
 * measured window. */
static void check_publication_fsync_census(struct server_chunk* sc) {
	string_t world;
	struct region_archive ra;
	struct chunk_entities entities = {0};
	struct chunk_tile_entities te = complete_te();
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 800);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));

	uint32_t rj0[24], rj1[24];
	region_archive_journal_stats(rj0);
	region_archive_batch_begin(&ra);
	for(int k = 0; k < 4; k++) {
		fill_chunk(sc, 801u + (unsigned)k);
		assert(region_archive_set_chunk(&ra, k, 0, sc, &te, &entities));
	}
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	region_archive_journal_stats(rj1);
	assert(rj1[0] == rj0[0] + 1 && "the batch must publish exactly once");
	assert(rj1[3] == rj0[3] + 1 && "one journal record for the publication");
	assert(rj1[2] == rj0[2] + 2
		   && "four chunks in one 512-byte offset sector, and the one "
			  "timestamp sector that carries their stamps");
	assert(rj1[4] == rj0[4] + 3
		   && "three fsyncs per publication -- payload, journal, sectors -- "
			  "unchanged by the timestamp sectors joining the record");
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	printf("PASS publication fsync census: a four-chunk batch writes one "
		   "journal record over two header sectors (offset + timestamp) and "
		   "still pays exactly three fsyncs\n");
}

/* --- 8. THE SECOND-HANDLE GUARD (round-4 fix round, item 5) ---------------
 * death_drop_recovery.c opens its own region handle on a region the world LRU
 * may already hold. Its writes are never canonical, so they clear bits on disk
 * that the cached handle's in-memory bitmap would go on trusting -- and the
 * cached handle would then skip the read-back over the very entities the
 * recovery just restored. Any overlapping open on the same path disarms the
 * fast path for every handle involved, for the rest of its life. */
static void check_canonical_second_handle(struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	struct chunk_tile_entities te = complete_te();
	struct chunk_entities entities = {0};
	uint32_t rj0[24], rj1[24];

	/* A canonical record on disk, written and closed. */
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 600);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	assert_no_journal(1);

	/* CONTROL: a handle that is alone on the file honours the flag it loaded
	 * from the timestamp table at open. */
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 601);
	region_archive_journal_stats(rj0);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	region_archive_journal_stats(rj1);
	assert(rj1[16] == rj0[16] + 1 && rj1[15] == rj0[15]
		   && "a lone handle must take the canonical skip");
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	assert_no_journal(1);

	/* THE CASE: a second handle is opened on the SAME region file while the
	 * first is live -- death_drop_recovery.c's exact shape. The overlap alone
	 * disarms the cached bitmap; the very next save reads back instead of
	 * trusting a bit the other handle may already have cleared on disk. */
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	{
		string_t w2;
		open_world(w2, 1);
		struct region_archive second;
		assert(region_archive_create(&second, w2, 0, 0, WORLD_DIM_OVERWORLD));
		assert(region_archive_destroy(&second));
		string_clear(w2);
	}
	fill_chunk(sc, 602);
	region_archive_journal_stats(rj0);
	assert(region_archive_set_chunk(&ra, 0, 0, sc, &te, &entities));
	region_archive_journal_stats(rj1);
	assert(rj1[15] == rj0[15] + 1 && rj1[16] == rj0[16]
		   && "a handle that has shared its region file with another handle "
			  "must stop trusting its cached canonical bitmap");
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	printf("PASS canonical second-handle guard: a second open on the same "
		   "region file disarms the cached canonical bitmap, so a recovery "
		   "handle's non-canonical write can never be skipped over\n");
}

/* --- 5. ROUND 5: the publication runs on the region I/O helper thread -------
 * region_archive_batch_end no longer performs the publication, it REQUESTS
 * one. Three properties have to hold, and every OTHER check in this file
 * already runs against the asynchronous path by default, so the batched arm of
 * every equivalence and crash-point case above is itself async coverage. */

/* (a) BYTE EQUIVALENCE. The same scripted batch, published synchronously and
 * published on the helper, must produce identical bytes -- the publication
 * code is moved, not changed. */
static void check_async_publication_is_byte_identical(struct server_chunk* sc) {
	uint32_t rj0[24], rj1[24], rj2[24];
	const int append[] = {0, 1, 2, 3, 200, 201, 400, 900};
	region_archive_io_async_enable(false);
	region_archive_journal_stats(rj0);
	run_sequence(0, true, append, sizeof(append) / sizeof(*append), sc);
	region_archive_journal_stats(rj1);
	region_archive_io_async_enable(true);
	run_sequence(1, true, append, sizeof(append) / sizeof(*append), sc);
	region_archive_journal_stats(rj2);
	assert_same_bytes_modulo_timestamps(
		"an asynchronously published batch must be byte-identical to the "
		"synchronously published one");
	assert(rj2[21] > rj1[21]
		   && "the publication must have run on the region I/O helper thread");
	/* Publications, chunk deltas, header sectors, journal records, fsyncs and
	 * journal-handle opens: the two arms must cost exactly the same. */
	static const int census[] = {0, 1, 2, 3, 4, 13};
	for(size_t k = 0; k < sizeof(census) / sizeof(*census); k++) {
		int slot = census[k];
		assert(rj2[slot] - rj1[slot] == rj1[slot] - rj0[slot]
			   && "moving the publication to the helper thread must not "
				  "change one byte of I/O it performs");
	}
	assert_no_journal(0);
	assert_no_journal(1);

	/* Rewrite-heavy: space is freed inside the batch, so only the RECORDS are
	 * required to match (the same weaker claim the batched-vs-per-write case
	 * above makes, for the same allocator reason). */
	const int rewrite[] = {0, 1, 0, 2, 1, 0, 3, 2, 33, 64, 65, 33};
	region_archive_io_async_enable(false);
	run_sequence(0, true, rewrite, sizeof(rewrite) / sizeof(*rewrite), sc);
	region_archive_io_async_enable(true);
	run_sequence(1, true, rewrite, sizeof(rewrite) / sizeof(*rewrite), sc);
	assert_same_records(
		"a rewrite-heavy batch must store identical records whichever thread "
		"published it");
	assert_no_journal(0);
	assert_no_journal(1);
	printf("PASS async publication equivalence: the helper thread writes the "
		   "same region bytes, the same journal record and the same three "
		   "fsyncs the server thread did\n");
}

/* (b) THE FSYNCS LEAVE THE SERVER THREAD. Stall every fsync by 50 ms and read
 * the fsync census the instant batch_end returns: synchronously it would
 * already have grown by three. This is a stronger instrument than a wall clock
 * -- it names WHICH THREAD paid, not just how long something took. */
static void check_async_publication_leaves_the_server_thread(
	struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));

	uint32_t rj0[24], rj1[24], rj2[24];
	region_archive_io_async_enable(true);
	region_archive_test_fsync_delay(50);
	region_archive_journal_stats(rj0);
	region_archive_batch_begin(&ra);
	for(int k = 0; k < 4; k++) {
		fill_chunk(sc, 900u + (unsigned)k);
		assert(region_archive_set_blocks(&ra, k, 0, sc, NULL));
	}
	assert(region_archive_batch_end(&ra));
	region_archive_journal_stats(rj1);
	assert(rj1[4] == rj0[4]
		   && "region_archive_batch_end must not have paid a single fsync: "
			  "the payload barrier, the journal fsync and the sector fsync "
			  "all belong to the helper thread now");
	region_archive_test_publish_wait(&ra);
	region_archive_journal_stats(rj2);
	/* Three publication fsyncs, plus the one region_journal_open pays the
	 * first time a handle writes (journal-handle opens are slot 13). Both are
	 * the helper thread now. */
	assert(rj2[4] == rj0[4] + 3 + (rj2[13] - rj0[13])
		   && "the three fsyncs must appear once the helper has run");
	assert(rj2[21] == rj0[21] + 1
		   && "exactly one publication must have run on the helper");
	assert(rj2[22] > rj1[22]
		   && "the barrier must have recorded the wait it performed");
	region_archive_test_fsync_delay(0);
	assert(region_archive_sync(&ra));
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	printf("PASS async publication offload: with every fsync stalled 50 ms, "
		   "region_archive_batch_end returns having issued none of them; all "
		   "three appear on the helper thread\n");
}

/* H1 (round-5 fix round). A WAIT THAT IS NOT NEEDED FOR THE ANSWER IS A BUG.
 *
 * server_world_chunk_region finds the handle that owns a chunk by probing
 * EVERY resident handle with region_archive_contains, so all but one of those
 * probes is a region-coordinate mismatch. region_archive_contains used to wait
 * for the handle to leave the helper BEFORE testing the coordinates, which
 * meant a chunk load for region A stood in region B-s publication -- the exact
 * cross-region stall this round removes, and invisible on PC where an fsync is
 * nearly free.
 *
 * With B-s publication stalled (500 ms of stubbed fsync delay), a mismatching
 * contains() on B and a real chunk load from A must both return with the
 * server-side wait counter (journal-stats slot 22) UNCHANGED. */
static void check_contains_does_not_wait_for_another_region(
	struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	open_world(world, 1);
	struct region_archive a, b;
	assert(region_archive_create_new(&a, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_create_new(&b, world, 1, 0, WORLD_DIM_OVERWORLD));

	/* A gets a durable chunk to load back (no batch open: published inline). */
	fill_chunk(sc, 771);
	assert(region_archive_set_blocks(&a, 3, 4, sc, NULL));

	region_archive_io_async_enable(true);
	region_archive_test_fsync_delay(500);
	/* B publishes on the helper and will sit in the stubbed fsync for ~1.5 s
	 * (three fsyncs x 500 ms), which is far longer than the probes below. */
	region_archive_batch_begin(&b);
	fill_chunk(sc, 772);
	assert(region_archive_set_blocks(&b, REGION_SIZE + 1, 0, sc, NULL));
	assert(region_archive_batch_end(&b));

	uint32_t rj0[24], rj1[24];
	region_archive_journal_stats(rj0);
	bool exists = true;
	/* (1) the residency probe that MISSES: B does not own region A-s chunk. */
	assert(!region_archive_contains(&b, 3, 4, &exists));
	assert(!exists);
	/* (2) the residency probe that HITS its own handle, and the load itself. */
	assert(region_archive_contains(&a, 3, 4, &exists) && exists);
	struct server_chunk out;
	alloc_chunk(&out);
	assert(region_archive_get_blocks(&a, 3, 4, &out, NULL));
	free_chunk(&out);
	region_archive_journal_stats(rj1);
	assert(rj1[22] == rj0[22]
		   && "a chunk load for region A must not wait on region B-s "
			  "publication: no server-side wait may be recorded");

	region_archive_test_fsync_delay(0);
	region_archive_test_publish_wait(&b);
	assert(region_archive_sync(&b));
	assert(region_archive_destroy(&b));
	assert(region_archive_destroy(&a));
	string_clear(world);
	printf("PASS cross-region probe: with region B stalled 500 ms per fsync "
		   "inside its publication, contains(B, coords-of-A) and a full chunk "
		   "load from A both return without a single server-side wait\n");
}

/* (c) FAILURE SURFACES AT THE NEXT BARRIER, and the chunk is left in its
 * COMPLETE OLD state -- exactly what an inline publication failure did. */
static void check_async_publication_failure_surfaces(struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 950);
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));
	assert(region_archive_sync(&ra));
	int old_fp = loaded_fingerprint(&ra, 0, 0);
	assert(old_fp >= 0);

	region_archive_io_async_enable(true);
	region_archive_batch_begin(&ra);
	fill_chunk(sc, 951);
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	/* The request itself succeeds -- the failure happens on the helper. */
	assert(region_archive_batch_end(&ra));
	assert(!region_archive_sync(&ra)
		   && "a helper-side publication failure must be reported by the next "
			  "durability barrier, not swallowed");
	/* One-shot: the latch is consumed, so the following barrier is clean. */
	assert(region_archive_sync(&ra));
	assert(loaded_fingerprint(&ra, 0, 0) == old_fp
		   && "a failed publication must leave the COMPLETE OLD record");
	assert(region_archive_destroy(&ra));
	string_clear(world);
	printf("PASS async publication failure: the helper latches it, the next "
		   "barrier reports it once, and the chunk keeps its complete old "
		   "record\n");
}

/* (d) S1 -- THE ABA: A LATER JOB MUST NOT ANSWER FOR AN EARLIER ONE.
 * (Acceptance #3 of docs/p2_storage_ruling_fable_2026-09-09.md.)
 *
 * The handle used to classify a write by comparing the caller's recorded
 * PUBLICATION EPOCH against two cumulative high-water marks. A failed
 * publication recorded `failed_epoch = E` but left `save_epoch` ON E, so the
 * NEXT batch -- an unrelated set of deltas -- was handed E as well. When that
 * one succeeded it wrote `durable_epoch = E`, and region_archive_save_state
 * (which tests durable BEFORE failed) then answered DURABLE for the earlier,
 * FAILED job. Its only consumer, server_world_reconcile_pending_saves, clears
 * `save_pending` on a DURABLE answer and never re-dirties, so the newest state
 * of a resident chunk was dropped with nothing reporting a failure.
 *
 * Fail job X, publish an unrelated job Y that succeeds, and force Y's
 * completion before either is consumed: X must still read FAILED and Y must
 * read DURABLE. Both chunks are read back as well -- X's record on disk is the
 * COMPLETE OLD one (its publication rolled back), Y's is the new one -- so the
 * test pins the actual disk truth the two answers claim. */
static void check_failed_job_is_not_aliased_by_a_later_one(
	struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));

	/* Durable starting point for BOTH chunks, so "X kept its old record" is a
	 * statement about a record that exists rather than about absence. */
	fill_chunk(sc, 960);
	assert(region_archive_set_blocks(&ra, 1, 1, sc, NULL)); /* X */
	fill_chunk(sc, 961);
	assert(region_archive_set_blocks(&ra, 2, 2, sc, NULL)); /* Y */
	assert(region_archive_sync(&ra));
	int x_old = loaded_fingerprint(&ra, 1, 1);
	int y_old = loaded_fingerprint(&ra, 2, 2);
	assert(x_old >= 0 && y_old >= 0 && x_old != y_old);

	region_archive_io_async_enable(true);

	/* --- JOB X: fails on the helper. --------------------------------- */
	region_archive_batch_begin(&ra);
	uint64_t job_x = region_archive_save_job(&ra);
	fill_chunk(sc, 962);
	assert(region_archive_set_blocks(&ra, 1, 1, sc, NULL));
	assert(region_archive_save_state(&ra, job_x) == REGION_SAVE_PENDING
		   && "a write that has joined an unpublished job is PENDING");
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	assert(region_archive_batch_end(&ra)); /* the request itself succeeds */
	region_archive_test_publish_wait(&ra);
	assert(region_archive_save_state(&ra, job_x) == REGION_SAVE_FAILED
		   && "the job whose publication failed must report FAILED");

	/* --- JOB Y: an unrelated chunk, and it succeeds. ------------------ */
	region_archive_batch_begin(&ra);
	uint64_t job_y = region_archive_save_job(&ra);
	assert(job_y != job_x
		   && "a new job must never reuse the token of a completed one -- that "
			  "reuse IS the aliasing this test exists for");
	fill_chunk(sc, 963);
	assert(region_archive_set_blocks(&ra, 2, 2, sc, NULL));
	/* batch_end reports X's latched failure here (that is the seam that
	 * surfaces it) and publishes Y in the same call. */
	assert(!region_archive_batch_end(&ra)
		   && "the barrier must still report the earlier failure once");
	region_archive_test_publish_wait(&ra);

	/* --- THE ASSERTION S1 EXISTS FOR. -------------------------------- */
	assert(region_archive_save_state(&ra, job_y) == REGION_SAVE_DURABLE
		   && "the job that succeeded must report DURABLE");
	assert(region_archive_save_state(&ra, job_x) == REGION_SAVE_FAILED
		   && "a LATER job's success must not make an EARLIER failed job read "
			  "durable: the failed job's chunk is still un-named on disk and "
			  "its only consumer would clear the dirty bit and lose it");

	/* And the disk agrees with both answers. */
	assert(loaded_fingerprint(&ra, 1, 1) == x_old
		   && "X's failed publication must have left its COMPLETE OLD record");
	assert(loaded_fingerprint(&ra, 2, 2) != y_old
		   && "Y's publication landed, so Y reads back as its new record");
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_no_journal(1);
	printf("PASS per-job completion: a failed job stays FAILED after an "
		   "unrelated later job publishes successfully, and each chunk reads "
		   "back the record its own job's answer claims\n");
}

/* (e) S1 EQUIVALENCE (acceptance #10). With NO failure injected the
 * classification layer must be behaviour-identical to the epoch it replaced:
 * every job a caller records resolves DURABLE, none ever resolves FAILED (a
 * false FAILED is not a corruption but it IS a behaviour change -- it re-
 * dirties a chunk and buys an extra write), a write that adds no delta stays
 * PENDING for the caller's chunk_pending probe to resolve, and the bytes are
 * the same ones the synchronous arm wrote. Twelve batches, so the completion
 * ring wraps several times over and the fold that reclaims its slots is
 * exercised rather than assumed. */
static void check_job_records_are_equivalent_without_failures(
	struct server_chunk* sc) {
	const int seq[] = {0, 1, 2, 3, 200, 201, 400, 900};
	const size_t count = sizeof(seq) / sizeof(*seq);
	/* Arm 0: the synchronous, per-write path -- the reference bytes. */
	region_archive_io_async_enable(false);
	run_sequence(0, false, seq, count, sc);

	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	region_archive_io_async_enable(true);

	uint64_t jobs[sizeof(seq) / sizeof(*seq)];
	for(int batch = 0; batch < 12; batch++) {
		region_archive_batch_begin(&ra);
		for(size_t k = 0; k < count; k++) {
			jobs[k] = region_archive_save_job(&ra);
			fill_chunk(sc, (unsigned)(100 + k));
			assert(region_archive_set_blocks(&ra, seq[k] % REGION_SIZE,
											 seq[k] / REGION_SIZE, sc, NULL));
			/* Batches 1..11 rewrite the identical content, so every write
			 * after the first short-circuits (round 4) and joins no delta:
			 * its sampled token was never minted and must read PENDING, which
			 * is what routes the caller to region_archive_chunk_pending. */
			enum region_save_state now
				= region_archive_save_state(&ra, jobs[k]);
			assert(now == REGION_SAVE_PENDING
				   && "a token sampled before a write is PENDING until that "
					  "write's job completes");
			if(batch > 0)
				assert(!region_archive_chunk_pending(
						   &ra, seq[k] % REGION_SIZE, seq[k] / REGION_SIZE)
					   && "an identical rewrite contributes no delta");
		}
		assert(region_archive_batch_end(&ra));
		region_archive_test_publish_wait(&ra);
		for(size_t k = 0; k < count; k++) {
			enum region_save_state after
				= region_archive_save_state(&ra, jobs[k]);
			assert(after != REGION_SAVE_FAILED
				   && "no failure was injected: nothing may report FAILED");
			/* Batch 0 really published; the rewrite batches minted no token at
			 * all, so theirs stays PENDING and the chunk_pending probe above
			 * is what settles them -- byte-for-byte the behaviour the epoch
			 * had. */
			assert(after == (batch == 0 ? REGION_SAVE_DURABLE
										: REGION_SAVE_PENDING));
		}
	}
	assert(region_archive_destroy(&ra));
	string_clear(world);
	assert_same_records("per-job completion records must not change one byte "
						"of what a batch writes");
	assert_no_journal(0);
	assert_no_journal(1);
	printf("PASS per-job completion equivalence: twelve batches with no "
		   "injected failure resolve every recorded job DURABLE (or PENDING "
		   "for a write that added no delta), never FAILED, and store the "
		   "records the synchronous path stored\n");
}

/* Astra consult 2026-09-10, contract §2: a FAILED job must stay FAILED even
 * after MORE than REGION_JOB_RECORDS later jobs publish successfully. Before
 * the fix the full ring dropped the FAILED head and the next DURABLE fold
 * raised job_durable_below past its id -> save_state answered DURABLE. */
static void check_failed_job_never_shadowed(struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 970);
	assert(region_archive_set_blocks(&ra, 3, 3, sc, NULL));
	assert(region_archive_sync(&ra));
	int x_old = loaded_fingerprint(&ra, 3, 3);
	region_archive_io_async_enable(true);

	region_archive_batch_begin(&ra);
	uint64_t job_x = region_archive_save_job(&ra);
	fill_chunk(sc, 971);
	assert(region_archive_set_blocks(&ra, 3, 3, sc, NULL));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	assert(region_archive_save_state(&ra, job_x) == REGION_SAVE_FAILED);

	/* 2 x the ring depth of unrelated successful jobs on the same handle. */
	for(int n = 0; n < 2 * REGION_JOB_RECORDS; n++) {
		region_archive_batch_begin(&ra);
		fill_chunk(sc, 972 + n);
		assert(region_archive_set_blocks(&ra, 4 + (n % 3), 4, sc, NULL));
		(void)region_archive_batch_end(&ra);
		region_archive_test_publish_wait(&ra);
		assert(region_archive_save_state(&ra, job_x) == REGION_SAVE_FAILED
			   && "a failed job must never be shadowed by later durable folds");
	}
	assert(loaded_fingerprint(&ra, 3, 3) == x_old
		   && "X still holds its complete OLD record");
	assert(region_archive_destroy(&ra));
	string_clear(world);
	printf("PASS failed job never shadowed: after %d later durable jobs the "
		   "failed job still answers FAILED\n", 2 * REGION_JOB_RECORDS);
}

/* Astra review 2026-09-10 (A): MORE than REGION_JOB_FAILED_MAX failed jobs on
 * one handle, interleaved with successful ones, with a PENDING job at the
 * end. Every failed job answers FAILED (the ones retired from the parking
 * ring through the job_base raise included); a successful job minted after
 * the newest retired failure still answers DURABLE, an older one is retired
 * with it (FAILED: the safe direction); the pending one answers PENDING.
 * The failed chunk keeps its complete OLD record throughout. */
static void check_parked_failures_overflow(struct server_chunk* sc) {
	remove(region_path(1));
	remove(test_fixture_join(region_path(1), ".idx"));
	string_t world;
	struct region_archive ra;
	open_world(world, 1);
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	fill_chunk(sc, 980);
	assert(region_archive_set_blocks(&ra, 5, 5, sc, NULL));
	assert(region_archive_sync(&ra));
	int f_old = loaded_fingerprint(&ra, 5, 5);
	region_archive_io_async_enable(true);

	enum { N = REGION_JOB_FAILED_MAX + 2 };
	uint64_t failed[N], durable[N];
	for(int n = 0; n < N; n++) {
		region_archive_batch_begin(&ra);
		failed[n] = region_archive_save_job(&ra);
		fill_chunk(sc, 981 + n);
		assert(region_archive_set_blocks(&ra, 5, 5, sc, NULL));
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE);
		(void)region_archive_batch_end(&ra);
		region_archive_test_publish_wait(&ra);
		assert(region_archive_save_state(&ra, failed[n]) == REGION_SAVE_FAILED);

		region_archive_batch_begin(&ra);
		durable[n] = region_archive_save_job(&ra);
		fill_chunk(sc, 1000 + n);
		assert(region_archive_set_blocks(&ra, 6, 6 + (n % 2), sc, NULL));
		(void)region_archive_batch_end(&ra); /* reports the latch, publishes */
		region_archive_test_publish_wait(&ra);
		assert(region_archive_save_state(&ra, durable[n]) == REGION_SAVE_DURABLE);
	}
	region_archive_batch_begin(&ra);
	uint64_t pending = region_archive_save_job(&ra);
	fill_chunk(sc, 1100);
	assert(region_archive_set_blocks(&ra, 7, 7, sc, NULL));

	/* N - REGION_JOB_FAILED_MAX failures overflowed the parking ring; each
	 * overflow retired the OLDEST parked id by raising job_base past it. */
	assert(ra.job_base == failed[N - REGION_JOB_FAILED_MAX - 1] + 1
		   && "job_base sits right past the newest RETIRED failure");
	for(int n = 0; n < N; n++) {
		assert(region_archive_save_state(&ra, failed[n]) == REGION_SAVE_FAILED
			   && "every failed job stays FAILED past the parking depth");
		/* A durable token minted after the newest retired failure keeps its
		 * exact answer; an older one is retired with it and answers FAILED
		 * (one needless re-save, never a false DURABLE). */
		enum region_save_state d = region_archive_save_state(&ra, durable[n]);
		if(durable[n] >= ra.job_base)
			assert(d == REGION_SAVE_DURABLE && "exact past the retired range");
		else
			assert(d == REGION_SAVE_FAILED && "retired: the safe direction");
	}
	assert(region_archive_save_state(&ra, pending) == REGION_SAVE_PENDING);
	assert(loaded_fingerprint(&ra, 5, 5) == f_old && "old record intact");
	assert(region_archive_batch_end(&ra));
	region_archive_test_publish_wait(&ra);
	assert(region_archive_save_state(&ra, pending) == REGION_SAVE_DURABLE);
	assert(region_archive_destroy(&ra));
	string_clear(world);
	printf("PASS parked failures overflow: %d failed + %d durable jobs "
		   "interleaved, all answers exact past the parking depth\n", N, N);
}

int main(void) {
	test_fixture_dir("test_region_journal_equiv_tmp", s_fixture_root,
					 sizeof s_fixture_root);
	make_world(0);
	make_world(1);

	struct server_chunk sc;
	alloc_chunk(&sc);

	check_append_only_is_byte_identical(&sc);
	check_rewrites_preserve_every_record(&sc);
	check_multi_sector_batch(&sc);
	check_batch_survives_overflow(&sc);
	check_in_batch_old_range_reserved(&sc);
	check_evict_inside_batch(&sc);
	check_evict_outside_batch(&sc);
	check_destroy_publishes_unclosed_batch(&sc);
	check_mixed_save_and_evict_same_chunk(&sc);
	check_crash_points(&sc);
	check_multi_sector_crash_points(&sc);
	check_journal_seq_gate(&sc);
	check_journal_record_length_tear(&sc);
	check_create_new_removes_stale_journal();
	check_canonical_records_are_identical(&sc);
	check_canonical_read_counters(&sc);
	check_pending_noncanonical_overrides_published(&sc);
	check_pending_foreign_record_preserved(&sc);
	check_pending_canonical_rewrite(&sc);
	check_failed_join_keeps_pending_certificate(&sc);
	check_canonical_flag_durability(&sc);
	check_canonical_set_durability(&sc);
	check_publication_fsync_census(&sc);
	check_canonical_second_handle(&sc);
	check_async_publication_is_byte_identical(&sc);
	check_async_publication_leaves_the_server_thread(&sc);
	check_async_publication_failure_surfaces(&sc);
	check_failed_job_never_shadowed(&sc);
	check_parked_failures_overflow(&sc);
	check_contains_does_not_wait_for_another_region(&sc);
	check_failed_job_is_not_aliased_by_a_later_one(&sc);
	check_job_records_are_equivalent_without_failures(&sc);

	free_chunk(&sc);
	printf("test_region_index_journal_equiv: batched publication is record- "
		   "and (append-only) byte-identical to per-write publication; the "
		   "eviction barrier publishes an open batch (inside/outside a batch, "
		   "and with the same chunk saved twice); every manufactured crash "
		   "point -- v1 and the round-2 multi-sector journal -- recovers to a "
		   "complete per-chunk state OK\n");
	test_fixture_dir_cleanup(s_fixture_root);
	return 0;
}
