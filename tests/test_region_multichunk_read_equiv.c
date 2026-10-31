/*
	T1 equivalence test for the coalesced-read refactor of region_archive.c
	(2026-08-24 Wii-only world-load-stall investigation,
	docs/resource_lessons_review_2026-08-24.md section (b)).

	region_archive_read_chunk_nbt (behind region_archive_get_chunk/get_blocks)
	and region_archive_stream_block_ids (behind region_archive_get_block_ids*)
	used to read one on-disk record via a fseek followed by THREE (resp. TWO)
	small freads: the 4-byte length field, the 1-byte compression-type marker,
	then the payload. They now read the whole allocated sector range (resp. the
	5-byte header) in ONE fread and parse the already-in-memory buffer. The
	hard rule for a T1 change is that not one decoded byte may differ -- this
	test proves that across SEVERAL chunks sharing one region file (different
	offsets, different sector counts, one at the region's local-coordinate
	edge), and pins the read-coalescing itself by asserting the exact fread
	count region_archive_get_chunk now costs per record: regressing back to
	the old three-fread shape would fail the COUNT assertions below even if
	every byte still happened to decode correctly.

	It also pins the "handle stays open across a chunk-load batch" property
	that predates this task (region_archive.c's cached FILE*, commit
	4524981, and the server_world MAX_REGIONS=4 LRU): fopen_n must not move
	across either read loop, proving neither reader reopens the region per
	chunk.
*/

#include <assert.h>
#include <stdbool.h>
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
#include "network/server_world.h" // struct server_chunk, CHUNK_SIZE, WORLD_HEIGHT
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2)
#define HM_SIZE	 (CHUNK_SIZE * CHUNK_SIZE)
static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_multichunk_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define N_CHUNKS 5

struct chunk_fixture {
	w_coord_t x, z;
	uint32_t seed; // decorrelates the pattern between chunks
};

// Five records in one region: the origin, two axis-offset neighbours, an
// interior point, and (31,31) -- the maximum LOCAL coordinate in a region,
// so the allocator's sort/gap-search sees a spread of offsets/sector counts.
static const struct chunk_fixture s_fixtures[N_CHUNKS] = {
	{0, 0, 0x1u}, {5, 0, 0x2u}, {0, 5, 0x3u}, {17, 23, 0x4u}, {31, 31, 0x5u},
};

static void fill_chunk(struct server_chunk* sc, uint32_t seed) {
	sc->ids = malloc(IDS_SIZE);
	sc->metadata = malloc(NIB_SIZE);
	sc->lighting_sky = malloc(NIB_SIZE);
	sc->lighting_torch = malloc(NIB_SIZE);
	sc->heightmap = malloc(HM_SIZE);
	assert(sc->ids && sc->metadata && sc->lighting_sky && sc->lighting_torch
		   && sc->heightmap);
	for(int i = 0; i < IDS_SIZE; i++)
		sc->ids[i] = (uint8_t)(i * 7 + 3 + seed * 17);
	for(int i = 0; i < NIB_SIZE; i++) {
		sc->metadata[i] = (uint8_t)(i * 5 + 1 + seed * 11);
		sc->lighting_sky[i] = (uint8_t)(i * 11 + 2 + seed * 13);
		sc->lighting_torch[i] = (uint8_t)(i * 13 + 4 + seed * 19);
	}
	for(int i = 0; i < HM_SIZE; i++)
		sc->heightmap[i] = (uint8_t)(i + 64 + seed);
	sc->modified = true;
	sc->populated = true;
}

static void free_write_chunk(struct server_chunk* sc) {
	free(sc->ids);
	free(sc->metadata);
	free(sc->lighting_sky);
	free(sc->lighting_torch);
	free(sc->heightmap);
}

static void free_read_chunk(struct server_chunk* sc) {
	mem2_free(sc->ids);
	mem2_free(sc->metadata);
	mem2_free(sc->lighting_sky);
	mem2_free(sc->lighting_torch);
	mem2_free(sc->heightmap);
}

static void assert_chunk_matches(const struct server_chunk* got,
								 const struct server_chunk* want) {
	assert(memcmp(got->ids, want->ids, IDS_SIZE) == 0);
	assert(memcmp(got->metadata, want->metadata, NIB_SIZE) == 0);
	assert(memcmp(got->lighting_sky, want->lighting_sky, NIB_SIZE) == 0);
	assert(memcmp(got->lighting_torch, want->lighting_torch, NIB_SIZE) == 0);
	assert(memcmp(got->heightmap, want->heightmap, HM_SIZE) == 0);
	assert(got->populated == want->populated);
}

