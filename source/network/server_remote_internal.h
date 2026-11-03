/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef SERVER_REMOTE_INTERNAL_H
#define SERVER_REMOTE_INTERNAL_H

/* PRIVATE to the server_remote_*.c family (transport/protocol/entities/
 * chunks + this file's own server_remote.c seam). Not for inclusion outside
 * source/network/ -- it exposes struct server_remote_impl's full layout and
 * the cross-module helpers the four translation units share. Every function
 * declared below is defined in exactly one of the server_remote_*.c files
 * (named in its own comment) and consumed by at least one other; anything
 * used from only a single .c stays `static` in that file and is NOT listed
 * here. This header exists purely so those five translation units can build
 * as one logical implementation of server_remote.h's public API -- it carries
 * no additional public surface of its own.
 *
 * 2026-08-26 split (maintainer-register item 1): server_remote.c was a single
 * ~3970-line file spanning transport, wire-protocol dispatch, remote-entity
 * tracking and chunk retention/promotion. This header is the seam that split
 * created: struct server_remote_impl (and every type nested inside it) moved
 * here verbatim so all five .c files see the identical layout, and the few
 * dozen functions genuinely called across module lines were promoted from
 * `static` and declared here. Nothing in server_remote.h (the PUBLIC API)
 * changed -- this is purely an internal seam. */

#include "server_remote.h"

#ifdef CAVEWII_MP_CLIENT

#include "../platform/net_socket.h"
#include "../platform/thread.h"
#include "../platform/time.h"
#include "../platform/cw_canary.h"
#include "../block/blocks.h"
#include "../block/noteblock_logic.h" // S4: note_instrument/noteblock_pitch (0x36)
#include "../platform/audio.h"        // S4: SOUND_NOTE_*/SOUND_RANDOM_* ids
#include "../worldgen/jmath.h" // S4 fix: java_double_to_int (explosion origin cast)
#include "../worldgen/worldgen.h"
#include "../util.h" // residual item 4: struct random_gen/rand_gen_* for the
                      // dedicated mob/player hurt-death sound pitch wobble
                      // (server_local_sound_pitch_wobble's own precedent,
                      // reimplemented locally to avoid pulling in server_local.h)
#include "../game/sign_text_store.h" // SIGN_TEXT_LINES/SIGN_TEXT_COLS (MP_EVENT_UPDATE_SIGN forward)
#include "../item/item_types.h" // ITEM_MAP == Item.mapItem.shiftedIndex (358) gate
#include "client_interface.h"
#include "map_data.h" // MAP_DATA_DIM/MAP_DATA_COLORS_SIZE (0x83 strip apply)
#include "mp_codec.h"
#include "server_interface.h"

/* Eight planes; only clean LRU entries may be reused. */
#define REMOTE_MAP_MIRRORS 8
#define REMOTE_MAP_RETRIES 64

#include <assert.h>
#include <math.h>
#include <stdatomic.h> /* Fix B (2026-08-31): cross-thread-sampled diagnostic
                         * counters (rx_bytes, dec_*_sample below) -- see
                         * their own field comments. GCC/Clang provide this
                         * unconditionally regardless of -std=c99/gnu99
                         * (verified: this project's C_STANDARD 99 setting
                         * does not gate it out), and devkitPPC's GCC front
                         * end carries the same C11 atomics support. */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define REMOTE_QUEUE_CAPACITY SERVER_REMOTE_EVENT_QUEUE_CAPACITY
/* The header's capacity comment promises this assert exists (hostile-review
 * low 15) -- keep the two names agreeing if either ever decouples. */
_Static_assert(REMOTE_QUEUE_CAPACITY == SERVER_REMOTE_EVENT_QUEUE_CAPACITY,
			   "queue capacity names must agree");
#define REMOTE_TRACE_CAPACITY 32
/* Read-thread socket drain size -- see struct server_remote_impl's
 * read_receive_buffer field comment for the full root-cause writeup. Kept
 * equal to the Wii backend's NET_IOS_MAX_CHUNK (source/platform/wii/
 * net_socket.c) so one buffer fill is exactly one IOS network transfer. */
#define REMOTE_READ_BUFFER_BYTES 8192u

/* IN-009e: mp_end 7 = the session ended because a socket WRITE failed
 * (net_socket_send <= 0 in remote_write_thread). Latched by the write thread
 * at the producer, in the same critical section that sets stop_now and shuts
 * the socket down, so the receive EOF/error that teardown causes (2/3) and
 * the pump's unclassified side-slot latch (6) can no longer record that
 * consequence in place of the original failure. First-wins like every code;
 * a decoded Packet255 keeps its authority and a graceful local quit is
 * never latched as a failure (remote_write_fail_locked). */
#define SERVER_REMOTE_END_SEND 7u
/* Beta PlayerManager.java:31-38 permits radius 15 (31x31 = 961 columns).
 * Its move loop (:121-147) loads before unloading; installed old entries
 * wait for the main-thread unload, so reserve two views plus 126 entries.
 * Measured with devkitPPC/PPC32: 208 B/entry, 425,984 B total, +306,176 B
 * over 576 entries. Compressed bodies retain their independent byte budget.
 * No read-thread wait: a wait would also stall decoding later keep-alives. */
#define REMOTE_CHUNK_ENTRY_CAPACITY 2048
/* Matches SIGN_SIZE/MP_SIGN_LINE_COUNT (item/inventory.h's SIGN_SIZE 64 over
 * mp_codec.h's 4 lines): 16 cells per line in the local sign-edit buffer. */
#define SIGN_EDIT_COLS 16
/* Client window slot for the online sign editor. Beta's GuiEditSign is not a
 * container window, so no server window id exists; 200 is outside the
 * 1..100 range Beta's server hands out (EntityPlayerMP.getNextWindowId) and
 * never crosses the wire (its close flushes Packet130, never Packet101). */
#define REMOTE_SIGN_EDIT_WINDOW 200
/* S3 (entities/remote players): bounded remote-entity table. A real Beta
 * server's own per-client tracked-entity set is unbounded in principle but
 * practically small for a single connected peer; 256 gives generous headroom
 * over "every mob/item/vehicle/other-player near one client" without an
 * unbounded allocation driven purely by network input. Capacity exhaustion
 * drops the new spawn (never crashes/blocks) and is counted. */
#define REMOTE_ENTITY_CAPACITY SERVER_REMOTE_ENTITY_CAPACITY
/* The header's capacity comment promises this assert exists (mirrors
 * REMOTE_QUEUE_CAPACITY's own assert above) -- keep the two names agreeing
 * if either ever decouples. */
_Static_assert(REMOTE_ENTITY_CAPACITY == SERVER_REMOTE_ENTITY_CAPACITY,
			   "entity capacity names must agree");

enum remote_chunk_state {
	REMOTE_CHUNK_UNUSED,
	REMOTE_CHUNK_ANNOUNCED,
	REMOTE_CHUNK_RETAINED,
	REMOTE_CHUNK_PROMOTING,
	REMOTE_CHUNK_RESIDENT,
	/* LEGACY, UNREACHABLE since W2 (design section 2.2). This was the mark
	 * left by the lossy farthest-first eviction that freed a column's only
	 * authoritative copy when the compressed cap was exceeded; protocol 14
	 * has no chunk re-request, so a body for a tombstoned coordinate could
	 * only be answered with a forced reconnect. Astra gate-2 item 5 rejected
	 * that trade outright, and the backing reservation
	 * (MEM2_MP_BACKING_BYTES) refuses cleanly instead -- so nothing assigns
	 * this state any more. Kept as a value so the state numbering, the
	 * existing goldens and tests/test_mp_admission.c's "0 tombstones"
	 * assertion all keep meaning what they meant; delete it only together
	 * with those. */
	REMOTE_CHUNK_TOMBSTONE,
};

/* Chain depth beyond the head body: the TRANSIENT queue, not a per-column
 * limit. Beta applies every Packet51 region to a held chunk in arrival order
 * with no bound (client CFR NetClientHandler.handleMapChunk ->
 * World.setChunkData -> Chunk.setChunkData; server CFR
 * PlayerInstance.java:90-153 sends any number of bounding-box partials per
 * column). IN-009: when a fifth body arrives for a column whose head is a
 * complete column, remote_retain_map_locked FOLDS the head, the queued bodies
 * and the new body into one full-column head (losslessly, in wire order) --
 * see the fold block in server_remote_chunks.c. A head that cannot be folded
 * yet (mid-decode, a partial on installed voxels, or older edits for the
 * column still in the receive queue) is transient backpressure: the read
 * thread waits for the pump. Only a fold that can never happen ends the
 * session (mp_end = SERVER_REMOTE_END_PROTOCOL). */
#define REMOTE_CHUNK_QUEUE_MAX 3

/* F4 MEMORY-AWARE PROMOTION: the two terms of the carve test
 * remote_start_promotion_locked applies before it starts a column. The full
 * derivation, and the betamc_adm_0913 reading that forced it, live at the
 * gate in server_remote_chunks.c; they are declared HERE rather than there
 * so tests/test_mp_admission.c can build the gate's own threshold out of the
 * production constants instead of restating 81,920 and 6 MiB as literals a
 * later tuning pass would silently desynchronise.
 *
 * REMOTE_VOXEL_COLUMN_BYTES -- what one installed column costs the MEM2
 * carve: 8 chunks x 10,240 B (chunk.c's chunk_alloc, tenant CARVE_VOXEL).
 * REMOTE_MESH_RESERVE_BYTES -- carve bytes the gate refuses to let voxels
 * spend, so the mesher can still build display lists for the columns already
 * resident. 0 on PC, which has no carve at all. */
#define REMOTE_VOXEL_COLUMN_BYTES (8u * 10240u) /* == 81,920 */
#ifdef PLATFORM_WII
#define REMOTE_MESH_RESERVE_BYTES (6u * 1024u * 1024u)
#else
#define REMOTE_MESH_RESERVE_BYTES 0u
#endif

