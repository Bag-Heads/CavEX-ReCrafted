/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef SERVER_WORLD_H
#define SERVER_WORLD_H

#include "residency.h"

#include <m-lib/m-dict.h>
#include <stdbool.h>
#include <stdint.h>

#include "../platform/mem2.h"
#include "../carve_observer.h"
#include "../platform/thread.h"
#include "../platform/time.h"
#include "../worldgen/java_random.h" // hell_populate_rng (shared Nether decorate RNG)
#include "region_archive.h"

struct world_modification_entry; /* lighting.h */
#include "region_pool.h"
#include "../cNBT/buffer.h" /* S2: struct buffer in server_world_retiring */

struct worldgen;
struct server_local;
struct server_world;
struct server_world_populate_scratch;
struct piston_moving_state;
struct far_rpc_slot; // M1 deep reach: compact far-chunk payload (client_interface.h)
struct column_view;
struct server_light_trial_capture;

#include "../column_view.h"

/* Stable array owner. Dictionary values only borrow these five pointers; one
 * dictionary reference and any transport/client leases own the allocation.
 * Tags are immutable and never contain a pointer back into a server world.
 * Local client reads use registered differential views. Published voxel
 * writers enter column_view's barriers; raw aliases remain server-thread-only.
 * Never allocate, call out, or wait for RPC/GPU work while holding a stripe.
 * MEM2 and stripes are never nested (in either direction); release before
 * alloc/free. Never hold two stripes. Stripes outlive ALL worlds and leases. */
#define COLUMN_BACKING_LOCK_STRIPES 8
/* Every persistent mutex column_backing_init creates in its one atomic
 * transaction: the eight backing stripes plus the G9 light-drain heartbeat
 * lock (bf92f3e33). Each spends one of libogc's 64 LWP mutex slots for the
 * process lifetime; there are zero per-column constructors (CD1, Astra
 * ruling 2026-10-08 item 3). */
#define SERVER_WORLD_PERSISTENT_MUTEXES (COLUMN_BACKING_LOCK_STRIPES + 1)
struct column_backing {
	uint8_t* ids;
	uint8_t* metadata;
	uint8_t* lighting_sky;
	uint8_t* lighting_torch;
	uint8_t* heightmap;
	/* Adopted from the chunk: one MEM2_POOL_COLUMN slot (free ids only) when
	 * set, five arrays otherwise (server_chunk.arrays_slot). */
	bool arrays_slot;
	struct thread_mutex* mutex; // borrowed process-lifetime stripe
	uint32_t references;
	uint32_t world_instance;
	uint32_t generation;
	enum world_dim dimension;
	struct column_view* views; // stripe-protected presentation generations
	struct column_revision revision; // stripe; local reuse bookkeeping only
	bool resident; // stripe; false before the dictionary lease is released
	uint32_t settled_light_revision; // certified only by a committed drain
	uint32_t event_previous_content; // server-only; one normal edit's start
};

/* Called at client boot; standalone server worlds also ensure initialization.
 * Idempotent across sessions. Failed initialization unwinds every new lock. */
bool column_backing_init(void);

/* Every caller already owns a reference (directly, or via its dictionary,
 * slot or shell). Ref cannot resurrect an unowned pointer. NULL is a no-op.
 * references/matches are observations, never permission to acquire a lease. */
void column_backing_ref(struct column_backing* backing);
void column_backing_unref(struct column_backing* backing);
void column_backing_get_stats(uint32_t* live, uint32_t* shared);
uint32_t column_backing_references(struct column_backing* backing);
bool column_backing_matches(const struct column_backing* backing,
	uint32_t world_instance, enum world_dim dimension, uint32_t generation);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Atomic lifetime totals; no reset can hide outstanding owners at teardown. */
void column_backing_test_counts(uint32_t* created, uint32_t* destroyed,
	uint32_t* references);
void column_backing_test_fail_next(void);


#endif

struct server_chunk {
	/* Server-only, borrowed only during a coherent checkpoint trial. */
	struct server_light_trial_capture* light_trial_capture;
	/* Dormant census rejection filter, never a persistence decision. A match
	 * MUST be verified against the complete previous disk payload. Zero is a
	 * cold key; eviction/reload drops history conservatively. */
	uint32_t census_save_hash[2]; /* two words avoid adding Wii struct alignment */
	uint8_t* ids;
	uint8_t* metadata;
	uint8_t* lighting_sky;
	uint8_t* lighting_torch;
	uint8_t* heightmap;
	/* The five arrays above live in ONE MEM2_POOL_COLUMN slot
	 * (server_chunk_arrays_alloc) when set: only `ids` (the slot base) is
	 * freed, the other four are interior pointers. Chunks built by hand
	 * (tests, temps) keep five arrays and five frees. */
	bool arrays_slot;
	struct column_backing* backing;
	bool modified;
	/* CaveWii-owned full-image marker for Level.TileEntities. Old CaveWii/Beta
	 * chunks can contain an empty list while chests.dat/signs.dat still own rows,
	 * so list presence alone is not authoritative. A marked record replaces the
	 * flat chest/sign rows for its chunk; an unmarked record keeps migration merge
	 * semantics. Set only after parsing/writing CaveWiiTileEntitiesComplete=1. */
	bool persisted_tile_entities_complete;
	/* True when the last successfully loaded/written Level.Entities TAG_List
	 * contained at least one raw element. This is deliberately independent of
	 * recognized runtime entity count: unknown records still require a later
	 * empty rewrite so they cannot survive or resurrect after support changes. */
	bool persisted_entities_nonempty;
	// Beta populate (decorate) lifecycle. `populated`: this chunk's own
	// populate() quadrant has run (disk-loaded chunks are pre-marked populated
	// -- they were saved post-decoration, so must never be re-decorated).
	// `sent`: a CRPC_CHUNK has been emitted for this chunk (the chunk-send
	// gate uses this instead of mere residency, because populate loads
	// neighbour chunks into the dict without sending them).
	bool populated;
	bool sent;
	/* A2-W slot>=1 publication is one resumable FIFO sequence. The cursor is
	 * retained with the resident column so inbox refusal cannot replay an
	 * already accepted sign or entity spawn. Zero-initialized on load; reset
	 * explicitly on a scoped STREAM_RESET. Rollback source restoration also
	 * uses the cursor for slot 0 while it owns a partially accepted column. */
	uint8_t a2w_pub_stage;
	/* IMP-TE-CAPS: a sign ROW index; the sign table grows past 65535 rows,
	 * so the cursor is 32-bit (a 16-bit one wrapped and replayed forever). */
	uint32_t a2w_sign_cursor;
	uint32_t a2w_entity_after_id;
	/* Runtime-only load-rearm state. CaveWii does not persist Beta's
	 * NextTickList, so a resident chunk is scanned once each time its complete
	 * 3x3 neighbourhood becomes resident. Any member unloading clears this bit;
	 * it is deliberately not serialized in the region archive. */
	bool load_ticks_rearmed;
	/* Next linear block cell for a capacity-interrupted reconstruction scan.
	 * This prevents a >256-entry ring from repeatedly duplicating its admitted
	 * prefix and starving later cells. Zero when no partial scan is active. */
	uint32_t load_tick_rearm_cursor;
	/* Near-send FIX (movement flake, 2026-09-02): consecutive server_world_
	 * ensure_populated failures for this chunk since its last success (or
	 * since it became resident). server_local.c's near-send scan resets this
	 * to 0 the moment ensure_populated succeeds, and only escalates a chunk
	 * into the bounded 16-slot near_send_fail backoff table (the full
	 * NEAR_SEND_FAIL_RETRY_TICKS delay) once this reaches NEAR_SEND_FAIL_
	 * STREAK_THRESHOLD. Below that threshold, near_send_fail_seq (the
	 * near_send_seq value at the last failure) merely excludes the chunk for
	 * the REST OF THE CURRENT tick's scan -- preserving the "skip and move on
	 * within the same scan" property the original stuck-chunk fix relies on --
	 * so a transient one-off "not ready yet" (routine for a chunk newly
	 * entering view as the player walks, or for a fresh dimension load) costs
	 * at most one tick, never the full 20. Per-chunk rather than table-slotted
	 * so an ordinary walk exposing many new edge columns at once can never
	 * exhaust the bounded table with transient entries. Runtime-only, like
	 * load_ticks_rearmed above: deliberately not serialized in the region
	 * archive (a reloaded/disk chunk starts at streak 0, same as any other
	 * freshly-resident chunk). */
	uint8_t near_send_fail_streak;
	uint32_t near_send_fail_seq;
	/* --- M3: DURABILITY HAND-OFF (round-5 fix round) ----------------------
	 * `modified` used to be cleared -- and, on the eviction path, the whole
	 * chunk freed -- the moment region_archive_set_chunk returned true. Since
	 * the save batch (round 2) and the I/O helper (round 5) that is up to a
	 * batch window BEFORE the on-disk index names the new record, so a failed
	 * publication silently reverted the chunk to its old disk content with
	 * nothing left in memory to write again.
	 *
	 * `save_job` is the region-archive PUBLICATION JOB the last successful
	 * write of this chunk joined and `save_pending` says that job has not
	 * been acknowledged yet. While it is set the chunk KEEPS `modified`, and
	 * server_world_reconcile_pending_saves RE-DIRTIES the chunk if the archive
	 * later says that job failed.
	 * Any later edit clears `save_pending` (server_world_chunk_touch): the
	 * pending record no longer describes the chunk, so its durability must not
	 * clear the new dirty bit.
	 * S1: this was the publication EPOCH, compared against cumulative
	 * durable/failed high-water marks. A failed publication left the epoch
	 * unconsumed, so the next batch reused it and its success reported the
	 * FAILED job's chunks as durable -- this bit then cleared with the chunk
	 * never re-dirtied. The job id is process-wide monotonic and never reused,
	 * so a completion answers for exactly the publication that carried these
	 * bytes. */
	uint64_t save_job;
	bool save_pending;
	/* G9X-6: client PUBLICATION debt, separate from persistence (`modified`)
	 * and from `sent`. Set by every store that actually changes this column's
	 * Sky or Block light (Beta publishes every light change; block changes
	 * are published by the notify setters' CRPC_SET_BLOCK, the no-notify
	 * setters publish nothing -- see server_world.c s_pub_trial); cleared
	 * only when a complete CRPC_CHUNK snapshot of the
	 * current bytes is accepted by the client FIFO (initial near send or the
	 * post-drain republish, server_local.c). Beta publishes every stored
	 * change of a watched column [CFR World.java:512-517 setLightValue ->
	 * markBlockNeedsUpdate; Chunk.java:474-486 getChunkData copies the stored
	 * planes]; without this, light/blocks written into an already-sent column
	 * by neighbour generation never reached the client. Checkpoint-trial
	 * writes and their exact restore neither set nor clear it; a population
	 * rollback leaves it set (a redundant refresh is safe, a lost one is not).
	 * Runtime-only, never serialized. */
	bool pub_debt;
	/* G9X-6: digest of the planes of the last snapshot the client FIFO
	 * accepted (valid only for snapshots sent by server_local.c). A store
	 * sequence that returns the column to exactly those bytes leaves nothing
	 * to publish: the refresh then drops the debt without sending. */
	uint64_t pub_digest;
	bool pub_digest_valid;
};

/* Mark a resident chunk dirty. Also drops any outstanding durability record:
 * the bytes already handed to the archive are older than this edit, so their
 * publication must not be allowed to clear the bit this call sets. */
static inline void server_world_chunk_touch(struct server_chunk* sc) {
	sc->modified = true;
	sc->save_pending = false;
}

// Single canonical zero-init for a struct server_chunk about to be built by
// ANY of the several construction sites (disk load, worldgen generation,
// far-skin extract). Call this FIRST, before setting any field -- see
// server_world.c's doc comment on the definition for why a single explicit
// call replaces three previously-implicit "the rest happens to be zero"
// routes to the same guarantee.
void server_chunk_init(struct server_chunk* sc);
// Releases the dictionary's backing reference (or unpublished arrays).
// Logical eviction is independent of any remaining transport/client leases.
// Paired with server_chunk_init
// (or an equivalent construction) -- see its own doc comment.
void server_world_chunk_destroy(struct server_chunk* sc);

#define MAX_REGIONS 4
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Read-only complete queue image, for exact checkpoint rollback proof. */
size_t server_world_test_generation_queue_copy(const struct server_world* w,
	void* out,size_t capacity);
#endif
/* M3, MEASURED OUTCOME -- WHY AN EVICTED CHUNK IS *NOT* HELD RESIDENT UNTIL
 * ITS BATCH IS DURABLE. That shape was implemented and rejected by the suite:
 * a saved-and-erased chunk leaving w->chunks IMMEDIATELY is a contract
 * test_respawn_residency, test_tnt_primed_env, test_column_dedup_lifetime /
 * _state_equiv / _render_equiv, test_split_residency_single_equiv and
 * test_startup_retirement all pin, and they fail at a deferral cap of 8 and at
 * a cap of 2 alike. Forcing durability instead -- one region_archive_sync per
 * evicting write -- is worse: an eviction runs inside the OPEN save batch, so
 * it reinstates the per-chunk publication cost the whole pace program removed.
 * Had the deferral landed it would also have cost cap x 82,176 bytes of MEM2
 * (8 -> 642 KB; REGION_BATCH_MAX 32 -> 2.51 MB, already more than the
 * split-scene carve minimum of ~2.2 MB the round-5 note measures).
 * The eviction path is therefore covered by the ARCHIVE-side half of M3 only
 * (latched failure, refused N+1, reported at the next barrier); the deferred
 * dirty-clear covers the checkpoint/autosave path, where the chunk is still
 * resident and can simply be re-dirtied. */
