/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#ifndef REGION_ARCHIVE_H
#define REGION_ARCHIVE_H

#include <m-lib/m-i-list.h>
#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef PLATFORM_WII
// lwp_t for the owner_thread field below.
#include <gccore.h>
#endif

#include "../world.h"

struct server_chunk;
struct chunk_tile_entities;
struct chunk_entities;

enum region_archive_read_result {
	REGION_ARCHIVE_READ_OK,
	REGION_ARCHIVE_READ_ABSENT,
	REGION_ARCHIVE_READ_ERROR,
};

struct wall_sign_metadata_migration_stats {
	size_t regions_scanned;
	size_t chunks_scanned;
	size_t signs_rewritten;
};

enum wall_sign_metadata_resolution {
	WALL_SIGN_METADATA_NEEDS_LEGACY_INTENT,
	WALL_SIGN_METADATA_MIGRATED,
	WALL_SIGN_METADATA_ALREADY_BETA,
	WALL_SIGN_METADATA_REFUSED_UNKNOWN,
	WALL_SIGN_METADATA_REFUSED_MIXED,
};

/* An authoritative CaveWii full-image marker may never reinterpret a missing
 * or malformed TileEntities node as an empty list: doing so would erase legacy
 * flat owners. Unmarked old/Beta chunks retain tolerant migration semantics. */
static inline bool region_archive_tile_entities_shape_valid(
	bool complete, bool is_list, bool has_list_payload) {
	return !complete || (is_list && has_list_payload);
}

/* Upper bound on suppressed-claim ranges tracked per open. An open salvages at
 * most REGION_SALVAGE_PERSIST_MAX entries on disk, but the in-memory-only kinds
 * (runs-past-EOF, truncated tail record, overlap) are not capped by that, so
 * size this for the pathological torn file rather than the common one. Past the
 * bound we fall back to a conservative append floor -- correct, just without
 * gap reuse, i.e. exactly the old behaviour, and only for a file that is
 * already badly damaged. */
#define REGION_PRESERVED_MAX 16

/* Chunk writes buffered inside one SAVE BATCH before the header sector(s) they
 * touch are republished (region_archive_batch_begin/end). The bound is what
 * keeps occupied_sorted's allocation and the reserved-old-range sweep O(1); a
 * batch that reaches it publishes what it has and keeps going, so this is a
 * cadence cap, never a write limit. Sized well above what the measured far
 * walk puts in one SAVE_BATCH_MAX_TICKS window (~8-9 committed writes across
 * all four resident regions) while still covering a burst that takes the full
 * per-tick NEAR_EVICT_BUDGET into a single region. */
#define REGION_BATCH_MAX 32

/* One bit per local chunk slot of a region (REGION_SIZE * REGION_SIZE = 1024
 * slots, 32 bits per word). REGION_SIZE itself is defined below the struct,
 * so the bound is spelled out here and pinned by a _Static_assert in
 * region_archive.c. */
#define REGION_CANON_WORDS 32

/* --- S1: PER-JOB COMPLETION RECORDS ----------------------------------------
 * One record per PUBLICATION (a "job": the payload barrier, the journal
 * record, the header-sector flip and the retire, for one handle's pending
 * deltas). `id` is a process-wide monotonic counter that is NEVER reused, so a
 * completion can never be read as the completion of a different job -- which
 * is what the cumulative durable/failed epoch high-water marks could not
 * promise. `state` is one of `enum region_save_state`, stored as a uint32_t
 * because it is the ONE field the region I/O helper thread writes and the
 * server thread reads (thread.h's _Static_assert proves a lock-free uint32_t;
 * a 64-bit atomic is NOT lock-free on Broadway). Everything else in the ring
 * is server-thread-only: the server reserves the slot before the job is
 * queued, the helper only stores the outcome into it. */
struct region_job_record {
	uint64_t id;
	uint32_t state;
};

/* Completion records kept per handle. A handle has at most ONE publication in
 * flight (region_archive_batch_begin waits for the helper to leave the handle
 * before a new batch may join deltas, and every other public entry point waits
 * too), so "jobs outstanding" is 1; the ring is longer only so that COMPLETED
 * records survive until the server has consumed them. A record that completed
 * DURABLE at the head of the ring is folded into `job_durable_below` and its
 * slot reclaimed -- that fold answers exactly what the record answered, so
 * nothing is lost -- which means the ring only ever holds a FAILED record plus
 * whatever completed after it. A FAILED publication latches `publish_blocked`
 * and refuses the next one until the barrier reports it, and that barrier is
 * also where server_world_reconcile_pending_saves consumes it, so in practice
 * the occupancy is 1-2. Eight slots is that bound with slack for the in-batch
 * overflow publications a single batch window can add. */
#define REGION_JOB_RECORDS 8
/* Failed job ids that left the ring (see region_job_reserve): the fold may
 * pass a FAILED head only after parking its id here, so it can never be
 * shadowed by a later DURABLE fold. If THIS parking ring overflows, the
 * evicted id is retired the conservative way (job_base raised past it:
 * everything older answers FAILED, never DURABLE). */
#define REGION_JOB_FAILED_MAX 8

struct region_io_op;
struct region_io_fence;