struct remote_chunk {
	enum remote_chunk_state state;
	int32_t chunk_x, chunk_z;
	struct mp_map_chunk wire;
	/* World epoch this entry belongs to (remote->world_epoch at creation).
	 * Promotion only installs entries whose epoch the consumer has already
	 * applied (remote->applied_epoch), so a chunk of the NEW dimension that
	 * the read thread retained right after Packet9 can never land in the OLD
	 * world before CRPC_WORLD_RESET swaps it (dimension-change fix 2026-09-11). */
	uint32_t epoch;
	uint64_t arrival;
	bool unload_pending;
	bool reload_after_unload;
	/* ORDERED BODY CHAIN (2026-09-10, MP audit D1; Astra mp1). `wire` is the
	 * head -- the body the promotion pipeline decodes next (RETAINED) or is
	 * decoding right now (PROMOTING, by reference, never swapped out).
	 * `queued[]` holds the LATER Packet51 bodies for this column in receive
	 * order. Beta applies each Packet51's explicit region in order
	 * [740c583 NetClientHandler.handleMapChunk -> World.setChunkData], and a
	 * later partial does NOT replace the rest of the column, so "replace the
	 * body" was only ever right when the new body COVERS the old one's
	 * volume (absolute, half-open containment on all three axes): those are
	 * coalesced on arrival (the covered older body is dropped, the newer one
	 * keeps its own arrival position); everything else is appended. The
	 * in-flight decode always finishes and installs; the chain continues
	 * from queued[0] (state stays RETAINED). Depth REMOTE_CHUNK_QUEUE_MAX +
	 * the head; one more body FOLDS a complete head and its chain into a new
	 * complete head (IN-009, see REMOTE_CHUNK_QUEUE_MAX), so the chain never
	 * drops a region and never ends the session for being long.
	 * Bytes are counted in retained_bytes like the head's. */
	struct mp_map_chunk queued[REMOTE_CHUNK_QUEUE_MAX];
	uint64_t queued_arrival[REMOTE_CHUNK_QUEUE_MAX];
	uint8_t queued_count;
	/* RESIDENCY STAMP (Astra mp2 P1). Packet50 runs on the READ thread the
	 * moment it arrives; Packet52/53 wait for the pump. So an edit received
	 * BEFORE an unload can reach remote_apply_block_change AFTER the unload
	 * already purged the column's parked edits -- and would be parked again
	 * for a residency that has ended (never applied, never purged: it would
	 * sit in the log until the exhaustion path fired for nothing). The
	 * receive stamp of the latest Packet50 LOAD, in the body/edit comparison
	 * space (seq << 16); an edit older than it belongs to a previous
	 * residency and is dropped (counted deferred_edits_dropped, the unload
	 * purge's own counter). An edit for a column with NO entry at all is
	 * dropped too -- NOT because that residency is proven dead: the Beta
	 * server writes Packet50 through its ordinary queue and Packet52 through
	 * its chunk queue [NetworkManager.java:86, Packet50PreChunk.java:17,
	 * Packet52MultiBlockChange.java:20 (server decompile)], so a block change
	 * CAN overtake the load that announces its column. Dropping it is still
	 * the right outcome here: the full Packet51 that follows the load carries
	 * the server's state INCLUDING that change, and this client has no chunk
	 * storage to apply it to before then. Exact client-side parity of that
	 * corner is unverified (client decompile unavailable to the reviewer). */
	uint64_t residency_seq;
	/* NEWEST APPLIED STAMP (W1 demotion, design section 2.5). The highest
	 * stamp -- in the shared body/edit comparison space -- of anything this
	 * client has actually APPLIED to this column's installed voxels: the
	 * arrival of the last body remote_finish_promotion_locked published, and
	 * the arrival of every edit applied on top of it (live through
	 * remote_apply_block_change, or replayed by the deferred drain). A
	 * demotion re-encodes those voxels into a new head body and MUST stamp
	 * it max(chunk->arrival, newest_applied): a lower stamp would let the
	 * drain replay edits the re-encode already contains (double-apply), a
	 * higher one would shadow genuinely newer edits. Cleared whenever the
	 * column's voxels go away (unload, teardown). */
	uint64_t newest_applied;
	/* VOXELS EXIST (W2). True from the moment a promotion publishes this
	 * column's blocks to the client until they are taken away again
	 * (demotion, or the pump's unload emit). It is NOT the same as
	 * `state == REMOTE_CHUNK_RESIDENT`: a body arriving for an already
	 * installed column moves the entry back to RETAINED (the new body is
	 * pending) while the voxels stay installed, and a RESIDENT-only
	 * demotion precondition therefore left those columns holding 81,920 B
	 * of carve forever when they were out of the admission radius -- the
	 * residual W1 recorded in its own commit message. Demotion keys off
	 * THIS instead, and the re-encode preserves the pending body by
	 * prepending itself ahead of it (remote_demote_one_locked). */
	bool installed;
	/* SYNTHETIC BASE (REL-MP-FIX2a): the head was folded onto the implicit
	 * Beta pre-chunk [ChunkProviderClient.java:40] because no full Packet51
	 * had arrived, stamped at residency_seq so parked edits outside its
	 * regions stay newer. It may install (Beta renders a prepared chunk,
	 * ChunkProviderClient.java:39-41) but never finishes loading; a later
	 * full body replaces it. Cleared with the head (drop_body). */
	bool synthetic_base;
	/* REL-MP-FIX2b: the INSTALLED voxels hold no genuine full snapshot (a
	 * synthetic head, or a partial expanded onto the pre-chunk). A demotion
	 * hands it back to its head; cleared when `installed` clears. */
	bool voxels_synthetic;
	/* REL-MP-JOINSPEED: this column's parked edits -- 1-based pool indices
	 * of the oldest and the newest (0 = none) and how many (see the block
	 * after struct remote_deferred_edit). */
	uint16_t edit_head, edit_tail;
	uint16_t edit_count;
};

struct remote_promotion {
	bool active;
	bool ready_to_publish;
	size_t entry;
	z_stream stream;
	bool stream_initialized;
	uint8_t* inflated;
	struct chunk_rpc_slot* slot;
	uint8_t overflow_byte;
};

/* Bounded per-coordinate deferred-edit log (2026-09-01, Plan section 7): a
 * Packet52/53 or explosion block-clear that lands against a chunk not yet
 * REMOTE_CHUNK_RESIDENT used to be discarded PERMANENTLY here
 * (remote_apply_block_change's old deferred_edits++-and-return -- see that
 * function's header comment in server_remote_chunks.c for the defect this
 * replaces). Retained instead, keyed by absolute block coordinate so a
 * resend for the same cell supersedes the earlier value (last-write-wins,
 * mirroring Fix A's pending_wire park above for whole-chunk resends);
 * remote_finish_promotion_locked (server_remote_chunks.c) drains every entry
 * for a chunk the moment that chunk's own promotion completes, applying each
 * through the same live CRPC_SET_BLOCK path remote_apply_block_change already
 * uses for a resident chunk (identical relight/dig-particle behaviour, no
 * duplicated nibble-packing logic), immediately after that chunk's CRPC_CHUNK
 * send -- both calls are synchronous on this same (main/pump) thread with no
 * yield between them, so no renderer/mesher ever observes the pre-edit base
 * volume.
 *
 * Global (not one array per chunk entry) because deferred edits are rare by
 * design -- they only exist for the brief window between "server thinks a
 * chunk is loaded" and "this client's own promotion pipeline has finished
 * decoding it" -- so one shared bounded pool is simpler and more memory-
 * honest than REMOTE_CHUNK_ENTRY_CAPACITY (576) separate small arrays. 256
 * gives generous headroom over an ordinary Packet52 multi-block burst; even a
 * pathological MP_EXPLOSION_MAX_BLOCKS (4096) explosion landing entirely on
 * one not-yet-resident chunk overflows it, but protocol 14 has no client
 * chunk re-request (remote_retain_map_locked's own TOMBSTONE-case comment),
 * so there is no "force a restream" fallback available -- the honest
 * recourse is drop-oldest, counted (deferred_edits_dropped), never a silent
 * loss. */
/* REL-MP-JOINSPEED (2026-10-22): 1024 -> 16384, stored in its own MEM2
 * scratch slot (MP_BACKING_SCRATCH_EDITS, 16 B per entry = 256 KiB) and
 * indexed per column -- see the block after struct remote_deferred_edit. */
#define REMOTE_DEFERRED_EDIT_CAPACITY 16384
/* 2026-09-10 (MP audit D3, Astra mp1 F3): 256 -> 1024, and the drop-oldest
 * described above is GONE -- an overwritten entry is authoritative terrain
 * lost for the rest of the session. Exhaustion now (1) reclaims every parked
 * edit already superseded by a newer queued body covering its cell, then
 * (2) if still full, terminates the connection visibly through the same
 * terminal path a bad packet takes ("remote deferred edit log exhausted"),
 * so the user reconnects to correct terrain instead of playing on wrong
 * terrain. 1024 * 24 B = 24 KB, PC-only mainline (MP is not on the Wii). */
/* W2 (design section 2.5, Astra gate-2 item 7): the log is bounded WITHOUT
 * losing state, so termination is no longer the first answer to pressure.
 * Once more than this many edits are parked, each pump COMPACTS the backed
 * column holding the most of them -- inflate its head, apply its parked
 * edits in stamp order, deflate back into a new head, free those log
 * entries. The "log exhausted -> kill" path below stays as the LAST resort,
 * now only reachable when a single column parks more than the whole capacity.
 *
 * F5a (review finding M8): 768 -> 512, one compaction per pump ->
 * REMOTE_COMPACTIONS_PER_PUMP, and -- the part that actually removes the
 * cliff -- the exhaustion path itself now compacts before it kills (see
 * remote_deferred_edit_park_locked). W2's claim that the kill was "only
 * reachable if one column alone parks > 1024 edits" did not hold for a BROAD
 * burst: with edits spread over hundreds of columns, one compaction per pump
 * frees roughly one column's worth of slots while the log fills faster, and
 * whether the pump even runs between two edits is not something the read
 * thread controls. 512 = half of capacity gives the pump twice the runway,
 * two compactions per pump double its rate under pressure, and the
 * compact-before-kill retry makes the guarantee structural instead of a
 * race: the log can only be exhausted when NO backed column has parked
 * edits left to fold, which is exactly the single-column case. */
/* REL-MP-JOINSPEED: 512 -> 3/4 of the larger log (see the block after
 * struct remote_deferred_edit for why the old cap made compaction O(edits)). */
#define REMOTE_DEFERRED_EDIT_HIGH_WATER 12288
_Static_assert(REMOTE_DEFERRED_EDIT_HIGH_WATER
				   < REMOTE_DEFERRED_EDIT_CAPACITY,
			   "the compaction trigger must leave slots to compact into");
/* REL-MP-FIX3a: once the log passes HIGH_WATER the pressure LATCHES
 * (deferred_pressure) and the pump keeps compacting, still at most
 * REMOTE_COMPACTIONS_PER_PUMP per pump, until the log is at or below this
 * mark. Without it the pump hovered at 512, folding ~1 slot per compaction
 * forever (tmp/jobs/triage-1020/walk-diag.md: 1,263 in 15 s on vanilla). */
#define REMOTE_DEFERRED_EDIT_LOW_WATER 8192 /* REL-MP-JOINSPEED: 256 -> 1/2 */
_Static_assert(REMOTE_DEFERRED_EDIT_LOW_WATER < REMOTE_DEFERRED_EDIT_HIGH_WATER,
			   "hysteresis needs a gap between the marks");
/* Compactions one pump may run while the log is over the high-water mark.
 * Bounded for the same reason the demotion is one-per-pump: each is an
 * inflate + re-deflate of one column (~1.5 ms measured on PC, the dominant
 * cost in remote_compact_one_locked), and the pump runs on the main thread. */
#define REMOTE_COMPACTIONS_PER_PUMP 2u

/* THE HARD RESIDENT CEILING (F5b, review finding H1), in columns, as a
 * function of the live admission radius. (2R+3)^2 == the (R+1) hysteresis
 * disk: remote_residency_step_locked demotes everything past admit_radius+1,
 * so that disk is the largest set that can legitimately hold voxels at once,
 * and it is therefore the honest bound rather than the (2R+1)^2 the status
 * doc quoted (which is only reachable with a centre that never moved).
 * remote_start_promotion_locked refuses to START a promotion above it,
 * INDEPENDENTLY of the carve gate and of whether a centre is known at all --
 * that independence is the whole point: H1's 242-column peak happened with
 * the carve gate in place and with a perfectly valid position. */