#define SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY 4096
// Beta 1.7.3 World.tickUpdates executes at most 1000 due callbacks per game tick.
// Keep that as the absolute fire ceiling around the Wii's lower fluid cap.
#define SERVER_WORLD_SCHEDULED_TICK_BETA_CAP 1000u
// HISTORICAL (HUNT-14 item 10, ci-1101/39): the ring depth above which the old O(count) linear
// (pos,type) dedup scan was SKIPPED (plain append) to dodge a Broadway stall. Beta
// World.scheduleBlockUpdate [740c583 World.java:1161] gates EVERY insert on
// scheduledTickSet.contains(), at any depth, so that cutoff was a Bucket A difference (a
// duplicate (x,y,z,id) could fire twice in a deep fluid cascade). The queue now keeps a parallel
// open-addressing (x,y,z,id) hash set == Beta's O(1) HashSet and always dedups; the define is
// kept only so the goldens can name the old cutoff when proving dedup holds past it.
#define SCHEDULED_TICK_DEDUP_SCAN_MAX 256
// Shift in UNSIGNED then cast: `(int64_t)(z) << 32` is signed-shift UB for negative z
// (UBSan halts the CI ASan soak); the key bit-pattern is byte-identical for any 32-bit z.
#define S_CHUNK_ID(x, z)                                                       \
	((int64_t)(((uint64_t)(uint32_t)(z) << 32) | (uint32_t)(x)))
#define S_CHUNK_X(id) ((int32_t)((id) & 0xFFFFFFFF))
#define S_CHUNK_Z(id) ((int32_t)((id) >> 32))
// KEY-CONSTRUCTION CONTRACT for dict_server_chunks (and every other dict
// below keyed the same way, e.g. dict_far_sent, dict_autosave_saved): the
// ONLY correct way to form or decompose this 64-bit key is the three macros
// above -- x packed into the LOW 32 bits, z into the HIGH 32 bits, always
// through S_CHUNK_ID/S_CHUNK_X/S_CHUNK_Z, never hand-rolled shifts. A
// followups-lane investigation (2026-09-02) into a reported
// "dict_server_chunks_get() NULL for a key dict_server_chunks_it() had just
// enumerated" anomaly found NO engine/m-lib bug: inserting via
// S_CHUNK_ID(x,z), iterating via dict_server_chunks_it()/_ref()->key, and
// get()-ing each iterated key round-trips correctly for every sign
// combination and the INT32 extremes (tests/test_dict_server_chunks_lookup.c,
// PASS). The likely cause of the original report is external probe/test code
// forming the lookup key some OTHER way (e.g. a swapped S_CHUNK_ID(z, x), or
// a hand-rolled shift without the (uint32_t) cast this macro uses to dodge
// signed-shift UB on negative coordinates) instead of reusing S_CHUNK_ID or
// decomposing an iterated key with S_CHUNK_X/S_CHUNK_Z. If this ever
// reproduces again, extend that test with the exact repro coordinates before
// suspecting m-lib itself.


struct scheduled_tick {
	w_coord_t x, y, z;
	uint32_t fire_at_tick;
	uint8_t block_type;
	// Internal drain marker (kept-in-place original); cleared before the drain
	// returns. Fresh schedules always clear it.
	uint8_t queue_flags;
};

// key not!!! stored in multiples of CHUNK_SIZE
DICT_DEF2(dict_server_chunks, int64_t, M_BASIC_OPLIST, struct server_chunk,
		  M_POD_OPLIST)

// M1 deep reach: set of (x,z) chunks streamed as compact far columns (key =
// S_CHUNK_ID, value unused). Kept SEPARATE from server_chunk.sent: a far chunk is
// freed the same tick it is streamed (never resident, so it has no server_chunk to
// hang a flag on), and the near-send gate must still pick it as not-full-sent when
// it promotes into the full view radius. Drained by the far evictor beyond the far
// radius via CRPC_UNLOAD_CHUNK, so the uncapped client section dict cannot leak.
DICT_DEF2(dict_far_sent, int64_t, M_BASIC_OPLIST, uint8_t, M_BASIC_OPLIST)

/* S2: one evicted chunk's committed-but-unpublished record (see the
 * `retiring` field below). `record` is the exact compressed bytes the
 * archive appended; `save_job` is the publication job id sampled before that
 * append, i.e. what region_archive_save_state answers about. */
#define SERVER_WORLD_RETIRING_MAX 128
/* Byte budget for the retained records (compressed, shrunk to length). The
 * slot is reserved BEFORE the write and the record size is only known after
 * it, so the bound is MAX_BYTES + one record: a new eviction is refused as
 * soon as the retained total reaches the budget. */
#define SERVER_WORLD_RETIRING_MAX_BYTES (512u * 1024u)
struct server_world_retiring {
	int64_t id;
	uint64_t save_job; /* region_archive_save_job sampled before the append */
	struct buffer record;
	bool active;
};

/* A1a (split-dimensions-plan-0924, Astra D1/D8): a durable NAME for one live
 * world instance. `world_index` is the server_local slot (worlds[]);
 * `generation` is the instance's process-unique identity, assigned at
 * create and cleared at destroy/discard, so a handle taken from a live world
 * resolves (server_local_world_from_handle) only while that exact instance is
 * live -- never after a discard, and never to a later world re-created in the
 * same slot. Delayed work (portal jobs, queued publications, window bindings)
 * stores a handle and revalidates it at use; it never caches a world pointer
 * across ticks. generation == 0 is never a live world. */
/* G9: one column's pre-batch skylight (WORLD_HEIGHT nibbles), private to
 * the light drain. */
struct server_light_undo {
	w_coord_t x, z;
	uint8_t sky[WORLD_HEIGHT / 2];
};

struct server_world_handle {
	uint8_t world_index;
	uint32_t generation;
};

