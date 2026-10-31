/* Copyright (c) 2026 CaveWii_v2 contributors */
/* MAINTAINER RULING 2026-09-11 (runtime split server load radius), Astra round
 * 7 A: a faithful-mode far job in flight at the join. Before the join the host
 * streams the far ring; a covering quadrant of far column F defers under the
 * generation budget, so gen_pending[1] = F and both eviction selectors pin F's
 * 3x3 support. The guest joins: the split policy turns the far ring off and the
 * render reach makes it invisible, so the far walk never runs again -- and
 * without SPLIT_FAR_JOB_IN_FLIGHT_DRAIN nothing ever resumes F: its support
 * (outside BOTH dial squares) stays pinned for the whole split. This pins:
 *  1. the mechanism -- while gen_pending[1] is set the support column is never
 *     offered (that pin is right: an evict/regenerate livelock otherwise);
 *  2. the fix -- the server loop finishes the in-flight job under split without
 *     starting new far work, the pin lifts, and the support column is evicted
 *     like any other column outside the keep union. */
#include "chunk.h"
#include "game/game_state.h"
#include "network/region_archive.h"
#include "support/split_residency_fixture.h"
#include <stdatomic.h>

#define DIAL 4
#define SEP_CHUNKS 24
#define SEP_BLOCKS (SEP_CHUNKS * 16)

static atomic_bool run_drain = ATOMIC_VAR_INIT(false);
static void* drain_rpc(void* unused) {
	(void)unused;
	while(atomic_load(&run_drain)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static bool kept_by(struct server_local* s, w_coord_t x, w_coord_t z) {
	return residency_contains(s->residency_anchors,
							  server_local_two_anchor(s) ? 2 : 1,
							  server_local_view_radius(s), x, z);
}
static bool pick(struct server_local* s, w_coord_t* x, w_coord_t* z) {
	w_coord_t r = server_local_view_radius(s);
	if(server_local_two_anchor(s))
		return server_world_furthest_chunk_anchors(&s->world, r, 0, 0, -1,
												   s->residency_anchors, 2, x, z);
	return server_world_furthest_chunk(&s->world, r, 0, 0, -1,
									   s->residency_anchors[0].x,
									   s->residency_anchors[0].z, x, z);
}
/* Evict everything offered; return whether (sx,sz) was among the offers. */
static bool drain_offers(struct server_local* s, w_coord_t sx, w_coord_t sz) {
	bool offered = false;
	unsigned evicted = 0;
	for(;;) {
		w_coord_t x, z;
		if(!pick(s, &x, &z))
			break;
		assert(!kept_by(s, x, z));
		if(x == sx && z == sz)
			offered = true;
		assert(server_world_save_chunk(&s->world, true, x, z));
		assert(++evicted < 4096);
	}
	return offered;
}

int main(void) {
	assert(clin_init());
	blocks_init();
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(
		SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	s->split_view_radius = DIAL;
	s->far_lod_terrain_only = false; /* the faithful far path is the one that populates */
	server_local_set_split_reach(DIAL);   /* the Wii config: reach 4 */
	s->residency_anchors[0] = (struct residency_anchor) {0, 0};
	assert(s->far_view_distance == MAX_VIEW_DISTANCE + 1);

	/* Single player, far ring on: far column F at the far edge defers. */
	const w_coord_t fx = MAX_VIEW_DISTANCE + 1, fz = 0;
	const w_coord_t sx = fx + 1, sz = 1; /* inside F's 3x3 support */
	server_world_test_fail_gen_attempts(1);
	struct server_chunk* c = NULL;
	assert(!server_world_ensure_populated_far(&s->world, s, fx, fz, &c));
	server_world_test_fail_gen_attempts(0);
	assert(s->world.gen_pending[1] && s->world.gen_pending_x[1] == fx
		   && s->world.gen_pending_z[1] == fz);
	assert(split_populate(s, sx, sz)); /* the support column, resident, unsent */
	assert(s->world.gen_pending[1] && "populating support uses the near stream");

	/* The guest joins: dial 4, far ring off. Both dial squares fully served so
	 * the near loop is idle and the server loop reaches the far stage. */
	assert(server_local_set_anchor_sep(s, SEP_BLOCKS));
	s->residency_anchors[0] = (struct residency_anchor) {0, 0};
	s->residency_anchors[1] = (struct residency_anchor) {SEP_CHUNKS, 0};
	assert(server_local_view_radius(s) == DIAL && s->far_view_distance == DIAL);
	assert(SEP_CHUNKS - (DIAL + 1) > sx && sx > DIAL + 1
		   && "the support column must lie outside both dial squares and their +1 rings");
	const w_coord_t ax[2] = {0, SEP_CHUNKS};
	for(unsigned p = 0; p < 2; p++)
		for(int z = -DIAL; z <= DIAL; z++)
			for(int x = ax[p] - DIAL; x <= ax[p] + DIAL; x++)
				split_populate(s, x, z)->sent = true;
	assert(s->world.gen_pending[1] && "no near work resumed the far job");

	/* 1. the pin: the support column is never offered while the job hangs */
	assert(!drain_offers(s, sx, sz));
	assert(server_world_is_chunk_loaded(&s->world, sx, sz));
	assert(s->world.gen_pending[1]);
	puts("PASS an in-flight far job pins its support (no evict/regenerate livelock)");

	/* 2. the fix: the server loop finishes the job under split, then the
	 *    support column is evicted like anything else outside the keep union */
	s->player.finished_loading = true;
	struct thread consumer;
	atomic_store(&run_drain, true);
	assert(thread_create(&consumer, drain_rpc, NULL, 64));
	unsigned ticks = 0;
	while(s->world.gen_pending[1] && ticks < 400) {
		server_local_test_update_once(s);
		ticks++;
	}
	atomic_store(&run_drain, false);
	thread_join(&consumer);
	assert(!s->world.gen_pending[1] && "the in-flight far job never finished under split");
	assert(s->far_view_distance == DIAL && "no far work restarted the ring");
	assert(!server_world_is_far_sent(&s->world, fx, fz) && "finished, never far-sent");
	if(server_world_is_chunk_loaded(&s->world, sx, sz))
		assert(drain_offers(s, sx, sz));
	assert(!server_world_is_chunk_loaded(&s->world, sx, sz));
	printf("PASS the in-flight far job finished in %u server iterations under split; "
		   "its support pin lifted and the column was evicted\n", ticks);

	split_destroy(s);
	return 0;
}
