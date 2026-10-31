/*
	T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed -- see
	docs/far_ring_geomip_design_2026-08-26.md).

	Measures the far-ring geomip merge against the REAL client chunk_mesher
	worker (not a hand-mirrored copy) over a deterministic staircase terrain,
	flag-off vs three error budgets. Reports, per configuration:
	  - total emitted vertices across the meshed area (the quality/perf tradeoff
	    this spike exists to make),
	  - total + average per-chunk worker build time, read via
	    chunk_mesher_build_ms() -- the worker THREAD's own self-timed wall clock
	    for chunk_mesher_build, not this process's send/wait loop. CLAUDE.md's
	    "measure before optimizing" rule specifically distrusts a caller-side
	    wall clock (it conflates compute with scheduler preemption); this is the
	    same self-timed instrument the debug overlay reads, per-chunk, summed
	    ourselves across the run instead of relying on the process-lifetime
	    running max chunk_mesher_build_ms() otherwise reports,
	  - the geomip patch considered/merged counts (chunk_mesher_geomip_stats).

	Always exits 0 (it is a benchmark, not a pass/fail gate) -- like bench_mesher,
	read its stdout. PC-MEASURED ONLY: this says nothing about Broadway/GX build
	cost or draw cost; that verdict is owed to the real-Wii rig (see the design
	doc's measurement plan).

	Terrain: a staircase height field h(x) = floor(x/3) over a 4x4 grid of chunk
	COLUMNS (64x64 blocks), flat in z. This deliberately does NOT align a height
	step with a 2-column patch boundary (patch pairs are (0,1),(2,3),(4,5)... --
	steps land on x=3,6,9,... so some patches straddle a step (1-block internal
	error) and most don't), giving a controlled, reproducible MIX of mergeable
	and marginal patches instead of the all-or-nothing result flat terrain would
	give (see test_far_geomip_flagoff_equiv.c's CASE B for that simpler case).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "network/client_interface.h"
#include "platform/displaylist.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#define GRID_CHUNKS 4 // 4x4 chunk columns = 64x64 blocks
#define HANG_TIMEOUT_MS 5000

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static struct world w;

static void set(w_coord_t x, w_coord_t y, w_coord_t z) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = BLOCK_STONE,
										 .metadata = 0,
										 .sky_light = 15,
										 .torch_light = 0},
					false);
}

static bool wait_for_mesh(struct chunk* c) {
	ptime_t t0 = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		if(c->mesh_in_flight) {
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS) {
				fprintf(stderr, "bench_far_geomip: HANG (mesh_in_flight after "
								"%d ms)\n",
						HANG_TIMEOUT_MS);
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

static bool force_skin_mesh_and_wait(struct chunk* c, uint16_t dist_blocks) {
	c->lod = 1;
	c->foliage_lod = 0;
	c->lod_dist_blocks = dist_blocks;
	c->dirty_generation++;
	c->mesh_in_flight = false;
	if(!chunk_mesher_send(c))
		return false;
	c->rebuild_displist = false;
	c->first_meshed = true;
	c->mesh_in_flight = true;
	return wait_for_mesh(c);
}

static size_t total_vertices(struct chunk* c) {
	size_t total = 0;
	for(int k = 0; k < CHUNK_MESH_LISTS; k++)
		if(c->has_displist[k])
			total += c->mesh[k].index;
	return total;
}

// Build the staircase terrain across the full GRID_CHUNKS x GRID_CHUNKS block
// area, plus a 1-chunk skirt of the SAME height field just outside it so every
// meshed chunk's own immediate ring neighbour is real terrain (not the missing-
// neighbour placeholder), matching how a real interior far-ring chunk sees its
// neighbours. Returns the chunk pointers for the interior grid (row-major).
static void build_staircase(struct chunk* out[GRID_CHUNKS][GRID_CHUNKS],
							w_coord_t yf) {
	const w_coord_t W = GRID_CHUNKS * CHUNK_SIZE;
	for(w_coord_t x = -CHUNK_SIZE; x < W + CHUNK_SIZE; x++) {
		w_coord_t xc = x < 0 ? 0 : (x >= W ? W - 1 : x); // clamp the skirt ring
		w_coord_t h = yf + xc / 3;
		for(w_coord_t z = -CHUNK_SIZE; z < W + CHUNK_SIZE; z++)
			set(x, h, z);
	}
	for(int cx = 0; cx < GRID_CHUNKS; cx++)
		for(int cz = 0; cz < GRID_CHUNKS; cz++)
			out[cx][cz] = world_find_chunk(
				&w, cx * CHUNK_SIZE + 8, yf + (cx * CHUNK_SIZE) / 3, cz * CHUNK_SIZE + 8);
}

struct run_result {
	size_t total_vertices;
	double total_build_ms;
	double max_build_ms;
	uint32_t considered;
	uint32_t merged;
};

static struct run_result run_config(struct chunk* grid[GRID_CHUNKS][GRID_CHUNKS],
									bool geomip_on, float budget_px) {
	world_lod_set_geomip(geomip_on, budget_px, 70.0F, 480.0F);

	uint32_t considered0 = 0, merged0 = 0;
	chunk_mesher_geomip_stats(&considered0, &merged0);

	struct run_result r = {0};
	for(int cx = 0; cx < GRID_CHUNKS; cx++) {
		for(int cz = 0; cz < GRID_CHUNKS; cz++) {
			struct chunk* c = grid[cx][cz];
			assert(c);
			bool ok = force_skin_mesh_and_wait(c, /*dist_blocks=*/128);
			assert(ok);
			r.total_vertices += total_vertices(c);
			float last = 0.0F;
			chunk_mesher_build_ms(&last, NULL);
			r.total_build_ms += last;
			if(last > r.max_build_ms)
				r.max_build_ms = last;
		}
	}

	uint32_t considered1 = 0, merged1 = 0;
	chunk_mesher_geomip_stats(&considered1, &merged1);
	r.considered = considered1 - considered0;
	r.merged = merged1 - merged0;

	world_lod_set_geomip(false, 0.0F, 70.0F, 480.0F); // restore the faithful default
	return r;
}

