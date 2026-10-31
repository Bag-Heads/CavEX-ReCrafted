/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "server_remote_internal.h"

#ifdef CAVEWII_MP_CLIENT

/* W1 demote-on-exit reads the INSTALLED voxels of a column back out of the
 * client world to re-encode them (design section 2.3). client_interface.h
 * (already included above) brings in world.h/chunk.h; `gstate` itself needs
 * this, the same include client_interface.c takes for the identical reason. */
#include "../game/game_state.h"
/* W2: the MEM2 CARVE_MP_BACKING reservation every compressed body lives in
 * (mp_backing_alloc / _free / _stats; design section 2.4). */
#include "../platform/mem2.h"

/* Chunk retention (bounded compressed-payload cache), promotion (zlib
 * inflate -> resident chunk pipeline) and pre-chunk unload machinery. Split
 * out of the former monolithic server_remote.c (2026-08-26, maintainer-
 * register item 1); see server_remote_internal.h's top comment. Functions
 * here that other server_remote_*.c files call are declared non-static in
 * server_remote_internal.h; everything else stays static. */


#define REMOTE_PROMOTION_OUTPUT_BUDGET (32u * 1024u)

/* --- F4: MEMORY-AWARE PROMOTION -----------------------------------------
 * docs/mp_admission_budget_design_2026-09-13.md 2.3/2.4; Astra gate-2 items
 * 3-4 ("treat 5/4/10 as proposed limits pending memory proof", "check
 * capacity before allocating", "reserve measured operating headroom").
 *
 * WHY A RADIUS IS NOT A MEMORY BOUND. Real run betamc_adm_0913 ran W1's
 * radius gate at R=5 on BetaMC and still hit the wall: mem2_carve_free_kb
 * went 25570 (tick 1) -> 10226 (tick 5) -> 39 (tick 11) and stayed pinned at
 * 39 for the rest of the 60 s, with mp_backlog stuck at 281-284 of 441 and
 * **ndisp = 0 for the whole run**. Nothing crashed (oom=0, MEM1 floor 5888
 * KB): the allocator refused gracefully and the radius held ~157 resident
 * columns. But 157 x 81,920 B = 12.6 MB of CARVE_VOXEL plus the meshes those
 * columns wanted is more than the ~17.4 MB the carve had left after the
 * backing reservation, so the display lists lost the race and the console
 * rendered NOTHING while holding a perfectly good world. A radius bounds the
 * COLUMN COUNT; it does not bound the carve, because the mesh cost per column
 * is not a constant the radius knows about.
 *
 * THE GATE. Before starting a promotion, require that the carve can hold the
 * column's voxels AND still leave a reserve for meshes. On a refusal the
 * column is NOT dropped or evicted -- it keeps its complete compressed body
 * (it is still BACKED, exactly as an out-of-radius column is), the deferral
 * is counted, and the pick retries next pump, when a demotion or a 0x32
 * unload may have freed the space. So the failure mode of an over-deep
 * radius becomes "the far columns arrive late", not "nothing is drawn".
 *
 * REMOTE_VOXEL_COLUMN_BYTES is what one installed column actually costs the
 * carve: 8 chunks x 10,240 B of ids+metadata+2 light planes (chunk.c's
 * chunk_alloc, tenant CARVE_VOXEL). The ~6.4 KB of MEM1 shells per column is
 * a different arena and is not what this gate is about.
 *
 * REMOTE_MESH_RESERVE_BYTES = 6 MiB on the Wii, derived:
 *   - measured: two full radius-5 rings (2 x 121 columns) are ~15 MiB of GX
 *     streams against a ~12 MiB mesh budget (world.c's split-reach comment,
 *     citing docs/p2_astra_high_2026-09-10.md) -- i.e. ~62 KB/column for
 *     full-detail near-ring mesh on dense terrain;
 *   - at the R=4 default the moving-maximum resident set is 121 columns, so
 *     the meshes want ~7 MB and the carve has ~10 MB left for them after
 *     voxels (see the budget table in server_remote.h);
 *   - 6 MiB is therefore a FLOOR, not an allocation: it is the part of that
 *     ~10 MB the gate refuses to let voxels eat, which is what keeps ndisp
 *     above zero. A reserve equal to the full ~7 MB estimate would start
 *     deferring columns inside the R=4 ring even when meshing was keeping
 *     up, trading the measured failure for a new one.
 *   - the far-ring skin is NOT in this number: it lives in its own fixed
 *     CARVE_SLOTS pool (224 x 10,240 B), already spent before an MP session
 *     starts and therefore already absent from the free reading.
 * 0 on PC: mem2_carve_free_bytes is SIZE_MAX there (no carve), so the test
 * is vacuous unless a test overrides the reading -- and then the reserve is
 * whatever that test chose to leave.
 *
 * Both constants are defined in server_remote_internal.h (so the suite can
 * build this same threshold symbolically); this comment is their rationale.
 *
 * COST AND LOCK ORDER. The reading is __lwp_heap_getinfo under the mem2
 * mutex, i.e. a walk of the carve's free list. It runs at most once per
 * pump, and only when the pick found a candidate AND no promotion is already
 * active -- so during a join burst it is ~1 call per 3-4 frames (a column
 * takes 3 inflate steps), and once the backlog drains it stops being called
 * at all. Taking the mem2 mutex while holding impl->lock is the order this
 * file already uses for every mp_backing_alloc/_free, and mem2 never reaches
 * back for impl->lock, so no new cycle is introduced. */

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Test override of the carve reading; SIZE_MAX = "use the real one". See
 * server_remote_test_set_carve_free_bytes in server_remote.h for why this is
 * process-wide rather than per-connection. */
static size_t s_test_carve_free_bytes = SIZE_MAX;
void server_remote_test_set_carve_free_bytes(size_t bytes) {
	s_test_carve_free_bytes = bytes;
}
/* REL-MP-LOADHANG: SIZE_MAX = "use REMOTE_MESH_RESERVE_BYTES". */
static size_t s_test_mesh_reserve_bytes = SIZE_MAX;
void server_remote_test_set_mesh_reserve_bytes(size_t bytes) {
	s_test_mesh_reserve_bytes = bytes;
}
#endif

/* The gate's reading of the carve, SIZE_MAX meaning "no carve to measure"
 * (mem2.h's contract). */
static size_t remote_carve_free_bytes(void) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_carve_free_bytes != SIZE_MAX)
		return s_test_carve_free_bytes;
#endif
	return mem2_carve_free_bytes();
}

static size_t remote_mesh_reserve_bytes(void) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_mesh_reserve_bytes != SIZE_MAX)
		return s_test_mesh_reserve_bytes;
#endif
	return (size_t)REMOTE_MESH_RESERVE_BYTES;
}

/* The gate's reading of the carve. SIZE_MAX means "no carve to measure"
 * (mem2.h's contract), which must behave as UNKNOWN -> admit, i.e. exactly
 * as this code did before F4, and let the install's own allocation failure
 * answer. Refusing on an unknown reading would refuse every column. */
static bool remote_carve_can_hold_column(void) {
	size_t free_bytes = remote_carve_free_bytes();
	if(free_bytes == SIZE_MAX)
		return true;
	return free_bytes
		>= (size_t)REMOTE_VOXEL_COLUMN_BYTES + remote_mesh_reserve_bytes();
}

/* REL-MP-LOADHANG (hw1021f, tmp/jobs/hw1021f/LOADHANG-RCA.md): what a
 * re-send of an ALREADY-INSTALLED column costs the carve.
 *  - a partial head is applied onto the installed voxels: no new voxel
 *    memory at all (the rule the IN-009 fold-wait bypass already states);
 *  - a FULL head installs through a replacement that allocates the new
 *    column before releasing the old one (world.c), so it needs exactly one
 *    column of carve for that overlap -- and NOT the mesh reserve on top:
 *    the reserve keeps room for the meshes of columns being ADDED, and a
 *    replacement adds none (resident_cols does not change; its meshes
 *    replace the old column's).
 * Before this, both went through the cold-column gate (column + 6 MiB on
 * the Wii). Once the 9x9 disk filled, the carve never held that again and
 * nothing could demote, so a re-sent core column stayed RETAINED for the
 * whole session: an endless loading screen, and every server block update
 * to an installed column parked forever. */
static bool remote_body_is_full_column(const struct mp_map_chunk* wire);
static bool remote_carve_can_replace_installed(const struct remote_chunk* chunk) {
	if(!remote_body_is_full_column(&chunk->wire))
		return true;
	size_t free_bytes = remote_carve_free_bytes();
	return free_bytes == SIZE_MAX
		|| free_bytes >= (size_t)REMOTE_VOXEL_COLUMN_BYTES;
}

/* inflateInit failure is expected to be transient (allocation pressure), so a
 * single miss retries next pump. A run of these without an intervening success
 * means the condition isn't transient; match every other zlib failure path's
 * fail-fast posture instead of retrying forever. */
#define REMOTE_INFLATE_INIT_FAILURE_LIMIT 3

/* --- W1: compressed-body ownership seam --------------------------------
 * THE single allocation/free pair for every compressed Packet51 body this
 * file owns -- the decoder's (handed over by remote_retain_map_locked) and
 * the demotion re-encode's. W2 replaces the bodies' MEM1 malloc with the
 * MEM2 CARVE_MP_BACKING reservation (design section 2.4); keeping one helper
 * here makes that a single edit instead of the eight free() sites this file
 * used to carry. W2 MUST swap BOTH halves together, AND mp_codec.c:825 (the
 * decoder's own malloc, whose result is freed through remote_body_free
 * below) in the same change: a pooled alloc paired with a plain free -- or
 * the reverse -- is a heap corruption, not a leak.
 *
 * W2 DID that: both halves now go to the MEM2 CARVE_MP_BACKING reservation
 * (platform/mem2.h), and mp_codec.c's Packet51 body site went with them in
 * the same commit -- including its three paired free sites, which the range
 * test mp_backing_owns keeps honest against the multi-block / explosion
 * bodies that share the decoder's chunk_body field and stay on malloc.
 *
 * A NULL from remote_body_alloc is a CAPACITY answer (the reservation is
 * full), not transient pressure: every caller must either reclaim and retry
 * or hand it to remote_capacity_failure_locked below. It must never become a
 * silent dropped column -- protocol 14 has no chunk re-request. */
static void* remote_body_alloc(size_t length) {
	return mp_backing_alloc(length);
}
static void remote_body_free(void* body) {
	/* Dispatch BY RANGE, the same way mp_codec.c's mp_body_free does and for
	 * the same reason: this helper frees whatever the decoder handed over,
	 * and the decoder's allocation seam has a documented override
	 * (mp_codec_set_alloc_hook) that some harnesses drive. Owning the range
	 * test here means a hook-allocated or harness-supplied body is never
	 * passed to the reservation's free, and a reservation body is never
	 * passed to the C library's -- which is heap corruption in either
	 * direction, not a leak. mp_backing_free itself stays strict (it asserts
	 * the pointer is one of its own), so the check is here, once. */
	if(!body)
		return;
	if(mp_backing_owns(body)) {
		mp_backing_free(body);
		return;
	}
	free(body);
}

/* --- W1: demotion re-encode geometry ------------------------------------
 * One FULL column in Packet51 wire layout: 16*128*16 block ids, then three
 * 4-bit planes (metadata, block/torch light, sky light) over the same cells.
 * Same sizes remote_finish_promotion_locked consumes. */
#define REMOTE_COLUMN_CELLS (16u * 128u * 16u)
#define REMOTE_COLUMN_NIBBLES (REMOTE_COLUMN_CELLS / 2u)
#define REMOTE_COLUMN_RAW_BYTES \
	(REMOTE_COLUMN_CELLS + 3u * REMOTE_COLUMN_NIBBLES)
_Static_assert(REMOTE_COLUMN_RAW_BYTES == 81920u,
			   "a full remote column re-encodes to MP_CHUNK_INFLATED_CAP bytes");
/* The deflate DESTINATION cannot be the body itself: the body's exact length
 * is only known once deflate has finished, and W2's reservation wants bodies
 * allocated at their true size (not at compressBound). So the fixed scratch
 * carries both the raw planes and a staging buffer large enough for zlib's
 * worst case, and the body is allocated exact and memcpy'd once. This is the
 * one place this slice's scratch is bigger than the brief's 81,920 B figure
 * (which covers the raw planes only) -- stated on purpose, and still ONE
 * allocation for the whole connection, never one per demotion.
 * compressBound(81920) is 81958 on zlib's formula; +1024 is headroom, and
 * remote_demote_scratch_locked asserts the real bound fits. */
#define REMOTE_DEMOTE_STAGE_BYTES (REMOTE_COLUMN_RAW_BYTES + 1024u)
#define REMOTE_DEMOTE_SCRATCH_BYTES \
	(REMOTE_COLUMN_RAW_BYTES + REMOTE_DEMOTE_STAGE_BYTES)
/* zlib level 6 == Z_DEFAULT_COMPRESSION's ratio, which is what a Beta server
 * uses for Packet51 [740c583 Packet51MapChunk.writePacketData -> Deflater
 * default]. A demotion body is never sent anywhere, so the level only trades
 * this client's CPU against its own backing bytes; matching the wire's ratio
 * keeps the backing budget's measurements comparable to the arrival bytes. */
#define REMOTE_DEMOTE_DEFLATE_LEVEL 6

static void remote_chunk_drop_body_locked(struct server_remote_impl* remote,
										  struct remote_chunk* chunk) {
	if(chunk->wire.compressed) {
		if(remote->retained_bytes >= chunk->wire.compressed_length)
			remote->retained_bytes -= chunk->wire.compressed_length;
		else
			remote->retained_bytes = 0;
		remote_body_free(chunk->wire.compressed);
	}
	memset(&chunk->wire, 0, sizeof(chunk->wire));
	chunk->synthetic_base = false; /* provenance goes with the head */
}

/* Fix A support (overlap-supersede park -- see struct remote_chunk's field
 * comment in server_remote_internal.h): discard a parked pending payload
 * without adopting it. Mirrors remote_chunk_drop_body_locked's shape
 * exactly, over pending_wire instead of wire. */
static void remote_chunk_drop_queued_locked(struct server_remote_impl* remote,
											struct remote_chunk* chunk) {
	for(uint8_t k = 0; k < chunk->queued_count; k++) {
		struct mp_map_chunk* q = &chunk->queued[k];
		if(q->compressed) {
			if(remote->retained_bytes >= q->compressed_length)
				remote->retained_bytes -= q->compressed_length;
			else
				remote->retained_bytes = 0;
			remote_body_free(q->compressed);
		}
		memset(q, 0, sizeof(*q));
		chunk->queued_arrival[k] = 0;
	}
	chunk->queued_count = 0;
}

/* Absolute, half-open containment on all three axes: does body `a` cover
 * every cell of body `b`? Widened arithmetic so x + sx cannot wrap. */
static bool remote_volume_covers(const struct mp_map_chunk* a,
								 const struct mp_map_chunk* b) {
	int64_t ax0 = a->x, ax1 = (int64_t)a->x + a->sx;
	int64_t az0 = a->z, az1 = (int64_t)a->z + a->sz;
	int64_t ay0 = a->y, ay1 = (int64_t)a->y + a->sy;
	int64_t bx0 = b->x, bx1 = (int64_t)b->x + b->sx;
	int64_t bz0 = b->z, bz1 = (int64_t)b->z + b->sz;
	int64_t by0 = b->y, by1 = (int64_t)b->y + b->sy;
	return ax0 <= bx0 && bx1 <= ax1 && az0 <= bz0 && bz1 <= az1
		&& ay0 <= by0 && by1 <= ay1;
}
static bool remote_volume_contains(const struct mp_map_chunk* v, int32_t x,
								   uint8_t y, int32_t z) {
	return (int64_t)x >= v->x && (int64_t)x < (int64_t)v->x + v->sx
		&& (int64_t)z >= v->z && (int64_t)z < (int64_t)v->z + v->sz
		&& (int64_t)y >= v->y && (int64_t)y < (int64_t)v->y + v->sy;
}

/* Defined with the deferred-edit log below; retain needs it first. */
static void remote_deferred_edit_supersede_locked(
	struct server_remote_impl* remote, struct remote_chunk* chunk,
	const struct mp_map_chunk* wire, uint64_t arrival);

/* Take ownership of `wire` as this column's HEAD body. */
static void remote_chunk_install_head_locked(struct server_remote_impl* remote,
											 struct remote_chunk* chunk,
											 struct mp_map_chunk* wire,
											 uint64_t arrival) {
	remote_chunk_drop_body_locked(remote, chunk);
	chunk->wire = *wire;
	wire->compressed = NULL;
	wire->compressed_length = 0;
	chunk->arrival = arrival;
	remote->retained_bytes += chunk->wire.compressed_length;
	if(remote->retained_bytes > remote->retained_bytes_peak)
		remote->retained_bytes_peak = remote->retained_bytes;
}

/* Append a later body to a column that already holds one (RETAINED head, or
 * PROMOTING head mid-decode). Older QUEUED bodies the new one covers are
 * coalesced away (later supersedes earlier); the in-flight head is never
 * touched. A RETAINED head with an empty queue that the new body covers is
 * replaced outright -- exactly the pre-chain "resend supersedes" path.
 * Returns false when the chain is full (caller: IN-009 fold, or wait). */
static bool remote_chunk_append_body_locked(struct server_remote_impl* remote,
											struct remote_chunk* chunk,
											struct mp_map_chunk* wire,
											uint64_t arrival) {
	uint8_t kept = 0;
	for(uint8_t k = 0; k < chunk->queued_count; k++) {
		struct mp_map_chunk* q = &chunk->queued[k];
		if(remote_volume_covers(wire, q)) {
			if(q->compressed) {
				if(remote->retained_bytes >= q->compressed_length)
					remote->retained_bytes -= q->compressed_length;
				else
					remote->retained_bytes = 0;
				remote_body_free(q->compressed);
			}
			memset(q, 0, sizeof(*q));
			continue;
		}
		if(kept != k) {
			chunk->queued[kept] = *q;
			chunk->queued_arrival[kept] = chunk->queued_arrival[k];
			memset(q, 0, sizeof(*q));
		}
		kept++;
	}
	chunk->queued_count = kept;
	if(chunk->state == REMOTE_CHUNK_RETAINED && chunk->queued_count == 0
	   && remote_volume_covers(wire, &chunk->wire)) {
		remote_chunk_install_head_locked(remote, chunk, wire, arrival);
		return true;
	}
	if(chunk->queued_count >= REMOTE_CHUNK_QUEUE_MAX)
		return false;
	chunk->queued[chunk->queued_count] = *wire;
	chunk->queued_arrival[chunk->queued_count] = arrival;
	chunk->queued_count++;
	wire->compressed = NULL;
	wire->compressed_length = 0;
	remote->retained_bytes += chunk->queued[chunk->queued_count - 1].compressed_length;
	if(remote->retained_bytes > remote->retained_bytes_peak)
		remote->retained_bytes_peak = remote->retained_bytes;
	return true;
}

/* The head body is consumed (installed, or abandoned on cancel): free it and
 * advance the chain. Returns true when a new head exists. The new head keeps
 * ITS OWN arrival stamp -- never re-stamped. */
static bool remote_chunk_shift_queued_locked(struct server_remote_impl* remote,
											 struct remote_chunk* chunk) {
	remote_chunk_drop_body_locked(remote, chunk);
	if(!chunk->queued_count)
		return false;
	chunk->wire = chunk->queued[0];
	chunk->arrival = chunk->queued_arrival[0];
	for(uint8_t k = 1; k < chunk->queued_count; k++) {
		chunk->queued[k - 1] = chunk->queued[k];
		chunk->queued_arrival[k - 1] = chunk->queued_arrival[k];
	}
	chunk->queued_count--;
	memset(&chunk->queued[chunk->queued_count], 0, sizeof(chunk->queued[0]));
	chunk->queued_arrival[chunk->queued_count] = 0;
	return true;
}

static void remote_promotion_cancel_locked(struct server_remote_impl* remote,
											 bool retain_payload) {
	struct remote_promotion* promotion = &remote->promotion;
	if(!promotion->active)
		return;
	if(promotion->stream_initialized)
		inflateEnd(&promotion->stream);
	/* F5a (L10): the inflate scratch is a reservation SCRATCH slot now. */
	mp_backing_scratch_release(MP_BACKING_SCRATCH_INFLATE);
	promotion->inflated = NULL;
	if(promotion->slot)
		clin_chunk_slot_release(promotion->slot);
	if(promotion->entry < REMOTE_CHUNK_ENTRY_CAPACITY) {
		struct remote_chunk* chunk = &remote->chunks[promotion->entry];
		/* Retry keeps EVERY non-superseded body (head + chain) with its own
		 * stamp; teardown frees the whole chain. */
		if(chunk->state == REMOTE_CHUNK_PROMOTING)
			chunk->state = retain_payload ? REMOTE_CHUNK_RETAINED
				: REMOTE_CHUNK_ANNOUNCED;
		if(!retain_payload) {
			remote_chunk_drop_body_locked(remote, chunk);
			remote_chunk_drop_queued_locked(remote, chunk);
		}
	}
	memset(promotion, 0, sizeof(*promotion));
}

