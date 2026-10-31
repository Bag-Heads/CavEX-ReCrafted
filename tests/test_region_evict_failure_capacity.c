/* P03 capacity-driven version; original four-slot fixture untouched.
 * All sync/close failure, retained-handle and durability assertions remain. */
/*
	Regression tests for two region-handle durability holes. Before the
	fix, server_world_chunk_region destroyed the evicted region handle with a
	BARE region_archive_destroy -- no sync first, and fclose/f_sync failure
	swallowed -- so buffered chunk writes could leave the process unflushed
	while a later server_world_sync_regions (which only walks handles still
	resident in the LRU) reported durable success. Eviction was the one way
	writes escaped every barrier, and no later barrier could ever rediscover
	the loss: the handle is gone by then.

	The patched server_world must retain an evicted handle whose sync failed.
	Destroying it makes the failed data permanently unreachable, so a later
	barrier can only forget the failure -- it cannot prove the data durable.
	The load that needed an LRU slot therefore fails closed; a later region
	barrier re-syncs the retained handle before eviction may be retried.

	The first case also proves that a brand-new region cannot be marked clean
	while its throwaway archive has never passed region_archive_sync. The second
	case drives the full LRU path with the one-shot
	REGION_ARCHIVE_TEST_FAIL_SYNC injection:
	  1. write one modified chunk in each of regions 0..capacity-1 -- fills the
	     configured region pool exactly;
	  2. arm the one-shot sync failure;
	  3. touch the first region beyond capacity -- the load opens its .mcr through
	     server_world_chunk_region, the LRU is full, and the oldest handle's
	     eviction barrier consumes the armed failure;
	  4. assert the load failed closed and the candidate did not replace the handle;
	  5. inject another failed barrier and prove the exact LRU state remains;
	  6. assert a healthy barrier actually re-syncs every retained handle before
	     a later barrier can succeed;
	  7. retry the first region beyond capacity and prove eviction can now complete.

	FAIL-BEFORE: the old implementation destroyed the failed handle, installed
	the candidate, and returned success at step 4. Its next barrier returned false
	only because of a bit, then the following barrier returned true despite
	never obtaining positive durability evidence for the destroyed handle.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "platform/mem2.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

/* CHUNK_REGION_COORD divides by REGION_SIZE=32; all requests use z=0. */
static w_coord_t region_chunk_x(size_t region) {
	return (w_coord_t)(region * 32);
}

static void test_new_region_requires_barrier(
	string_t level, size_t region_index,
	enum region_archive_test_fail_stage failure) {
	struct server_world w;
	memset(&w, 0xA5, sizeof(w));
	server_world_create(&w, level, WORLD_DIM_OVERWORLD, 4242,
					true /* superflat: cheap, deterministic generation */);
	assert(!w.region_evict_failed && !w.region_close_failed
		   && "server_world_create must reset both durability latches");

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(
		&w, region_chunk_x(region_index), 0, &sc) && sc);
	sc->ids[0] = BLOCK_BRICKS;
	sc->modified = true;

	/* A2-S: a missing .mcr is no longer written through a throwaway per-chunk
	 * handle that syncs and closes inside the checkpoint; it is admitted into
	 * the session pool like every other region (server_world_chunk_region_
	 * create), so the checkpoint commits the record into the cached handle
	 * and the DURABILITY BARRIER is the next server_world_sync_regions. The
	 * armed one-shot failure is consumed THERE (sync) or at the handle's
	 * close (release at destroy). Exercise each independent result. */
	region_archive_test_fail_next(failure);
	assert(server_world_checkpoint_chunks(&w)
		   && "the write commits into the pooled handle (no per-chunk sync)");
	if(failure == REGION_ARCHIVE_TEST_FAIL_SYNC) {
		/* Exactly the RESIDENT-region contract every other region always had:
		 * the record is committed (the owner is clean), the failed fsync
		 * barrier is reported by sync_regions and RETRIED by the next one
		 * (no eviction was involved, so nothing latches beyond that report). */
		assert(!sc->modified);
		assert(!server_world_sync_regions(&w)
			   && "the armed barrier failure fails the sync, not the write");
		assert(server_world_sync_regions(&w)
			   && "the retried barrier is positive evidence for every handle");
		assert(!sc->modified);
	} else {
		/* FAIL_CLOSE stays armed until region_pool_release_world closes the
		 * handle at destroy; the record was synced first, so it is durable
		 * and the reopen below reads it back. */
		assert(!sc->modified);
		assert(server_world_sync_regions(&w));
	}
	server_world_destroy(&w);

	/* Cold reopen: the retry's exact marker survived through the archive. */
	struct server_world reopened = {0};
	server_world_create(&reopened, level, WORLD_DIM_OVERWORLD, 4242, true);
	struct server_chunk* loaded = NULL;
	assert(server_world_load_chunk(
		&reopened, region_chunk_x(region_index), 0, &loaded) && loaded);
	assert(loaded->ids[0] == BLOCK_BRICKS);
	server_world_destroy(&reopened);
}

