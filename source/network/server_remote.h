/* Remote-server provider: P0 transport, world stream and movement. */
#ifndef SERVER_REMOTE_H
#define SERVER_REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* S5: the MP client compiles on BOTH platforms. Every server_remote_*.c /
 * mp_session.c / connect-screen / main.c MP block gates on this ONE macro
 * (the S5 design doc's CAVEWII_MP_CLIENT shape) instead of PLATFORM_PC.
 * The Wii transport backend is source/platform/wii/net_socket.c; the Wii
 * title-screen entry stays unregistered until S5.2 lands net_bringup. */
#if defined(PLATFORM_PC) || defined(PLATFORM_WII)
#define CAVEWII_MP_CLIENT 1
#endif

enum server_remote_state {
	REMOTE_DISCONNECTED,
	REMOTE_CONNECTING,
	REMOTE_HANDSHAKE_SENT,
	REMOTE_LOGIN_SENT,
	REMOTE_PLAY,
	REMOTE_KICKED,
	REMOTE_CLOSING,
};

/* Decoded-event ring capacity, public so tests can size burst fixtures that
 * deliberately outrun server_remote_pump() (the backpressure regression).
 * The .c defines REMOTE_QUEUE_CAPACITY to this and static-asserts they
 * agree. */
#define SERVER_REMOTE_EVENT_QUEUE_CAPACITY 64

/* S3 bounded remote-entity table capacity, public so a hostile-vector spawn-
 * overflow test can size its fixture (REMOTE_ENTITY_CAPACITY+1 distinct ids)
 * without hand-duplicating the .c's private #define. The .c defines
 * REMOTE_ENTITY_CAPACITY to this and static-asserts they agree, mirroring
 * SERVER_REMOTE_EVENT_QUEUE_CAPACITY's own precedent above. */
#define SERVER_REMOTE_ENTITY_CAPACITY 256

/* MP ADMISSION RADIUS (W1, docs/mp_admission_budget_design_2026-09-13.md
 * section 2.1). Only columns within this Chebyshev distance -- in columns,
 * from the server-authoritative player column -- are installed at full
 * detail; farther ones stay BACKED, holding their complete compressed body
 * (nothing lossy, Astra gate-2 item 5). The maximum mirrors
 * MAX_VIEW_DISTANCE (network/server_local_state.h) per platform: that header
 * is deliberately NOT included by the MP client (server_remote_internal.h's
 * own "avoid pulling in server_local.h" rule), so main.c -- the one TU that
 * sees both -- carries a _Static_assert that the two agree. The minimum is
 * the smallest radius that still leaves the player a full 5x5 column
 * neighbourhood (the safe core readiness needs, design section 2.6). */
#ifdef PLATFORM_PC
#define SERVER_REMOTE_ADMIT_RADIUS_MAX 10
#else
#define SERVER_REMOTE_ADMIT_RADIUS_MAX 5
#endif
#define SERVER_REMOTE_ADMIT_RADIUS_MIN 2

/* THE DEFAULT, sized from the carve (F4, design 2.1/2.4). W1 defaulted to
 * the maximum (MAX_VIEW_DISTANCE), which on the Wii is 5 -- and real run
 * betamc_adm_0913 showed 5 is too deep to also MESH: the carve fell 25570 KB
 * -> 39 KB free and ndisp stayed 0 for the whole 60 s, so the world was
 * resident and nothing could be drawn. The arithmetic behind 4, all of it
 * from that run:
 *
 *   carve free when an MP session starts        ~25.5 MB  (measured, tick 1)
 *   - MP backing reservation                    - 6.0 MiB (MEM2_MP_BACKING_BYTES)
 *   = for voxels + meshes                       ~19.9 MB
 *   voxels at R=4, moving max (2R+3)^2 = 121    -  9.9 MB (121 x 81,920 B)
 *   mesh reserve the promotion gate protects    -  6.0 MiB (REMOTE_MESH_RESERVE_BYTES)
 *   = slack                                     ~ 3.7 MB
 *
 * At R=5 the same table is 121 -> 169 columns = 13.8 MB of voxels, leaving
 * 3.8 MB where the meshes need ~7 MB: that is the observed ndisp=0. The
 * static resident set at R=4 is (2R+1)^2 = 81 columns (6.6 MB); the 121 is
 * the moving maximum, because demotion only fires past admit_radius + 1
 * (the design's one column of hysteresis). PC keeps the full 10 -- it has no
 * carve, so none of this arithmetic binds there.
 *
 * This is the DEFAULT, not a cap: `mp.install_radius` still reaches
 * SERVER_REMOTE_ADMIT_RADIUS_MAX for anyone who wants to spend the carve
 * differently, and the memory gate in server_remote_chunks.c protects the
 * mesh reserve whatever radius is configured. */
#ifdef PLATFORM_PC
#define SERVER_REMOTE_ADMIT_RADIUS_DEFAULT 10
#else
#define SERVER_REMOTE_ADMIT_RADIUS_DEFAULT 4
#endif

/* Terminal-latch codes for struct server_remote_status::mp_end. Only the new
 * one gets a name: the first five predate this header's needing them, and
 * renaming the existing literals would churn every reader (main.c's
 * heartbeat, the collector, the recorded PASS artifacts) for no behaviour.
 * 5 = the MP backing reservation could not hold another compressed column
 * even after the reclaim pass -- a CAPACITY answer, visible and clean, never
 * an OOM crash and never a silently dropped column (design section 2.4,
 * Astra gate-2 item 5). */
#define SERVER_REMOTE_END_CAPACITY 5u
/* 6 = the session ended on a protocol/internal failure this client detected
 * itself (an invalid or undecodable Packet51 body, a column-crossing region,
 * an exhausted coordinate table, a body chain that could not be folded, or an
 * otherwise-unclassified error published into the pump's error side-slot).
 * First-wins like every other code: a kick (1), EOF (2), receive error (3),
 * connect error (4) or capacity death (5) latched first keeps its code
 * (IN-009, tmp/jobs/loop/IN-009-astra-plan.md step 7). */
#define SERVER_REMOTE_END_PROTOCOL 6u

/* push-0921/B2 OBSERVATION SEAMS -- ring capacities, public so a reader (and
 * the suite) can size its own scan without duplicating a private #define, the
 * SERVER_REMOTE_EVENT_QUEUE_CAPACITY precedent above. See the seam block at
 * the tail of struct server_remote_status for what they hold and why. */
#define SERVER_REMOTE_BLOCK_ECHO_RING 16
#define SERVER_REMOTE_TXN_RING 8
#define SERVER_REMOTE_SET_SLOT_RING 8
/* hw1030g: every echo for the watched cell since it was armed, not only the
 * newest (struct server_remote_play_obs::watch_history). */
#define SERVER_REMOTE_WATCH_HISTORY 4

/* One server-confirmed block change: the cell the SERVER said changed, and
 * what it said it changed to. x/y/z are world coordinates (y is widened from
 * the wire's unsigned byte to int32_t so the whole record reads as one
 * coordinate triple); `type`/`metadata` are the raw decoded packet bytes --
 * note remote_apply_block_change masks metadata to 4 bits before it reaches
 * the client world, so a reader comparing against a rendered block compares
 * `metadata & 0x0F`. */
struct server_remote_block_echo {
	int32_t x;
	int32_t y;
	int32_t z;
	uint8_t type;
	uint8_t metadata;
	uint32_t serial;
};

/* One window-transaction RESULT the server sent us (Packet106 inbound, i.e.
 * MP_EVENT_TRANSACTION -- never the outbound SRPC_WINDOW_TRANSACTION ack this
 * client queues in reply). `window`/`action_id` are the wire values, so they
 * match the action number the click that caused them carried. */
struct server_remote_txn_result {
	int8_t window;
	int16_t action_id;
	bool accepted;
	uint32_t serial;
};

/* One server-confirmed slot state. `window`/`slot` are the RESOLVED values
 * the client applied, not the raw wire pair: Beta's (window -1, slot -1)
 * cursor form is recorded as (the active window, SPECIAL_SLOT_PICKED_ITEM),
 * which is the slot that actually changed. EMPTY CONVENTION: item == -1 means
 * the stack is absent -- that is the decoded packet's own `present == false`
 * (the wire writes id 0xFFFF for it), and count/damage are 0 there. A
 * whole-window refresh (MP_EVENT_WINDOW_ITEMS) is recorded as ONE entry with
 * slot == -1 and item == -1: the batch's individual slot states are not
 * ringed (the client's window is that record) -- it is here so a reader can
 * tell a batch refresh apart from a single-slot update at the same serial. */
struct server_remote_set_slot {
	int8_t window;
	int16_t slot;
	int16_t item;
	uint8_t count;
	int16_t damage;
	uint32_t serial;
};

struct server_remote_status {
	enum server_remote_state state;
	uint32_t generation;
	bool logged_in;
	int32_t entity_id;
	int64_t map_seed;
	int8_t dimension;
	bool socket_open;
	/* An fd has been shut down and retired from the live socket field but its
	 * NUMBER is not yet released to the OS -- it is parked until the workers
	 * are joined (server_remote_transport.c's "Socket teardown" invariant).
	 * Diagnostic/observability only; nothing in the client branches on it. */
	bool socket_close_deferred;
	bool read_thread_started;
	bool write_thread_started;
	bool read_thread_exited;
	bool write_thread_exited;
	size_t queued_events;
	size_t queued_frames;
	uint32_t keep_alives_received;
	uint32_t keep_alives_sent;
	size_t retained_bytes;
	size_t retained_bytes_peak;
	size_t retained_byte_cap;
	size_t retained_entries;
	size_t retained_entry_peak;
	size_t retained_entry_cap;
	size_t ready_entries;
	size_t promotion_backlog;
	size_t inflated_bytes_frame;
	uint32_t retention_evictions;
	uint32_t retention_allocation_failures;
	uint32_t reconnect_recoveries;
	/* Deferred-edit log (Plan section 7): deferred_edits is the LIVE pending
	 * count (a Packet52/53/explosion edit parked against a chunk that was
	 * not yet resident); deferred_edits_applied/_dropped are cumulative
	 * counters for edits the log successfully replayed once their chunk
	 * promoted vs. ones a full bounded log had to drop-oldest for (counted,
	 * never silent -- see struct remote_deferred_edit's comment in
	 * server_remote_internal.h). */
	uint32_t deferred_edits;
	uint32_t deferred_edits_applied;
	uint32_t deferred_edits_dropped;
	uint32_t forced_reconnects;
	/* Take-50 world-install probe (DIAGNOSTIC): lock-free sample of the read
	 * thread's decoder state + raw rx byte counter. Torn reads acceptable --
	 * these name the phase/packet the decoder sits in when chunk delivery
	 * stalls (Wii take-50: 11 pre-chunks retained, then nothing for 59s). */
	uint32_t dec_phase;
	uint32_t dec_packet_id;
	uint32_t dec_chunk_have;
	uint32_t dec_chunk_need;
	uint32_t rx_bytes;
	/* Authoritative-kick guards + terminal-event latch (mp-split-0912/5).
	 * errors_dropped_after_kick counts transport errors the pump refused to
	 * let overwrite a Packet255 kick reason (see process_event's
	 * REMOTE_EVENT_ERROR guard) -- a suppressed diagnostic that is counted,
	 * not silent. mp_end/_phase/_rx are the first-wins record of HOW this
	 * connection generation ended, so an unattended hardware run reports the
	 * cause in its heartbeat: mp_end 0 none, 1 kicked-by-Packet255,
	 * 2 remote-eof, 3 receive-error, 4 connect-error,
	 * 5 = SERVER_REMOTE_END_CAPACITY (W2, design section 2.4: the MP backing
	 * reservation could not hold another compressed column even after the
	 * reclaim pass, so the session ended cleanly with the
	 * "does not fit this console" reason instead of an OOM crash or a
	 * silently dropped column), 6 = SERVER_REMOTE_END_PROTOCOL (a
	 * protocol/internal failure this client detected, see the define);
	 * mp_end_phase is the
	 * enum server_remote_state the diagnostic's phase word comes from; and
	 * mp_end_rx is the EXACT byte count at that moment (mp_rx_kb rounds a
	 * 21-byte death to 0). All three reset on the next connect. */
	uint32_t errors_dropped_after_kick;
	uint32_t mp_end;
	uint32_t mp_end_phase;
	uint32_t mp_end_rx;
	/* Times the read thread parked on a full decoded-event queue until the
	 * pump freed a slot (REL-MP-FIX5: no short deadline; Beta's 60 s
	 * backstop only). Nonzero under real burst traffic or a slow frame is
	 * expected, not itself a fault -- see push_event_locked in
	 * server_remote_transport.c. */
	/* True once teardown is committed (stop_now) even if state has not yet
	 * left REMOTE_PLAY -- the seam's deadline belt keys off this so a
	 * dying-in-play session cannot re-arm its own rescue timer. */
	bool dying;
	/* S4 disconnect UX: true when the stop was LOCALLY requested
	 * (server_remote_disconnect / quit-to-title) as opposed to a kick,
	 * protocol error, or transport death. main.c's teardown branch uses
	 * !graceful + logged_in to route the player to the visible
	 * "Connection lost" screen instead of silently dropping to the title.
	 * [740c583 NetClientHandler.handleKickDisconnect] (Beta always shows
	 * a GuiConnectFailed with the reason). */
	bool graceful;
	uint32_t event_backpressure_waits;
	/* Frames where the pump's per-frame event budget capped the drain and
	 * left queue tail for the next frame (bounded main-thread hold; see
	 * REMOTE_PUMP_EVENT_BUDGET in server_remote.c). Nonzero under a LAN
	 * firehose is expected, not a fault. */
	uint32_t events_deferred_frames;
	/* Reconnect-burst diagnostics (2026-08-27 live "remote event queue
	 * overflow" on rejoin): decoded-event ring high-water mark this
	 * connection -- how close a login/install burst came to the overflow
	 * kill -- and the longest gap (ms) between server_remote_pump calls
	 * while the session was live. The kill can only fire when a pump gap
	 * exceeds the read thread's backpressure patience (one pump pass always
	 * opens the ring: its budget is 4x capacity), so a kill plus a large
	 * pump_gap_max_ms names a main-thread stall, not a hostile peer. */
	size_t queued_events_peak;
	uint32_t pump_gap_max_ms;
	uint32_t corrections_received;
	uint32_t corrections_echoed;
	uint32_t correction_seq;
	uint32_t climate_create_ms;
	/* S3: live remote-entity table occupancy, for the reconnect-stress test's
	 * "no monotonic growth" proof (plan S4 gate) alongside the existing
	 * chunk/event counters above. */
	size_t remote_entity_count;
	uint32_t remote_entity_capacity_drops;
	/* S4 P3 (misc/polish). "Unsupported content fails visibly": every
	 * counted no-op below has a name, shows on F3, and never silently drops. */
	uint32_t chat_lines_received;
	uint32_t chat_lines_sent; /* Packet3 frames queued from SRPC_CHAT_SEND */
	uint32_t attach_applied;  /* Packet39 (local player) forwarded to the client */
	uint32_t attach_ignored;  /* Packet39 (local player) naming an untracked vehicle */
	uint32_t chat_lines_rejected; /* malformed UTF-16, never delivered */
	uint32_t door_change_unhandled;  /* 0x3D effect ids other than 1003 */
	uint32_t bed_events_unhandled;   /* 0x46 codes outside 0..2 */
	uint32_t weather_lightning_noop; /* 0x47: no lightning render exists */
	/* 0x83 Packet131MapData -- LIVE consumer (held-map render path):
	 * type-0 colour strips are applied to a per-map mirror and forwarded
	 * to the client's held-map texture via CRPC_MAP_DATA. */
	uint32_t map_strips_applied;   /* type-0 strips applied to a mirror */
	uint32_t map_markers_applied; /* type-1 lists installed in the client cache */
	uint32_t map_data_dropped;     /* non-map item id / malformed / unknown
	                                  type (Beta logs or AIOOBEs; we count) */
	uint32_t map_mirror_evictions; /* LRU cap hit (deviation: Beta client
	                                  cache is unbounded; ours is 4) */
	uint32_t map_pushes;           /* CRPC_MAP_DATA delivered to the client */
	uint32_t map_push_deferred;    /* both pool slots in flight; retried */
	uint32_t statistic_noop;         /* 0xC8: no client statistic display */
	/* W1 admission / residency (design sections 2.1-2.3).
	 * admit_radius     -- this connection's install radius, in columns.
	 * resident_entries -- columns currently installed at full detail
	 *                     (<= (2*admit_radius+1)^2 once the radius is live).
	 * demotions        -- columns re-encoded losslessly back to BACKED
	 *                     because they left admit_radius + 1.
	 * demote_bytes     -- compressed bytes those re-encodes produced.
	 * demotion_failures-- attempts that left the column RESIDENT (no
	 *                     scratch, deflate error, body-allocation refusal,
	 *                     or a client that refused the unload). Retried next
	 *                     pump; counted, never a silent loss. */
	int admit_radius;
	size_t resident_entries;
	uint32_t demotions;
	size_t demote_bytes;
	uint32_t demotion_failures;
	/* W4 telemetry (design section 2.7): the remaining admission-budget
	 * counters W1/W2 kept inside the impl, surfaced so the 1 Hz heartbeat
	 * can carry the memory high-water evidence Astra gate-2 item 4 asked
	 * for. Pure additive readers of state the earlier slices already
	 * maintain -- nothing here changes admission, backing or readiness.
	 * backed_entries -- columns whose ONLY authoritative copy is a
	 *                   compressed body right now (RETAINED or PROMOTING):
	 *                   the tier demote-on-exit moves a column into, and
	 *                   what resident_entries above is the complement of.
	 *                   Counted in the same single pass over the entry
	 *                   table as resident_entries/ready_entries.
	 * compactions    -- deferred-log folds into a backed head (W2 2.5).
	 * backing_refusals -- times the reservation refused a body even after
	 *                   the reclaim pass, i.e. clean mp_end=5 capacity
	 *                   failures. Zero on any acceptance workload.
	 * backing_bytes / _peak_bytes / _capacity_bytes -- mp_backing_stats()
	 *                   (source/platform/mem2.h). PROCESS-GLOBAL, not
	 *                   per-connection: the reservation is one region shared
	 *                   by the decoder, the retained chain and the
	 *                   re-encodes, and it is never handed back, so the peak
	 *                   is a per-RUN high-water that outlives a generation.
	 *                   Reported here because this status snapshot is the
	 *                   only place a reader already has the rest of the
	 *                   budget picture in one consistent sample. */
	size_t backed_entries;
	uint32_t compactions;
	uint32_t backing_refusals;
	size_t backing_bytes;
	size_t backing_peak_bytes;
	size_t backing_capacity_bytes;
	/* F4 memory-aware promotion (design 2.3, Astra gate-2 item 4 "check
	 * capacity before allocating"). Pumps where an in-radius column was
	 * eligible but NOT started, because installing it would have left the
	 * MEM2 carve below the mesh reserve. It is a DEFERRAL, not a loss: the
	 * column keeps its complete body and the pick retries every pump, so a
	 * demotion or a 0x32 unload lets it through. Nonzero means the radius
	 * is writing cheques the carve cannot cash -- the honest signal that
	 * `mp.install_radius` is set deeper than this world can mesh. */
	uint32_t promote_deferred_memory;
	/* F5b (review findings H1, H4) -- the admission-budget evidence the
	 * earlier slices could not produce. All pure mirrors of impl counters;
	 * nothing here changes behaviour.
	 * resident_columns / _peak -- columns holding voxels right now, and the
	 *                   session high-water, from the maintained `installed`
	 *                   count rather than an entry-state scan. THE number a
	 *                   CARVE_VOXEL claim must be read against: it is
	 *                   hard-capped at (2*admit_radius+3)^2 regardless of
	 *                   centre history (H1: a same-dimension respawn used to
	 *                   double it, which is the measured OOM).
	 * promote_deferred_cap -- promotions the hard cap deferred (waiting for a
	 *                   demotion). A deferral, never a drop.
	 * demote_deflate_us_last / _max -- microseconds the demotion re-encode
	 *                   spent in its UNLOCKED gather+deflate phase (H4: that
	 *                   work used to run under impl->lock, blocking the read
	 *                   thread's Packet51 retention for its whole duration,
	 *                   and nothing measured it).
	 * recode_aborts  -- re-encodes discarded because the column changed
	 *                   identity while the lock was down. Retried, not lost.
	 * unload_refused_retries -- server 0x32 unloads the client's gated
	 *                   handler refused; the entry is kept and retried (M7).
	 */
	size_t resident_columns;
	size_t resident_columns_peak;
	uint32_t promote_deferred_cap;
	uint32_t demote_deflate_us_last;
	uint32_t demote_deflate_us_max;
	uint32_t recode_aborts;
	uint32_t unload_refused_retries;
	char reason[256];
};

/* push-0921/B18 -- review B17 finding H2. THE OBSERVATION SEAMS LIVE HERE,
 * NOT IN struct server_remote_status, and are read through their own
 * accessor (server_remote_get_play_obs below).
 *
 * WHY THE SPLIT. server_remote_status is a STACK LOCAL at six call sites that
 * have nothing to do with the scripted play driver -- main.c:649
 * (emit_hw_heartbeat, every heartbeat, every profile), main.c:3379 (inside
 * main()'s own frame), mp_session.c:41 / :125,
 * screen_multiplayer_connect.c:222 / :402 (every frame an ordinary player
 * sits on the connect screen) and server_remote.c's terminal-status latch --
 * and the whole struct is memcpy'd under the remote lock on every one of
 * them. Carrying 560 bytes of play-driver rings there charged an ordinary LAN
 * player, mp_connect_smoke, mp_reconnect_soak and split_play_8m for state
 * only mp_play_10m reads. Measured: the struct was 696 B (PC) / 584 B (Wii)
 * on main 73bc319c and 1256 / 1144 with the rings inlined. main()'s frame is
 * the subject of a recorded Wii boot death (188 KB frame vs the 128 KB stack,
 * 0758c12), so this is a hardware-risk path, not a tidiness question.
 *
 * WHAT DID NOT CHANGE: the writer side. process_event still records into the
 * impl's own rings, under the same lock, gated exactly as before -- the
 * recording contract below is copied verbatim from where it used to sit
 * inside server_remote_status. Only WHO PAYS TO READ it moved.
 */
struct server_remote_play_obs {
	/* push-0921/B2 OBSERVATION SEAMS (design tmp/jobs/push-0921/
	 * design-mp-play-10m.md section 6 (a)/(b); Astra gate 1, Lane B item 3:
	 * "authorize only the minimal observation seams"). READ-ONLY records of
	 * what the SERVER confirmed, published through this existing snapshot
	 * exactly like chat_lines_sent above -- so a scripted acceptance driver
	 * counts a dig / place / inventory move on the server's own echo and
	 * never on the client's own prediction. Nothing here changes packet
	 * handling, ordering, the wire, or inventory behaviour: every write sits
	 * beside an effect that has already happened, and no reader in the game
	 * branches on any of it.
	 *
	 * Common shape. Each serial starts at 0 and is assigned 1, 2, 3 ... as
	 * records arrive; the record carrying serial `s` is written at ring index
	 * `s % <ring capacity>`, so the ring always holds the newest <capacity>
	 * records and newest overwrites oldest. A reader finds a record BY
	 * SERIAL: index straight to `s % capacity` and confirm the hit with
	 * `.serial == s` (a serial older than the newest <capacity> has been
	 * overwritten), or scan -- the rings are tiny. `serial == 0` in a slot
	 * means the slot was never written. Like chat_lines_received and the
	 * map counters above, these are CUMULATIVE for the life of the impl:
	 * server_remote_start does not reset them.
	 *
	 * block_echo_serial / block_echoes -- one record per block change APPLIED
	 *     FROM THE SERVER: MP_EVENT_BLOCK_CHANGE (Packet53) once, and
	 *     MP_EVENT_MULTI_BLOCK_CHANGE (Packet52) once PER CELL, in the
	 *     packet's own cell order. This is the dig/place receipt: the cell
	 *     and the resulting id/meta the server named, recorded whether or not
	 *     the client's column was resident (a not-yet-resident edit is parked
	 *     in the deferred-edit log and replayed -- the SERVER still confirmed
	 *     it, which is what a driver is counting).
	 * txn_serial / txn_results -- one record per window-transaction RESULT
	 *     RECEIVED (MP_EVENT_TRANSACTION). Accepted and rejected are both
	 *     recorded; `accepted` is the server's own verdict.
	 * set_slot_serial / set_slots -- one record per MP_EVENT_SET_SLOT the
	 *     handler actually applied, and one per MP_EVENT_WINDOW_ITEMS batch
	 *     it actually delivered. A set-slot the existing handler already
	 *     DROPS (an out-of-range window/slot) is not recorded: it changed
	 *     nothing, so crediting it would be counting a non-event.
	 * world_install_generation -- how many remote world installs have
	 *     completed IN THIS PROCESS (the design's connection generation),
	 *     counted at the one site that finishes one,
	 *     remote_emit_load_completion_locked's false->true edge in
	 *     server_remote_pump. PROCESS-GLOBAL, not per-connection, for the
	 *     same reason backing_bytes above is: a rejoin frees the impl
	 *     (mp_session_release_remote) and builds a new one, and the whole
	 *     point of the counter is to tell gen 1 from gen 2 ACROSS that. It
	 *     therefore also advances on a dimension change, which re-arms the
	 *     same gate (MP_EVENT_RESPAWN clears remote_load_complete_sent): a
	 *     nether trip is a second world install, and a driver matching
	 *     receipts by generation must read it as one. Saturates rather than
	 *     wrapping, so it can never appear to go backwards. */
	uint32_t block_echo_serial;
	struct server_remote_block_echo block_echoes[SERVER_REMOTE_BLOCK_ECHO_RING];
	uint32_t txn_serial;
	struct server_remote_txn_result txn_results[SERVER_REMOTE_TXN_RING];
	uint32_t set_slot_serial;
	struct server_remote_set_slot set_slots[SERVER_REMOTE_SET_SLOT_RING];
	uint32_t world_install_generation;
	/* push-0921/B19 -- THE WATCHED CELL. The newest block echo for the ONE
	 * cell the script said it is waiting on (server_remote_play_watch), kept
	 * OUTSIDE block_echoes[] so a burst of unrelated block changes cannot
	 * evict it. `serial == 0` means none -- which is also what every non-play
	 * profile and normal play report, because nothing is ever stored here
	 * until a script arms the watch.
	 *
	 * WHY IT EXISTS. The ring is 16 entries and the fixture now streams and
	 * populates terrain around a moving player, so one Packet52 can be larger
	 * than the whole ring: PC run pc_mp_play_20260921_191831 lost its first
	 * two build legs to "server echoes overflowed the ring" -- the script's
	 * own dig receipt evicted by terrain it does not care about.
	 *
	 * SAME RECORD, SAME SERIAL. This is not a second reading of the packet:
	 * the value stored here is the record the ring entry got, serial included,
	 * assigned by the one counter. The core compares slot and ring serials
	 * against a single baseline (mpp_echo_for / mpp_echo_for_type in
	 * mp_play_core.h), so two counters would be a silent correctness bug. */
	struct server_remote_block_echo watch_echo;
	/* hw1030g (real-Wii mp_play_10m, build 53c75ff: "dig never echoed by the
	 * server" although the fixture logged the dig, the drop and the client
	 * fell into the hole). The slot above keeps only the NEWEST record, so a
	 * later echo for the same cell (water flowing in, a resend) masked the
	 * air the dig produced; and a server may answer with a Packet51 body
	 * instead of Packet52/53 (Notchian/mc173 PlayerInstance: more than 10
	 * changes in one column per tick are resent as a region), which never
	 * reached the slot at all.
	 *
	 * watch_history -- EVERY record stored for the watched cell since it was
	 *     armed (same records, same serials as watch_echo), the one with
	 *     history count n at index (n - 1) % SERVER_REMOTE_WATCH_HISTORY.
	 * watch_history_n -- how many records were stored since the arm/move.
	 * watch_bodies -- Packet51 bodies INSTALLED since the arm/move that
	 *     covered the watched cell (each one also stored a record: the
	 *     body's own voxel at that cell, see remote_record_watch_body).
	 * watch_clears -- cumulative: how many arm/move/off calls DROPPED a
	 *     stored record (the "re-armed after the echo landed" hypothesis
	 *     reads this number instead of guessing).
	 * All four are zero whenever no script armed a watch. Diagnostic and
	 * play-driver only: nothing in the game branches on them. */
	struct server_remote_block_echo watch_history[SERVER_REMOTE_WATCH_HISTORY];
	uint32_t watch_history_n;
	uint32_t watch_bodies;
	uint32_t watch_clears;
};

/* B17 H2 asked for a bound, and grepped that the tree had none. 696 is the
 * size struct server_remote_status had on main 73bc319c, measured with the
 * real toolchains: PC (x86_64-w64-mingw32) 696 B, Wii (powerpc-eabi -mrvl)
 * 584 B. One literal covers both because the Wii build is the smaller of the
 * two; a target on which this struct grows past its pre-lane PC size now
 * fails the BUILD rather than quietly growing six stack frames. Its visible
 * twin is tests/test_mp_s0_env.c's test_mp_status_struct_stays_small -- a
 * compile-time assert nobody has ever seen fail reads like one that cannot. */
_Static_assert(sizeof(struct server_remote_status) <= 696,
			   "struct server_remote_status grew past its pre-lane size (696 B on "
			   "main 73bc319c); play-driver state belongs in struct "
			   "server_remote_play_obs, not on every MP stack frame");

struct server_remote {
	void* impl;
	/* mp-split-0912/7: the terminal diagnostic must OUTLIVE the impl.
	 * server_remote_destroy frees the impl, and a 1 Hz heartbeat that
	 * samples server_remote_get_status afterwards used to read the
	 * impl-less zero fill -- which is how real-Wii run fixture_login_0912
	 * managed to log a handshake -> login -> kick -> clean close in ~400 ms
	 * and still report mp_state = mp_gen = mp_end = 0 in all 118 of its
	 * heartbeats. This snapshot is taken in server_remote_destroy BEFORE
	 * the impl is freed (so `state` is the terminal state the impl actually
	 * had, and mp_end/_phase/_rx, generation, logged_in, rx_bytes and
	 * reason are the live values) and is what get_status hands out while
	 * impl == NULL. Cleared whenever a new impl is created
	 * (server_remote_init) and re-asserted on every generation bump
	 * (server_remote_start), so a dead session's record can never be
	 * mistaken for a live one's and the NEXT teardown's snapshot wins.
	 * last_status is only meaningful while last_status_valid is true. */
	struct server_remote_status last_status;
	bool last_status_valid;
};

/* Terminal-status latch plumbing (mp-split-0912/7), defined in
 * server_remote.c next to server_remote_get_status -- the snapshot's only
 * reader -- and called from server_remote_transport.c's lifecycle
 * (init/start/destroy). Neither takes impl->lock: both touch the OUTER
 * handle only, so they are safe to call with the impl lock held or not.
 * _capture is a no-op for a handle whose impl never ran a connect attempt
 * (generation 0) -- there is no session to remember, so get_status keeps
 * its zero fill. */
void server_remote_status_latch_capture(struct server_remote* remote);
void server_remote_status_latch_clear(struct server_remote* remote);

bool server_remote_init(struct server_remote* remote);
void server_remote_destroy(struct server_remote* remote);
bool server_remote_start(struct server_remote* remote, const char* host,
						 uint16_t port, const char* username_utf8);
void server_remote_pump(struct server_remote* remote);
bool server_remote_send_keep_alive(struct server_remote* remote);
/* S2 harness/diagnostic chat send -- see the .c definition's comment. Not
 * wired to any player-facing UI in this slice. */
bool server_remote_send_chat(struct server_remote* remote,
							 const char* message_utf8);
bool server_remote_disconnect(struct server_remote* remote,
							  const char* reason_utf8);
void server_remote_cancel(struct server_remote* remote);
void server_remote_get_status(struct server_remote* remote,
							  struct server_remote_status* status);
/* hw1029b/G1: per-packet-kind wire counters, TELEMETRY ONLY (no behaviour
 * reads them). They let a real-Wii mp_* run prove the mp-effects / MP-BED
 * packets actually crossed the wire on the live session:
 *   rx = decoded and handed to process_event (counted before dispatch, so a
 *        packet the handler then ignores still counts as RECEIVED);
 *   tx = the frame was accepted by queue_frame_playing.
 * Per connection generation (the impl is calloc'd per connect, so they start
 * at 0 and are gone with the impl). Names are the JSON keys main.c emits. */
enum server_remote_kind {
	SR_KIND_RX_BED = 0,      /* 0x11 Packet17Sleep */
	SR_KIND_TX_LEAVE_BED,    /* 0x13 Packet19EntityAction state 3 */
	SR_KIND_RX_ATTACH,       /* 0x27 Packet39AttachEntity */
	SR_KIND_RX_BLOCK_ACTION, /* 0x36 Packet54PlayNoteBlock (note/piston) */
	SR_KIND_RX_AUX_SFX,      /* 0x3D Packet61DoorChange (playAuxSFX) */
	SR_KIND_RX_SELF_STATUS,  /* 0x26 Packet38EntityStatus for OUR entity id */
	SR_KIND_RX_METADATA,     /* 0x28 Packet40EntityMetadata (any entity) */
	SR_KIND_TX_USE_ITEM,     /* 0x0F Packet15Place air-use (-1,-1,-1, 255) */
	SR_KIND_COUNT
};
#define SERVER_REMOTE_KIND_NAMES                                              \
	{"mp_rx_bed", "mp_tx_leavebed", "mp_rx_attach", "mp_rx_blockact",        \
	 "mp_rx_auxsfx", "mp_rx_selfstatus", "mp_rx_meta", "mp_tx_useitem"}
/* Lock-guarded copy into out[SR_KIND_COUNT]. Returns false (and zero-fills)
 * when there is no live impl. */
bool server_remote_get_kind_counts(struct server_remote* remote,
								   uint32_t out[SR_KIND_COUNT]);
/* push-0921/B18 (review B17 H2): the observation seams' OWN accessor, separate
 * from server_remote_get_status so the six non-play status readers do not carry
 * 560 B of play-driver rings on their stacks (see struct
 * server_remote_play_obs). Same lock, same shape, same main-thread contract as
 * get_status: a plain lock-guarded copy of pump-thread-exclusive records, taken
 * only when someone asks.
 *
 * ONE CALLER by design -- main.c's mpp_hook_remote_play_obs, the mp_play_10m
 * driver's hook. Zero-fills `out` and leaves it zero-filled when there is no
 * impl, which is the same answer a fresh impl gives (every serial 0 = "never
 * written"), so the core cannot tell a torn read from an honest empty.
 *
 * world_install_generation is stamped even for a destroyed session, exactly as
 * get_status used to stamp it: it is PROCESS state, not session state. */
void server_remote_get_play_obs(struct server_remote* remote,
								struct server_remote_play_obs* out);
/* push-0921/B19: NAME THE ONE CELL THE SCRIPT IS WAITING ON, so its echo is
 * kept out of the ring's way (see struct server_remote_play_obs::watch_echo).
 *
 * `on == false` turns the watch off; x/y/z are then ignored. Takes the same
 * lock the recorder writes under, so the watch can be moved from the main
 * thread while the pump is recording.
 *
 * CLEARS the stored echo when the watch is turned OFF or moved to a DIFFERENT
 * cell -- the core moves the watch from the dig cell to the place cell inside
 * one build cycle, and a stale echo surviving that move would credit the place
 * with the dig's receipt. Re-stating the SAME cell is not a move and keeps the
 * echo, because the driver re-states the watch every frame.
 *
 * THE INERTNESS GATE. The ring records unconditionally (its records are simply
 * never read unless the play driver asks); this slot is stricter -- nothing is
 * stored until this function turns it on, and its only production caller is
 * the mp_play driver's per-frame hook, reachable only when mp_play_armed().
 * For every other profile and for normal play the slot stays serial 0. */
void server_remote_play_watch(struct server_remote* remote, bool on, int32_t x,
							  int32_t y, int32_t z);
void server_remote_trace_dump(struct server_remote* remote, FILE* output);
/* W1: fix THIS connection's admission radius, clamped to
 * [SERVER_REMOTE_ADMIT_RADIUS_MIN, SERVER_REMOTE_ADMIT_RADIUS_MAX]. Call it
 * right after a successful server_remote_start: start's own per-connection
 * reset defaults the field to SERVER_REMOTE_ADMIT_RADIUS_MAX, so a call
 * BEFORE start would be silently clobbered. Defined in
 * server_remote_chunks.c, next to the admission gate it feeds. */
void server_remote_set_admit_radius(struct server_remote* remote, int radius);

/* M3 readiness census (docs/mp_admission_budget_design_2026-09-13.md 2.6,
 * Astra gate-2 item 8). The loading screen's remote branch must define
 * "done" by what THIS PEER has actually announced, not by a radius the
 * client invented -- see tmp/jobs/mp-split-0912/m3-readiness-report.md: a
 * server whose own view loop sends 36 columns can never satisfy a 441-column
 * local-disk predicate, so every MP join used to be released by the 30 s
 * backstop instead of by readiness.
 *
 * One bounded pass over the entry table under remote->lock, no allocation:
 *  - announced_pending -- entries within Chebyshev `radius` of (cx, cz) that
 *    are ANNOUNCED / RETAINED / PROMOTING *and* belong to the epoch the
 *    consumer has already applied. These are the obligations the peer has
 *    committed to and the promotion pump will install. A stale-epoch entry
 *    (a new dimension's body retained before CRPC_WORLD_RESET swapped the
 *    world) is NOT an obligation of the episode now loading and is skipped.
 *  - resident_in_radius -- RESIDENT entries in the same disk. Diagnostic.
 *  - core_resident -- the player's column and its 8 neighbours are ALL
 *    REMOTE_CHUNK_RESIDENT. This is the safe core that the 30 s backstop may
 *    never bypass (Astra item 9). RESIDENT is not epoch-filtered: promotion
 *    only ever installs an entry whose epoch the consumer already applied
 *    (server_remote_chunks.c's publication gate), and an epoch change clears
 *    the whole table on the read thread, so a stale RESIDENT cannot exist.
 *  - core_announced -- how many of those 9 have an entry at all (any state
 *    other than UNUSED). Diagnostic: it separates "the peer has not told us
 *    about our own column yet" from "it did and installation is behind".
 *
 * A core column left TOMBSTONE (lossy eviction -- which design 2.2 removes
 * from the normal path) therefore keeps core_resident false for good, and the
 * loading screen holds with its visible waiting state. That is the intended
 * outcome, not an oversight: protocol 14 has no chunk re-request, so the
 * column can never be installed, and placing the player on it would be
 * placing them on terrain this client does not have. The screen's existing
 * no-progress escape hatch ("Connection stalled", 30 s) is the way out.
 *
 * Returns false (and zero-fills *out) when there is no live impl, so a caller
 * can never mistake "no session" for "nothing outstanding". `radius` < 0 is
 * treated as 0. Safe to call from the main thread every frame. */
struct server_remote_ready_census {
	size_t announced_pending;
	size_t resident_in_radius;
	bool core_resident;
	int core_announced;
	/* F5b (review findings M6, L12). The census answers WHICH disk it
	 * actually counted, because the caller's request is not the final word:
	 * admit_radius  -- the connection's live install radius (0 when the
	 *                  session has none yet).
	 * radius_used   -- the requested radius CLAMPED to [1, admit_radius].
	 *                  M6: mp.ready_radius was clamped only to
	 *                  [1, MAX_VIEW_DISTANCE], so `install_radius = 2,
	 *                  ready_radius = 5` counted every entry at Chebyshev
	 *                  3-5 as an obligation the admission gate would NEVER
	 *                  install -- announced_pending could not reach 0 and
	 *                  every join was released by the 30 s backstop, the
	 *                  exact defect the readiness gate exists to remove.
	 * centre_x / _z -- the column the count was centred on, and
	 * centre_authoritative -- whether that came from the server's admission
	 *                  centre (true) or fell back to the caller's camera
	 *                  column (false, i.e. no centre known yet). L12: the
	 *                  screen used to pass its own camera column while
	 *                  admission used the server's, so during a prediction
	 *                  divergence the screen demanded a disk the gate was
	 *                  not filling, with no log line explaining it. */
	int admit_radius;
	int radius_used;
	int centre_x, centre_z;
	bool centre_authoritative;
	/* ci-1101/54 (hw1102b betamc): in-radius, this-epoch, NOT-installed
	 * entries (ANNOUNCED / RETAINED) that the F4 memory gate is refusing
	 * right now. They are NOT counted in announced_pending: the gate only
	 * reopens when a demotion or a 0x32 unload frees the carve, neither of
	 * which happens while the player sits on the loading screen, so counting
	 * them held every dense-terrain join until the 30 s backstop
	 * (mp_ready_pending=14 flat, lw_to_admits=1). They still install in the
	 * background the moment the gate allows. PROMOTING entries and re-sends
	 * of installed columns are never held (they are past / outside the cold
	 * gate). Always 0 where there is no carve (PC). */
	size_t memory_held;
};
bool server_remote_ready_census(struct server_remote* remote, int cx, int cz,
								int radius,
								struct server_remote_ready_census* out);

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void server_remote_test_set_retained_byte_cap(struct server_remote* remote,
											 size_t bytes);
/* F4: override what the promotion gate reads for "MEM2 carve free bytes".
 * PC has no carve (mem2_carve_free_bytes is SIZE_MAX there), so without this
 * the gate is unreachable on the only platform the suite runs on. The
 * override is process-wide and not per-connection on purpose: it stands in
 * for a PLATFORM property, so a fresh session inherits it exactly as a Wii
 * session inherits the console's carve. Pass SIZE_MAX to go back to the real
 * reading, which is also what an untouched test gets. */
void server_remote_test_set_carve_free_bytes(size_t bytes);
/* REL-MP-LOADHANG: override the MESH RESERVE the promotion gate keeps free
 * on top of one column (REMOTE_MESH_RESERVE_BYTES: 6 MiB on the Wii, 0 on
 * PC). With the PC value of 0 the Wii hang -- a carve that holds a column but
 * not column + reserve -- cannot be reproduced, so a case injects the Wii
 * figure here. Process-wide for the same reason as the carve override; pass
 * SIZE_MAX to go back to the platform constant. */
void server_remote_test_set_mesh_reserve_bytes(size_t bytes);
/* W1 reconstruction proof (design section 4.1, Astra gate-2 item 10 "verify
 * reconstructed state byte-for-byte"). Writes the column at
 * (chunk_x, chunk_z) into four caller-owned planes in Packet51 slot layout
 * -- ids[(x*16 + z)*128 + y] (32768 B) and the three 4-bit planes at the
 * same index >> 1 (16384 B each, low nibble for even y). A RESIDENT column
 * is read back from the installed voxels; a BACKED one is reconstructed by
 * inflating its retained head body and applying nothing. Returns false when
 * the coordinate has no entry, the column is neither installed nor backed,
 * or the backing fails to inflate. */
bool server_remote_test_export_column(struct server_remote* remote,
									  int32_t chunk_x, int32_t chunk_z,
									  uint8_t* out_ids, uint8_t* out_metadata,
									  uint8_t* out_sky, uint8_t* out_torch);
#endif

#endif