void remote_chunks_clear_locked(struct server_remote_impl* remote) {
	remote_promotion_cancel_locked(remote, false);
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		remote_chunk_drop_queued_locked(remote, &remote->chunks[i]);
		remote_chunk_drop_body_locked(remote, &remote->chunks[i]);
		memset(&remote->chunks[i], 0, sizeof(remote->chunks[i]));
	}
	remote->retained_entries = 0;
	remote->retained_bytes = 0;
	/* Every `installed` bit went away with the entries above, so the hard
	 * cap's counter goes with them (F5b, H1). The PEAK is a session
	 * high-water and deliberately survives: it is the number a memory claim
	 * has to be read against, and a dimension change must not erase it. */
	remote->resident_cols = 0;
	/* Every chunks[] entry above just went away (either torn down at
	 * connection teardown, or -- the dimension-changed respawn caller --
	 * superseded by an unrelated world). Any edit still parked in the
	 * deferred-edit log necessarily targets one of THOSE now-gone
	 * coordinates, so it is dead weight, not a leak to preserve: wipe the
	 * log alongside the live pending gauge it backs. The cumulative
	 * deferred_edits_applied/_dropped counters are session-lifetime stats
	 * (retention_evictions/allocation_failures' own precedent) and are
	 * reset only by server_remote_start's per-connection reset, not here. */
	/* REL-MP-JOINSPEED: the pool goes back with its slot (every chunks[]
	 * chain above is already gone); the next park re-claims and re-zeroes
	 * it. Only ever touched under impl->lock -- an unlocked compaction
	 * applies its edits before it unlocks and revalidates before it reads
	 * the pool again -- so no recode_busy parking is needed for it. */
	if(remote->deferred_edit_log) {
		mp_backing_scratch_release(MP_BACKING_SCRATCH_EDITS);
		remote->deferred_edit_log = NULL;
	}
	remote->deferred_edit_free = 0;
	remote->deferred_edit_cursor = 0;
	remote->deferred_edits = 0;
	remote->deferred_pressure = false;
	/* W1: no column can be demoted until one is installed again, so the
	 * re-encode scratch has no owner across this clear. Both callers
	 * (server_remote_cancel and the dimension-changed respawn) reach a state
	 * where the next demotion is many frames away, and
	 * remote_demote_scratch_locked re-allocates on demand -- so this frees
	 * ~160 KB per teardown/dimension change and never per demotion. */
	if(remote->demote_scratch) {
		/* F5b (review finding H4): an UNLOCKED re-encode may own this buffer
		 * right now -- the demotion releases impl->lock across its deflate,
		 * and this clear runs on the read thread. Releasing the slot under
		 * it would be a use-after-free of 160 KB of reservation scratch, so
		 * the release is parked and the re-encode performs it when it
		 * re-acquires the lock and discovers its column is gone. */
		if(remote->recode_busy) {
			remote->scratch_release_pending = true;
		} else {
			/* F5a (L10): a scratch SLOT release, not a free -- the side
			 * region itself goes back to the carve with the reservation at
			 * session end (server_remote_cancel's
			 * mp_backing_release_if_idle). */
			mp_backing_scratch_release(MP_BACKING_SCRATCH_RECODE);
			remote->demote_scratch = NULL;
		}
	}
}

static size_t remote_chunk_find_locked(const struct server_remote_impl* remote,
										 int32_t chunk_x, int32_t chunk_z) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++)
		if(remote->chunks[i].state != REMOTE_CHUNK_UNUSED
		   && remote->chunks[i].chunk_x == chunk_x
		   && remote->chunks[i].chunk_z == chunk_z)
			return i;
	return REMOTE_CHUNK_ENTRY_CAPACITY;
}

static size_t remote_chunk_create_locked(struct server_remote_impl* remote,
										   int32_t chunk_x, int32_t chunk_z) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		if(remote->chunks[i].state != REMOTE_CHUNK_UNUSED)
			continue;
		remote->chunks[i].state = REMOTE_CHUNK_ANNOUNCED;
		remote->chunks[i].chunk_x = chunk_x;
		remote->chunks[i].chunk_z = chunk_z;
		remote->chunks[i].arrival = ++remote->arrival_sequence;
		remote->chunks[i].epoch = remote->world_epoch;
		remote->retained_entries++;
		if(remote->retained_entries > remote->retained_entry_peak)
			remote->retained_entry_peak = remote->retained_entries;
		return i;
	}
	return REMOTE_CHUNK_ENTRY_CAPACITY;
}

/* --- F5b: THE ONE ADMISSION CENTRE (review findings H5, L12) -------------
 * Contract and rationale in server_remote_internal.h beside the fields. The
 * whole point is that there is exactly ONE answer to "which column is the
 * player in" for admission, demotion hysteresis and the readiness census. */
bool remote_admission_centre_locked(const struct server_remote_impl* remote,
									int32_t* out_cx, int32_t* out_cz) {
	if(!remote->have_admit_centre)
		return false;
	if(out_cx)
		*out_cx = remote->admit_centre_x;
	if(out_cz)
		*out_cz = remote->admit_centre_z;
	return true;
}

void remote_admission_centre_set(struct server_remote_impl* remote, double x,
								 double z, bool authoritative) {
	/* A non-finite or absurd coordinate is not a centre. The correction path
	 * already rejects those before installing authoritative_x/z; the spawn
	 * hint (Packet6) is int32 block coordinates off the wire and cannot be
	 * non-finite, but it CAN be absurd, and floor() of an out-of-range
	 * double into int32 is UB rather than a large number. */
	if(!isfinite(x) || !isfinite(z))
		return;
	double cx = floor(x / 16.0), cz = floor(z / 16.0);
	if(cx < (double)INT32_MIN || cx > (double)INT32_MAX
	   || cz < (double)INT32_MIN || cz > (double)INT32_MAX)
		return;
	/* The weaker SPAWN hint never overwrites a live position: once the
	 * server has said where the player IS, a world-spawn coordinate is
	 * stale information, and H5's mirror case (a stale centre refusing
	 * everything) is exactly what that would cause. */
	if(!authoritative && remote->admit_centre_authoritative)
		return;
	remote->admit_centre_x = (int32_t)cx;
	remote->admit_centre_z = (int32_t)cz;
	remote->have_admit_centre = true;
	if(authoritative)
		remote->admit_centre_authoritative = true;
}

void remote_admission_centre_clear(struct server_remote_impl* remote) {
	remote->admit_centre_x = 0;
	remote->admit_centre_z = 0;
	remote->have_admit_centre = false;
	remote->admit_centre_authoritative = false;
}

static uint64_t remote_chunk_distance_locked(const struct server_remote_impl* remote,
											 const struct remote_chunk* chunk) {
	int32_t centre_x = 0, centre_z = 0;
	/* No centre at all (a server that has sent neither a spawn nor a
	 * position before the burst) measures from column (0,0), exactly as
	 * this function always did. That reading is not trustworthy, which is
	 * why the callers gate the RADIUS test on have_admit_centre -- and why
	 * the hard resident cap in remote_start_promotion_locked does NOT. */
	(void)remote_admission_centre_locked(remote, &centre_x, &centre_z);
	int64_t px = centre_x, pz = centre_z;
	uint64_t dx = chunk->chunk_x >= px ? (uint64_t)(chunk->chunk_x - px)
		: (uint64_t)(px - chunk->chunk_x);
	uint64_t dz = chunk->chunk_z >= pz ? (uint64_t)(chunk->chunk_z - pz)
		: (uint64_t)(pz - chunk->chunk_z);
	return dx > dz ? dx : dz;
}

/* THE ONLY place `installed` flips, so remote->resident_cols is a maintained
 * COUNT rather than a 576-entry scan the promotion gate would have to pay for
 * on every pump (F5b, review finding H1). Every caller that used to assign
 * the bit directly goes through here; the assert catches a double-flip, which
 * would corrupt the count and with it the hard cap. */
static void remote_set_installed_locked(struct server_remote_impl* remote,
										struct remote_chunk* chunk,
										bool installed) {
	if(chunk->installed == installed)
		return;
	chunk->installed = installed;
	if(installed) {
		remote->resident_cols++;
		if(remote->resident_cols > remote->resident_cols_peak)
			remote->resident_cols_peak = remote->resident_cols;
	} else {
		assert(remote->resident_cols > 0);
		if(remote->resident_cols > 0)
			remote->resident_cols--;
		chunk->voxels_synthetic = false; /* REL-MP-FIX2b: goes with them */
	}
}

/* ======================================================================
 * W2: THE LOSSY EVICTION PATH IS GONE (design section 2.2, Astra item 5)
 * ======================================================================
 * `remote_evict_one_locked` used to free the FARTHEST retained column's only
 * authoritative copy when the compressed cap was exceeded, leave it
 * TOMBSTONE, and force a reconnect the moment the server sent that
 * coordinate again -- because protocol 14 has no client chunk re-request, so
 * a second body could not prove the edits between eviction and now survived.
 * Astra's gate-2 ruling rejected destroying the only copy outright: it
 * "creates permanent holes when the player returns."
 *
 * What replaces it: the backing reservation (MEM2_MP_BACKING_BYTES) is
 * capacity-checked BEFORE each body is allocated, and a reservation that
 * genuinely cannot hold one -- after the reclaim pass in
 * remote_capacity_failure_locked -- ends the session visibly and cleanly
 * with mp_end = SERVER_REMOTE_END_CAPACITY. Nothing in this file evicts,
 * tombstones or silently drops a column any more; retention_evictions stays
 * on the impl as a session counter that must now always read 0 (it is on the
 * status/heartbeat path, so a future regression would be visible rather than
 * removed from view).
 *
 * A capacity failure on the 441-column acceptance workload is itself an
 * acceptance FAILURE (Astra item 5), which is what tests (g)/(h) in
 * tests/test_mp_admission.c exist to keep true.
 *
 * Free ONE column's worth of backing without losing any state: a RESIDENT
 * column whose queued chain already holds a body that covers its whole head
 * does not need that head -- the covering body supersedes it by
 * construction, exactly the relation remote_chunk_append_body_locked's
 * coalescing uses. Returns the bytes reclaimed (0 = nothing was free of
 * cost). This is the ONLY reclaim that touches a body, and it is lossless by
 * the same argument the coalescer already relies on. */
static size_t remote_reclaim_superseded_locked(struct server_remote_impl* remote) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		struct remote_chunk* chunk = &remote->chunks[i];
		if(!chunk->wire.compressed || !chunk->queued_count)
			continue;
		if(remote->promotion.active && remote->promotion.entry == i)
			continue; /* the head is mid-decode: it is being consumed, not idle */
		for(uint8_t k = 0; k < chunk->queued_count; k++) {
			if(chunk->queued_arrival[k] <= chunk->arrival
			   || !remote_volume_covers(&chunk->queued[k], &chunk->wire))
				continue;
			size_t freed = chunk->wire.compressed_length;
			/* Drop the superseded head and promote the chain: the new head
			 * keeps its own stamp (remote_chunk_shift_queued_locked), so the
			 * publication gate and drain rules are untouched (REL-MP-FIX2a: a
			 * synthetic head never gets here, append coalesces it first). */
			(void)remote_chunk_shift_queued_locked(remote, chunk);
			return freed;
		}
	}
	return 0;
}

/* `force`: ignore the REMOTE_DEFERRED_EDIT_HIGH_WATER trigger and compact
 * whatever is compactable. F5a (review finding M8) -- the exhaustion path has
 * to be able to compact at any log level, because by the time it runs the
 * high-water test is not the question any more. */
static bool remote_compact_one_locked(struct server_remote_impl* remote,
									  bool force, bool pump);

/* THE CAPACITY FAILURE (design section 2.4, Astra gate-2 items 4 and 5).
 *
 * RECLAIM ORDER, cheapest and most lossless first:
 *   1. one deferred-log compaction -- folds a backed column's parked edits
 *      into its own compressed head. It usually SHRINKS the backing (a
 *      re-deflate of nearly the same bytes) and always frees log slots, and
 *      it cannot lose anything: the edits end up inside the body.
 *   2. one superseded-head reclaim -- drops a head a queued body already
 *      covers in full. Lossless by the coalescer's own argument.
 * Neither reclaim is allowed to destroy authoritative state, which is why
 * "evict the farthest column" is NOT on this list.
 *
 * If the caller still cannot allocate, the session ends: reason text through
 * the SAME terminal path a kick takes (set_reason + publish_error + stop_now
 * + socket close, all inside kill_connection_locked), plus the first-wins
 * mp_end latch at SERVER_REMOTE_END_CAPACITY so an unattended run reports
 * the cause without a screenshot. The read loop then exits cleanly -- never
 * an OOM crash, never a dropped column.
 *
 * Returns true when a reclaim happened and the caller should RETRY the
 * allocation; false once the session has been terminated. */
bool remote_backing_reclaim_or_fail_locked(struct server_remote_impl* remote,
										   size_t wanted) {
	if(remote->stop_now)
		/* Already terminal: mp_end was latched by whoever killed it, so the
		 * caller's fail guard is satisfied without a second kill. */
		return false;
	if(remote_compact_one_locked(remote, false, false))
		return true;
	if(remote->stop_now)
		/* The compactor's chain collapse hit REMOTE_FOLD_FAILED and already
		 * ended the session with its own reason and mp_end latch. Do not run
		 * the capacity terminal: it would overwrite that reason. */
		return false;
	if(remote_reclaim_superseded_locked(remote))
		return true;
	remote_backing_capacity_fail_locked(remote, wanted);
	return false;
}

/* F5a (review findings H2/H3): the terminal, callable on its own -- see the
 * declaration in server_remote_internal.h for why the ladder's tail had to
 * become a function two other paths can reach. */
void remote_backing_capacity_fail_locked(struct server_remote_impl* remote,
										 size_t wanted) {
	/* IDEMPOTENT. The read loop's terminal (remote_read_decode_error) calls
	 * this unconditionally for a refused Packet51 body, and on the ordinary
	 * path the decoder's reclaim hook has ALREADY ended the session through
	 * the ladder -- so without this early-out one capacity death would count
	 * two backing_refusals and re-publish its own reason.
	 *
	 * The early-out covers ANY session already ended, not only a capacity
	 * death: it keeps the first reason and the first mp_end. Under decoder
	 * allocation pressure the hook's compaction can collapse a chain whose
	 * retained body fails to decode, latching protocol end 6 with the decode
	 * reason; the decoder then reports allocation failure and the read loop's
	 * unconditional call lands here. Without the wider guard it would
	 * overwrite that reason with "world does not fit" while mp_end stayed 6. */
	if(remote->stop_now && remote->mp_end != 0)
		return;
	size_t in_use = 0, peak = 0, capacity = 0;
	mp_backing_stats(&in_use, &peak, &capacity);
	remote->backing_refusals++;
	if(remote->mp_end == 0) {
		remote->mp_end = SERVER_REMOTE_END_CAPACITY;
		remote->mp_end_phase = (uint32_t)remote->state;
		remote->mp_end_rx = atomic_load_explicit(&remote->rx_bytes,
												 memory_order_relaxed);
	}
	/* The columns figure is what the server ASKED this console to hold, which
	 * is the number a player can act on ("that server is too big for a Wii"),
	 * and the KB figure is the reservation it did not fit in. `wanted` is
	 * folded in only when it is the pathological part (a single body larger
	 * than any free run), because otherwise it adds noise. */
	char reason[160];
	if(wanted > capacity)
		snprintf(reason, sizeof(reason),
				 "this server's world does not fit this console (%u columns, "
				 "%u KB backing; one column wanted %u KB)",
				 (unsigned)remote->retained_entries,
				 (unsigned)(capacity / 1024u), (unsigned)(wanted / 1024u));
	else
		snprintf(reason, sizeof(reason),
				 "this server's world does not fit this console (%u columns, "
				 "%u KB backing)",
				 (unsigned)remote->retained_entries,
				 (unsigned)(capacity / 1024u));
	kill_connection_locked(remote, reason);
}

void remote_backing_fail_guard_locked(struct server_remote_impl* remote) {
	if(remote->mp_end != 0)
		return; /* the contract: SOMETHING latched a terminal cause */
	remote->backing_latch_bugs++;
	assert(false
		   && "reclaim_or_fail returned failure with mp_end still 0 -- the "
			  "session would die as mp_end=0 with a stale reason (H3)");
	/* Release repair, not a symptom patch: the caller is about to return
	 * false and publish impl->reason, so give it the true cause rather than
	 * whatever string was last set. */
	remote_backing_capacity_fail_locked(remote, 0);
}

void remote_latch_protocol_end_locked(struct server_remote_impl* remote) {
	/* First-wins, and a decoded Packet255 owns the ending even before the
	 * pump has transcoded its text (terminal_reason_pending). A graceful
	 * local quit (graceful_stop) is the player leaving, never a protocol
	 * failure, so nothing is latched under it either. */
	if(remote->mp_end != 0 || remote->terminal_reason_pending
	   || remote->reason_from_disconnect || remote->graceful_stop)
		return;
	remote->mp_end = SERVER_REMOTE_END_PROTOCOL;
	remote->mp_end_phase = (uint32_t)remote->state;
	remote->mp_end_rx
		= atomic_load_explicit(&remote->rx_bytes, memory_order_relaxed);
}

/* A terminal protocol/internal failure on the retain path: latch mp_end 6,
 * count the termination (forced_reconnects counts a session this client
 * ended, not an implemented reconnect) and set the reason the read loop
 * publishes when the caller returns false. */
static void remote_retain_fail_locked(struct server_remote_impl* remote,
									  const char* reason) {
	remote_latch_protocol_end_locked(remote);
	remote->forced_reconnects++;
	set_reason_locked(remote, reason);
}

/* ======================================================================
 * IN-009: LOSSLESS FOLDING OF A RETAINED COLUMN'S BODY CHAIN
 * ======================================================================
 * Beta applies every Packet51 region to a held chunk in arrival order, with
 * no limit on how many: NetClientHandler.handleMapChunk -> World.setChunkData
 * -> Chunk.setChunkData (client CFR, IN-009 plan step 3), and the server's
 * PlayerInstance (CFR PlayerInstance.java:90-153) emits any number of
 * bounding-box Packet51s per column over a session. The four-body chain
 * (head + REMOTE_CHUNK_QUEUE_MAX) used to end the session on the fifth body
 * -- the IN-007 walk disconnect, column (12,-9), stream offset 336,517.
 *
 * Now the fifth body FOLDS: the complete head is inflated into the four raw
 * planes, each queued body and then the new one is inflated and copied over
 * its own region in wire order, and the result is deflated into ONE complete
 * head. Region layout, both directions, is the server's encoder
 * C:/tmp/mcb173-server-decompile/out/net/minecraft/src/Chunk.java:474-519
 * (getChunkData): for x in [x0,x1), z in [z0,z1) the id stripe of length
 * y1-y0 at (x<<11 | z<<7 | y0); then the same x/z walk three more times
 * copying (y1-y0)/2 bytes at that index >> 1 from metadata, block light and
 * sky light, in that order. The raw planes are kept in that same WIRE order
 * (ids, metadata, block light, sky light), so a body's plane p always lands
 * in raw plane p -- sky and block light can never be exchanged. Every cell
 * outside a region keeps its value. Compressed bytes may differ from the
 * originals; the decoded 81,920 bytes are exactly sequential application.
 *
 * STAMP. The folded head keeps the ORIGINAL head's arrival. Parked
 * Packet52/53 edits stay in the deferred log untouched, so each keeps its
 * CRPC_SET_BLOCK relight on installation: an edit older than the old head is
 * already shadowed by it, and a parked edit newer than the old head is, by
 * the supersede rule (remote_deferred_edit_supersede_locked at every retain,
 * the covered test in remote_deferred_edit_record_locked), not inside any
 * newer retained body -- so applying it after the folded head equals
 * applying it between the bodies. Stamping the merged snapshot with the
 * NEWEST arrival instead would shadow those edits and lose them.
 *
 * ORDER BARRIER. That argument needs every edit received before the new body
 * to be in the log already, not still in the receive queue: the fold waits
 * (read thread, impl->lock released) while a queued Packet52/53/explosion
 * could touch the column. It also waits while a promotion owns the head
 * (never touches the head or the inflate buffer of an in-flight decode),
 * while the recode scratch is busy (an unlocked demotion owns it), and while
 * the head is a PARTIAL on installed voxels (the base is the client world,
 * which only the pump may read: the pump installs that head first --
 * promoted ahead of the radius because it costs no new voxel memory -- and
 * the chain gets its room back). Waits are transient backpressure, bounded
 * by REMOTE_FOLD_WAIT_MS; a head that can never be folded (a partial with no
 * base, outside the admission radius) ends the session with mp_end 6 rather
 * than zero-filling cells this client never received.
 *
 * MEMORY (tmp/jobs/loop/IN-009-mem.md). Persistent MEM1 delta: exactly
 * REMOTE_FOLD_STATE_BYTES (24, _Static_assert'ed) in server_remote_impl; no
 * per-column fields, no new buffers. The fold reuses the RECODE scratch slot
 * (81,920 raw + 82,944 staging, the backing reservation's side budget): the
 * head inflates into raw, each later body inflates into staging and is
 * copied into raw, its inflater is ended, and staging is then reused for the
 * deflate. zlib's default allocators (MEM1 heap) hold one inflate state
 * (~7.1 KB + 32 KB window) OR one deflate state (level 6, windowBits 15,
 * memLevel 8: ~256 KB + ~5.8 KB) at a time, never both. A Z_MEM_ERROR is
 * counted in fold.zlib_alloc_failures and, if it outlasts the wait, ends the
 * session with an allocation-specific reason. Runs under impl->lock, like
 * the read-thread compaction.
 *
 * SHUTDOWN. A graceful local quit (graceful_stop / REMOTE_CLOSING) breaks
 * any wait at once: the body is freed, nothing is latched and the reason is
 * untouched (the pump no longer promotes, so the wait could not progress). */

#define REMOTE_FOLD_WAIT_MS 5000u
#define REMOTE_FOLD_RECLAIM_RETRIES 8

enum remote_fold_result {
	REMOTE_FOLD_DONE,
	REMOTE_FOLD_NOMEM,   /* zlib Z_MEM_ERROR: retry after the pump runs */
	REMOTE_FOLD_NOSPACE, /* the backing refused the folded body */
	REMOTE_FOLD_FAILED,  /* terminal; reason and mp_end already latched */
};

static uint8_t* remote_demote_scratch_locked(struct server_remote_impl* remote);

static bool remote_body_is_full_column(const struct mp_map_chunk* wire) {
	return wire->compressed && wire->sx == 16 && wire->sy == 128
		&& wire->sz == 16 && wire->y == 0 && wire->x % 16 == 0
		&& wire->z % 16 == 0 && wire->inflated_length == REMOTE_COLUMN_RAW_BYTES;
}

/* Inflate one body completely into `out`. 1 = decoded exactly
 * inflated_length bytes with the stream fully consumed; 0 = corrupt
 * (terminal); -1 = zlib could not allocate its state (Z_MEM_ERROR from
 * inflateInit or inflate: transient, counted as an allocation failure). */