#define REMOTE_RESIDENT_COLUMN_CAP(radius) \
	((size_t)(2 * (radius) + 3) * (size_t)(2 * (radius) + 3))
/* Consecutive pumps the decoded-event ring may veto a demotion before the
 * residency step demotes anyway (F5b, H1: an unbounded veto let a busy edit
 * stream pin the resident set at the cap). 8 pumps is ~0.13 s at 60 Hz --
 * long enough that an ordinary burst still drains first (one pass' event
 * budget is 4x the ring, so the ring is normally empty at the pump tail),
 * short enough that no stream can hold the carve hostage. */
#define REMOTE_RESIDENCY_VETO_PUMP_LIMIT 8u

struct remote_deferred_edit {
	uint64_t arrival; /* Shares remote->arrival_sequence's counter space;
	                    * only relative order among log entries matters. */
	/* REL-MP-JOINSPEED: 1-based chunks[] slot of the column that parks this
	 * edit (0 = a free entry), the 1-based pool index of that column's next
	 * edit (or of the next free entry), and the cell inside the column in
	 * Packet51 index order: (x & 15) << 11 | (z & 15) << 7 | y. The absolute
	 * x/z are the owner column's; remote_deferred_edit_x/_z read them. */
	uint16_t owner;
	uint16_t next;
	uint16_t cell;
	uint8_t type, metadata;
};
_Static_assert(sizeof(struct remote_deferred_edit) == 16,
			   "the edit pool's MEM2 budget is sized at 16 B per entry");
_Static_assert(REMOTE_DEFERRED_EDIT_CAPACITY <= 65535
				   && REMOTE_CHUNK_ENTRY_CAPACITY <= 65535,
			   "1-based uint16_t pool and owner indices");
/* REL-MP-JOINSPEED (2026-10-22): WHY THE LOG GREW AND WHY IT IS INDEXED.
 *
 * A FRESH vanilla Beta 1.7.3 world floods block changes onto announced
 * columns long before their full Packet51 arrives: the server subscribes the
 * player to all (2V+1)^2 columns at once [PlayerInstance.java:62-69, Packet50]
 * and forwards every block AND light change of each one [World.setLightValue
 * -> markBlockNeedsUpdate; PlayerInstance.onUpdate:122-166] while it streams
 * at most one full column per tick and only with fewer than 4 chunk packets
 * queued [EntityPlayerMP.java:131-142]. The recorded join
 * (tests/fixtures/mp_join_vanilla/stream.bin) parks ~15,000 distinct cells
 * at its peak at the Wii radius 4, and most of them are DISCARDED for free
 * when the column's full body arrives (remote_retain_map_locked's supersede:
 * a full Packet51 overwrites every id, metadata and light nibble of the
 * column [World.setChunkData -> Chunk.setChunkData:548-591]).
 *
 * At 1,024 entries the log sat at capacity for the whole join, so EVERY
 * further distinct edit ran the exhaustion rescue, whose victim rule (full
 * heads first) folds a backed column that may hold ONE edit: one 81,920-byte
 * inflate + level-6 deflate per incoming edit, under impl->lock, on the main
 * thread -- 2,800-10,400 compactions per join on the Wii, 1-2 fps, a starved
 * mesher and a 40 s loading screen (tmp/jobs/join-regress-1022). The pump's
 * own latch added two more per frame.
 *
 * Now the log holds a whole fresh-world join, so parked edits wait for the
 * body that supersedes them (or for the promotion that drains them, once),
 * and compaction is back to what it was meant to be: rare relief, folding the
 * column that holds the MOST edits, with its deflate outside impl->lock.
 * Every operation is per column: each remote_chunk owns a chain of its edits
 * through `next`, kept in ascending `arrival` order (an update of a parked
 * cell moves it to its new place), so find / supersede / drain / purge / fold
 * cost that column's edits, never a scan of the pool, and the drain's
 * oldest-first order is simply the chain order. */

/* S3: what a tracked remote entity id maps to, driving which CRPC(s) its
 * later move/destroy packets translate to. UNSUPPORTED covers wire types
 * this engine has no client render/CRPC for (the generic Mob/Monster/
 * Giant mob ids) -- the
 * id is still tracked so later DESTROY/move packets about it are silently
 * absorbed instead of erroring, exactly like a real Beta client's
 * `if (entity != null)` guards. */
enum remote_entity_kind {
	REMOTE_ENTITY_PLAYER,
	REMOTE_ENTITY_MOB,
	REMOTE_ENTITY_ITEM,
	REMOTE_ENTITY_MINECART,
	REMOTE_ENTITY_BOAT,
	REMOTE_ENTITY_PROJECTILE, /* arrow/egg/snowball/fireball/fishing hook
							   * -> CRPC_SPAWN_ARROW */
	REMOTE_ENTITY_PRIMED_TNT,
	/* Painting slice MP upgrade: Packet25EntityPainting is a dedicated spawn
	 * packet (not routed through MP_EVENT_VEHICLE_SPAWN's vtype switch like
	 * the kinds above), decoded to MP_EVENT_ENTITY_PAINTING (mp_codec.c) and
	 * consumed in server_remote_protocol.c. No later move/correction packet
	 * exists for it in Beta (a painting never moves once placed), so this
	 * kind is spawn+destroy only, like REMOTE_ENTITY_ITEM/PROJECTILE. */
	REMOTE_ENTITY_PAINTING,
	/* Card m3: Packet23 types 70/71 [NetClientHandler.java:109-116] ->
	 * CRPC_SPAWN_FALLING_SAND; moves/velocity/destroy like the other
	 * Packet23 kinds. */
	REMOTE_ENTITY_FALLING_SAND,
	REMOTE_ENTITY_UNSUPPORTED,
};

struct remote_entity {
	bool used;
	int32_t id;
	enum remote_entity_kind kind;
	/* Beta Entity.serverPosX/Y/Z: fixed-point, 1/32-block integer units.
	 * Relative-move packets ADD signed byte deltas directly to these ints (no
	 * float drift, exactly Beta's own accumulation); absolute packets
	 * (spawn/teleport) replace them outright. [740c583
	 * NetClientHandler.handleEntity / handleEntityTeleport] */
	int32_t server_x, server_y, server_z;
	float yaw, pitch; /* degrees; last absolute value received */
	/* REMOTE_ENTITY_MOB only: the engine monster_id this entity was spawned
	 * as, plus the small set of CRPC_ENTITY_MOVE-carried cosmetic flags this
	 * table caches so a later standalone Packet40 (or another relative move)
	 * republishes the LATEST known values instead of zeroing them out. */
	int monster_id;
	uint8_t fleece_color;
	bool sheared, saddled, charged, ghast_attacking;
	uint8_t wolf_flags;
	int32_t wolf_health;
	/* Packet38EntityStatus cosmetics [740c583 EntityLiving.
	 * handleHealthUpdate]: status 2 arms the 10-tick hurt flash (hurtTime
	 * = maxHurtTime = 10 -> 500ms at 20 TPS; wall-clock here because the
	 * remote layer has no game-tick pass -- the pump-tail sweep clears
	 * it); status 3 starts the client-run death roll (Beta's client
	 * advances deathTime locally after onDeath; SP streams the
	 * progression instead, so for MP this table drives it: elapsed/50ms
	 * + 1, capped at Beta's 20 [EntityLiving.onEntityUpdate] == combat.h
	 * mob_death_finished's constant). */
	bool hurt_active, death_active;
	ptime_t hurt_until, death_started;
	int8_t death_time_sent;
	/* Residual item 4 (2026-08-26): a one-shot latch for the death SOUND,
	 * DELIBERATELY separate from death_active above -- that flag also drives
	 * flush_entity_status's kind-unguarded pump-tail sweep (the MOB-only
	 * death-ROLL progression), so reusing it to gate a PLAYER's one-shot
	 * death sound would make that sweep advance/republish a player entry too
	 * (harmless but undocumented cross-contamination). Set once on the first
	 * status==3 arrival, for either kind; never cleared (a corpse is
	 * destroyed shortly after, never revived in place). */
	bool death_sound_played;
};

struct remote_entity_table {
	struct remote_entity entries[REMOTE_ENTITY_CAPACITY];
	size_t count; /* live (used) entries, for F3/tests */
	uint32_t capacity_drops; /* spawns rejected because the table was full */
};

enum remote_event_kind {
	REMOTE_EVENT_CONNECTED,
	REMOTE_EVENT_PACKET,
	REMOTE_EVENT_ERROR,
};

struct remote_event {
	enum remote_event_kind kind;
	uint32_t generation;
	/* remote->world_epoch when the read thread queued this event: the RESPAWN
	 * handler applies it (applied_epoch) so promotion unblocks the new world. */
	uint32_t world_epoch;
	struct mp_event packet;
	char error[256];
	/* RECEIVE-ORDER STAMP (2026-09-10, MP audit D2; Astra mp1 blocking
	 * prerequisite). Assigned on the READ thread, under remote->lock, from
	 * the same counter that stamps a Packet51 body the moment it is retained
	 * (remote->arrival_sequence), so a queued edit and a retained snapshot
	 * compare in WIRE order even though the edit is only applied when the
	 * pump reaches it. A Packet52/53/explosion edit's arrival is
	 * (seq << 16) | index-within-packet; a body's is (seq << 16). Never
	 * re-stamped on retry, adoption or promotion. */
	uint64_t seq;
};

struct remote_frame {
	uint32_t generation;
	struct mp_frame frame;
	bool correction_echo;
};

struct remote_trace {
	bool transmit;
	uint32_t generation;
	uint8_t bytes[MP_S0_FRAME_CAPACITY];
	size_t length;
};

/* IN-009 LOSSLESS CHAIN FOLDING state (server_remote_chunks.c), embedded
 * ONCE in server_remote_impl as `fold`. The whole persistent MEM1 cost of
 * the fold is this struct: REMOTE_FOLD_STATE_BYTES, no per-column fields and
 * no new buffers (tmp/jobs/loop/IN-009-mem.md has the full accounting).
 *   folds: complete head + queued bodies + new body re-encoded as one
 *     complete head.
 *   waits: retains that had to wait for the pump first (transient
 *     backpressure, never a refusal).
 *   zlib_alloc_failures: inflateInit/inflate/deflateInit returned
 *     Z_MEM_ERROR during a fold -- counted separately so an allocation
 *     failure is never reported as a generic timeout.
 *   wait_x/wait_z/wait_active: the column the read thread is waiting on
 *     right now; remote_start_promotion_locked promotes it first when its
 *     voxels are already installed, so the wait always progresses.
 *   wait_cause: enum remote_fold_wait_cause of the LAST wait pass -- what
 *     the terminal reason names if the wait expires.
 * Read-thread writes, pump reads, both under impl->lock. */