struct region_archive {
	w_coord_t x, z;
	/* R24 ordered retryable stream, server consumes each error. */
	bool rio_ordered, rio_error_seen;
	uint32_t rio_errors_reconciled;
	uint32_t* offsets;
	uint32_t* occupied_sorted;
	size_t occupied_index;
	/* Sector ranges claimed by index entries that OPEN suppressed in memory
	 * only. Their payload may still be recoverable, so no new write may land on
	 * them -- but they are deliberately NOT members of occupied_sorted:
	 *
	 *  - the OVERLAP salvage kind overlaps a surviving entry by construction,
	 *    so inserting it would break the sorted/non-overlapping invariant the
	 *    allocator's gap search depends on;
	 *  - merging it into the entry it overlaps is not an escape either, because
	 *    occupied_sorted packs `sectors` into 8 bits -- a union can exceed
	 *    UINT8_MAX and truncate into a silent UNDER-reserve, handing live chunk
	 *    sectors to the allocator. That is strictly worse than the bug.
	 *
	 * So they live in their own bounded list that the gap search treats as
	 * blocked. This replaces a single `preserved_end` FLOOR, which disabled
	 * free-gap reuse entirely for the life of the handle: after any salvage
	 * every write appended past the floor and the sectors freed by each rewrite
	 * were never reclaimed, so the .mcr grew monotonically for the rest of the
	 * session -- and since handles are LRU-cached and re-opening re-runs the
	 * salvage, re-opening re-armed it. */
	uint32_t preserved_start[REGION_PRESERVED_MAX];
	uint32_t preserved_end_sector[REGION_PRESERVED_MAX];
	size_t preserved_count;
	/* Fallback for a file damaged past REGION_PRESERVED_MAX distinct claims:
	 * a plain append floor, i.e. the old conservative behaviour. Only a badly
	 * torn file reaches it, and correctness still wins over reuse there. */
	uint32_t preserved_floor;
	/* --- SAVE BATCH (2026-09-07, pace regression) -------------------------
	 * Chunk index deltas committed to memory + payload but whose header sector
	 * has NOT been republished yet. `pending_old[k]` is the value the ON-DISK
	 * index still holds for `pending_index[k]`, i.e. the batch's before-image
	 * contribution and -- equally important -- a payload range that is still
	 * live from the disk's point of view, so rebuild_occupied_list keeps it
	 * reserved for the whole batch. Empty outside a batch. */
	uint16_t pending_index[REGION_BATCH_MAX];
	uint32_t pending_old[REGION_BATCH_MAX];
	/* CANONICAL-RECORD FLAG (round 4, 2026-09-08) -- per pending delta, whether
	 * the record this batch is about to publish for that slot is a CANONICAL
	 * one, i.e. a plain dump of region_archive_set_chunk's freshly built
	 * `root` with an EMPTY TileEntities list, an EMPTY Entities list and
	 * CaveWiiTileEntitiesComplete == 1. Only such a record lets the NEXT save
	 * of the same chunk skip the pre-save read-back: see the CANONICAL RECORD
	 * block in region_archive.c for the byte-identity argument. Every write
	 * that does not come from that path (migrations, the preservation merge,
	 * raw tool commits) leaves it false, which is the conservative direction.
	 */
	bool pending_canon[REGION_BATCH_MAX];
	size_t pending_count;
	/* The epoch-seconds stamp this publication writes into the per-chunk
	 * TIMESTAMP table for EVERY slot in the batch, chosen once so the two
	 * passes that build a sector's after-image (journal, then flip) produce
	 * the same bytes. Bit 31 -- the canonical flag -- is added per slot from
	 * pending_canon[k]. There is no separate stamp write left: the timestamp
	 * sectors ride the batch's crc'd journal record exactly like the offset
	 * sectors. See THE CANONICAL FLAG'S DURABILITY in region_archive.c. */
	uint32_t publish_stamp;
	/* Consume-once hand-off from region_archive_set_chunk to the commit that
	 * follows it, so region_archive_commit_buffer -- which every other writer
	 * also goes through -- defaults to "not canonical". Cleared by the commit
	 * that reads it. */
	bool next_write_canonical;
	/* g9-savemiss-1009 READ-BACK WATCH (T1). Nonzero `rb_token` names the
	 * ONE index entry (`rb_index`) whose current record a checkpoint encode
	 * cache wrote (or proved byte-identical) from bytes it still holds. Every
	 * in-memory change of offsets[rb_index] (commit, rollback, repair) clears
	 * it via region_offset_store; create/destroy clear it. Server-thread
	 * set; cleared with an atomic store on any thread. */
	uint32_t rb_token;
	uint16_t rb_index;
	/* In-memory mirror of the durable canonical flag, one bit per local chunk
	 * slot. Loaded from the per-chunk timestamp table at open (bit 31 of each
	 * big-endian u32 -- 0 in every world written by Beta, by an older CaveWii
	 * build or by any other tool, which is exactly the "unknown, read it back"
	 * answer), and kept in step with every stamp this layer writes. */
	uint32_t canon[REGION_CANON_WORDS];
	/* SECOND-HANDLE GUARD. The bitmap above is a CACHE of on-disk state, and it
	 * is only a truthful one while this handle is the sole open handle on its
	 * region file. death_drop_recovery.c legitimately opens its own handle on a
	 * region the world LRU may already hold, and that handle's writes clear
	 * bits on disk that the cached handle would go on trusting. `canon_gen` is
	 * the value of the module-global conflict generation at open; the fast path
	 * is armed only while it still matches, so ANY overlapping open on the same
	 * path disarms every handle involved for the rest of its life (the
	 * conservative direction: a lost optimization, never a lost record). */
	uint32_t canon_gen;
	/* Slot this handle holds in the open-region table, or SIZE_MAX when the
	 * table was full (which is itself treated as a conflict). */
	size_t canon_open_slot;
	/* --- ROUND 5: ASYNC PUBLICATION HAND-OFF (2026-09-08) ------------------
	 * `io_state` is a region_io_state (region_archive.c): IDLE means the
	 * SERVER thread owns this handle, QUEUED/RUNNING mean the region I/O
	 * helper does. Every public entry point waits for IDLE first
	 * (region_io_drain), which is what keeps the two threads off one FILE*.
	 * `io_failed` latches a helper-side publication failure until the next
	 * barrier consumes it -- the same one-shot report region_archive_batch_end
	 * used to make inline. `path_hash` lets a second open on the same file
	 * drain exactly the in-flight publications that could collide with it.
	 * All three are plain PODs so the by-value handle move in
	 * server_world_chunk_region (`*lru = ra`) stays a memcpy; a fresh handle is
	 * always IDLE, never queued, so nothing is ever moved out from under the
	 * helper. */
	uint8_t io_state;
	bool io_failed;
	uint32_t path_hash;
	/* --- M3: PUBLICATION EPOCH (round-5 fix round; S1, 2026-09-09) --------
	 * `save_epoch` is a STRICT ORDERING COUNTER over this handle's
	 * publications and nothing else: it is consumed (advanced to E+1) whether
	 * the publication of E succeeded or FAILED, so no two jobs ever run on the
	 * same epoch. It used to also CLASSIFY a write, paired with cumulative
	 * `durable_epoch`/`failed_epoch` high-water marks -- and that could not
	 * separate two jobs, because a failure left save_epoch on E, the next
	 * batch reused E, and its success set durable_epoch = E, which
	 * region_archive_save_state (durable tested first) then reported as the
	 * DURABILITY OF THE EARLIER, FAILED JOB. Classification now comes from the
	 * per-job completion ring below; the two high-water marks are gone.
	 * `publish_blocked` is the sticky refusal that stops batch N+1 from
	 * publishing before batch N-s failure has been reported; it is only ever
	 * touched while the handle is server-owned or by the helper inside its own
	 * publication. */
	uint32_t save_epoch;
	bool publish_blocked;
	/* --- S1: the completion ring (see struct region_job_record) -----------
	 * `cur_job` is the id the deltas currently in the pending list belong to
	 * (0 when there are none); it is allocated when the FIRST delta of a job
	 * joins, which is what makes region_archive_save_job's pre-write sample
	 * name the same job the write about to be issued will join. `job_base` is
	 * the first id this INCARNATION of the handle could take: a token minted
	 * by an earlier handle on the same region is below it and can never be
	 * matched against this handle's records. `job_durable_below` is the fold
	 * of head-of-ring DURABLE records ("every id <= this one completed
	 * DURABLE"). `job_slot` is the ring slot the in-flight publication will
	 * complete into, reserved by the server before the job is queued (-1 when
	 * no publication is outstanding). */
	uint64_t cur_job;
	/* S2: the job id the MOST RECENT delta joined (set at the join, on the
	 * server thread). A pre-write sample of region_archive_save_job can name
	 * the wrong job when the write itself publishes on overflow first; read
	 * this after the write instead. */
	uint64_t last_join_job;
	uint64_t job_base;
	uint64_t job_durable_below;
	struct region_job_record job_ring[REGION_JOB_RECORDS];
	uint8_t job_head;
	uint8_t job_count;
	int8_t job_slot;
	uint64_t job_failed[REGION_JOB_FAILED_MAX];
	uint8_t job_failed_head, job_failed_count;
	/* True between region_archive_batch_begin and _end. When false, every
	 * commit publishes itself immediately (batch of one) -- the behaviour every
	 * non-batched caller (tools, migrations, death-drop recovery) still gets. */
	bool batch_open;
	/* --- G9 R19: DEFERRED REGION IO (experimental.g9_save_io_int, default
	 * OFF; see region_archive_deferred_io_enable and docs/g9_save_io.md) ---
	 * SERVER-OWNED: the per-handle submission list of not-yet-retired IO ops
	 * (oldest first), the logical file length the allocator uses instead of
	 * an fseek/ftell on the FILE* the worker may be writing, and the mirror
	 * of a worker-side dead file. `rio_incomplete` is written by both threads
	 * under the module IO lock (and read lock-free with ACQUIRE).
	 * WORKER-OWNED: `rio_disk_dead` (a failed rollback left the header in an
	 * unknown state: every later op of this handle fails) and
	 * `rio_poison_job` (a payload write of that job failed, so its
	 * publication must fail -- an index may never name a missing payload).
	 * All inert (NULL/0) while the knob is off. */
	struct region_io_op* rio_head;
	struct region_io_op* rio_tail;
	/* g9-savemiss-1009: retired successful payload writes whose ranges are
	 * not yet visible to a fresh libfat handle; server-only, newest first. */
	struct region_io_op* rio_shadow;
	uint32_t rio_incomplete;
	uint16_t rio_ops;
	bool rio_dead_seen;
	bool rio_disk_dead;
	uint64_t rio_poison_job;
	unsigned long rio_file_bytes;
	/* Conservative length visible to a freshly opened read-only libfat
	 * handle. Worker RELEASE after payload fsync; server ACQUIRE. */
	unsigned long rio_read_bytes;
	unsigned long rio_disk_bytes; /* worker: successful physical write extent */
	/* Speculation generations (see rebuild_occupied_list): server-owned
	 * `rio_spec_gen` stamps every op; the worker records in `rio_failed_gen`
	 * the generation of a publication that failed uncommitted, and fails
	 * every later op of that generation before it touches the file. */
	uint32_t rio_spec_gen;
	uint32_t rio_failed_gen;
	/* The per-handle in-place journal (v3, round 3). Opened once, LAZILY, on
	 * the handle's first publication -- a read-only handle never creates it --
	 * and then rewritten in place per publication instead of created + renamed
	 * + removed; `journal_seq` is the seq the NEXT record
	 * gets, `journal_record` the seq of the record currently in the file, and
	 * `journal_published` the seq whose sectors are known committed. Recovery
	 * replays a record only while journal_record > journal_published, which is
	 * what replaced the unlink as the retirement signal. See region_archive.c's
	 * REGION_JOURNAL_MAGIC block for the layout and the durability argument. */
	FILE* journal;
	uint32_t journal_seq;
	uint32_t journal_record;
	uint32_t journal_published;
	string_t file_name;
	// Cached FILE* — opened once in region_archive_create with mode "rb+",
	// reused across every read/write, closed in region_archive_destroy.
	// Replaces the previous fopen/fseek/fread/fclose-per-call pattern that
	// was dominating chunk load time on the Wii (libfat's open path walks
	// the FAT cluster chain). Safe to keep on the struct because all
	// region_archive_* calls funnel through server_world.c on the single
	// server_local_thread — see platform-performance milestone Task 1.
	FILE* file;
	// Thread that owns this region_archive. Captured by
	// region_archive_create from LWP_GetSelf() (Wii) or pthread_self()
	// (PC). Every region_archive_{get,set}_blocks asserts in debug
	// builds that the calling thread matches; this catches future
	// regressions where region access leaks out of server_local_thread
	// onto the main render or mesher threads, which would corrupt
	// ra->file silently (libfat is not thread-safe across the same
	// FILE*, and we don't take a mutex). Zero overhead in release.
#ifdef PLATFORM_WII
	lwp_t owner_thread;
#else
	unsigned long owner_thread; // pthread_t on PC; opaque uintptr value
#endif
	ILIST_INTERFACE(ilist_regions, struct region_archive);
};