static int remote_body_inflate(const struct mp_map_chunk* wire, uint8_t* out,
							   size_t capacity) {
	if(!wire->compressed || wire->inflated_length == 0
	   || wire->inflated_length > capacity
	   || wire->compressed_length > (size_t)UINT32_MAX)
		return 0;
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	int init = inflateInit(&zs);
	if(init == Z_MEM_ERROR)
		return -1;
	if(init != Z_OK)
		return 0;
	zs.next_in = wire->compressed;
	zs.avail_in = (uInt)wire->compressed_length;
	zs.next_out = out;
	zs.avail_out = (uInt)wire->inflated_length;
	int result = inflate(&zs, Z_FINISH);
	size_t produced = (size_t)zs.total_out;
	uInt left = zs.avail_in;
	inflateEnd(&zs);
	if(result == Z_MEM_ERROR)
		return -1;
	return result == Z_STREAM_END && produced == wire->inflated_length
			&& left == 0
		? 1
		: 0;
}

/* Chunk.java:474-519's region walk, writing `body` (decoded) into a full
 * column's ids and wire-order planes (metadata, block light, sky light).
 * False when the region is not inside the column or the body is too short. */
static bool remote_column_apply_region(uint8_t* ids, uint8_t* const planes[3],
									   const struct mp_map_chunk* wire,
									   int32_t chunk_x, int32_t chunk_z,
									   const uint8_t* body) {
	int64_t x0 = (int64_t)wire->x - (int64_t)chunk_x * 16;
	int64_t z0 = (int64_t)wire->z - (int64_t)chunk_z * 16;
	int64_t y0 = wire->y;
	if(!wire->sx || !wire->sy || !wire->sz || x0 < 0 || z0 < 0 || y0 < 0
	   || x0 + wire->sx > 16 || z0 + wire->sz > 16 || y0 + wire->sy > 128)
		return false;
	size_t stripe = wire->sy, half = (size_t)wire->sy / 2u;
	size_t columns = (size_t)wire->sx * wire->sz;
	if(columns * stripe + 3u * columns * half > wire->inflated_length)
		return false;
	size_t o = 0;
	for(int64_t x = x0; x < x0 + wire->sx; x++)
		for(int64_t z = z0; z < z0 + wire->sz; z++) {
			size_t idx = ((size_t)x << 11) | ((size_t)z << 7) | (size_t)y0;
			memcpy(ids + idx, body + o, stripe);
			o += stripe;
		}
	for(unsigned plane = 0; plane < 3; plane++) {
		uint8_t* base = planes[plane];
		for(int64_t x = x0; x < x0 + wire->sx; x++)
			for(int64_t z = z0; z < z0 + wire->sz; z++) {
				size_t idx = ((size_t)x << 11) | ((size_t)z << 7) | (size_t)y0;
				memcpy(base + (idx >> 1), body + o, half);
				o += half;
			}
	}
	return true;
}

/* Deflate the folded column into `stage` at REMOTE_DEMOTE_DEFLATE_LEVEL
 * (zlib defaults: windowBits 15, memLevel 8 -- the same parameters as the
 * demotion's remote_column_deflate, kept separate only so a Z_MEM_ERROR is
 * distinguishable). 1 = complete stream in *out_length bytes; 0 = deflate
 * failed for a reason other than memory (terminal); -1 = Z_MEM_ERROR. */
static int remote_fold_deflate(const uint8_t* raw, uint8_t* stage,
							   size_t* out_length) {
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	int init = deflateInit(&zs, REMOTE_DEMOTE_DEFLATE_LEVEL);
	if(init == Z_MEM_ERROR)
		return -1;
	if(init != Z_OK)
		return 0;
	zs.next_in = (Bytef*)raw;
	zs.avail_in = (uInt)REMOTE_COLUMN_RAW_BYTES;
	zs.next_out = stage;
	zs.avail_out = (uInt)REMOTE_DEMOTE_STAGE_BYTES;
	int result = deflate(&zs, Z_FINISH);
	size_t produced = (size_t)zs.total_out;
	deflateEnd(&zs);
	if(result == Z_MEM_ERROR)
		return -1;
	if(result != Z_STREAM_END || produced == 0
	   || produced > MP_CHUNK_COMPRESSED_CAP)
		return 0;
	*out_length = produced;
	return 1;
}

static void remote_fold_fail_locked(struct server_remote_impl* remote,
									const struct remote_chunk* chunk,
									const char* what) {
	char reason[160];
	snprintf(reason, sizeof(reason), "%s (column %ld,%ld)", what,
			 (long)chunk->chunk_x, (long)chunk->chunk_z);
	remote->chain_rejections++;
	remote_retain_fail_locked(remote, reason);
}

/* Fold the complete head, every queued body and `incoming` into one complete
 * head (see the block comment; REL-MP-FIX2a: or onto a pre-chunk base).
 * Caller: impl->lock held, the head not owned by a promotion, no older
 * edit for the column still queued, recode scratch not busy. On DONE the
 * chain is a single head with the old head's stamp and `incoming`'s body has
 * been consumed (freed, NULLed). On any other result the chain is unchanged
 * (only superseded edits left the log). `incoming` may be
 * NULL (REL-MP compaction): head plus queued bodies only. */
static enum remote_fold_result
remote_chunk_fold_locked(struct server_remote_impl* remote,
						 struct remote_chunk* chunk,
						 struct mp_map_chunk* incoming, size_t* nospace_bytes) {
	uint8_t* scratch = remote_demote_scratch_locked(remote);
	if(!scratch) {
		/* The reservation's scratch side budget cannot hold the slot at
		 * all: a capacity answer, ended visibly (mp_end 5). */
		remote->chain_rejections++;
		remote_backing_capacity_fail_locked(remote, REMOTE_DEMOTE_SCRATCH_BYTES);
		return REMOTE_FOLD_FAILED;
	}
	/* REL-MP-FIX2a: supersede against every body's ORIGINAL stamp before the
	 * fold erases them (both callers; retain supersedes `incoming` itself).
	 * Lossless whatever the result: the drain applies the same rule. */
	if(chunk->wire.compressed)
		remote_deferred_edit_supersede_locked(remote, chunk, &chunk->wire,
											  chunk->arrival);
	for(uint8_t q = 0; q < chunk->queued_count; q++)
		remote_deferred_edit_supersede_locked(remote, chunk, &chunk->queued[q],
											  chunk->queued_arrival[q]);
	uint8_t* raw = scratch;
	uint8_t* stage = scratch + REMOTE_COLUMN_RAW_BYTES;
	/* No full head on a never-installed column: fold onto the implicit Beta
	 * pre-chunk, materialised in scratch and encoded once with the regions.
	 * Air, metadata/block light 0, sky light 15 [ChunkProviderClient.java:40
	 * prepareChunk]. An installed column's base is its voxels. */
	bool base_full = remote_body_is_full_column(&chunk->wire);
	bool synthetic = chunk->synthetic_base || !base_full;
	if(!base_full && (chunk->installed || (!chunk->wire.compressed && !incoming))) {
		assert(!"synthetic base for an installed or bodyless column");
		remote_fold_fail_locked(remote, chunk,
								"remote Packet51 partial column has no base "
								"to fold onto");
		return REMOTE_FOLD_FAILED;
	}
	int r = 1;
	if(base_full)
		r = remote_body_inflate(&chunk->wire, raw, REMOTE_COLUMN_RAW_BYTES);
	else {
		memset(raw, 0, REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES);
		memset(raw + REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES, 0xFF,
			   REMOTE_COLUMN_NIBBLES);
	}
	if(r < 0) {
		remote->fold.zlib_alloc_failures++;
		return REMOTE_FOLD_NOMEM;
	}
	if(r == 0) {
		remote_fold_fail_locked(remote, chunk,
								"remote Packet51 head does not decode");
		return REMOTE_FOLD_FAILED;
	}
	/* k = -1 is a partial head applied onto the pre-chunk base. */
	for(int k = base_full ? 0 : -1; k <= (int)chunk->queued_count; k++) {
		const struct mp_map_chunk* body = k < 0 ? &chunk->wire
			: k < (int)chunk->queued_count      ? &chunk->queued[k]
												: incoming;
		if(k < 0 && !body->compressed)
			continue; /* bodyless: the base and `incoming` only */
		if(!body)
			break; /* NULL incoming: the chain alone */
		if(remote_body_is_full_column(body))
			synthetic = false; /* a real snapshot overwrote the base */
		r = remote_body_inflate(body, stage, REMOTE_COLUMN_RAW_BYTES);
		if(r < 0) {
			remote->fold.zlib_alloc_failures++;
			return REMOTE_FOLD_NOMEM;
		}
		if(r == 0) {
			remote_fold_fail_locked(remote, chunk,
									"remote Packet51 body does not decode");
			return REMOTE_FOLD_FAILED;
		}
		uint8_t* const planes[3] = {raw + REMOTE_COLUMN_CELLS,
			raw + REMOTE_COLUMN_CELLS + REMOTE_COLUMN_NIBBLES,
			raw + REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES};
		if(!remote_column_apply_region(raw, planes, body, chunk->chunk_x,
									   chunk->chunk_z, stage)) {
			remote_fold_fail_locked(remote, chunk,
									"remote Packet51 region outside its column");
			return REMOTE_FOLD_FAILED;
		}
	}
	size_t compressed_length = 0;
	r = remote_fold_deflate(raw, stage, &compressed_length);
	if(r < 0) {
		remote->fold.zlib_alloc_failures++;
		return REMOTE_FOLD_NOMEM;
	}
	if(r == 0) {
		remote_fold_fail_locked(remote, chunk,
								"remote Packet51 fold could not re-encode");
		return REMOTE_FOLD_FAILED;
	}
	/* Replacement allocated BEFORE anything of the original chain is freed;
	 * a refusal leaves the chain exactly as it was. */
	void* body = remote_body_alloc(compressed_length);
	if(!body) {
		*nospace_bytes = compressed_length;
		return REMOTE_FOLD_NOSPACE;
	}
	memcpy(body, stage, compressed_length);
	/* A pre-chunk base is anchored at the load (residency_seq), not at the
	 * first partial: an older edit outside every region must stay newer than
	 * the folded head, or the drain would shadow it. */
	uint64_t stamp = base_full ? chunk->arrival : chunk->residency_seq;
	struct mp_map_chunk folded = {
		.x = chunk->chunk_x * 16,
		.y = 0,
		.z = chunk->chunk_z * 16,
		.sx = 16,
		.sy = 128,
		.sz = 16,
		.inflated_length = REMOTE_COLUMN_RAW_BYTES,
		.compressed = body,
		.compressed_length = compressed_length,
	};
	/* Commit: the chain's bodies go, the folded head takes the old head's
	 * place and stamp, the incoming body is consumed. All under the lock. */
	remote_chunk_drop_queued_locked(remote, chunk);
	remote_chunk_install_head_locked(remote, chunk, &folded, stamp);
	chunk->synthetic_base = synthetic;
	if(incoming) {
		remote_body_free(incoming->compressed);
		incoming->compressed = NULL;
		incoming->compressed_length = 0;
	}
	remote->fold.folds++;
	return REMOTE_FOLD_DONE;
}

/* Could a Packet52/53/explosion still in the receive queue touch this
 * column? Those were received BEFORE the body being retained, so they must
 * reach the deferred log before a fold (the ORDER BARRIER above). The ring
 * keeps an event counted until the pump has finished processing it.
 * Explosions are treated as touching (their offsets span columns).
 * REL-MP-FIX2b: one received after `newest` (the newest stamp the fold
 * erases) parks above the folded head anyway, so it is no barrier. */
static bool remote_ring_touches_column_locked(
	const struct server_remote_impl* remote, int32_t chunk_x, int32_t chunk_z,
	uint64_t newest) {
	for(size_t n = 0; n < remote->event_count; n++) {
		const struct remote_event* e
			= &remote->events[(remote->event_head + n) % REMOTE_QUEUE_CAPACITY];
		if(e->kind != REMOTE_EVENT_PACKET || (e->seq << 16) > newest)
			continue;
		switch(e->packet.type) {
			case MP_EVENT_BLOCK_CHANGE:
				if((int32_t)floor((double)e->packet.payload.block_change.x / 16.0)
					   == chunk_x
				   && (int32_t)floor(
						  (double)e->packet.payload.block_change.z / 16.0)
					   == chunk_z)
					return true;
				break;
			case MP_EVENT_MULTI_BLOCK_CHANGE:
				if(e->packet.payload.multi_block_change.chunk_x == chunk_x
				   && e->packet.payload.multi_block_change.chunk_z == chunk_z)
					return true;
				break;
			case MP_EVENT_EXPLOSION: return true;
			default: break;
		}
	}
	return false;
}

static uint64_t remote_chunk_distance_locked(const struct server_remote_impl* remote,
											 const struct remote_chunk* chunk);

enum remote_fold_gate {
	REMOTE_FOLD_GATE_NOW,
	REMOTE_FOLD_GATE_WAIT,
};

static enum remote_fold_gate
remote_fold_gate_locked(const struct server_remote_impl* remote, size_t index,
						uint64_t newest, uint8_t* cause) {
	const struct remote_chunk* chunk = &remote->chunks[index];
	if(chunk->state == REMOTE_CHUNK_PROMOTING
	   || (remote->promotion.active && remote->promotion.entry == index)) {
		*cause = REMOTE_FOLD_WAIT_HEAD_BUSY; /* the decode owns the head */
		return REMOTE_FOLD_GATE_WAIT;
	}
	/* A partial head on installed voxels: the pump installs it (the base it
	 * lands on) and frees a chain slot. REL-MP-FIX2b: with no voxels it folds
	 * onto the pre-chunk, as Beta applies it [NetClientHandler.java:294]. */
	if(!remote_body_is_full_column(&chunk->wire) && chunk->installed) {
		*cause = REMOTE_FOLD_WAIT_PARTIAL_HEAD;
		return REMOTE_FOLD_GATE_WAIT;
	}
	if(remote_ring_touches_column_locked(remote, chunk->chunk_x,
										 chunk->chunk_z, newest)) {
		*cause = REMOTE_FOLD_WAIT_EDIT_ORDER;
		return REMOTE_FOLD_GATE_WAIT;
	}
	if(remote->recode_busy) {
		*cause = REMOTE_FOLD_WAIT_SCRATCH;
		return REMOTE_FOLD_GATE_WAIT;
	}
	return REMOTE_FOLD_GATE_NOW;
}

bool remote_retain_map_locked(struct server_remote_impl* remote,
										 struct mp_event* event) {
	struct mp_map_chunk* wire = &event->payload.map_chunk;
	int32_t chunk_x = (int32_t)floor((double)wire->x / 16.0);
	int32_t chunk_z = (int32_t)floor((double)wire->z / 16.0);
	/* Retention is keyed by the ORIGIN column; the codec bounds the size but
	 * not the placement, so a body crossing a column boundary (x=15, sx=2)
	 * would silently mis-key its far part. Beta's PlayerInstance never sends
	 * one (its sub-volume is the change bbox within one chunk), so refuse it
	 * visibly rather than guess. */
	if(wire->sx == 0 || wire->sy == 0 || wire->sz == 0
	   || (int32_t)floor(((double)wire->x + wire->sx - 1) / 16.0) != chunk_x
	   || (int32_t)floor(((double)wire->z + wire->sz - 1) / 16.0) != chunk_z) {
		remote_retain_fail_locked(remote,
								  "remote Packet51 crosses a column boundary");
		return false;
	}
	size_t index = remote_chunk_find_locked(remote, chunk_x, chunk_z);
	if(index == REMOTE_CHUNK_ENTRY_CAPACITY)
		index = remote_chunk_create_locked(remote, chunk_x, chunk_z);
	if(index == REMOTE_CHUNK_ENTRY_CAPACITY) {
		remote_retain_fail_locked(remote, "remote chunk coordinate cap exhausted");
		return false;
	}
	/* W2: NO capacity test and NO eviction here any more. The body this
	 * event carries was already allocated out of the backing reservation by
	 * the decoder (mp_codec.c's Packet51 header case), so "does it fit" was
	 * answered BEFORE a byte of it was read -- the check-before-allocate
	 * Astra item 4 asks for, instead of a cap tested after the fact with
	 * destruction as the only remedy. The two TOMBSTONE guards that used to
	 * stand here went with the eviction that created tombstones; the state
	 * is now unreachable (see its enum comment).
	 *
	 * retained_bytes/_peak stay maintained by the install/drop helpers as the
	 * MP layer's own view of the backing, and retained_byte_cap now holds the
	 * reservation capacity (server_remote_init), so the pair still reads as
	 * "bytes held / bytes available" on the status and heartbeat paths.
	 *
	 * The byte TEST itself stays, as a second and cheaper guard in front of
	 * the same clean refusal: in production it is the reservation's own
	 * capacity, so it agrees with the allocator by construction and never
	 * fires first; it remains the lever
	 * server_remote_test_set_retained_byte_cap exists to move, so a test can
	 * drive the capacity path with two columns instead of two thousand. What
	 * changed is the REMEDY -- reclaim losslessly, then end the session
	 * visibly (mp_end=5), never evict a column's only copy. */
	if(remote->retained_bytes > remote->retained_byte_cap
	   || wire->compressed_length
		   > remote->retained_byte_cap - remote->retained_bytes) {
		if(!remote_backing_reclaim_or_fail_locked(remote,
												  wire->compressed_length)) {
			remote_backing_fail_guard_locked(remote);
			return false;
		}
		if(remote->retained_bytes > remote->retained_byte_cap
		   || wire->compressed_length
			   > remote->retained_byte_cap - remote->retained_bytes) {
			/* The reclaim freed something, just not enough: END, do not
			 * reclaim again. F5a (review finding H3) -- this used to be a
			 * second `(void) remote_backing_reclaim_or_fail_locked(remote, 0)`
			 * whose SUCCESS path set no reason and latched no mp_end, so the
			 * `return false` below published whatever stale string
			 * impl->reason held and the session died as mp_end=0. The
			 * terminal is now unconditional (wanted = 0 so the reason names
			 * the budget rather than this one body). */
			remote_backing_capacity_fail_locked(remote, 0);
			return false;
		}
	}
	/* Receive-order stamp (struct remote_event::seq): this runs on the read
	 * thread under the lock, so everything already parked or queued was
	 * received BEFORE this body. */
	uint64_t arrival = ((uint64_t)++remote->arrival_sequence) << 16;
	bool waited = false;
	ptime_t wait_deadline = time_get();
	unsigned reclaims = 0;
	for(;;) {
		/* Re-resolved every pass: a fold wait releases impl->lock. */
		index = remote_chunk_find_locked(remote, chunk_x, chunk_z);
		if(index == REMOTE_CHUNK_ENTRY_CAPACITY)
			index = remote_chunk_create_locked(remote, chunk_x, chunk_z);
		if(index == REMOTE_CHUNK_ENTRY_CAPACITY) {
			remote->fold.wait_active = false;
			remote_retain_fail_locked(remote,
									  "remote chunk coordinate cap exhausted");
			return false;
		}
		struct remote_chunk* chunk = &remote->chunks[index];
		/* Parked edits inside this body's volume are older than it: the
		 * snapshot post-dates them (Beta later-wins). Safe because this body
		 * WILL be installed, or be coalesced into a covering successor that
		 * will, or folded into a head that will, or the residency ends
		 * (unload purge / teardown). Re-run after a wait: the pump may have
		 * parked older edits meanwhile. */
		remote_deferred_edit_supersede_locked(remote, chunk, wire, arrival);
		if(chunk->state != REMOTE_CHUNK_PROMOTING
		   && chunk->state != REMOTE_CHUNK_RETAINED) {
			remote->fold.wait_active = false;
			remote_chunk_install_head_locked(remote, chunk, wire, arrival);
			chunk->state = REMOTE_CHUNK_RETAINED;
			return true;
		}
		/* Fix A (2026-08-30) generalized: a body for a column whose head is
		 * mid-decode (or still waiting) is APPENDED in receive order -- the
		 * in-flight decode always finishes and installs; a resend that
		 * covers a waiting head replaces it (the old park semantics); a
		 * partial that does not is applied after it, as Beta does. */
		if(remote_chunk_append_body_locked(remote, chunk, wire, arrival)) {
			remote->fold.wait_active = false;
			return true;
		}
		/* IN-009: the chain is full -- fold it (see the block above). */
		uint8_t cause = REMOTE_FOLD_WAIT_NONE;
		enum remote_fold_gate gate
			= remote_fold_gate_locked(remote, index, UINT64_MAX, &cause);
		if(gate == REMOTE_FOLD_GATE_NOW) {
			size_t nospace_bytes = 0;
			enum remote_fold_result result
				= remote_chunk_fold_locked(remote, chunk, wire, &nospace_bytes);
			if(result == REMOTE_FOLD_DONE) {
				remote->fold.wait_active = false;
				return true;
			}
			if(result == REMOTE_FOLD_FAILED) {
				remote->fold.wait_active = false;
				return false;
			}
			if(result == REMOTE_FOLD_NOSPACE) {
				/* The backing ladder (lossless reclaims, then the visible
				 * capacity end). Its compaction may reuse the recode scratch,
				 * so a reclaim restarts the fold from the bodies. */
				if(++reclaims > REMOTE_FOLD_RECLAIM_RETRIES) {
					remote->fold.wait_active = false;
					remote->chain_rejections++;
					remote_backing_capacity_fail_locked(remote, nospace_bytes);
					return false;
				}
				if(!remote_backing_reclaim_or_fail_locked(remote, nospace_bytes)) {
					remote->fold.wait_active = false;
					remote->chain_rejections++;
					remote_backing_fail_guard_locked(remote); /* F5a (H3) */
					return false;
				}
				continue;
			}
			/* REMOTE_FOLD_NOMEM: zlib could not allocate its state
			 * (already counted in fold.zlib_alloc_failures). Transient --
			 * the pump frees MEM1 as it installs/demotes -- but if the wait
			 * expires on it the reason says so, never a generic timeout. */
			cause = REMOTE_FOLD_WAIT_ZLIB_ALLOC;
		}
		/* INTENTIONAL LOCAL SHUTDOWN. A graceful quit (graceful_stop, state
		 * REMOTE_CLOSING) stops the pump promoting, so a promotion-dependent
		 * wait could never progress. It is not a failure: drop this body
		 * cleanly (the session's world is being torn down; nothing will
		 * ever install it), latch nothing, keep the reason, and report it
		 * consumed so the read loop keeps draining to the server's EOF --
		 * the graceful path's own exit (remote_read_thread). */
		if(remote->graceful_stop || remote->state == REMOTE_CLOSING) {
			remote->fold.wait_active = false;
			remote_body_free(wire->compressed);
			wire->compressed = NULL;
			wire->compressed_length = 0;
			return true;
		}
		/* TRANSIENT BACKPRESSURE: hold this body (still owned by the caller)
		 * and stop reading until the pump has moved the column on. */
		remote->fold.wait_cause = cause;
		if(!waited) {
			waited = true;
			wait_deadline = time_add_ms(time_get(), REMOTE_FOLD_WAIT_MS);
			remote->fold.waits++;
		} else if(time_reached(time_get(), wait_deadline)) {
			remote->fold.wait_active = false;
			remote_fold_fail_locked(
				remote, chunk,
				cause == REMOTE_FOLD_WAIT_ZLIB_ALLOC
					? "remote Packet51 fold: zlib allocation failed"
					: "remote Packet51 body chain could not be folded in time");
			return false;
		}
		remote->fold.wait_active = true;
		remote->fold.wait_x = chunk_x;
		remote->fold.wait_z = chunk_z;
		thread_mutex_unlock(&remote->lock);
		thread_msleep(1);
		thread_mutex_lock(&remote->lock);
		/* Cancel/teardown ran while unlocked: the session is over, the body
		 * stays with the caller, nothing is counted against the stream. */
		if(remote->stop_now) {
			remote->fold.wait_active = false;
			return false;
		}
		/* A graceful quit that began while unlocked: handled HERE, before the
		 * next pass retries retention or folding, so no terminal path (a
		 * capacity end, a decode failure) can latch or overwrite the reason
		 * of a session that is being shut down on purpose. Same clean drop as
		 * the guard above. */
		if(remote->graceful_stop || remote->state == REMOTE_CLOSING) {
			remote->fold.wait_active = false;
			remote_body_free(wire->compressed);
			wire->compressed = NULL;
			wire->compressed_length = 0;
			return true;
		}
	}
}

