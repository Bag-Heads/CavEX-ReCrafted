/*
	Far-ring keep-up probe tests (2026-09-03 reach-sweep gate 4: "does
	worldgen keep up while walking").

	Two independent halves, covered here in one binary:

	  1. world_far_missing_count (world.c/.h): the throttled annulus scan --
	     proven against direct world_build_far_section (receive) /
	     world_unload_section (evict) bookkeeping, plus its Chebyshev bounds.
	  2. far_ring_keepup.h: the pure missing_max/settle_ms running-max state
	     machine, driven with synthetic (cx, cz, far_missing, now_ms) samples
	     with no world/network dependency at all.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks_data.h"
#include "game/far_ring_keepup.h"
#include "world.h"

// --- world_far_missing_count: receive/evict bookkeeping -------------------

static void build_flat_far_section(struct world* w, w_coord_t cx,
								   w_coord_t cz) {
	uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t top_id[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE] = {0};
	uint8_t grass_j[CHUNK_SIZE * CHUNK_SIZE] = {0};
	memset(skin_y, 40, sizeof(skin_y));
	memset(top_id, BLOCK_STONE, sizeof(top_id));
	bool ok = world_build_far_section(w, cx * CHUNK_SIZE, cz * CHUNK_SIZE,
									  skin_y, top_id, grass_i, grass_j);
	assert(ok);
}

static void test_far_missing_zero_when_outer_not_past_inner(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;

	assert(world_far_missing_count(&w, 0, 0, 5, 5) == 0);
	assert(world_far_missing_count(&w, 0, 0, 5, 3) == 0);
	// Degenerate but well-defined: no annulus at all still returns 0, not a
	// negative/garbage count, even with an empty world.
	assert(world_far_missing_count(&w, 0, 0, 0, 0) == 0);

	world_destroy(&w);
	puts("  far_missing: outer<=inner is always 0");
}

static void test_far_missing_counts_empty_annulus(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;

	// Chebyshev ring of radius exactly 1 around the origin: the 3x3 block
	// minus the 1x1 center = 8 slots, all missing in a freshly created world.
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 8);

	world_destroy(&w);
	puts("  far_missing: empty world reports the full annulus missing");
}

static void test_far_missing_decrements_on_receive_increments_on_evict(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;

	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 8);

	// RECEIVE two of the eight ring slots (world_build_far_section is the
	// production client install path for a far-skin packet, clin_far_chunk).
	build_flat_far_section(&w, 1, 0);
	build_flat_far_section(&w, 0, 1);
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 6);

	// A section built OUTSIDE the annulus (Chebyshev 2, past outer=1) must
	// not perturb the in-annulus count either way.
	build_flat_far_section(&w, 2, 0);
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 6);

	// A section built AT the center (Chebyshev 0, at/inside inner=0) is
	// likewise outside the annulus and must not perturb the count.
	build_flat_far_section(&w, 0, 0);
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 6);

	// EVICT one of the two received ring slots (world_unload_section is the
	// production client path for a server CRPC_UNLOAD_CHUNK far-evict, both
	// clin_process's CRPC_UNLOAD_CHUNK handler and world_build_far_section's
	// own all-air replacement route through it).
	world_unload_section(&w, 1, 0);
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 7);

	// Re-receive it: back down to 6.
	build_flat_far_section(&w, 1, 0);
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 6);

	world_destroy(&w);
	puts("  far_missing: tracks receive (build_far_section) and evict "
		 "(unload_section) correctly, ignoring out-of-annulus slots");
}

static void test_far_missing_recenters_with_player_position(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;

	build_flat_far_section(&w, 1, 0); // Chebyshev 1 from origin
	assert(world_far_missing_count(&w, 0, 0, 0, 1) == 7);

	// Recentering on (1, 0) makes the built section the CENTER (Chebyshev 0,
	// outside the (0,1] annulus) -- every annulus slot around the new center
	// is unbuilt, so the count goes back to the full 8. This is exactly why
	// far_missing is computed fresh each heartbeat from the player's CURRENT
	// position rather than an incremental counter tied to a fixed center.
	assert(world_far_missing_count(&w, 1, 0, 0, 1) == 8);

	world_destroy(&w);
	puts("  far_missing: recenters correctly as the player moves");
}

// --- far_ring_keepup.h: pure missing_max/settle_ms state machine ---------

static void test_keepup_tracks_missing_max_across_samples(void) {
	struct far_ring_keepup_state st;
	far_ring_keepup_reset(&st);
	assert(far_ring_keepup_missing_max(&st) == 0);
	assert(far_ring_keepup_settle_ms_max(&st) == 0);

	far_ring_keepup_update(&st, 0, 0, 5, 100);
	assert(far_ring_keepup_missing_max(&st) == 5);
	far_ring_keepup_update(&st, 0, 0, 2, 150); // dip: max must not fall
	assert(far_ring_keepup_missing_max(&st) == 5);
	far_ring_keepup_update(&st, 0, 0, 9, 200); // new peak
	assert(far_ring_keepup_missing_max(&st) == 9);

	puts("  keepup: missing_max is a true running max, never decreases");
}

static void test_keepup_settle_ms_is_time_since_last_crossing(void) {
	struct far_ring_keepup_state st;
	far_ring_keepup_reset(&st);

	// The very first sample counts as a "crossing" (have_center was false).
	far_ring_keepup_update(&st, 0, 0, 5, 100);
	assert(far_ring_keepup_settle_ms_max(&st) == 0); // not settled yet

	far_ring_keepup_update(&st, 0, 0, 0, 250); // settles 150ms after t=100
	assert(far_ring_keepup_settle_ms_max(&st) == 150);

	// No crossing (same cx/cz): further far_missing==0 samples must not
	// fabricate a new settle measurement.
	far_ring_keepup_update(&st, 0, 0, 0, 900);
	assert(far_ring_keepup_settle_ms_max(&st) == 150);

	// A real crossing that settles FASTER than the running max must not
	// lower it (it is a running MAX across the whole run).
	far_ring_keepup_update(&st, 1, 0, 3, 910); // crossing at t=910
	far_ring_keepup_update(&st, 1, 0, 0, 920); // settles in 10ms
	assert(far_ring_keepup_settle_ms_max(&st) == 150);

	// A slower crossing raises the max again.
	far_ring_keepup_update(&st, 2, 0, 4, 1000); // crossing at t=1000
	far_ring_keepup_update(&st, 2, 0, 0, 1400); // settles in 400ms
	assert(far_ring_keepup_settle_ms_max(&st) == 400);

	puts("  keepup: settle_ms_max is the running max time-to-settle across "
		 "every crossing, never lowered by a faster one");
}

static void test_keepup_crossing_already_settled_is_zero_not_stale(void) {
	struct far_ring_keepup_state st;
	far_ring_keepup_reset(&st);

	far_ring_keepup_update(&st, 0, 0, 5, 100);
	far_ring_keepup_update(&st, 0, 0, 0, 500); // settles in 400ms
	assert(far_ring_keepup_settle_ms_max(&st) == 400);

	// A crossing sampled at a moment where far_missing is ALREADY 0 (the
	// annulus caught up within one heartbeat period -- the documented
	// sub-second-crossing caveat) must fold in a 0ms settle, not silently
	// skip the crossing or leave the pending flag stuck open.
	far_ring_keepup_update(&st, 1, 0, 0, 600);
	assert(far_ring_keepup_settle_ms_max(&st) == 400); // unchanged: 0 <= 400
	assert(!st.crossing_pending);

	puts("  keepup: a crossing already settled at sample time folds in a "
		 "0ms settle rather than leaving crossing_pending stuck");
}

static void test_keepup_reset_clears_everything(void) {
	struct far_ring_keepup_state st;
	far_ring_keepup_reset(&st);
	far_ring_keepup_update(&st, 3, 4, 12, 1000);
	far_ring_keepup_update(&st, 3, 4, 0, 1500);
	assert(far_ring_keepup_missing_max(&st) == 12);
	assert(far_ring_keepup_settle_ms_max(&st) == 500);

	far_ring_keepup_reset(&st);
	assert(far_ring_keepup_missing_max(&st) == 0);
	assert(far_ring_keepup_settle_ms_max(&st) == 0);
	assert(!st.have_center);
	assert(!st.crossing_pending);

	puts("  keepup: reset clears missing_max/settle_ms_max/center state "
		 "(a fresh world load starts clean)");
}

int main(void) {
	test_far_missing_zero_when_outer_not_past_inner();
	test_far_missing_counts_empty_annulus();
	test_far_missing_decrements_on_receive_increments_on_evict();
	test_far_missing_recenters_with_player_position();
	test_keepup_tracks_missing_max_across_samples();
	test_keepup_settle_ms_is_time_since_last_crossing();
	test_keepup_crossing_already_settled_is_zero_not_stale();
	test_keepup_reset_clears_everything();
	puts("test_far_ring_keepup: all tests passed");
	return 0;
}