enum remote_fold_wait_cause {
	REMOTE_FOLD_WAIT_NONE = 0,
	REMOTE_FOLD_WAIT_HEAD_BUSY,   /* a promotion owns the head */
	REMOTE_FOLD_WAIT_PARTIAL_HEAD, /* partial head awaiting its install */
	REMOTE_FOLD_WAIT_EDIT_ORDER,  /* older Packet52/53/60 still queued */
	REMOTE_FOLD_WAIT_SCRATCH,     /* recode scratch owned by a demotion */
	REMOTE_FOLD_WAIT_ZLIB_ALLOC,  /* zlib Z_MEM_ERROR (allocation) */
};
struct remote_fold_state {
	uint32_t folds;
	uint32_t waits;
	uint32_t zlib_alloc_failures;
	int32_t wait_x, wait_z;
	bool wait_active;
	uint8_t wait_cause;
};
/* 5 x 4-byte words + bool + uint8_t + 2 bytes tail padding (4-byte
 * alignment) = 24 bytes on both the PC (x86-64 MinGW/GCC) and the Wii
 * (PowerPC EABI, sizeof(bool) == 1) ABIs. Embedded between two uint32_t/int
 * members of server_remote_impl, so it adds no padding of its own there:
 * the impl grows by exactly this many bytes. */
#define REMOTE_FOLD_STATE_BYTES 24u
_Static_assert(sizeof(struct remote_fold_state) == REMOTE_FOLD_STATE_BYTES,
			   "IN-009 fold state MEM1 cost changed: update "
			   "tmp/jobs/loop/IN-009-mem.md");

struct server_remote_impl {
	uint8_t active_window; // cursor Packet103(-1,-1) belongs to this Container
	struct thread_mutex lock;
	/* REL-MP-FIX3e: the outgoing ring's own short-held lock, as Beta's
	 * NetworkManager guards its send queue with sendQueueLock, never the
	 * world (NetworkManager.java:18, push 61-68, writer pops 85/99), so a
	 * chunk compaction holding `lock` can no longer starve pop and send.
	 * LOCK ORDER: `lock` may take frame_lock; a frame_lock holder never takes
	 * `lock` or any other lock, never sends, never allocates and never does
	 * file I/O (traces and dumps snapshot under it, then write). frame_lock
	 * guards frames/frame_head/frame_count, traces/trace_*, keep_alives_sent,
	 * corrections_echoed and the w_* writer view below; write_signal is
	 * waited on under frame_lock. */
	struct thread_mutex frame_lock;
	struct thread_cond write_signal;
	/* Writer view, set by `lock` holders with frame_lock nested: w_socket and
	 * w_generation mirror socket/generation; w_stop is stop_now as of the
	 * last close_socket_locked (every stop_now site calls it); w_graceful
	 * mirrors graceful_stop. */
	net_socket_t w_socket;
	uint32_t w_generation;
	bool w_stop, w_graceful;
	/* S5.3 hunt PROBE (temporary, ROUND 10): the coordinator's discriminator
	 * -- hold the login SEND (not just receipt) for ~3s after the handshake
	 * reply, so the read/write threads' concurrent IOS-socket burst around
	 * the login round-trip lands at a LATER frame instead of frame 4. If the
	 * mp_connect_smoke wall moves with it, the killer is the concurrency
	 * spike itself, not frame 4's own tick/render work; if the wall stays at
	 * frame 4 regardless, it rules that out. See server_remote_protocol.c's
	 * MP_EVENT_HANDSHAKE case (sets these instead of sending immediately)
	 * and server_remote.c's server_remote_pump (polls and fires when due --
	 * process_event only runs per-DEQUEUED-event, so the deadline needs a
	 * per-frame check, not a per-event one). */
	bool login_delay_pending;
	ptime_t login_delay_deadline;
	/* Read-thread decode scratch (S5.3 stack-gate fix, 2026-08-27):
	 * mp_decoder (~8.9K) + an mp_event (~8.2K) + a remote_event staging
	 * copy (~8.2K) used to live ON the read thread's 64KB LWP stack -- a
	 * measured 34,712-byte frame (tools/check_wii_stack_usage.sh FAIL,
	 * empty-allowlist discipline) whose overflow trampled adjacent heap:
	 * the shifting-victim silent lockups of the S5.3 mp_connect hunt
	 * (takes 6-14: death moved between call transitions per build, no
	 * watchdog/fault dump ever fired). Heap-resident here (impl is
	 * calloc'd). EXCLUSIVE to the read thread within a generation
	 * (read_event only ever touched under `lock`); mp_decoder_init
	 * re-arms read_decoder at each read thread's start. */
	struct mp_decoder read_decoder;
	struct mp_event read_packet;
	struct remote_event read_event;
	/* Socket drain buffer -- ONE recv() per IOS transfer.
	 *
	 * ROOT CAUSE, mp_lan_walk_10m / mp_reconnect_soak "world never reported
	 * loaded" on real Wii (2026-09-02, tmp/wii_runs/staged_mp_lan_walk_10m_
	 * 20260902_065212 + _073825 + staged_mp_reconnect_soak_20260902_075109):
	 * this used to be a 256-BYTE stack scratch in remote_read_thread, so the
	 * client drained mc173's join burst 256 bytes at a time -- ~800 IOS
	 * network-IPC round trips (plus 1600 impl->lock acquisitions for the
	 * per-iteration stop_now check) for the ~200 KB the server writes in ONE
	 * tick.
	 *
	 * mc173 cannot survive a client that slow. handle_player_join ->
	 * update_chunks (mc173-server/src/player.rs:1414-1443) marks all 36
	 * columns of its 6x6 window tracked BEFORE sending -- `if
	 * self.tracked_chunks.insert((cx, cz)) { self.send(...) }`, insert is the
	 * condition -- and the send goes straight to a NON-BLOCKING mio TcpStream
	 * with no userspace buffer and the I/O result thrown away
	 * (mc173-server/src/net.rs:479-485: `let _ = packet.write(&mut &*stream)`,
	 * commented "we ignore I/O errors"). WRITABLE readiness is registered but
	 * never handled (net.rs:326-348), so an interrupted write is never
	 * resumed. Once the Wii's receive window fills, every remaining column in
	 * that loop is silently discarded while still marked delivered, and
	 * update_chunks -- the ONLY re-offer path, driven by a position-bearing
	 * client packet (player.rs:264) -- will never send it again. There is no
	 * chunk re-request in protocol 14 and mc173 has no un-track path.
	 *
	 * The hardware signature is that cut, exactly: mp_ret_n PINNED at 15 (the
	 * first 15 columns of mc173's `for cx in ocx-3..ocx+2 { for cz in ... }`
	 * scan order) across three runs on two builds, mp_rx_kb 92 at the first
	 * heartbeat vs 197 on the 2026-08-31 PASS that got all 36. The player's
	 * OWN column (0,0 -- mc173 config::SPAWN_POS is (0,100,0), and the
	 * telemetry confirms the pre-world position install: pos=(0,100,0)) is
	 * index 18 of that 36-entry scan, i.e. just past the cut -- so
	 * remote_finish_promotion_locked's spawn-column gate
	 * (server_remote_chunks.c) waited 480 s for a column the server had
	 * already written off. NOT eviction: mp_evict=0 and mp_alloc_fail=0 all
	 * run, with an 87 KB retained peak against REMOTE_RETAINED_BYTE_CAP's
	 * 2 MB and 15 entries against REMOTE_CHUNK_ENTRY_CAPACITY's 576.
	 *
	 * 8192 == wii/net_socket.c's NET_IOS_MAX_CHUNK, the largest single IOS
	 * transfer that backend will issue (its own cap exists so libogc's
	 * multi-chunk net_recvfrom loop stays single-iteration). One buffer fill
	 * is therefore one IPC: the same ~200 KB burst is now ~25 round trips
	 * instead of ~800. Heap-resident for the same reason the decode trio
	 * above is -- the read thread's LWP stack is 64 KB and already carries a
	 * MP_S0_FRAME_CAPACITY trace staging array.
	 *
	 * BIT-EXACT by construction: the loop body feeds mp_decoder_feed ONE byte
	 * at a time out of this buffer, so the decoded event stream cannot depend
	 * on where recv() happened to split the socket stream -- and the loopback
	 * suite already proves that independently, every fixture writing through
	 * socket_write_fragmented (tests/test_mp_s0_env.c). */
	uint8_t read_receive_buffer[REMOTE_READ_BUFFER_BYTES];
	/* Take-50 world-install probe (DIAGNOSTIC): raw rx byte counter, single
	 * writer (read thread), sampled lock-free by get_status.
	 * Fix B (2026-08-31, correctness-milestone): was `volatile uint32_t`,
	 * written by a bare `+=` on the read thread (server_remote_transport.c)
	 * and read unsynchronized-with-the-writer by get_status
	 * (server_remote.c) -- `volatile` gives NO atomicity or cross-thread
	 * ordering guarantee in the C standard (it only blocks certain
	 * compiler reorderings/eliminations against a single thread's own
	 * accesses), so this was genuine concurrent read+write UB (ISO C
	 * 5.1.2.4), not merely a "torn read" as the old comment characterized
	 * it. Widened to a proper C11 atomic: atomic_fetch_add_explicit
	 * (relaxed) on the write side, atomic_load_explicit (relaxed) on the
	 * read side -- relaxed is sufficient because nothing else ever
	 * synchronizes against this counter (it is read-only diagnostics, same
	 * as before); this only makes the access itself well-defined, it does
	 * not add any new ordering/visibility semantics or change the counted
	 * value. */
	atomic_uint rx_bytes;
	/* Fix B companion (2026-08-31): read-thread-owned decoder-phase
	 * DIAGNOSTIC mirror. get_status (server_remote.c) used to read
	 * impl->read_decoder.phase/packet_id/chunk_have/chunk_need directly --
	 * plain, non-atomic fields that mp_decoder_feed (mp_codec.c) mutates
	 * continuously from the read thread with NO lock held (the read
	 * thread only takes remote->lock around a completed MP_DECODE_EVENT,
	 * not around each byte fed to the decoder) -- the same unsynchronized-
	 * concurrent-access UB class as rx_bytes above, "torn reads acceptable,
	 * diagnostic only" notwithstanding.
	 * struct mp_decoder itself (mp_codec.h) stays untouched -- it is
	 * shared by the single-threaded codec golden tests
	 * (tests/test_mp_s0_env.c) and mp_codec.c's own internal state-machine
	 * logic switches on these fields throughout; retyping them there would
	 * ripple into code that has no threads to race with. These four fields
	 * are instead a DEDICATED atomic copy, updated (relaxed atomic store)
	 * by the read thread immediately after every mp_decoder_feed call
	 * (server_remote_transport.c) and sampled (relaxed atomic load) by
	 * get_status -- same live-tracking freshness as before, now with a
	 * defined cross-thread access on both sides. Widened to uint32_t to
	 * match struct server_remote_status's existing dec_chunk_have/
	 * dec_chunk_need fields (server_remote.h) and the (uint32_t) cast the
	 * old lock-free read already applied to mp_decoder's size_t chunk_*
	 * fields -- no truncation-behavior change from before. */
	atomic_uint dec_phase_sample;
	atomic_uint dec_packet_id_sample;
	atomic_uint dec_chunk_have_sample;
	atomic_uint dec_chunk_need_sample;
#ifdef PLATFORM_WII
	/* S5.3 hunt PROBE (temporary, ROUND 6): guard canary immediately after
	 * the read-thread scratch trio above -- catches an overflow FROM
	 * read_decoder/read_packet/read_event spilling into the rest of this
	 * heap-allocated struct (impl is calloc'd; see server_remote_init).
	 * Painted once right after calloc, scanned from the read thread's own
	 * marker sites (cw_decode_trace/cw_lock_trace call sites) via
	 * cw_read_canary_scan below -- see cw_canary.h for the design. */
	struct cw_canary read_scratch_canary;
#endif
	bool synchronization_ready;
	bool platform_ready;