int main(void) {
	test_fixture_dir("test_region_multichunk_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	struct region_archive ra;
	assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));

	struct server_chunk written[N_CHUNKS];
	struct te_chest chests[N_CHUNKS];
	for(int k = 0; k < N_CHUNKS; k++) {
		memset(&written[k], 0, sizeof(written[k]));
		fill_chunk(&written[k], s_fixtures[k].seed);

		// One distinct chest per chunk -- exercises the TE sink through the
		// refactored reader too, and perturbs each record's compressed size
		// a little so the five records don't all claim identical sectors.
		memset(&chests[k], 0, sizeof(chests[k]));
		chests[k] = (struct te_chest) {
			.x = s_fixtures[k].x * CHUNK_SIZE + 1,
			.y = 64,
			.z = s_fixtures[k].z * CHUNK_SIZE + 1,
		};
		chests[k].items[0]
			= (struct item_data) {.id = (uint16_t)(260 + k), .count = (uint8_t)(k + 1)};

		struct chunk_tile_entities te
			= {.chests = &chests[k], .n_chest = 1};
		assert(region_archive_set_blocks(
			&ra, s_fixtures[k].x, s_fixtures[k].z, &written[k], &te));
	}

	// Close and reopen: the write-side handle is gone, so the read loop below
	// starts from a genuinely fresh region_archive_create (its own fopen +
	// index-table read), exactly like a Wii session opening a saved region
	// for the first time this run.
	assert(region_archive_destroy(&ra));
	assert(region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));

	// --- region_archive_get_chunk (the world-load hot path) -----------------
	region_archive_io_stats_reset();
	for(int k = 0; k < N_CHUNKS; k++) {
		struct server_chunk loaded = {0};
		struct chunk_tile_entities te_out = {0};
		assert(region_archive_get_chunk(&ra, s_fixtures[k].x, s_fixtures[k].z,
										&loaded, &te_out, NULL));
		assert_chunk_matches(&loaded, &written[k]);
		assert(te_out.n_chest == 1);
		assert(te_out.chests[0].x == chests[k].x);
		assert(te_out.chests[0].y == chests[k].y);
		assert(te_out.chests[0].z == chests[k].z);
		assert(te_out.chests[0].items[0].id == chests[k].items[0].id);
		assert(te_out.chests[0].items[0].count == chests[k].items[0].count);
		free_read_chunk(&loaded);
		tile_entities_parse_free(&te_out);
	}
	{
		uint32_t fopen_n = 999, fread_n = 0;
		uint64_t bytes_read = 0;
		region_archive_io_stats(&fopen_n, &fread_n, &bytes_read);
		// THE pin: one coalesced fread per record, for every one of the five
		// chunks -- not the old three (length, type, payload). A regression
		// back to the per-field shape would read 15 here, not 5.
		assert(fread_n == N_CHUNKS
			   && "region_archive_get_chunk must cost exactly one fread per "
				  "chunk record (coalesced header+payload read)");
		// THE handle-lifetime pin: five chunk loads through the SAME open
		// region, zero additional fopens.
		assert(fopen_n == 0
			   && "region_archive_get_chunk must not reopen the region file "
				  "per chunk -- the handle is cached on region_archive");
		assert(bytes_read > 0);
	}

	// --- region_archive_get_block_ids_metadata (portal search / respawn) ---
	// This path's payload still streams through region_block_reader's bounded
	// 1KB window on purpose (MEM1-bounded reader, see region_archive.c) -- only
	// its 5-byte header is coalesced -- so the fread count is payload-size
	// dependent and not pinned exactly. Byte-for-byte ids/metadata content and
	// the zero-fopen handle-lifetime property ARE pinned.
	region_archive_io_stats_reset();
	for(int k = 0; k < N_CHUNKS; k++) {
		uint8_t ids[IDS_SIZE];
		uint8_t metadata[NIB_SIZE];
		enum region_archive_read_result result
			= region_archive_get_block_ids_metadata(
				&ra, s_fixtures[k].x, s_fixtures[k].z, ids, metadata);
		assert(result == REGION_ARCHIVE_READ_OK);
		assert(memcmp(ids, written[k].ids, IDS_SIZE) == 0);
		assert(memcmp(metadata, written[k].metadata, NIB_SIZE) == 0);
	}
	{
		uint32_t fopen_n = 999, fread_n = 0;
		uint64_t bytes_read = 0;
		region_archive_io_stats(&fopen_n, &fread_n, &bytes_read);
		assert(fopen_n == 0
			   && "region_archive_get_block_ids_metadata must not reopen the "
				  "region file per chunk either");
		// At least the five coalesced headers; a generous ceiling catches a
		// gross regression (e.g. back to per-field header reads) without
		// coupling the test to zlib's exact output size for this fixture.
		assert(fread_n >= N_CHUNKS && fread_n <= N_CHUNKS * 40);
	}

	for(int k = 0; k < N_CHUNKS; k++)
		free_write_chunk(&written[k]);
	assert(region_archive_destroy(&ra));
	string_clear(level);
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	printf("test_region_multichunk_read_equiv: %d chunks, coalesced-read "
		   "byte-equivalence + fread-count pin OK\n",
		   N_CHUNKS);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
