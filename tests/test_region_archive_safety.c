/*
	Region-file allocator and two-phase commit regression coverage.

	OPEN salvages malformed/overlapping/out-of-file index entries per-chunk
	(Beta RegionFile parity -- only the torn chunk regenerates, the region
	survives; the "New World 120" torn-write hang, 2026-07-30). The WRITE-path
	validator still rejects them outright. Rewriting a chunk must use a
	different payload range, flush it before committing the index, and leave
	the old indexed chunk readable at every injected failure boundary.
*/

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "platform/mem2.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_safety_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define REGION_PATH test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")
#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (IDS_SIZE / 2)
#define HM_SIZE (CHUNK_SIZE * CHUNK_SIZE)

bool region_archive_test_sector_range_seekable(uint32_t offset,
										   uint32_t sectors);

static void check_sector_seek_bounds(void) {
	uint64_t max_bytes = (uint64_t)SIZE_MAX;
	if((uint64_t)LONG_MAX < max_bytes)
		max_bytes = (uint64_t)LONG_MAX;
	uint64_t max_end_sector = max_bytes / REGION_SECTOR_SIZE;
	assert(max_end_sector > 0);
	uint32_t last_seekable = max_end_sector - 1 < UINT32_MAX
		? (uint32_t)(max_end_sector - 1) : UINT32_MAX;
	assert(region_archive_test_sector_range_seekable(last_seekable, 1));
	if(max_end_sector <= UINT32_MAX)
		assert(!region_archive_test_sector_range_seekable(
			(uint32_t)max_end_sector, 1));
}

static void put_be32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t* in) {
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16)
		| ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static void write_raw_region(uint32_t first, uint32_t second,
							 size_t complete_sectors, size_t tail_bytes) {
	assert(complete_sectors >= 2);
	FILE* f = fopen(REGION_PATH, "wb");
	assert(f);

	uint8_t sector[REGION_SECTOR_SIZE] = {0};
	put_be32(sector, first);
	put_be32(sector + 4, second);
	assert(fwrite(sector, sizeof(sector), 1, f) == 1);
	memset(sector, 0, sizeof(sector));
	for(size_t k = 1; k < complete_sectors; k++)
		assert(fwrite(sector, sizeof(sector), 1, f) == 1);
	if(tail_bytes)
		assert(fwrite(sector, tail_bytes, 1, f) == 1);
	assert(fclose(f) == 0);
}