	struct thread read_thread;
	struct thread write_thread;
	bool read_thread_started;
	bool write_thread_started;
	bool read_thread_exited;
	bool write_thread_exited;
	bool stop_now;
	bool graceful_stop;
	/* Authoritative-kick latches (mp-split-0912/5). A server that kicks
	 * normally sends Packet255 and then CLOSES -- the read thread decodes the
	 * kick, queues MP_EVENT_DISCONNECT, and its very next
	 * net_socket_receive returns 0 (FIN) or -1 (RST). Before these latches
	 * existed that follow-on close looked UNEXPECTED to the read loop
	 * (stop_now/graceful_stop are only set later, by the pump that applies
	 * the kick), so it published "remote closed the socket" / "socket read
	 * failed (...)" into the error side-slot; the pump then drained the
	 * disconnect (reason = the server's text, state = REMOTE_KICKED) and ran
	 * the side-slot AFTER it, whose handler unconditionally overwrote the
	 * reason and demoted the state to REMOTE_CLOSING. The player was shown
	 * the transport's EOF string instead of why they were kicked.
	 *
	 * terminal_reason_pending: set by the READ THREAD (under the lock) once a
	 * decoded MP_EVENT_DISCONNECT has been successfully queued. It makes any
	 * following EOF/negative receive EXPECTED (no error publish) and ends the
	 * read loop -- the session is over, nothing after the kick is meaningful.
	 * reason_from_disconnect: set by the PUMP (process_event's
	 * MP_EVENT_DISCONNECT case) once the decoded reason is installed. It is
	 * the second, independent guard: the error side-slot handler refuses to
	 * overwrite the reason/state of a session whose reason came from a kick,
	 * whatever raced into the slot first. Both are required -- the first stops
	 * the stale error from ever being published in the common case, the second
	 * makes a stale error that got in anyway (published BEFORE the kick
	 * decoded, or by another thread) harmless.
	 * errors_dropped_after_kick: how many error events the second guard
	 * dropped. Never silent -- it is on the status struct and the F3/heartbeat
	 * path so a dropped diagnostic is visible rather than vanished. */
	bool terminal_reason_pending;
	bool reason_from_disconnect;
	uint32_t errors_dropped_after_kick;
	/* Terminal-event latch for unattended runs (mp-split-0912/5, Astra
	 * gate-1 item 2). First-wins snapshot of HOW this connection generation
	 * ended, surfaced on the heartbeat as mp_end/mp_end_phase/mp_end_rx so a
	 * hardware run that ends in a disconnect says WHY without a screenshot.
	 * mp_end: 0 none, 1 kicked-by-Packet255, 2 remote-eof (recv == 0),
	 * 3 receive-error (recv < 0), 4 connect-error, 5 capacity
	 * (SERVER_REMOTE_END_CAPACITY), 6 protocol/internal failure
	 * (SERVER_REMOTE_END_PROTOCOL: invalid/undecodable body, column-crossing
	 * region, exhausted coordinate table, an unfoldable body chain, or an
	 * unclassified error side-slot failure -- remote_latch_protocol_end_locked),
 * 7 send-error (SERVER_REMOTE_END_SEND: net_socket_send failed, latched by
 * the write thread at the failure -- remote_write_fail_locked).
	 * mp_end_phase: the enum server_remote_state value the diagnostic
	 * message's phase word is derived from ("play"/"login"/"handshake"), so
	 * the number and the text can never disagree.
	 * mp_end_rx: the EXACT rx_bytes count at the terminal event (not the
	 * KB-rounded mp_rx_kb) -- the difference between "kicked after a full
	 * world install" and "died 21 bytes in". Cleared by server_remote_start,
	 * so it describes the CURRENT generation and stays readable for the whole
	 * teardown/reconnect window. */
	uint32_t mp_end;
	uint32_t mp_end_phase;
	uint32_t mp_end_rx;

	net_socket_t socket;
	/* fd parked by close_socket_locked, awaiting net_socket_close AFTER
	 * join_workers. See server_remote_transport.c's "Socket teardown" block
	 * comment for the invariant this exists to enforce -- in short, the fd
	 * NUMBER must not be released to the OS while either worker thread may
	 * still hold an unlocked thread-local copy of it. Lock-guarded like
	 * `socket`; NET_SOCKET_INVALID when nothing is parked (note that a
	 * calloc'd 0 is a VALID fd, so server_remote_init sets this
	 * explicitly). At most one fd per session ever lands here. */
	net_socket_t pending_close_socket;
	char host[256];
	uint16_t port;
	/* Connection-immutable: written ONLY by server_remote_start, under the
	 * lock and BEFORE thread_create publishes it to the workers (that call
	 * is the happens-before edge). start() refuses unless state ==
	 * REMOTE_DISCONNECTED with both threads joined, so the next write cannot
	 * overlap a live connection. The handshake/login encoders therefore read
	 * it unlocked from server_remote_pump -- the MAIN thread, the same one
	 * that would perform the next write. Reviewed 2026-08-16: this is a
	 * deliberate lock-free read of an effectively-immutable field, not a
	 * missing lock; do not "fix" it by taking the mutex. */
	char username[65];
	uint32_t generation;
	enum server_remote_state state;

	struct remote_event events[REMOTE_QUEUE_CAPACITY];
	size_t event_head;
	size_t event_count;
	/* REL-MP-FIX5: the read thread parks here (under `lock`, never with
	 * frame_lock held) while the ring is full. Signalled under `lock` by the
	 * pump's pop, close_socket_locked (every stop_now site),
	 * drain_event_queue_locked and each generation bump. */
	struct thread_cond event_space;
	/* Error SIDE-SLOT (2026-08-20 redesign, hostile-review finding 4):
	 * publish_error may be called FROM the pump thread (correction
	 * validation, remote_try_crpc's failure tail, protocol-state branches),
	 * and push_event_locked's backpressure loop waits on a drain that only
	 * the pump thread performs -- an error published from the pump against
	 * a full queue would wait on itself (REL-MP-FIX5 removed the old 500ms
	 * overflow kill that used to overwrite the true reason).
	 * Errors therefore bypass the ring entirely: first-error-wins slot,
	 * set under the lock with zero waiting from ANY thread; the pump
	 * synthesizes the ERROR event from it after each drain pass. A
	 * terminal error may thus be observed ahead of queued tail events --
	 * same net effect as the old overflow-kill, but the true reason
	 * survives. */
	bool error_pending;
	uint32_t error_pending_generation;
	char error_pending_msg[128];
	struct remote_frame frames[REMOTE_QUEUE_CAPACITY];
	size_t frame_head;
	size_t frame_count;
	struct remote_trace traces[REMOTE_TRACE_CAPACITY];
	size_t trace_head;
	size_t trace_count;

