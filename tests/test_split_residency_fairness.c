/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "chunk.h"
#include "game/game_state.h"
#include "network/region_archive.h"
#include "support/split_residency_fixture.h"
#include <stdatomic.h>
static unsigned count_debt(struct server_local* s) {
	unsigned n = 0;
	for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
		for(unsigned i = 0; i < 2; i++)
			n += s->residency->debt[b][i].used;
	return n;
}
static void refresh(struct server_local* s, int x0, int z0, int x1, int z1) {
	s->near_send_seq++;
	s->residency_anchors[0] = (struct residency_anchor) {x0, z0};
	s->residency_anchors[1] = (struct residency_anchor) {x1, z1};
	server_local_residency_refresh(s);
}
static atomic_bool run_drain = ATOMIC_VAR_INIT(false);
static unsigned delivered[2];
static void record_send(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	clin_drain_discard();
	if(rpc->type == CRPC_CHUNK)
		delivered[rpc->payload.chunk.x >= 512]++;
}
static void* drain(void* unused) {
	(void)unused;
	while(atomic_load(&run_drain)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static struct residency_debt* find_debt(struct server_local* s, int x, int z) {
	for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
		for(unsigned i = 0; i < 2; i++) {
			struct residency_debt* d = &s->residency->debt[b][i];
			if(d->used && d->x == x && d->z == z)
				return d;
		}
	return NULL;
}
/* Alternate deferred near cascades: gen_pending[0] can represent only one. */
static void test_unfinished_dependencies_and_age(void) {
	struct server_local* s = split_fixture();
	assert(server_local_set_anchor_sep(s, 1024));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	/* Support columns' COORDINATES, not their chunk pointers: dict_server_chunks
	 * is m-lib's open-addressing DICT_DEF2 (source/network/server_world.h), which
	 * rehashes -- reallocate + memcpy every live entry into a new array, then
	 * free the old one -- whenever an insert/erase crosses its load-factor
	 * threshold. A `struct server_chunk*` obtained before such a rehash dangles
	 * afterward. This loop runs 1220 ticks of server_local_residency_refresh()/
	 * refresh(), which mutates that same dict, so caching the pointer here and
	 * reusing it for the whole loop was a genuine use-after-free: quiet on an
	 * idle box (the freed block often still holds its old bytes), a segfault
	 * dereferencing a garbage sc->ids once something else's allocation reused
	 * the freed slot -- exactly the `ctest -jN` flake this was audited for.
	 * Re-look-up the chunk by id on every use instead of caching the pointer. */
	const int support_x[2] = {1, 65};
	const int support_z = 1;
	assert(split_populate(s, support_x[0], support_z));
	assert(split_populate(s, support_x[1], support_z));
	refresh(s, 0, 0, 64, 0);
	uint32_t first = s->near_send_seq;
	struct far_rpc_slot* slot = clin_far_slot_try_acquire();
	assert(slot);
	for(unsigned tick = 0; tick < 1220; tick++) {
		for(unsigned p = 0; p < 2; p++) {
			struct residency_debt* d = find_debt(s, p * 64, 0);
			assert(d && d->first_wait == first);
			d->attempted_once = true;
			d->attempted = s->near_send_seq;
			d->retry = s->near_send_seq + 20;
			s->world.gen_pending[0] = true;
			s->world.gen_pending_x[0] = p * 64;
			s->world.gen_pending_z[0] = 0;
			unsigned other = 1 - p;
			/* From the second alternation on, both cascades are unfinished. */
			if(tick || p) {
				struct server_chunk* support = dict_server_chunks_get(
					s->world.chunks, S_CHUNK_ID(support_x[other], support_z));
				assert(support);
				assert(server_local_test_finish_far_column(s, other * 64 + 1, 1,
					 support, slot));
				assert(server_world_is_chunk_loaded(&s->world, other * 64 + 1, 1));
			}
		}
		server_local_residency_refresh(s); /* post-service, ineligible debt */
		assert(s->anchor_oldest[0] == tick && s->anchor_oldest[1] == tick);
		refresh(s, 0, 0, 64, 0);
	}
	assert(s->anchor_oldest[0] > 1200);
	clin_far_slot_release(slot);
	split_destroy(s);
	puts("PASS alternating unfinished cascades retain support; failed debt age grows past 1200");
}
/* P0 permanently fails generation. P1's completed near set may still stream
 * far columns, including recovery after a sustained exhausted far-slot pool. */
static void test_far_failure_isolation(void) {
	struct server_local* s = split_fixture();
	assert(server_local_set_anchor_sep(s, 1024));
	s->far_send_near_owed = 8;
	s->far_lod_terrain_only = true;
	s->render_demo_mob = 0;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	for(int z = -MAX_VIEW_DISTANCE; z <= MAX_VIEW_DISTANCE; z++)
		for(int x = 64-MAX_VIEW_DISTANCE; x <= 64+MAX_VIEW_DISTANCE; x++)
			split_populate(s, x, z)->sent = true;
	/* Keep the real player's admission collision-safe while P0's frontier fails. */
	split_populate(s, 0, 0)->sent = true;
	s->player.finished_loading = true;
	struct far_rpc_slot* slots[64];
	unsigned held = 0;
	while((slots[held] = clin_far_slot_try_acquire()) != NULL) {
		held++;
		assert(held < 64);
	}
	struct thread consumer;
	atomic_store(&run_drain, true);
	assert(thread_create(&consumer, drain, NULL, 64));
	clin_test_set_rpc_observer(record_send, NULL);
	for(unsigned tick = 0; tick < 1240; tick++) {
		if(tick == 80)
			while(held) clin_far_slot_release(slots[--held]);
		server_world_test_fail_gen_attempts(1000000);
		server_local_test_update_once(s);
		assert(s->anchor_near_owed[0] > 8 && s->anchor_near_owed[1] == 0);
		if(tick < 80)
			assert(!server_world_is_far_sent(&s->world, 64, MAX_VIEW_DISTANCE+1));
	}
	atomic_store(&run_drain, false);
	thread_join(&consumer);
	unsigned far_delivered = 0;
	for(int z = -s->far_view_distance; z <= s->far_view_distance; z++)
		for(int x = 64-s->far_view_distance; x <= 64+s->far_view_distance; x++)
			far_delivered += server_world_is_far_sent(&s->world, x, z);
	assert(far_delivered > 0);
	assert(s->anchor_oldest[0] > 1200 && s->anchor_oldest[1] == 0);
	assert(!server_world_is_far_sent(&s->world, 0, MAX_VIEW_DISTANCE+1));
	clin_test_set_rpc_observer(NULL, NULL);
	server_world_test_fail_gen_attempts(0);
	split_destroy(s);
	puts("PASS permanent P0 failure isolates P1 far service; 80 exhausted + 1160 recovered ticks");
}
/* MAINTAINER RULING 2026-09-11: runtime split server load radius. With two
 * anchors the residency proofs must follow split.server_view_radius, not the
 * compile-time MAX_VIEW_DISTANCE: the near debt a view owes is the dial square,
 * and far service still refuses every column inside either dial square. */
static void test_dial_radius(void) {
	struct server_local* s = split_fixture();
	s->split_view_radius = 4;
	assert(server_local_set_anchor_sep(s, 1024));
	assert(server_local_view_radius(s) == 4);
	/* DIAL4_FAR_RING_OFF_PINNED: the setter applied the split policy (9561a42d) --
	 * while the dial is active the far ring is OFF (effective far distance =
	 * the dial; the configured value is remembered for the leave). */
	assert(s->far_view_distance == 4
		   && s->far_view_configured == MAX_VIEW_DISTANCE + 1);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_world_test_fail_gen_attempts(0);
	/* Identical footprints deduplicate, exactly as the full-radius near_side
	 * assertion in main(): one populated + sent centre discharges both views. */
	refresh(s, 0, 0, 0, 0);
	struct server_chunk* c = split_populate(s, 0, 0);
	c->sent = true;
	refresh(s, 0, 0, 0, 0);
	unsigned dial_side = 2 * 4 + 1;
	assert(s->anchor_near_owed[0] == dial_side * dial_side - 1);
	assert(s->anchor_near_owed[1] == s->anchor_near_owed[0]);
	/* With both dial squares sent, near_owed is 0 and the far share would be
	 * open -- and the far picker must still have NOTHING: the ring is off,
	 * so no far claim can be created or age (stg25 root cause: far debt the
	 * reach never streams, anchor_oldest climbing 526 -> 6,973). */
	const int32_t anchor_x[2] = {0, 16};
	for(unsigned p = 0; p < 2; p++)
		for(int z = -4; z <= 4; z++)
			for(int x = anchor_x[p] - 4; x <= anchor_x[p] + 4; x++)
				split_populate(s, x, z)->sent = true;
	refresh(s, anchor_x[0], 0, anchor_x[1], 0);
	assert(s->anchor_near_owed[0] == 0 && s->anchor_near_owed[1] == 0);
	int32_t x, z;
	for(int n = 0; n < 40; n++)
		assert(!server_local_residency_pick(s, true, &x, &z)
			   && "far ring off under the dial: no far requirement exists");
	/* Leaving two-anchor mode restores the configured ring and the constant. */
	assert(server_local_set_anchor_sep(s, 0));
	assert(s->far_view_distance == MAX_VIEW_DISTANCE + 1
		   && server_local_view_radius(s) == MAX_VIEW_DISTANCE);
	split_destroy(s);
	puts("PASS dial 4 two-anchor near-owed follows the runtime radius; far "
		 "ring off while split, restored on leave");
}
int main(void) {
	assert(clin_init());
	blocks_init();
	test_unfinished_dependencies_and_age();
	test_far_failure_isolation();
	struct server_local* s = split_fixture();
	assert(server_local_set_anchor_sep(s, 96));
	refresh(s, 0, 0, 0, 0);
	unsigned side = 2 * s->far_view_distance + 1;
	assert(count_debt(s) == side * side); /* identical footprints deduplicate */
	int32_t x, z;
	assert(server_local_residency_pick(s, false, &x, &z) && x == 0 && z == 0);
	assert(server_local_residency_pick(s, false, &x, &z)
		   && !(x == 0 && z == 0));
	/* Success installs one shared owner, discharging both views on refresh. */
	server_world_test_set_gen_elapsed_mode(
		SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_chunk* c = split_populate(s, 0, 0);
	c->sent = true;
	refresh(s, 0, 0, 0, 0);
	unsigned near_side = 2 * MAX_VIEW_DISTANCE + 1;
	assert(s->anchor_near_owed[0] == near_side * near_side - 1);
	assert(s->anchor_near_owed[1] == s->anchor_near_owed[0]);
	/* Disjoint frontiers both receive every second eligible opportunity. */
	refresh(s, -40, -1, 40, -1);
	for(int n = 0; n < 20; n++) {
		assert(server_local_residency_pick(s, false, &x, &z));
		assert((n % 2 == 0) == (x < 0));
	}
	/* An older overlapping frontier beats newly introduced nearest debt. */
	for(int n = 0; n < 21; n++)
		refresh(s, -40, -1, 40, -1);
	refresh(s, -39, -1, 41, -1);
	assert(server_local_residency_pick(s, false, &x, &z));
	assert(x <= -30); /* old P0 work, never the newly introduced +edge */
	assert(s->anchor_oldest[0] >= 20 && s->anchor_oldest[1] >= 20);
	/* Far service must refuse every full requirement in either footprint. */
	refresh(s, 0, 0, MAX_VIEW_DISTANCE + 1, 0);
	for(int n = 0; n < 40; n++)
		if(server_local_residency_pick(s, true, &x, &z))
			assert(!residency_contains(s->residency_anchors, 2,
									   MAX_VIEW_DISTANCE, x, z));
	/* A far debt promoted into near retains its original wait timestamp. */
	refresh(s, 0, 0, 0, 0);
	struct residency_debt* promotion = find_debt(s, MAX_VIEW_DISTANCE + 1, 0);
	assert(promotion && !promotion->near_owners);
	uint32_t first = promotion->first_wait;
	refresh(s, 1, 0, 1, 0);
	promotion = find_debt(s, MAX_VIEW_DISTANCE + 1, 0);
	assert(promotion && promotion->near_owners == 3
		   && promotion->first_wait == first);
	/* Force an old edge debt against a fresh nearest candidate. */
	for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
		for(unsigned i = 0; i < 2; i++)
			s->residency->debt[b][i].first_wait = s->near_send_seq;
	promotion->first_wait = s->near_send_seq - RESIDENCY_AGE_TICKS;
	assert(server_local_residency_pick(s, false, &x, &z));
	assert(x == MAX_VIEW_DISTANCE + 1 && z == 0);
	/* Installed center is protected by either anchor; external pending
	 * population support remains protected too. */
	refresh(s, 0, 0, 40, 0);
	c = split_populate(s, 40, 0);
	c->sent = true;
	int32_t ex, ez;
	while(server_world_furthest_chunk_anchors(&s->world, MAX_VIEW_DISTANCE, 0,
											  0, -1, s->residency_anchors, 2,
											  &ex, &ez)) {
		assert(!(ex == 0 && ez == 0) && !(ex == 40 && ez == 0));
		/* Querying does not erase an owner: a failed eviction remains eligible.
		 */
		int32_t again_x, again_z;
		assert(server_world_furthest_chunk_anchors(
			&s->world, MAX_VIEW_DISTANCE, 0, 0, -1, s->residency_anchors, 2,
			&again_x, &again_z));
		assert(ex == again_x && ez == again_z);
		break;
	}
	/* Failed real persistence must retain the exact installed column. */
	c = split_populate(s, 80, 80);
	c->modified = true;
	uint8_t before = c->ids[0];
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	assert(!server_world_save_chunk(&s->world, true, 80, 80));
	assert(server_world_is_chunk_loaded(&s->world, 80, 80));
	assert(c->ids[0] == before);
	assert(server_world_save_chunk(&s->world, true, 80, 80));
	assert(!server_world_is_chunk_loaded(&s->world, 80, 80));
	/* Client collision, full-detail-wins LOD and a held worker/GPU reference
	 * use the same section and chunk lifetime as ordinary streaming. */
	struct world client;
	world_create(&client);
	c = split_populate(s, 40, 0);
	static const uint8_t tint[256] = {0};
	assert(world_build_full_section(&client, 640, 0, c->ids, c->metadata,
									c->lighting_sky, c->lighting_torch,
									c->heightmap, tint, tint));
	world_set_anchor_sep(640);
	int missing[2], pending[2], far[2], far_pending[2];
	bool collision;
	for(int y = 0; y < WORLD_HEIGHT; y += CHUNK_SIZE)
		world_find_chunk(&client, 640, y, 0)->lod = 1;
	world_anchor_status(&client, 0, 70, 0, 0, 1, missing, pending, far,
		far_pending, &collision);
	assert(missing[0] == 1 && missing[1] == 0 && pending[1] == COLUMN_HEIGHT);
	assert(far[1] == 8);
	struct world_section* section = dict_wsection_get(client.sections, SECTION_TO_ID(40, 0));
	struct chunk* saved_chunk = section->column[0];
	section->column[0] = NULL;
	world_anchor_status(&client, 0, 70, 0, 0, 1, missing, pending, far,
		far_pending, &collision);
	assert(missing[1] == 1); /* a partial section is not a full column */
	section->column[0] = saved_chunk;
	world_anchor_status(&client, -16, 70, 0, 0, 1, missing, pending, far,
		far_pending, &collision);
	assert(missing[1] == 1 && far[1] == 7 && far_pending[1] == COLUMN_HEIGHT);
	puts("PASS installed anchor observation includes band-2 and far mesh jobs");
	gstate.players[0].camera.x = 0;
	gstate.players[0].camera.z = 0;
	world_update_lod(&client);
	struct chunk* held = world_find_chunk(&client, 640, 0, 0);
	assert(held && held->lod == 0);
	/* Split render reach (T2): a second column 40 chunks past the anchor is
	 * dormant (lod 2, no lists, never offered, not counted as pending); it
	 * comes back the moment a camera is within reach; reach off restores. */
	c = split_populate(s, 80, 0);
	assert(world_build_full_section(&client, 1280, 0, c->ids, c->metadata,
									c->lighting_sky, c->lighting_torch,
									c->heightmap, tint, tint));
	world_set_split_render_reach(4);
	world_update_lod(&client);
	struct chunk* far_c = world_find_chunk(&client, 1280, 0, 0);
	assert(far_c && far_c->lod == 2 && !far_c->rebuild_displist);
	assert(!chunk_check_built(far_c) && "a dormant chunk is never offered");
	assert(held->lod == 0 && "within reach of the anchor: unchanged");
	/* Readiness disk wide enough (server radius 40) to reach the dormant
	 * column from the anchor: only the anchor's own (unmeshed) column counts
	 * as pending work; the dormant one is a policy exclusion. */
	world_anchor_status(&client, 0, 70, 0, 40, 1, missing, pending, far,
		far_pending, &collision);
	assert(pending[1] == COLUMN_HEIGHT && "dormant chunks are not pending work");
	world_set_anchor_sep(1280); /* the anchor now stands on the far column */
	world_update_lod(&client);
	assert(far_c->lod == 0 && far_c->rebuild_displist
		   && "back within reach: full tier, rebuild requested");
	world_set_split_render_reach(0);
	world_set_anchor_sep(640);
	world_update_lod(&client);
	assert(far_c->lod == 1 && "reach off: ordinary skin tier again");
	world_anchor_status(&client, 0, 70, 0, 40, 1, missing, pending, far,
		far_pending, &collision);
	assert(pending[1] == 2 * COLUMN_HEIGHT
		   && "restored: the same column is pending work again");
	world_unload_section(&client, 80, 0);
	puts("PASS split render reach: dormant beyond reach, restored within, "
		 "excluded from readiness");
	struct AABB solid = {640, 0, 0, 641, 1, 1};
	assert(world_aabb_intersection(&client, &solid));
	chunk_ref(held);
	world_unload_section(&client, 40, 0);
	assert(!world_find_chunk(&client, 640, 0, 0) && held->reference_count == 1);
	chunk_unref(held);
	world_destroy(&client);
	world_set_anchor_sep(0);
	/* Negative modulo buckets and the full radius cap retain all union work. */
	s->far_view_distance = 16;
	refresh(s, -1000, -1000, 1000, 1000);
	assert(count_debt(s) == 2 * RESIDENCY_BUCKETS);
	/* Sequence zero is a valid tick after wrap, not an unattempted sentinel. */
	s->input_epoch_counter++;
	s->near_send_seq = UINT32_MAX - 1;
	refresh(s, -1000, -1000, 1000, 1000);
	assert(server_local_residency_pick(s, false, &x, &z));
	refresh(s, -1000, -1000, 1000, 1000);
	assert(s->near_send_seq == 0);
	assert(server_local_residency_pick(s, false, &x, &z));
	assert(server_local_residency_dependency(s, x, z));
	/* Exercise the actual bounded send loop with disjoint frontiers and
	 * injected generation failures, rather than only the selection seam. */
	assert(server_local_set_anchor_sep(s, 1024));
	s->far_view_distance = MAX_VIEW_DISTANCE + 1;
	s->input_epoch_counter++;
	server_world_test_fail_gen_attempts(3);
	clin_test_set_rpc_observer(record_send, NULL);
	struct thread consumer;
	atomic_store(&run_drain, true);
	assert(thread_create(&consumer, drain, NULL, 64));
	for(unsigned tick = 0; tick < 80 && (!delivered[0] || !delivered[1]);
		tick++)
		server_local_test_update_once(s);
	atomic_store(&run_drain, false);
	thread_join(&consumer);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(delivered[0] && delivered[1]);
	split_destroy(s);
	puts("PASS overlap, deduplication, round-robin, aging, full-detail "
		 "promotion, eviction protection");
	test_dial_radius();
	return 0;
}