/* R24 stress tokens remain owned until the durable ordered prefix retires. */
#define SERVER_CHECKPOINT_MAX 4
#define SERVER_CHECKPOINT_SLOTS (SERVER_CHECKPOINT_MAX + 1) /* held REJECTED request */
struct server_checkpoint_token {
	uint32_t sequence, generation, logical_tick;
	enum region_checkpoint_state state;
	ptime_t submitted, noted;
	uint32_t latency_us;
	bool rejection_reported;
	unsigned handles;
	struct region_archive* archive[REGION_POOL_SLOTS];
	struct region_io_fence* fence[REGION_POOL_SLOTS];
};
struct server_world {
	uint32_t backing_world_instance;
	uint32_t backing_generation;
	/* A1a identity (see struct server_world_handle). world_index is the slot
	 * in server_local.worlds[] this storage IS; server_local assigns it once
	 * at init (a calloc'd standalone/test world reads 0), create/destroy never
	 * touch it. generation: process-unique per create, 0 while not live. */
	uint8_t world_index;
	uint32_t generation;
	/* G9X-6: fair cursor of the post-drain publication refresh
	 * (server_local.c): the S_CHUNK_ID of the last column republished. */
	int64_t pub_cursor;
	bool pub_cursor_valid;
	/* Is `chunks` (and `far_sent`) actually constructed right now? A
	 * struct server_world spends real time in a state where it is NOT: from
	 * process start until the first server_world_create, and again from every
	 * server_world_destroy/_discard until the next create (menu -> world ->
	 * menu -> world). m*lib's dict_clear frees the index array and leaves the
	 * handle with index == NULL, so ANY read of `chunks` in that window --
	 * iteration included -- dereferences NULL (asserts on PC, faults on Wii).
	 * The server's own code never reads the dict in that window; a per-tick
	 * OBSERVER hook handed `s` does, because it is armed at boot (main.c's
	 * carve_observer_start) and cannot know the world's lifecycle. Read it
	 * through server_world_chunks_ready(). */
	bool chunks_ready;
	dict_server_chunks_t chunks;
	// Tier-0 chunk-resolution cache (engine_foundation_rethink names it
	// MANDATORY): the last (cx,cz)->server_chunk* any per-block accessor
	// resolved. Block access is spatially clustered (random ticks, mob
	// collision sweeps, lighting floods walk neighbours), so one entry kills
	// most of the dict-hash storm that made real-Wii set_block ~27x slower
	// than Dolphin. SERVER-THREAD-ONLY (every accessor runs on the server
	// tick thread) -- no lock. INVALIDATION IS LOAD-BEARING: the dict stores
	// chunks BY VALUE, so ANY insert (a rehash moves every value) or erase
	// dangles this pointer -- every dict mutation must call
	// server_world_chunk_cache_invalidate or the cache is a use-after-free.
	// I07 lane 3 (G9 drain, T1): FOUR ways, not one. A light flood walks a
	// cell's six neighbours and, at a chunk seam, alternates between two
	// (at a corner, up to four) chunks inside ONE pop; the one-entry cache
	// then thrashed -- 9% of the drain's resolves (about 1.2 dict lookups
	// per pop) went to the dict on the perf_stress_5m light_queue_stress
	// worst case (bench_g9_drain, measured) -- 0% with four ways. Pure
	// caching: the same (cx,cz) -> chunk answer, so every accessor's values
	// are unchanged (test_g9_drain_equiv pins the drain's write trace).
	// Lookup is a four-compare scan; replacement is round-robin.
#define SERVER_WORLD_CHUNK_CACHE_WAYS 4
	int64_t chunk_cache_id[SERVER_WORLD_CHUNK_CACHE_WAYS];
	struct server_chunk* chunk_cache[SERVER_WORLD_CHUNK_CACHE_WAYS];
	uint8_t chunk_cache_next; /* the way the next miss fills */
	dict_far_sent_t far_sent;
	enum world_dim dimension;
	// Owning local server, or NULL. Set by server_local right after
	// server_world_create. The chest/sign tables live in server_local, so the
	// chunk load/save lifecycle reaches them through this back-pointer: it loads
	// each chunk's Level.TileEntities into the tables on load and gathers them
	// back out on save (docs/chest_store_redesign.md). NULL-safe -- a generator-
	// or test-only world with no local server just persists empty TileEntities.
	struct server_local* local;
	/* A1a (split-dimensions-plan-0924, D1 "session services through the
	 * world"): the SESSION this world belongs to -- the server_local whose
	 * worlds[] slot this storage is. The explicit-world mutation APIs derive
	 * every session service from it (entity table, RNG streams, sound and
	 * client sinks, actor membership, block-callback dispatch). It is bound
	 * by server_local_init for every slot and re-bound by the load path after
	 * create (create clears it, because a standalone world may live in fresh
	 * stack storage; destroy/discard do NOT clear it -- they zero only
	 * `generation` -- so a released slot keeps its session binding until the
	 * next create). It is deliberately NOT `local`:
	 * `local` is the ARCHIVE binding that gates entity/tile-entity
	 * persistence on chunk load/save and is only set once the archive is
	 * open, so a minimal test fixture that owns a world must be able to run
	 * callbacks (session) without acquiring persistence (local). In a live
	 * session the two are equal. NULL only for a world no session owns
	 * (generator/standalone fixtures): such a world mutates without block
	 * callbacks and publishes unconditionally, as before A1a. */
	struct server_local* session;
	// World seed (level.dat RandomSeed) and the Beta terrain generator built
	// from it. `generator` is NULL for dimensions worldgen does not handle.
	int64_t seed;
	struct worldgen* generator;
	// CFR ChunkProviderHell.java:61-71,202-209,300-347: ONE provider Random
	// across constructor octaves, terrain reseeding/surface and population.
	// Saved-chunk loads leave it untouched; unused in the Overworld.
	// (Nether populate is non-deterministic per-chunk by this Beta design.)
	struct java_random hell_populate_rng;
	// F4-RNG (ASTRA-RNG-RULING 3/4): Beta World.rand -- `public Random rand = new
	// Random()` (World.java:37), ONE java.util.Random per live world instance,
	// independent across dimensions and generations. Seeded once at create from
	// an independent runtime seed (server_world_runtime_jrand_seed), after which
	// the World initializer's own `ambientTickCountdown = rand.nextInt(12000)`
	// (World.java:54) is consumed; never saved (WorldInfo stores only the
	// TERRAIN RandomSeed). Drawn by explosion rays/sound (Explosion.java:50,127),
	// chain-TNT fuse and every block drop (Block.java:294-326). Distinct from
	// the session xorshift rand_src, Math.random and every entity's own rand.
	struct java_random rand;
	// Debug ("debug.superflat"): flat-world test stage -- the generator emits flat
	// layers and the populate pass (trees/ores/snow) is skipped. Set in
	// server_world_create.
	bool superflat;
	// Beta World.editingBlocks: while true, server_world_set_block still writes the
	// block, runs the lighting pass, and tells the client, but the six-neighbour
	// notify loop (GAP#1 auto-schedule + onNeighbourBlockChange dispatch) is
	// SUPPRESSED -- Beta gates World.notifyBlockOfNeighborChange on !editingBlocks
	// so a multi-block structure (the portal fill in BlockPortal.tryToCreatePortal,
	// the Teleporter frame build) can be placed without its own half-built shape
	// self-breaking via BlockPortal.onNeighborBlockChange mid-fill.
	// [decompile 740c583 World.notifyBlockOfNeighborChange:516]
	bool editing_blocks;
	/* G9 (ASTRA-1005B B2-B4, ASTRA-G9-RULING): this world's pending gameplay
	 * light. Beta's Chunk.setBlockIDWithMetadata writes the column/height at
	 * once (func_339_g) and only SCHEDULES Sky/Block propagation
	 * (World.scheduleLightingUpdate), which MinecraftServer.doTick drains
	 * after World.tick and before updateEntities. `light_seeds` are the cells
	 * whose propagation is pending; `light_undo` holds, per column, the sky
	 * nibbles as they were before this batch's first immediate column write
	 * (private to the drain); `light_pins` the chunk ids holding seeds (a pin
	 * covers that chunk and its 8 neighbours). Capacity is reserved before
	 * any mutation; `light_fault` stops the world when a reservation fails. */
	struct world_modification_entry* light_seeds;
	size_t light_seed_n, light_seed_cap;
	/* Original Beta generation's ordered MetadataChunkBlock regions. These
	 * are independent of the historical gameplay fixed-point frontier. */
	/* G9X-5: a lossless paged LIFO (server_beta_light.inc). n is the logical
	 * depth; pages [0, base) live in an owned temporary backing file, the rest
	 * in at most `slots` resident 128-entry page slots (page p in slot p%slots),
	 * so the newest five (the last-five merge window) are always resident. */
	struct server_beta_light_region** generation_light_pages;
	size_t generation_light_n, generation_light_base;
	uint32_t generation_light_slots;
	struct server_beta_light_backing* generation_light_backing;
	size_t generation_light_stat[3]; /* spills, loads, logical peak */
	/* Bounded, server-owned exact-input compression memo; coherent saves only. */
	struct region_encode_cache* checkpoint_encode_cache;
	bool checkpoint_encode_active;
	bool checkpoint_stream_active;
	bool checkpoint_failed; /* latched for final lifecycle/profile report */
	struct server_checkpoint_token checkpoints[SERVER_CHECKPOINT_SLOTS];
	unsigned checkpoint_head, checkpoint_count;
	uint32_t checkpoint_sequence, checkpoint_durable;
	struct server_light_undo* light_undo;
	size_t light_undo_n, light_undo_cap;
	int64_t* light_pins;
	size_t light_pin_n, light_pin_cap;
	/* Outstanding reservations of the write sites currently on the stack (a
	 * nested set_block inside onBlockRemoval reserves on top of its caller). */
	size_t light_resv_seeds, light_resv_cols;
	bool light_fault;
	/* Server-thread terminal dispatch latch. No publication synchronization. */
	bool simulation_stopped;
	/* Cold authority accounting, separate from peer stop and tentative faults. */
	bool light_fault_accounted;
	/* Why the world stopped (enum server_light_fault). */
	uint8_t light_fault_reason;
	/* The drain's own lossless frontier (lighting_drain_*). Live between a
	 * started drain and its completion; a stopped drain keeps it, with the
	 * seeds, pins and undo, so no work is lost and nothing pinned is saved
	 * or sent (ASTRA-G9-REVIEW Required 2). */
	struct stack light_frontier;
	bool light_frontier_live;
	/* G9 item 4 (ASTRA-G9-REVIEW-2): the generation ORDER guard (Nether
	 * only: the Overworld populate reseeds per chunk). A populate
	 * deferred because pending light pins its area holds this world's
	 * generation: every other populate request defers too until the held
	 * column is populated (its stream's retry after the drain), so a light
	 * deferral never lets another column's populate draw the shared
	 * population RNG (the Nether's hell_populate_rng) first. Released when
	 * the held column populates, or when two prescribed drains passed with
	 * the column unpinned and not retried (its stream moved on). */
	bool light_gen_hold;
	uint8_t light_gen_hold_age;
	w_coord_t light_gen_hold_x, light_gen_hold_z;
	/* G9X-5a: backing-file I/O failures of this world's generation light
	 * queue (server_beta_light.inc sbl_io_fail): count, the last cause
	 * (enum server_population_storage_cause) and site. */
	size_t generation_light_io_fails;
	uint8_t generation_light_io_cause, generation_light_io_site;
	/* G9X-5a POPULATION_STORAGE_FAILED: the first backing I/O failure during
	 * population (its neighbour loads, nested populations, the pre-
	 * transaction tail reload, or inside the transaction, which was rolled
	 * back completely first) stops THIS world for good: sticky, set after
	 * any rollback, never cleared by a light-fault reset, no automatic
	 * retry. Pending work and the backing file stay owned by the world
	 * until its teardown. Cause = enum server_population_storage_cause,
	 * site = enum server_population_storage_site, (x, z) = the quadrant
	 * (the provisioned column for SITE_PROVISION, the generated chunk for
	 * SITE_TERRAIN). */
	bool gen_storage_failed;
	/* G9X-5b part 2: the checkpoint journal this world reserved before
	 * pressure (server_world.c, struct server_light_journal; NULL until the
	 * first gated checkpoint) and its sticky failed-restore latch. */
	struct server_light_journal* light_journal;
	bool light_journal_failed;
	uint8_t gen_storage_cause, gen_storage_site;
	w_coord_t gen_storage_x, gen_storage_z;
	string_t level_name;
	/* A2-S: region handles live in the SESSION's shared pool (region_pool.h,
	 * REGION_POOL_SLOTS across both worlds); this world holds a compact vector
	 * of the pool indices it owns, in acquisition order. `region_pool` is bound
	 * at the first acquisition: the owning session's pool when this world is a
	 * session slot (w->session), else the world's own embedded pool (a
	 * standalone/test world keeps today's cap of MAX_REGIONS by itself). */
	struct region_pool* region_pool;
	struct region_pool own_region_pool;
	uint8_t region_slots[REGION_POOL_SLOTS];
	uint8_t region_slot_count;
	/* Sticky until a successful region barrier: an LRU eviction attempt failed.
	 * A sync-failed old handle remains resident, so server_world_sync_regions
	 * can retry the exact evidence instead of merely forgetting an error bit. */
	bool region_evict_failed;
	/* Terminal for this world instance: fclose consumed a region handle but
	 * reported failure, so no later resident walk can retry that close. */
	bool region_close_failed;
	/* IMP-QUIT-UNSAVABLE: the last server_world_checkpoint_chunks pass's
	 * unsavable chunks -- how many failed, and the FIRST one's coordinates and
	 * region save stage (enum region_save_fail_stage). Reset at the start of
	 * every pass; a quit that cannot save names these in its dialog/log. */
	uint32_t ckpt_unsaved_count;
	w_coord_t ckpt_unsaved_x, ckpt_unsaved_z;
	uint8_t ckpt_unsaved_stage;
	/* The same pass also failed its light trial (begin or end), so its
	 * failure is NOT only the unsaved chunks above. */
	bool ckpt_light_failed;
	/* A save batch is open (server_world_save_batch_begin): chunk saves append
	 * their index delta to the owning region archive instead of republishing
	 * the header sector per chunk. Cleared by server_world_save_batch_end,
	 * which publishes every resident archive. See region_archive.h's SAVE BATCH
	 * seam comment for the fsync sequence and the invariant it preserves. */
	bool save_batch_open;
	/* S2 two-phase retirement (docs/p2_storage_ruling_fable_2026-09-09.md).
	 * An EVICTING save inside an open batch used to free the chunk, its
	 * entities and tile entities the moment the record was in the file --
	 * before the helper thread published the index. A publication failure
	 * then rolled the index back to the OLD record with nothing left to
	 * retry (the newest state was lost, silently). Now the evicting write
	 * hands its committed compressed record back and the world RETAINS that
	 * record here until the archive answers DURABLE; on FAILED it is
	 * re-appended from this exact copy at the next batch seam. The chunk
	 * still leaves the dict immediately (the visibility contract the
	 * residency/dedup/load-completion tests pin); only the ~5-15 KiB record
	 * outlives it. Invariant: a retiring id is never resident -- a reload
	 * consumes its entry (server_world_load_chunk_impl). A full ring refuses
	 * the eviction (retain-everything path; callers back off), never drops.
	 * Contract (docs/p2_storage_ruling §3) by construction: a retained record
	 * IS the persistence image and nothing can mutate it (the chunk is not
	 * resident); the ring dies with this world instance, so no cross-instance
	 * identity check is needed; an id that becomes resident again CONSUMES its
	 * entry first, so a stale record can never be re-appended over a newer
	 * resident save. */
	struct server_world_retiring retiring[SERVER_WORLD_RETIRING_MAX];
	size_t retiring_active;
	size_t retiring_bytes; /* sum of retained record lengths */
	uint32_t retiring_retries; /* FAILED entries re-appended */
	uint32_t retiring_refused; /* evictions refused by a full ring */
	/* WORLD-RETIRE-SRV (Fable B2-A2T F1, server half): set once this LIVE
	 * world, vacated by every actor and drained to zero resident columns,
	 * has told the client to release its slot (CRPC_WORLD_RELEASE {retiring=false}). Cleared
	 * when an actor is committed to it again. Client-side only: the server
	 * keeps the world LIVE (Astra A2 plan: "an unoccupied dimension normally
	 * remains LIVE with zero resident columns"). */
	bool client_retired;
	/* stage22-0927: set by the last server_world_save_chunk* call when an
	 * EVICTING save was deferred because the client inbox could not admit
	 * the chunk's entity-destroy batch (+ unload). Not a save failure:
	 * nothing is lost, every owner stays live; callers retry next tick. */
	bool evict_retire_deferred;
	uint32_t retiring_dropped; /* entries still unpublished at world teardown */
	struct scheduled_tick* scheduled_ticks;
	// Drain-only (all 0 outside server_world_drain_scheduled): the drain leaves the
	// originals it does not fire (not yet due, freeze-retained, budget-deferred) IN
	// PLACE behind the head instead of re-appending them, so Beta's (scheduledTime,
	// tickEntryID) order needs no permutation. kept_span = slots popped this drain,
	// i.e. the region [head - kept_span, head); kept = how many of them are still
	// pending (flagged SCHEDULED_TICK_FLAG_KEPT, still in the containment set).
	// Occupancy for the capacity check is scheduled_tick_count + scheduled_tick_kept.
	// drain_mode: 0 idle, 1 in place, 2 the saturated-ring re-append fallback.
	size_t scheduled_tick_kept;
	size_t scheduled_tick_kept_span;
	uint8_t scheduled_tick_drain_mode;
	// Beta World.scheduledTickSet: an open-addressing hash set over the pending entries, keyed
	// (x,y,z,block_type) like NextTickListEntry.equals/hashCode. Slot value 0 = empty,
	// 0xFFFF = tombstone, else the entry's PHYSICAL ring index + 1. Same allocation as
	// scheduled_ticks; scheduled_tick_hash_cap is a power of two >= 2 * capacity;
	// scheduled_tick_hash_used counts live + tombstone slots (rebuild trigger).
	uint16_t* scheduled_tick_hash;
	size_t scheduled_tick_hash_cap;
	size_t scheduled_tick_hash_used;
	size_t scheduled_tick_capacity;
	size_t scheduled_tick_count;
	size_t scheduled_tick_head;
	uint32_t current_tick;
	uint32_t scheduled_tick_drops;
	uint32_t explosions_this_tick;
	uint32_t explosion_deferrals;
	bool day_phase_initialized;
	bool is_day_phase;
	/* Populate rollback starts while the load screen is streaming/meshing its
	 * destination ring. Reserve a complete four-chunk block/metadata snapshot
	 * before that MEM2 peak, so block rollback never grows an allocator-backed
	 * journal. The private layout and exclusive attachment live in server_world.c. */
	struct server_world_populate_scratch* populate_scratch_storage;
	bool populate_scratch_owned;
	// Generation throttle (server_world_ensure_populated). A frontier crossing
	// can otherwise cascade up to ~9 chunk generations + 4 populate passes in a
	// single blocking call, starving the render thread on the one Wii core. The
	// throttle is ARMED only during that cascade. The hard count cap is
	// unconditional; elapsed time may defer work only after one indivisible disk/
	// generation/populate attempt finishes, so slow SD metadata reads cannot
	// reject the first real generation and failures cannot multiply in one call.
	// Deferred chunks are simply generated on a later cycle --
	// terrain-only chunks persist and populate runs once a 2x2 quadrant is
	// complete, so output is unchanged; only the work is spread across cycles.
	ptime_t gen_window_start;
	/* Near and far streaming each retain one unfinished 3x3 cascade.
	 * Eviction must retain that work across budgeted retries; a new target
	 * replaces the pin and successful completion releases it. */
	bool gen_pending[2];
	w_coord_t gen_pending_x[2], gen_pending_z[2];
	bool gen_throttle_armed;
	/* Tick-first pacing (2026-09-10): per-cascade generation budget in ms;
	 * 0 = SERVER_GEN_BUDGET_MS. The server thread lowers it while ticks are
	 * backlogged so streaming never runs a full pass between two due ticks. */
	int gen_budget_ms;
	int gen_attempts_window; // disk/generation/populate attempts, successful or failed
	int gen_this_window;     // completed terrain generations + populate transactions
	int gen_deferred_window; // work attempts deferred or transaction-failed
};

/* Hot simulation checks must reload this byte after nested callbacks. */
static inline bool server_world_simulation_stopped(const struct server_world* w) {
	return w && w->simulation_stopped;
}
void server_world_stop_simulation(struct server_world* w, uint8_t reason);
/* Explicit owner before any constructor exposure; NULL scratch owns storage. */
void server_world_create_owned(struct server_world* w, string_t level_name,
	enum world_dim dimension, int64_t seed, bool superflat,
	struct server_local* session, uint8_t slot);
bool server_world_create_owned_with_populate_scratch(
	struct server_world* w, string_t level_name, enum world_dim dimension,
	int64_t seed, bool superflat, struct server_world_populate_scratch* scratch,
	struct server_local* session, uint8_t slot);
#ifdef CAVEWII_SERVER_LOCAL_TEST
extern void (*server_world_test_create_hook)(struct server_world* w);
extern void (*server_world_test_fluid_tail_hook)(struct server_world* w, bool reactivation);
#endif
void server_world_create(struct server_world* w, string_t level_name,
						 enum world_dim dimension, int64_t seed, bool superflat);