	bool logged_in;
	int32_t entity_id;
	/* Online mounts: the vehicle the LOCAL player rides per the server's
	 * Packet39 (docs/mp_mounts_design_2026-09-11.md). Pump-thread only
	 * (written by process_event, read by remote_process_srpc), like the
	 * chat counters -- no lock. */
	bool mounted;
	int32_t mounted_vehicle_id;
	/* MP-BED: the LOCAL player's EntityPlayer.sleeping as the server set it
	 * (Packet17 for our id, NetClientHandler.java:370-378; cleared by our own
	 * Packet18 animate 3 or a respawn), and the last SRPC_PLAYER_STATE
	 * leave_bed_seq already sent as Packet19 state 3. Pump-thread only, like
	 * mounted above. */
	bool local_sleeping;
	uint32_t leave_bed_seq_sent;
	int64_t map_seed;
	int8_t dimension;
	/* Dimension in STREAM order as the read thread sees it (login, then every
	 * Packet9). remote->dimension above is the consumer's view; the two differ
	 * while a dimension-change Respawn sits in the event queue -- exactly the
	 * window in which Beta's server already streams the new world's Packet50/51
	 * (ServerConfigurationManager.sendPlayerToOtherDimension sends Respawn and
	 * then the chunk burst back to back). Beta's client handles both on one
	 * thread in order (NetClientHandler.handleRespawn -> new WorldClient,
	 * then handleMapChunk); this provider retains chunk bodies on the read
	 * thread, so it must drop the old world's entries at the SAME point of the
	 * stream (see server_remote_transport.c) and gate promotion by epoch. */
	int8_t read_dimension;
	uint32_t world_epoch;   /* read thread: ++ on every dimension change */
	uint32_t applied_epoch; /* consumer: epoch whose CRPC_WORLD_RESET is done */
	uint32_t keep_alives_received;
	uint32_t keep_alives_sent;
	struct worldgen* climate;
	struct remote_chunk chunks[REMOTE_CHUNK_ENTRY_CAPACITY];
	struct remote_promotion promotion;
	uint64_t arrival_sequence;
	double authoritative_x, authoritative_y, authoritative_z;
	float authoritative_yaw, authoritative_pitch;
	bool have_authoritative_position;
	bool have_authoritative_look;
	bool authoritative_on_ground;
	/* THE ADMISSION CENTRE (F5b, review findings H5/L12). The column the
	 * radius gate, the demotion hysteresis and the readiness census all
	 * measure from -- ONE accessor
	 * (remote_admission_centre_locked, server_remote_chunks.c) so the three
	 * can never disagree about where the player is (L12: the census used the
	 * client CAMERA column while admission used authoritative_x/z).
	 *
	 * Why it is not just authoritative_x/z: that pair is set only by a
	 * MOVING Packet13 correction, so the gate was INERT from login until the
	 * first one (H5: a server that streams before it teleports got the
	 * pre-admission behaviour -- every announced column installed -- and the
	 * 8 MiB backing reservation made that window strictly worse than before
	 * the slice). Beta's own login order sends Packet6SpawnPosition and the
	 * 0x0D PlayerPositionLook BEFORE the chunk burst [740c583
	 * ServerConfigurationManager.login -> NetServerHandler.teleport], so a
	 * provisional centre is available from LOGIN; `authoritative` records
	 * whether the centre came from a real position packet (0x0D/0x0B) or
	 * from the weaker spawn hint, so a spawn hint can never overwrite a live
	 * position.
	 *
	 * Pump-thread-exclusive for writes, exactly like authoritative_* above
	 * (process_event is the sole writer); read under impl->lock by the
	 * promotion/residency steps and the census, which run on that same
	 * thread. */
	int32_t admit_centre_x, admit_centre_z;
	bool have_admit_centre;
	bool admit_centre_authoritative;
	double last_sent_x, last_sent_y, last_sent_z;
	float last_sent_yaw, last_sent_pitch;
	bool have_last_sent_position, have_last_sent_look;
	bool last_sent_sneaking, have_last_sent_sneaking;
	uint32_t correction_seq;
	/* SRPC_SIGN_SET accumulates per-cell edits into this local 4x16 buffer
	 * and flushes it as one whole-sign Packet130UpdateSign on the matching
	 * SRPC_WINDOW_CLOSE, mirroring GuiEditSign.onGuiClosed. Only one sign can
	 * be under edit at a time (matches the single-screen client). [740c583
	 * Packet130UpdateSign] [740c583 GuiEditSign.onGuiClosed] */
	struct {
		bool active;
		uint8_t window;
		int32_t x;
		int16_t y;
		int32_t z;
		char lines[MP_SIGN_LINE_COUNT][SIGN_EDIT_COLS + 1];
	} sign_edit;
	/* P3 MP-MISC: the cell our last Packet15 with a held sign would fill
	 * (ItemSign.onItemUse's face offsets). Beta's client opens GuiEditSign
	 * from its own local ItemSign.onItemUse; this thin client has no local
	 * placement, so it opens the editor when the server's Packet53 for that
	 * cell (NetServerHandler.handlePlace always sends one for the target
	 * cell) shows a sign there. Any other block there disarms it. Pump
	 * thread only. [740c583 ItemSign.onItemUse] [NetServerHandler.java
	 * handlePlace Packet53 pair] */
	struct {
		bool armed;
		int32_t x;
		int16_t y;
		int32_t z;
	} sign_place_pending;
	/* CRPC_WINDOW_PROGRESS carries all three furnace-progress fields on
	 * every message (client_interface.c overwrites them unconditionally),
	 * but Packet105 updates exactly one bar id per packet. Track the last
	 * known triple per currently-open window so an update to one bar never
	 * zeroes the other two. Reset whenever a new window opens. [740c583
	 * Packet105UpdateProgressbar] */
	int16_t progress_window; /* -1 = none tracked yet */
	uint16_t progress_cook, progress_burn, progress_fuel;
	bool load_completion_pending;
	bool remote_load_complete_sent;
	size_t retained_bytes, retained_bytes_peak;
	size_t retained_byte_cap;
	size_t retained_entries, retained_entry_peak;
	size_t inflated_bytes_frame;
	uint32_t retention_evictions;
	uint32_t retention_allocation_failures;
	/* Consecutive (not lifetime) inflateInit failures since the last success;
	 * reset on any successful inflateInit and at connection start. */
	uint32_t promotion_inflate_init_failures;
	uint32_t reconnect_recoveries;
	/* Live pending gauge: entries currently parked in deferred_edit_log
	 * below (goes up on a new parked edit, down on drain/overflow-drop --
	 * see struct remote_deferred_edit's comment above for the full design).
	 * deferred_edits_applied/_dropped are cumulative counters alongside it. */
	uint32_t deferred_edits;
	/* REL-MP-FIX3a-r: set above HIGH_WATER, cleared at <= LOW_WATER, both
	 * evaluated by the pump compaction gate only (self-healing: a stale true
	 * at a low log clears on the next pump). */
	bool deferred_pressure;
	/* REL-MP-JOINSPEED: REMOTE_DEFERRED_EDIT_CAPACITY entries in the MEM2
	 * scratch slot MP_BACKING_SCRATCH_EDITS, claimed on the first park and
	 * released by remote_chunks_clear_locked (NULL = no pool yet). Free
	 * entries are a list through `next` (deferred_edit_free, 1-based) plus
	 * the never-used tail from deferred_edit_cursor; deferred_edits_peak is
	 * the session high-water of `deferred_edits`. */
	struct remote_deferred_edit* deferred_edit_log;
	uint16_t deferred_edit_free;
	uint32_t deferred_edit_cursor;
	uint32_t deferred_edits_peak;
	uint32_t deferred_edits_applied;
	uint32_t deferred_edits_dropped;
	/* Parked edits removed because a NEWER Packet51 body covering their cell
	 * arrived (the snapshot post-dates the edit: Beta's later-wins), and the
	 * count of exhaustion terminations. Neither is a silent loss. */
	uint32_t deferred_edits_superseded;
	uint32_t deferred_edit_log_exhausted;
	/* Body-chain refusals (a chain that could never be folded -> the session
	 * ends, mp_end = SERVER_REMOTE_END_PROTOCOL) and publications held
	 * because an OLDER event was still queued. */
	uint32_t chain_rejections;
	uint32_t promotion_publish_waits;
	/* IN-009 LOSSLESS CHAIN FOLDING: see struct remote_fold_state. Exactly
	 * REMOTE_FOLD_STATE_BYTES (24) of persistent impl growth; the fold's
	 * buffers are the existing RECODE scratch (raw 81,920 + staging 82,944
	 * bytes, MEM2 backing side budget). */
	struct remote_fold_state fold;
	/* ADMISSION RADIUS (W1, design section 2.1). Columns farther than this
	 * (Chebyshev, in columns, from the server-authoritative player column)
	 * are never promoted; RESIDENT columns beyond admit_radius + 1 are
	 * demoted back to BACKED. Fixed per connection: the per-connection reset
	 * in server_remote_start defaults it to
	 * SERVER_REMOTE_ADMIT_RADIUS_MAX (== MAX_VIEW_DISTANCE), and main.c's
	 * server_remote_set_admit_radius call right after a successful start
	 * applies the "mp.install_radius" dial. Read under the lock by the
	 * promotion/residency steps. */
	int admit_radius;
	/* Demotion re-encode scratch (design section 2.4: "the deflate scratch
	 * for demotion is a fixed reusable buffer (no per-demotion malloc)").
	 * REMOTE_DEMOTE_SCRATCH_BYTES, allocated lazily on the first demotion
	 * that needs it and freed by remote_chunks_clear_locked -- i.e. at
	 * server_remote_cancel, and also at a dimension change (which routes
	 * through the same clear and then re-allocates on demand). Owned and
	 * sized entirely by server_remote_chunks.c. */
	uint8_t* demote_scratch;
	/* Lossless demote-on-exit accounting (design section 2.3). demotions /
	 * demote_bytes are successes and the compressed bytes they produced;
	 * demotion_failures counts attempts that left the column RESIDENT (no
	 * scratch, deflate error, body-allocation refusal, or a client that
	 * refused the unload) and will be retried next pump. */
	uint32_t demotions;
	size_t demote_bytes;
	uint32_t demotion_failures;
	/* THE HARD RESIDENT CAP (F5b, review finding H1). resident_cols is the
	 * number of entries whose `installed` bit is set, maintained by
	 * remote_set_installed_locked at every single flip -- so it is a COUNT,
	 * not a scan, and the promotion gate can test it for free. H1: the
	 * (2R+1)^2 ceiling was a derived property of a STABLE centre, not an
	 * invariant; a same-dimension respawn moved the centre, left 121 old
	 * columns installed (the same-dimension branch clears nothing) and let
	 * 121 NEW ones install on top -- 242 columns x 81,920 B, which is the
	 * measured OOM. The cap is the demotion HYSTERESIS disk, (2R+3)^2:
	 * everything past admit_radius + 1 is demoted, so that disk is the most
	 * that can legitimately be resident at once, and a promotion above it
	 * WAITS for a demotion instead of growing the set.
	 * resident_cols_peak is the session high-water (the number a budget
	 * claim must be read against); promote_deferred_cap counts promotions
	 * the cap deferred -- a deferral, never a drop: the body stays RETAINED
	 * and the pick retries the next pump. */
	size_t resident_cols;
	size_t resident_cols_peak;
	uint32_t promote_deferred_cap;
	/* THE BOUNDED ORDERING VETO (F5b, H1). The residency step used to return
	 * immediately whenever the decoded-event ring was non-empty, so a busy
	 * edit stream could pin the resident set at the cap for as long as it
	 * lasted. It now skips at most REMOTE_RESIDENCY_VETO_PUMP_LIMIT
	 * CONSECUTIVE pumps and then demotes anyway -- safely, because the
	 * re-encode is stamped by the publication-gate rule (the oldest queued
	 * event's stamp bounds it), which is the same rule
	 * remote_finish_promotion_locked publishes under. residency_veto_skips
	 * counts the skips, residency_veto_overrides the forced demotions. */
	uint32_t residency_veto_pumps;
	uint32_t residency_veto_skips;
	uint32_t residency_veto_overrides;
	/* THE RE-ENCODE LOCK SPLIT (F5b, review finding H4). The demotion's
	 * gather + deflate used to run with impl->lock HELD, i.e. the read
	 * thread could not retain a single Packet51 for the whole 81,920 B
	 * level-6 deflate -- a self-inflicted socket stall during exactly the
	 * burst this card is about, and unmeasured. The deflate now runs with
	 * impl->lock RELEASED and these are the measurement the rule demands:
	 * microseconds spent in the unlocked phase, last and session maximum.
	 *
	 * recode_busy says an unlocked re-encode owns demote_scratch right now.
	 * It is the ONLY thing that keeps the read thread's compaction (which
	 * needs the same scratch) and remote_chunks_clear_locked (which releases
	 * it) off that buffer while the lock is down; both run under impl->lock,
	 * and the pump sets/clears the flag under the same lock, so the flag is
	 * as safe as the data it guards. A clear that arrives while busy parks
	 * its scratch release in scratch_release_pending for the re-encode to
	 * honour when it re-acquires. recode_aborts counts re-encodes thrown
	 * away because the column changed identity while the lock was down. */
	uint32_t demote_deflate_us_last;
	uint32_t demote_deflate_us_max;
	uint32_t recode_aborts;
	bool recode_busy;
	bool scratch_release_pending;
	/* M7: server unloads (0x32) the client REFUSED -- its CRPC_UNLOAD_CHUNK
	 * handler no-ops while the world is not reset-ready or during stream
	 * recovery. The entry keeps installed = true and unload_pending set and
	 * is retried the next pump, instead of destroying the record and leaking
	 * 81,920 B of CARVE_VOXEL with no ledger entry pointing at it. */
	uint32_t unload_refused_retries;
	/* F4 MEMORY-AWARE PROMOTION (design 2.3/2.4). Pumps where the pick found
	 * an eligible in-radius column and did not start it, because the MEM2
	 * carve could not hold its 81,920 B of voxels AND still leave
	 * REMOTE_MESH_RESERVE_BYTES for the meshes that make those voxels
	 * visible. A deferral, never a drop: the body stays, the pick retries
	 * next pump. Public mirror: server_remote_status::promote_deferred_memory.
	 * Rationale and the measured arithmetic live at the gate in
	 * server_remote_chunks.c. */
	uint32_t promote_deferred_memory;
	/* DEFERRED-LOG COMPACTION accounting (W2, design section 2.5). A BACKED
	 * column's parked edits are folded INTO its compressed head (inflate,
	 * apply in stamp order, deflate) once the log passes
	 * REMOTE_DEFERRED_EDIT_HIGH_WATER, so the log is bounded without ever
	 * losing authoritative state (Astra gate-2 item 7). compactions /
	 * compacted_edits are successes and the parked edits they absorbed;
	 * compaction_failures counts attempts that changed nothing (no scratch,
	 * a partial head that cannot reconstruct a column, zlib refusal, or a
	 * backing refusal) and are retried next pump. */
	uint32_t compactions;
	uint32_t compacted_edits;
	uint32_t compaction_failures;
	/* REL-MP-JOINSPEED: what compaction costs. compaction_lock_us_* is the
	 * time one compaction held impl->lock (last / session max / session
	 * total, microseconds); compaction_unlocked counts the pump compactions
	 * that ran their deflate with the lock RELEASED (the demotion's
	 * recode_busy protocol) and compaction_aborts the ones thrown away
	 * because their column changed while the lock was down. */
	uint32_t compaction_lock_us_last;
	uint32_t compaction_lock_us_max;
	uint64_t compaction_lock_us_total;
	uint32_t compaction_unlocked;
	uint32_t compaction_aborts;
	/* BACKING REFUSALS (W2, design section 2.4): times the reservation could
	 * not hold a body even after the reclaim pass, i.e. the number of clean
	 * mp_end=5 capacity failures this session latched. Zero on every
	 * acceptance workload by requirement (Astra gate-2 item 5). */
	uint32_t backing_refusals;
	/* F5a (review finding H3): times a caller saw
	 * remote_backing_reclaim_or_fail_locked return FAILURE while mp_end was
	 * still 0 -- i.e. a capacity death that would have been reported as
	 * mp_end=0 with whatever stale reason impl->reason happened to hold. That
	 * is a BUG in the reclaim ladder, not a runtime condition: the guard
	 * asserts on it in a test build (-UNDEBUG) and counts it here in release,
	 * where it also repairs the latch so the session still ends visibly.
	 * Must stay 0 in every test. */
	uint32_t backing_latch_bugs;
	/* F5a (review finding M8): compactions the EXHAUSTION path had to run
	 * because the deferred-edit log reached its capacity anyway -- the pump's
	 * high-water compaction did not keep up (a very broad edit burst, or no
	 * pump between two edits). Nonzero is healthy, not an error: it is the
	 * cliff being converted into work. The kill only happens when one of
	 * these finds nothing left to fold. */
	uint32_t deferred_log_rescues;
	/* The packet the pump is applying right now (its receive stamp) and the
	 * running index of the edit within it -- pump-thread-only; see struct
	 * remote_event::seq. */
	uint64_t pump_packet_seq;
	uint32_t pump_packet_sub;
	uint32_t forced_reconnects;
	/* Times the read thread parked on a full event queue (see
	 * push_event_locked's header comment); nonzero is normal under real
	 * gameplay burst traffic or a slow frame, not itself an error. */
	uint32_t event_backpressure_waits;
	/* Frames where server_remote_pump hit REMOTE_PUMP_EVENT_BUDGET and
	 * deferred the rest of the queue to the next frame (M8: the drain loop
	 * must not hold the main thread unboundedly against a fast producer). */
	uint32_t events_deferred_frames;
	/* Reconnect-burst diagnostics (2026-08-27 live "remote event queue
	 * overflow"): ring high-water mark this connection, and the longest gap
	 * between server_remote_pump calls while a session was live -- the
	 * main-thread stall that parks the reader on a full ring (REL-MP-FIX5).
	 * Reset by server_remote_start. */
	size_t queued_events_peak;
	uint32_t pump_gap_max_ms;
	ptime_t last_pump_time;
	bool have_last_pump;
	uint32_t corrections_received;
	uint32_t corrections_echoed;
	uint32_t climate_create_ms;
	/* S3: pump-thread-exclusive (only process_event touches it, same as
	 * remote->climate/authoritative_* above), so no lock is needed. */
	struct remote_entity_table remote_entities;
	/* S4 P3 (misc/polish). Every field below is pump-thread-exclusive, same
	 * as corrections_received/deferred_edits above (process_event's own
	 * thread is the sole writer); server_remote_get_status copies them under
	 * the lock like every other counter. */
	uint32_t chat_lines_received;
	uint32_t chat_lines_sent; /* Packet3 frames queued from SRPC_CHAT_SEND */
	uint32_t attach_applied; /* Packet39 for the local player forwarded (attach or detach) */
	uint32_t attach_ignored; /* Packet39 for the local player naming an untracked vehicle */
	uint32_t chat_lines_rejected;
	uint32_t door_change_unhandled;
	uint32_t bed_events_unhandled;
	uint32_t weather_lightning_noop; /* 0x47 with type != 1 (Beta spawns nothing) */
	uint32_t weather_lightning_bolts; /* 0x47 type 1: thunder + explode played */
	uint32_t statistic_noop;          /* 0xC8 not forwarded (negative amount) */
	uint32_t statistic_forwarded;     /* 0xC8 -> GE_REMOTE_STAT (Packet200) */
	uint32_t sign_editor_opened;      /* own sign placement echoed -> editor */
	/* hw1029b/G1: telemetry-only wire counters (enum server_remote_kind in
	 * server_remote.h). rx slots: process_event's thread is the sole writer;
	 * tx slots: remote_process_srpc's caller (the same pump drain) is. */
	uint32_t kind_counts[SR_KIND_COUNT];
	/* 0x83 Packet131MapData consumer state -- pump-thread-exclusive like
	 * remote_entities (process_event and the pump-tail flush are the only
	 * touchers). Beta's client caches EVERY map id it ever sees, unbounded
	 * [740c583 ItemMap.getMPMapData -> World.setItemData]; a 16KB colour
	 * plane per id makes that a hostile-server OOM vector and a real MEM
	 * cost on Wii, so this port caps the cache at REMOTE_MAP_MIRRORS with
	 * clean-only LRU eviction. Recover evicted bytes from the client cache;
	 * if recovery/capacity fails, use the separate bounded map retry queue.
	 * Vanilla never requests map data [740c583 NetClientHandler.handleMapData,
	 * ItemMap.onUpdate]; retry is local retention, not a network request. */
	struct remote_map_mirror {
		bool used;
		bool dirty; /* strip applied since last successful CRPC push */
		int16_t map_id;
		uint32_t last_touch; /* LRU stamp from map_touch_counter */
		uint8_t colors[MAP_DATA_COLORS_SIZE];
	} map_mirrors[REMOTE_MAP_MIRRORS];
	/* Main-thread-only strip FIFO, independent of transport. Entries for an
	 * identity retain arrival order; failed identities do not block others. */
	struct remote_map_retry {
		int16_t map_id;
		uint8_t length;
		uint8_t bytes[255];
	} map_retries[REMOTE_MAP_RETRIES];
	unsigned map_retry_count;
	uint32_t map_retry_overflows;
	uint32_t map_update_deferred; /* saturates; failed strip attempts */
	uint32_t map_touch_counter;
	uint32_t map_strips_applied;
	uint32_t map_markers_applied;
	uint32_t map_data_dropped;
	uint32_t map_mirror_evictions;
	uint32_t map_pushes;
	uint32_t map_push_deferred;
	/* push-0921/B2 observation seams -- the impl-side originals of the
	 * records struct server_remote_status publishes (server_remote.h carries
	 * the full contract: ring index = serial % capacity, serial 0 = never
	 * written, item -1 = empty stack). Pump-thread-exclusive, exactly like
	 * chat_lines_received and the map counters above: process_event is the
	 * only writer and server_remote_get_status copies them under the lock
	 * with every other counter, so no new lock and no allocation. Cumulative
	 * for the life of the impl -- server_remote_start does not reset them,
	 * same as chat_lines_received. */
	uint32_t block_echo_serial;
	struct server_remote_block_echo block_echoes[SERVER_REMOTE_BLOCK_ECHO_RING];
	uint32_t txn_serial;
	struct server_remote_txn_result txn_results[SERVER_REMOTE_TXN_RING];
	uint32_t set_slot_serial;
	struct server_remote_set_slot set_slots[SERVER_REMOTE_SET_SLOT_RING];
	/* push-0921/B19 -- the watched cell's own slot, outside the ring above.
	 * Written by remote_record_block_echo (pump thread) and by
	 * server_remote_play_watch (main thread), both under `lock`; published by
	 * server_remote_get_play_obs. `watch_on` is false until a script arms it,
	 * so this is dead weight for every other profile -- see
	 * server_remote.h's struct server_remote_play_obs::watch_echo for why the
	 * slot exists and what clears it. */
	bool watch_on;
	int32_t watch_x, watch_y, watch_z;
	struct server_remote_block_echo watch_echo;
	/* hw1030g: see server_remote.h struct server_remote_play_obs. */
	struct server_remote_block_echo watch_history[SERVER_REMOTE_WATCH_HISTORY];
	uint32_t watch_history_n;
	uint32_t watch_bodies;
	uint32_t watch_clears;
	char reason[256];
};

