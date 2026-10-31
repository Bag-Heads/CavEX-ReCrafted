/* Copyright (c) 2026 CaveWii_v2 contributors
 * A1c (split-dimensions-plan-0924, PLAN-v3 s.2 "Residency anchors are
 * (dimension,x,z)"): residency anchors, the two-anchor debt scheduler's rows
 * and the near-unload send are world-qualified.
 *
 *  1. An anchor carries its dimension. residency_contains_in refuses the
 *     same (x,z) in another dimension; the legacy dimension-blind
 *     residency_contains keeps its meaning for the unconverted callers
 *     (server_world.c's furthest-chunk scan, carve_observer).
 *  2. Debt rows are keyed (dimension,x,z): the same column coordinate in two
 *     dimensions is two rows; a lookup in the other dimension misses.
 *  3. The scheduler refresh only serves anchors whose dimension has a live
 *     world: with both anchors in the Overworld every row is Overworld and
 *     both owners are accounted; moving anchor 1 to a dimension with no
 *     live world leaves it with no rows, no owed and no oldest.
 *  4. Single-world sent-state/retry: rows created under (Overworld) anchors
 *     are exactly the rows the pre-A1c scheduler created (the anchor
 *     dimension is slot 0's) -- pinned by test_split_residency_* goldens;
 *     here the row shape (attempted/retry seq stamps) is checked directly.
 *  5. The near-unload send is per world: a world with no client route sends
 *     nothing and reports "sent" (nothing to unload on any client); slot 0
 *     publishes CRPC_UNLOAD_CHUNK.
 *
 * RED by construction until SERVER_LOCAL_A1C_WORLD_OPS exists. */
#include "support/split_residency_fixture.h"

#include <inttypes.h>
#include <stdio.h>

#ifndef SERVER_LOCAL_A1C_WORLD_OPS
int main(void) {
	puts("RED: SERVER_LOCAL_A1C_WORLD_OPS absent -- residency anchors/rows are "
		 "not world-qualified yet");
	return 1;
}
#else

static unsigned long unload_sends, other_sends;
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_UNLOAD_CHUNK)
		unload_sends++;
	else
		other_sends++;
	clin_drain_discard();
}

static struct server_world* make_slot1(struct server_local* s,
									   enum world_dim dim) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, dim, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

static unsigned rows_with(struct server_local* s, int32_t dimension,
						  uint8_t owner_bit, unsigned* total) {
	unsigned n = 0, all = 0;
	for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
		for(unsigned i = 0; i < RESIDENCY_ANCHORS; i++) {
			const struct residency_debt* d = &s->residency->debt[b][i];
			if(!d->used)
				continue;
			all++;
			if(d->dimension == dimension
			   && ((d->near_owners | d->far_owners) & owner_bit))
				n++;
		}
	if(total)
		*total = all;
	return n;
}