/* A local server reserves this complete four-chunk rollback snapshot once for
 * its whole session and lends it exclusively to each dimension world. The
 * borrowed create returns false if the handle is already attached. Standalone/
 * test worlds use server_world_create, which owns an equivalent reservation. */
struct server_world_populate_scratch*
server_world_populate_scratch_create(void);
void server_world_populate_scratch_destroy(
	struct server_world_populate_scratch* storage);
bool server_world_create_with_populate_scratch(
	struct server_world* w, string_t level_name, enum world_dim dimension,
	int64_t seed, bool superflat,
	struct server_world_populate_scratch* populate_scratch);
void server_world_destroy(struct server_world* w);
// Release all in-memory world resources without writing resident chunks. Used
// only after a checked checkpoint, or to abandon an uncommitted destination.
void server_world_discard(struct server_world* w);
/* residency-1005/4 closure item 12: the runtime residency fault. While set,
 * a load/generation attempt of one of these <= 4 columns of the world
 * instance (slot, generation) FAILS like a failed generation attempt (the
 * world's existing backoff applies); every other column, world and
 * generation loads normally. Default empty (one branch on the load path). */
void server_world_rz_fault_set(uint8_t world_index, uint32_t generation,
	const int32_t* cx, const int32_t* cz, unsigned n);
void server_world_rz_fault_clear(void);
unsigned server_world_rz_fault_blocked(void); /* failed attempts so far */
/* True only between a successful create and the matching destroy/discard, i.e.
 * exactly while `w->chunks` may be read. Every consumer that can observe a
 * server_world it does not own the lifecycle of -- the per-tick observer hooks
 * above all -- MUST gate its dict access on this. See struct server_world's
 * chunks_ready comment for the failure this closes. */
bool server_world_chunks_ready(const struct server_world* w);
/* A1a: the handle naming this live instance ({world_index, generation}); a
 * world that is not live yields generation 0, which never resolves. */
struct server_world_handle server_world_handle_of(const struct server_world* w);
/* A1b (D10): retained-record bytes charged against the shared
 * SERVER_WORLD_RETIRING_MAX_BYTES budget -- the sum over every constructed
 * world of w's session (w alone when it has no session). */
size_t server_world_session_retiring_bytes(const struct server_world* w);

/* Dormant fixture entry point: invoke the production admission relight over
 * an already resident 2x2. No alternate predicates/flood implementation. */
bool server_world_test_relight_columns(struct server_world* w, w_coord_t x, w_coord_t z);

// Last generation-window stats for the F3 overlay (the local server runs in the
// same process). ms = total wall time inside the last ensure_populated, including
// disk probes/loads; generated = completed terrain/populate units; deferred =
// rejected or failed work attempts (not unique chunks). A routinely non-zero
// deferred count is the signal to inspect/widen the budget on real hardware.
void server_world_gen_stats(int* ms, int* generated, int* deferred);
int server_world_light_flood_us(void);
/* G9 light queue. server_world_light_drain settles every pending seed of `w`
 * (MinecraftServer.doTick:347 `while (func_6156_d())`); the tick calls it
 * after the scheduled + random block ticks and before entities. */
enum server_light_fault {
	SERVER_LIGHT_FAULT_NONE = 0,
	SERVER_LIGHT_FAULT_RESERVE,		  /* a write-site reservation failed */
	SERVER_LIGHT_FAULT_DRAIN_OOM,	  /* the drain frontier could not grow */
	SERVER_LIGHT_FAULT_DRAIN_CEILING, /* the drain hit its pop ceiling */
	SERVER_LIGHT_FAULT_PUBLICATION,   /* output exhausted; partial tick cannot save */
	SERVER_LIGHT_FAULT_POPULATION_STORAGE, /* G9X-5a: sticky population backing-I/O stop */
	SERVER_LIGHT_FAULT_CHECKPOINT_JOURNAL, /* G9X-5b: journal restore failed; sticky stop */
};
/* G9X-5a: the classified backing-I/O cause of a generation light-queue
 * failure (allocation failures are not storage causes). */
enum server_population_storage_cause {
	SERVER_POPULATION_STORAGE_NONE = 0,
	SERVER_POPULATION_STORAGE_OPEN,
	SERVER_POPULATION_STORAGE_SEEK,
	SERVER_POPULATION_STORAGE_READ,
	SERVER_POPULATION_STORAGE_SHORT_READ,
	SERVER_POPULATION_STORAGE_WRITE,
	SERVER_POPULATION_STORAGE_SHORT_WRITE,
	SERVER_POPULATION_STORAGE_FLUSH,
};
/* Where population met it. */
enum server_population_storage_site {
	SERVER_POPULATION_SITE_NONE = 0,
	SERVER_POPULATION_SITE_PROVISION, /* ensure_populated's own column load */
	SERVER_POPULATION_SITE_LOAD,      /* a quadrant's neighbour loads (incl. nested) */
	SERVER_POPULATION_SITE_TX_BEGIN,  /* the pre-transaction tail reload */
	SERVER_POPULATION_SITE_TX,        /* inside the transaction (rolled back) */
	SERVER_POPULATION_SITE_SNOW,      /* snow/commit (rolled back) */
	SERVER_POPULATION_SITE_TERRAIN,   /* a terrain generation's height-edge requests
	                                   * (the generated chunk discarded, never
	                                   * installed); (x, z) = that chunk */
};
/* Returns true when the queue is settled (empty). False = the drain stopped
 * (light_fault set, reason recorded, all work retained): the caller must not
 * run any consumer, save or publication of this world. */
bool server_world_light_drain(struct server_world* w);
/* G9 R3: true once w (or any world of its session) stopped its light: every
 * simulation loop (scheduled, random, tile callbacks, entities) ends at its
 * next iteration and the publication barrier is up. */
bool server_world_light_halted(const struct server_world* w);
bool server_world_light_pending(const struct server_world* w);
/* G9 R1: a durable non-evicting save of (x,z) that never waits for or
 * performs a live drain: a pinned column is written from a settled trial
 * image and the live world is rolled back (see checkpoint_chunks). */
bool server_world_save_chunk_coherent(struct server_world* w, w_coord_t x,
									  w_coord_t z);
/* True when chunk (cx,cz) or one of its 8 neighbours holds a pending seed:
 * save, unload, send and generation provision must wait for the drain. */
bool server_world_light_pinned(const struct server_world* w, w_coord_t cx,
							   w_coord_t cz);
struct server_world_light_stats {
	uint32_t drains, seeds_peak, undo_peak, drain_us_last, drain_us_max;
	uint64_t seeds_total;
	uint32_t provision_deferred, save_deferred, send_deferred, faults;
	uint32_t drain_oom, drain_ceiling; /* drain stops by cause */
	uint32_t drain_pops_max;		   /* largest completed drain (cells) */
	uint32_t checkpoint_trials;		   /* R1 settled-trial checkpoints */
	uint32_t trial_alloc_fail;		   /* R1 trial snapshot allocs failed */
	uint32_t evict_deferred;		   /* A2-W evictions waiting on a pin */
	uint32_t gen_order_held;		   /* populates held behind a light hold */
};
void server_world_light_stats(struct server_world_light_stats* out);
/* G9 R6 (ASTRA-G9-REVIEW-2 item 5): the Wii heartbeat view of the
 * pending-light queue, per world slot (index = world_index, slot >= 2 folds
 * into 1). All peaks/maxima are monotonic since boot (or the last test
 * reset); bytes/peak are the queue totals kept for tests and F3.
 *   seed_b / undo_b / pin_b: peak ALLOCATED capacity of each queue array
 *     (capacity x element size, not the used count);
 *   front_b: peak drain-frontier (flood scratch) allocation, sampled after
 *     every flood -- completed OR stopped (OOM / ceiling);
 *   ckpt_b: peak settled-trial checkpoint snapshot (Sky/Block of the 3x3
 *     around each pin + its chunk-id list) allocation;
 *   seeds_pk: most seeds pending at once; drain_us / drain_pops: the
 *     slowest / largest drain (completed or stopped);
 *   debt: most seeds still pending IMMEDIATELY after a prescribed drain
 *     boundary (server_world_light_note_boundary; 0 on every healthy run);
 *   tick_us: the largest per-update sum of drain + checkpoint-trial time;
 *   ckpt_us: the slowest checkpoint trial (snapshot, drain, saves,
 *     rollback);
 *   faults/ceil/oom: session-wide light stops by cause; ckpt_fail: trial
 *     snapshot allocation failures (the checkpoint failed retryably);
 *   trials: settled-trial checkpoints taken.
 * Written by the server thread, read racily by the heartbeat (each field is
 * a naturally aligned 32-bit word). */
struct server_world_light_hb {
	uint32_t bytes[2], peak[2];
	uint32_t seed_b[2], undo_b[2], pin_b[2], front_b[2], ckpt_b[2];
	uint32_t seeds_pk[2], drain_us[2], drain_pops[2], debt[2];
	uint32_t tick_us, ckpt_us;
	uint32_t faults, ceil, oom, ckpt_fail, trials;
};
void server_world_light_hb(struct server_world_light_hb* out);
/* G9 R6 paired elapsed intervals (not CPU time). us includes undo + flood + scratch trim.
 * yield_us sums yield calls, lock_us mutex acquisitions, recovery_us sleeps.
 * Residual time includes preemption away from these calls. */
struct server_world_light_drain_rec {
	uint32_t seeds, pops, pushes, writes, depth_peak, us;
	uint32_t slices, slice_min_us, slice_max_us;
	uint32_t yield_us, lock_us, recovery_us;
	uint32_t resolves, resolve_misses;
	/* Probe-gated kernel-only cache census; zero with the probe OFF.
	 * Excludes undo restoration, checkpoint work and post-flood trimming. */
	uint64_t kernel_resolve_hits, kernel_resolve_misses;
	uintptr_t frontier_address, seeds_address;
	/* ci-1101/37: this drain's longest >= 10 ms mutex wait, when one happened
	 * (hw1101c: a 71,206 us lock_us inside the server tick's light drain).
	 * wait_mutex is the contended mutex's address low 32 bits; wait_stripe and
	 * wait_mutex_name name it once it is stripe-registered (0/"" when not);
	 * wait_holder_id is the stripe_thread_id() of the holder (0 free/unknown)
	 * and wait_holder_tag its enum thread_stripe_tag.
	 * wait_us 0 means every acquisition in this drain waited < 10 ms. */
	uint32_t wait_us, wait_mutex, wait_holder_id, wait_holder_tag, wait_stripe;
	char wait_mutex_name[THREAD_DRAIN_WAIT_MUTEX_NAME_MAX];
};
/* The heaviest (largest us) drain per world slot since the previous call
 * (a window: the caller is the once-per-heartbeat reader), plus the
 * monotonic drain count and pop total per slot. Reads and clears the window;
 * worst-ever is the max over the windows. The checkpoint trial's drain is
 * not a window candidate (ckpt_us covers it). */
/* ASTRA-G9-R5 points 3/10: the settled-trial checkpoint (coherent save /
 * portal / death journal) split into its parts, per window maxima,
 * unsaturated: total us (begin -> end), the pinned-neighbourhood snapshot
 * loop, the trial drain (us / pops), the save(s) between begin and end and
 * the restore loop; plus the number of trials in the window. Temporary
 * attribution instruments to select the repair (order 3). */
struct server_world_light_ckpt_rec {
	uint32_t us, snap_us, drain_us, drain_pops, save_us, restore_us, n;
	/* G9X-5b: checkpoints that took the on-disk journal, and the window
	 * maxima of its synchronous SD phases (write + flush, read-back verify,
	 * restore); the hardware 50 ms gate is judged on these. */
	uint32_t journal_n, journal_prepare_us, journal_verify_us, journal_restore_us;
};
struct server_world_light_drain_hb {
	struct server_world_light_drain_rec win[2];
	uint32_t drains[2], pops_total[2];
	struct server_world_light_ckpt_rec ckpt;
	/* ci-1101/37: the LONGEST >= 10 ms drain mutex wait anywhere this window,
	 * with the contended mutex identified and its holder. Unlike win[] this is
	 * not tied to the heaviest drain, so a stall that lands on a lighter drain
	 * still names its lock. wait_us 0 = no such wait this window. */
	struct {
		uint32_t wait_us, mutex, holder_id, holder_tag, stripe;
		char name[THREAD_DRAIN_WAIT_MUTEX_NAME_MAX];
	} wait_attrib;
};
void server_world_light_drain_hb_take(struct server_world_light_drain_hb* out);
/* The most recent completed drain record (tests, benches, F3). */
void server_world_light_drain_last(struct server_world_light_drain_rec* out);
/* Opt-in boot self-test, before gfx/workers; emits paired diagnostic rows. */
bool server_world_g9_diagnostic(bool (*emit)(const char*));
/* G9 R6: a prescribed drain boundary of `w` just ran (the caller's drain
 * returned): record the pending seeds left right after it (the debt). */
void server_world_light_note_boundary(const struct server_world* w);
/* G9 R6: a server update starts: the previous update's summed light time
 * becomes a tick_us candidate. */
