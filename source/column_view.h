/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef COLUMN_VIEW_H
#define COLUMN_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include "block/blocks_data.h"

struct column_backing;
struct column_view;

/* R23 local publication identities. Zero or exhausted revisions never match. */
struct column_revision {
	uint32_t owner, incarnation, content, light, events;
};
#define COLUMN_LIGHT_PUBLICATIONS 4u
#define COLUMN_LIGHT_PUBLICATION_BYTES (256u * 1024u)
#define COLUMN_LIGHT_READS 64u
#define COLUMN_LIGHT_DEPENDENCIES 9u
struct column_light_dependency {
	struct column_backing* backing; /* owning lease; NULL is absent */
	int32_t x, z;
	struct column_revision revision;
};
struct column_light_cell {
	int32_t x, y, z;
	struct block_data block;
	uint8_t height;
	bool exists;
};
/* Immutable after enqueue. This bounded certificate admits only a flood
 * whose sole write is the unchanged forced source. Changing floods fallback. */
struct column_light_publication {
	uint32_t owner, world_generation, mutation_epoch, presentation_epoch;
	uint8_t world_index;
	int dimension;
	int32_t x, y, z;
	uint32_t pops, cells, dependencies;
	struct column_light_cell read[COLUMN_LIGHT_READS];
	struct column_light_dependency dependency[COLUMN_LIGHT_DEPENDENCIES];
};
enum column_light_fallback {
	COLUMN_LIGHT_OWNER, COLUMN_LIGHT_SEEDS, COLUMN_LIGHT_CAP, COLUMN_LIGHT_OOM,
	COLUMN_LIGHT_CHANGED, COLUMN_LIGHT_VERSION, COLUMN_LIGHT_RESIDENCY,
	COLUMN_LIGHT_BYTES, COLUMN_LIGHT_ACTIVE, COLUMN_LIGHT_BUDGET,
	COLUMN_LIGHT_INGRESS, COLUMN_LIGHT_FALLBACKS
};
struct column_light_stats {
	uint32_t published, hits, avoided_pops, probe_pops;
	uint32_t live, peak_live, bytes, peak_bytes, copy_us, validate_us;
	uint32_t fallback[COLUMN_LIGHT_FALLBACKS];
};
void column_light_configure(bool enabled);
bool column_light_enabled(void);
uint32_t column_light_mutation_epoch(void);
uint32_t column_view_presentation_epoch(void);
void column_light_mutated(struct column_backing* backing, bool light);
struct column_light_publication* column_light_publication_alloc(void);
void column_light_publication_release(struct column_light_publication* p);
void column_light_note_fallback(enum column_light_fallback reason);
void column_light_note_probe(uint32_t pops, uint32_t us, bool published);
void column_light_note_hit(uint32_t pops);
void column_light_note_validation(uint32_t us);
void column_light_get_stats(struct column_light_stats* out);
void column_view_bind_revision(struct column_view* view,
	struct column_revision revision);
bool column_view_revision_matches(struct column_view* view,
	const struct column_revision* revision);
bool column_view_content_matches(struct column_view* view,
	const uint8_t* heightmap);
bool column_view_apply_revision(struct column_view* view,
	struct column_revision revision, uint32_t previous_content);

#define COLUMN_VIEW_PAGE_CELLS 256u
#define COLUMN_VIEW_PAGES 512u
#define COLUMN_VIEW_INDEX_BYTES 2112u
/* Real Wii split_anchor_6m: peak 88 pages (seed12345/d96); seed42/d256
 * reached 77, so 256 pages once looked like 2.91x margin. SPLIT-SPILL
 * (2026-10-14, tmp/jobs/loop/SPLIT-SPILL/DIAG.md): split_play_8m pins the
 * 256-page pool full in every run since 09-26 (op peak 255-256, opf 414-480
 * cumulative failed admissions). Each failure costs an 80 KiB private image
 * (COLUMN_VIEW_PRIVATE_BYTES + 256 B guard) instead of a 256 B page, and
 * that churn fragments the shared carve until its largest hole drops below
 * the 80.25 KiB request and it spills. opf is cumulative, not simultaneous
 * demand, so it cannot size the pool; 1024 is the bounded candidate and the
 * real-Wii split_play_8m runs decide sufficiency. Cost: pool payload
 * 64 -> 256 KiB of MEM2 carve (+192 KiB, one block taken at the first local
 * view install), s_used bitmap 32 -> 128 B of MEM1 .bss (+96 B). Still
 * (8192-1024)*256 = 1792 KiB below the original 8192-page pool. Emergency
 * image, recovery, and opf/oer/orc stay enabled. Presentation bytes are
 * identical whether a cell lives in a page or a private image
 * (test_column_dedup_overlay replays the same operations at limits
 * 0/1/256/1024). */
