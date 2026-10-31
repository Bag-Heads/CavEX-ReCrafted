/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S (astra-a2s-design-0926 "test_a2_region_donor_faults"): faults on the
 * DONOR side of the shared pool's admission. With both worlds' regions
 * competing for REGION_POOL_SLOTS handles:
 *  - a donor whose settle (sync = publish) FAILS stays resident with its
 *    dirty state and the exact LRU position; the requester is refused; the
 *    donor world's region_evict_failed latches and its next barriers carry
 *    the failure once (no false durability); no fifth handle was ever open;
 *  - a donor whose CLOSE fails after a SUCCESSFUL sync is consumed: its data
 *    is durable, the owner's region_close_failed latches for the rest of that
 *    world instance (barriers stay fail-closed), the slot is FREE again and
 *    the request proceeds into it (A2S-P1 review L1) -- still within the cap;
 *  - the requester's own open failure (an absent file without create) leaves
 *    the freed slot FREE and the donor's data durable on disk.
 * Injected through region_archive_test_fail_next (the production hooks the
 * retained test_region_evict_failure uses), never through new stubs.
 *
 * RED by construction until REGION_POOL_SHARED_ADMISSION exists. */
#include "support/split_residency_fixture.h"

#include "block/blocks.h"
#include "network/server_world.h"

#include <inttypes.h>
#include <stdio.h>

#ifndef REGION_POOL_SHARED_ADMISSION
int main(void) {
	puts("RED: REGION_POOL_SHARED_ADMISSION absent -- no shared donor admission");
	return 1;
}
#else

static w_coord_t rx_chunk(int r) {
	return (w_coord_t)(r * 32);
}

static struct server_world* make_slot1_nether(struct server_local* s) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, true);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

static void touch_region(struct server_world* w, int r) {
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(w, rx_chunk(r), 0, &sc) && sc);
	sc->ids[0] = BLOCK_BRICKS;
	sc->modified = true;
	assert(server_world_checkpoint_chunks(w));
}

static bool world_holds(struct server_world* w, int r) {
	for(size_t k = 0; k < server_world_region_count(w); k++)
		if(server_world_region_at(w, k)->x == r
		   && server_world_region_at(w, k)->z == 0)
			return true;
	return false;
}

static size_t faulted_slots(const struct region_pool* p) {
	size_t n = 0;
	for(size_t i = 0; i < REGION_POOL_SLOTS; i++)
		if(p->slot[i].state == REGION_SLOT_FAULTED)
			n++;
	return n;
}