/* Drops every parked deferred edit for one chunk column WITHOUT applying it
 * -- called from remote_pre_chunk_locked's unload branch below. A parked
 * entry's entire reason for existing is "apply it the moment this column's
 * NEXT promotion completes" (remote_deferred_edit_drain_chunk_locked's own
 * comment, further down this file); once the column actually unloads, that
 * promise is broken on purpose -- the server will stream a brand-new
 * REMOTE_CHUNK_RESIDENT payload (Packet51/CRPC_CHUNK) on the NEXT promotion,
 * and replaying a pre-unload edit on top of that fresh base volume would
 * silently resurrect a value the server itself may since have overwritten
 * (a redstone toggle, a piston retraction, another player's edit landing
 * while this column was gone) -- stale data winning over current data. Left
 * unpurged, remote_deferred_edit_drain_chunk_locked would replay it anyway
 * on the reload, because it drains purely by (chunk_x, chunk_z) coordinate
 * match with no notion of "which residency this edit was parked during".
 * Counted via deferred_edits_dropped -- the same "this parked edit will
 * never be applied" counter remote_deferred_edit_record_locked's own
 * drop-oldest eviction increments -- never a silent disappearance. Defined
 * ahead of remote_deferred_edit_record_locked/_drain_chunk_locked (which
 * otherwise appear together, further down) purely so it is declared before
 * remote_pre_chunk_locked, its only caller, needs it; it does not depend on
 * either. */
static void remote_edit_drop_locked(struct server_remote_impl* remote,
									struct remote_chunk* chunk, uint16_t* link,
									uint16_t prev);
static void remote_deferred_edit_purge_chunk_locked(
	struct server_remote_impl* remote, struct remote_chunk* chunk) {
	while(chunk->edit_head) {
		remote_edit_drop_locked(remote, chunk, &chunk->edit_head, 0);
		remote->deferred_edits_dropped++;
	}
}

bool remote_pre_chunk_locked(struct server_remote_impl* remote,
										int32_t chunk_x, int32_t chunk_z, bool mode) {
	size_t index = remote_chunk_find_locked(remote, chunk_x, chunk_z);
	if(index == REMOTE_CHUNK_ENTRY_CAPACITY)
		index = remote_chunk_create_locked(remote, chunk_x, chunk_z);
	if(index == REMOTE_CHUNK_ENTRY_CAPACITY) {
		remote_retain_fail_locked(remote, "remote chunk announce cap exhausted");
		return false;
	}
	struct remote_chunk* chunk = &remote->chunks[index];
	if(mode) {
		/* Read thread, under the lock: everything already queued was
		 * received before this load (struct remote_chunk::residency_seq). */
		chunk->residency_seq = ((uint64_t)++remote->arrival_sequence) << 16;
		if(chunk->unload_pending)
			chunk->reload_after_unload = true;
		/* W2: the TOMBSTONE guard that stood here is gone with the eviction
		 * that produced tombstones -- no state in this table is ever a
		 * "column we threw away", so a load can no longer collide with one. */
		return true;
	}
	if(chunk->state == REMOTE_CHUNK_PROMOTING)
		remote_promotion_cancel_locked(remote, false);
	else
		remote_chunk_drop_body_locked(remote, chunk);
	remote_chunk_drop_queued_locked(remote, chunk);
	chunk->state = REMOTE_CHUNK_ANNOUNCED;
	chunk->unload_pending = true;
	/* W1: the installed voxels are about to go (the pump's unload emit), and
	 * the parked edits below are purged, so nothing has been "applied" to
	 * this coordinate's next residency yet. */
	chunk->newest_applied = 0;
	/* The unload ends the residency for every edit received before it
	 * (struct remote_chunk::residency_seq): an edit the pump has not reached
	 * yet compares older than this and is dropped, exactly like the parked
	 * ones purged below. */
	chunk->residency_seq = ((uint64_t)++remote->arrival_sequence) << 16;
	/* This column's promise to apply any parked deferred edit ends here --
	 * see remote_deferred_edit_purge_chunk_locked's own comment for why a
	 * reload must start from a clean slate instead of replaying stale edits
	 * over the fresh chunk data the next promotion brings. */
	remote_deferred_edit_purge_chunk_locked(remote, chunk);
	/* The newest Packet50 mode owns the final state. unload -> load -> unload
	 * before a pump must not preserve the middle load's reload intent. */
	chunk->reload_after_unload = false;
	return true;
}

static bool remote_chunk_resident_locked(const struct server_remote_impl* remote,
										 int32_t block_x, int32_t block_z) {
	int32_t chunk_x = (int32_t)floor((double)block_x / 16.0);
	int32_t chunk_z = (int32_t)floor((double)block_z / 16.0);
	size_t index = remote_chunk_find_locked(remote, chunk_x, chunk_z);
	return index != REMOTE_CHUNK_ENTRY_CAPACITY
		&& remote->chunks[index].state == REMOTE_CHUNK_RESIDENT;
}

/* Deferred-edit log helpers (Plan section 7 -- see struct remote_deferred_
 * edit's comment in server_remote_internal.h for the full design). All of
 * them assume remote->lock is already held by the caller, matching every
 * other "_locked" helper in this file.
 *
 * REL-MP-JOINSPEED: the log is a pool of REMOTE_DEFERRED_EDIT_CAPACITY
 * entries with one chain per column (remote_chunk::edit_head/_tail/_count),
 * oldest arrival first. Indices are 1-based so a zeroed chunk entry is an
 * empty chain. */

static inline struct remote_deferred_edit*
remote_edit_at(struct server_remote_impl* remote, uint16_t ref) {
	return &remote->deferred_edit_log[ref - 1u];
}

/* The Packet51 column index of (x, y, z): ids[(x << 11) | (z << 7) | y]. The
 * caller guarantees y < 128 (remote_deferred_edit_record_locked drops the
 * rest, as Beta's World.setBlockAndMetadata does). */
static uint16_t remote_edit_cell(int32_t x, uint8_t y, int32_t z) {
	return (uint16_t)((((uint32_t)x & 15u) << 11) | (((uint32_t)z & 15u) << 7)
					  | ((uint32_t)y & 127u));
}

/* Is the parked edit `e` of `chunk` inside body volume `v`? */
static bool remote_edit_in_volume(const struct remote_chunk* chunk,
								  const struct remote_deferred_edit* e,
								  const struct mp_map_chunk* v) {
	return remote_volume_contains(v,
		chunk->chunk_x * 16 + (int32_t)((e->cell >> 11) & 15u),
		(uint8_t)(e->cell & 127u),
		chunk->chunk_z * 16 + (int32_t)((e->cell >> 7) & 15u));
}

/* Claim the pool on the first park of a session (or after a dimension
 * change's clear). 256 KiB of the MEM2 scratch side budget, zeroed once. */
static bool remote_edit_pool_locked(struct server_remote_impl* remote) {
	if(remote->deferred_edit_log)
		return true;
	size_t bytes = (size_t)REMOTE_DEFERRED_EDIT_CAPACITY
		* sizeof(struct remote_deferred_edit);
	void* pool = mp_backing_scratch_acquire(MP_BACKING_SCRATCH_EDITS, bytes);
	if(!pool)
		return false;
	memset(pool, 0, bytes);
	remote->deferred_edit_log = pool;
	remote->deferred_edit_free = 0;
	remote->deferred_edit_cursor = 0;
	return true;
}

/* A free entry, or 0 when the pool is full (or cannot be claimed). */
static uint16_t remote_edit_alloc_locked(struct server_remote_impl* remote) {
	if(!remote_edit_pool_locked(remote))
		return 0;
	if(remote->deferred_edit_free) {
		uint16_t ref = remote->deferred_edit_free;
		remote->deferred_edit_free = remote_edit_at(remote, ref)->next;
		return ref;
	}
	if(remote->deferred_edit_cursor < REMOTE_DEFERRED_EDIT_CAPACITY)
		return (uint16_t)++remote->deferred_edit_cursor;
	return 0;
}

/* Unlink the entry `*link` points at from `chunk`'s chain; `prev` is the
 * entry that owns `link` (0 when link is &chunk->edit_head). The entry stays
 * allocated and owned. Returns it. */
static uint16_t remote_edit_detach_locked(struct server_remote_impl* remote,
										  struct remote_chunk* chunk,
										  uint16_t* link, uint16_t prev) {
	uint16_t ref = *link;
	struct remote_deferred_edit* e = remote_edit_at(remote, ref);
	*link = e->next;
	if(chunk->edit_tail == ref)
		chunk->edit_tail = prev;
	if(chunk->edit_count)
		chunk->edit_count--;
	e->next = 0;
	return ref;
}

/* Unlink AND free (the entry left the log: applied, superseded, dropped or
 * folded -- the caller counts which). */
static void remote_edit_drop_locked(struct server_remote_impl* remote,
									struct remote_chunk* chunk, uint16_t* link,
									uint16_t prev) {
	uint16_t ref = remote_edit_detach_locked(remote, chunk, link, prev);
	struct remote_deferred_edit* e = remote_edit_at(remote, ref);
	e->owner = 0;
	e->next = remote->deferred_edit_free;
	remote->deferred_edit_free = ref;
	if(remote->deferred_edits > 0)
		remote->deferred_edits--;
}

/* Link a detached entry into `chunk`'s chain at its arrival position. Edits
 * are recorded in receive order, so this is an append in practice; the
 * ordered insert keeps the invariant whatever a caller does. */
static void remote_edit_link_locked(struct server_remote_impl* remote,
									struct remote_chunk* chunk, uint16_t ref) {
	struct remote_deferred_edit* e = remote_edit_at(remote, ref);
	e->next = 0;
	if(!chunk->edit_tail
	   || remote_edit_at(remote, chunk->edit_tail)->arrival <= e->arrival) {
		if(chunk->edit_tail)
			remote_edit_at(remote, chunk->edit_tail)->next = ref;
		else
			chunk->edit_head = ref;
		chunk->edit_tail = ref;
	} else {
		/* Some entry is newer, so the insert point is before it and the tail
		 * does not move. */
		uint16_t* link = &chunk->edit_head;
		while(remote_edit_at(remote, *link)->arrival <= e->arrival)
			link = &remote_edit_at(remote, *link)->next;
		e->next = *link;
		*link = ref;
	}
	chunk->edit_count++;
}

/* Drop every parked edit of `chunk` that a NEWER body `wire` covering its cell
 * has superseded (Beta later-wins: a Packet51 overwrites its whole region).
 * Called at retain (the new body), by the fold, and as the reclaim pass of an
 * exhausted log. The chain is in arrival order, so the walk stops at the
 * first edit that is not older than the body. */
static void remote_deferred_edit_supersede_locked(
	struct server_remote_impl* remote, struct remote_chunk* chunk,
	const struct mp_map_chunk* wire, uint64_t arrival) {
	uint16_t prev = 0;
	uint16_t* link = &chunk->edit_head;
	while(*link) {
		struct remote_deferred_edit* e = remote_edit_at(remote, *link);
		if(e->arrival >= arrival)
			break;
		if(remote_edit_in_volume(chunk, e, wire)) {
			remote_edit_drop_locked(remote, chunk, link, prev);
			remote->deferred_edits_superseded++;
			continue;
		}
		prev = *link;
		link = &e->next;
	}
}

/* Park (or update) one edit. A coordinate already in the log is a resend --
 * later data supersedes earlier (last-write-wins), the exact rule Fix A's
 * pending_wire park applies to a whole-chunk resend above; it does not
 * change the live pending count. A brand-new coordinate consumes a free
 * slot; a full log first reclaims superseded entries, then compacts, and
 * only then ends the session (see the passes below). */
static void remote_deferred_edit_record_locked(struct server_remote_impl* remote,
												 int32_t x, uint8_t y, int32_t z,
												 uint8_t type, uint8_t metadata) {
	if(remote->stop_now)
		return; /* connection already terminated: nothing to park for */
	uint64_t arrival = (remote->pump_packet_seq << 16)
		| (uint64_t)(remote->pump_packet_sub++ & 0xFFFFu);
	/* REL-MP-JOINSPEED: a cell above the 128-block column does not exist
	 * client-side either -- Beta's World.setBlockAndMetadata returns false
	 * for y >= 128 -- so it is dropped (counted) rather than parked; parking
	 * it would have folded OUTSIDE the column's planes. */
	if(y >= 128) {
		remote->deferred_edits_dropped++;
		return;
	}
	/* Astra mp2 P1: an edit from a residency that has ended (received before
	 * the unload the read thread already applied, or for a column that was
	 * never announced) must not be parked for a promotion that will never
	 * come -- it would be stale data at best and log exhaustion at worst. */
	int32_t chunk_x = (int32_t)floor((double)x / 16.0);
	int32_t chunk_z = (int32_t)floor((double)z / 16.0);
	size_t index = remote_chunk_find_locked(remote, chunk_x, chunk_z);
	if(index == REMOTE_CHUNK_ENTRY_CAPACITY
	   || arrival < remote->chunks[index].residency_seq
	   /* received after the unload but before any reload: the server will
	    * resend the column in full on the next load, and the entry is about
	    * to be released by remote_emit_pending_unloads_locked -- parking
	    * would orphan the edit in the log. */
	   || (remote->chunks[index].unload_pending
		   && !remote->chunks[index].reload_after_unload)) {
		remote->deferred_edits_dropped++;
		return;
	}
	struct remote_chunk* chunk = &remote->chunks[index];
	/* Astra mp2 P2: an edit a NEWER retained body of its column already
	 * covers is superseded on arrival -- it needs no slot, and at a
	 * saturated log it must not be the one that terminates the session. */
	{
		bool covered = false;
		if((chunk->state == REMOTE_CHUNK_RETAINED
			|| chunk->state == REMOTE_CHUNK_PROMOTING)
		   && arrival < chunk->arrival
		   && remote_volume_contains(&chunk->wire, x, y, z))
			covered = true;
		for(uint8_t q = 0; !covered && q < chunk->queued_count; q++)
			if(arrival < chunk->queued_arrival[q]
			   && remote_volume_contains(&chunk->queued[q], x, y, z))
				covered = true;
		if(covered) {
			remote->deferred_edits_superseded++;
			return;
		}
	}
	uint16_t cell = remote_edit_cell(x, y, z);
	{
		uint16_t prev = 0;
		uint16_t* link = &chunk->edit_head;
		while(*link) {
			struct remote_deferred_edit* e = remote_edit_at(remote, *link);
			if(e->cell == cell) {
				/* The later edit of this cell wins, and moves to its new
				 * arrival position in the chain. */
				uint16_t ref = remote_edit_detach_locked(remote, chunk, link, prev);
				e->type = type;
				e->metadata = metadata;
				e->arrival = arrival;
				remote_edit_link_locked(remote, chunk, ref);
				return;
			}
			prev = *link;
			link = &e->next;
		}
	}
	uint16_t ref = 0;
	/* F5a (review finding M8): THREE passes, not two. Pass 2 is the one that
	 * removes the cliff -- compact backed columns until a slot frees.
	 *
	 * Why it belongs here and not only in the pump: whether a pump runs
	 * between two edits is not something this path controls, and a broad
	 * burst can fill the log faster than any per-pump rate drains it. With
	 * the compaction reachable from the exhaustion itself, "the log is full"
	 * can only mean "and no backed column has a parked edit left to fold into
	 * its own body". REL-MP-JOINSPEED: at 16,384 entries with the pump's
	 * latch at 12,288 this is a last resort -- one pump drains at most
	 * REMOTE_PUMP_EVENT_BUDGET events, far fewer edits than the 4,096-entry
	 * margin -- not the per-edit path the 1,024 log had made it. */
	for(int pass = 0; pass < 3 && !ref; pass++) {
		if(pass == 1) {
			/* Exhausted: reclaim what a newer queued body already
			 * supersedes, then look again. Never overwrite the oldest --
			 * that was terrain lost for the session (MP audit D3). */
			for(size_t k = 0; k < REMOTE_CHUNK_ENTRY_CAPACITY; k++) {
				struct remote_chunk* c = &remote->chunks[k];
				if(!c->edit_count
				   || (c->state != REMOTE_CHUNK_RETAINED
					   && c->state != REMOTE_CHUNK_PROMOTING))
					continue;
				if(c->wire.compressed)
					remote_deferred_edit_supersede_locked(remote, c, &c->wire,
														  c->arrival);
				for(uint8_t q = 0; q < c->queued_count; q++)
					remote_deferred_edit_supersede_locked(
						remote, c, &c->queued[q], c->queued_arrival[q]);
			}
		} else if(pass == 2) {
			/* Still full. Fold parked edits INTO the bodies that will carry
			 * them: lossless (the edits end up inside the compressed head, and
			 * test (j) proves that byte for byte), and it frees that column's
			 * whole share of the log at once. Bounded by the log: each
			 * successful compaction strictly reduces deferred_edits, so this
			 * terminates, and the first success is enough because one freed
			 * slot is all this caller needs. */
			if(!remote_compact_one_locked(remote, true, false))
				break; /* nothing left to fold: the kill below is the truth */
			remote->deferred_log_rescues++;
		}
		ref = remote_edit_alloc_locked(remote);
		if(!ref && !remote->deferred_edit_log) {
			/* The pool itself could not be claimed: a capacity answer from
			 * the MEM2 side budget, ended visibly (mp_end 5) like any other
			 * reservation refusal -- never a silently dropped edit. */
			remote_backing_capacity_fail_locked(
				remote, (size_t)REMOTE_DEFERRED_EDIT_CAPACITY
							* sizeof(struct remote_deferred_edit));
			return;
		}
	}
	if(!ref && remote->stop_now)
		return; /* a compaction ended the session (REL-MP): keep its reason */
	if(!ref) {
		remote->deferred_edit_log_exhausted++;
		kill_connection_locked(remote, "remote deferred edit log exhausted");
		return;
	}
	struct remote_deferred_edit* slot = remote_edit_at(remote, ref);
	slot->owner = (uint16_t)(index + 1u);
	slot->cell = cell;
	slot->type = type;
	slot->metadata = metadata;
	slot->arrival = arrival;
	remote_edit_link_locked(remote, chunk, ref);
	remote->deferred_edits++;
	if(remote->deferred_edits > remote->deferred_edits_peak)
		remote->deferred_edits_peak = remote->deferred_edits;
}

/* Drains every parked edit for one chunk column, applying each through the
 * same live CRPC_SET_BLOCK path remote_apply_block_change uses once a chunk
 * is already resident -- reusing its relight/dig-particle behaviour instead
 * of re-deriving it, and with no need to know which sub-volume a partial
 * Packet51 covered (world_set_block works in absolute coordinates, unlike
 * the promotion slot's own sub-volume-relative byte planes). Called from
 * remote_finish_promotion_locked immediately after that chunk's CRPC_CHUNK
 * send: both calls run synchronously on this same (main/pump) thread with no
 * yield between them, so nothing -- renderer, mesher, any other system --
 * ever observes the pre-edit base volume, which is what "applied just before
 * the base volume is exposed" (this file's former header comment on
 * remote_apply_block_change) actually requires. Applied oldest-arrival
 * first -- the chain order (REL-MP-JOINSPEED); the log holds at most one
 * entry per coordinate. */