ILIST_DEF(ilist_regions, struct region_archive, M_POD_OPLIST)

#define REGION_SIZE 32
#define REGION_SIZE_BITS 5
#define REGION_SECTOR_SIZE 4096

#define CHUNK_REGION_COORD(x) ((w_coord_t)floor(x / (float)REGION_SIZE))

// Create a missing region. Fails closed if the target path already exists;
// callers must never replace an unreadable or corrupt archive with an empty one.
bool region_archive_create_new(struct region_archive* ra, string_t world_name,
							   w_coord_t x, w_coord_t z,
							   enum world_dim dimension);
bool region_archive_create(struct region_archive* ra, string_t world_name,
						   w_coord_t x, w_coord_t z, enum world_dim dimension);
/* False when the close failed (stdio can defer a write error to fclose). Does
 * not fsync -- callers that need durability call region_archive_sync first. */
bool region_archive_destroy(struct region_archive* ra);
/* T1 measurement instrumentation (2026-08-24 Wii-only world-load-stall
 * investigation, docs/resource_lessons_review_2026-08-24.md section (b)):
 * process-lifetime counters for every region-file fopen + fread the archive
 * layer issues, plus total bytes actually read. Exposed for the F3-style
 * debug overlay (screen_ingame.c "region io" row) and any headless perf
 * dump -- mirrors the existing s_dbg_rt_us/g_srv_tick_* debug-counter
 * pattern (server_local.c); this is NOT a network/telemetry wire field.
 * fopen_n counts only region_archive_create's cached "rb+" open (the one
 * the LRU keeps alive across a load batch -- see the FILE* field comment
 * below); fread_n/bytes_read count every fread the archive layer issues
 * (region-open index table, chunk-record reads, salvage/validation probes,
 * the block-id streaming reader used by portal search/respawn). A NULL
 * argument is skipped, the same "don't care about this one" convention the
 * codebase's other multi-out-param debug getters use (e.g.
 * server_local_far_stats, server_world_edit_stats). */
void region_archive_io_stats(uint32_t* fopen_n, uint32_t* fread_n,
							 uint64_t* bytes_read);
/* Developer work-profile only (off in normal play). Region AND journal
 * fopen attempts, including failures, and their cumulative elapsed us.
 * uint32 counters are atomic on Broadway; rig deltas use modulo arithmetic.
 * Legacy io_stats above retains its successful-region-only contract. */
/* out[2]: inclusive region-create, journal-open, region-stat and level
 * load/save elapsed us. fopen time is nested inside it, never add them. */
