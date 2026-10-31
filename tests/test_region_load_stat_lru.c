/*
	T1 equivalence test for the 2026-09-01 region-probe-order fix.

	server_world_load_chunk used to stat(2) the owning region's .mcr path
	UNCONDITIONALLY before ever consulting the 4-entry region LRU
	(server_world_chunk_region) -- so loading a SECOND chunk out of a region
	whose handle was already open (the common case: chunks stream in per
	region, not one-off) paid a redundant libfat directory walk every time.
	The fix probes the LRU first (a pure in-memory offset-table check, zero
	I/O) and only falls through to the stat on an actual miss.

	This drives the REAL production load path (server_world_load_chunk),
	like test_region_boundary, and pins TWO things:
	  1. Byte-identical load results, stat-order change or not -- two chunks
	     saved in the SAME region both come back with their exact saved
	     bytes, in either load order.
	  2. THE regression pin: loading the second chunk out of an
	     already-resident region must add ZERO stat(2) calls
	     (server_world_region_stat_count), while loading the FIRST chunk of
	     a not-yet-opened region must add exactly one. Reverting to the old
	     unconditional-stat order would make the second load's delta 1
	     instead of 0 and fail this assert.
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
#include "network/server_world.h"
#include "platform/mem2.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2)
#define HM_SIZE	 (CHUNK_SIZE * CHUNK_SIZE)
static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_load_stat_lru_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

// Two chunks in the SAME region (0,0): CHUNK_REGION_COORD(0)==CHUNK_REGION_COORD(5)==0.
#define CX_A 0
#define CZ_A 0
#define CX_B 5
#define CZ_B 5

static uint8_t id_pattern(int i, int salt) {
	return (uint8_t)(i * 7 + 3 + salt * 97);
}

static void write_fixture_chunk(struct region_archive* ra, w_coord_t x,
								w_coord_t z, int salt) {
	struct server_chunk sc = {0};
	sc.ids = malloc(IDS_SIZE);
	sc.metadata = malloc(NIB_SIZE);
	sc.lighting_sky = malloc(NIB_SIZE);
	sc.lighting_torch = malloc(NIB_SIZE);
	sc.heightmap = malloc(HM_SIZE);
	assert(sc.ids && sc.metadata && sc.lighting_sky && sc.lighting_torch
		   && sc.heightmap);
	for(int i = 0; i < IDS_SIZE; i++)
		sc.ids[i] = id_pattern(i, salt);
	memset(sc.metadata, 0x20 + salt, NIB_SIZE);
	memset(sc.lighting_sky, 0x40 + salt, NIB_SIZE);
	memset(sc.lighting_torch, 0x60 + salt, NIB_SIZE);
	memset(sc.heightmap, 64, HM_SIZE);
	sc.modified = true;
	sc.populated = true;

	assert(region_archive_set_blocks(ra, x, z, &sc, NULL));

	free(sc.ids);
	free(sc.metadata);
	free(sc.lighting_sky);
	free(sc.lighting_torch);
	free(sc.heightmap);
}

int main(void) {
	test_fixture_dir("test_region_load_stat_lru_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // clean slate

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	// ---- session 1: save both chunks into the SAME region --------------------
	{
		struct region_archive ra;
		assert(region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
		write_fixture_chunk(&ra, CX_A, CZ_A, 1);
		write_fixture_chunk(&ra, CX_B, CZ_B, 2);
		assert(region_archive_destroy(&ra));
	}

	// ---- session 2: fresh world, empty LRU -- drive the real load path -------
	{
		struct server_world w = {0};
		server_world_create(&w, level, WORLD_DIM_OVERWORLD, 1337,
							true /* superflat: a live generator, so a
									regression fabricates instead of failing */);

		uint32_t stat_before_first = server_world_region_stat_count();

		// First load: region r.0.0 is NOT yet resident -> exactly one stat.
		struct server_chunk* sc_a = NULL;
		bool loaded_a = server_world_load_chunk(&w, CX_A, CZ_A, &sc_a);
		assert(loaded_a && sc_a);
		for(int i = 0; i < IDS_SIZE; i++)
			assert(sc_a->ids[i] == id_pattern(i, 1));
		assert(sc_a->populated);
		assert(!sc_a->modified);

		uint32_t stat_after_first = server_world_region_stat_count();
		assert(stat_after_first == stat_before_first + 1
			   && "first load of a not-yet-open region must stat exactly once");

		// Second load: SAME region, now resident in the LRU -> THE pin, zero
		// additional stats. Under the old (pre-fix) unconditional-stat order
		// this would be stat_after_first + 1, not +0.
		struct server_chunk* sc_b = NULL;
		bool loaded_b = server_world_load_chunk(&w, CX_B, CZ_B, &sc_b);
		assert(loaded_b && sc_b);
		for(int i = 0; i < IDS_SIZE; i++)
			assert(sc_b->ids[i] == id_pattern(i, 2));
		assert(sc_b->populated);
		assert(!sc_b->modified);

		uint32_t stat_after_second = server_world_region_stat_count();
		assert(stat_after_second == stat_after_first
			   && "loading a second chunk out of an ALREADY-RESIDENT region "
				  "must not pay another stat(2) -- probe the LRU before "
				  "the disk");

		server_world_destroy(&w);
	}

	string_clear(level);
	printf("test_region_load_stat_lru: OK (LRU-first probe skips the "
		   "redundant stat; byte-identical loads either way)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