static void remote_deferred_edit_drain_chunk_locked(
	struct server_remote_impl* remote, struct remote_chunk* chunk) {
	while(chunk->edit_head) {
		struct remote_deferred_edit* entry
			= remote_edit_at(remote, chunk->edit_head);
		int32_t x = chunk->chunk_x * 16 + (int32_t)((entry->cell >> 11) & 15u);
		int32_t z = chunk->chunk_z * 16 + (int32_t)((entry->cell >> 7) & 15u);
		uint8_t y = (uint8_t)(entry->cell & 127u);
		/* ORDER (2026-09-10, MP audit D2; Astra mp1 F2). The head body was
		 * just installed. An edit OLDER than a body covering its cell (the
		 * head, or one still queued) is superseded by that snapshot; an edit
		 * NEWER than a still-queued body covering its cell must wait for it
		 * -- and so must every later edit of this column (head-of-line:
		 * CRPC_SET_BLOCK is not just a cell write, it relights and touches
		 * neighbours, so application order stays wire order). */
		bool shadowed = entry->arrival < chunk->arrival
			&& remote_volume_contains(&chunk->wire, x, y, z);
		bool blocked = false;
		for(uint8_t q = 0; q < chunk->queued_count; q++) {
			if(!remote_volume_contains(&chunk->queued[q], x, y, z))
				continue;
			if(entry->arrival < chunk->queued_arrival[q])
				shadowed = true;
			else
				blocked = true;
		}
		if(shadowed) {
			remote_edit_drop_locked(remote, chunk, &chunk->edit_head, 0);
			remote->deferred_edits_superseded++;
			continue;
		}
		if(blocked)
			return;
		struct client_rpc call = {
			.type = CRPC_SET_BLOCK,
			.payload.set_block = {
				.x = x, .y = (w_coord_t)y, .z = z,
				.block = {.type = entry->type,
						  .metadata = (uint8_t)(entry->metadata & 0x0F)},
			},
		};
		uint64_t arrival = entry->arrival;
		remote_edit_drop_locked(remote, chunk, &chunk->edit_head, 0);
		remote->deferred_edits_applied++;
		/* W1 (design section 2.5): this edit is now part of the column's
		 * installed voxels, so a later demotion's re-encode contains it --
		 * see struct remote_chunk::newest_applied. */
		if(arrival > chunk->newest_applied)
			chunk->newest_applied = arrival;
		clin_process_direct_remote(&call);
	}
}

/* Packet52/53 land on whatever chunk the server thinks is loaded, but
 * protocol 14's own promotion pipeline may not have published that chunk
 * client-side yet. Plan section 7's bounded per-coordinate deferred-edit log
 * (struct remote_deferred_edit, server_remote_internal.h) covers that gap:
 * an edit against a not-yet-resident chunk is parked there instead of being
 * discarded, and remote_finish_promotion_locked below drains and applies
 * every parked edit for a chunk the moment that chunk's own promotion
 * completes -- see remote_deferred_edit_drain_chunk_locked's own comment for
 * exactly where/why. */
void remote_apply_block_change(struct server_remote_impl* remote,
									  int32_t x, uint8_t y, int32_t z,
									  uint8_t type, uint8_t metadata) {
	thread_mutex_lock(&remote->lock);
	bool resident = remote_chunk_resident_locked(remote, x, z);
	if(!resident)
		remote_deferred_edit_record_locked(remote, x, y, z, type, metadata);
	else {
		/* W1 (design section 2.5): the LIVE path applies this edit straight
		 * into the installed voxels, so a later demotion's re-encode
		 * contains it and must be stamped no older than this edit. Same
		 * stamp expression remote_deferred_edit_record_locked uses, so both
		 * paths share the one comparison space (bodies (seq<<16), edits
		 * (seq<<16)|sub) -- and the sub counter is consumed either way, so
		 * an edit's position within its packet never depends on whether its
		 * column happened to be resident. */
		uint64_t arrival = (remote->pump_packet_seq << 16)
			| (uint64_t)(remote->pump_packet_sub++ & 0xFFFFu);
		int32_t chunk_x = (int32_t)floor((double)x / 16.0);
		int32_t chunk_z = (int32_t)floor((double)z / 16.0);
		size_t index = remote_chunk_find_locked(remote, chunk_x, chunk_z);
		if(index != REMOTE_CHUNK_ENTRY_CAPACITY
		   && arrival > remote->chunks[index].newest_applied)
			remote->chunks[index].newest_applied = arrival;
	}
	thread_mutex_unlock(&remote->lock);
	if(!resident)
		return;
	struct client_rpc call = {
		.type = CRPC_SET_BLOCK,
		.payload.set_block = {
			.x = x, .y = (w_coord_t)y, .z = z,
			.block = {.type = type, .metadata = (uint8_t)(metadata & 0x0F)},
		},
	};
	clin_process_direct_remote(&call);
}

static void remote_compute_heightmap(struct chunk_rpc_slot* slot) {
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			uint8_t height = 0;
			for(int y = 127; y >= 0; y--) {
				uint8_t id = slot->ids[((size_t)x * 16u + (size_t)z) * 128u
					+ (size_t)y];
				if(blocks[id]
				   && (!blocks[id]->can_see_through || blocks[id]->opacity > 0)) {
					height = (uint8_t)(y + 1);
					break;
				}
			}
			slot->heightmap[x + z * 16] = height;
		}
}

static bool remote_start_promotion_locked(struct server_remote_impl* remote) {
	if(remote->promotion.active)
		return true;
	size_t best = REMOTE_CHUNK_ENTRY_CAPACITY;
	uint64_t best_distance = UINT64_MAX, best_arrival = UINT64_MAX;
	/* IN-009 FOLD-WAIT PRIORITY. The read thread is holding a Packet51 for
	 * this column until its chain has room, and the column's head is a
	 * partial on voxels the client ALREADY holds -- the only base that
	 * partial can be folded onto. Installing it costs no new voxel memory
	 * (the 81,920 B are already resident and counted in resident_cols), so
	 * it goes first and bypasses the radius, the hard cap and the carve gate:
	 * none of them is about columns that are already installed. Without
	 * this, a column in the R+1 hysteresis band (not promoted, not demoted)
	 * would hold the read thread until REMOTE_FOLD_WAIT_MS. A FULL head is
	 * never given this bypass (its install allocates a replacement column). */
	bool fold_priority = false;
	if(remote->fold.wait_active) {
		size_t wait_index = remote_chunk_find_locked(remote, remote->fold.wait_x,
													 remote->fold.wait_z);
		if(wait_index != REMOTE_CHUNK_ENTRY_CAPACITY
		   && remote->chunks[wait_index].state == REMOTE_CHUNK_RETAINED
		   && remote->chunks[wait_index].installed
		   /* Partial heads only: a partial reuses the installed voxels, but
		    * a FULL head installs through a replacement that allocates all
		    * new chunks before releasing the old column (world.c), i.e.
		    * another 81,920 B -- that one keeps the cap and carve gate (its
		    * fold wait is edit order or scratch, not promotion). */
		   && !remote_body_is_full_column(&remote->chunks[wait_index].wire)
		   && !remote->chunks[wait_index].unload_pending
		   && remote->chunks[wait_index].epoch == remote->applied_epoch) {
			best = wait_index;
			fold_priority = true;
		}
	}
	/* REL-MP-LOADHANG: the nearest RE-SEND of an installed column, kept
	 * apart from the nearest-first pick so a cold column the cap or the carve
	 * refuses cannot stand in front of it (the pick used to return on that
	 * refusal). Eligible anywhere in the (R+1) hysteresis disk, i.e. wherever
	 * remote_residency_step_locked will NOT demote the column -- there it
	 * stays installed and drawn, so its new body (and the block updates
	 * parked behind it) must apply; past R+1 the column is demoted with its
	 * chain preserved, exactly as before. */
	size_t best_installed = REMOTE_CHUNK_ENTRY_CAPACITY;
	uint64_t installed_distance = UINT64_MAX,
			 installed_arrival = UINT64_MAX;
	for(size_t i = 0; !fold_priority && i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		struct remote_chunk* chunk = &remote->chunks[i];
		if(chunk->state != REMOTE_CHUNK_RETAINED)
			continue;
		/* A body of a dimension whose Respawn the consumer has not applied
		 * yet waits: installing it now would put it into the OLD world and
		 * CRPC_WORLD_RESET would throw it away (protocol 14 never re-sends). */
		if(chunk->epoch != remote->applied_epoch)
			continue;
		uint64_t distance = remote_chunk_distance_locked(remote, chunk);
		if(chunk->installed && !chunk->unload_pending
		   && (!remote->have_admit_centre
			   || distance <= (uint64_t)remote->admit_radius + 1u)
		   && (best_installed == REMOTE_CHUNK_ENTRY_CAPACITY
			   || distance < installed_distance
			   || (distance == installed_distance
				   && chunk->arrival < installed_arrival))) {
			best_installed = i;
			installed_distance = distance;
			installed_arrival = chunk->arrival;
		}
		/* ADMISSION GATE (W1, design section 2.1/2.3). THE single cap on
		 * installs: a column farther than admit_radius stays BACKED -- it
		 * keeps its complete compressed body (and its chain, and its parked
		 * edits), so nothing is lost and it promotes through this very pick
		 * the moment the player brings it inside the radius (promote-on-entry
		 * needs no other code). Nothing else about the pick changes: rate,
		 * budget, epoch test and nearest-first/oldest-arrival order are
		 * untouched.
		 *
		 * Gated on "a centre is known" on purpose. Without one
		 * remote_chunk_distance_locked measures from column (0,0), which for
		 * a server streaming around a distant spawn would refuse EVERY
		 * column -- and with a bounded compressed cap that turns a memory
		 * problem into an eviction/reconnect loop.
		 *
		 * F5b (review finding H5): that window used to run from login to the
		 * first MOVING Packet13, during which every announced column
		 * installed -- 8 MiB worse than pre-slice, because the backing
		 * reservation had already taken its cut of the carve. The centre is
		 * now seeded by the LOGIN spawn position (Packet6) and refreshed by
		 * every position/teleport/respawn, and Beta sends both before the
		 * chunk burst [740c583 ServerConfigurationManager.login ->
		 * NetServerHandler.teleport], so the gate is live from the first
		 * column for any server that follows the protocol. For one that does
		 * not, the HARD CAP below is the backstop -- it does not depend on a
		 * centre at all. */
		if(remote->have_admit_centre
		   && distance > (uint64_t)remote->admit_radius)
			continue;
		if(best == REMOTE_CHUNK_ENTRY_CAPACITY || distance < best_distance
		   || (distance == best_distance && chunk->arrival < best_arrival)) {
			best = i;
			best_distance = distance;
			best_arrival = chunk->arrival;
		}
	}
	if(best == REMOTE_CHUNK_ENTRY_CAPACITY
	   && best_installed == REMOTE_CHUNK_ENTRY_CAPACITY)
		return true;
	/* THE HARD RESIDENT CAP (F5b, review finding H1). Placed BEFORE the
	 * carve gate and independent of it, because H1's 242-column peak
	 * happened with the carve gate in place: the radius test bounds where a
	 * column may be, but nothing counted how many were already resident, so
	 * a centre that MOVED (a same-dimension respawn, which clears no
	 * entries) let a second full disk install on top of the first.
	 *
	 * Counted, not scanned: remote_set_installed_locked maintains
	 * resident_cols at every flip of the bit that actually means "this
	 * column holds 81,920 B of CARVE_VOXEL".
	 *
	 * At the cap the promotion WAITS for a demotion -- it is not a drop. The
	 * cap is the (R+1) hysteresis disk, so whenever the cap blocks an
	 * in-radius promotion there is by construction a column at distance
	 * R+1 or beyond for remote_residency_step_locked to demote (the centre
	 * moved; that is the only way to reach the cap), and its bounded
	 * ordering veto guarantees it actually fires.
	 *
	 * An admit_radius of 0 means no radius has been applied to this
	 * connection yet (server_remote_start defaults it to
	 * SERVER_REMOTE_ADMIT_RADIUS_MAX, so a live session always has one; a
	 * hand-built impl in a test may not). The cap then takes the platform
	 * MAXIMUM, because a session that has not been told a radius must not be
	 * capped tighter than the deepest radius it could still be told -- and 0
	 * would otherwise mean a 9-column ceiling, which is not a backstop but a
	 * behaviour change. The carve gate below still binds in that window. */
	int cap_radius = remote->admit_radius > 0 ? remote->admit_radius
											  : SERVER_REMOTE_ADMIT_RADIUS_MAX;
	bool cold_ok = fold_priority;
	if(!fold_priority && best != REMOTE_CHUNK_ENTRY_CAPACITY
	   && !remote->chunks[best].installed) {
		if(remote->resident_cols >= REMOTE_RESIDENT_COLUMN_CAP(cap_radius))
			remote->promote_deferred_cap++;
		else
			cold_ok = true;
	}
	/* MEMORY GATE (F4, design 2.3/2.4). The radius above bounds how MANY
	 * columns may be resident; this bounds whether the carve can afford the
	 * NEXT one and still mesh it. Placed here on purpose: after the pick (so
	 * a deferral names a column that was genuinely eligible, which is what
	 * makes the counter actionable) and before the chunk_rpc_slot acquire and
	 * the inflate-scratch malloc (so a refusal costs nothing and leaks
	 * nothing). Not a drop: the entry stays RETAINED with its whole
	 * compressed body, and the next pump picks it again -- by then a
	 * demotion or a 0x32 unload may have returned the 81,920 B this needs. */
	if(cold_ok && !fold_priority && !remote_carve_can_hold_column()) {
		remote->promote_deferred_memory++;
		cold_ok = false;
	}
	/* REL-MP-LOADHANG: a re-send of an INSTALLED column never meets the two
	 * gates above -- it adds no resident column, and its carve cost is
	 * remote_carve_can_replace_installed's. The nearest-first pick still
	 * wins whenever it can start; only when it cannot (no in-radius
	 * candidate, or a cold column the gates refused) does the nearest
	 * re-send go instead -- before this fix the pick simply returned, so a
	 * refused cold column also blocked every re-send behind it. */
	if(!fold_priority) {
		bool start = cold_ok;
		if(best != REMOTE_CHUNK_ENTRY_CAPACITY && remote->chunks[best].installed) {
			start = remote_carve_can_replace_installed(&remote->chunks[best]);
			if(!start)
				remote->promote_deferred_memory++;
		}
		if(!start && best_installed != REMOTE_CHUNK_ENTRY_CAPACITY
		   && best_installed != best
		   && remote_carve_can_replace_installed(
				  &remote->chunks[best_installed])) {
			best = best_installed;
			start = true;
		}
		if(!start)
			return true;
	}
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	if(!slot)
		return true;
	struct remote_chunk* chunk = &remote->chunks[best];
	/* F5a (review finding L10): the inflate scratch comes out of the backing
	 * reservation's scratch side budget, not MEM1 malloc -- 81,920 B per
	 * promotion on the heap the betamc_0913 run showed collapsing, outside
	 * every budget. The slot is claimed at the CAP rather than at this
	 * column's length so its offset and size are the same for every
	 * promotion of the session (see mem2.h: a slot's size is fixed by its
	 * first claim); a partial body simply uses less of it, exactly as the
	 * malloc version's smaller buffer did. */
	_Static_assert(MP_CHUNK_INFLATED_CAP <= MEM2_MP_SCRATCH_BYTES,
				   "the inflate scratch slot must fit the scratch side budget");
	uint8_t* inflated = NULL;
	if(chunk->wire.inflated_length <= MP_CHUNK_INFLATED_CAP)
		inflated = mp_backing_scratch_acquire(MP_BACKING_SCRATCH_INFLATE,
											  MP_CHUNK_INFLATED_CAP);
	if(!inflated) {
		remote->retention_allocation_failures++;
		clin_chunk_slot_release(slot);
		return true;
	}
	struct remote_promotion* promotion = &remote->promotion;
	memset(promotion, 0, sizeof(*promotion));
	promotion->active = true;
	promotion->entry = best;
	promotion->inflated = inflated;
	promotion->slot = slot;
	promotion->stream.zalloc = Z_NULL;
	promotion->stream.zfree = Z_NULL;
	promotion->stream.opaque = Z_NULL;
	promotion->stream.next_in = chunk->wire.compressed;
	promotion->stream.avail_in = (uInt)chunk->wire.compressed_length;
	promotion->stream.next_out = inflated;
	if(inflateInit(&promotion->stream) != Z_OK) {
		remote->retention_allocation_failures++;
		if(++remote->promotion_inflate_init_failures
		   >= REMOTE_INFLATE_INIT_FAILURE_LIMIT) {
			remote_latch_protocol_end_locked(remote);
			set_reason_locked(remote,
				"remote Packet51 inflateInit repeated failure");
			remote->stop_now = true;
			close_socket_locked(remote);
			thread_cond_broadcast(&remote->write_signal);
			remote_promotion_cancel_locked(remote, false);
			return false;
		}
		remote_promotion_cancel_locked(remote, true);
		return true;
	}
	remote->promotion_inflate_init_failures = 0;
	promotion->stream_initialized = true;
	chunk->state = REMOTE_CHUNK_PROMOTING;
	return true;
}

/* hw1030g -- THE WATCHED CELL'S PACKET51 SHAPE. A Beta 1.7.3 server answers a
 * block change with Packet53 (one cell), Packet52 (2..10 cells) or, when more
 * than 10 cells of one column changed in a tick, a Packet51 REGION resend
 * (PlayerInstance.sendBlockChanges; mc173's ChunkTracker uses the same
 * threshold). Only the first two reached remote_record_block_echo, so a dig
 * answered by a region left the play driver waiting on an echo that had
 * already been applied to the world. When -- and only when -- a script has
 * armed a watch and the body being installed covers that cell, the body's own
 * voxel there is recorded as one more echo for the cell (ring + slot +
 * history, one serial). `body` is the decoded wire layout
 * (((x-x0)*sz + (z-z0))*sy + (y-y0) ids, then the metadata nibbles, low nibble
 * first -- Chunk.getChunkData). Cost with no watch armed: one branch. */
static void remote_record_watch_body_locked(struct server_remote_impl* remote,
											const struct mp_map_chunk* wire,
											const uint8_t* body) {
	if(!remote->watch_on || !body || remote->watch_y < 0
	   || remote->watch_y > 127
	   || !remote_volume_contains(wire, remote->watch_x,
								  (uint8_t)remote->watch_y, remote->watch_z))
		return;
	size_t cells = (size_t)wire->sx * wire->sy * wire->sz;
	size_t idx = ((size_t)(remote->watch_x - wire->x) * wire->sz
				  + (size_t)(remote->watch_z - wire->z))
			* wire->sy
		+ (size_t)(remote->watch_y - wire->y);
	if(idx >= cells || cells + cells / 2u > wire->inflated_length)
		return;
	uint8_t nib = body[cells + idx / 2u];
	uint8_t metadata = (uint8_t)((idx & 1u) ? (nib >> 4) : (nib & 0x0F));
	if(remote->watch_bodies != UINT32_MAX)
		remote->watch_bodies++;
	remote_record_block_echo(remote, remote->watch_x, (uint8_t)remote->watch_y,
							 remote->watch_z, body[idx], metadata);
}

static bool remote_finish_promotion_locked(struct server_remote_impl* remote) {
	struct remote_promotion* promotion = &remote->promotion;
	struct remote_chunk* chunk = &remote->chunks[promotion->entry];
	/* PUBLICATION GATE (Astra mp1 F2): an edit received BEFORE this body may
	 * still be sitting in the event queue (the pump drains a bounded budget
	 * per frame, then promotes). Installing now would make the column
	 * RESIDENT, and the next pump would apply that older edit LIVE on top of
	 * the newer snapshot. Hold the finished decode until the queue head is
	 * newer than this body; ready_to_publish stays set, so the next pump
	 * retries here without decoding again. The in-flight body is never
	 * discarded for a later one: it installs, then the chain continues. */
	if(remote->event_count
	   && remote->events[remote->event_head].seq < (chunk->arrival >> 16)) {
		remote->promotion_publish_waits++;
		return true;
	}
	struct mp_map_chunk* wire = &chunk->wire;
	size_t cells = (size_t)wire->sx * wire->sy * wire->sz;
	size_t nibble = cells / 2;
	for(size_t i = 0; i < cells; i++) {
		uint8_t id = promotion->inflated[i];
		if(id != 0 && !blocks[id]) {
			remote_latch_protocol_end_locked(remote);
			set_reason_locked(remote, "remote Packet51 invalid block id");
			remote->stop_now = true;
			close_socket_locked(remote);
			thread_cond_broadcast(&remote->write_signal);
			remote_promotion_cancel_locked(remote, false);
			return false;
		}
	}
	memset(promotion->slot->ids, 0, sizeof(promotion->slot->ids));
	memset(promotion->slot->metadata, 0, sizeof(promotion->slot->metadata));
	memset(promotion->slot->lighting_sky, 0,
		   sizeof(promotion->slot->lighting_sky));
	memset(promotion->slot->lighting_torch, 0,
		   sizeof(promotion->slot->lighting_torch));
	memcpy(promotion->slot->ids, promotion->inflated, cells);
	memcpy(promotion->slot->metadata, promotion->inflated + cells, nibble);
	/* Wire order is block-light then sky-light, opposite the slot declaration.
	 * [740c583 Chunk.getChunkData] */
	memcpy(promotion->slot->lighting_torch,
		   promotion->inflated + cells + nibble, nibble);
	memcpy(promotion->slot->lighting_sky,
		   promotion->inflated + cells + 2 * nibble, nibble);
	piston_moving_chunk_snapshot_destroy(&promotion->slot->piston_moving);
	promotion->slot->piston_moving
		= (struct piston_moving_chunk_snapshot) {0};
	bool full = wire->x % 16 == 0 && wire->z % 16 == 0 && wire->y == 0
		&& wire->sx == 16 && wire->sy == 128 && wire->sz == 16;
	/* REL-MP-FIX2b: a partial on a column without voxels lands on Beta's
	 * prepared chunk [ChunkProviderClient.java:39-41, NetClientHandler.java
	 * :294] and installs whole: untouched cells are air with sky light 15,
	 * not absent sections (world_get_block's stone not-loaded read). */
	bool expand = !full && !chunk->installed;
	if(expand) {
		struct chunk_rpc_slot* s = promotion->slot;
		uint8_t* const planes[3]
			= {s->metadata, s->lighting_torch, s->lighting_sky};
		memset(s->ids, 0, sizeof(s->ids));
		memset(s->metadata, 0, sizeof(s->metadata));
		memset(s->lighting_torch, 0, sizeof(s->lighting_torch));
		memset(s->lighting_sky, 0xFF, sizeof(s->lighting_sky));
		if(!remote_column_apply_region(s->ids, planes, wire, chunk->chunk_x,
									   chunk->chunk_z, promotion->inflated)) {
			remote_latch_protocol_end_locked(remote);
			kill_connection_locked(remote, "remote Packet51 region invalid");
			remote_promotion_cancel_locked(remote, false);
			return false;
		}
	}
	if(full || expand) {
		remote_compute_heightmap(promotion->slot);
		if(remote->climate)
			worldgen_climate(remote->climate, chunk->chunk_x, chunk->chunk_z,
				promotion->slot->grass_i, promotion->slot->grass_j);
		else {
			memset(promotion->slot->grass_i, 0,
				   sizeof(promotion->slot->grass_i));
			memset(promotion->slot->grass_j, 0,
				   sizeof(promotion->slot->grass_j));
		}
	}
	struct client_rpc call = {
		.type = CRPC_CHUNK,
		.payload.chunk = {
			.x = expand ? chunk->chunk_x * 16 : wire->x,
			.y = expand ? 0 : wire->y,
			.z = expand ? chunk->chunk_z * 16 : wire->z,
			.sx = expand ? 16 : wire->sx, .sy = expand ? 128 : wire->sy,
			.sz = expand ? 16 : wire->sz,
			.slot = promotion->slot,
		},
	};
	// S5.3 hunt fix (round 4): this is the CRPC_CHUNK send round 4's static
	// pass named as a THIRD instance of the wrong-thread-producer pattern
	// (remote_finish_promotion_locked runs on the main thread via
	// remote_promotion_step_locked <- server_remote_pump). clin_process_direct
	// never returns false (there is no pool to exhaust), so the old
	// "retry next pump" deferral branch is now unreachable and removed --
	// see clin_process_direct's declaration comment for the full pattern.
	clin_process_direct_remote(&call);
	/* hw1030g: a body covering the watched cell IS the server's answer for
	 * that cell -- record it before the parked edits below replay on top. */
	remote_record_watch_body_locked(remote, wire, promotion->inflated);
	/* W1 (design section 2.5): this body is now the column's installed
	 * voxels. Recorded BEFORE remote_chunk_shift_queued_locked below, which
	 * overwrites chunk->arrival with the NEXT body's own stamp. The drain
	 * that follows raises it further for every edit it applies. */
	if(chunk->arrival > chunk->newest_applied)
		chunk->newest_applied = chunk->arrival;
	/* W2: the client now HOLDS this column's voxels, whatever the entry's
	 * state becomes below -- a queued body moves it back to RETAINED while
	 * the blocks stay installed, and that is exactly the case the
	 * RESIDENT-only demotion precondition used to miss. See struct
	 * remote_chunk::installed. F5b: through the one setter, so
	 * remote->resident_cols (the hard cap's counter) cannot drift. */
	remote_set_installed_locked(remote, chunk, true);
	/* REL-MP-FIX2b: genuine-full tracking (a partial on voxels keeps it). */
	if(full || expand)
		chunk->voxels_synthetic = expand || chunk->synthetic_base;
	/* Plan section 7: replay every edit parked against this chunk while it
	 * was mid-promotion (remote_apply_block_change's deferred path) now that
	 * its base volume is installed -- see remote_deferred_edit_drain_chunk_
	 * locked's header comment for why doing this here, immediately after the
	 * CRPC_CHUNK send with no yield in between, satisfies "applied just
	 * before the base volume is exposed" without hand-patching the
	 * promotion slot's own sub-volume-relative byte planes. */
	remote_deferred_edit_drain_chunk_locked(remote, chunk);
	if(full && !chunk->voxels_synthetic && remote->have_authoritative_position
	   && chunk->chunk_x == (int32_t)floor(remote->authoritative_x / 16.0)
	   && chunk->chunk_z == (int32_t)floor(remote->authoritative_z / 16.0))
		remote->load_completion_pending = true;
	promotion->slot = NULL; /* Ownership moved to CRPC_CHUNK. */
	if(promotion->stream_initialized)
		inflateEnd(&promotion->stream);
	promotion->stream_initialized = false;
	mp_backing_scratch_release(MP_BACKING_SCRATCH_INFLATE);
	promotion->inflated = NULL;
	/* Chain: the next body of this column (a later partial, or a later
	 * covering resend) is decoded and installed on top, in receive order. */
	chunk->state = remote_chunk_shift_queued_locked(remote, chunk)
		? REMOTE_CHUNK_RETAINED
		: REMOTE_CHUNK_RESIDENT;
	memset(promotion, 0, sizeof(*promotion));
	return true;
}

