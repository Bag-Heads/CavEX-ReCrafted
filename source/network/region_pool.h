/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-S (split-dimensions-plan-0924, Astra astra-a2s-design-0926): the SHARED
 * region-handle pool -- ONE admission authority for every region file a
 * session opens, across both worlds.
 *
 * Before A2-S each struct server_world owned its own MAX_REGIONS=4 LRU, so two
 * live worlds could hold eight region FILE*s (+ their ~9 KB MEM1 index arrays
 * each), the admission path OPENED the candidate before evicting the donor
 * (a transient fifth handle), and the save path fell back to an uncached
 * temporary handle when the region file did not exist yet (a fifth handle
 * again, synced and closed per chunk). The pool removes all three:
 *
 *  - REGION_POOL_SLOTS (8) stable slots per session; a world holds a compact
 *    vector of slot indices (struct server_world.region_slots). A slot's
 *    persistent key is (save root, dimension, rx, rz) -- carried by the
 *    archive's file name and coordinates -- and its runtime identity is
 *    (owner world slot, owner world generation, slot incarnation).
 *  - States FREE -> OPENING -> READY <-> BUSY -> EVICTING -> FREE, plus
 *    FAULTED (reserved for a handle left in an unknown state; a donor whose
 *    sync succeeded but whose fclose reported failure is consumed -- its slot
 *    is FREE again and only the owner's region_close_failed latch remains,
 *    A2S-P1 review L1).
 *  - Admission: a FREE slot, else the oldest UNPINNED READY donor across both
 *    worlds (ties by pool index). The donor is settled (sync = publish) and
 *    CLOSED before the candidate is opened -- never over capacity. A
 *    donor whose sync fails stays READY with its dirty state (the owner's
 *    region_evict_failed latches, the request fails); a donor whose close
 *    fails frees its slot and latches the owner's region_close_failed. No
 *    eligible donor (all pinned/busy) refuses with STORAGE_BUSY. A `!create`
 *    request for an ABSENT region file answers STORAGE_ABSENT before any
 *    admission (M1: a load probe never evicts for a file that is not there).
 *  - Missing-region creation goes through the same admission
 *    (region_pool_acquire(..., create = true)): the created handle is cached
 *    and batched like any other, so the old per-chunk temporary is gone.
 *
 * The pool is synchronous (tick-thread) in this slice: the queued/ticketed
 * storage API of the design (region_request_poll, storage_ticket,
 * checkpoint_*) is the next A2-S row and layers on these slots. */
#ifndef REGION_POOL_H
#define REGION_POOL_H

#include "region_archive.h"
#include "storage_rc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* P03: eight saved-region keys in the measured two-dimension load window.
 * 4 -> 8 costs 10288 B PPC MEM1 BSS + 33296 B archive heap arrays; reserve
 * 128 KiB including FILE/buffers/allocator overhead. Full saved-byte proof:
 * test_save_path_ops_equiv, corrected P01 corpus and Wii-range replay. */
#define REGION_POOL_SLOTS 8
/* Announces the shared-admission API (the A2 RED tests key on it). */
#define REGION_POOL_SHARED_ADMISSION 1

enum region_slot_state {
	REGION_SLOT_FREE = 0,
	REGION_SLOT_OPENING,
	REGION_SLOT_READY,
	REGION_SLOT_BUSY, /* pinned: a lease/write is in flight, never a donor */
	REGION_SLOT_EVICTING,
	REGION_SLOT_FAULTED,
};

struct server_world;

struct region_pool_slot {
	struct region_archive archive; /* valid while state is READY/BUSY/EVICTING */
	enum region_slot_state state;
	struct server_world* owner; /* the world whose vector names this slot */
	uint8_t world_index;
	uint32_t world_generation;
	uint32_t incarnation; /* bumped per open; a stale index can never alias */
	uint32_t pins;
	uint64_t last_use;
};

struct region_pool {
	struct region_pool_slot slot[REGION_POOL_SLOTS];
	uint64_t clock;
	uint32_t next_incarnation;
	/* Accounting the tests pin: handles open NOW, the peak ever, and the
	 * eviction/fault census. open_handles can never exceed REGION_POOL_SLOTS. */
	uint32_t open_handles;
	uint32_t open_handles_peak;
	uint32_t evictions;
	uint32_t evict_sync_failures;
	uint32_t close_failures;
	uint32_t busy_refusals;
	uint32_t absent_probes; /* M1: !create requests answered without admission */
	bool ready;
};

void region_pool_init(struct region_pool* p);
/* Every slot must already be FREE or FAULTED (worlds release their regions at
 * teardown); tombstones are cleared. */
void region_pool_destroy(struct region_pool* p);

/* Acquire the region (rx, rz) of `w` (its save root + dimension). READY/BUSY
 * hit: touched, returned. Miss: admission as documented above, then open --
 * `create` also creates an absent region file (the save path); without it an
 * absent/unopenable file is STORAGE_ABSENT. On STORAGE_OK `*out` is the
 * cached handle (a synchronous borrow: never held across another acquire). */
enum storage_rc region_pool_acquire(struct region_pool* p,
									struct server_world* w, w_coord_t rx,
									w_coord_t rz, bool create,
									struct region_archive** out);
/* Read-only admission after the caller JUST checked this exact region's file
 * status. Reuse that answer instead of a second full-path stat. Never cache
 * it across calls: an absent answer still precedes eviction. */
enum storage_rc region_pool_acquire_probed(struct region_pool* p,
	struct server_world* w, w_coord_t rx, w_coord_t rz, bool exists,
	struct region_archive** out);
/* Pin/unpin a READY handle so it cannot be chosen as a donor (BUSY while
 * pins > 0). */
void region_pool_pin(struct region_pool* p, struct region_archive* ra);
void region_pool_unpin(struct region_pool* p, struct region_archive* ra);
/* Sync then close every handle `w` owns (teardown / world release); FAULTED
 * tombstones owned by `w` are cleared. Reports into w->region_evict_failed /
 * w->region_close_failed like the eviction path and returns false if any
 * handle failed. */
bool region_pool_release_world(struct region_pool* p, struct server_world* w);
/* The donor admission WOULD choose right now (oldest unpinned READY, ties by
 * index), or NULL. Test/diagnostic accessor. */
struct region_archive* region_pool_oldest_donor(struct region_pool* p);
const struct region_pool_slot* region_pool_slot_of(
	const struct region_pool* p, const struct region_archive* ra);
size_t region_pool_open_count(const struct region_pool* p);
#ifdef CAVEWII_REGION_ARCHIVE_TEST
/* Fail the NEXT candidate open (after its donor was settled and closed):
 * models the candidate's own allocation/fopen failure. One-shot. */
void region_pool_test_fail_next_open(void);
#endif

#endif