static size_t indexed_record_end(uint32_t entry) {
	uint32_t offset = entry >> 8;
	uint8_t raw[4];
	FILE* f = fopen(REGION_PATH, "rb");
	assert(f);
	assert(fseek(f, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	assert(fread(raw, sizeof(raw), 1, f) == 1);
	assert(fclose(f) == 0);
	size_t end = (size_t)offset * REGION_SECTOR_SIZE
		+ sizeof(uint32_t) + get_be32(raw);
	assert(end <= (size_t)(offset + (entry & 0xFF)) * REGION_SECTOR_SIZE);
	return end;
}

static void truncate_region(size_t bytes) {
	FILE* f = fopen(REGION_PATH, "rb+");
	assert(f);
#ifdef _WIN32
	assert(bytes <= LONG_MAX);
	assert(_chsize(_fileno(f), (long)bytes) == 0);
#else
	assert(ftruncate(fileno(f), (off_t)bytes) == 0);
#endif
	assert(fclose(f) == 0);
}

static bool open_region(struct region_archive* ra, string_t world) {
	return region_archive_create(ra, world, 0, 0, WORLD_DIM_OVERWORLD);
}

static uint32_t read_index_at(unsigned k) {
	uint8_t raw[4];
	FILE* f = fopen(REGION_PATH, "rb");
	assert(f);
	assert(fseek(f, 4L * k, SEEK_SET) == 0);
	assert(fread(raw, sizeof(raw), 1, f) == 1);
	assert(fclose(f) == 0);
	return get_be32(raw);
}

static uint32_t read_first_index(void) {
	return read_index_at(0);
}

static uint8_t* read_payload_allocation(uint32_t entry, size_t* size) {
	uint32_t offset = entry >> 8;
	uint32_t sectors = entry & 0xFF;
	assert(offset >= 2 && sectors > 0);
	*size = (size_t)sectors * REGION_SECTOR_SIZE;
	uint8_t* data = malloc(*size);
	assert(data);

	FILE* f = fopen(REGION_PATH, "rb");
	assert(f);
	assert(fseek(f, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	assert(fread(data, *size, 1, f) == 1);
	assert(fclose(f) == 0);
	return data;
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

static void assert_saved_byte(struct region_archive* ra, uint8_t expected) {
	struct server_chunk loaded = {0};
	assert(region_archive_get_blocks(ra, 0, 0, &loaded, NULL));
	assert(loaded.ids[0] == expected);
	server_chunk_arrays_free(&loaded);
}

static size_t region_file_size(void) {
	FILE* f = fopen(REGION_PATH, "rb");
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long end = ftell(f);
	assert(end >= 0);
	assert(fclose(f) == 0);
	return (size_t)end;
}

/* HEADLINE REGRESSION for the salvage gap-reuse fix.
 *
 * A claim that STARTS inside the file but RUNS PAST its end is salvaged in
 * memory only (its payload may be partly recoverable, so the on-disk header is
 * deliberately left alone) and its sectors are reserved so no new write lands
 * on them.
 *
 * That reservation used to be a single `preserved_end` FLOOR, which switched
 * the free-gap search OFF for the life of the handle: every subsequent write
 * appended past the floor and the sectors freed by each rewrite were never
 * reclaimed, so one torn index entry made the .mcr grow without bound for the
 * rest of the session.
 *
 * Rewriting the SAME chunk over and over must reach a steady state: the
 * previous record is freed by each write and the next write may reuse it.
 * FAIL-BEFORE: with the floor, the file grows by the record size on every
 * single iteration and this assert fires on the first comparison. */
static void check_gap_reuse_after_salvage(string_t world,
										  struct server_chunk* sc) {
	/* Six complete sectors. Chunk (0,0) is a valid one-sector record at 2;
	 * chunk (1,0) claims [4,8) -- starts inside the file, ends past it. */
	write_raw_region((2 << 8) | 1, (4 << 8) | 4, 6, 0);

	struct region_archive ra;
	assert(open_region(&ra, world));

	/* The torn entry is suppressed in memory but its header is preserved. */
	assert(read_index_at(1) == ((4 << 8) | 4));

	sc->ids[0] = 7;
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));
	size_t settled = region_file_size();

	for(int k = 0; k < 8; k++) {
		sc->ids[0] = (uint8_t)(100 + k);
		assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));
		assert_saved_byte(&ra, (uint8_t)(100 + k));
		/* The steady state may be reached one write later than the first, so
		 * allow the size to settle, but it must never keep climbing. */
		size_t now = region_file_size();
		if(k == 0)
			settled = now;
		assert(now == settled
			   && "salvaged region must REUSE freed sectors, not append forever");
	}

	/* The preserved payload was never handed to the allocator. */
	assert(read_index_at(1) == ((4 << 8) | 4));

	assert(region_archive_destroy(&ra));
	remove(REGION_PATH);
}

/* Write a header carrying `count` (slot, entry) pairs over `complete_sectors`
 * whole sectors. write_raw_region only reaches slots 0 and 1, which is not
 * enough to overflow the 16-slot reserve list. */
static void write_raw_region_slots(const unsigned* slot, const uint32_t* entry,
								   size_t count, size_t complete_sectors) {
	assert(complete_sectors >= 2);
	FILE* f = fopen(REGION_PATH, "wb");
	assert(f);

	uint8_t sector[REGION_SECTOR_SIZE] = {0};
	for(size_t k = 0; k < count; k++) {
		assert(slot[k] < REGION_SECTOR_SIZE / 4);
		put_be32(sector + 4 * slot[k], entry[k]);
	}
	assert(fwrite(sector, sizeof(sector), 1, f) == 1);
	memset(sector, 0, sizeof(sector));
	for(size_t k = 1; k < complete_sectors; k++)
		assert(fwrite(sector, sizeof(sector), 1, f) == 1);
	assert(fclose(f) == 0);
}

/* F1 REGRESSION -- the free-gap search must honour the DEGRADED FLOOR.
 *
 * A suppressed claim is normally reserved as an exact range, but the range
 * list holds only REGION_PRESERVED_MAX (16) of them. Past that, and on the
 * salvage-flush-failure path, the reservation survives ONLY as preserved_floor.
 * The append branch always honoured that floor; the free-gap search did not --
 * it consulted the exact-range array alone. So once the floor was the only
 * record of a reservation, a gap below it was handed out and the new payload
 * landed on sectors the on-disk index still names.
 *
 * FAIL-BEFORE: the write lands at sector 3, below a floor that is the sole
 * surviving record of a reservation. After the fix it lands at 253.
 *
 * Layout -- and DO NOT "simplify" it, the obvious version cannot fail:
 * chunk (0,0)@[2,3) is valid, then sixteen far-apart OVERLAPPING PAIRS
 * ([base,base+2) and [base+1,base+3) for base = 100,110,...,250), then a
 * seventeenth pair low in the file at [4,6)/[5,7). Overlap suppression drops
 * the later-starting half of each pair and reserves it, so the sixteen high
 * pairs fill the exact-range list and the seventeenth has nowhere to go but
 * preserved_floor. The surviving [4,6) leaves a one-sector gap at [3,4) --
 * below the floor, and exactly what the old gap search handed out.
 *
 * The tempting construction -- seventeen past-EOF claims -- CANNOT trigger
 * this. Past-EOF claims all straddle the same file end, so region_reserve_range
 * coalesces them into a single range that never overflows the list, and the
 * floor stays 0. Overlapping pairs sit inside the file and can be spread far
 * enough apart to stay distinct, which is the only way to get seventeen
 * reservations. */
static void check_gap_reuse_honours_degraded_floor(string_t world,
												   struct server_chunk* sc) {
	/* Reservations come from OVERLAP suppression (region_entry_salvage); see
	 * the layout note above for why pairs are the only workable construction. */
	unsigned slots[2 * REGION_PRESERVED_MAX + 3];
	uint32_t entries[2 * REGION_PRESERVED_MAX + 3];
	size_t n = 0;

	slots[n] = 0; entries[n++] = (2U << 8) | 1U;   /* valid: [2,3) */

	/* 16 far-apart overlapping pairs -> 16 reservations, filling the list. */
	for(unsigned k = 0; k < REGION_PRESERVED_MAX; k++) {
		uint32_t base = 100U + 10U * k;
		slots[n] = (unsigned)n; entries[n++] = (base << 8) | 2U;
		slots[n] = (unsigned)n; entries[n++] = ((base + 1U) << 8) | 2U;
	}

	/* The 17th pair, LOW in the file. Its suppressed half [5,7) has nowhere to
	 * go but preserved_floor -- and it sits just above the [3,4) gap that the
	 * free-gap search will be offered first. */
	slots[n] = (unsigned)n; entries[n++] = (4U << 8) | 2U;
	slots[n] = (unsigned)n; entries[n++] = (5U << 8) | 2U;

	write_raw_region_slots(slots, entries, n, 260);

	struct region_archive ra;
	assert(open_region(&ra, world));

	sc->ids[0] = 42;
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));

	/* FAIL-BEFORE: the old gap search saw only the exact-range array, so the
	 * one-sector gap at [3,4) looked free and the write landed there -- below a
	 * floor that is the sole surviving record of a reservation. */
	assert((read_index_at(0) >> 8) >= 7
		   && "gap reuse must not allocate below the degraded preserved floor");
	assert_saved_byte(&ra, 42);

	assert(region_archive_destroy(&ra));
	remove(REGION_PATH);
}

