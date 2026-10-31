/* Copyright (c) 2026 CaveWii_v2 contributors */
/* MAINTAINER RULING 2026-09-11 (runtime split server load radius), Astra round
 * 6 question B / round 7 C: the join / leave transitions, pinned at the
 * eviction-selector level server_local.c uses (server_world_furthest_chunk[_anchors]
 * called with server_local_view_radius(s)) on a real server_world:
 *  1. one anchor protects the full MAX_VIEW_DISTANCE square around the player
 *     and offers everything beyond it (the second player's columns included);
 *  2. the second anchor appearing (server_local_set_anchor_sep) applies the
 *     split policy by itself -- far ring off, radius = the dial -- and the
 *     columns between the dial and MAX_VIEW_DISTANCE of EITHER anchor become
 *     evictable while everything inside either dial square stays protected;
 *  3. the second anchor leaving restores the configured far distance and the
 *     full radius: a re-loaded outer column is protected again (it comes back
 *     through the unchanged single-anchor near loop, never eagerly);
 *  4. a guest body reaches the same state through the same policy call the
 *     admission path makes, and leaves it the same way.
 * Every drain checks the FULL resident set (round 7 C), not only the named
 * sentinels: every offered coordinate lies outside the union of the active
 * keep squares, every initially resident coordinate inside that union
 * survives, and -- there is no pending work here -- every coordinate outside
 * it disappears. Sentinels sit on both axes and on corners. The dial is set
 * explicitly: the residency fixtures run at 0 (= no policy). The in-flight
 * far-job transition is a separate test (test_split_view_radius_pending_far). */
#include "chunk.h"
#include "game/game_state.h"
#include "network/region_archive.h"
#include "support/split_residency_fixture.h"

#define DIAL 4
#define SEP_CHUNKS 24 /* 384 blocks: the two FULL squares must not touch on PC (MAX_VIEW_DISTANCE 10) */
#define SEP_BLOCKS (SEP_CHUNKS * 16)

struct col {
	w_coord_t x, z;
};
/* Anchor A at (0,0), anchor B at (SEP_CHUNKS,0) -- every column inside region
 * (0,0). Per anchor: the centre, +x at the dial, +x just outside it, -x just
 * outside it, a corner at the dial and a corner just outside it. */
static const struct col A0 = {0, 0}, A4 = {DIAL, 0}, A5 = {DIAL + 1, 0},
						A5n = {-(DIAL + 1), 0}, A4c = {-DIAL, -DIAL},
						A5c = {DIAL + 1, DIAL + 1};
static const struct col B0 = {SEP_CHUNKS, 0}, B4 = {SEP_CHUNKS + DIAL, 0},
						B5 = {SEP_CHUNKS + DIAL + 1, 0},
						B5n = {SEP_CHUNKS - (DIAL + 1), 0},
						B4c = {SEP_CHUNKS + DIAL, -DIAL},
						B5c = {SEP_CHUNKS - (DIAL + 1), DIAL + 1};

static void load_col(struct server_local* s, struct col c) {
	struct server_chunk* sc = split_populate(s, c.x, c.z);
	assert(sc);
	sc->sent = true;
}
static void load_all(struct server_local* s, const struct col* cols, size_t n) {
	for(size_t i = 0; i < n; i++)
		load_col(s, cols[i]);
}

/* The union of the active keep squares, exactly as the selectors see it. */
static bool kept_by(struct server_local* s, w_coord_t x, w_coord_t z) {
	return residency_contains(s->residency_anchors,
							  server_local_two_anchor(s) ? 2 : 1,
							  server_local_view_radius(s), x, z);
}

/* Mirror of server_local.c's near-evict selection: two anchors -> the
 * _anchors selector over both residency anchors, one anchor -> the player's
 * own square; radius = the effective runtime radius; no pinned zone. */
static bool pick(struct server_local* s, w_coord_t* x, w_coord_t* z) {
	w_coord_t r = server_local_view_radius(s);
	if(server_local_two_anchor(s))
		return server_world_furthest_chunk_anchors(&s->world, r, 0, 0, -1,
												   s->residency_anchors, 2, x, z);
	return server_world_furthest_chunk(&s->world, r, 0, 0, -1,
									   s->residency_anchors[0].x,
									   s->residency_anchors[0].z, x, z);
}

static struct col snap[4096];
static size_t snap_n;
static void snapshot(struct server_local* s) {
	snap_n = 0;
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, s->world.chunks);
	while(!dict_server_chunks_end_p(it)) {
		int64_t id = dict_server_chunks_ref(it)->key;
		assert(snap_n < sizeof(snap) / sizeof(snap[0]));
		snap[snap_n++] = (struct col) {S_CHUNK_X(id), S_CHUNK_Z(id)};
		dict_server_chunks_next(it);
	}
}

/* Evict everything the selector offers, then check the whole resident set:
 * nothing offered was protected, everything protected survived, and (no
 * pending work in this test) everything unprotected is gone. The named
 * sentinels are asserted on top for readability. */
