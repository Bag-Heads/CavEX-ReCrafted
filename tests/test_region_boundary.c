/*
	Regression test for the 2026-06-12 persistence-audit headline: a saved
	chunk whose owning region file was NOT yet open silently REGENERATED
	(server_world_load_chunk only scanned the already-open region LRU), and
	the evictor then overwrote the real .mcr with the regenerated terrain --
	permanent, silent save corruption at region/frontier boundaries.

	The test drives the REAL production load path, cross-session:
	  session 1: write a recognizable chunk into region r.1.0 (chunk 33,5 --
	             deliberately NOT region 0,0) through region_archive, exactly
	             as the evictor's save does, then close everything.
	  session 2: a FRESH server_world (empty region LRU, superflat generator
	             armed) must come back with the SAVED bytes via
	             server_world_load_chunk. Under the old code the LRU scan
	             missed, the superflat generator fabricated a replacement,
	             and this assert fired.

	The superflat generator is armed on purpose: a regression must FABRICATE
	a wrong chunk (caught by the byte compare), not merely fail the load.
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
static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_boundary_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

// Chunk (33,5) lives in region (1,0): CHUNK_REGION_COORD(33) == 1. The whole
// point is that a fresh session has never opened r.1.0.mcr.
#define TEST_CX 33
#define TEST_CZ 5

static uint8_t id_pattern(int i) {
	return (uint8_t)(i * 7 + 3);
}

int main(void) {
	test_fixture_dir("test_region_boundary_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.1.0.mcr")); // clean slate

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	// ---- session 1: save the chunk the way the evictor does -----------------
	{
		struct region_archive ra;
		bool ok = region_archive_create_new(&ra, level, 1, 0,
											WORLD_DIM_OVERWORLD);
		assert(ok);

		struct server_chunk sc = {0};
		sc.ids = malloc(IDS_SIZE);
		sc.metadata = malloc(NIB_SIZE);
		sc.lighting_sky = malloc(NIB_SIZE);
		sc.lighting_torch = malloc(NIB_SIZE);
		sc.heightmap = malloc(HM_SIZE);
		assert(sc.ids && sc.metadata && sc.lighting_sky && sc.lighting_torch
			   && sc.heightmap);
		for(int i = 0; i < IDS_SIZE; i++)
			sc.ids[i] = id_pattern(i);
		memset(sc.metadata, 0x21, NIB_SIZE);
		memset(sc.lighting_sky, 0x43, NIB_SIZE);
		memset(sc.lighting_torch, 0x65, NIB_SIZE);
		memset(sc.heightmap, 64, HM_SIZE);
		sc.modified = true;
		sc.populated = true;

		ok = region_archive_set_blocks(&ra, TEST_CX, TEST_CZ, &sc, NULL);
		assert(ok);

		region_archive_destroy(&ra);
		free(sc.ids);
		free(sc.metadata);
		free(sc.lighting_sky);
		free(sc.lighting_torch);
		free(sc.heightmap);
	}

	// ---- session 2: a fresh world MUST load, not regenerate -----------------
	{
		struct server_world w = {0};
		server_world_create(&w, level, WORLD_DIM_OVERWORLD, 1337,
							true /* superflat: a live generator, so a
									regression fabricates instead of failing */);

		struct server_chunk* sc = NULL;
		bool loaded = server_world_load_chunk(&w, TEST_CX, TEST_CZ, &sc);
		assert(loaded && sc);

		// The saved bytes, not superflat's. Under the old LRU-only scan the
		// ids came back as the superflat layer cake and this fired.
		for(int i = 0; i < IDS_SIZE; i++)
			assert(sc->ids[i] == id_pattern(i));
		assert(sc->populated); // TerrainPopulated=1 survived the round trip
		assert(!sc->modified); // a LOADED chunk is clean (nothing to re-save)

		server_world_destroy(&w);
	}

	string_clear(level);
	printf("test_region_boundary: OK (cross-session region-boundary load)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
