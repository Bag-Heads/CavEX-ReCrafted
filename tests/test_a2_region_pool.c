/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S (split-dimensions-plan-0924, astra-a2s-design-0926 "test_a2_region_pool"):
 * the SHARED region-handle pool. Two live worlds of one session together
 * request more than REGION_POOL_SLOTS (4) regions; the number of region
 * handles actually open never exceeds four -- including missing-region
 * CREATION through the save path (the old uncached per-chunk temporary is
 * gone) -- the LRU donor is chosen across BOTH worlds (oldest unpinned,
 * ties by pool index), pinned handles are never donors (no eligible donor =
 * STORAGE_BUSY, never a fifth handle), and evicting a slot in the middle of
 * a world's vector compacts the indices while every remaining handle keeps
 * its identity (coordinates, file, chunks it contains).
 *
 * RED by construction until REGION_POOL_SHARED_ADMISSION (region_pool.h)
 * exists (exit 1, WILL_FAIL). */
#include "support/split_residency_fixture.h"

#include "block/blocks.h"
#include "network/server_world.h"

#include <inttypes.h>
#include <stdio.h>

#ifndef REGION_POOL_SHARED_ADMISSION
int main(void) {
	puts("RED: REGION_POOL_SHARED_ADMISSION absent -- region handles are still "
		 "a per-world MAX_REGIONS LRU");
	return 1;
}
#else

/* Chunk X per region: region r covers chunks [32r, 32r+31]. */
static w_coord_t rx_chunk(int r) {
	return (w_coord_t)(r * 32);
}

static struct server_world* make_slot1_nether(struct server_local* s) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	/* The Nether writes DIM-1/region/: distinct region FILES from slot 0. */
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, true);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

/* Load (generate) the chunk at (x, 0), mark it dirty, checkpoint: the region
 * is created (absent) or opened through the pool's admission. */
static void touch_region(struct server_world* w, int r) {
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(w, rx_chunk(r), 0, &sc) && sc);
	sc->ids[0] = BLOCK_BRICKS;
	sc->modified = true;
	assert(server_world_checkpoint_chunks(w));
}

static bool world_holds(struct server_world* w, int r) {
	for(size_t k = 0; k < server_world_region_count(w); k++) {
		struct region_archive* ra = server_world_region_at(w, k);
		if(ra->x == r && ra->z == 0)
			return true;
	}
	return false;
}