/* F1 REGRESSION, second trigger: salvage repaired the header but the flush
 * FAILED, so the repairs may never have reached the card. That path also
 * degrades to preserved_floor alone, and the gap search must honour it for the
 * same reason. Reached only through the injection hook -- the flush is a bare
 * fflush with no other way to fail deterministically. */
static void check_gap_reuse_honours_floor_after_flush_failure(
	string_t world, struct server_chunk* sc) {
	/* (0,0)@[2,3) and (1,0)@[15,16) are valid and leave a gap at [3,15). Slot 2
	 * starts entirely PAST EOF, which is "dead beyond doubt": OPEN zeroes it on
	 * disk (a repair) and folds its end into repair_end -- so the injected
	 * flush failure pins preserved_floor at 102. Only repaired entries feed
	 * repair_end, which is why this claim must start past EOF rather than
	 * straddle it. */
	unsigned slots[3] = {0, 1, 2};
	uint32_t entries[3]
		= {(2U << 8) | 1U, (15U << 8) | 1U, (100U << 8) | 2U};
	write_raw_region_slots(slots, entries, 3, 20);

	struct region_archive ra;
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_SALVAGE_FLUSH);
	assert(open_region(&ra, world));

	sc->ids[0] = 77;
	assert(region_archive_set_blocks(&ra, 0, 0, sc, NULL));

	assert((read_index_at(0) >> 8) >= 102
		   && "a failed salvage flush must switch gap reuse off below the floor");
	assert_saved_byte(&ra, 77);

	assert(region_archive_destroy(&ra));
	remove(REGION_PATH);
}