void region_archive_path_profile_stats(uint32_t out[3]);
void region_archive_path_profile_note(uint32_t elapsed_us);
void region_archive_path_profile_emit(const char* marker, uint32_t elapsed_us);
#ifdef CAVEWII_REGION_ARCHIVE_TEST
// Test-only: zero the counters so a test can assert an exact delta rather
// than a whole-process cumulative total.
void region_archive_io_stats_reset(void);
#endif
// Durability barrier: fflush + fsync the cached region handle so buffered
// chunk writes AND the FAT metadata survive a power cut while the LRU keeps
// the file open. No-op (true) when no file is open. See the .c comment.
bool region_archive_sync(struct region_archive* ra);
/* --- SAVE BATCH seam --------------------------------------------------------
 * Amortize the journaled header publication (one file create + rename + remove
 * and THREE fsyncs) over a batch of chunk writes instead of paying it per
 * chunk. Between begin and end, region_archive_set_chunk writes each payload
 * and updates the in-memory index, but leaves the on-disk offset table alone;
 * _end then performs, ONCE per touched 512-byte header sector:
 *
 *   fsync(region file)      -- every payload of the batch is durable
 *   write + fsync(journal)  -- before/after images, crc32-checked, IN PLACE
 *   write + fsync(sector)   -- the header entries flip together
 *   write published_seq     -- retire; four bytes, no fsync, no directory op
 *
 * Round 3 (2026-09-08) removed the create + rename + remove that used to
 * bracket the journal write: the file is opened once per handle and rewritten
 * in place, and retirement is a sequence number rather than an unlink. Three
 * fsyncs and ZERO directory transactions per publication.
 *
 * INVARIANT (unchanged from the per-chunk version): no on-disk index entry can
 * ever name a payload that is not already durable, and a torn header sector is
 * restored from the journal's before-image on the next open. What the batch
 * ADDS is that the flip is atomic per sector for the whole batch; what it costs
 * is that an unpublished batch reverts to the previous on-disk chunk contents
 * after a power cut -- exactly the pre-journal (dab600e9) exposure window,
 * which the callers bound with SAVE_BATCH_MAX_TICKS (40 ticks = 2 s, Beta's autosave cadence) and this file's
 * REGION_BATCH_MAX -- still a far tighter window than dab600e9's "until the
 * next explicit region_archive_sync barrier".
 *
 * _begin is idempotent. _end is a no-op returning true when no batch is open or
 * nothing is pending. region_archive_sync and region_archive_destroy publish
 * any pending batch first, so a handle can never be synced, LRU-evicted or
 * closed with unpublished index deltas. */
void region_archive_batch_begin(struct region_archive* ra);
bool region_archive_batch_end(struct region_archive* ra);
bool region_archive_contains(struct region_archive* ra, w_coord_t x,
							 w_coord_t z, bool* chunk_exists);
/* Stream only Level.Blocks into a caller-owned 32 KiB buffer. Decode/parser
 * scratch is bounded, no server_chunk is installed, and tile/entities are
 * validated/skipped rather than restored. */
enum region_archive_read_result region_archive_get_block_ids(
	struct region_archive* ra, w_coord_t x, w_coord_t z, uint8_t* ids);
/* Same bounded streaming reader, additionally capturing Level.Data's 16 KiB
 * metadata nibble array. Respawn validation needs the saved bed direction but
 * must not materialize a complete chunk while the Nether is still resident. */
enum region_archive_read_result region_archive_get_block_ids_metadata(
	struct region_archive* ra, w_coord_t x, w_coord_t z, uint8_t* ids,
	uint8_t* metadata);
bool region_archive_get_blocks(struct region_archive* ra, w_coord_t x,
							   w_coord_t z, struct server_chunk* sc,
							   struct chunk_tile_entities* out_te);
bool region_archive_get_chunk(struct region_archive* ra, w_coord_t x,
							  w_coord_t z, struct server_chunk* sc,
							  struct chunk_tile_entities* out_te,
							  struct chunk_entities* out_entities);
/* S2 two-phase retirement (docs/p2_storage_ruling_fable_2026-09-09.md):
 * identical to region_archive_set_chunk, but on success moves the committed
 * compressed record into *out_record so an EVICTING caller keeps an exact
 * retry source after it frees the live chunk. An unchanged record (nothing
 * appended, epoch unmoved) yields an empty buffer. The caller owns the buffer
 * (buffer_free). */
struct buffer;
bool region_archive_set_chunk_retain(struct region_archive* ra, w_coord_t x,
									 w_coord_t z, struct server_chunk* sc,
									 const struct chunk_tile_entities* te,
									 const struct chunk_entities* entities,
									 struct buffer* out_record);
/* Copy the exact compressed payload of this chunk's expected pending job.
 * Does not append, publish or restamp. Failure leaves the caller owning the
 * resident chunk; in particular a rolled-back OLD index cannot be copied as
 * the retry source for the failed NEW job. Caller frees *out_record. */
bool region_archive_copy_pending_record(struct region_archive* ra, w_coord_t x,
    w_coord_t z, uint64_t job, struct buffer* out_record);
/* Re-append a record previously handed out by _set_chunk_retain after its
 * publication FAILED (the archive rolled the index back to the old record).
 * Joins the open batch like any write; the caller re-samples the epoch. */
bool region_archive_recommit_record(struct region_archive* ra, w_coord_t x,
									w_coord_t z, const struct buffer* record);
bool region_archive_set_blocks(struct region_archive* ra, w_coord_t x,
							   w_coord_t z, struct server_chunk* sc,
							   const struct chunk_tile_entities* te);
bool region_archive_set_chunk(struct region_archive* ra, w_coord_t x,
							  w_coord_t z, struct server_chunk* sc,
							  const struct chunk_tile_entities* te,
							  const struct chunk_entities* entities);

/* Optional, bounded per-world memo of two exact checkpoint NBT inputs and
 * their zlib bytes. Only its owner thread may use it. No chunk pointers or
 * persistence decisions are retained; callers still commit every record.
	 * Keys/values and codec states use strict MEM2 (no MEM1 spill). Codec
	 * states share a 900 KiB budget including the working copy; two keys,
	 * compressed values and prefix outputs add at most 233 KiB per world. */
/* Startup-only T2 region codec: integer 1..6, invalid -> faithful 6.
 * Configure before worlds open. Cache keys include the level. */
void region_archive_save_zlib_level_configure(int level);
struct region_encode_cache;
bool region_archive_set_chunk_cached(struct region_archive* ra, w_coord_t x,
	w_coord_t z, struct server_chunk* sc, const struct chunk_tile_entities* te,
	const struct chunk_entities* entities, struct region_encode_cache** cache);
void region_archive_encode_cache_free(struct region_encode_cache** cache);
/* g9-savemiss-1009: an ordinary (uncached-codec) save that keeps `memo`'s
 * read-back watch exact when it rewrites the one entry the memo watches;
 * for every other entry it is region_archive_set_chunk. Same bytes. */
bool region_archive_set_chunk_watched(struct region_archive* ra, w_coord_t x,
	w_coord_t z, struct server_chunk* sc, const struct chunk_tile_entities* te,
	const struct chunk_entities* entities, struct region_encode_cache** memo);
/* g9-savemiss-1009: [0] read-backs served from the exact memo, [1] arms. */
void region_archive_encode_cache_readback_stats(
	const struct region_encode_cache* cache, uint32_t out[2]);
void region_archive_encode_cache_stats(const struct region_encode_cache* cache,
	uint32_t out[4]); /* hits (full + prefix), misses, retained bytes, prefix hits */