int main(void) {
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	chunk_mesher_init();
	world_create(&w);

	struct chunk* grid[GRID_CHUNKS][GRID_CHUNKS];
	build_staircase(grid, /*yf=*/64);

	printf("bench_far_geomip: %dx%d chunk staircase terrain (h=floor(x/3)), "
		   "PC-measured only; Broadway/GX verdict owed to the rig "
		   "(far_idle_2m/far_walk_6m/perf_stress_5m)\n\n",
		   GRID_CHUNKS, GRID_CHUNKS);

	// Untimed warm-up: the FIRST config run in-process pays a one-time cost
	// (cold caches, first-touch page faults, the worker thread's first wake)
	// that has nothing to do with geomip. Discard one full pass before timing
	// anything, so every reported config starts from the same warmed state --
	// CLAUDE.md's "measure before optimizing" rule applies to this bench's own
	// methodology, not just the code it measures.
	(void)run_config(grid, false, 0.0F);

	struct run_result off = run_config(grid, false, 0.0F);
	printf("  flag-off (faithful default): %8zu verts total, %6.1f verts/chunk "
		   "avg, %7.4f ms worker-build total, %7.4f ms max/chunk\n",
		   off.total_vertices, (double)off.total_vertices / (GRID_CHUNKS * GRID_CHUNKS),
		   off.total_build_ms, off.max_build_ms);

	const float budgets[] = {1.0F, 2.0F, 4.0F};
	for(size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
		struct run_result on = run_config(grid, true, budgets[i]);
		double pct_saved = off.total_vertices > 0 ?
			100.0 * (1.0 - (double)on.total_vertices / (double)off.total_vertices) :
			0.0;
		printf("  geomip-on budget=%.1fpx:      %8zu verts total, %6.1f verts/chunk "
			   "avg, %7.4f ms worker-build total, %7.4f ms max/chunk -- "
			   "%5.1f%% fewer verts than flag-off; %u/%u patches merged\n",
			   budgets[i], on.total_vertices,
			   (double)on.total_vertices / (GRID_CHUNKS * GRID_CHUNKS),
			   on.total_build_ms, on.max_build_ms, pct_saved, on.merged,
			   on.considered);
	}

	printf("\nNOTE: worker-build ms is chunk_mesher_build_ms() -- the mesher "
		   "WORKER THREAD's own self-timed wall clock (CLAUDE.md's measurement "
		   "rule: not this process's send/wait loop, which would conflate "
		   "compute with scheduler preemption). PC single-core numbers only; no "
		   "claim is made about Broadway CPU cost or GX draw cost.\n");

	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);
	return 0;
}