static void drain(struct server_local* s, const struct col* keep, size_t keep_n,
				  const struct col* gone, size_t gone_n) {
	snapshot(s);
	unsigned evicted = 0;
	for(;;) {
		w_coord_t x, z;
		if(!pick(s, &x, &z))
			break;
		assert(!kept_by(s, x, z) && "selector offered a protected column");
		assert(server_world_save_chunk(&s->world, true, x, z));
		assert(!server_world_is_chunk_loaded(&s->world, x, z));
		assert(++evicted < 4096 && "selector never drained");
	}
	for(size_t i = 0; i < snap_n; i++) {
		bool loaded = server_world_is_chunk_loaded(&s->world, snap[i].x, snap[i].z);
		if(kept_by(s, snap[i].x, snap[i].z))
			assert(loaded && "a column inside the keep union was evicted");
		else
			assert(!loaded && "a column outside the keep union survived");
	}
	for(size_t i = 0; i < keep_n; i++) {
		assert(kept_by(s, keep[i].x, keep[i].z));
		assert(server_world_is_chunk_loaded(&s->world, keep[i].x, keep[i].z));
	}
	for(size_t i = 0; i < gone_n; i++) {
		assert(!kept_by(s, gone[i].x, gone[i].z));
		assert(!server_world_is_chunk_loaded(&s->world, gone[i].x, gone[i].z));
	}
}

int main(void) {
	assert(clin_init());
	blocks_init();
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(
		SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	s->split_view_radius = DIAL;
	s->residency_anchors[0] = (struct residency_anchor) {A0.x, A0.z};
	s->residency_anchors[1] = (struct residency_anchor) {B0.x, B0.z};
	assert(DIAL + 1 <= MAX_VIEW_DISTANCE && "the +1 sentinels must sit inside the full square");
	assert(SEP_CHUNKS > 2 * MAX_VIEW_DISTANCE + 1 && "the two full squares must not touch");
	const int far_configured = s->far_view_distance;
	assert(far_configured == MAX_VIEW_DISTANCE + 1);
	const struct col a_all[] = {A0, A4, A5, A5n, A4c, A5c};
	const struct col b_all[] = {B0, B4, B5, B5n, B4c, B5c};

	/* 1. single player: full square around A; B's columns are simply far away */
	assert(!server_local_two_anchor(s));
	assert(server_local_view_radius(s) == MAX_VIEW_DISTANCE);
	load_all(s, a_all, 6);
	load_all(s, b_all, 6);
	drain(s, a_all, 6, b_all, 6);
	puts("PASS single player protects the full MAX_VIEW_DISTANCE square");

	/* 2. join: the setter applies the policy; the dial squares are protected,
	 *    the shell between the dial and MAX_VIEW_DISTANCE is evictable. */
	load_all(s, b_all, 6);
	assert(server_local_set_anchor_sep(s, SEP_BLOCKS));
	s->residency_anchors[0] = (struct residency_anchor) {A0.x, A0.z};
	s->residency_anchors[1] = (struct residency_anchor) {B0.x, B0.z};
	assert(server_local_two_anchor(s));
	assert(server_local_view_radius(s) == DIAL);
	assert(s->far_view_distance == DIAL && "join: far ring off (setter applied the policy)");
	assert(s->far_view_configured == far_configured);
	{
		const struct col keep[] = {A0, A4, A4c, B0, B4, B4c};
		const struct col gone[] = {A5, A5n, A5c, B5, B5n, B5c};
		drain(s, keep, 6, gone, 6);
	}
	puts("PASS join: shell between the dial and MAX_VIEW_DISTANCE evictable, dial squares kept");

	/* 3. leave: configured far distance and the full radius are back; the
	 *    outer columns, once re-served, are protected again. */
	assert(server_local_set_anchor_sep(s, 0));
	assert(!server_local_two_anchor(s));
	assert(server_local_view_radius(s) == MAX_VIEW_DISTANCE);
	assert(s->far_view_distance == far_configured && "leave: far ring restored");
	assert(s->far_view_configured == 0);
	load_col(s, A5);
	load_col(s, A5n);
	load_col(s, A5c);
	drain(s, a_all, 6, b_all, 6);
	puts("PASS leave: full radius and far ring restored, outer columns protected again");

	/* 4. a guest body: the same policy call the admission path makes */
	load_all(s, b_all, 6);
	s->guest_gameplay = true;
	server_local_split_policy_apply(s);
	s->residency_anchors[1] = (struct residency_anchor) {B0.x, B0.z};
	assert(server_local_two_anchor(s) && server_local_view_radius(s) == DIAL);
	assert(s->far_view_distance == DIAL);
	{
		const struct col keep[] = {A0, A4, A4c, B0, B4, B4c};
		const struct col gone[] = {A5, A5n, A5c, B5, B5n, B5c};
		drain(s, keep, 6, gone, 6);
	}
	s->guest_gameplay = false;
	server_local_split_policy_apply(s);
	assert(server_local_view_radius(s) == MAX_VIEW_DISTANCE);
	assert(s->far_view_distance == far_configured);
	puts("PASS guest body: same join/leave state as the synthetic anchor");

	split_destroy(s);
	return 0;
}