/* Classify every unmarked block-68 value before touching a chunk. A proven
 * legacy decision is returned to the caller for durable journaling before any
 * conversion; only a durable intent permits mutation. Beta-only 2/3 data is
 * left intact; mixed or ambiguous data is refused without mutation. Converted
 * chunks retain an idempotence marker (never provenance), and every changed
 * region is fsynced before success. Call only while the world is not resident. */
/* FH-07 (save version 3): rewrite the fork's iron chest off Beta's id 95
 * (BlockLockedChest) to the net-new 200, per-instance provenance-proven by
 * the Chest tile entity every placed iron chest owns. rewrite_unproven_blocks
 * additionally converts TE-less 95s (sound only for CaveWii-created worlds,
 * where locked chests were never obtainable); rewrite_item_ids also rewrites
 * item-stack id 95 inside TE Items lists and dropped-item entities (equally
 * provenance-gated by the caller). Idempotent; both dimensions; regions are
 * committed copy-on-write so a crash between regions simply re-runs. */
struct iron_chest_migration_stats {
	size_t regions_scanned;
	size_t chunks_scanned;
	size_t blocks_rewritten;
	size_t items_rewritten;
};
bool region_archive_migrate_iron_chest_ids(
	string_t world_name, bool rewrite_unproven_blocks, bool rewrite_item_ids,
	struct iron_chest_migration_stats* stats);
/* FH-17 (save version 4): synthesize a faithful MobSpawner tile entity for
 * every dungeon spawner this port persisted with its mob type smuggled into
 * the block's metadata nibble instead (legacy CaveWii saves, spawner.c's now-
 * migration-only spawner_mob_from_meta). Unlike the iron-chest migration
 * above, there is no id-collision ambiguity to resolve with world
 * provenance: a BLOCK_MOB_SPAWNER that already owns a MobSpawner TE (a real
 * Beta import -- Beta's own worldgen/loader ALWAYS creates one -- or an
 * already-migrated instance) is left untouched; one with no TE is
 * unambiguously this port's legacy encoding, so it is unconditionally
 * synthesized regardless of world creation provenance. The metadata nibble
 * of a synthesized instance is zeroed to Beta's own faithful spawner-block
 * value (0); an already-TE'd instance's metadata (dead going forward either
 * way, since the runtime reads the TE, never metadata) is left as-is rather
 * than rewritten for a block this pass is otherwise told to leave alone.
 * Idempotent; both dimensions; regions are committed copy-on-write so a
 * crash between regions simply re-runs. */
struct mob_spawner_te_migration_stats {
	size_t regions_scanned;
	size_t chunks_scanned;
	size_t spawners_synthesized;
};
bool region_archive_migrate_mob_spawner_tes(
	string_t world_name, struct mob_spawner_te_migration_stats* stats);
/* PAR-BED-META (save version 5): CaveWii saves before version 5 stored bed
 * direction as index<<1 over {+Z,-Z,+X,-X}; Beta stores meta&3 over
 * headBlockToFootBlockMap {+Z,-X,-Z,+X} with 0x4 = occupied (bed_meta.h).
 * Rewrites every BLOCK_BED nibble through bed_meta_from_legacy in both
 * dimensions. The CALLER decides provenance (server_local runs this only
 * for a CaveWii-created world going from version 4 to 5; a real Beta import
 * is never converted). The mapping is not idempotent by itself, so every
 * rewritten chunk carries .Level.CaveWiiBedMetadataVersion=5 in the SAME
 * copy-on-write record as its converted nibbles, and a marked chunk is
 * skipped: a crash between regions (version still 4) re-runs safely. A
 * chunk whose beds all convert to themselves (+Z/-Z, or no beds) is not
 * rewritten. */
struct bed_metadata_migration_stats {
	size_t regions_scanned;
	size_t chunks_scanned;
	size_t chunks_already_marked;
	size_t beds_seen;
	size_t beds_rewritten;
};
bool region_archive_migrate_bed_metadata(
	string_t world_name, struct bed_metadata_migration_stats* stats);
bool region_archive_prepare_wall_sign_metadata(
	string_t world_name, bool cavewii_created, bool legacy_intent,
	enum wall_sign_metadata_resolution* resolution,
	struct wall_sign_metadata_migration_stats* stats);

/* IMP-MIGRATE-FAST (T1, same output): ONE read-only survey of every chunk in
 * both dimensions that evaluates the save-version 2/3/4 per-chunk predicates
 * exactly as the staged passes above would, so the common case -- a real Beta
 * import, where all three stages change zero bytes -- needs one read+parse per
 * chunk instead of three. Records are read in FILE-OFFSET order through a
 * bounded sequential window (no per-record random seek on the SD card).
 *
 * The survey never writes. Region files are opened with region_archive_create
 * exactly like the first staged pass, so open-time salvage happens on the same
 * open it always did. A stage may use the survey only while it is `valid`:
 * the *_surveyed entry points below return the survey's numbers ONLY when the
 * survey proves that stage is a pure no-op success; for every other outcome
 * (a write, a failure, anything the survey could not decide) they clear
 * `valid` and run the unchanged staged pass, so the written bytes, the
 * failures and the stats are the staged path's own by construction. */
struct region_migration_survey {
	bool valid;
	size_t regions_scanned; /* region files opened, both dimensions */
	size_t chunks_scanned;	/* existing chunks parsed */
	/* v2 wall signs (region_wall_sign_inspect_chunk's evidence) */
	bool ws_failed;
	bool ws_beta, ws_legacy, ws_wall_sign_seen, ws_current_marker_seen;
	/* v3 iron chest (region_iron_chest_migrate_chunk's predicates) */
	bool iron_failed;
	size_t iron_proven_blocks, iron_bare_blocks, iron_items;
	/* v4 spawner TE (region_spawner_migrate_chunk's predicates) */
	bool spawner_failed;
	size_t spawners_missing_te;
};
/* `progress` (optional) receives 0..1000 permille, monotonic, from the
 * calling thread. Always fills `out`; out->valid false means "use the staged
 * passes" (never an error by itself). */
void region_archive_migration_survey(string_t world_name,
	struct region_migration_survey* out,
	void (*progress)(uint32_t permille, void* ctx), void* ctx);
bool region_archive_prepare_wall_sign_metadata_surveyed(
	string_t world_name, bool cavewii_created, bool legacy_intent,
	struct region_migration_survey* survey,
	enum wall_sign_metadata_resolution* resolution,
	struct wall_sign_metadata_migration_stats* stats);
bool region_archive_migrate_iron_chest_ids_surveyed(
	string_t world_name, bool rewrite_unproven_blocks, bool rewrite_item_ids,
	struct region_migration_survey* survey,
	struct iron_chest_migration_stats* stats);
bool region_archive_migrate_mob_spawner_tes_surveyed(
	string_t world_name, struct region_migration_survey* survey,
	struct mob_spawner_te_migration_stats* stats);

