/*
	W1-c (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
	"Server-side scan cost"): golden order-equivalence proof + iteration-count
	measurement for the far-pick frontier optimization in server_local.c.

	BEFORE this change, the far send loop rescanned the WHOLE
	(2*far_view_distance+1)^2 square once per SENT column (up to
	FAR_SEND_BUDGET times a tick) to find the global-nearest not-yet-sent
	annulus column: 8 x 1089 = 8712 iterations/tick at reach 16. AFTER, the
	annulus membership + distance-sorted order (an OFFSET template, position-
	independent) is precomputed once per far_view_distance value and reused;
	each tick does a single forward walk that resumes across the fsent
	budget instead of rescanning from scratch.

	This test drives the REAL production selection code (server_local.c's
	far_pick_next / far_pick_template_build, exposed under
	CAVEWII_SERVER_LOCAL_TEST as server_local_test_far_pick_next /
	server_local_test_far_pick_rebuild) against a hand-mirrored REFERENCE
	copy of the exact pre-this-commit full-rescan algorithm (OLD_far_pick
	below), so a silent divergence between "what the code does now" and
	"what the code used to do" fails loudly rather than being masked by two
	copies of the same (possibly wrong) logic.

	CASE A: a scripted walk (the player's chunk position changes every few
	ticks, as it does mid-game) at a fixed mid-size reach, up to
	FAR_SEND_BUDGET picks per tick, asserting the exact (x,z) sequence the
	old and new algorithms produce is IDENTICAL, including a synthetic
	"near chunk" (chunk_sent=true) planted inside the annulus that both
	algorithms must skip identically.

	CASE B: a reach CHANGE mid-walk (grow then shrink far_view_distance),
	proving the cached template correctly invalidates/rebuilds and the pick
	sequence still matches the reference algorithm across the change.

	CASE C: the measurement this Wave 1 item asks for -- iterations examined
	per tick, old vs new, at reach 11 and reach 16 (MAX_VIEW_DISTANCE+1 and
	MAX_FAR_VIEW_DISTANCE_CAP on this PC build), both for a single first
	tick (the spec's literal "8 x 1089" framing, an empty far_sent set) and
	for settling the whole annulus (repeated ticks until nothing is left to
	pick), printed for the commit report. PC-only wall-clock/iteration
	counts are an instrumentation signal, not a hardware cert (CLAUDE.md).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <m-lib/m-string.h>

#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "world.h" // enum world_dim

#define HANG_NONE ((w_coord_t)0)

// ---------------------------------------------------------------------------
// Reference copy of the algorithm this commit REPLACES (server_local.c's far
// send loop, pre-W1-c): rescans the whole square from scratch, returns the
// nearest not-yet-sent annulus column (or false), and counts every (x,z)
// cell it examines -- including ones the annulus-membership test rejects,
// matching the spec's "8 x 1089" framing (the FULL square, not just the
// annulus-filtered subset).
// ---------------------------------------------------------------------------
static bool old_far_pick(struct server_local* s, w_coord_t px, w_coord_t pz,
						 w_coord_t* out_x, w_coord_t* out_z,
						 uint64_t* iterations) {
	bool f_found = false;
	w_coord_t f_x = 0, f_z = 0, f_dist2 = 0;
	for(w_coord_t z = pz - s->far_view_distance; z <= pz + s->far_view_distance;
		z++) {
		for(w_coord_t x = px - s->far_view_distance;
			x <= px + s->far_view_distance; x++) {
			if(iterations)
				(*iterations)++;
			w_coord_t ax = px - x, az = pz - z;
			if(ax < 0)
				ax = -ax;
			if(az < 0)
				az = -az;
			w_coord_t cheb = ax > az ? ax : az;
			if(cheb <= MAX_VIEW_DISTANCE || cheb > s->far_view_distance)
				continue;
			if(server_world_is_far_sent(&s->world, x, z)
			   || server_world_is_chunk_sent(&s->world, x, z))
				continue;
			w_coord_t d2 = (px - x) * (px - x) + (pz - z) * (pz - z);
			if(!f_found || d2 < f_dist2) {
				f_found = true;
				f_dist2 = d2;
				f_x = x;
				f_z = z;
			}
		}
	}
	if(f_found) {
		*out_x = f_x;
		*out_z = f_z;
	}
	return f_found;
}

// Marks a synthetic RESIDENT "near" chunk so server_world_is_chunk_sent(x,z)
// reads true, without any real chunk data/generation -- is_chunk_sent only
// reads the `sent` flag, so a zeroed struct server_chunk with sent=true is a
// faithful stand-in for this test's purposes.
static void mark_synthetic_chunk_sent(struct server_world* w, w_coord_t x,
									  w_coord_t z) {
	struct server_chunk tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.sent = true;
	dict_server_chunks_set_at(w->chunks, S_CHUNK_ID(x, z), tmp);
}

static struct server_local* make_server(void) {
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t level;
	string_init_set_str(level, "test_far_pick_frontier_tmp");
	assert(server_world_create_with_populate_scratch(
		&s->world, level, WORLD_DIM_OVERWORLD, INT64_C(4242), true,
		s->world_populate_scratch));
	string_clear(level);
	s->world.local = s;
	s->worlds[0].session = s; /* A1a (D1): owning session for services/callbacks */
	return s;
}