int main(void) {
	test_fixture_dir("test_region_evict_failure_tmp", s_fixture_TMP_WORLD,
		sizeof s_fixture_TMP_WORLD);
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	/* Clean slate: no stale region files from a previous run. */
	for(int r = 0; r < REGION_POOL_SLOTS + 3; r++) {
		char path[64];
		snprintf(path, sizeof(path),
			test_fixture_join(TMP_WORLD, "/region/r.%d.0.mcr"), r);
		remove(path);
	}

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	test_new_region_requires_barrier(
		level, REGION_POOL_SLOTS + 1, REGION_ARCHIVE_TEST_FAIL_SYNC);
	test_new_region_requires_barrier(
		level, REGION_POOL_SLOTS + 2, REGION_ARCHIVE_TEST_FAIL_CLOSE);

	/* Pre-create capacity+1 files: read-only admission must find an existing
	 * candidate before it can reach the full-pool eviction barrier. These
	 * temporary setup archives are outside the live world, as before. */
	for(int r = 0; r <= REGION_POOL_SLOTS; r++) {
		struct region_archive ra;
		assert(region_archive_create_new(&ra, level, r, 0,
										 WORLD_DIM_OVERWORLD));
		region_archive_destroy(&ra);
	}

	struct server_world w = {0};
	server_world_create(&w, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat: cheap, deterministic generation */);

	/* ---- fill the region pool: one modified chunk per region ---- */
	for(int r = 0; r < REGION_POOL_SLOTS; r++) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&w, region_chunk_x(r), 0, &sc) && sc);
		sc->modified = true;
	}
	assert(server_world_checkpoint_chunks(&w));
	assert(server_world_region_count(&w) == REGION_POOL_SLOTS
		   && "region writes must fill the configured pool exactly");
	assert(!w.region_evict_failed);
	/* A2-S: the LRU is the session pool's donor choice (oldest unpinned). */
	struct region_archive* blocked_front
		= region_pool_oldest_donor(server_world_region_pool(&w));
	assert(blocked_front);
	w_coord_t blocked_x = blocked_front->x;
	w_coord_t blocked_z = blocked_front->z;

	/* ---- arm the one-shot sync failure, then touch the first region beyond capacity.
	 * The load opens the candidate .mcr through server_world_chunk_region; the LRU is
	 * full, so the front handle's eviction barrier consumes the armed failure.
	 * The failed path must leave that exact handle in place; nothing
	 * else on the load/checkpoint path calls region_archive_sync (writes only
	 * buffer), so the one-shot cannot be stolen by an earlier call. */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_SYNC);
	{
		struct server_chunk* sc = NULL;
		assert(!server_world_load_chunk(&w, region_chunk_x(REGION_POOL_SLOTS), 0, &sc));
		assert(!sc && "a failed eviction must fail the dependent load closed");
	}
	assert(w.region_evict_failed
		   && "the eviction's failed sync must latch the sticky flag");
	assert(server_world_region_count(&w) == REGION_POOL_SLOTS
		   && "the failed handle must remain in the full LRU");
	assert(region_pool_oldest_donor(server_world_region_pool(&w)) == blocked_front
		   && blocked_front->x == blocked_x && blocked_front->z == blocked_z
		   && "failed eviction must preserve the exact LRU front and order");
	for(size_t k = 0; k < server_world_region_count(&w); k++) {
		bool belongs = false;
		assert(!region_archive_contains(server_world_region_at(&w, k),
									region_chunk_x(REGION_POOL_SLOTS), 0, &belongs));
		assert(!belongs && "the candidate must not replace the failed handle");
	}

	/* ---- another actual sync failure cannot clear the carried state. A fully
	 * healthy barrier then re-syncs every retained handle but conservatively
	 * reports the earlier eviction once; only the following barrier succeeds. */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_SYNC);
	assert(!server_world_sync_regions(&w)
		   && "a failed retry must keep the retained handle non-durable");
	assert(w.region_evict_failed);
	assert(!server_world_sync_regions(&w)
		   && "the successful retry must still report the carried failure");
	assert(!w.region_evict_failed);
	assert(server_world_sync_regions(&w)
		   && "success now rests on positive evidence for every retained handle");

	/* With durability proven, the exact load that was blocked may now retry and
	 * perform the LRU replacement. */
	{
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&w,
			region_chunk_x(REGION_POOL_SLOTS), 0, &sc) && sc);
		sc->modified = true;
	}
	assert(server_world_checkpoint_chunks(&w));
	assert(server_world_sync_regions(&w));

	/* A close failure is different from a retryable sync failure: fclose has
	 * already consumed the old handle, so no future resident-barrier walk can
	 * obtain new close evidence for it. The world must stay fail-closed. */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_CLOSE);
	{
		/* A2-S pool + A2S-P1 review L1: the donor's sync SUCCEEDED (its data
		 * is durable) and only its fclose reported failure, so the slot is
		 * consumed-and-free: the candidate opens into it and this load
		 * proceeds; the owner's terminal latch is the evidence that stays. */
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&w,
			region_chunk_x(REGION_POOL_SLOTS + 1), 0, &sc) && sc);
		assert(w.region_close_failed);
		assert(server_world_region_count(&w) == REGION_POOL_SLOTS
			   && "the consumed handle's slot was reused within the cap");
	}
	assert(!server_world_sync_regions(&w));
	assert(!server_world_sync_regions(&w)
		   && "a lost-handle close failure must not clear on observation");
	assert(!server_world_sync_regions(&w)
		   && "all later durability barriers must remain fail-closed");

	server_world_destroy(&w);
	string_clear(level);
	printf("test_region_evict_failure: OK (failed eviction retained until a "
		   "later barrier proves it durable)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