#ifdef CAVEWII_REGION_ARCHIVE_TEST
// Deterministic one-shot failures for the crash-safe commit regression test.
// Production builds do not expose or execute these hooks.
enum region_archive_test_fail_stage {
	REGION_ARCHIVE_TEST_FAIL_NONE = 0,
	REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE,
	REGION_ARCHIVE_TEST_FAIL_PAYLOAD_FLUSH,
	REGION_ARCHIVE_TEST_FAIL_INDEX_WRITE,
	REGION_ARCHIVE_TEST_FAIL_INDEX_FLUSH,
	/* Fail the NEXT region_archive_sync: proves a durability barrier GATES the
	 * irreversible step after it (recovery's journal removal). */
	REGION_ARCHIVE_TEST_FAIL_SYNC,
	/* Report failure after the NEXT real fclose has executed. This preserves
	 * test process hygiene while proving callers do not forget a handle whose
	 * close result can no longer be retried. */
	REGION_ARCHIVE_TEST_FAIL_CLOSE,
	/* Fail the post-salvage header flush during OPEN. That path cannot trust
	 * the on-disk repairs reached the card, so it degrades every repaired
	 * claim to the append floor -- the only way to reach the floor without
	 * REGION_PRESERVED_MAX distinct claims, and therefore the only way to
	 * regression-test it. */
	REGION_ARCHIVE_TEST_FAIL_SALVAGE_FLUSH,
	/* Fail the NEXT per-chunk timestamp stamp (the sector-1 table write that
	 * follows a committed chunk write). The stamp is post-commit forensic
	 * metadata and Beta-faithfully best-effort, so the injected failure must
	 * leave the chunk write SUCCESSFUL, the payload committed, and the old
	 * stamp in place -- and must not poison the following write. */
	REGION_ARCHIVE_TEST_FAIL_TIMESTAMP_WRITE,
	/* Fail validation of a joined same-key batch replacement after its payload
	 * is written, before the pending offset/certificate can change. */
	REGION_ARCHIVE_TEST_FAIL_REBUILD_VALIDATION,
};
void region_archive_test_fail_next(enum region_archive_test_fail_stage stage);
/* Make every fsync this layer issues sleep for `ms` (0 disables). Lets a test
 * read the fsync census at a known instant and prove the publication left the
 * calling thread. */
void region_archive_test_fsync_delay(uint32_t ms);
/* Fail the nth covering fsync call once (1-based; zero disarms). */
void region_archive_test_fsync_fail_after(uint32_t n);
/* Block until this handle has no asynchronous publication in flight. Since
 * round 5, region_archive_batch_end only REQUESTS one, so a test that reads
 * the region file bytes directly has to wait for it first. */
void region_archive_test_publish_wait(struct region_archive* ra);
/* G9 R19 test seams (inert unless armed):
 *  - clock: a non-zero value replaces time(NULL) for every publication
 *    stamp, so two runs can be compared byte for byte without masking;
 *  - trace: an ordered callback per region-file mutation (payload write,
 *    flush, fsync, journal record, sector flip, retire, rollback, journal
 *    open/remove, close), invoked on whichever thread performs it, with the
 *    region file name and two op-specific values (offset/length, seq, crc);
 *  - hold: while held, the deferred worker does not START another op
 *    (deterministic stalls / queue exhaustion);
 *  - point: called after every persistence transition (a completed fsync,
 *    rename or remove) -- the power-cut snapshot hook of
 *    test_g9_save_io_durability. */
enum region_trace_op {
	RT_PAYLOAD = 1,
	RT_FLUSH,
	RT_SYNC_REGION,
	RT_JOURNAL,
	RT_SYNC_JOURNAL,
	RT_SECTORS,
	RT_RETIRE,
	RT_ROLLBACK,
	RT_JOURNAL_OPEN,
	RT_JOURNAL_REMOVE,
	RT_CLOSE,
	RT_LEVEL_WRITE,
	RT_LEVEL_SYNC,
	RT_LEVEL_ROTATE,
};
typedef void (*region_trace_hook)(int op, const char* path, uint64_t a,
								  uint64_t b);
void region_archive_test_clock(uint32_t stamp);
/* Frozen clone/dump preflight and double-serialization arm, test pool only. */
void region_archive_test_encode_reference(bool on);
void region_archive_test_trace(region_trace_hook hook);
/* Pause inside payload IO, after framing and before the payload fwrite. */
void region_archive_test_payload_hook(void (*hook)(void));
void region_archive_test_trace_note(int op, const char* path, uint64_t a,
									uint64_t b);
void region_archive_test_io_hold(bool hold);
/* Number of deferred ops not yet completed (all handles). */
uint32_t region_archive_test_io_incomplete(void);
/* Fail the NEXT deferred op of this kind on the worker before it touches the
 * file (1 write, 2 publish, 3 sync), or (4) the next deferred op-struct
 * allocation on the server, or (5) sequence retirement after a committed
 * header (retry must retire only). One-shot. */
void region_archive_test_rio_fail_next(int kind);
#endif

/* T1 instrumentation for the journaled header publication (2026-09-07, round 2
 * of the pace regression). Process-lifetime counters, single server-thread
 * writer, atomic reads -- same always-compiled shape as region_archive_io_stats
 * above. It exists because the round-1 numbers could not answer "how much of
 * REGION_COMMIT is per-BATCH cost and how much is per-SECTOR cost", nor "how
 * many publications never reach batch_end at all" (the LRU-eviction path
 * publishes through region_archive_sync). Slots:
 *   0 publications that had at least one pending delta
 *   1 chunk index deltas published (sum over those publications)
 *   2 header sectors published
 *   3 journal RECORDS written (round 3: rewritten IN PLACE, 3 fsyncs and ZERO
 *     directory transactions each -- was "one .idx create + rename + remove"
 *     per publication before round 3; see slots 13/14 for what still costs a
 *     directory transaction)
 *   4 fsyncs this layer issued (region file AND journal)
 *   5 publications reached from region_archive_batch_end
 *   6 ... from region_archive_sync (LRU eviction, checkpoint barrier)
 *   7 ... from region_archive_destroy (a handle closing with deltas pending)
 *   8 ... from a chunk write with no batch open (batch of one)
 *   9 ... forced by a full batch (REGION_BATCH_MAX)
 *  10 region_archive_set_chunk calls that ran the preservation merge
 *  11 ... of which took the short path (nothing foreign in the stored record)
 *  12 PRESERVE microseconds (Wii only; 0 on PC -- read the TPS observer there)
 *  13 journal HANDLE opens (region_journal_open: create + fsync + remove +
 *      rename, once per WRITING handle -- the directory-transaction cost
 *      round 3 amortizes down from once per PUBLICATION to this)
 *  14 journal HANDLE destroys that actually removed the .idx (region_journal_
 *      close's remove when the handle's last record retired cleanly)
 *  15 region_archive_set_chunk calls that performed the pre-save READ-BACK
 *  16 ... that skipped it because the stored record is flagged CANONICAL
 *  17 CANONICAL records written (the flag this layer set on a published slot)
 *  18 payload bytes handed to region_archive_commit_buffer
 *  19 region I/O helper busy microseconds (Wii only, and only while the
 *      microsecond profile is armed -- a census line must print them as
 *      "off" otherwise, not as a zero)
 *  20 ... the longest single publication, same gating
 *  21 publications that ran on the helper thread
 *  22 server-thread waits for the helper (region_io_wait). A cross-region
 *      chunk load must never add to this: see region_archive_contains
 *  23 publications ABANDONED by region_archive_io_quiesce (the helper was
 *      still inside IOS when the 250 ms deadline expired)
 *  19 region I/O helper busy microseconds (Wii only, and only while the
 *      microsecond profile is armed -- a census line must print them as
 *      "off" otherwise, not as a zero)
 *  20 ... the longest single publication, same gating
 *  21 publications that ran on the helper thread
 *  22 server-thread waits for the helper (region_io_wait). A cross-region
 *      chunk load must never add to this: see region_archive_contains
 *  23 publications ABANDONED by region_archive_io_quiesce (the helper was
 *      still inside IOS when the 250 ms deadline expired)
 * chunks/publication and sectors/journal are the two ratios that decide
 * whether batching is actually amortizing anything; 11/10 is how often the
 * chest-sort read-modify-compare avoided its lossless clones, and 12/10 is its
 * per-call cost (round 3). publishes/(13+14) is round 3's OWN amortization
 * claim: how many in-place, zero-directory-transaction publications each
 * create+fsync+remove+rename pair actually bought. */