void remote_emit_load_completion_locked(struct server_remote_impl* remote) {
	if(!remote->load_completion_pending || remote->remote_load_complete_sent
	   || !remote->have_authoritative_position)
		return;
	if(++remote->correction_seq == 0)
		remote->correction_seq = 1;
	uint8_t flags = PLAYER_CORRECTION_POSITION_ONLY
		| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
		| PLAYER_CORRECTION_ZERO_VEL_Z | PLAYER_CORRECTION_SET_ON_GROUND
		| PLAYER_CORRECTION_FINISH_REMOTE_LOAD;
	if(remote->authoritative_on_ground)
		flags |= PLAYER_CORRECTION_ON_GROUND;
	struct client_rpc correction = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {(float)remote->authoritative_x,
				(float)remote->authoritative_y,
				(float)remote->authoritative_z},
			.rotation = {remote->authoritative_yaw,
				remote->authoritative_pitch},
			.correction_seq = remote->correction_seq,
			.correction_flags = flags
				| (remote->have_authoritative_look
					? PLAYER_CORRECTION_SET_ROTATION : 0),
		},
	};
	// S5.3 hunt fix (round 4): same wrong-thread-producer pattern, same fix
	// -- called from server_remote_pump() (server_remote.c:169), main
	// thread. clin_process_direct never returns false.
	clin_process_direct_remote(&correction);
	remote->load_completion_pending = false;
	remote->remote_load_complete_sent = true;
}

bool remote_promotion_step_locked(struct server_remote_impl* remote) {
	/* W2: ONE deferred-log compaction per pump, BEFORE promotion (design
	 * section 2.5). Before, because a compaction is the cheap way to relieve
	 * both pressures this pump can face -- log slots and backing bytes -- and
	 * doing it first means the promotion that follows runs against the
	 * relieved state. It is a no-op below the high-water mark, so the
	 * ordinary pump pays one uint32_t comparison for it. Deliberately not
	 * bundled into remote_residency_step_locked: that step is demotion, with
	 * its own one-per-pump bound and its own vetoes, and a compaction must
	 * still happen on a pump where no column is eligible for demotion.
	 *
	 * F5a (review finding M8): up to REMOTE_COMPACTIONS_PER_PUMP of them, not
	 * one. With edits spread over hundreds of columns each compaction frees
	 * only that column's parked entries, so one per pump could not keep up
	 * with a broad burst and the log still reached its cap. The loop stops
	 * once the latched pressure releases at LOW_WATER (REL-MP-FIX3a-r; the
	 * `pump` gate inside) or on the first refusal, so an unpressured pump
	 * pays two compares. */
	for(unsigned k = 0; k < REMOTE_COMPACTIONS_PER_PUMP; k++)
		if(!remote_compact_one_locked(remote, false, true))
			break;
	if(!remote_start_promotion_locked(remote) || !remote->promotion.active)
		return true;
	struct remote_promotion* promotion = &remote->promotion;
	struct remote_chunk* chunk = &remote->chunks[promotion->entry];
	if(promotion->ready_to_publish)
		return remote_finish_promotion_locked(remote);
	if(promotion->stream.total_out > chunk->wire.inflated_length) {
		remote_latch_protocol_end_locked(remote);
		set_reason_locked(remote, "remote Packet51 inflated length overflow");
		remote->stop_now = true;
		close_socket_locked(remote);
		thread_cond_broadcast(&remote->write_signal);
		remote_promotion_cancel_locked(remote, false);
		return false;
	}
	size_t remaining = chunk->wire.inflated_length
		- (size_t)promotion->stream.total_out;
	size_t budget = remaining < REMOTE_PROMOTION_OUTPUT_BUDGET
		? remaining : REMOTE_PROMOTION_OUTPUT_BUDGET;
	if(budget == 0) {
		/* Consume the zlib trailer without granting room for another decoded
		 * byte. Any write to this one-byte sentinel proves an oversized stream. */
		promotion->stream.next_out = &promotion->overflow_byte;
		promotion->stream.avail_out = 1;
	} else {
		promotion->stream.next_out
			= promotion->inflated + promotion->stream.total_out;
		promotion->stream.avail_out = (uInt)budget;
	}
	uLong before = promotion->stream.total_out;
	int result = inflate(&promotion->stream, Z_NO_FLUSH);
	remote->inflated_bytes_frame
		+= (size_t)(promotion->stream.total_out - before);
	if(result != Z_OK && result != Z_STREAM_END) {
		remote_latch_protocol_end_locked(remote);
		set_reason_locked(remote, "remote Packet51 zlib failure");
		remote->stop_now = true;
		close_socket_locked(remote);
		thread_cond_broadcast(&remote->write_signal);
		remote_promotion_cancel_locked(remote, false);
		return false;
	}
	if(result == Z_STREAM_END) {
		if(promotion->stream.total_out != chunk->wire.inflated_length
		   || promotion->stream.avail_in != 0) {
			remote_latch_protocol_end_locked(remote);
			set_reason_locked(remote, "remote Packet51 inflated length mismatch");
			remote->stop_now = true;
			close_socket_locked(remote);
			thread_cond_broadcast(&remote->write_signal);
			remote_promotion_cancel_locked(remote, false);
			return false;
		}
		promotion->ready_to_publish = true;
		return remote_finish_promotion_locked(remote);
	}
	if(promotion->stream.avail_out != 0 && promotion->stream.avail_in == 0) {
		remote_latch_protocol_end_locked(remote);
		kill_connection_locked(remote, "remote Packet51 truncated zlib stream");
		remote_promotion_cancel_locked(remote, false);
		return false;
	}
	return true;
}

/* ======================================================================
 * W1: DEMOTE-ON-EXIT (lossless re-encode back to BACKED)
 * ======================================================================
 * Astra gate-2 item 5 rejects destroying the only authoritative copy of a
 * column, and protocol 14 has no chunk re-request -- so a column leaving the
 * admission radius may only give up its 81,920 B of voxel storage if its
 * COMPLETE current state survives. That is what this block does: it reads the
 * installed voxels back, deflates them into one Packet51-shaped body stamped
 * with everything already applied (design section 2.5), installs that body as
 * the column's new head, and only THEN tells the client to unload. Every step
 * is reversible until the head is installed, and the voxels are never freed
 * before it is.
 */

/* The fixed re-encode scratch (design section 2.4 -- "no per-demotion
 * malloc"). One allocation per connection, lazily on first need; released by
 * remote_chunks_clear_locked. NULL means "cannot demote this pump", counted
 * by the caller as a demotion failure and retried.
 *
 * F5a (review finding L10): this comes out of the backing reservation's
 * SCRATCH side budget (mem2.h), not MEM1 `malloc`. 163,904 B per connection
 * sat on exactly the heap the betamc_0913 run showed collapsing, and no
 * telemetry field reported it. The slot's fixed-offset discipline also fixes
 * a latent hazard the malloc version had: the reclaim ladder calls the
 * compaction that needs this buffer precisely when memory is tight, so an
 * allocation that can fail under pressure is an allocation that fails when it
 * is needed most. */
static uint8_t* remote_demote_scratch_locked(struct server_remote_impl* remote) {
	if(!remote->demote_scratch) {
		/* zlib's own bound, checked against the compile-time staging size
		 * rather than assumed (the formula is version-dependent). */
		if(compressBound((uLong)REMOTE_COLUMN_RAW_BYTES)
		   > (uLong)REMOTE_DEMOTE_STAGE_BYTES)
			return NULL;
		remote->demote_scratch = mp_backing_scratch_acquire(
			MP_BACKING_SCRATCH_RECODE, REMOTE_DEMOTE_SCRATCH_BYTES);
	}
	return remote->demote_scratch;
}

/* The INVERSE of chunk_load_packed_slice, for one full column: gather the
 * client's installed voxels of (chunk_x, chunk_z) into the four Packet51
 * planes -- ids[(x*16 + z)*128 + y], and the three 4-bit planes at that index
 * >> 1 with the low nibble for even y. That is exactly the indexing
 * remote_finish_promotion_locked's memcpy produces and remote_compute_
 * heightmap reads back, and exactly what chunk_load_packed_slice consumes.
 *
 * chunk.c carries no packed-slice READER (chunk_load_packed_slice is
 * write-only) and chunk.c is not this slice's file to change, so the gather
 * goes through the public per-cell accessor. The eight shells are resolved
 * ONCE each, so the cost is 8 section lookups + 32768 packed reads -- not
 * 32768 dict lookups, which is what a world_get_block loop would have cost.
 * Returns false when any shell is missing (the column is not actually
 * installed); the caller then leaves the entry RESIDENT. */
static bool remote_column_gather(int32_t chunk_x, int32_t chunk_z,
								 uint8_t* ids, uint8_t* metadata,
								 uint8_t* sky, uint8_t* torch) {
	enum { COLUMN_CHUNKS = WORLD_HEIGHT / CHUNK_SIZE };
	struct chunk* column[COLUMN_CHUNKS];
	for(unsigned cy = 0; cy < COLUMN_CHUNKS; cy++) {
		column[cy] = world_find_chunk(&gstate.world,
			(w_coord_t)chunk_x * CHUNK_SIZE,
			(w_coord_t)(cy * CHUNK_SIZE),
			(w_coord_t)chunk_z * CHUNK_SIZE);
		if(!column[cy])
			return false;
	}
	memset(metadata, 0, REMOTE_COLUMN_NIBBLES);
	memset(sky, 0, REMOTE_COLUMN_NIBBLES);
	memset(torch, 0, REMOTE_COLUMN_NIBBLES);
	for(unsigned x = 0; x < CHUNK_SIZE; x++)
		for(unsigned z = 0; z < CHUNK_SIZE; z++)
			for(unsigned y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data blk
					= chunk_get_block(column[y / CHUNK_SIZE], (c_coord_t)x,
									  (c_coord_t)(y % CHUNK_SIZE), (c_coord_t)z);
				size_t idx
					= ((size_t)x * CHUNK_SIZE + (size_t)z) * WORLD_HEIGHT + y;
				unsigned shift = (y & 1u) * 4u;
				ids[idx] = blk.type;
				metadata[idx >> 1]
					|= (uint8_t)((blk.metadata & 15u) << shift);
				sky[idx >> 1] |= (uint8_t)((blk.sky_light & 15u) << shift);
				torch[idx >> 1] |= (uint8_t)((blk.torch_light & 15u) << shift);
			}
	return true;
}

/* Raw planes -> one zlib stream, the same framing inflateInit consumes on the
 * way back in. Single-shot Z_FINISH into the fixed staging buffer; returns
 * false on any zlib refusal (the caller leaves the column RESIDENT). */
static bool remote_column_deflate(const uint8_t* raw, size_t raw_length,
								  uint8_t* stage, size_t stage_capacity,
								  size_t* out_length) {
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	zs.zalloc = Z_NULL;
	zs.zfree = Z_NULL;
	zs.opaque = Z_NULL;
	if(deflateInit(&zs, REMOTE_DEMOTE_DEFLATE_LEVEL) != Z_OK)
		return false;
	zs.next_in = (Bytef*)raw;
	zs.avail_in = (uInt)raw_length;
	zs.next_out = stage;
	zs.avail_out = (uInt)stage_capacity;
	int result = deflate(&zs, Z_FINISH);
	size_t produced = (size_t)zs.total_out;
	deflateEnd(&zs);
	if(result != Z_STREAM_END || produced == 0
	   || produced > MP_CHUNK_COMPRESSED_CAP)
		return false;
	*out_length = produced;
	return true;
}

/* Reconstruct a BACKED column from its retained head body: inflate it into
 * the four planes. Only a FULL column body reconstructs a whole column --
 * a partial head describes a sub-volume and says nothing about the rest, so
 * it is refused rather than silently zero-filled. Used by the test export
 * hook; the demotion path never needs it (it reads the voxels). */
static bool remote_column_inflate_head_locked(
	const struct remote_chunk* chunk, uint8_t* raw, size_t raw_capacity) {
	const struct mp_map_chunk* wire = &chunk->wire;
	if(!wire->compressed || wire->inflated_length != raw_capacity
	   || wire->sx != 16 || wire->sy != 128 || wire->sz != 16 || wire->y != 0
	   || wire->x % 16 != 0 || wire->z % 16 != 0)
		return false;
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	if(inflateInit(&zs) != Z_OK)
		return false;
	zs.next_in = wire->compressed;
	zs.avail_in = (uInt)wire->compressed_length;
	zs.next_out = raw;
	zs.avail_out = (uInt)raw_capacity;
	int result = inflate(&zs, Z_FINISH);
	size_t produced = (size_t)zs.total_out;
	inflateEnd(&zs);
	return result == Z_STREAM_END && produced == raw_capacity;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* H4 proof hook -- see the declaration in server_remote_internal.h. */
static void (*s_deflate_probe)(struct server_remote_impl* remote) = NULL;
void server_remote_test_set_deflate_probe(void (*probe)(
	struct server_remote_impl* remote)) {
	s_deflate_probe = probe;
}
/* THE REFUSING CLIENT (F5b; review section 4's "the riskiest branch in the
 * diff has no test", and M7's own recourse). The client's
 * CRPC_UNLOAD_CHUNK handler silently no-ops while !s_world_reset_ready or
 * during CLIN_STREAM_RECOVERY_WAIT_RESET -- state private to
 * client_interface.c that a harness cannot enter without faking a partial
 * world reset. This seam reproduces the refusal the way the client does it:
 * the CRPC is simply NOT SENT, so the column genuinely stays installed and
 * the verification that follows (world_find_chunk, unchanged) reaches the
 * revert/retry branch against a TRUE world state rather than a lie. Arms for
 * the next `count` unload attempts, on either path. */
static uint32_t s_test_unload_refusals = 0;
void server_remote_test_refuse_unloads(uint32_t count) {
	s_test_unload_refusals = count;
}
static bool remote_test_unload_refused(void) {
	if(!s_test_unload_refusals)
		return false;
	s_test_unload_refusals--;
	return true;
}
#endif

/* Send the client's chunk unload -- the one place both the demotion and the
 * server-0x32 path go through, so the refusal seam above and the
 * verification that follows it cannot drift apart. */
static void remote_send_unload(int32_t chunk_x, int32_t chunk_z) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(remote_test_unload_refused())
		return;
#endif
	struct client_rpc call = {
		.type = CRPC_UNLOAD_CHUNK,
		.payload.unload_chunk = {.x = chunk_x, .z = chunk_z},
	};
	clin_process_direct_remote(&call);
}

/* Re-encode ONE resident column back into a compressed head body and unload
 * its voxels. Returns true when the entry ended up RETAINED (demoted);
 * false leaves it exactly as it was, RESIDENT, for the next pump.
 *
 * THE LOCK SPLIT (F5b, review finding H4). Called with impl->lock HELD (the
 * pump) and returns with it held, but it does NOT hold it across the
 * expensive part. Three phases:
 *
 *   (a) under impl->lock: the scratch, and an IDENTITY SNAPSHOT of the
 *       column -- coordinate, epoch, residency_seq, stamps, chain shape.
 *   (b) with impl->lock RELEASED: the 32,768-cell gather out of the client
 *       world and the level-6 deflate of 81,920 B. This is the whole reason
 *       the split exists: the read thread needs the SAME lock to retain a
 *       Packet51 (remote_retain_map_locked), so holding it across a deflate
 *       that costs tens of milliseconds on Broadway is a self-inflicted
 *       socket stall during exactly the join burst this card is about. It
 *       is now MEASURED, per the project's "measure, don't assume" rule:
 *       demote_deflate_us_last / _max.
 *   (c) under impl->lock again: REVALIDATE that the column is still the
 *       same residency of the same epoch (the read thread may have retained
 *       a new body, a dimension change may have cleared the table, a 0x32
 *       may have arrived), then install the head and unload the voxels.
 *       Anything that changed aborts the re-encode -- recode_aborts -- and
 *       the next pump retries from scratch; nothing is installed from a
 *       snapshot of a world that moved on.
 *
 * The world reads in (b) need no lock of their own: gstate.world is written
 * only from this (the main/pump) thread, and this function IS on it. What
 * does need protecting is demote_scratch, which the read thread's compaction
 * also uses and remote_chunks_clear_locked releases -- recode_busy holds
 * both of them off it for the duration (both test it under impl->lock). */
