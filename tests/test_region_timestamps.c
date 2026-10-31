/*
	McRegion per-chunk timestamp table coverage (sector 1, file bytes
	4096..8191, one big-endian u32 of epoch SECONDS per chunk slot).

	Beta 1.7.3 stamps EVERY chunk write: RegionFile.write ends with
	func_22208_b(x, z, (int)(System.currentTimeMillis() / 1000L)) -> seek
	4096 + (x + z*32)*4, writeInt [decompile 740c583 RegionFile.java
	write:232, func_22208_b:265-269], and swallows the IOException (:233-235),
	so the stamp is last (payload, offset entry, timestamp) and best-effort.
	Our writer used to leave the whole table zero, which cost the 2026-08-07
	save forensics its per-chunk dating (docs/save_forensics_2026-08-07.md).

	Covered here:
	  - a fresh region's table is all-zero; a written chunk's slot becomes a
	    real epoch stamp BRACKETED by time(NULL) taken around the write, so a
	    ticks/millis/uptime-based source is caught, not just a zero;
	  - rewrites stamp monotonically non-decreasing values;
	  - slot addressing: chunk (rx,rz) lands at byte 4096+4*(rx+rz*32) exactly,
	    neighbouring slots stay zero, and rewriting one chunk leaves every
	    other chunk's stamp untouched (including across a reopen and in the
	    negative-coordinate region r.-1.0, the rail-ring world's geometry);
	  - a FAILED (rolled-back) chunk write leaves the old stamp in place --
	    the old payload is still the live one, so the old date is the truth;
	  - the stamp is NO LONGER a separate post-commit best-effort write
	    (round-4 fix round 2, 2026-09-08): bit 31 of it carries the
	    canonical-record flag that gates the next save's read-back, so the
	    timestamp sector rides the same crc'd journal record and the same
	    barrier as the offset entry. An injected stamp failure therefore rolls
	    the whole publication back -- old payload live, old offset entry, old
	    stamp -- and does not poison the following write, whose stamp lands
	    again. The deviation from Beta's swallowed IOException is recorded in
	    docs/deviations.md ("Bit 31 of the per-chunk TIMESTAMP entry carries a
	    CANONICAL-RECORD flag").
*/

#include <assert.h>
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
#include "network/region_archive.h"
#include "network/server_world.h"
#include "platform/mem2.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_timestamps_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define REGION_PATH test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")
#define REGION_NEG_PATH test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr")
#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

static uint32_t get_be32(const uint8_t* in) {
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16)
		| ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

/* Raw on-disk read of timestamp slot `k` -- independent of the production
 * code's own view of the file, like the forensics tooling. */
static uint32_t read_stamp_at(const char* path, unsigned k) {
	uint8_t raw[4];
	FILE* f = fopen(path, "rb");
	assert(f);
	assert(fseek(f, 4096L + 4L * k, SEEK_SET) == 0);
	assert(fread(raw, sizeof(raw), 1, f) == 1);
	assert(fclose(f) == 0);
	return get_be32(raw);
}

