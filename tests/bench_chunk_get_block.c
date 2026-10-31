/*
	Microbenchmark for chunk_get_block's per-cell cost on an ORDINARY (near/
	full, far_skin==NULL) chunk -- companion to bench_mesher.c, added for the
	Stage-2 LOD S2 far-storage-compression lane
	(docs/stage2_lod_plan_2026-09-03.md).

	chunk_get_block gained an `if(c->far_skin)` branch when S2 landed. Because
	chunk_mesher_send's padded gather calls it (via chunk_lookup_block) up to
	18^3=5832 times per chunk build, that branch is on the mesher's single
	hottest per-cell path -- paid by EVERY chunk, not just far-ring ones. A
	direct instrument here (this file) measured ~2.10 ns/call pre-S2 (git tip
	9411d8a) vs ~2.55 ns/call immediately after S2 landed (~21%, well above
	noise) before the fix landed: chunk_get_block_voxel (chunk.c) is the SAME
	packed-buffer body chunk_get_block had before S2, callable directly by a
	per-chunk-hoisted caller that has already checked far_skin ONCE outside
	the per-cell loop (chunk_mesher_send now does exactly this).

	Prints ns/call for BOTH entry points on the same chunk:
	  - chunk_get_block: the generic, self-checking dispatcher (still carries
	    the branch -- correct for a far-skin chunk too, but no longer the
	    mesher's hot-path caller for an ordinary chunk).
	  - chunk_get_block_voxel: the hoisted, branch-free target the mesher's
	    per-cell interior loop now calls. Expect this back within noise of
	    the pre-S2 ~2.10 ns/call baseline.
	Always exits 0 (it's a benchmark, not a pass/fail gate); read its stdout.
*/

#define NDEBUG // honest timing -- no assert overhead, same convention as the
			   // other bench_* targets (they link the -UNDEBUG-free
			   // cavex_prod_* pool; this keeps the .c file's OWN asserts,
			   // e.g. in <assert.h> users below, compiled out too).
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "platform/time.h"
#include "world.h"

int main(void) {
	blocks_init();
	struct world w;
	world_create(&w);
	for(w_coord_t x = 0; x < CHUNK_SIZE; x++)
		for(w_coord_t z = 0; z < CHUNK_SIZE; z++)
			for(w_coord_t y = 0; y < CHUNK_SIZE; y++)
				world_set_block(&w, x, y, z,
								(struct block_data) {.type = BLOCK_STONE,
													 .metadata = 0,
													 .sky_light = 15,
													 .torch_light = 0},
								false);
	struct chunk* c = world_find_chunk(&w, 4, 4, 4);
	if(!c) {
		printf("bench_chunk_get_block: chunk missing\n");
		return 1;
	}

	enum { ITERS = 20000000 };

	volatile uint32_t sink = 0;
	ptime_t t0 = time_get();
	for(int i = 0; i < ITERS; i++) {
		c_coord_t x = (c_coord_t)(i & 0xF), y = (c_coord_t)((i >> 4) & 0xF),
				 z = (c_coord_t)((i >> 8) & 0xF);
		struct block_data b = chunk_get_block(c, x, y, z);
		sink += b.type;
	}
	double ms_dispatch = time_diff_ms(t0, time_get());

	sink = 0;
	ptime_t t1 = time_get();
	for(int i = 0; i < ITERS; i++) {
		c_coord_t x = (c_coord_t)(i & 0xF), y = (c_coord_t)((i >> 4) & 0xF),
				 z = (c_coord_t)((i >> 8) & 0xF);
		struct block_data b = chunk_get_block_voxel(c, x, y, z);
		sink += b.type;
	}
	double ms_voxel = time_diff_ms(t1, time_get());

	printf("chunk_get_block (dispatcher, far_skin checked): %d calls, "
		   "%.4f ns/call\n",
		   (int)ITERS, ms_dispatch * 1e6 / ITERS);
	printf("chunk_get_block_voxel (hoisted, branch-free): %d calls, "
		   "%.4f ns/call (sink=%u/%u)\n",
		   (int)ITERS, ms_voxel * 1e6 / ITERS, sink, (uint32_t)ITERS);

	world_destroy(&w);
	return 0;
}
