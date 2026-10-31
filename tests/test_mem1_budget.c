/*
	Regression guard for the P0 MEM1-exhaustion crash: a real (non-superflat) world
	crashed a few frames in on the Wii. The ~80 KB/chunk SERVER chunk arrays
	(worldgen_alloc_chunk: ids + metadata + 2 light nibble arrays + heightmap) used to
	malloc() from the ~6 MB MEM1 newlib heap; at view distance the resident set (the
	|dx|,|dz| <= 5 keep square = 121 chunks, ~9.5 MB) OVERFLOWED that heap -> a NULL
	allocation was dereferenced in the NDEBUG'd displaylist GX path -> DSI fault. The fix
	routes those arrays through mem2_alloc (the idle ~36 MB MEM2 pool; mem2_* == plain
	malloc on PC, so PC behaviour is byte-unchanged).

	This test exercises the REAL worldgen code path (worldgen_generate_chunk ->
	worldgen_alloc_chunk) and proves:
	  1. every server-chunk array is allocated via mem2_alloc (not bare malloc) -- the
	     mem2 alloc counter advances 5x per generated chunk;
	  2. the view-distance-5 resident working set EXCEEDS the old ~6 MB MEM1 heap, i.e. it
	     WOULD have OOM'd there -- now it lives in MEM2;
	  3. every mem2_alloc is matched by a mem2_free (no leak across stream cycles).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network/server_world.h"
#include "platform/mem2.h"
#include "worldgen/noise.h"
#include "worldgen/simplex.h"
#include "worldgen/worldgen.h"

// --- stubbed external deps: worldgen consults the global block table + nibble helpers.
//     The terrain ids don't depend on the table (only the optional sky-light pass does),
//     so a null table is sufficient -- same standalone harness as test_worldgen.c. -------
struct block;
struct block* blocks[256] = {0};
uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}
// -------------------------------------------------------------------------------------

// View distance 5 -> the server keep set is the |dx|,|dz| <= 5 square = 11 x 11 = 121.
#define GRID 11
#define N (GRID * GRID)

// One server_chunk's array bytes: ids (full column) + 3 nibble arrays (half each) + heightmap.
#define PER_CHUNK_BYTES                                                                    \
	((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT                                         \
	 + 3 * ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2)                             \
	 + (size_t)CHUNK_SIZE * CHUNK_SIZE)

// The capped MEM1 newlib heap these arrays used to fill (the F3 overlay read ~6041 KB used
// / 79 KB free at the crash). Conservative 6 MB.
#define OLD_MEM1_HEAP_BYTES (6u * 1024u * 1024u)

static size_t worldgen_alloc_countdown;
static void* fail_nth_worldgen_alloc(size_t size) {
	if(worldgen_alloc_countdown > 0 && --worldgen_alloc_countdown == 0)
		return NULL;
	return malloc(size);
}

static void free_arrays(struct server_chunk* sc) {
	// MUST mem2_free: matches worldgen_alloc_chunk's mem2_alloc AND advances the free
	// counter (a plain free() on a real-MEM2 pointer would corrupt the lwp heap on Wii).
	mem2_free(sc->ids);
	mem2_free(sc->metadata);
	mem2_free(sc->lighting_sky);
	mem2_free(sc->lighting_torch);
	mem2_free(sc->heightmap);
}

int main(void) {
	/* Generator construction owns eight Perlin arrays followed by three Simplex
	 * arrays. A late failure must release the completed prefix and return NULL,
	 * never index through the failed allocation under Wii NDEBUG. */
	worldgen_alloc_countdown = 4;
	octave_test_set_alloc_hook(fail_nth_worldgen_alloc);
	assert(worldgen_create(12345, WORLD_DIM_OVERWORLD) == NULL);
	octave_test_set_alloc_hook(NULL);

	worldgen_alloc_countdown = 2;
	simplex_octaves_test_set_alloc_hook(fail_nth_worldgen_alloc);
	assert(worldgen_create(12345, WORLD_DIM_OVERWORLD) == NULL);
	simplex_octaves_test_set_alloc_hook(NULL);

	struct worldgen* wg = worldgen_create(12345, WORLD_DIM_OVERWORLD);
	assert(wg);

	static struct server_chunk chunks[N];
	memset(chunks, 0, sizeof(chunks));

	size_t alloc0 = mem2_stat_alloc_calls();
	size_t bytes0 = mem2_stat_alloc_bytes();

	// Generate the full view-distance-5 set and KEEP every chunk resident -- this is exactly
	// the working set the streamer holds at spawn, i.e. the crash trigger.
	int gen = 0;
	for(int cx = -GRID / 2; cx <= GRID / 2; cx++) {
		for(int cz = -GRID / 2; cz <= GRID / 2; cz++) {
			struct server_chunk* sc = &chunks[gen];
			bool ok = worldgen_generate_chunk(wg, sc, cx, cz);
			assert(ok);
			// All five arrays present (a failed mem2_alloc returns NULL -> generate fails).
			assert(sc->ids && sc->metadata && sc->lighting_sky && sc->lighting_torch
				   && sc->heightmap);
			gen++;
		}
	}
	assert(gen == N);

	size_t resident_calls = mem2_stat_alloc_calls() - alloc0;
	size_t resident_bytes = mem2_stat_alloc_bytes() - bytes0;

	// PROOF 1: the five server arrays of every chunk route through mem2_alloc (>= 5 per chunk),
	// accounting for >= the per-chunk array bytes. (If worldgen also takes transient mem2
	// scratch it only raises these, so >= is the robust check.)
	assert(resident_calls >= (size_t)5 * N);
	assert(resident_bytes >= (size_t)N * PER_CHUNK_BYTES);

	printf("[mem1_budget] resident server-array set in MEM2: %zu KB across %d chunks "
		   "(%zu mem2 allocs, %zu B/chunk)\n",
		   resident_bytes / 1024, N, resident_calls, (size_t)PER_CHUNK_BYTES);

	// PROOF 2 (why it crashed): the resident set EXCEEDS the ~6 MB MEM1 heap it used to malloc
	// from -> it WOULD OOM there. It now lives in the idle ~36 MB MEM2 instead.
	assert((size_t)N * PER_CHUNK_BYTES > OLD_MEM1_HEAP_BYTES);
	printf("[mem1_budget]   %zu KB resident > %u KB MEM1 heap -> would OOM on MEM1; "
		   "now in MEM2.\n",
		   ((size_t)N * PER_CHUNK_BYTES) / 1024, OLD_MEM1_HEAP_BYTES / 1024);

	// PROOF 3 (no leak): free every array via mem2_free; the five per chunk are all matched.
	size_t free0 = mem2_stat_free_calls();
	for(int i = 0; i < N; i++)
		free_arrays(&chunks[i]);
	assert(mem2_stat_free_calls() - free0 >= (size_t)5 * N);

	worldgen_destroy(wg);

	// Net balance: every mem2_alloc this session has a matching mem2_free (the arrays plus any
	// transient worldgen mem2 scratch). A persistent imbalance would be a leak that, over many
	// stream cycles, refills the heap and reintroduces the crash.
	assert(mem2_stat_alloc_calls() == mem2_stat_free_calls());

	printf("[mem1_budget] PASS: server arrays in MEM2, %d-chunk set > 6MB MEM1, no leak.\n",
		   N);
	return 0;
}