/* --- Cross-module declarations ------------------------------------------
 * Grouped by the .c file that DEFINES each symbol; every symbol here is
 * called from at least one OTHER server_remote_*.c file (that is what
 * earns a promotion out of `static`). Signatures are copied verbatim from
 * their definitions. */

/* Defined in server_remote_transport.c (socket lifecycle, connect handoff,
 * read/write worker threads, framing/backpressure, kill/teardown). */
void set_reason_locked(struct server_remote_impl* remote,
						const char* reason);
void close_socket_locked(struct server_remote_impl* remote);
/* F5a (review finding H2): wire `decoder`'s body-allocation reclaim seam
 * (struct mp_decoder::reclaim) to `remote`'s reservation. The read thread
 * calls it once, right after mp_decoder_init; a test calls the same function
 * so the path it drives is wired exactly as production is. */
void remote_attach_decoder_reclaim(struct server_remote_impl* remote,
								   struct mp_decoder* decoder);
/* F5a (review finding H2): the read loop's MP_DECODE_ERROR terminal, factored
 * out of the loop so the PRODUCTION diagnosis is reachable from a test (the
 * review's "the tests do not cover this path"). Ends the session either way
 * -- the caller always stops reading -- and returns true when it ended on
 * CAPACITY (mp_end=5 + the "does not fit this console" reason) rather than on
 * a protocol error. Takes the lock itself; must be called WITHOUT it. */
bool remote_read_decode_error(struct server_remote_impl* remote,
							  const struct mp_decoder* decoder,
							  uint32_t generation);
void kill_connection_locked(struct server_remote_impl* remote,
							 const char* reason);
void publish_error(struct server_remote_impl* remote, uint32_t generation,
					const char* message);
bool queue_frame(struct server_remote_impl* remote,
				  const struct mp_frame* frame);
/* REL-MP-FIX3e: append under `lock` (taken by the caller; frame_lock nested
 * inside). Exported for tests/test_mp_walk_session.c's writer-isolation case. */
#define SERVER_REMOTE_FRAME_LOCK 1
bool queue_frame_locked(struct server_remote_impl* remote,
						 const struct mp_frame* frame, bool correction_echo);