// One tick's worth of picks: drains up to `budget` candidates from BOTH
// algorithms, asserting they agree at every step (found/not-found and, when
// found, the exact coordinate), and marks each confirmed pick far_sent
// before the next one -- exactly like the production loop marking a
// successfully generated+sent column before the next fsent iteration.
static void run_tick_and_check(struct server_local* s, w_coord_t px,
							   w_coord_t pz, int budget,
							   uint64_t* old_iters_out) {
	int new_cursor = 0;
	for(int i = 0; i < budget; i++) {
		w_coord_t ox = 0, oz = 0, nx = 0, nz = 0;
		bool old_found = old_far_pick(s, px, pz, &ox, &oz, old_iters_out);
		bool new_found
			= server_local_test_far_pick_next(s, px, pz, &new_cursor, &nx, &nz);
		assert(old_found == new_found);
		if(!old_found)
			break;
		assert(ox == nx && oz == nz);
		server_world_mark_far_sent(&s->world, ox, oz);
	}
}

static void case_a_scripted_walk(void) {
	struct server_local* s = make_server();
	s->far_view_distance = MAX_VIEW_DISTANCE + 3;
	server_local_test_far_pick_rebuild(s);

	// A synthetic "near chunk" landing inside the FIRST position's annulus --
	// both algorithms must skip it identically, at every subsequent step
	// too (it never gets far_sent, since it is never eligible).
	w_coord_t promoted_x = 0 + MAX_VIEW_DISTANCE + 1;
	w_coord_t promoted_z = 0;
	mark_synthetic_chunk_sent(&s->world, promoted_x, promoted_z);

	// A scripted walk: chunk position advances a little every tick, as a
	// player walking diagonally would (mix of straight runs and turns).
	static const w_coord_t dxs[] = {0, 1, 0, 1, 1, 0, -1, 0, -1, -1, 0, 1};
	static const w_coord_t dzs[] = {0, 0, 1, 0, 1, 1, 0, -1, 0, -1, -1, 0};
	w_coord_t px = 0, pz = 0;
	int total_ticks = (int)(sizeof(dxs) / sizeof(dxs[0]));
	for(int t = 0; t < total_ticks; t++) {
		px += dxs[t];
		pz += dzs[t];
		if(s->far_pick_template_built_for != s->far_view_distance)
			server_local_test_far_pick_rebuild(s);
		run_tick_and_check(s, px, pz, FAR_SEND_BUDGET, NULL);
	}
	// Keep ticking at the final position until the annulus is exhausted, to
	// prove the two algorithms agree all the way to "nothing left" too.
	for(int guard = 0; guard < 200; guard++) {
		int new_cursor = 0;
		w_coord_t ox = 0, oz = 0, nx = 0, nz = 0;
		bool old_found = old_far_pick(s, px, pz, &ox, &oz, NULL);
		bool new_found = server_local_test_far_pick_next(s, px, pz,
														 &new_cursor, &nx, &nz);
		assert(old_found == new_found);
		if(!old_found)
			break;
		assert(ox == nx && oz == nz);
		server_world_mark_far_sent(&s->world, ox, oz);
	}
	// The synthetic near chunk was NEVER picked by either algorithm.
	assert(!server_world_is_far_sent(&s->world, promoted_x, promoted_z));

	server_world_destroy(&s->world);
	free(s);
	printf("CASE A (scripted 12-step walk + synthetic near-chunk gate, "
		   "reach %d): PASS\n",
		   MAX_VIEW_DISTANCE + 3);
}