void server_world_light_tick_begin(void);
/* A chunk send skipped because the column is pinned (counted for F3/tests). */
void server_world_light_note_send_deferred(void);
/* An eviction skipped because the column is pinned (A2-W streamer). */
void server_world_light_note_evict_deferred(void);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Override the drain pop ceiling (0 = the production per-seed budget). */
void server_world_test_light_drain_ceiling(size_t pops);
#endif
// Max wall-time (us) of a single top-level neighbour-notify cascade. Surfaced on F3 ("ncas")
// to confirm the redstone place/break freeze: a value approaching ~3e6 us on real Wii means
// the synchronous cascade is the watchdog stall the thread_yield pacing now breaks up.
#define SERVER_WORLD_NOTIFY_MAX_DEPTH 16
uint32_t server_world_notify_cascade_max_us(void);
int server_world_notify_depth_max(void);
int server_world_notify_depth_cap_hits(void);
int server_world_notify_cascade_max_dispatches(void);
// Monotonic handler-dispatch total since boot (NOT reset by diag_reset): the
// hw-run heartbeat "ndisp" scenario signal -- a running redstone clock keeps it
// growing, so the collector can prove the stressor engaged rather than just
// that the console survived.
uint32_t server_world_notify_dispatch_total(void);
// Beta onNeighborBlockChange's var5 (the CHANGED block's id): published by both notify
// dispatchers immediately before each handler call; consumers (BlockDoor's canProvidePower
// source gate) must read it at handler ENTRY. Server thread only. 0 = air/unknown.
uint8_t server_world_notify_origin(void);
void server_world_notify_origin_set(uint8_t type);
void server_world_notify_diag_reset(void);
// P0 perf instruments (debug overlay), see server_world.c. edit_stats returns the
// last tick's set_block wall time (us) + write count and get_block fetch count;
// perf_tick_reset latches+zeroes those at the tick boundary (server_local_update).
// Tier-0 chunk-resolution cache hit rate (percent, previous tick).
int server_world_chunkres_hit_pct(void);
void server_world_edit_stats(int* setblock_us, int* setblock_n, int* setblock_max_us,
							 int* getblock_n, int* getblock_max_n);
void server_world_perf_tick_reset(void);
// Stress counters: explosion deferrals (cap saturation), scheduled-tick queue depth
// (a correctness ceiling), resident server-chunk count (evictor health). Any NULL.
void server_world_perf_counters(struct server_world* w, int* explosion_deferrals,
								int* sched_ticks, int* resident_chunks);
// Process-lifetime count of server_world_region_file_status's stat(2) calls
// (2026-09-01 region-probe-order T1 fix). Mirrors region_archive_io_stats's
// fopen_n idiom -- a plain always-compiled counter, no reset needed since
// every test process starts fresh. Proves server_world_load_chunk's
// LRU-first probe actually skips the stat when the owning region is already
// open, rather than merely reading that way by inspection.
uint32_t server_world_region_stat_count(void);

bool server_world_get_block(struct server_world* w, w_coord_t x, w_coord_t y,
							w_coord_t z, struct block_data* blk);
// server_world_is_chunk_loaded's answer via the cached chunk resolver
// (chunk coords). next-gaps-0924/W1C-b.
bool server_world_chunk_resident(struct server_world* w, w_coord_t cx,
								 w_coord_t cz);
/* Beta World.isMaterialInBB(..., Material.lava): floor(min),
 * floor(max + 1.0), half-open loops, first flowing/still lava hit wins.
 * A cell in a non-resident column reads as no lava (the Beta dummy-chunk
 * answer); callers that must not guess use server_world_aabb_lava_scan. */
bool server_world_aabb_has_lava(struct server_world* w,
	double min_x, double min_y, double min_z,
	double max_x, double max_y, double max_z);
/* residency-1005 (ASTRA-1005-RULING.md:2): the tri-state answer of a
 * material scan. PRESENT = a resident cell holds the material; ABSENT = the
 * scan covered EVERY in-range cell with resident data and found none;
 * UNKNOWN = no hit among the resident cells but at least one in-range cell's
 * column is not resident. UNKNOWN is neither air nor solid. Cells outside
 * 0..WORLD_HEIGHT-1 are known air (Beta World.getBlockId returns 0 there
 * before any chunk lookup), never UNKNOWN. */
enum server_world_scan {
	SERVER_WORLD_SCAN_ABSENT = 0,
	SERVER_WORLD_SCAN_PRESENT,
	SERVER_WORLD_SCAN_UNKNOWN,
};
/* server_world_aabb_has_lava's exact cell walk with the tri-state answer:
 * has_lava == (scan == PRESENT), byte-for-byte. */
enum server_world_scan server_world_aabb_lava_scan(struct server_world* w,
	double min_x, double min_y, double min_z,
	double max_x, double max_y, double max_z);
/* residency-1005: true iff every chunk column touched by the block-space XZ
 * span floor(min)..floor(max) (inclusive) is resident. Non-finite input or a
 * span wider than 4 chunks per axis (never a body footprint) reads false. */
bool server_world_columns_resident(struct server_world* w, double min_x,
	double min_z, double max_x, double max_z);
// Column heightmap (top non-transparent block + 1). foot_y >= this == Beta's
// canBlockSeeTheSky(x, foot_y, z): nothing opaque above the column. Returns false
// if the chunk isn't loaded.
bool server_world_get_height(struct server_world* w, w_coord_t x, w_coord_t z,
							 uint8_t* out_height);
/* ===== A1a explicit-world mutation contract (split-dimensions-plan-0924, D1)
 * Every mutation below names the world it mutates; session services (entity
 * table, RNG streams, sound/client sinks) are derived through w->session.
 * Coordinates never select a dimension. Lighting, scheduled ticks,
 * explosion counters, drops and client publication follow `w`. Legacy callers
 * that have only a server_local pass &s->worlds[0] (inventoried in the A1a
 * result; converted by D4/A1b/A1c). Block callbacks are dispatched only for
 * the world they can reach (slot 0) until D4 -- see server_world.c's bridge
 * comment; that is an explicit A2 activation blocker. */
bool server_world_set_block(struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z, struct block_data blk);
/* Beta World.setBlockWithNotify: ID-only no-op, metadata reset on a change. */
bool server_world_set_block_plain(struct server_world* w, w_coord_t x,
	w_coord_t y, w_coord_t z, uint8_t type);
/* Beta World.setBlockAndMetadata + the caller's local client mark:
 * Chunk removal/metadata/height/Sky+Block/metadata/on-added, without World
 * neighbour notification. Identity is a no-op; same-ID changed metadata lights.
 * RetroMCP server World.java:192-205, Chunk.java:215-250. */
bool server_world_set_block_quiet(struct server_world* w, w_coord_t x,
	w_coord_t y, w_coord_t z, struct block_data blk);
// Raw write: set the block + tell the client, but WITHOUT lifecycle or lighting
// (the redstone wire flood uses this -- it manages wire propagation itself + notifies non-wire
// neighbours explicitly). Metadata adapters and deliberate raw fixtures only.
bool server_world_set_block_silent(struct server_world* w, w_coord_t x, w_coord_t y,
								   w_coord_t z, struct block_data blk);
/* Beta World.setBlockMetadata: write only the metadata nibble, mark the chunk
 * modified, and perform no lifecycle, lighting, client, or neighbour work.
 * Unlike setBlockAndMetadata, Chunk.setBlockMetadata succeeds even when the
 * nibble already has that value. */
bool server_world_set_block_metadata_quiet(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	uint8_t metadata);
/* Beta's default setBlockMetadataWithNotify branch. The metadata primitive
 * above runs first (including for an identical value), then the live block is
 * published and its six neighbours are notified. Piston bases use the default
 * Block metadata-notify policy; blocks that disabled client marking retain
 * their existing specialized paths. */
bool server_world_set_block_metadata_with_notify(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	uint8_t metadata);
/* Beta's no-neighbour moving-block install: id 36 + stored metadata and its TE
 * state travel in one CRPC, with the block lighting update completed first. */
bool server_world_set_piston_moving(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct piston_moving_state* state);
/* Re-publish exact progress after a TE tick without emitting a duplicate
 * CRPC_SET_BLOCK. The client atomically upserts state before touching the cell. */
bool server_world_send_piston_moving(
	struct server_world* w, w_coord_t x, w_coord_t y, w_coord_t z,
	const struct piston_moving_state* state);

#ifdef CAVEWII_FLUID_LIFECYCLE_TEST
// Test-only entry into the production raw-populate immediate-fluid adapter.
// The caller must have the quadrant (px,pz)..(px+1,pz+1) resident.
enum server_world_test_gen_elapsed_mode {
	SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL,
	SERVER_WORLD_TEST_GEN_ELAPSED_FORCE,
	SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE,
};
void server_world_test_set_gen_elapsed_mode(
	enum server_world_test_gen_elapsed_mode mode);
void server_world_test_fail_gen_attempts(unsigned count);
unsigned server_world_test_gen_failures_remaining(void);
/* Test seam: the next `count` loads fail after the chunk is installed (the
 * failed-entity-restore shape); the retained retiring record must survive. */
void server_world_test_fail_loads_after_install(unsigned count);
unsigned server_world_test_fail_loads_after_install_remaining(void);
void server_world_test_populate_scratch_stats_reset(void);
void server_world_test_populate_scratch_stats(
	size_t* scratch_claims, size_t* committed_writes,
	size_t* max_writes);
bool server_world_test_populate_scratch_in_use(
	const struct server_world* world);
bool server_world_test_populate_snapshot_rollback(
	struct server_world* w, w_coord_t px, w_coord_t pz,
	size_t unique_writes);
size_t server_world_test_populate_fluid_peak_actions(void);
void server_world_test_fail_populate_fluid_action_reserve_above(
	size_t action_count);
size_t server_world_test_populate_fluid_failed_after_writes(void);
void server_world_test_populate_fluid_failed_staged(
	size_t* chests, size_t* drops, uint32_t* lava_mix_effects);
bool server_world_test_populate_fluid_immediate(
	struct server_world* w, w_coord_t px, w_coord_t pz, int x, int y, int z,
	uint8_t type, struct java_random* feature_rng);
struct server_world_test_fluid_source {
	int x, y, z;
	uint8_t type;
};
/* Script several generated sources inside ONE logical quadrant transaction.
 * marker is an earlier decoration write; stage_chest exercises deferred dungeon
 * inventory publication.  Used only by the allocation-failure regression. */
bool server_world_test_populate_fluid_transaction(
	struct server_world* w, w_coord_t px, w_coord_t pz,
	int marker_x, int marker_y, int marker_z, uint8_t marker_type,
	uint8_t marker_meta, const struct server_world_test_fluid_source* sources,
	size_t source_count, struct java_random* feature_rng, bool stage_chest);
/* G9X-2: scripted population setter calls through the production quadrant
 * writer/view/removal callbacks, one committed transaction per call. */
enum server_world_test_setter_api {
	SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, /* World.setBlockAndMetadata */
	SERVER_WORLD_TEST_SET_BLOCK,              /* World.setBlock (plain ID) */
	SERVER_WORLD_TEST_SET_BLOCK_METADATA,     /* World.setBlockMetadata */
};
struct server_world_test_setter_op {
	enum server_world_test_setter_api api;
	int x, y, z;
	uint8_t id, meta;
};
bool server_world_test_populate_setter_script(struct server_world* w,
	w_coord_t px, w_coord_t pz, const struct server_world_test_setter_op* ops,
	size_t count);
/* G9X-3: the same production writer, but the transaction is FAILED just
 * before ops[fail_before] (that op and every later one are still attempted,
 * so a failed write is exercised) and then rolled back. *writes_at_fail and
 * *writes_after report the transaction's recorded write count at the
 * injection point and after the last op. `observe` (may be NULL) is called
 * after EVERY op with the op index and the recorded write count, on the live
 * world and BEFORE the rollback, so the caller can inspect the bytes and
 * flags each op (failed ones included) left behind. Returns false only when
 * the quadrant is not resident or the transaction cannot begin. */
typedef void (*server_world_test_setter_observe_fn)(struct server_world* w,
	size_t op_index, size_t write_count, void* ctx);
bool server_world_test_populate_setter_script_fail(struct server_world* w,
	w_coord_t px, w_coord_t pz, const struct server_world_test_setter_op* ops,
	size_t count, size_t fail_before, size_t* writes_at_fail,
	size_t* writes_after, server_world_test_setter_observe_fn observe,
	void* observe_ctx);
/* G9X-4: population allocation fault points (test pool only). While a
 * population phase in phase_mask is open, EVERY light-region reservation,
 * fluid/sand notification-driver reservation, staged-drop reservation and
 * outside-chunk record allocation attempt is one numbered point; point
 * fail_at (0-based) fails as the allocator would there (-1 = never).
 * Arming (nonzero mask) resets the count, disarming (mask 0) keeps it;
 * _points() reports the attempts since the last arm. */
enum {
	SERVER_WORLD_TEST_POP_PHASE_DECORATE = 1, /* the scripted seam's ops */
	SERVER_WORLD_TEST_POP_PHASE_SNOW = 2,     /* snow + its notifications */
};
void server_world_test_populate_fault(unsigned phase_mask, long fail_at);
size_t server_world_test_populate_fault_points(void);
/* Called on the live, failed world at the start of every population
 * rollback, before anything is restored. NULL = off. */
extern void (*server_world_test_populate_rollback_hook)(struct server_world* w);
/* Called (read-only) after every population World.setBlockWithNotify
 * call, its notification cascade included, has returned: an external-call
 * boundary for the ordered witness. NULL = off. */
extern void (*server_world_test_notify_boundary_hook)(struct server_world* w,
	int x, int y, int z);
struct server_world_test_notify_op {
	int x, y, z;
	uint8_t id;
};
/* Scripted decorate-stage World.setBlockWithNotify calls through the
 * production notifying view hook, then the production snow-and-commit tail
 * (snow only in the Overworld, with temp_grid[256] as the +8 temperature
 * field), in one real population transaction. *committed reports whether it
 * committed (false = rolled back). Returns false only when the quadrant is
 * not resident or the transaction cannot begin. */
bool server_world_test_populate_snow_script(struct server_world* w,
	w_coord_t px, w_coord_t pz, const struct server_world_test_notify_op* ops,
	size_t count, const double* temp_grid, bool* committed);