/* Fix B (2026-08-30, MP stuck-respawn root cause): remote_process_srpc's own
 * REMOTE_PLAY gate check and the eventual queue_frame/queue_movement append
 * used to run under TWO SEPARATE lock acquisitions -- a window where any
 * other thread's single locked kill site (Fix A's overlap park included)
 * could flip the connection out of REMOTE_PLAY in between. The gate had
 * already said "yes", so the SRPC (once dequeued from svin's channel, never
 * retried -- see server_interface.c's svin_rpc_send/svin_rpc_flush_pending
 * header comments) was silently eaten with no retry and no caller-visible
 * failure. These two re-validate REMOTE_PLAY under the SAME lock
 * acquisition as the append, closing the window for every SRPC that funnels
 * through remote_process_srpc, not just SRPC_RESPAWN. */
bool queue_frame_playing(struct server_remote_impl* remote,
						  const struct mp_frame* frame);
bool queue_movement(struct server_remote_impl* remote,
					 const struct mp_movement* movement,
					 enum mp_event_type type, bool correction_echo);
bool queue_movement_playing(struct server_remote_impl* remote,
							 const struct mp_movement* movement,
							 enum mp_event_type type);
bool pop_event_locked(struct server_remote_impl* remote,
					   struct remote_event* event);

/* Defined in server_remote_chunks.c (retention/promotion/unload). */
void remote_chunks_clear_locked(struct server_remote_impl* remote);
/* REL-MP-JOINSPEED: a parked edit's absolute cell (owner column + `cell`).
 * Only meaningful for a used entry (owner != 0); impl->lock held. */
static inline bool remote_deferred_edit_used(const struct remote_deferred_edit* e) {
	return e->owner != 0;
}
static inline int32_t remote_deferred_edit_x(const struct server_remote_impl* remote,
											 const struct remote_deferred_edit* e) {
	return remote->chunks[e->owner - 1u].chunk_x * 16 + (int32_t)((e->cell >> 11) & 15u);
}
static inline int32_t remote_deferred_edit_z(const struct server_remote_impl* remote,
											 const struct remote_deferred_edit* e) {
	return remote->chunks[e->owner - 1u].chunk_z * 16 + (int32_t)((e->cell >> 7) & 15u);
}
static inline uint8_t remote_deferred_edit_y(const struct remote_deferred_edit* e) {
	return (uint8_t)(e->cell & 127u);
}

bool remote_retain_map_locked(struct server_remote_impl* remote,
							   struct mp_event* event);
bool remote_pre_chunk_locked(struct server_remote_impl* remote,
							  int32_t chunk_x, int32_t chunk_z, bool mode);
void remote_apply_block_change(struct server_remote_impl* remote, int32_t x,
								uint8_t y, int32_t z, uint8_t type,
								uint8_t metadata);
/* push-0921/B2 + hw1030g: the one block-echo recorder (ring + watched cell),
 * server_remote_protocol.c. No lock taken: main/pump thread, see its header. */
void remote_record_block_echo(struct server_remote_impl* remote, int32_t x,
							  uint8_t y, int32_t z, uint8_t type,
							  uint8_t metadata);
void remote_emit_load_completion_locked(struct server_remote_impl* remote);
bool remote_promotion_step_locked(struct server_remote_impl* remote);
/* W1 demote-on-exit (design section 2.3): at most ONE column per pump whose
 * distance has passed admit_radius + 1 is losslessly re-encoded back to
 * BACKED and unloaded client-side. Called once per pump immediately after
 * remote_promotion_step_locked. */
void remote_residency_step_locked(struct server_remote_impl* remote);
/* W2 (design section 2.4): the backing reservation refused a body. Try the
 * two lossless reclaims (one deferred-log compaction, then one head a queued
 * body already supersedes) and return true if the caller should RETRY its
 * allocation; return false once the session has been ended cleanly with
 * mp_end = SERVER_REMOTE_END_CAPACITY and the "does not fit this console"
 * reason. `wanted` is the refused allocation's size, folded into the reason
 * only when one body alone exceeds the whole reservation. Callers: the
 * demotion/compaction re-encodes here, and the read thread's decoder-side
 * refusal in server_remote_transport.c. */
bool remote_backing_reclaim_or_fail_locked(struct server_remote_impl* remote,
										   size_t wanted);
/* F5a (review findings H2/H3): the TERMINAL half of the ladder above, on its
 * own. Latches mp_end = SERVER_REMOTE_END_CAPACITY (first-wins), builds the
 * "does not fit this console" reason from the live column/KB figures, and
 * ends the session through kill_connection_locked. Unconditional: unlike
 * remote_backing_reclaim_or_fail_locked it never reclaims and never reports
 * "try again", so a caller that has run out of retries cannot accidentally
 * fall through to a protocol-error diagnosis (H2) or to `return false` with
 * no reason set (H3). Idempotent -- a session already killed keeps its first
 * reason and its first mp_end. */
void remote_backing_capacity_fail_locked(struct server_remote_impl* remote,
										 size_t wanted);
/* F5a (review finding H3): the CALLER-SIDE guard on the ladder's contract.
 * Call immediately after remote_backing_reclaim_or_fail_locked returned
 * false: a failure must never leave mp_end at 0, because the caller's own
 * `return false` then publishes whatever stale string impl->reason holds and
 * an unattended run reports mp_end=0 for a capacity death. Asserts in a test
 * build; in release it counts backing_latch_bugs AND repairs the latch, so
 * the worst case is a less specific reason rather than an invisible death. */
void remote_backing_fail_guard_locked(struct server_remote_impl* remote);
/* IN-009 step 7: first-wins mp_end = SERVER_REMOTE_END_PROTOCOL latch (phase
 * and exact rx at the moment of detection). A no-op once any terminal code
 * is latched or a decoded Packet255 owns the session's ending. The caller
 * sets the reason and performs the teardown it already performed. */
void remote_latch_protocol_end_locked(struct server_remote_impl* remote);
void remote_emit_pending_unloads_locked(struct server_remote_impl* remote);
/* THE ONE ADMISSION-CENTRE ACCESSOR (F5b, H5/L12). Writes the centre column
 * and returns whether one is known at all. Every consumer -- the promotion
 * gate, the demotion hysteresis (both in server_remote_chunks.c) and the
 * readiness census (server_remote.c) -- goes through this, so the screen can
 * never demand a disk the gate is not filling. Call with impl->lock held. */
bool remote_admission_centre_locked(const struct server_remote_impl* remote,
									int32_t* out_cx, int32_t* out_cz);
/* ci-1101/54: the F4 memory gate's answer for a COLD (not yet installed)
 * column right now -- the same remote_carve_can_hold_column test
 * remote_start_promotion_locked applies. The readiness census uses it so the
 * loading screen never waits on a column the gate is refusing (the M6
 * radius/census disagreement, for memory instead of radius). Takes the mem2
 * mutex; impl->lock may be held (the pump's own order). */
bool remote_carve_admits_cold_column(void);
/* Feed the centre. `authoritative` = the value came from a server position
 * packet (0x0D/0x0B correction, or a respawn's position); false = the weaker
 * login/respawn SPAWN hint (Packet6), which may seed a centre but must never
 * overwrite an authoritative one. Coordinates are BLOCK coordinates, floored
 * to columns here so there is one floor() in the codebase for this.
 * Pump-thread only (process_event for incoming corrections, remote_process_srpc
 * for the position the client sends -- F7), like authoritative_x/z themselves. */
void remote_admission_centre_set(struct server_remote_impl* remote, double x,
								 double z, bool authoritative);
/* A dimension change invalidates the centre with the table it belonged to. */
void remote_admission_centre_clear(struct server_remote_impl* remote);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* H4 proof hook: called from INSIDE the demotion's unlocked deflate phase.
 * The test's implementation trylocks impl->lock and asserts it SUCCEEDS --
 * i.e. the 81,920 B level-6 deflate genuinely does not hold the lock the read
 * thread needs to retain a Packet51. A no-op when unset. */
void server_remote_test_set_deflate_probe(void (*probe)(
	struct server_remote_impl* remote));
/* Make the next `count` chunk unloads behave the way the client's GATED
 * CRPC_UNLOAD_CHUNK handler does when it refuses -- the RPC is not sent, so
 * the column really stays installed and both verifying paths (the demotion's
 * full-chain revert, and M7's keep-and-retry) run against a true world. The
 * client state that causes a real refusal (s_world_reset_ready, the
 * stream-recovery phase) is private to client_interface.c. */
void server_remote_test_refuse_unloads(uint32_t count);
/* REL-MP-FIX2a: run remote_chunk_fold_locked on one column (no incoming
 * body); true on REMOTE_FOLD_DONE. */
bool server_remote_test_fold_column(struct server_remote* remote,
									int32_t chunk_x, int32_t chunk_z);
#endif

/* Defined in server_remote_entities.c (remote-entity table, spawn/move/
 * look/metadata/status handlers, the map-data mirror). */
void remote_entities_clear(struct server_remote_impl* remote);
struct remote_entity* remote_entity_find(struct server_remote_impl* remote,
										  int32_t id);
struct remote_entity* remote_entity_create(struct server_remote_impl* remote,
											int32_t id,
											enum remote_entity_kind kind);
void remote_entity_destroy(struct server_remote_impl* remote, int32_t id);
bool beta_mob_type_to_monster_id(uint8_t beta_type, int* out_monster_id);
void apply_mob_metadata(struct remote_entity* entry,
						 const struct mp_metadata* metadata);
/* Finds a decoded DataWatcher entry by index/type, or NULL. Exposed (not
 * static) so remote_process_srpc/process_event's own dispatch (server_
 * remote_protocol.c) can pull generic entity-flag bits (e.g. sneaking,
 * watcher index 0 bit 1) without duplicating the linear scan. */
const struct mp_watchable_entry* metadata_find(
	const struct mp_metadata* metadata, uint8_t index, uint8_t type);
struct remote_map_mirror*
remote_map_mirror_for(struct server_remote_impl* remote, int16_t map_id);
void flush_map_mirrors(struct server_remote_impl* remote);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool server_remote_test_push_event(struct server_remote* remote, const struct remote_event* event);
#endif
void remote_emit_status_move(struct server_remote_impl* remote,
							  const struct remote_entity* entry);
void flush_entity_status(struct server_remote_impl* remote);
/* Residual item 4 (2026-08-26): one-shot hurt/death SOUND on ENTITY_STATUS
 * arrival (status 2/3 only -- called once per arrival, never from the
 * pump-tail sweep, matching Beta's own placement inside handleHealthUpdate
 * rather than a repeating clock). */
void remote_emit_status_sound(struct server_remote_impl* remote,
							   const struct remote_entity* entry,
							   int8_t status);

/* Defined in server_remote_protocol.c (process_event S->C dispatch minus
 * entity/chunk domains, plus the S2 remote_process_srpc C->S dispatch). */
// false leaves an unapplied map strip owned by the event queue for retry.
bool process_event(struct server_remote* owner, struct remote_event* event);
void remote_process_srpc(struct server_rpc* call, void* user);

/* Defined in server_remote.c (the pump/status/API seam): the one primitive
 * every domain uses to hand a decoded packet's translation to the local
 * client -- not owned by transport, protocol, entities or chunks alone. */
bool remote_try_crpc(struct server_remote_impl* remote,
					  struct client_rpc* call, const char* failure);

#endif /* CAVEWII_MP_CLIENT */

#endif /* SERVER_REMOTE_INTERNAL_H */