static uint32_t read_index_at(const char* path, unsigned k) {
	uint8_t raw[4];
	FILE* f = fopen(path, "rb");
	assert(f);
	assert(fseek(f, 4L * k, SEEK_SET) == 0);
	assert(fread(raw, sizeof(raw), 1, f) == 1);
	assert(fclose(f) == 0);
	return get_be32(raw);
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

static void free_input_chunk(struct server_chunk* sc) {
	free(sc->ids);
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

static void assert_saved_byte(struct region_archive* ra, w_coord_t x,
							  w_coord_t z, uint8_t expected) {
	struct server_chunk loaded = {0};
	assert(region_archive_get_blocks(ra, x, z, &loaded, NULL));
	assert(loaded.ids[0] == expected);
	server_chunk_arrays_free(&loaded);
}

/* Write chunk (x,z) and return its freshly read on-disk stamp, asserting the
 * Beta contract: nonzero, epoch seconds (bracketed by time(NULL) around the
 * write), non-decreasing vs `prev`. */
static uint32_t write_and_check_stamp(struct region_archive* ra,
									  struct server_chunk* sc, w_coord_t x,
									  w_coord_t z, const char* path,
									  unsigned slot, uint32_t prev) {
	time_t before = time(NULL);
	assert(before != (time_t)-1 && before > 0
		   && "test host clock must be sane");
	assert(region_archive_set_blocks(ra, x, z, sc, NULL));
	time_t after = time(NULL);

	uint32_t stamp = read_stamp_at(path, slot);
	assert(stamp != 0 && "a written chunk's timestamp slot must be stamped");
	assert(stamp >= (uint32_t)before && stamp <= (uint32_t)after
		   && "stamp must be epoch SECONDS of the write moment "
			  "(Beta System.currentTimeMillis()/1000), not ticks/millis");
	assert(stamp >= prev && "chunk stamps must be monotonic non-decreasing");
	return stamp;
}

int main(void) {
	test_fixture_dir("test_region_timestamps_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(REGION_PATH);
	remove(REGION_NEG_PATH);

	string_t world;
	string_init_set_str(world, TMP_WORLD);

	struct region_archive ra;
	struct server_chunk sc;
	alloc_chunk(&sc);

	// --- fresh region: both header tables all-zero ------------------------
	assert(region_archive_create_new(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(unsigned k = 0; k < REGION_SIZE * REGION_SIZE; k += 101)
		assert(read_stamp_at(REGION_PATH, k) == 0
			   && "fresh region must have an all-zero timestamp table");

	// --- first write stamps its slot, and ONLY its slot -------------------
	sc.ids[0] = 7;
	uint32_t t1 = write_and_check_stamp(&ra, &sc, 0, 0, REGION_PATH, 0, 0);
	assert(read_stamp_at(REGION_PATH, 1) == 0
		   && read_stamp_at(REGION_PATH, 32) == 0
		   && "neighbouring slots must stay zero");

	// --- slot addressing: chunk (5,3) -> slot 5+3*32 = 101 -----------------
	sc.ids[0] = 8;
	uint32_t tb = write_and_check_stamp(&ra, &sc, 5, 3, REGION_PATH,
										5 + 3 * REGION_SIZE, 0);
	assert(read_stamp_at(REGION_PATH, 100) == 0
		   && read_stamp_at(REGION_PATH, 102) == 0
		   && "the (5,3) stamp must land at exactly 4096+4*101");
	assert(read_stamp_at(REGION_PATH, 0) == t1
		   && "writing one chunk must not touch another chunk's stamp");

	// --- rewrites: monotonic non-decreasing, other slots untouched ---------
	uint32_t prev = t1;
	for(int k = 0; k < 3; k++) {
		sc.ids[0] = (uint8_t)(10 + k);
		prev = write_and_check_stamp(&ra, &sc, 0, 0, REGION_PATH, 0, prev);
	}
	assert(read_stamp_at(REGION_PATH, 5 + 3 * REGION_SIZE) == tb
		   && "rewriting (0,0) must leave (5,3)'s stamp untouched");

	// --- a FAILED chunk write must leave the old stamp in place ------------
	// (the rollback keeps the OLD payload live, so the OLD date is the truth)
	uint32_t committed_index = read_index_at(REGION_PATH, 0);
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	sc.ids[0] = 66;
	assert(!region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
	assert(read_stamp_at(REGION_PATH, 0) == prev
		   && "a rolled-back write must NOT stamp");
	assert(read_index_at(REGION_PATH, 0) == committed_index);
	assert_saved_byte(&ra, 0, 0, 12); // last successful write above

	// --- injected STAMP failure: the whole publication rolls back ----------
	// DELIVERATE CHANGE OF BEHAVIOUR (round-4 fix round 2, 2026-09-08). The
	// per-chunk stamp used to be a separate trailing write, and this test
	// pinned Beta's swallow of its IOException (RegionFile.java:233-235). It is
	// no longer separate: bit 31 of the stamp carries the canonical flag that
	// gates the next save's read-back, so the timestamp sector now rides the
	// SAME crc'd journal record and the same sector barrier as the offset
	// entry. There is no stamp-only failure mode left to swallow -- a stamp
	// this layer cannot produce fails the publication, which rolls back to the
	// OLD payload and the OLD stamp, i.e. to a self-consistent pair, and the
	// caller retries. Recorded in docs/deviations.md.
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_TIMESTAMP_WRITE);
	sc.ids[0] = 42;
	assert(!region_archive_set_blocks(&ra, 0, 0, &sc, NULL)
		   && "a stamp that cannot be written must fail the publication, not "
			  "commit an index entry whose flag may be stale");
	assert_saved_byte(&ra, 0, 0, 12); // the rolled-back write left the old one
	assert(read_index_at(REGION_PATH, 0) == committed_index
		   && "the rolled-back publication must not flip the offset entry");
	assert(read_stamp_at(REGION_PATH, 0) == prev
		   && "the failed stamp must leave the previous stamp in place");

	// --- and must not poison the NEXT write, whose stamp lands again -------
	sc.ids[0] = 43;
	prev = write_and_check_stamp(&ra, &sc, 0, 0, REGION_PATH, 0, prev);
	assert_saved_byte(&ra, 0, 0, 43);

	// --- reopen (new session over the same file): stamps survive open ------
	assert(region_archive_destroy(&ra));
	assert(region_archive_create(&ra, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(read_stamp_at(REGION_PATH, 0) == prev
		   && "open must not clobber the timestamp table");
	sc.ids[0] = 44;
	prev = write_and_check_stamp(&ra, &sc, 0, 0, REGION_PATH, 0, prev);
	assert(region_archive_destroy(&ra));

	// --- negative-coordinate region r.-1.0 (the rail-ring geometry):
	// chunk (-1,0) -> rx = -1 & 31 = 31 -> slot 31 ---------------------------
	assert(region_archive_create_new(&ra, world, -1, 0, WORLD_DIM_OVERWORLD));
	sc.ids[0] = 45;
	uint32_t tn = write_and_check_stamp(&ra, &sc, -1, 0, REGION_NEG_PATH,
										31, 0);
	(void)tn;
	assert(read_stamp_at(REGION_NEG_PATH, 30) == 0
		   && read_stamp_at(REGION_NEG_PATH, 0) == 0
		   && "negative chunk coords must mask to the Beta slot, not alias");
	assert(region_archive_destroy(&ra));

	free_input_chunk(&sc);
	string_clear(world);
	remove(REGION_PATH);
	remove(REGION_NEG_PATH);

	printf("test_region_timestamps: OK (fresh table zero; writes stamp epoch "
		   "seconds, monotonic, slot-exact; rollback and injected stamp "
		   "failure both leave the old stamp with the chunk data committed)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