#define COLUMN_VIEW_POOL_PAGES 1024u
#define COLUMN_VIEW_PRIVATE_BYTES 81920u
/* Bound the server's wait for main to accept exhaustion recovery. This is
 * wall time, not CPU time; scheduler/stripe acquisition can add latency. */
#define COLUMN_VIEW_RECOVERY_WAIT_MS 25u

enum column_plane { COLUMN_IDS, COLUMN_META, COLUMN_SKY, COLUMN_TORCH };

/* One owning reference per shell, including shells retained by worker/GPU
 * retirement. The constructor also returns one reference to its caller. */
struct column_view* column_view_create(struct column_backing* backing,
	const uint8_t* ids, const uint8_t* metadata, const uint8_t* sky,
	const uint8_t* torch);
void column_view_ref(struct column_view* view);
void column_view_unref(struct column_view* view);
struct block_data column_view_get(struct column_view* view, unsigned index);
/* Light-only read; same stripe and presentation planes as column_view_get. */
void column_view_get_light(struct column_view* view, unsigned index,
	uint8_t* sky, uint8_t* torch);
void column_view_set(struct column_view* view, unsigned index,
	struct block_data block, bool light_only);
/* Caller holds this view's backing stripe; never hold two stripes. */
struct block_data column_view_get_locked(struct column_view* view, unsigned index);
/* Observer export: compare actual presentation bytes, never a dirty/version
 * cache. Destinations are private packed planes and must not alias backing. */
bool column_view_copy(struct column_view* view, uint8_t* ids, uint8_t* metadata,
	uint8_t* sky, uint8_t* torch);
/* Telemetry-only, read-only census (nether-dark/1): cells with BlockLight
 * (torch) > 0 in this view's PRESENTATION (what the client renders) and in
 * its backing (the server's own column arrays), one stripe hold, no writes. */
void column_view_torch_census(struct column_view* view, uint32_t* view_lit,
	uint32_t* backing_lit);

/* The server owns its aliases, but ALL published voxel writes use this pair.
 * Begin preserves affected presentation pages, then returns with the stripe
 * held. End rejoins equal pages and unlocks. NULL is an unpublished column.
 * No allocation, callbacks, RPC, or other lock inside the caller's region. */
void column_backing_write_begin(struct column_backing* backing, unsigned planes,
	unsigned first, unsigned count);
void column_backing_write_end(struct column_backing* backing, unsigned planes,
	unsigned first, unsigned count);
/* Point writer: caller already owns the server alias. Skips identical values
 * and updates divergence counts in O(number of views), without a page scan. */
void column_backing_set_cell(struct column_backing* backing, unsigned plane,
	unsigned index, uint8_t value);
/* The light pair (planes bit 0 = SKY, bit 1 = TORCH; SKY first) under one
 * stripe hold; each selected plane exactly as column_backing_set_cell. */
void column_backing_set_light(struct column_backing* backing, unsigned index,
	unsigned planes, uint8_t sky, uint8_t torch);
/* The same with backing->mutex ALREADY HELD by the caller (thread_stripe_lock),
 * and still held on return (a page-pool slow path drops and retakes it inside
 * column_backing_write_begin exactly as set_cell does). The caller owns the
 * no-allocation / no-other-lock rule for its whole held region. */
void column_backing_set_light_locked(struct column_backing* backing,
	unsigned index, unsigned planes, uint8_t sky, uint8_t torch);

struct column_view_stats {
	uint32_t live_views, pages, peak_pages, fallback_live, fallback_total;
	uint32_t page_failures, emergency_uses, recovery_entries;
	/* Boot-lifetime: requests (0->1), expiries (1->3), longest completed wait. */
	uint32_t recovery_cycles, recovery_expiries, recovery_wait_max_us;
};
void column_view_get_stats(struct column_view_stats* out);
/* Main-thread quiescent teardown. Refuses while a view or allocation-gap
 * writer still owns storage. The next local create reserves both tiers. */
bool column_view_pool_shutdown(void);
bool column_view_pool_present(void);
/* Allocation exhaustion requests the existing voxel-stream recovery. Server
 * writes wait with the stripe unlocked for at most RECOVERY_WAIT_MS before
 * invalidating the obsolete presentation epoch themselves. Main still owns
 * recovery/retirement, including all worker/GPU shells; no server write is
 * dropped or reordered. The bound excludes scheduler and stripe latency. */
bool column_view_recovery_pending(void);
bool column_view_recovery_begin(void);
void column_view_recovery_reset(void);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Limit new pool admissions without invalidating already owned pages. */
void column_view_test_page_limit(unsigned limit);
#endif
#endif