static bool remote_demote_one_locked(struct server_remote_impl* remote,
									 size_t index) {
	struct remote_chunk* chunk = &remote->chunks[index];
	/* REL-MP-FIX2a/b: demotion needs `installed`; a synthetic head exists
	 * only without voxels and its own promotion consumes it, so none reaches
	 * the prepend/revert below, which copy descriptors without the flag. The
	 * re-encode takes the voxels' provenance (voxels_synthetic). */
	assert(!chunk->synthetic_base);
	uint8_t* scratch = remote_demote_scratch_locked(remote);
	if(!scratch)
		return false;
	/* (a) THE IDENTITY SNAPSHOT. Everything (c) must find unchanged before
	 * it may install a body describing voxels read in (b). */
	const int32_t snap_x = chunk->chunk_x, snap_z = chunk->chunk_z;
	const uint32_t snap_epoch = chunk->epoch;
	const uint64_t snap_residency = chunk->residency_seq;
	const uint64_t snap_arrival = chunk->arrival;
	const uint64_t snap_newest = chunk->newest_applied;
	const enum remote_chunk_state snap_state = chunk->state;
	const uint8_t snap_queued = chunk->queued_count;
	const void* snap_head = chunk->wire.compressed;
	uint8_t* raw = scratch;
	uint8_t* stage = scratch + REMOTE_COLUMN_RAW_BYTES;
	/* Wire plane order is ids, metadata, BLOCK light, SKY light -- the
	 * opposite of the slot declaration's sky-then-torch, matching
	 * remote_finish_promotion_locked's memcpy pair. [740c583
	 * Chunk.getChunkData] */
	uint8_t* ids = raw;
	uint8_t* metadata = raw + REMOTE_COLUMN_CELLS;
	uint8_t* torch = metadata + REMOTE_COLUMN_NIBBLES;
	uint8_t* sky = torch + REMOTE_COLUMN_NIBBLES;
	size_t compressed_length = 0;
	/* (b) UNLOCKED. Nothing between the unlock and the re-lock may touch
	 * `remote` except the scratch this claim reserves, and nothing may call
	 * back into the client interface -- the gather is pure world reads. */
	assert(!remote->recode_busy);
	remote->recode_busy = true;
	thread_mutex_unlock(&remote->lock);
	ptime_t recode_t0 = time_get();
	bool encoded = remote_column_gather(snap_x, snap_z, ids, metadata, sky,
										torch);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	/* Inside the unlocked window, between the two costs: the test's probe
	 * trylocks impl->lock and asserts it SUCCEEDS. There is no lock
	 * operation anywhere in this phase, so a probe that succeeds here proves
	 * the whole phase runs unlocked. */
	if(s_deflate_probe)
		s_deflate_probe(remote);
#endif
	if(encoded)
		encoded = remote_column_deflate(raw, REMOTE_COLUMN_RAW_BYTES, stage,
										REMOTE_DEMOTE_STAGE_BYTES,
										&compressed_length);
	/* The project's own microsecond idiom (server_world.c's notify-cascade
	 * gauge): time_diff_s x 1e6, since platform/time.h exposes no us call. */
	uint32_t recode_us
		= (uint32_t)(time_diff_s(recode_t0, time_get()) * 1.0e6f);
	thread_mutex_lock(&remote->lock);
	/* (c) LOCKED AGAIN. */
	remote->recode_busy = false;
	remote->demote_deflate_us_last = recode_us;
	if(recode_us > remote->demote_deflate_us_max)
		remote->demote_deflate_us_max = recode_us;
	if(remote->scratch_release_pending) {
		/* A teardown or dimension change ran while the lock was down and
		 * parked its scratch release for us. Honour it and abandon this
		 * re-encode: its column no longer exists. */
		remote->scratch_release_pending = false;
		if(remote->demote_scratch) {
			mp_backing_scratch_release(MP_BACKING_SCRATCH_RECODE);
			remote->demote_scratch = NULL;
		}
		remote->recode_aborts++;
		return false;
	}
	if(!encoded)
		return false;
	/* IDENTITY REVALIDATION. A body describing the voxels as they were in
	 * (b) may only be installed onto the very same residency: a new
	 * Packet51 retained meanwhile would make `arrival` (and the chain shape
	 * the prepend below depends on) different, an unload would have taken
	 * the voxels, a dimension change would have replaced the entry. Any
	 * difference is a retry, not a repair -- the voxels are still installed,
	 * so nothing is lost by waiting one pump. */
	if(remote->stop_now || chunk->state != snap_state
	   || chunk->chunk_x != snap_x || chunk->chunk_z != snap_z
	   || chunk->epoch != snap_epoch || chunk->residency_seq != snap_residency
	   || chunk->arrival != snap_arrival || chunk->newest_applied != snap_newest
	   || chunk->queued_count != snap_queued
	   || chunk->wire.compressed != snap_head || !chunk->installed
	   || chunk->unload_pending
	   || (remote->promotion.active && remote->promotion.entry == index)) {
		remote->recode_aborts++;
		return false;
	}
	/* W2: the body comes out of the backing reservation, which can answer
	 * "no room". Reclaim (compaction, then a superseded head) and retry once;
	 * a second refusal has already ended the session cleanly with
	 * mp_end=SERVER_REMOTE_END_CAPACITY, and the caller counts this as a
	 * demotion failure. The voxels are still installed at this point, so
	 * nothing has been lost either way. */
	void* body = remote_body_alloc(compressed_length);
	if(!body) {
		if(!remote_backing_reclaim_or_fail_locked(remote, compressed_length)) {
			remote_backing_fail_guard_locked(remote); /* F5a (H3) */
			return false;
		}
		body = remote_body_alloc(compressed_length);
		if(!body)
			return false;
	}
	memcpy(body, stage, compressed_length);
	/* THE STAMP (design section 2.5): the newest thing the re-encode
	 * contains -- which is the INSTALLED voxels, no more and no less.
	 * newest_applied is exactly that: the arrival of the last body
	 * remote_finish_promotion_locked published plus every edit applied on
	 * top of it. Lower would let the drain replay an edit the re-encode
	 * already holds; higher would shadow a genuinely newer one.
	 *
	 * W1 wrote max(chunk->arrival, newest_applied) because for a column with
	 * no pending body those two are the same thing (publication raises
	 * newest_applied to chunk->arrival). W2 has to distinguish them: for a
	 * BACKED-with-voxels column chunk->arrival is the PENDING body's stamp --
	 * a body the re-encode does NOT contain -- so the max() form would stamp
	 * the re-encode over it and shadow it. Keep W1's form where it is
	 * correct, and use the installed-content stamp where it is not. */
	bool has_pending = chunk->wire.compressed != NULL;
	uint64_t stamp = has_pending
		? chunk->newest_applied
		: (chunk->arrival > chunk->newest_applied ? chunk->arrival
												  : chunk->newest_applied);
	if(has_pending && (stamp == 0 || stamp >= chunk->arrival)) {
		/* A pending body that is not newer than what is installed cannot be
		 * ordered around a re-encode, so refuse rather than guess. Unreachable
		 * through the production path (a body arriving for an installed column
		 * always gets a fresh, larger arrival stamp), asserted as such. */
		assert(!"pending body older than the installed voxels");
		remote_body_free(body);
		return false;
	}
	struct mp_map_chunk wire = {
		.x = chunk->chunk_x * 16,
		.y = 0,
		.z = chunk->chunk_z * 16,
		.sx = 16,
		.sy = 128,
		.sz = 16,
		.inflated_length = REMOTE_COLUMN_RAW_BYTES,
		.compressed = body,
		.compressed_length = compressed_length,
	};
	/* FULL CHAIN SNAPSHOT, so the client refusing the unload below is
	 * genuinely reversible. W1 saved only arrival + state, which was enough
	 * when the head was always NULL; the W2 prepend below MOVES the head and
	 * the whole queue, and a half-applied prepend would be an ordering bug
	 * that no later pump could repair. `struct remote_chunk`'s chain is
	 * head + REMOTE_CHUNK_QUEUE_MAX descriptors (pointers, not payloads), so
	 * this is a few hundred bytes of stack and zero copies of body bytes. */
	struct {
		struct mp_map_chunk wire;
		struct mp_map_chunk queued[REMOTE_CHUNK_QUEUE_MAX];
		uint64_t queued_arrival[REMOTE_CHUNK_QUEUE_MAX];
		uint8_t queued_count;
		uint64_t arrival;
		enum remote_chunk_state state;
		size_t retained_bytes;
	} saved;
	saved.wire = chunk->wire;
	memcpy(saved.queued, chunk->queued, sizeof(saved.queued));
	memcpy(saved.queued_arrival, chunk->queued_arrival,
		   sizeof(saved.queued_arrival));
	saved.queued_count = chunk->queued_count;
	saved.arrival = chunk->arrival;
	saved.state = chunk->state;
	saved.retained_bytes = remote->retained_bytes;
	if(!has_pending) {
		/* No pending body: takes ownership of `body` and drops the old head,
		 * which is already NULL (freed at publication), so nothing
		 * authoritative is released here. */
		remote_chunk_install_head_locked(remote, chunk, &wire, stamp);
	} else {
		/* W2 PREPEND (the W1 residual). This column has voxels AND a pending
		 * body that has not been installed yet -- the state a Packet51
		 * arriving for an already-installed column leaves behind. The
		 * re-encode describes what is installed, i.e. everything BEFORE that
		 * pending body, so it must become the HEAD and the existing chain
		 * must shift one place back, keeping every stamp. Then a
		 * re-promotion replays exactly the original order: installed state,
		 * then the pending body, then the rest of the chain.
		 *
		 * The caller guarantees there is room (queued_count <
		 * REMOTE_CHUNK_QUEUE_MAX); a full chain is left RESIDENT for a later
		 * pump, when the pending bodies have been consumed. */
		assert(chunk->queued_count < REMOTE_CHUNK_QUEUE_MAX);
		for(uint8_t k = chunk->queued_count; k > 0; k--) {
			chunk->queued[k] = chunk->queued[k - 1];
			chunk->queued_arrival[k] = chunk->queued_arrival[k - 1];
		}
		chunk->queued[0] = chunk->wire;
		chunk->queued_arrival[0] = chunk->arrival;
		chunk->queued_count++;
		memset(&chunk->wire, 0, sizeof(chunk->wire));
		remote_chunk_install_head_locked(remote, chunk, &wire, stamp);
	}
	chunk->state = REMOTE_CHUNK_RETAINED;
	chunk->synthetic_base = chunk->voxels_synthetic;
	/* The SAME teardown remote_emit_pending_unloads_locked uses, so mesher
	 * and worker references retire exactly as they do for a server unload --
	 * but residency_seq and unload_pending are deliberately untouched: this
	 * is not a server unload, the column is still loaded as far as the
	 * server (and every edit still arriving for it) is concerned, and those
	 * two fields are what tell an in-flight edit its residency ended. */
	remote_send_unload(chunk->chunk_x, chunk->chunk_z);
	/* The client's CRPC_UNLOAD_CHUNK handler is GATED (world-reset readiness,
	 * stream recovery) and silently no-ops when it refuses. A refusal would
	 * leave the voxels installed while this entry says BACKED -- stale
	 * terrain that a later re-promotion would install on top of. So verify
	 * the outcome instead of trusting the send, and revert cleanly if the
	 * client kept the column. */
	if(world_find_chunk(&gstate.world, (w_coord_t)chunk->chunk_x * CHUNK_SIZE, 0,
						(w_coord_t)chunk->chunk_z * CHUNK_SIZE)) {
		/* Restore the ENTIRE chain from the snapshot: free only the body this
		 * function created, then put every descriptor back exactly as it was
		 * (including retained_bytes, which install_head moved). */
		remote_body_free(chunk->wire.compressed);
		chunk->wire = saved.wire;
		memcpy(chunk->queued, saved.queued, sizeof(chunk->queued));
		memcpy(chunk->queued_arrival, saved.queued_arrival,
			   sizeof(chunk->queued_arrival));
		chunk->queued_count = saved.queued_count;
		chunk->arrival = saved.arrival;
		chunk->state = saved.state;
		chunk->synthetic_base = false; /* the assert above */
		remote->retained_bytes = saved.retained_bytes;
		return false;
	}
	/* The voxels are gone: nothing is applied to this coordinate any more
	 * until the re-promotion republishes the head. */
	chunk->newest_applied = 0;
	remote_set_installed_locked(remote, chunk, false);
	remote->demotions++;
	remote->demote_bytes += compressed_length;
	return true;
}

/* ======================================================================
 * W2: DEFERRED-LOG COMPACTION (design section 2.5, Astra gate-2 item 7)
 * ======================================================================
 * "Bound logs without silently losing authoritative state." The deferred
 * log parks one entry per coordinate for columns that are not installed yet;
 * a long join burst against a busy server can fill it, and W1's only answer
 * to a full log was to kill the connection. Compaction removes that cliff by
 * folding a backed column's parked edits INTO its own compressed head:
 * inflate, apply the edits, deflate, free the log entries. Nothing is lost --
 * the edits end up inside the authoritative body -- and the log gets its
 * slots back. This is the demotion code minus the world teardown (there are
 * no voxels to unload: the column is BACKED).
 *
 * THE STAMP is max(head arrival, newest folded edit), i.e. derived from what
 * the new body actually CONTAINS. That is what makes the ordering safe
 * without the demotion's decoded-ring veto: every edit still sitting in the
 * ring was received AFTER every edit already parked in the log (the ring is
 * FIFO and the pump fills the log from it in order), so a ring edit's stamp
 * is necessarily higher than this body's and the drain will apply it rather
 * than shadow it. A fresh ++arrival_sequence stamp would NOT have that
 * property, which is exactly why it is not used.
 *
 * LIGHT: the folded edits change ids and metadata only; the sky/torch planes
 * stay as the server sent them, byte for byte. That is not an approximation
 * -- it is what the ordinary path does too. A parked edit replayed by
 * remote_deferred_edit_drain_chunk_locked goes through CRPC_SET_BLOCK, whose
 * relight happens on the CLIENT after the column is installed; a compacted
 * column therefore relights on re-promotion exactly as a freshly installed
 * one does, from the same wire light arrays plus the same post-install
 * relight of the changed cells. Tests (j) proves that equivalence against a
 * resident column byte for byte.
 *
 * Returns true when a column was compacted (the caller may retry an
 * allocation, and the log has free slots). */
/* REL-MP-JOINSPEED: microseconds since `t0` (the project's idiom, as the
 * demotion's recode gauge) and the lock-hold gauges a compaction feeds. */
static uint32_t remote_us_since(ptime_t t0) {
	return (uint32_t)(time_diff_s(t0, time_get()) * 1.0e6f);
}
static void remote_compaction_lock_time(struct server_remote_impl* remote,
										uint32_t us) {
	remote->compaction_lock_us_last = us;
	if(us > remote->compaction_lock_us_max)
		remote->compaction_lock_us_max = us;
	remote->compaction_lock_us_total += us;
}

/* Apply every parked edit of `chunk` that is not older than `head_arrival`
 * to the raw column (ids + metadata nibbles; light stays as the body has it,
 * see LIGHT above). Returns how many, and the newest stamp in *stamp (which
 * starts at head_arrival). Chain order = stamp order; one entry per cell, so
 * the order between them is unobservable anyway. */
static uint32_t remote_compact_apply_edits_locked(struct server_remote_impl* remote,
												  const struct remote_chunk* chunk,
												  uint8_t* raw,
												  uint64_t head_arrival,
												  uint64_t* stamp) {
	uint8_t* ids = raw;
	uint8_t* metadata = raw + REMOTE_COLUMN_CELLS;
	uint32_t folded = 0;
	*stamp = head_arrival;
	for(uint16_t ref = chunk->edit_head; ref;) {
		const struct remote_deferred_edit* entry = remote_edit_at(remote, ref);
		ref = entry->next;
		/* An edit OLDER than the head is already inside it (the supersede
		 * pass on body arrival removes those, so this is a belt) -- folding
		 * it would resurrect a value the snapshot overwrote. */
		if(entry->arrival < head_arrival)
			continue;
		size_t idx = entry->cell; /* ((x*16 + z) * 128 + y), wire order */
		unsigned shift = ((unsigned)entry->cell & 1u) * 4u;
		ids[idx] = entry->type;
		metadata[idx >> 1] = (uint8_t)((metadata[idx >> 1]
										& ~(uint8_t)(0x0Fu << shift))
									   | (uint8_t)((entry->metadata & 15u) << shift));
		if(entry->arrival > *stamp)
			*stamp = entry->arrival;
		folded++;
	}
	return folded;
}

/* Release EXACTLY the set remote_compact_apply_edits_locked folded -- same
 * column, same [head_arrival, stamp] window. Widening it by one comparison
 * would release an edit the new body does NOT contain, which is the silent
 * loss this whole mechanism exists to avoid. */
static void remote_compact_release_locked(struct server_remote_impl* remote,
										  struct remote_chunk* chunk,
										  uint64_t head_arrival, uint64_t stamp) {
	uint16_t prev = 0;
	uint16_t* link = &chunk->edit_head;
	while(*link) {
		struct remote_deferred_edit* entry = remote_edit_at(remote, *link);
		if(entry->arrival >= head_arrival && entry->arrival <= stamp) {
			remote_edit_drop_locked(remote, chunk, link, prev);
			/* Applied, not dropped: it is inside the body now. Counted on the
			 * same cumulative gauge the drain uses, so "every parked edit was
			 * eventually applied" stays a true statement about the session. */
			remote->deferred_edits_applied++;
			continue;
		}
		prev = *link;
		link = &entry->next;
	}
}

/* Install the compacted column as `chunk`'s head and release the folded
 * edits. Takes ownership of `body`. */
static void remote_compact_commit_locked(struct server_remote_impl* remote,
										 struct remote_chunk* chunk, void* body,
										 size_t compressed_length, bool bodyless,
										 uint64_t head_arrival, uint64_t stamp,
										 uint32_t folded) {
	struct mp_map_chunk wire = {
		.x = chunk->chunk_x * 16,
		.y = 0,
		.z = chunk->chunk_z * 16,
		.sx = 16,
		.sy = 128,
		.sz = 16,
		.inflated_length = REMOTE_COLUMN_RAW_BYTES,
		.compressed = body,
		.compressed_length = compressed_length,
	};
	/* Takes ownership of `body` and frees the old head -- which the new one
	 * supersedes cell for cell by construction (it IS that head plus the
	 * folded edits), so this is the one place a head may be dropped without
	 * losing state. Only AFTER it succeeds are the log entries released.
	 * Head plus edits keeps the head's provenance (REL-MP-FIX2a). */
	bool synthetic = chunk->synthetic_base || bodyless;
	remote_chunk_install_head_locked(remote, chunk, &wire, stamp);
	chunk->synthetic_base = synthetic;
	chunk->state = REMOTE_CHUNK_RETAINED; /* bodyless: published atomically */
	remote_compact_release_locked(remote, chunk, head_arrival, stamp);
	remote->compactions++;
	remote->compacted_edits += folded;
}

/* The fully LOCKED compaction of `victim` (chain collapse, partial heads, and
 * every non-pump caller: the exhaustion rescue and the backing reclaim). */
static bool remote_compact_victim_locked(struct server_remote_impl* remote,
										 size_t victim, bool pump) {
	struct remote_chunk* chunk = &remote->chunks[victim];
	uint8_t* scratch = remote_demote_scratch_locked(remote);
	if(!scratch) {
		remote->compaction_failures++;
		return false;
	}
	uint8_t* raw = scratch;
	uint8_t* stage = scratch + REMOTE_COLUMN_RAW_BYTES;
	/* REL-MP: collapse a chain into its head first. The supersede rule is
	 * re-run for the head and EVERY queued body with their own stamps by the
	 * fold, after the ring barrier: retain-time supersede cannot see an older
	 * edit the pump parked after the body arrived, and once the bodies fold
	 * into the head (which keeps the ORIGINAL head stamp) their stamps are
	 * gone. Then every edit still parked for the column is newer than every
	 * body covering its cell, so bodies-then-edits equals wire order. */
	bool superseded = false, collapsed_chain = false;
	bool bodyless = !chunk->wire.compressed;
	if(chunk->queued_count
	   || (!bodyless && !remote_body_is_full_column(&chunk->wire))) {
		uint32_t parked = remote->deferred_edits;
		size_t nospace_bytes = 0;
		enum remote_fold_result collapsed
			= remote_chunk_fold_locked(remote, chunk, NULL, &nospace_bytes);
		superseded = remote->deferred_edits < parked;
		if(collapsed == REMOTE_FOLD_FAILED) {
			/* A retained body does not decode: terminal, with the fold's own
			 * reason (mp_end 6 already latched). The retain path's caller
			 * publishes it by returning false; here nobody would, so end the
			 * session through the ordinary kill. */
			if(!remote->stop_now) {
				char reason[sizeof(remote->reason)];
				snprintf(reason, sizeof(reason), "%s", remote->reason);
				kill_connection_locked(remote, reason);
			}
			return false;
		}
		if(collapsed != REMOTE_FOLD_DONE) {
			/* NOMEM / NOSPACE: the chain is untouched (the fold allocates its
			 * body before freeing anything) and no edit was released. No
			 * reclaim from in here -- this IS a reclaim step. */
			remote->compaction_failures++;
			return superseded && !pump; /* pump: refusal ends this pump */
		}
		collapsed_chain = true;
	}
	/* REL-MP-FIX2b: a bodyless column's base is the pre-chunk, stamped at
	 * its load (record drops every older edit). */
	if(bodyless) {
		memset(raw, 0, REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES);
		memset(raw + REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES, 0xFF,
			   REMOTE_COLUMN_NIBBLES);
	} else if(!remote_column_inflate_head_locked(chunk, raw,
											   REMOTE_COLUMN_RAW_BYTES)) {
		remote->compaction_failures++;
		return (superseded || collapsed_chain) && !pump;
	}
	uint64_t head_arrival = bodyless ? chunk->residency_seq : chunk->arrival;
	uint64_t stamp = head_arrival;
	uint32_t folded = remote_compact_apply_edits_locked(remote, chunk, raw,
														head_arrival, &stamp);
	if(!folded) /* nothing this column could contribute; not a failure */
		return superseded || collapsed_chain; /* progress all the same */
	size_t compressed_length = 0;
	if(!remote_column_deflate(raw, REMOTE_COLUMN_RAW_BYTES, stage,
							  REMOTE_DEMOTE_STAGE_BYTES, &compressed_length)) {
		remote->compaction_failures++;
		return false;
	}
	/* No reclaim-and-retry recursion here: this function IS the first reclaim
	 * step, and a compaction that cannot get its (usually SMALLER) body is
	 * simply not the way out of a full reservation. The caller falls through
	 * to the next reclaim, then to the clean capacity failure. */
	void* body = remote_body_alloc(compressed_length);
	if(!body) {
		remote->compaction_failures++;
		return false;
	}
	memcpy(body, stage, compressed_length);
	remote_compact_commit_locked(remote, chunk, body, compressed_length,
								 bodyless, head_arrival, stamp, folded);
	return true;
}

/* REL-MP-JOINSPEED: the PUMP's compaction of a chain-free column (a full
 * head, or none at all) with its deflate -- the dominant cost, ~5x the
 * inflate -- run with impl->lock RELEASED, exactly the demotion's protocol
 * (F5b, H4): recode_busy owns demote_scratch across the gap, the read
 * thread's fold and reclaim see it and wait or skip, and a teardown parks its
 * scratch release in scratch_release_pending.
 *
 * Why the edits may be applied before the unlock: only this (pump) thread
 * records edits, so while the lock is down the column can LOSE edits (a
 * retained body supersedes them, an unload purges them) but never gain or
 * change one -- and every way of losing one also changes an identity field
 * revalidated below (the head pointer, the chain, the state, the residency).
 * Any change throws the work away (compaction_aborts) and nothing was
 * released: the edits are still parked and the next pump retries. */