void region_archive_journal_stats(uint32_t out[24]);
/* Slots 24-31 (2026-09-11): the save-path attribution the split-screen TPS
 * work needs -- what the server thread's COMMIT time is actually made of.
 *  24 payload fwrite + stdio flush microseconds (server thread, Wii-only)
 *  25 microseconds of publications that ran INLINE on the server thread
 *      (every site: batch-end fallback, sync, destroy, immediate, overflow)
 *  26 microseconds the server thread stood blocked on the helper
 *      (region_io_wait; slot 22 is the count, this is the time)
 *  27 RETIRED (always 0): timed the 4-byte on-disk index read that used to
 *      precede every write; stg41 measured it at 57 ms/s (42/256) for a
 *      value nothing consumed, and the read was deleted (Astra round 11)
 *  28 count of inline publications (portable)
 *  29 microseconds in the per-write sector count + occupied-list rebuild
 *  30-31 reserved (zero)
 * Cumulative like the first 24; a Wii run's per-second figures are
 * differences of consecutive region_journal observer lines (main.c). */
void region_archive_journal_stats_ex(uint32_t out[8]);
/* Round 5 kill switch. Default ON: region_archive_batch_end hands its batch to
 * the region I/O helper thread instead of publishing inline. Flip it OFF to
 * get the synchronous path back byte for byte (the equivalence test drives
 * both arms through this). Only call it while no batch is in flight -- test
 * setup or world teardown. */
void region_archive_io_async_enable(bool enabled);
bool region_archive_io_async_enabled(void);
/* True while the region I/O helper is INSIDE a publication, i.e. inside
 * libfat/IOS holding the FAT partition lock. A single relaxed bool load with
 * no lock behind it, because the caller is the prio-100 watchdog thread and
 * its fault path -- see wd_write_report (M1): libogc mutexes do not inherit
 * priority, so a watchdog fopen() issued while this is true can queue behind
 * the prio-48 helper and the fault report is lost. */
bool region_archive_io_busy(void);
/* G9 R19 (ASTRA-G9-R19-IO): DEFERRED REGION IO, opt-in, default OFF.
 * When enabled, every SD/FAT mutation of a region file -- the payload write
 * and its stdio flush, the publication (payload fsync, journal record +
 * fsync, header-sector flip + fsync, retire) and the sync barrier's fsync --
 * runs on the existing lower-priority region I/O helper as IMMUTABLE jobs in
 * the original submission order. The server thread allocates sectors and
 * updates the in-memory index exactly as before (so the bytes and their order
 * are unchanged), copies/moves the compressed record into the job, freezes
 * the publication's timestamp and job id at enqueue, and reconciles each
 * completion on its own thread (rollback of a failed publication, canonical
 * bitmap, reservation release). Every synchronous-success return is still a
 * BARRIER that waits for its own jobs: an unbatched write, region_archive_
 * sync, _destroy, the second-handle open and the migrations. What changes is
 * WHEN a batched (autosave/eviction) publication becomes durable: up to one
 * further batch window later, because the next window's batch_end -- not the
 * next tick's batch_begin -- waits for it. That is a documented T2 timing
 * deviation (docs/g9_save_io.md), hence opt-in and OFF by default. Call only
 * while no region handle is open (process start / test setup). */
void region_archive_deferred_io_enable(bool enabled);
bool region_archive_deferred_io_enabled(void);
/* Cumulative deferred-IO census (all 0 while the knob is off):
 *  0 ops enqueued  1 payload writes  2 publications  3 sync ops
 *  4 backpressure waits  5 barrier waits  6 barrier wait us (Wii only)
 *  7 failed ops  8 peak queued payload bytes  9 peak incomplete ops
 * 10 read-after-write hits (a read served from a queued record)
 * 11 reads that took the disk lock (a job of that handle was in flight) */
#define REGION_DEFERRED_STATS 12
void region_archive_deferred_stats(uint32_t out[REGION_DEFERRED_STATS]);
/* The job the newest not-yet-durable record of this chunk waits on: the open
 * batch's job, or a queued/unreconciled publication's (deferred mode); 0 when
 * the slot's newest record is durable as far as this handle knows. */
uint64_t region_archive_chunk_job(const struct region_archive* ra, w_coord_t x,
								  w_coord_t z);
/* Server-thread barrier: wait until every deferred op of EVERY handle has
 * completed and been reconciled. A no-op while the knob is off. Used by the
 * world-level barriers (checkpoint success, death/transfer commit, exit). */
void region_archive_io_barrier_all(void);
/* R24: reserve bounded metadata/payload capacity before the settled trial.
 * Ordinary synchronous APIs retain their covering barriers. */
#define REGION_CHECKPOINT_BYTES (128u * 1024u)
bool region_archive_checkpoint_capacity(struct region_archive* ra);
bool region_archive_checkpoint_reserve(unsigned handles);
void region_archive_checkpoint_unreserve(void);
uint32_t region_archive_checkpoint_pending_bytes(void);
bool region_archive_checkpoint_ordered(struct region_archive* ra);
struct region_io_fence* region_archive_checkpoint_fence(struct region_archive* ra);
enum region_checkpoint_state {
	REGION_CHECKPOINT_REJECTED = 0, REGION_CHECKPOINT_PENDING,
	REGION_CHECKPOINT_DURABLE, REGION_CHECKPOINT_FAILED,
};
/* Poll reconciles without IO/waits; retry wakes the exact parked operation. */
enum region_checkpoint_state region_archive_checkpoint_poll(
	struct region_io_fence* fence, bool retry);
void region_archive_checkpoint_release(struct region_io_fence* fence);

/* The length a region file WILL have once its queued deferred writes land,
 * for an open handle that still has ops queued (false otherwise: stat it).
 * Lets level.dat's SizeOnDisk see exactly what the synchronous path sees. */
bool region_archive_logical_file_size(const char* path, unsigned long* bytes);
/* A non-region job (the level.dat autosave) queued on the same worker FIFO.
 * NULL when deferred IO is off or the job cannot be queued: the caller then
 * runs `fn` itself, synchronously. `fn` runs on the worker and may touch only
 * what `ctx` owns. poll is non-blocking; wait is a barrier that returns the
 * job's result and frees the handle (exactly once, on the server thread). */