/* The production population of quadrant (px,pz); true iff it ran fully. */
bool server_world_test_populate_quadrant(struct server_world* w,
	w_coord_t px, w_coord_t pz);
bool server_world_test_generation_light_region(const struct server_world* w,
	size_t i, int out[7]);
/* G9X-5 bounded generation light queue (test pool only). */
struct server_world_light_queue_stats {
	size_t resident_bytes, meta_bytes, ckpt_bytes;   /* live, this world */
	size_t spills, loads, spilled_pages, file_pages; /* this world */
	size_t peak_logical;                             /* this world */
	size_t subsystem_bytes, subsystem_peak;          /* every world + ckpt copy */
	size_t world_resident_peak, ckpt_peak;           /* max over worlds / trials */
	/* the PPC (Wii) allocator ledger of the same allocations: mem2_alloc's
	 * MEM2_GUARD + lwp block overhead per page, newlib malloc overhead,
	 * the FILE/devoptab state, PPC pointer and struct sizes */
	size_t wii_bytes, wii_peak;
	size_t storage_failures, alloc_failures;  /* backing I/O vs allocation */
	unsigned last_fail_site;                  /* enum server_world_test_lq_site */
	unsigned last_fail_cause;                 /* enum server_population_storage_cause */
};
void server_world_test_light_queue_stats(const struct server_world* w,
	struct server_world_light_queue_stats* out);
void server_world_test_light_queue_stats_reset(void);
/* Resident slots of w's ring (128 = production cap; 8192 = uncapped
 * reference). Only while the queue is empty and nothing is spilled. */
bool server_world_test_light_queue_slots(struct server_world* w, uint32_t slots);
enum server_world_test_lq_fault {
	SERVER_WORLD_TEST_LQ_OPEN, SERVER_WORLD_TEST_LQ_WRITE,
	SERVER_WORLD_TEST_LQ_READ, SERVER_WORLD_TEST_LQ_ALLOC,
	SERVER_WORLD_TEST_LQ_RESERVE, /* logical: the Nth reserve call fails */
	SERVER_WORLD_TEST_LQ_SHORT_WRITE, /* half the page really written, then failure */
	SERVER_WORLD_TEST_LQ_SHORT_READ,  /* half the page really read, then failure */
	SERVER_WORLD_TEST_LQ_SEEK,        /* the Nth seek (spill or load) fails */
	SERVER_WORLD_TEST_LQ_FLUSH,       /* the page is written, the flush fails */
	SERVER_WORLD_TEST_LQ_FAULTS
};
/* Where the last storage failure happened (spill-side failures refuse the
 * reserve call; load-side failures stop before a pop). */
enum server_world_test_lq_site {
	SERVER_WORLD_TEST_LQ_SITE_NONE, SERVER_WORLD_TEST_LQ_SITE_SPILL,
	SERVER_WORLD_TEST_LQ_SITE_LOAD, SERVER_WORLD_TEST_LQ_SITE_OPEN,
	SERVER_WORLD_TEST_LQ_SITE_ALLOC
};
/* The next reserve call first spills one spillable page (logically
 * invisible), so a storage fault lands at that exact call. */
void server_world_test_light_queue_force_spill(bool on);
/* G9X-5a read-only view of w's sticky POPULATION_STORAGE_FAILED state. */
struct server_world_test_gen_storage {
	bool failed;
	unsigned cause, site; /* enum server_population_storage_cause / _site */
	int x, z;
	size_t io_fails;      /* w's backing I/O failures so far */
};
void server_world_test_gen_storage(const struct server_world* w,
	struct server_world_test_gen_storage* out);
/* G9X-5a copy-on-write witness: the raw bytes (3,584) of physical slot
 * phys_slot (0/1) of w's backing page `page` (out may be NULL), its current
 * slot bit, its "moved during this trial" bit and whether a trial owns the
 * bits. False when w has no backing or that slot was never written. */
bool server_world_test_light_queue_backing_page(const struct server_world* w,
	size_t page, unsigned phys_slot, void* out, int* loc, bool* flipped, bool* trial);
/* Fail the at-th (0-based) attempt of `kind` (-1 = never); arming resets
 * that kind's count. _count reports attempts since the last arm. */
void server_world_test_light_queue_fault(enum server_world_test_lq_fault kind, long at);
size_t server_world_test_light_queue_fault_count(enum server_world_test_lq_fault kind);
/* The production World.func_484_a admission of one request. */
bool server_world_test_generation_light_schedule(struct server_world* w,
	int channel, int x1, int y1, int z1, int x2, int y2, int z2);
/* The backing file path of w ("" when none is open). */
const char* server_world_test_light_queue_backing_path(const struct server_world* w);
#endif

bool server_world_furthest_chunk(struct server_world* w, w_coord_t dist,
								 w_coord_t pin_x, w_coord_t pin_z,
								 w_coord_t pin_r,
								 w_coord_t px, w_coord_t pz, w_coord_t* x,
								 w_coord_t* z);

bool server_world_is_chunk_loaded(struct server_world* w, w_coord_t x,
								  w_coord_t z);
bool server_world_is_chunk_sent(struct server_world* w, w_coord_t x,
								w_coord_t z);

// M1 deep reach far-sent tracking (x,z in CHUNK coords).
void server_world_mark_far_sent(struct server_world* w, w_coord_t x, w_coord_t z);
void server_world_clear_far_sent(struct server_world* w, w_coord_t x, w_coord_t z);
bool server_world_is_far_sent(struct server_world* w, w_coord_t x, w_coord_t z);
// Extract the compact far-ring surface (per column: highest visible water/lava
// above the opaque skin, otherwise the top non-see-through block used by the
// client's lod_surface_height, plus biome tint)
// from a FULLY-POPULATED chunk into `out`. Reads sc->ids; does not make sc resident.
void server_world_extract_far_column(struct server_world* w,
									 struct server_chunk* sc, w_coord_t cx,
									 w_coord_t cz, struct far_rpc_slot* out);
// CHEAP far-skin: generate the target chunk ALONE (no populate cascade, no resident
// insert) + extract its skin into `out`, then free the transient chunk. ~10x cheaper
// than ensure_populated; a render-only LOD path that drops decoration (trees/snow/
// lakes) from the distant silhouette but keeps the simulation byte-identical (the
// chunk is never populated -> promotion regenerates the full faithful world). Returns
// false if there is no generator. The caller bounds per-tick count + time budget.
// trunk_stub overlays an APPROXIMATE tree-trunk silhouette (render-only LOD; not real
// tree positions -- they reposition on promotion) so far forests aren't bald.
bool server_world_extract_far_skin(struct server_world* w, w_coord_t cx,
								   w_coord_t cz, struct far_rpc_slot* out,
								   bool trunk_stub);
#ifdef CAVEWII_SERVER_LOCAL_TEST
extern unsigned server_world_test_population_calls;
/* R5: declared structural fixture construction for the retained G2 debt
 * diagnostic only. Real history/parity captures use the normal load API. */
bool server_world_test_load_structural_chunk(struct server_world* w,
	w_coord_t x, w_coord_t z, struct server_chunk** sc);
#endif
bool server_world_load_chunk(struct server_world* w, w_coord_t x, w_coord_t z,
							 struct server_chunk** sc);
// Ensure chunk (cx,cz) is fully decorated and resident: runs the four Beta
// populate() quadrants whose 2x2 area covers it (generating the surrounding
// terrain on demand), and returns its server_chunk in *sc. The chunk is then
// safe to send. Worlds without a generator (disk-only / Nether) just load it.
bool server_world_ensure_populated(struct server_world* w,
								   struct server_local* s, w_coord_t cx,
								   w_coord_t cz, struct server_chunk** sc);
/* Independent far-stream retry ownership; same generator and output rules. */
bool server_world_ensure_populated_far(struct server_world* w,
								   struct server_local* s, w_coord_t cx,
								   w_coord_t cz, struct server_chunk** sc);
// Returns false when gathering or committing any owned chunk state fails. With
// erase=true, failure leaves the chunk and all runtime owners resident.
bool server_world_save_chunk(struct server_world* w, bool erase, w_coord_t x,
							 w_coord_t z);
bool server_world_save_chunk_obj(struct server_world* w, bool erase,
								 w_coord_t x, w_coord_t z,
								 struct server_chunk* c);
// Checkpoint every resident chunk's tile entities, dynamic entities, and
// primed-TNT fuse state. Untouched chunks with no owned/persisted dynamic state
// are sparse no-ops. Returns false if any gather or required region write fails;
// no chunks are evicted, so the live world remains retryable.
/* M3: settle every chunk waiting on a publication -- clear the dirty bit and
 * run the deferred eviction for a durable batch, re-dirty for a failed one.
 * Server thread only; called at the save-batch seams and the sync barrier. */
void server_world_reconcile_pending_saves(struct server_world* w);
bool server_world_checkpoint_chunks(struct server_world* w);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* IMP-QUIT-UNSAVABLE: while armed, checkpoint passes refuse to save chunk
 * (x, z) exactly like a fail-closed entity_preserve (nothing written, the
 * chunk stays dirty and resident). Disarm with on=false. */
void server_world_test_unsavable_chunk(bool on, w_coord_t x, w_coord_t z);
#endif
/* R24: REJECTED retains the caller's logical checkpoint for admission retry.
 * PENDING is queue acceptance only. The server owns token/generation/handles. */
enum region_checkpoint_state server_world_checkpoint_submit(
	struct server_world* w, w_coord_t x, w_coord_t z, uint32_t logical_tick,
	struct server_checkpoint_token** token);
void server_world_checkpoint_poll(struct server_world* w);
bool server_world_checkpoint_drain(struct server_world* w);
#define SERVER_CHECKPOINT_STATS 12
void server_world_checkpoint_stats(uint32_t out[SERVER_CHECKPOINT_STATS]);
// Fsync every open region handle -- the checkpoint's durability barrier
// against power cuts while the region LRU holds files open (see
// region_archive_sync).
bool server_world_sync_regions(struct server_world* w);
/* SAVE BATCH seam (2026-09-07, far_walk pace regression). Bracket a run of
 * chunk saves so the journaled region-header publication -- a file create, a
 * rename, a remove and three fsyncs -- is paid ONCE for the batch instead of
 * once per chunk. Nested/repeated begins are harmless; _end publishes every
 * resident region archive and returns false if any publication failed (the
 * failing archive rolled its unpublished deltas back, so the affected chunks
 * hold their previous on-disk contents). Callers must pair them within a
 * bounded window -- SAVE_BATCH_MAX_TICKS ticks, ~1 s -- because an open batch
 * is exactly the pre-journal durability exposure: a power cut before _end
 * reverts the batch's chunks to their previous saved state. region_archive_sync and
 * region_archive_destroy publish independently, so a missed _end can never
 * lose a write across an LRU eviction, a checkpoint or a world teardown. */
void server_world_save_batch_begin(struct server_world* w);
bool server_world_save_batch_end(struct server_world* w);
/* S2: number of evicted chunks whose record is retained pending publication. */
size_t server_world_retiring_active(const struct server_world* w);

/* The five resident column arrays as ONE MEM2_POOL_COLUMN slot (2026-09-11,
 * Astra debate E): long-lived column storage no longer interleaves with
 * variable-size mesh blocks in the shared free list -- the measured spill
 * class at 42/256. alloc sets the five pointers (contents undefined) and
 * arrays_slot; free releases whichever shape the chunk has and NULLs all
 * five. Header-only: worldgen.c is compiled without server_world.c in the
 * worldgen benches/tests. */
#define SERVER_CHUNK_IDS_BYTES ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define SERVER_CHUNK_NIBBLE_BYTES ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2)
#define SERVER_CHUNK_HEIGHTMAP_BYTES ((size_t)CHUNK_SIZE * CHUNK_SIZE)
_Static_assert(SERVER_CHUNK_IDS_BYTES + 3 * SERVER_CHUNK_NIBBLE_BYTES
				   + SERVER_CHUNK_HEIGHTMAP_BYTES == MEM2_POOL_COLUMN_OBJECT,
			   "MEM2_POOL_COLUMN_OBJECT must equal the five column arrays");