int main(void) {
	assert(clin_init());
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_slot1_nether(s);
	struct region_pool* pool = server_world_region_pool(a);
	assert(pool == &s->region_pool && "a session world binds the session pool");
	assert(server_world_region_pool(b) == pool && "both worlds share ONE pool");
	assert(region_pool_open_count(pool) == 0);

	/* ---- 1. world A fills the cap through missing-region CREATION ---- */
	for(int r = 0; r < 4; r++)
		touch_region(a, r);
	assert(server_world_region_count(a) == 4);
	assert(region_pool_open_count(pool) == 4);
	assert(pool->open_handles_peak == 4);
	assert(pool->evictions == 0);
	for(int r = 0; r < 4; r++)
		assert(world_holds(a, r));

	/* ---- 2. world B's first region evicts A's OLDEST (global LRU) ---- */
	struct region_archive* donor = region_pool_oldest_donor(pool);
	assert(donor && donor->x == 0 && donor->z == 0);
	touch_region(b, 0);
	assert(region_pool_open_count(pool) == 4 && pool->open_handles_peak == 4);
	assert(pool->evictions == 1);
	assert(server_world_region_count(a) == 3 && server_world_region_count(b) == 1);
	assert(!world_holds(a, 0) && world_holds(a, 1) && world_holds(a, 2)
		   && world_holds(a, 3) && world_holds(b, 0));
	/* A's chunk in the evicted region is still on disk (the donor synced). */
	assert(server_world_disk_has_chunk(a, rx_chunk(0), 0));
	assert(region_pool_open_count(pool) == 4); /* that probe re-admitted r0 */
	assert(pool->evictions == 2); /* ... by evicting the next oldest (A r1) */
	assert(!world_holds(a, 1) && world_holds(a, 0));

	/* ---- 3. touching a handle makes it young; eviction compacts the
	 * middle of A's vector and the survivors keep their identity ---- */
	/* A's vector is now [r2, r3, r0] (acquisition order); touch r2 through a
	 * region probe (a HIT: no admission, the handle just becomes young). */
	assert(server_world_disk_has_chunk(a, rx_chunk(2), 0));
	assert(pool->evictions == 2);
	donor = region_pool_oldest_donor(pool);
	assert(donor && donor->x == 3 && donor->z == 0
		   && "oldest unpinned across BOTH worlds: A's r3 (B r0 is younger)");
	touch_region(b, 1);
	assert(pool->evictions == 3);
	assert(server_world_region_count(a) == 2 && server_world_region_count(b) == 2);
	assert(world_holds(a, 2) && world_holds(a, 0) && !world_holds(a, 3));
	assert(server_world_region_at(a, 0)->x == 2
		   && server_world_region_at(a, 1)->x == 0
		   && "compaction shifts indices, keeps order and identity");
	assert(world_holds(b, 0) && world_holds(b, 1));
	assert(region_pool_open_count(pool) == 4);

	/* ---- 4. pinned handles are never donors: no eligible donor refuses
	 * with STORAGE_BUSY and opens nothing ---- */
	for(size_t k = 0; k < server_world_region_count(a); k++)
		region_pool_pin(pool, server_world_region_at(a, k));
	for(size_t k = 0; k < server_world_region_count(b); k++)
		region_pool_pin(pool, server_world_region_at(b, k));
	assert(region_pool_oldest_donor(pool) == NULL);
	{
		struct region_archive* ra = NULL;
		assert(region_pool_acquire(pool, a, 5, 0, true, &ra) == STORAGE_BUSY);
		assert(!ra && pool->busy_refusals == 1);
		assert(region_pool_open_count(pool) == 4 && pool->open_handles_peak == 4);
		/* A `!create` probe for r5 (no file yet) answers ABSENT before any
		 * admission (M1), so it is not a BUSY refusal either. */
		uint32_t probes = pool->absent_probes;
		assert(server_world_chunk_region(a, rx_chunk(5), 0) == NULL);
		assert(pool->busy_refusals == 1 && pool->absent_probes == probes + 1);
	}
	for(size_t k = 0; k < server_world_region_count(a); k++)
		region_pool_unpin(pool, server_world_region_at(a, k));
	for(size_t k = 0; k < server_world_region_count(b); k++)
		region_pool_unpin(pool, server_world_region_at(b, k));
	touch_region(a, 5); /* admitted again once a donor is eligible */
	assert(world_holds(a, 5) && pool->evictions == 4);

	/* ---- 5. the same admission serves recovery/checked opens; an ABSENT
	 * region never evicts (A2S-P1 review M1: full pool + absent load = no
	 * eviction, no admission) ---- */
	{
		uint32_t ev = pool->evictions;
		struct region_archive* ra = NULL;
		assert(server_world_chunk_region(a, rx_chunk(7), 0) == NULL);
		assert(region_pool_acquire(pool, a, 8, 0, false, &ra) == STORAGE_ABSENT
			   && !ra);
		assert(pool->evictions == ev && "no donor chosen for an absent file");
		assert(pool->absent_probes >= 2);
		assert(region_pool_open_count(pool) == 4 && pool->open_handles_peak == 4);
		assert(server_world_chunk_region_checked(a, rx_chunk(7), 0, &ra)
			   == SERVER_WORLD_REGION_ABSENT
			   && !ra && "an absent region file is reported, not created");
		assert(region_pool_open_count(pool) == 4 && pool->evictions == ev);
		assert(server_world_chunk_region_checked(a, rx_chunk(3), 0, &ra)
			   == SERVER_WORLD_REGION_OK && ra && ra->x == 3);
		assert(region_pool_open_count(pool) == 4 && pool->open_handles_peak == 4);
	}

	/* ---- 6. barriers per world, then B's teardown releases only B's ---- */
	assert(server_world_sync_regions(a));
	assert(server_world_sync_regions(b));
	size_t a_count = server_world_region_count(a);
	server_world_destroy(b);
	assert(server_world_region_count(a) == a_count);
	assert(region_pool_open_count(pool) == a_count);
	for(size_t k = 0; k < a_count; k++)
		assert(region_pool_slot_of(pool, server_world_region_at(a, k))->owner == a);

	printf("PASS a2 region pool: peak open handles %" PRIu32 " (cap %d), "
		   "evictions %" PRIu32 ", busy refusals %" PRIu32 "\n",
		   pool->open_handles_peak, REGION_POOL_SLOTS, pool->evictions,
		   pool->busy_refusals);
	assert(pool->open_handles_peak == REGION_POOL_SLOTS);
	split_destroy(s);
	return 0;
}
#endif