struct region_io_call;
struct region_io_call* region_archive_io_call_submit(bool (*fn)(void* ctx),
													 void* ctx);
bool region_archive_io_call_poll(const struct region_io_call* c, bool* ok);
bool region_archive_io_call_wait(struct region_io_call* c);

/* M2: bounded quiesce for an abnormal exit -- close admission, wait up to 250
 * ms (REGION_IO_QUIESCE_MS, the same budget hw_telemetry_shutdown gives its
 * sender) for the record in flight, then ABANDON a helper still inside IOS
 * (counted in journal-stats slot 23) rather than join it forever. Call it
 * wherever the telemetry sender is quiesced on fault/exit. Idempotent, and
 * permanently disarms the async path for the rest of the process. */
void region_archive_io_quiesce(void);
/* M3/S1. The JOB a write issued NOW would join, and what became of a job a
 * caller recorded earlier. Sample the job id BEFORE the write: a write with no
 * batch open publishes itself, so by the time it returns the job its own bytes
 * belonged to has already completed. The id is process-wide monotonic and
 * never reused, so a completion answers for exactly one publication -- a later
 * job can never be mistaken for the one the caller asked about. */
enum region_save_state {
	REGION_SAVE_PENDING = 0, /* written, index not yet published */
	REGION_SAVE_DURABLE,     /* the on-disk index names this record */
	REGION_SAVE_FAILED,      /* the publication failed; write it again */
};
uint64_t region_archive_save_job(const struct region_archive* ra);
/* S2: the job id the most recent delta of this handle joined (0 = none yet).
 * Changes exactly when a delta joins a job; compare before/after a write to
 * learn which job THAT write joined. */
uint64_t region_archive_last_join_job(const struct region_archive* ra);
/* The handle-s publication ordering counter. Consumed by EVERY publication,
 * successful or failed; carries no durability meaning on its own. */
uint32_t region_archive_save_epoch(const struct region_archive* ra);
/* True while this chunk is one of the current batch-s unpublished deltas. A
 * write that changed nothing (the round-4 identical-record short-circuit) adds
 * no delta, so its chunk is durable even though the epoch has not moved. */
bool region_archive_chunk_pending(const struct region_archive* ra, w_coord_t x,
								  w_coord_t z);
enum region_save_state region_archive_save_state(
	const struct region_archive* ra, uint64_t job);

void region_archive_work_profile_enable(bool enabled);
/* Whether the Wii-only microsecond profile is armed. Journal-stats slots 19
 * and 20 (helper busy / worst single publication) are only accumulated when it
 * is, so a census line must not print them as numbers when it is not. */
bool region_archive_work_profile_enabled(void);
/* READ, PARSE, GET, COMPRESS, COMMIT, SYNC. PRESERVE is slot 6 internally but
 * is NOT published here: world_us[] is 14 wide and pinned by captured evidence
 * (see region_archive.c). Read it from journal_stats slot 12 / the TPS
 * observer's TPS_REGION_PRESERVE instead. */
void region_archive_work_stats(uint32_t out[6]);

/* lava-aabb-0924/H1: save-FAILURE visibility for the heartbeat. Until now a
 * failed chunk save was only a "[region_save] result=fail ... stage=" /
 * "[chunk_save] ..." stderr line -- invisible on the .dol -- so the collector
 * could not tell a save that FAILED (publishes flat, chunk re-dirtied) from
 * one that had nothing to write. The stage strings already computed by the
 * save path map to these ordinals (wire = ordinal: append, never reorder). */
enum region_save_fail_stage {
	RSF_NONE = 0,
	RSF_CONTAINS,            /* region index unreadable */
	RSF_READBACK_PARSE,      /* stored record exists but does not parse */
	RSF_PRESERVE_SCOPE,      /* read-back scope not writable */
	RSF_PRESERVE_CLONE,      /* nbt_clone(original) OOM */
	RSF_PRESERVE_LEVEL,      /* stored record has no .Level compound */
	RSF_TILE_ENTITY_PRESERVE,
	RSF_ENTITY_PRESERVE,     /* chunk_entities_preserve fail-closed; see
							  * region_save_failure_sub for WHICH exit */
	RSF_FIELD_CLONE,
	RSF_PRESERVE_REPLACE,
	RSF_PRESERVE_LOSSLESS,
	RSF_PRESERVE_COMPARE,
	RSF_PRESERVE,            /* generic: !ready with no finer stage */
	RSF_COMPRESS,
	RSF_COMMIT,              /* region_archive_commit_buffer (disk) */
	RSF_TILE_ENTITY_GATHER,  /* server_world: te_store_gather failed */
	RSF_ENTITY_GATHER,       /* server_world: entity gather failed (count >
							  * CHUNK_ENTITY_ARCHIVE_WRITE_MAX or OOM) */
	RSF_ARCHIVE_OPEN,        /* server_world: region_archive_create_new failed */
	RSF_RETIRING_RESERVE,    /* server_world: no retiring-ring slot for an evict */
	RSF_ARCHIVE_SYNC,
	RSF_ARCHIVE_CLOSE,
	RSF_ARCHIVE_PUBLISH,
	RSF_ENTITY_RETIRE,       /* RETIRED (stage22-0927): a refused entity-destroy
							  * batch is an eviction DEFERRAL (near_evict_defer),
							  * never a save failure; ordinal kept for the wire */
	RSF_FILE_CLOSED,         /* review fix (6): archive handle already closed */
	RSF_TILE_ENTITY_BUILD,   /* tile-entity NBT build failed (OOM) */
	RSF_ENTITY_BUILD,        /* entity NBT build failed (OOM / over WRITE_MAX) */
	RSF_OTHER,               /* a stage string this table does not know */
	RSF_MAX
};
/* Record one failed chunk save: bumps the counter, latches the stage (and,
 * for entity_preserve, chunk_entities_preserve_last_fail as the sub-stage).
 * Review note (8): stage, sub and count are three relaxed words written in
 * the order sub -> stage -> count by the saving thread and read one at a time
 * by the heartbeat; a heartbeat that lands between them can pair the new
 * count with the previous stage/sub for ONE sample. Accepted rather than
 * packed: the next heartbeat is consistent again and a count that rose is
 * the actionable signal either way.
 * O(1) apart from the one-time strcmp walk over the ~18-entry table on the
 * FAILURE path only. Called by the region save itself and by server_world
 * for the two gather stages. */
void region_save_failure_note(const char* stage);
/* The stage's log name ("entity_preserve", ...); "none"/"other" otherwise. */
const char* region_save_failure_stage_name(enum region_save_fail_stage stage);
uint32_t region_save_failure_count(void);
enum region_save_fail_stage region_save_failure_stage(void);
uint32_t region_save_failure_sub(void);
/* Saves that were a whole-record no-op (skip_unchanged: publishes flat by
 * design, nothing appended). Lets the collector tell "publishes flat because
 * unchanged" from "publishes flat because failing". */
uint32_t region_save_skip_unchanged_count(void);

#endif