static void case_b_reach_change(void) {
	struct server_local* s = make_server();
	w_coord_t px = 5, pz = -3;

	// Start small, settle it, then GROW the reach mid-walk (a config/hw-run
	// override changing far_view_distance at runtime), then SHRINK it back
	// down -- each change must force a template rebuild (built_for tracks
	// it) and picks must keep matching the reference algorithm throughout.
	int reaches[] = {MAX_VIEW_DISTANCE + 1, MAX_VIEW_DISTANCE + 4,
					 MAX_VIEW_DISTANCE + 2, MAX_FAR_VIEW_DISTANCE_CAP};
	for(size_t r = 0; r < sizeof(reaches) / sizeof(reaches[0]); r++) {
		s->far_view_distance = reaches[r];
		assert(s->far_pick_template_built_for != s->far_view_distance);
		server_local_test_far_pick_rebuild(s);
		assert(s->far_pick_template_built_for == s->far_view_distance);
		px += 1;
		pz += (r % 2 == 0) ? 1 : -1;
		run_tick_and_check(s, px, pz, FAR_SEND_BUDGET, NULL);
	}

	server_world_destroy(&s->world);
	free(s);
	printf("CASE B (reach change mid-walk, %zu reach values): PASS\n",
		  sizeof(reaches) / sizeof(reaches[0]));
}

// CASE C: the iteration-count measurement this Wave 1 item asks for. Two
// numbers per reach: FIRST-TICK (an empty far_sent set, the spec's literal
// "budget x square" framing) and SETTLE (repeated ticks at a fixed position
// until the whole annulus is claimed -- the total work needed to fully
// populate the ring once, which the old algorithm re-paid on every walk
// step and the new one never repeats within a reach).
static void measure_reach(int far_view_distance) {
	struct server_local* s = make_server();
	s->far_view_distance = far_view_distance;
	server_local_test_far_pick_rebuild(s);
	w_coord_t px = 0, pz = 0;

	uint64_t old_first_tick = 0;
	uint64_t new_before = server_local_far_pick_iterations();
	run_tick_and_check(s, px, pz, FAR_SEND_BUDGET, &old_first_tick);
	uint64_t new_first_tick = server_local_far_pick_iterations() - new_before;

	// Undo the first tick's marks so SETTLE starts from the same empty
	// state old_far_pick's iteration count is compared against.
	server_world_clear_far_sent(&s->world, 0, 0); // no-op safety; real clears below
	server_world_destroy(&s->world);
	free(s);
	s = make_server();
	s->far_view_distance = far_view_distance;
	server_local_test_far_pick_rebuild(s);

	uint64_t old_settle = 0;
	new_before = server_local_far_pick_iterations();
	for(int guard = 0; guard < 100000; guard++) {
		w_coord_t ox = 0, oz = 0;
		if(!old_far_pick(s, px, pz, &ox, &oz, &old_settle))
			break;
		server_world_mark_far_sent(&s->world, ox, oz);
	}
	// Reset far_sent so the new algorithm settles the SAME empty ring.
	server_world_destroy(&s->world);
	free(s);
	s = make_server();
	s->far_view_distance = far_view_distance;
	server_local_test_far_pick_rebuild(s);
	for(int guard = 0; guard < 100000; guard++) {
		int cursor = 0;
		w_coord_t nx = 0, nz = 0;
		if(!server_local_test_far_pick_next(s, px, pz, &cursor, &nx, &nz))
			break;
		server_world_mark_far_sent(&s->world, nx, nz);
	}
	uint64_t new_settle = server_local_far_pick_iterations() - new_before;

	printf("CASE C reach=%d: first-tick old=%llu new=%llu (%.1fx); "
		   "settle-whole-ring old=%llu new=%llu (%.1fx)\n",
		   far_view_distance, (unsigned long long)old_first_tick,
		   (unsigned long long)new_first_tick,
		   new_first_tick ? (double)old_first_tick / (double)new_first_tick
						 : 0.0,
		   (unsigned long long)old_settle, (unsigned long long)new_settle,
		   new_settle ? (double)old_settle / (double)new_settle : 0.0);

	server_world_destroy(&s->world);
	free(s);
}

static void case_c_measurement(void) {
	measure_reach(MAX_VIEW_DISTANCE + 1); // reach 11 on PC (MAX_VIEW_DISTANCE=10)
	measure_reach(MAX_FAR_VIEW_DISTANCE_CAP); // reach 16 on PC
	printf("CASE C: PASS (informational measurement, PC iteration counts -- "
		  "see commit message; not a hardware cert per CLAUDE.md)\n");
}

int main(void) {
	case_a_scripted_walk();
	case_b_reach_change();
	case_c_measurement();
	printf("test_far_pick_frontier_golden: PASS\n");
	return 0;
}