static inline void server_chunk_arrays_free(struct server_chunk* sc) {
	if(sc->arrays_slot) {
		mem2_free_sized(sc->ids, MEM2_POOL_COLUMN_OBJECT);
	} else {
		if(sc->ids) mem2_free_sized(sc->ids, SERVER_CHUNK_IDS_BYTES);
		if(sc->metadata) mem2_free_sized(sc->metadata, SERVER_CHUNK_NIBBLE_BYTES);
		if(sc->lighting_sky) mem2_free_sized(sc->lighting_sky, SERVER_CHUNK_NIBBLE_BYTES);
		if(sc->lighting_torch) mem2_free_sized(sc->lighting_torch, SERVER_CHUNK_NIBBLE_BYTES);
		if(sc->heightmap) mem2_free_sized(sc->heightmap, SERVER_CHUNK_HEIGHTMAP_BYTES);
	}
	sc->ids = sc->metadata = sc->lighting_sky = sc->lighting_torch = NULL;
	sc->heightmap = NULL;
	sc->arrays_slot = false;
}
/* The Wii slot shape: one MEM2_POOL_COLUMN slot carries all five arrays. */
static inline bool server_chunk_arrays_alloc_slot(struct server_chunk* sc) {
	uint8_t* base = mem2_tag(
		mem2_alloc_pooled(MEM2_POOL_COLUMN_OBJECT, MEM2_POOL_COLUMN),
		MEM2_POOL_COLUMN_OBJECT, CARVE_COLUMN);
	if(!base) {
		sc->ids = sc->metadata = sc->lighting_sky = sc->lighting_torch = NULL;
		sc->heightmap = NULL;
		sc->arrays_slot = false;
		return false;
	}
	sc->ids = base;
	sc->metadata = base + SERVER_CHUNK_IDS_BYTES;
	sc->lighting_sky = sc->metadata + SERVER_CHUNK_NIBBLE_BYTES;
	sc->lighting_torch = sc->lighting_sky + SERVER_CHUNK_NIBBLE_BYTES;
	sc->heightmap = sc->lighting_torch + SERVER_CHUNK_NIBBLE_BYTES;
	sc->arrays_slot = true;
	return true;
}
static inline bool server_chunk_arrays_alloc(struct server_chunk* sc) {
#ifndef PLATFORM_WII
	/* colcap-1006 (R4 point 3) PC TEST SEAM: with CW_TEST_COLUMN_POOL set the
	 * PC build takes the Wii slot shape from a host copy of the Wii column
	 * pool (mem2.c), so world/light/mesh goldens run under both placements. */
	if(mem2_column_pool_emulated())
		return server_chunk_arrays_alloc_slot(sc);
	/* PC: no carve, no pools -- keep the five separate arrays so every
	 * existing PC test that frees them one by one stays valid. The slot shape
	 * is Wii-only; its free paths are the ones converted to
	 * server_chunk_arrays_free. */
	sc->ids = mem2_tag(mem2_alloc(SERVER_CHUNK_IDS_BYTES), SERVER_CHUNK_IDS_BYTES, CARVE_COLUMN);
	sc->metadata = mem2_tag(mem2_alloc(SERVER_CHUNK_NIBBLE_BYTES), SERVER_CHUNK_NIBBLE_BYTES, CARVE_COLUMN);
	sc->lighting_sky = mem2_tag(mem2_alloc(SERVER_CHUNK_NIBBLE_BYTES), SERVER_CHUNK_NIBBLE_BYTES, CARVE_COLUMN);
	sc->lighting_torch = mem2_tag(mem2_alloc(SERVER_CHUNK_NIBBLE_BYTES), SERVER_CHUNK_NIBBLE_BYTES, CARVE_COLUMN);
	sc->heightmap = mem2_tag(mem2_alloc(SERVER_CHUNK_HEIGHTMAP_BYTES), SERVER_CHUNK_HEIGHTMAP_BYTES, CARVE_COLUMN);
	sc->arrays_slot = false;
	if(!sc->ids || !sc->metadata || !sc->lighting_sky || !sc->lighting_torch
	   || !sc->heightmap) {
		server_chunk_arrays_free(sc);
		return false;
	}
	return true;
#else
	return server_chunk_arrays_alloc_slot(sc);
#endif
}
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Deterministic SD-latency proxy: count calls that reach region chunk encode/
 * commit, excluding sparse checkpoint skips and archive-open failures. */
void server_world_test_chunk_write_attempts_reset(void);
size_t server_world_test_chunk_write_attempts(void);
/* F2 test hook: force the next `count` blast staging mallocs to fail. */
void server_world_test_fail_blast_mallocs(size_t count);
#endif
struct region_archive* server_world_chunk_region(struct server_world* w,
												 w_coord_t x, w_coord_t z);
enum server_world_region_result {
	SERVER_WORLD_REGION_OK,
	SERVER_WORLD_REGION_ABSENT,
	SERVER_WORLD_REGION_ERROR,
};
/* Distinguish a genuinely absent region from an existing archive that could
 * not be opened or validated. `out` is non-NULL only for OK. */
enum server_world_region_result server_world_chunk_region_checked(
	struct server_world* w, w_coord_t x, w_coord_t z,
	struct region_archive** out);
/* A2-S: the save path's admission -- the cached handle for (x, z)'s region,
 * CREATING the region file through the same pool admission when it is
 * absent. Replaces the old uncached per-chunk temporary handle. */
struct region_archive* server_world_chunk_region_create(struct server_world* w,
														w_coord_t x,
														w_coord_t z);
/* A2-S accessors over this world's pool vector (acquisition order). */
size_t server_world_region_count(const struct server_world* w);
struct region_archive* server_world_region_at(struct server_world* w, size_t k);
struct region_pool* server_world_region_pool(struct server_world* w);
bool server_world_disk_has_chunk(struct server_world* w, w_coord_t x,
								 w_coord_t z);
void server_world_scheduled_ticks_create(struct server_world* w,
										 size_t capacity);
void server_world_scheduled_ticks_destroy(struct server_world* w);
/* Rebuild the (x,y,z,id) containment set from the ring. Required after any
 * direct write of scheduled_tick_head/count that bypasses the queue API (the
 * populate-tx rollback), otherwise the set and the ring disagree. */
void server_world_scheduled_ticks_resync(struct server_world* w);

// Drop the chunk-resolution cache. MUST be called around every mutation of
// w->chunks (insert/erase/clear): the dict stores values in-table, so a rehash
// or erase moves/frees the cached pointer (see the struct field comment).
void server_world_chunk_cache_invalidate(struct server_world* w);
/* Queue-only primitives: deliberately bypass Beta's +/-8 resident-area gate.
 * Production block/redstone code must use the checked functions below; these
 * raw forms exist for isolated queue/timing goldens with no chunk world. */
bool server_world_schedule_tick_at_raw(struct server_world* w,
									   struct scheduled_tick tick);
bool server_world_schedule_tick_raw(struct server_world* w, w_coord_t x,
									w_coord_t y, w_coord_t z,
									uint8_t block_type, uint32_t delay);
bool server_world_schedule_block_tick_raw(struct server_world* w,
										  w_coord_t x, w_coord_t y,
										  w_coord_t z,
										  uint8_t block_type);
/* Beta World.checkChunksExist for the scheduled-tick radius-8 box. Exported so
 * admission and firing share one exact predicate and its boundary golden can
 * pin the Y-overlap rule. */
bool server_world_scheduled_tick_area_loaded(struct server_world* w,
										 w_coord_t x, w_coord_t y,
										 w_coord_t z);
/* Same predicate, caller-supplied radius (BlockSand.tryToFall's own inline
 * radius-32 checkChunksExist, distinct from the radius-8 scheduling gate
 * above). See server_world_tick_queue.c. */
bool server_world_area_loaded_radius(struct server_world* w, w_coord_t x,
									 w_coord_t y, w_coord_t z,
									 uint32_t radius);
bool server_world_schedule_tick(struct server_world* w, w_coord_t x,
								w_coord_t y, w_coord_t z,
								uint8_t block_type, uint32_t delay);
bool server_world_schedule_block_tick(struct server_world* w, w_coord_t x,
									  w_coord_t y, w_coord_t z,
									  uint8_t block_type);
/* Checked load-reconstruction insertion (Beta keep-first (position,id) dedup,
 * like every other insert since ci-1101/39 retired the scan cutoff). */
bool server_world_schedule_block_tick_rearm(struct server_world* w,
										w_coord_t x, w_coord_t y,
										w_coord_t z,
										uint8_t block_type);
bool server_world_scheduled_tick_pop(struct server_world* w,
									 struct scheduled_tick* tick);
void server_world_scheduled_tick_requeue(struct server_world* w,
										 struct scheduled_tick tick);
// Drain this game-tick's READY scheduled ticks. `budget` is the Wii-specific cap on
// FLOWING water/lava fires only; once it is spent, those entries are requeued while
// ready non-fluid timing entries (fire and redstone) continue through the
// original queue snapshot. Future-dated entries are also requeued and consume neither
// cap. With budget != 0, total fires retain Beta 1.7.3's absolute 1000/tick ceiling;
// budget == 0 is the explicit legacy/unlimited diagnostic mode. `yield`, if non-NULL,
// is called every 4 fires. Pure-fluid conservation/fixed-point coverage proves that
// cohort settles unchanged. In a mixed backlog, later non-fluid work deliberately
// overtakes capped fluids and can change cross-class interaction order; that explicit
// Wii playability deviation is documented in profile.h and wii_controller_playability.md.
void server_world_drain_scheduled(
	struct server_world* w, struct server_local* s, uint32_t budget,
	void (*fire)(struct server_world*, struct server_local*, struct scheduled_tick),
	void (*yield)(void));
bool server_world_try_begin_explosion(struct server_world* w);
// Called exactly once at the top of each logical server_local_update, before
// RPC/entity work.
void server_world_begin_update(struct server_world* w);
void server_world_tick(struct server_world* w, struct server_local* s);
void server_world_random_tick(struct server_world* w, struct random_gen* g,
							  struct server_local* s, w_coord_t px,
							  w_coord_t pz, w_coord_t dist);
// Ownerless/direct explosions (Nether beds and debug fixtures) run synchronously,
// matching Beta and avoiding any volatile teardown queue. Entity-owned producers
// must use the retryable call below so the Wii pacing cap remains effective.
void server_world_explode(struct server_world* w, vec3 center, float power);
/* Explosion.java:13-26: center fields and constructor arguments are double. */
void server_world_explode_precise(struct server_world* w,
                                const double center[3], float power);
void server_world_explode_flaming_precise(struct server_world* w,
                                        const double center[3], float power);
// Retryable source contract: true means the blast ran synchronously; false means
// the Wii pacing cap is full and the caller must keep/retry its source entity.
// Primed TNT and fully-fused creepers use this so a portal/menu checkpoint can
// persist the source and cap saturation can never delete its pending blast.
bool server_world_try_explode(struct server_world* w, vec3 center, float power);
// Same retryable contract with Beta's `exploder` identity (World.createExplosion's
// Entity argument): the exploder is excluded from the entity pass and, being
// non-null, gives every EntityLiving it hurts the attackEntityFrom knockBack
// away from it. Beta passes `this` ONLY for the creeper [740c583
// EntityCreeper.attackEntity:101-103]; TNT, beds and fireballs pass null.
struct entity;
bool server_world_try_explode_by(struct server_world* w,
								 const struct entity* exploder, vec3 center,
								 float power);
// Per-blast cost record of the LAST explosion (any entry point), for the Wii
// budget question: grid rays cast, ray march steps, block lookups the march
// made, cells collected (air included) and mutated (non-air), plus the entity
// pass's density rays and the cells those rays tested. Test/bench instrument.
struct server_world_explosion_cost {
	uint32_t rays;
	uint32_t ray_steps;
	uint32_t block_lookups;
	uint32_t cells;
	uint32_t broken;
	uint32_t entities;
	uint32_t density_rays;
	uint32_t density_cells;
	uint32_t fire_lit; /* isFlaming cells lit (before doExplosionB removes them) */
};
/* F4-RNG: an independent runtime seed for a fresh `new Random()` (World.rand,
 * a body's Entity.rand, a per-explosion ExplosionRNG): Java's
 * seedUniquifier ^ clock form -- never the terrain seed, never another stream.
 * Clears the Gaussian cache (jrand_set_seed). */
void server_world_runtime_jrand_seed(struct java_random* r);

/* ---- G9X-5b: per-increment memory preflight (allocator model) ----------
 * A plan is the ORDERED list of allocations an operation will make; each is
 * replayed against a model of the Wii allocators BEFORE anything is
 * allocated, and the operation is refused when any prefix could breach a
 * floor. Requested bytes are not growth: a newlib malloc either reuses a
 * free chunk (no sbrk) or grows by its own page-rounded increment, which
 * libogc's _sbrk_r places WHOLE in Arena1 while it fits and otherwise in
 * Arena2 -- sticky (server_world.c, server_world_mem_plan_run). */
#define SERVER_WORLD_MEM1_FLOOR 4194304u  /* MEM1 free >= 4,096 KiB */
#define SERVER_WORLD_CARVE_FLOOR 2299904u /* carve free >= 2,246 KiB (+ reserve) */
enum server_world_mem_arena {
	SERVER_WORLD_MEM_UNSWITCHED, /* malloc growth still in Arena1 */
	SERVER_WORLD_MEM_SWITCHED,   /* growth already moved to Arena2 */
	SERVER_WORLD_MEM_UNKNOWN     /* either (_sbrk_r's mem2_start is private) */
};
struct server_world_mem_state {
	size_t mem1_free;   /* Arena1 hi - lo */
	size_t arena2_free; /* Arena2 hi - lo (the malloc spill above the carve) */
	size_t bins_free;   /* upper bound of reusable newlib free chunks (SIZE_MAX: unknown) */
	size_t top_free;    /* upper bound of the newlib top chunk (SIZE_MAX: unknown) */
	enum server_world_mem_arena arena;
	bool carve_valid;     /* the carve's free total and largest hole are known */
	size_t carve_free;    /* physical free bytes */
	size_t carve_largest; /* largest REQUEST one hole satisfies (payload + guard) */
	size_t carve_reserve; /* admission reserve not yet materialized */
};
enum server_world_mem_kind {
	SERVER_WORLD_MEM_EV_MALLOC,     /* malloc/calloc/realloc growth of `bytes` */
	SERVER_WORLD_MEM_EV_MEM2,       /* mem2_alloc: carve, else memalign fallback */
	SERVER_WORLD_MEM_EV_MEM2_STRICT /* mem2_alloc_strict: carve only */
};
struct server_world_mem_event {
	enum server_world_mem_kind kind;
	size_t bytes;
	size_t count; /* the same allocation repeated (0 counts as 1) */
};
enum server_world_mem_reason {
	SERVER_WORLD_MEM_OK,
	SERVER_WORLD_MEM_MEM1_FLOOR,    /* an Arena1 increment would leave < floor */
	SERVER_WORLD_MEM_ARENA2_FULL,   /* the increment fits neither arena */
	SERVER_WORLD_MEM_CARVE_FLOOR,   /* carve free would drop below floor + reserve */
	SERVER_WORLD_MEM_CARVE_LARGEST, /* no single hole takes payload + guard */
	SERVER_WORLD_MEM_CARVE_UNKNOWN, /* carve topology unknown: refuse */
	SERVER_WORLD_MEM_OVERFLOW,      /* a size computation would overflow */
	SERVER_WORLD_MEM_STALE,         /* pre-drain revalidation failed */
	SERVER_WORLD_MEM_UNBOUNDED      /* an allocation without a defensible bound */
};
enum server_world_mem_where {
	SERVER_WORLD_MEM_AT_NONE, SERVER_WORLD_MEM_AT_MEM1, SERVER_WORLD_MEM_AT_ARENA2,
	SERVER_WORLD_MEM_AT_EITHER, SERVER_WORLD_MEM_AT_CARVE
};
#define SERVER_WORLD_MEM_TRACE_MAX 24
struct server_world_mem_step {
	uint32_t event;   /* index into the event list */
	uint8_t kind;     /* enum server_world_mem_kind */
	uint8_t where;    /* enum server_world_mem_where of its growth */
	size_t requested; /* bytes asked for */
	size_t growth;    /* worst-case sbrk bytes (main increment + correction) */
	size_t carve_debit;
	size_t mem1_after, arena2_after, carve_after, largest_after;
};
struct server_world_mem_plan {
	bool ok;
	enum server_world_mem_reason reason;
	size_t fail_event;   /* event index of the refusal */
	size_t steps;        /* allocations replayed (trace keeps the first MAX) */
	struct server_world_mem_step step[SERVER_WORLD_MEM_TRACE_MAX];
	size_t mem1_min, carve_min; /* lowest modelled values over every prefix */
};
/* Replay `events` in order against `st`. Pure: no allocation, no sampling. */
bool server_world_mem_plan_run(const struct server_world_mem_state* st,
	const struct server_world_mem_event* events, size_t n,
	struct server_world_mem_plan* out);