int main(void) {
	test_fixture_dir("test_region_safety_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	check_sector_seek_bounds();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t world;
	string_init_set_str(world, TMP_WORLD);

	// --- OPEN-time salvage (Beta RegionFile parity). A torn or overlapping
	// index entry marks ONLY that chunk absent; the region and every other
	// chunk stay readable. Beta: the constructor skips a past-EOF entry in
	// the sector map (RegionFile.java:70) and reads of it return null
	// ("invalid sector", :124-126), so just that chunk regenerates. The old
	// whole-region reject turned ONE torn entry -- the power-cut signature:
	// header updated, payload append never flushed -- into a bricked world
	// stuck at "generating terrain" forever ("New World 120", 2026-07-30).
	// The WRITE-path validator stays fail-closed (injected-failure block
	// below): a candidate write is never "repaired" by dropping a chunk.
	struct region_archive ra;
	struct server_chunk sc;
	alloc_chunk(&sc);
	bool exists;

	// Distinct starts are not enough: [2,4) and [3,5) overlap. The later-
	// starting claimant is dropped (absent); the earlier survives.
	write_raw_region((2U << 8) | 2U, (3U << 8) | 2U, 5, 0);
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && exists);
	assert(region_archive_contains(&ra, 1, 0, &exists) && !exists);
	// The suppressed claimant owns [3,5), including sector 4 that the surviving
	// claimant does not. A new chunk must append at/after 5, not reuse that
	// recoverable unique tail.
	sc.ids[0] = 5;
	assert(region_archive_set_blocks(&ra, 2, 0, &sc, NULL));
	assert((read_index_at(2) >> 8) >= 5);
	region_archive_destroy(&ra);
	// An OVERLAP drop is IN-MEMORY ONLY -- both claimants may be perfectly
	// readable (an overlap means the index is inconsistent, not that a payload
	// is gone), and "later starting" is an arbitrary tie-break, so persisting
	// it would permanently delete a possibly-good chunk and regenerate terrain
	// over the player's build. Observed live on the world-120 repro: the drop
	// victims were two chunks that had just been saved CORRECTLY.
	// Both disk entries must survive untouched.
	assert(read_first_index() == ((2U << 8) | 2U));
	assert(read_index_at(1) == ((3U << 8) | 2U)
		   && "an overlap drop must NEVER be written to the card");

	// An indexed allocation must fit entirely in complete file sectors; an
	// entry past EOF (THE "New World 120" corruption: offset=63 sectors=2 on
	// a 63-sector file) is treated as chunk-absent...
	write_raw_region((4U << 8) | 2U, 0, 5, 0);
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && !exists);
	// ...the chunk stays writable, and the zeroed header survives the write.
	sc.ids[0] = 7;
	assert(region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
	assert_saved_byte(&ra, 7);
	region_archive_destroy(&ra);
	assert(read_first_index() != ((4U << 8) | 2U));
	assert(read_first_index() != 0); // the rewrite landed a real entry

	// Nonzero entries with an invalid offset/sector half are corruption, not
	// empty slots: salvaged to absent, never trusted, zeroed on disk.
	write_raw_region(1, 0, 2, 0);
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && !exists);
	region_archive_destroy(&ra);
	assert(read_first_index() == 0);

	// An entry that merely RUNS OFF the end (starts inside the file) is salvaged
	// in memory but NOT zeroed on disk: its payload may be largely present, and
	// Beta pads the file to a whole sector before counting, so Beta would give
	// itself one more sector than our floor division does and would still read
	// it. Deleting it on the card could destroy a chunk Beta could have loaded.
	write_raw_region((4U << 8) | 2U, 0, 5, 0); // 4+2=6 > 5 sectors, starts at 4
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && !exists);
	// Leaving the header on disk also reserves its whole [4,6) claim. Prove a
	// later write cannot make "preserved" mean "silently overwritten".
	//
	// This asserted `offset >= 6` when the reservation was a single append
	// FLOOR. That over-specified the invariant: what must hold is that the new
	// record does not INTERSECT [4,6) -- landing below it (the free gap at
	// [2,4)) is equally safe, and refusing to use that gap is exactly the
	// unbounded-growth bug check_gap_reuse_after_salvage now covers. Assert the
	// real property instead of the old placement.
	sc.ids[0] = 8;
	assert(region_archive_set_blocks(&ra, 1, 0, &sc, NULL));
	uint32_t placed = read_index_at(1);
	uint32_t placed_off = placed >> 8;
	uint32_t placed_end = placed_off + (placed & 0xFF);
	assert((placed_end <= 4 || placed_off >= 6)
		   && "a new record must never intersect the preserved [4,6) claim");
	region_archive_destroy(&ra);
	assert(read_first_index() == ((4U << 8) | 2U)
		   && "an entry that only RUNS OFF the end must not be zeroed on disk");

	// Rewriting that suppressed slot is copy-on-write too. If the new index
	// commit fails after touching the header, rollback restores the real disk
	// claim, not the in-memory ABSENT zero.
	write_raw_region((2U << 8) | 2U, (3U << 8) | 2U, 5, 0);
	assert(open_region(&ra, world));
	sc.ids[0] = 9;
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH);
	assert(!region_archive_set_blocks(&ra, 1, 0, &sc, NULL));
	region_archive_destroy(&ra);
	assert(read_index_at(1) == ((3U << 8) | 2U));

	// ... whereas one that STARTS at or past EOF claims nothing that can exist,
	// so it is repaired (this is the case that bricked "New World 120").
	write_raw_region((9U << 8) | 1U, 0, 5, 0); // starts at 9, file is 5 sectors
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && !exists);
	region_archive_destroy(&ra);
	assert(read_first_index() == 0
		   && "an entry starting past EOF is dead beyond doubt -> repaired");

	// A file with MANY torn entries is truncated, not one torn write. Past the
	// cap we must stop persisting so a real recovery stays possible.
	{
		FILE* f = fopen(REGION_PATH, "wb");
		assert(f);
		uint8_t sector[REGION_SECTOR_SIZE] = {0};
		for(int k = 0; k < 40; k++) // all start past EOF, all "dead beyond doubt"
			put_be32(sector + 4 * k, (99U << 8) | 1U);
		assert(fwrite(sector, sizeof(sector), 1, f) == 1);
		memset(sector, 0, sizeof(sector));
		for(int k = 1; k < 5; k++)
			assert(fwrite(sector, sizeof(sector), 1, f) == 1);
		assert(fclose(f) == 0);
	}
	assert(open_region(&ra, world));
	region_archive_destroy(&ra);
	{
		int repaired = 0;
		for(unsigned k = 0; k < 40; k++)
			if(read_index_at(k) == 0)
				repaired++;
		assert(repaired > 0 && "the first few torn entries are still repaired");
		assert(repaired <= 8
			   && "a truncated-looking file must NOT have its whole header erased");
	}

	// Adjacent ranges are valid. A partial unindexed tail is also valid because
	// it is the expected remnant of an interrupted copy-on-write payload.
	write_raw_region((2U << 8) | 1U, (3U << 8) | 2U, 5, 31);
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && exists);
	assert(region_archive_contains(&ra, 1, 0, &exists) && exists);
	region_archive_destroy(&ra);

	// A fully present indexed record remains readable when only its sector
	// padding was lost. Beta extends this tail before sector-map construction
	// [740c583 RegionFile.java:48-54]. The allocator must then place the next
	// chunk after the claimed sector rather than overwrite it.
	remove(REGION_PATH);
	assert(region_archive_create_new(&ra, world, 0, 0,
									 WORLD_DIM_OVERWORLD));
	sc.ids[0] = 33;
	assert(region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
	uint32_t partial_entry = read_first_index();
	size_t record_end = indexed_record_end(partial_entry);
	assert((record_end % REGION_SECTOR_SIZE) != 0);
	region_archive_destroy(&ra);
	truncate_region(record_end);
	assert(open_region(&ra, world));
	assert(region_archive_contains(&ra, 0, 0, &exists) && exists);
	assert_saved_byte(&ra, 33);
	sc.ids[0] = 34;
	assert(region_archive_set_blocks(&ra, 1, 0, &sc, NULL));
	assert(read_first_index() == partial_entry);
	assert((read_index_at(1) >> 8)
		   >= (partial_entry >> 8) + (partial_entry & 0xFF));
	assert_saved_byte(&ra, 33);
	region_archive_destroy(&ra);

	remove(REGION_PATH);
	assert(region_archive_create_new(&ra, world, 0, 0,
								 WORLD_DIM_OVERWORLD));

	sc.ids[0] = 11;
	assert(region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
	uint32_t first_entry = read_first_index();
	size_t first_size;
	uint8_t* first_payload = read_payload_allocation(first_entry, &first_size);

	// A same-size rewrite must still move, leaving the sole indexed payload
	// byte-for-byte intact until the new index commit finishes.
	sc.ids[0] = 22;
	assert(region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
	uint32_t committed_entry = read_first_index();
	assert((committed_entry >> 8) != (first_entry >> 8));
	size_t old_size;
	uint8_t* old_payload = read_payload_allocation(first_entry, &old_size);
	assert(old_size == first_size);
	assert(memcmp(old_payload, first_payload, old_size) == 0);
	free(old_payload);
	free(first_payload);
	assert_saved_byte(&ra, 22);

	const enum region_archive_test_fail_stage failures[] = {
		REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE,
		REGION_ARCHIVE_TEST_FAIL_PAYLOAD_FLUSH,
		REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE,
		REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH,
	};
	for(size_t k = 0; k < sizeof(failures) / sizeof(*failures); k++) {
		sc.ids[0] = (uint8_t)(40 + k);
		region_archive_test_fail_next(failures[k]);
		assert(!region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
		assert(read_first_index() == committed_entry);
		assert_saved_byte(&ra, 22);
	}

	// The allocator remains usable after rollback.
	sc.ids[0] = 99;
	assert(region_archive_set_blocks(&ra, 0, 0, &sc, NULL));
	assert_saved_byte(&ra, 99);

	assert(region_archive_destroy(&ra));

	check_gap_reuse_after_salvage(world, &sc);
	check_gap_reuse_honours_degraded_floor(world, &sc);
	check_gap_reuse_honours_floor_after_flush_failure(world, &sc);

	free_input_chunk(&sc);
	string_clear(world);
	remove(REGION_PATH);
	printf("test_region_archive_safety: range validation, copy-on-write "
		   "failure rollback, and post-salvage gap reuse OK\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