int main(void) {
	assert(clin_init());
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_slot1_nether(s);
	struct region_pool* pool = server_world_region_pool(a);
	assert(server_world_region_pool(b) == pool);
	/* B's region FILES must exist up front: an absent region needs no handle
	 * to GENERATE a chunk (the stat short-circuits), so a load would never
	 * reach the admission whose donor fault is under test. Same setup idiom
	 * as the retained test_region_evict_failure (transient, before the pool
	 * holds anything). */
	for(int r = 0; r < 4; r++) {
		struct region_archive ra;
		assert(region_archive_create_new(&ra, b->level_name, r, 0,
										 WORLD_DIM_NETHER));
		assert(region_archive_destroy(&ra));
	}
	assert(region_pool_open_count(pool) == 0);

	for(int r = 0; r < 4; r++)
		touch_region(a, r);
	assert(region_pool_open_count(pool) == 4 && !a->region_evict_failed);
	struct region_archive* donor = region_pool_oldest_donor(pool);
	assert(donor && donor->x == 0);

	/* ---- 1. donor SYNC failure: refused, donor intact, no fifth handle ---- */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_SYNC);
	{
		struct server_chunk* sc = NULL;
		assert(!server_world_load_chunk(b, rx_chunk(0), 0, &sc) && !sc
			   && "a failed donor settle fails the dependent load closed");
	}
	assert(a->region_evict_failed && !b->region_evict_failed);
	assert(pool->evict_sync_failures == 1 && pool->evictions == 0);
	assert(region_pool_open_count(pool) == 4 && pool->open_handles_peak == 4);
	assert(server_world_region_count(a) == 4 && server_world_region_count(b) == 0);
	assert(region_pool_oldest_donor(pool) == donor && donor->x == 0
		   && "the exact donor keeps its LRU position");
	assert(region_pool_slot_of(pool, donor)->state == REGION_SLOT_READY);
	/* The dirty owner is retained: A's next barriers carry the failure once,
	 * then positive evidence for every retained handle clears it. */
	assert(!server_world_sync_regions(a) && "carried failure reported once");
	assert(!a->region_evict_failed);
	assert(server_world_sync_regions(a));
	/* B's request now succeeds through that same donor. */
	touch_region(b, 0);
	assert(pool->evictions == 1 && region_pool_open_count(pool) == 4);
	assert(!world_holds(a, 0) && world_holds(b, 0));
	assert(server_world_disk_has_chunk(a, rx_chunk(0), 0)
		   && "the evicted donor's record is durable");
	assert(region_pool_open_count(pool) == 4);

	/* ---- 2. donor CLOSE failure after a successful sync: explicit latch,
	 * the slot is reusable and this very request proceeds (L1) ---- */
	donor = region_pool_oldest_donor(pool);
	assert(donor && donor->x == 2 && "oldest: A r2 (r1 went at the probe)");
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_CLOSE);
	{
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(b, rx_chunk(1), 0, &sc) && sc
			   && "the donor's data is durable: the request proceeds");
	}
	assert(a->region_close_failed && !b->region_close_failed);
	assert(pool->close_failures == 1 && faulted_slots(pool) == 0);
	assert(region_pool_open_count(pool) == 4
		   && "the consumed handle's slot was reused by the candidate");
	assert(!world_holds(a, 2) && world_holds(b, 1));
	assert(server_world_disk_has_chunk(a, rx_chunk(2), 0)
		   && "the close-failed donor's record is on disk (sync succeeded)");
	assert(!server_world_sync_regions(a)
		   && "a lost-handle close failure never clears on observation");
	assert(!server_world_sync_regions(a));
	assert(pool->open_handles_peak == 4 && pool->close_failures == 1);

	/* ---- 3. a `!create` request for an ABSENT region answers ABSENT before
	 * any admission: nothing evicted (M1) ---- */
	donor = region_pool_oldest_donor(pool);
	assert(donor);
	w_coord_t donor_x = donor->x;
	struct server_world* donor_owner
		= region_pool_slot_of(pool, donor)->owner;
	{
		struct region_archive* ra = NULL;
		enum storage_rc rc = region_pool_acquire(pool, b, 9, 0, false, &ra);
		assert(rc == STORAGE_ABSENT && !ra);
	}
	assert(region_pool_open_count(pool) == 4 && faulted_slots(pool) == 0
		   && "an absent file needs no handle: no donor was evicted (M1)");
	assert(pool->absent_probes >= 1);
	assert(server_world_disk_has_chunk(donor_owner, (w_coord_t)(donor_x * 32), 0)
		   && "the would-be donor is untouched");

	/* ---- 4. the CANDIDATE's own open fails after a successful eviction:
	 * the slot is FREE (no phantom handle), the count is down by one, the
	 * evicted donor's record is durable on disk, and the retry succeeds ---- */
	{
		/* b's region 2 exists (pre-created) but is not resident: the request
		 * needs a donor. */
		size_t open_before = region_pool_open_count(pool);
		uint32_t ev_before = pool->evictions;
		struct region_archive* victim = region_pool_oldest_donor(pool);
		assert(victim);
		w_coord_t vx = victim->x;
		struct server_world* vowner = region_pool_slot_of(pool, victim)->owner;
		region_pool_test_fail_next_open();
		struct region_archive* ra = NULL;
		assert(region_pool_acquire(pool, b, 2, 0, true, &ra) == STORAGE_IO_ERROR
			   && !ra);
		assert(pool->evictions == ev_before + 1 && "the donor was evicted");
		assert(region_pool_open_count(pool) == open_before - 1
			   && "no phantom handle for the failed candidate");
		assert(server_world_disk_has_chunk(vowner, (w_coord_t)(vx * 32), 0)
			   && "the evicted donor's record is durable");
		assert(region_pool_acquire(pool, b, 2, 0, true, &ra) == STORAGE_OK && ra
			   && ra->x == 2 && "the retry opens into the freed slot");
		assert(region_pool_open_count(pool) <= REGION_POOL_SLOTS
			   && pool->open_handles_peak == 4);
	}
	puts("PASS 4: candidate open failure leaves the slot free and the donor durable");

	printf("PASS a2 donor faults: sync-fail %" PRIu32 " close-fail %" PRIu32
		   " tombstones %zu peak %" PRIu32 "\n",
		   pool->evict_sync_failures, pool->close_failures,
		   faulted_slots(pool), pool->open_handles_peak);
	server_world_destroy(b);
	assert(faulted_slots(pool) == 0);
	split_destroy(s);
	return 0;
}
#endif