/* G9X-5b part 2: the carve admission reserve of `reservations` transfer
 * column reservations when the column pool has `pool_free_slots` carved,
 * unused slots. A reservation a free slot covers needs no new carve. The
 * others take one of two production paths (mem2.c mem2_allocate_pooled):
 * while the pool is below its cap (`slab_room` more slabs) it first carves
 * a whole slab (`slab_request` bytes, MEM2_POOL_SLAB_SLOTS slots), which
 * succeeds whenever ONE hole fits it -- however close to the floor that
 * leaves the carve; when the slab cannot be carved, each falls back to one
 * unpooled MEM2_POOL_COLUMN_OBJECT. The reserve is the larger of the two. */
size_t server_world_mem_column_reserve(size_t reservations, size_t pool_free_slots,
	size_t slab_room, size_t slab_request);
/* How a checkpoint trial kept its before-image. */
enum server_world_ckpt_mode {
	SERVER_WORLD_CKPT_NONE,    /* no trial (nothing pending, refused or failed) */
	SERVER_WORLD_CKPT_FULL,    /* the in-RAM plane copy (admitted, or ungated) */
	SERVER_WORLD_CKPT_JOURNAL  /* the pre-reserved on-disk checkpoint journal */
};
/* The last checkpoint trial's preflight and journal record. */
struct server_world_test_mem_report {
	struct server_world_mem_plan plan;
	size_t refusals, revalidation_refusals, captures_skipped;
	enum server_world_ckpt_mode mode;
	size_t samples, sample_us;      /* sampler calls / time in the last trial */
	size_t journal_trials, journal_columns;
	uint64_t journal_written, journal_read; /* bytes, cumulative */
	size_t journal_reserve_fail, journal_prepare_fail, journal_restore_fail;
	size_t journal_reserve_refused; /* the reservation plan breached a floor */
	size_t journal_verify_fail;     /* read-back verify before the drain failed */
	size_t head_fallbacks;          /* journal unavailable: HEAD's RAM path ran */
	size_t ram_after_refusal;       /* refused, but HEAD's real allocations held the floors */
	size_t ram_floor_breach;        /* refused, HEAD's allocations breached a floor: freed */
	size_t revalidation_forecast_kept; /* lazy forecast refused, floors held: RAM kept */
	uint32_t journal_sequence;
};
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* F4-RNG: the destroyed-cell sequence of the LAST explosion in the exact order
 * Explosion.doEffects visits it (air included). Returns the cell count; copies
 * min(count, cap) cells. */
size_t server_world_test_last_blast_order(int (*xyz)[3], size_t cap);
/* One-shot fixture seed for exactly the specified runtime stream. */
void server_world_test_seed_runtime_jrand(struct java_random* target, int64_t seed);
#endif
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Test seams for the flaming pass: reseed Explosion's own RNG so two blasts
 * light the same cells, and route the fire write through the per-cell light
 * flood (World.setBlockWithNotify) instead of the batched T1 path. */
void server_world_test_seed_fire_rng(uint32_t seed);
void server_world_test_set_fire_flood_path(bool enable);
/* G9 test hooks: sync=true restores the old synchronous flood at every site
 * (the equivalence baseline); lifo=true drains by flooding each seed alone in
 * Beta's LIFO pop order instead of one batched flood; fail_after=N makes the
 * (N+1)-th light reservation fail (-1 = never). */
void server_world_test_set_light_sync(bool sync);
void server_world_test_set_light_lifo(bool lifo);
void server_world_test_light_fail_after(long n);
void server_world_test_light_stats_reset(void);
void server_world_test_light_ckpt_record(uint32_t us);
/* Lift a light stop so a test can prove the retained work resumes exactly
 * (the frontier, seeds and undo were kept). Never called in production. */
void server_world_test_light_clear_fault(struct server_world* w);
/* Called at the moment a world light stops (reservation or drain). */
extern void (*server_world_test_light_fault_hook)(struct server_world* w);
/* Called after every flood of a drain (completed or stopped), before the
 * scratch is trimmed: tests sample the frontier allocation independently. */
extern void (*server_world_test_light_drain_hook)(struct server_world* w);
/* I07: every light write of a drain, in order (the byte-exact order/value
 * trace test_g9_drain_equiv digests). NULL = off. */
extern void (*server_world_test_light_write_hook)(w_coord_t x, w_coord_t y,
												  w_coord_t z, uint8_t light);
/* Fail the next n checkpoint-trial snapshot allocations. */
void server_world_test_light_trial_fail(unsigned n);
/* g9-ckpt-1009: run the checkpoint trial's snapshot/restore with the frozen
 * pre-change per-cell loops (the reference test_g9_checkpoint_reuse_equiv
 * pairs the plane-copy / word-diff path against). */
void server_world_test_light_trial_reference(bool on);
void server_world_test_light_trial_eager(bool on);
/* G9X-7: run the generation (Beta MetadataChunkBlock) queue's relax with the
 * frozen pre-T1 body, the reference g9x_drain_t1_equiv pairs the shipping
 * one against. Work counters accumulate over every server_beta_light_drain:
 * [0] relaxed (x,z) columns, [1] relaxed (x,y,z) cells, [2] chunk-residency
 * dict probes (server_world_is_chunk_loaded), [3] resolve-cache chunk
 * lookups (server_world_resolve_chunk). Reset clears them. */
void server_world_test_beta_light_reference(bool on);
void server_world_test_beta_light_work(uint64_t out[4], bool reset);
/* G9X-5b: the allocator state the preflight samples. Each call to the
 * sampler returns states[min(k, n - 1)] (k = samples since arming), so a
 * test can make the pre-drain revalidation see a later, staler state.
 * n = 0 disarms (PC production: no fixed heaps, no gate). */
void server_world_test_mem_model(const struct server_world_mem_state* states,
	unsigned n);
/* The preflight switch (production: ON since G9X-5b part 2). */
void server_world_test_mem_preflight(bool on);
/* The last checkpoint-trial preflight: its plan (or the failing
 * revalidation), the refusals so far and the journal counters. */
void server_world_test_mem_last(struct server_world_test_mem_report* out);
/* G9X-5b part 2 journal fault points: fail the `at`-th (0-based) call of
 * `kind` (-1 = never). WRITE/SHORT_WRITE/FLUSH/SEEK hit the prepare phase,
 * READ/SHORT_READ/CORRUPT the restore phase, RESERVE/OPEN the reservation. */
enum server_world_test_journal_fault {
	SERVER_WORLD_TEST_JOURNAL_RESERVE, SERVER_WORLD_TEST_JOURNAL_OPEN,
	SERVER_WORLD_TEST_JOURNAL_SEEK, SERVER_WORLD_TEST_JOURNAL_WRITE,
	SERVER_WORLD_TEST_JOURNAL_SHORT_WRITE, SERVER_WORLD_TEST_JOURNAL_FLUSH,
	SERVER_WORLD_TEST_JOURNAL_READ, SERVER_WORLD_TEST_JOURNAL_SHORT_READ,
	SERVER_WORLD_TEST_JOURNAL_CORRUPT,
	SERVER_WORLD_TEST_JOURNAL_VERIFY, /* a read of the pre-drain read-back verify */
	SERVER_WORLD_TEST_JOURNAL_FAULTS
};
void server_world_test_journal_fault(enum server_world_test_journal_fault kind, long at);
/* Tamper with the journal file between the save and the restore (once):
 * 1 = record 0's column id becomes record 1's (header checksum untouched),
 * 2 = one queue-section byte flipped, 3 = record 0 names a non-resident
 * column with a recomputed header checksum, 4 = one queue byte flipped
 * AFTER the queue section was verified, before the final queue copy. 0 = off. */
void server_world_test_journal_tamper(int kind);
/* The journal file path of w ("" when none is reserved). */
const char* server_world_test_journal_path(const struct server_world* w);
/* Every block write that reaches finish_block_write (id + whether it lit). */
extern void (*server_world_test_block_write_hook)(struct server_world* w,
												  w_coord_t x, w_coord_t y,
												  w_coord_t z, uint8_t type,
												  bool light);
#endif
void server_world_explosion_last_cost(struct server_world_explosion_cost* out);
// Beta Explosion.isFlaming = true: after the block destruction, ~1/3 of the
// destroyed now-air cells sitting on an opaque cube take fire (nether bed,
// ghast fireball). Identical block/entity behaviour otherwise.
void server_world_explode_flaming(struct server_world* w, vec3 center,
								  float power);
// Filter used by the scheduled-tick path: returns true iff `type` is a
// block whose onWorldTick handler reads `info->neighbours`. Exported so
// tests/test_scheduled_tick_parity.c can verify that every block with a
// neighbour-reading onWorldTick handler is included; if any future
// onWorldTick reads neighbours without the type appearing in this
// filter, the handler will silently bail on its `if (!info->neighbours)`
// NULL guard and the block will appear to never tick.
bool server_world_block_needs_neighbours(uint8_t type);


bool server_world_find_empty_spot_nearby(const float pos[3], struct server_world *world, float out_pos[3]);

/* Before server start; LOAD, TERRAIN, DECORATE, VERTICAL_LIGHT,
 * BLOCK_LIGHT, SKY_BLEED, SAVE, TICK_REARM elapsed us; intervals can nest. */
void server_world_work_profile_enable(bool enabled);
void server_world_work_stats(uint32_t out[14]);

bool server_world_furthest_chunk_anchors(struct server_world* w, w_coord_t dist,
										 w_coord_t pin_x, w_coord_t pin_z,
										 w_coord_t pin_r,
										 const struct residency_anchor* anchors,
										 size_t count, w_coord_t* x,
										 w_coord_t* z);

/* Quiescent seam: 0 default, 1 recompute, 2 bitmap OOM, 4 source OOM. */
void server_world_test_light_memo_mode(unsigned mode);
bool server_world_test_light_memo_cells(const uint8_t* ids, unsigned columns,
	uint8_t* consumed, bool* fallback);
struct world_modification_entry;
extern void (*server_world_test_light_memo_output)(struct server_world*,
	const struct world_modification_entry*, size_t, unsigned);

/* G9B R1: read-only, server-thread observation. Test pool only; callbacks
 * must not call engine APIs or change scheduling. False latches capture failure
 * (the harness must assert !server_world_test_history_failed()). */
#ifdef CAVEWII_SERVER_LOCAL_TEST
struct server_world_history_event {
	const struct server_world* world;
	const struct server_local* session;
	const struct server_chunk* chunk; /* borrowed only for this callback */
	const char* operation;
	const char* edge;
	const char* outcome;
	uint64_t request, parent, update, generation, hell_rng, session_rng;
	w_coord_t x, y, z;
	int before_id, before_meta, after_id, after_meta;
	unsigned slot;
	int dimension;
	/* G9X-1: semantic site of a "store" (caller __FILE__/__LINE__). */
	const char* file;
	int line;
};
extern bool (*server_world_test_history_hook)(
	const struct server_world_history_event* event);
void server_world_test_history_reset(void);
bool server_world_test_history_failed(void);
uint64_t server_world_test_history_begin(struct server_world* w,
	const char* operation, w_coord_t x, w_coord_t z);
void server_world_test_history_end(struct server_world* w,
	const char* operation, uint64_t request, const char* outcome,
	w_coord_t x, w_coord_t z);
void server_world_test_history_note(struct server_world* w,
	const char* operation, const char* edge, const char* outcome,
	w_coord_t x, w_coord_t z, const struct server_chunk* chunk);
#define G9B_BEGIN(w, op, x, z) server_world_test_history_begin(w, op, x, z)
#define G9B_END(w, op, id, result, x, z) \
	server_world_test_history_end(w, op, id, result, x, z)
#define G9B_NOTE(w, op, edge, result, x, z, chunk) \
	server_world_test_history_note(w, op, edge, result, x, z, chunk)
#else
#define G9B_BEGIN(w, op, x, z) ((uint64_t)0)
#define G9B_END(w, op, id, result, x, z) ((void)(id))
#define G9B_NOTE(w, op, edge, result, x, z, chunk) ((void)0)
#endif

#endif