static bool remote_compact_unlocked_pump(struct server_remote_impl* remote,
										 size_t victim) {
	ptime_t t0 = time_get();
	struct remote_chunk* chunk = &remote->chunks[victim];
	uint8_t* scratch = remote_demote_scratch_locked(remote);
	if(!scratch) {
		remote->compaction_failures++;
		remote_compaction_lock_time(remote, remote_us_since(t0));
		return false;
	}
	uint8_t* raw = scratch;
	uint8_t* stage = scratch + REMOTE_COLUMN_RAW_BYTES;
	bool bodyless = !chunk->wire.compressed;
	if(bodyless) {
		memset(raw, 0, REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES);
		memset(raw + REMOTE_COLUMN_CELLS + 2u * REMOTE_COLUMN_NIBBLES, 0xFF,
			   REMOTE_COLUMN_NIBBLES);
	} else if(!remote_column_inflate_head_locked(chunk, raw,
											   REMOTE_COLUMN_RAW_BYTES)) {
		remote->compaction_failures++;
		remote_compaction_lock_time(remote, remote_us_since(t0));
		return false;
	}
	uint64_t head_arrival = bodyless ? chunk->residency_seq : chunk->arrival;
	uint64_t stamp = head_arrival;
	uint32_t folded = remote_compact_apply_edits_locked(remote, chunk, raw,
														head_arrival, &stamp);
	if(!folded) {
		remote_compaction_lock_time(remote, remote_us_since(t0));
		return false;
	}
	/* THE IDENTITY SNAPSHOT, revalidated after the gap. */
	const int32_t snap_x = chunk->chunk_x, snap_z = chunk->chunk_z;
	const uint32_t snap_epoch = chunk->epoch;
	const uint64_t snap_residency = chunk->residency_seq;
	const uint64_t snap_arrival = chunk->arrival;
	const enum remote_chunk_state snap_state = chunk->state;
	const void* snap_head = chunk->wire.compressed;
	const uint16_t snap_edits = chunk->edit_count;
	uint32_t locked_us = remote_us_since(t0);
	assert(!remote->recode_busy);
	remote->recode_busy = true;
	thread_mutex_unlock(&remote->lock);
	size_t compressed_length = 0;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_deflate_probe)
		s_deflate_probe(remote); /* inside the unlocked window (test (w)) */
#endif
	bool encoded = remote_column_deflate(raw, REMOTE_COLUMN_RAW_BYTES, stage,
										 REMOTE_DEMOTE_STAGE_BYTES,
										 &compressed_length);
	thread_mutex_lock(&remote->lock);
	ptime_t t1 = time_get();
	remote->recode_busy = false;
	bool ok = false;
	if(remote->scratch_release_pending) {
		/* A teardown or dimension change ran while the lock was down: its
		 * column is gone. Honour the parked release, abandon the work. */
		remote->scratch_release_pending = false;
		if(remote->demote_scratch) {
			mp_backing_scratch_release(MP_BACKING_SCRATCH_RECODE);
			remote->demote_scratch = NULL;
		}
		remote->compaction_aborts++;
	} else if(!encoded) {
		remote->compaction_failures++;
	} else if(remote->stop_now || chunk->state != snap_state
			  || chunk->chunk_x != snap_x || chunk->chunk_z != snap_z
			  || chunk->epoch != snap_epoch
			  || chunk->residency_seq != snap_residency
			  || chunk->arrival != snap_arrival
			  || chunk->wire.compressed != snap_head || chunk->queued_count
			  || chunk->installed || chunk->unload_pending
			  || chunk->edit_count != snap_edits
			  || (remote->promotion.active && remote->promotion.entry == victim)) {
		remote->compaction_aborts++;
	} else {
		void* body = remote_body_alloc(compressed_length);
		if(!body) {
			remote->compaction_failures++;
		} else {
			memcpy(body, stage, compressed_length);
			remote_compact_commit_locked(remote, chunk, body, compressed_length,
										 bodyless, head_arrival, stamp, folded);
			remote->compaction_unlocked++;
			ok = true;
		}
	}
	remote_compaction_lock_time(remote, locked_us + remote_us_since(t1));
	return ok;
}

static bool remote_compact_one_locked(struct server_remote_impl* remote,
									  bool force, bool pump) {
	if(remote->stop_now)
		return false;
	/* REL-MP-FIX3a-r: only the pump latches (HIGH_WATER..LOW_WATER); reclaim
	 * and the forced rescue keep the 450fbf31f gate and victim rule. */
	if(pump) {
		if(remote->deferred_edits > REMOTE_DEFERRED_EDIT_HIGH_WATER)
			remote->deferred_pressure = true;
		else if(remote->deferred_edits <= REMOTE_DEFERRED_EDIT_LOW_WATER)
			remote->deferred_pressure = false;
		if(!remote->deferred_pressure)
			return false;
	} else if(!force
			  && remote->deferred_edits <= REMOTE_DEFERRED_EDIT_HIGH_WATER)
		return false;
	/* F5b (review finding H4): the demotion's re-encode runs its gather and
	 * deflate with impl->lock RELEASED, and it owns demote_scratch for that
	 * whole window. This function -- reachable from the READ thread through
	 * the reclaim ladder -- needs the same buffer, so while a re-encode is
	 * in flight there is nothing to compact into. Not a failure and not
	 * counted as one: the pump clears the flag within one frame and the next
	 * call (the reclaim ladder's own retry, or the next pump) proceeds. */
	if(remote->recode_busy)
		return false;
	/* The tally of parked edits per chunk-table slot is each column's own
	 * chain length (REL-MP-JOINSPEED; it used to be rebuilt by mapping every
	 * log entry to its slot, a 1,024 x 576 scan per call). The table is the
	 * protocol's hard bound (576), so the scan below stays bounded without
	 * excluding any lossless victim. */
	size_t victim = REMOTE_CHUNK_ENTRY_CAPACITY;
	uint16_t victim_count = 0;
	bool victim_base = true;
	for(size_t index = 0; index < REMOTE_CHUNK_ENTRY_CAPACITY; index++) {
		struct remote_chunk* chunk = &remote->chunks[index];
		uint16_t tally = chunk->edit_count;
		if(!tally)
			continue;
		/* BACKED, or bodyless (REL-MP-FIX2b: the pre-chunk base). An
		 * installed column has no parked edits worth folding (they drain
		 * live), and a promoting column is mid-decode. */
		bool bodyless = !chunk->wire.compressed;
		if((chunk->state != REMOTE_CHUNK_RETAINED
			&& !(chunk->state == REMOTE_CHUNK_ANNOUNCED && bodyless))
		   || chunk->installed || chunk->unload_pending)
			continue;
		if(remote->promotion.active && remote->promotion.entry == index)
			continue;
		/* REL-MP (DIAG.md section 5): a CHAINED column is a victim only behind
		 * the IN-009 fold gate -- a full head, no promotion owning it, the
		 * recode scratch free, and the ORDER BARRIER: no Packet52/53 or
		 * explosion for it still in the receive ring, since such an edit can
		 * be older than a queued body yet parked only after the collapse below
		 * erased that body's stamp. WAIT and NEVER both skip it. An unchained
		 * head needs no barrier (every ring edit is newer than every parked
		 * edit, so it stays parked above the folded stamp), so it is admitted
		 * exactly as before. */
		/* REL-MP-FIX2b: so does a partial head (folds onto the pre-chunk). */
		bool base = bodyless || !remote_body_is_full_column(&chunk->wire);
		uint64_t newest = chunk->arrival;
		for(uint8_t q = 0; q < chunk->queued_count; q++)
			if(chunk->queued_arrival[q] > newest)
				newest = chunk->queued_arrival[q];
		uint8_t gate_cause = REMOTE_FOLD_WAIT_NONE;
		if((chunk->queued_count || (base && !bodyless))
		   && remote_fold_gate_locked(remote, index, newest, &gate_cause)
			   != REMOTE_FOLD_GATE_NOW)
			continue;
		/* A full head first; a pre-chunk base only if none (Astra). The pump
		 * (REL-MP-FIX3a-r, walk-diag.md) takes the greatest tally whatever
		 * backs it, the most slots per fold; ties keep the lowest slot. */
		if(pump ? tally > victim_count
				: (victim == REMOTE_CHUNK_ENTRY_CAPACITY || base < victim_base
				   || (base == victim_base && tally > victim_count))) {
			victim = index;
			victim_count = tally;
			victim_base = base;
		}
	}
	if(victim == REMOTE_CHUNK_ENTRY_CAPACITY)
		return false;
	const struct remote_chunk* chosen = &remote->chunks[victim];
	if(pump && !chosen->queued_count
	   && (!chosen->wire.compressed || remote_body_is_full_column(&chosen->wire)))
		return remote_compact_unlocked_pump(remote, victim);
	ptime_t t0 = time_get();
	bool compacted = remote_compact_victim_locked(remote, victim, pump);
	remote_compaction_lock_time(remote, remote_us_since(t0));
	return compacted;
}

void remote_residency_step_locked(struct server_remote_impl* remote) {
	/* F5b: keyed on the admission CENTRE, not on have_authoritative_position
	 * -- the same accessor the promotion gate uses, so the two halves of the
	 * radius policy can never disagree about where the player is (H5/L12).
	 * A session with no centre at all has no meaningful distance to demote
	 * by; the hard resident cap in remote_start_promotion_locked is what
	 * bounds that case, and it needs no centre. */
	if(remote->stop_now || !remote->have_admit_centre)
		return;
	/* ORDERING VETO. An edit received before this moment can still be
	 * sitting in the decoded-event ring (the pump drains a bounded budget
	 * per frame). Demoting now would stamp the re-encode NEWER than that
	 * edit, and when the column is re-promoted the drain's shadow rule would
	 * discard it as "already contained" -- which it is not, it was never
	 * applied. The publication gate (remote_finish_promotion_locked) refuses
	 * to publish under exactly the same condition; this is the same rule on
	 * the way out. A drained queue is the normal state at the pump tail (one
	 * pass' budget is 4x the ring), so this only defers a demotion under a
	 * sustained burst, and it retries every pump.
	 *
	 * F5b (review finding H1): the veto is BOUNDED now. Unbounded, a busy
	 * edit stream pinned the resident set -- the promotion gate's hard cap
	 * would then refuse every new column for as long as the stream lasted,
	 * which is a stall, not a bound. So the blanket veto holds for at most
	 * REMOTE_RESIDENCY_VETO_PUMP_LIMIT CONSECUTIVE pumps; past that the step
	 * runs and each candidate is tested individually against the
	 * publication-gate rule instead (see the per-column test below). The
	 * counter resets the moment a pump sees an empty ring, so an ordinary
	 * burst never reaches the limit. */
	bool veto_override = false;
	if(remote->event_count) {
		if(remote->residency_veto_pumps < REMOTE_RESIDENCY_VETO_PUMP_LIMIT) {
			remote->residency_veto_pumps++;
			remote->residency_veto_skips++;
			return;
		}
		veto_override = true;
	} else {
		remote->residency_veto_pumps = 0;
	}
	/* NO presentation-state veto here, on purpose (Astra gate-2 item 9:
	 * presentation resets and network-world identity stay separate).
	 * gstate.world_switch_pending / world_loaded are screen-and-input gates;
	 * the client's CRPC_UNLOAD_CHUNK handler gates on its OWN private state
	 * (s_world_reset_ready, the stream-recovery phase), which this side
	 * cannot read -- so guessing from the presentation flags would both
	 * mis-layer the decision and still be wrong. remote_demote_one_locked
	 * VERIFIES the unload actually took effect and reverts cleanly if the
	 * client refused, which is the authoritative test. The world-switch case
	 * itself cannot arise: a dimension change clears the whole table on the
	 * read thread (remote_chunks_clear_locked), so no RESIDENT entry of an
	 * old world survives to be demoted out from under a pending switch. */
	size_t victim = REMOTE_CHUNK_ENTRY_CAPACITY;
	uint64_t victim_distance = 0;
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		struct remote_chunk* chunk = &remote->chunks[i];
		/* W2: the precondition is "does this column HOLD VOXELS", not "is
		 * its entry RESIDENT". A Packet51 arriving for an already installed
		 * column moves the entry to RETAINED with the new body pending while
		 * the blocks stay installed, and the RESIDENT-only test left exactly
		 * those columns holding 81,920 B of carve for the rest of the
		 * session when they were out of the admission radius -- the residual
		 * W1 recorded. remote_demote_one_locked preserves the pending chain
		 * by prepending the re-encode ahead of it. */
		if(!chunk->installed)
			continue;
		if(chunk->state == REMOTE_CHUNK_PROMOTING)
			continue; /* its own pending body is mid-decode */
		if(chunk->unload_pending)
			continue; /* the pump's unload emit owns this entry */
		if(remote->promotion.active && remote->promotion.entry == i)
			continue; /* a body for this very column is mid-decode */
		/* The prepend needs one free chain slot. A column whose chain is
		 * already full keeps its voxels this pump; its pending bodies are
		 * being consumed by the promotion pipeline, so a later pump finds
		 * room. Not a failure -- nothing was attempted. */
		if(chunk->wire.compressed
		   && chunk->queued_count >= REMOTE_CHUNK_QUEUE_MAX)
			continue;
		/* HYSTERESIS of one column (design section 2.3): demote only past
		 * admit_radius + 1, so a player pacing across the boundary does not
		 * churn a column's 81,920 B in and out every other frame. */
		uint64_t distance = remote_chunk_distance_locked(remote, chunk);
		if(distance <= (uint64_t)remote->admit_radius + 1u)
			continue;
		if(veto_override) {
			/* THE PER-COLUMN ORDERING TEST (F5b, H1) -- the publication gate
			 * applied to the stamp this column's re-encode would carry
			 * (remote_demote_one_locked computes it the same way). The
			 * re-encode is safe exactly when every event still queued is
			 * NEWER than that stamp, because the drain shadows an edit only
			 * when it is STRICTLY older than the body's stamp
			 * (remote_deferred_edit_drain_chunk_locked). The ring is FIFO,
			 * so testing its HEAD tests all of them.
			 *
			 * Strict `>` on the packet seq, where the publication gate uses
			 * `>=`: an arrival is (seq << 16 | sub), so seq equality would
			 * still permit head_arrival < stamp for a stamp with a non-zero
			 * sub-index -- and that edit would then be shadowed, i.e. lost.
			 * One packet of extra patience is the cheap side of that. */
			uint64_t stamp = chunk->wire.compressed
				? chunk->newest_applied
				: (chunk->arrival > chunk->newest_applied
					   ? chunk->arrival
					   : chunk->newest_applied);
			if(!(remote->events[remote->event_head].seq > (stamp >> 16)))
				continue;
		}
		/* Farthest first (Astra gate-2 item 5 permits the choice, given the
		 * state survives), oldest-arrival as the tiebreak -- the same order
		 * remote_evict_one_locked picks in. */
		if(victim == REMOTE_CHUNK_ENTRY_CAPACITY || distance > victim_distance
		   || (distance == victim_distance
			   && chunk->arrival < remote->chunks[victim].arrival)) {
			victim = i;
			victim_distance = distance;
		}
	}
	if(victim == REMOTE_CHUNK_ENTRY_CAPACITY)
		return;
	/* A RESIDENT column never holds a chain: remote_finish_promotion_locked
	 * only reaches RESIDENT when the queue is empty, and a later body moves
	 * the entry straight back to RETAINED -- which W2 also demotes, and
	 * there the chain is exactly what the prepend preserves. */
	assert(remote->chunks[victim].state != REMOTE_CHUNK_RESIDENT
		   || remote->chunks[victim].queued_count == 0);
	/* At most ONE demotion per pump (design section 2.3): the re-encode is
	 * a 32768-cell gather plus a full-column deflate, and the promotion step
	 * that runs just before it already owns a bounded slice of this frame. */
	if(veto_override)
		remote->residency_veto_overrides++;
	if(!remote_demote_one_locked(remote, victim))
		remote->demotion_failures++;
}

void remote_emit_pending_unloads_locked(struct server_remote_impl* remote) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		struct remote_chunk* chunk = &remote->chunks[i];
		if(!chunk->unload_pending)
			continue;
		// S5.3 hunt fix (round 4): same wrong-thread-producer pattern, same
		// fix -- called from server_remote_pump() (server_remote.c:166),
		// main thread. clin_process_direct never returns false, so the old
		// early-return (abandoning the rest of this loop for next pump) is
		// unreachable and removed.
		remote_send_unload(chunk->chunk_x, chunk->chunk_z);
		/* M7 (F5b, review finding M7). The voxels are NOT gone either way,
		 * which is what W2's comment here assumed: the client's
		 * CRPC_UNLOAD_CHUNK handler is GATED (s_world_reset_ready, the
		 * stream-recovery phase) and silently no-ops when it refuses
		 * (client_interface.c) -- which is precisely why the DEMOTION path
		 * verifies its own unload. Trusting it here destroyed the entry and
		 * its body while 81,920 B of CARVE_VOXEL stayed installed with no
		 * ledger entry pointing at them: permanently un-demotable,
		 * un-unloadable, and invisible to resident_cols, so every budget
		 * claim built on that counter was understated. Protocol 14 has no
		 * re-request, so the column would only heal if the server happened
		 * to re-send it.
		 *
		 * Same verification as the demotion, same recourse: if the client
		 * kept the column, keep the entry AND `unload_pending` and retry on
		 * the next pump. The retry is free (the CRPC is a direct call on
		 * this thread) and the condition it waits on -- the client leaving
		 * stream recovery / becoming reset-ready -- is bounded by the
		 * client's own state machine. */
		if(world_find_chunk(&gstate.world, (w_coord_t)chunk->chunk_x * CHUNK_SIZE,
							0, (w_coord_t)chunk->chunk_z * CHUNK_SIZE)) {
			remote->unload_refused_retries++;
			continue;
		}
		remote_set_installed_locked(remote, chunk, false);
		bool reload = chunk->reload_after_unload;
		if(reload) {
			/* The read thread can observe unload -> load -> Packet51 before this
			 * main-thread CRPC is accepted. The mode=false handler already dropped
			 * the old body; preserve any subsequently retained replacement after
			 * ordering the client unload ahead of it. Clearing the whole record here
			 * would leak its compressed allocation/accounting and create a world hole. */
			chunk->unload_pending = false;
			chunk->reload_after_unload = false;
			continue;
		}
		remote_chunk_drop_body_locked(remote, chunk);
		/* REL-MP-JOINSPEED: the slot's edit chain goes with it. Empty by
		 * construction (the unload purged it and record drops edits for an
		 * unload with no reload), but a chain must never outlive its owner
		 * slot, so it is released (and counted) rather than assumed. */
		remote_deferred_edit_purge_chunk_locked(remote, chunk);
		memset(chunk, 0, sizeof(*chunk));
		if(remote->retained_entries > 0)
			remote->retained_entries--;
	}
}

/* W1: the public admission-radius dial. Defined here, beside the gate and the
 * residency step that read it, rather than in server_remote.c's API seam --
 * this is chunk-admission state, not session lifecycle. See the declaration
 * in server_remote.h for the call-ordering requirement (after start). */
void server_remote_set_admit_radius(struct server_remote* remote, int radius) {
	if(!remote || !remote->impl)
		return;
	struct server_remote_impl* impl = remote->impl;
	if(radius < SERVER_REMOTE_ADMIT_RADIUS_MIN)
		radius = SERVER_REMOTE_ADMIT_RADIUS_MIN;
	if(radius > SERVER_REMOTE_ADMIT_RADIUS_MAX)
		radius = SERVER_REMOTE_ADMIT_RADIUS_MAX;
	thread_mutex_lock(&impl->lock);
	impl->admit_radius = radius;
	thread_mutex_unlock(&impl->lock);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* REL-MP-FIX2a proof hook: fold (chunk_x, chunk_z)'s head and chain now. */
bool server_remote_test_fold_column(struct server_remote* remote,
									int32_t chunk_x, int32_t chunk_z) {
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	size_t index = remote_chunk_find_locked(impl, chunk_x, chunk_z);
	size_t nospace = 0;
	bool done = index != REMOTE_CHUNK_ENTRY_CAPACITY
		&& remote_chunk_fold_locked(impl, &impl->chunks[index], NULL, &nospace)
			== REMOTE_FOLD_DONE;
	thread_mutex_unlock(&impl->lock);
	return done;
}
#endif

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool server_remote_test_export_column(struct server_remote* remote,
									  int32_t chunk_x, int32_t chunk_z,
									  uint8_t* out_ids, uint8_t* out_metadata,
									  uint8_t* out_sky, uint8_t* out_torch) {
	assert(remote && remote->impl && out_ids && out_metadata && out_sky
		   && out_torch);
	struct server_remote_impl* impl = remote->impl;
	bool ok = false;
	thread_mutex_lock(&impl->lock);
	size_t index = remote_chunk_find_locked(impl, chunk_x, chunk_z);
	if(index != REMOTE_CHUNK_ENTRY_CAPACITY) {
		struct remote_chunk* chunk = &impl->chunks[index];
		if(chunk->state == REMOTE_CHUNK_RESIDENT) {
			/* Installed: read the voxels back -- the identical gather the
			 * demotion re-encode uses, so "export == original" is a real
			 * statement about what a demotion would have captured. */
			ok = remote_column_gather(chunk_x, chunk_z, out_ids, out_metadata,
									  out_sky, out_torch);
		} else {
			/* BACKED: inflate the head body and apply nothing. Needs the
			 * scratch as the inflate destination (81,920 B contiguous, which
			 * the four caller planes are not). */
			uint8_t* scratch = remote_demote_scratch_locked(impl);
			if(scratch
			   && remote_column_inflate_head_locked(chunk, scratch,
													REMOTE_COLUMN_RAW_BYTES)) {
				memcpy(out_ids, scratch, REMOTE_COLUMN_CELLS);
				memcpy(out_metadata, scratch + REMOTE_COLUMN_CELLS,
					   REMOTE_COLUMN_NIBBLES);
				memcpy(out_torch,
					   scratch + REMOTE_COLUMN_CELLS + REMOTE_COLUMN_NIBBLES,
					   REMOTE_COLUMN_NIBBLES);
				memcpy(out_sky,
					   scratch + REMOTE_COLUMN_CELLS
						   + 2 * REMOTE_COLUMN_NIBBLES,
					   REMOTE_COLUMN_NIBBLES);
				ok = true;
			}
		}
	}
	thread_mutex_unlock(&impl->lock);
	return ok;
}
#endif

#endif /* CAVEWII_MP_CLIENT */