int main(void) {
	assert(clin_init());
	/* ---- 1. anchors carry their dimension ---- */
	{
		struct residency_anchor a[2] = {{0, 0}, {64, 64}}; /* {x,z}: Overworld */
		assert(a[0].dimension == WORLD_DIM_OVERWORLD
			   && a[1].dimension == WORLD_DIM_OVERWORLD);
		assert(residency_contains(a, 2, 4, 2, -3));
		assert(residency_contains_in(a, 2, 4, WORLD_DIM_OVERWORLD, 2, -3));
		assert(!residency_contains_in(a, 2, 4, WORLD_DIM_NETHER, 2, -3));
		struct residency_anchor n = {2, -3, WORLD_DIM_NETHER};
		assert(residency_contains(&n, 1, 0, 2, -3)); /* legacy: dimension-blind */
		assert(residency_contains_in(&n, 1, 0, WORLD_DIM_NETHER, 2, -3));
		assert(!residency_contains_in(&n, 1, 0, WORLD_DIM_OVERWORLD, 2, -3));
		assert(!residency_contains_in(a, 2, 4, WORLD_DIM_OVERWORLD, 5, 0));
		puts("PASS anchor dimension: same (x,z) in another dimension is outside");
	}

	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	assert(server_local_set_anchor_sep(s, 64)); /* allocates the scheduler */
	assert(server_local_two_anchor(s) && s->residency);
	s->player.finished_loading = true;
	s->player.x = 0.5;
	s->player.z = 0.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;

	/* ---- 2. debt rows are keyed (dimension,x,z) ---- */
	{
		struct residency_debt* o = server_local_test_residency_debt_find(
			s, WORLD_DIM_OVERWORLD, 5, 5, true);
		assert(o && o->used && o->dimension == WORLD_DIM_OVERWORLD && o->x == 5
			   && o->z == 5);
		assert(!server_local_test_residency_debt_find(s, WORLD_DIM_NETHER, 5, 5,
													  false));
		struct residency_debt* n = server_local_test_residency_debt_find(
			s, WORLD_DIM_NETHER, 5, 5, true);
		assert(n && n != o && n->dimension == WORLD_DIM_NETHER && n->x == 5
			   && n->z == 5);
		assert(server_local_test_residency_debt_find(s, WORLD_DIM_OVERWORLD, 5,
													 5, false)
			   == o);
		assert(server_local_test_residency_debt_find(s, WORLD_DIM_NETHER, 5, 5,
													 false)
			   == n);
		/* Row shape (4): a fresh row is stamped with the current send seq for
		 * first_wait and retry, unattempted. */
		assert(o->first_wait == s->near_send_seq && o->retry == s->near_send_seq
			   && !o->attempted_once && o->attempted == 0);
		puts("PASS debt keying: (dimension,x,z) -- one column, two dimensions, "
			 "two rows");
		/* Reset for the refresh below (the refresh owns the table). */
		memset(s->residency, 0, sizeof(*s->residency));
	}

	/* ---- 3. the refresh serves anchors by dimension ---- */
	{
		s->residency_anchors[0] = (struct residency_anchor) {0, 0, a->dimension};
		s->residency_anchors[1] = (struct residency_anchor) {4, 0, a->dimension};
		s->input_epoch_counter++;
		server_local_residency_refresh(s);
		unsigned total = 0;
		unsigned p0 = rows_with(s, WORLD_DIM_OVERWORLD, 1u << 0, &total);
		unsigned p1 = rows_with(s, WORLD_DIM_OVERWORLD, 1u << 1, NULL);
		assert(total > 0 && p0 > 0 && p1 > 0);
		assert(rows_with(s, WORLD_DIM_NETHER, 0xFF, NULL) == 0);
		/* Every used row is an Overworld row. */
		for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
			for(unsigned i = 0; i < RESIDENCY_ANCHORS; i++) {
				const struct residency_debt* d = &s->residency->debt[b][i];
				if(d->used)
					assert(d->dimension == WORLD_DIM_OVERWORLD);
			}
		assert(s->anchor_near_owed[0] > 0 && s->anchor_near_owed[1] > 0);
		printf("PASS refresh (Overworld x2): %u rows, owner0 %u owner1 %u\n",
			   total, p0, p1);

		/* Anchor 1 moves to a dimension with NO live world: it owns nothing. */
		s->residency_anchors[1]
			= (struct residency_anchor) {4, 0, WORLD_DIM_NETHER};
		server_local_residency_refresh(s);
		assert(rows_with(s, WORLD_DIM_NETHER, 0xFF, NULL) == 0);
		assert(rows_with(s, WORLD_DIM_OVERWORLD, 1u << 1, NULL) == 0);
		assert(rows_with(s, WORLD_DIM_OVERWORLD, 1u << 0, NULL) > 0);
		assert(s->anchor_near_owed[1] == 0 && s->anchor_oldest[1] == 0);
		assert(s->anchor_near_owed[0] > 0);
		/* The pick for owner 1 finds nothing; owner 0 still has work. */
		int32_t px = 0, pz = 0;
		assert(server_local_residency_pick(s, false, &px, &pz));
		puts("PASS refresh (anchor 1 in an unserved dimension): no rows, no owed, "
			 "no oldest for owner 1; owner 0 unaffected");

		/* A LIVE Nether world in slot 1 serves that anchor: its rows are
		 * Nether rows at the same coordinates as the Overworld rows. */
		struct server_world* b = make_slot1(s, WORLD_DIM_NETHER);
		assert(server_local_world_for_dimension(s, WORLD_DIM_NETHER) == b);
		assert(server_local_world_for_dimension(s, WORLD_DIM_OVERWORLD) == a);
		server_local_residency_refresh(s);
		unsigned nether_rows = rows_with(s, WORLD_DIM_NETHER, 1u << 1, NULL);
		assert(nether_rows > 0);
		assert(rows_with(s, WORLD_DIM_OVERWORLD, 1u << 1, NULL) == 0);
		assert(server_local_test_residency_debt_find(s, WORLD_DIM_NETHER, 4, 0,
													 false));
		assert(server_local_test_residency_debt_find(s, WORLD_DIM_OVERWORLD, 0,
													 0, false));
		assert(s->anchor_near_owed[1] > 0);
		printf("PASS refresh (live Nether slot 1): %u Nether rows for owner 1, "
			   "Overworld rows untouched\n",
			   nether_rows);

		/* G8: the primary pick API returns only x/z. It must never hand
		 * secondary-world debt to the primary-world publisher. Keep that
		 * debt recorded for truthful per-owner readiness diagnostics. */
		assert(clin_world_payloads_set_enabled(true));
		for(unsigned bucket = 0; bucket < RESIDENCY_BUCKETS; bucket++)
			for(unsigned i = 0; i < RESIDENCY_ANCHORS; i++) {
				struct residency_debt* d = &s->residency->debt[bucket][i];
				if(d->dimension == WORLD_DIM_OVERWORLD)
					d->used = false;
			}
		assert(rows_with(s, WORLD_DIM_NETHER, 2, NULL) > 0);
		assert(!server_local_residency_pick(s, false, &px, &pz));
		puts("PASS primary picker refuses secondary debt without erasing it");

		/* A same-coordinate anchor in Nether cannot protect an unrelated
		 * Overworld column from eviction. Leave the near primary core safe. */
		memset(s->residency, 0, sizeof(*s->residency));
		s->residency_anchors[0] = (struct residency_anchor){0, 0, WORLD_DIM_OVERWORLD};
		s->residency_anchors[1] = (struct residency_anchor){20, 0, WORLD_DIM_NETHER};
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(a, 0, 0, &loaded));
		assert(server_world_load_chunk(a, 20, 0, &loaded));
		assert(server_world_furthest_chunk_anchors(a, 4, 0, 0, -1,
			s->residency_anchors, 2, &px, &pz));
		assert(px == 20 && pz == 0);
		assert(clin_world_payloads_set_enabled(false));
		assert(!server_world_furthest_chunk_anchors(a, 4, 0, 0, -1,
			s->residency_anchors, 2, &px, &pz));
		puts("PASS eviction keeps only same-world anchors; legacy behavior preserved");
		assert(clin_world_payloads_set_enabled(true));
		assert(server_world_load_chunk(a, -30, 0, &loaded));
		s->residency_anchors[0] = (struct residency_anchor){-100, 0, WORLD_DIM_NETHER};
		s->residency_anchors[1] = (struct residency_anchor){0, 0, WORLD_DIM_OVERWORLD};
		assert(server_world_furthest_chunk_anchors(a, 4, 0, 0, -1,
			s->residency_anchors, 2, &px, &pz));
		assert(px == -30 && pz == 0);
		puts("PASS host-away eviction ranks distance from the primary-world actor");

		/* Beta World.updateBlocksAndPlayCaveSounds builds its active set
		 * from this.playerEntities, never a player in another world.
		 * Put the supplied first-actor center outside every loaded column,
		 * leaving only the optional guest square eligible for RNG draws. */
		struct server_player guest = {0};
		s->guest_gameplay = true;
		s->guest_actor = &guest;
		s->residency_anchors[1] = (struct residency_anchor){20, 0, WORLD_DIM_NETHER};
		struct random_gen rng = {.seed = 12345};
		server_world_random_tick(a, &rng, s, 1000, 1000, 0);
		assert(rng.seed == 12345);
		assert(server_world_load_chunk(b, 20, 0, &loaded));
		memset(loaded->ids, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
		server_world_random_tick(b, &rng, s, 1000, 1000, 0);
		assert(rng.seed != 12345);
		s->guest_gameplay = false;
		s->guest_actor = NULL;
		assert(clin_world_payloads_set_enabled(false));
		puts("PASS random-tick guest union consumes RNG only in its owning world");

		/* ---- 5. near-unload send is per world ---- */
		clin_drain_discard();
		clin_test_set_rpc_observer(observe, NULL);
		unload_sends = other_sends = 0;
		assert(server_local_test_near_unload_send_w(b, 3, 3));
		assert(unload_sends == 0 && other_sends == 0);
		assert(server_local_test_near_unload_send_w(a, 3, 3));
		assert(unload_sends == 1 && other_sends == 0);
		struct server_world stray = {0};
		stray.world_index = 1; /* an unrouted slot, never live */
		assert(server_local_test_near_unload_send_w(&stray, 3, 3));
		assert(unload_sends == 1);
		clin_test_set_rpc_observer(NULL, NULL);
		clin_drain_discard();
		puts("PASS near-unload: unrouted world sends nothing; slot 0 publishes");

		server_world_discard(b);
		assert(!server_local_world_for_dimension(s, WORLD_DIM_NETHER));
		server_local_residency_refresh(s);
		assert(rows_with(s, WORLD_DIM_NETHER, 0xFF, NULL) == 0);
		puts("PASS discard: Nether rows released once the world is gone");
	}

	split_destroy(s);
	return 0;
}
#endif
