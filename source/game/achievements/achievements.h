/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * The achievement / statistic CONSUMER: the ring, the coalescer, the
 * acceptance matrix, the sixteen Beta unlock rules and the per-slot binding
 * lifecycle.
 *
 * Spec: docs/achievements_design.md sections 1.1 (the sixteen triggers), 1.2
 * (unlock semantics), 3 (identity and the bind contract), 5.1/5.2 (the seam,
 * the matrix, the no-loss envelope) and 6 (local vs remote columns).
 *
 * Division of labour inside source/game/achievements/:
 *   stats_table.h / achievements_state.*  -- the id table and the counters (B1)
 *   achievements_persist.*                -- the versioned .cwstats file  (B1)
 *   game_event.h                          -- the producer-facing seam     (B2)
 *   achievements.c (this)                 -- ring, matrix, rules, lifecycle (B2)
 *   the notice peek/ack and the screen data API                          (B4)
 *
 * Threading: game_event_emit() is callable from ANY thread. Everything else
 * in this header is MAIN-THREAD ONLY -- init, bind, unbind, tick, flush and
 * every diagnostic below mutate or read state that only the drain owns.
 */

#ifndef CW_ACHIEVEMENTS_H
#define CW_ACHIEVEMENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "achievements_persist.h"
#include "achievements_state.h"
#include "game_event.h"

/* One state per bound owner slot: P1 and the split-screen guest [design 3,
 * 7]. An event for a slot outside this range is unaddressable and is dropped
 * at emit (achievements_debug_bad_slot_drops()). */
#define CW_ACH_SLOT_COUNT 2

/* THE THREE QUEUES [design 5.2 v4, Astra call 2 + R2 HIGH 1].
 *
 * 1. the MAIN ring: every kind, in emit order, exact.
 * 2. the ORDERED OVERFLOW ring: when the main ring is full, every event
 *    CLASSIFIED ORDERED AT EMIT TIME goes here instead -- FIFO, never folded,
 *    never approximated. Full is NOT a fold: the event is rejected with
 *    ACH_REJECT_OVERFLOW and progress_incomplete is set -- loud, never silent.
 * 3. the per-slot COALESCER: FOLDABLE kinds only, i.e. pure counters whose
 *    order cannot change an achievement or a notice. Design sizes it 512
 *    against an estimated <= 320 distinct keys inside the envelope. Full is
 *    also loud: ACH_REJECT_COALESCE_FULL, never a silent drop (R2 MED 6).
 *
 * THE PER-CLASS ORDERED ENVELOPE THE OVERFLOW RING IS SIZED FOR (R2 HIGH 1).
 * v3 sized this ring against "one ordered client event per tick per rider plus
 * human-rate discrete events" while classifying EVERY pickup, craft take and
 * kill as ORDERED -- and the 64-events-per-player-per-tick envelope bounds
 * none of those three. Two players picking up a dozen item entities a tick
 * across a ten-second stall is 5,200 ORDERED events: inside the supported
 * envelope, past a 1024-entry ring. The class is now refined by PAYLOAD at
 * emit time (stats_table.h CW_EVENT_CLASS_ROWS), which bounds the ordered
 * share by Beta's own rates:
 *
 *     20 GE_TICK/s        (one per player per tick -- and the faithful
 *                          rail-origin reset, EntityPlayer.java:124-127)
 *   + 20 GE_RIDE/s        (minecart only; a rider emits at most one per tick)
 *   + 12 discrete/s       (achievement pickups 17/334, the 8 achievement
 *                          crafts, 265/350 furnace takes, hostile kills,
 *                          inventory opens, pig falls -- all hand-driven)
 *   = 52 per player per second; x 2 players x a 10 s stalled drain = 1,040.
 *
 * The ring is 2048: twice that envelope. */
#define CW_ACH_RING_CAPACITY 1024
#define CW_ACH_OVERFLOW_CAPACITY 2048
#define CW_ACH_COALESCE_CAPACITY 512

/* GuiAchievement's window [design 1.3]: d = (now - started_ms) / 3000, closed
 * outside 0..1. The info variant starts backdated so it holds on screen. */
#define ACH_NOTICE_WINDOW_MS 3000u
#define ACH_NOTICE_INFO_BACKDATE_MS 2500u

/* WHICH WORLD the session is in; picks the counter column [design 6]. Given
 * once at bind time -- producers never label provenance. */
enum ach_bind_mode {
	ACH_MODE_LOCAL_WORLD = 0,
	ACH_MODE_REMOTE_SERVER = 1,
};

/* [design 3 "Bind rules"]. B2 reports failures honestly. SAVE_PENDING retains
 * the previous owner's progress; retry flush before any replacement bind. */
enum ach_bind_status {
	ACH_BIND_OK = 0,
	/* NULL / empty / > 16 chars / a byte outside [a-z0-9_], an invalid mode,
	 * a slot outside
	 * 0..CW_ACH_SLOT_COUNT-1, or a module whose achievements_init() failed
	 * (nothing can be keyed without a data root either). */
	ACH_BIND_BAD_KEY,
	/* That profile_id is already bound on another slot: two players must
	 * never share one file. */
	ACH_BIND_DUPLICATE,
	/* This slot is still bound; unbind has to run first. */
	ACH_BIND_BUSY,
	/* Unsaved progress is retained in this slot. Main-thread flush_all must
	 * succeed before any reload/reuse; a retained key on another slot is still
	 * DUPLICATE, because both owners must never write one file. */
	ACH_BIND_SAVE_PENDING,
};

/* Why an event did not reach the counters. Every drop is counted by reason so
 * a producer bug names itself instead of degrading silently [design 5.2]. */
enum ach_reject_reason {
	ACH_REJECT_UNBOUND = 0,		 /* nothing bound on that slot right now */
	ACH_REJECT_STALE_GENERATION, /* a previous player's event */
	ACH_REJECT_BAD_EVENT,		 /* kind/sub/payload out of range */
	ACH_REJECT_WRONG_OBSERVER,	 /* authority does not own this class here */
	ACH_REJECT_NO_REMOTE_SERVER, /* PACKET200 while in a local world */
	ACH_REJECT_NO_LOCAL_SERVER,	 /* SERVER authority while on a remote one */
	ACH_REJECT_UNKNOWN_STAT,	 /* a Packet 200 id this build has no counter for */
	/* achievements_unbind() has taken the slot's lock and is releasing it:
	 * from that instant nothing new may enter the ring or the coalescer, so
	 * the emit is dropped HERE and counted, instead of landing in a coalescer
	 * that the release is about to clear [design 3, R1 MED 8]. */
	ACH_REJECT_CLOSING,
	/* The main ring AND the ordered overflow ring are both full, so an
	 * ORDERED event -- one that can change an achievement or a notice -- has
	 * nowhere exact to go [design 5.2 v4]. It is REJECTED here rather than
	 * folded, and `progress_incomplete` is set: outside the supported
	 * envelope the module says so instead of silently approximating. */
	ACH_REJECT_OVERFLOW,
	/* The per-slot COALESCER is full, so a FOLDABLE event -- a pure counter --
	 * has nowhere to go either. Design 5.2 calls this unreachable inside the
	 * envelope; if it happens the count IS lost, so it is a counted rejection
	 * with progress_incomplete set, not an anonymous bump of `ring_folds`
	 * (R2 MED 6: the old code counted the loss as if it had folded). */
	ACH_REJECT_COALESCE_FULL,
	ACH_REJECT_COUNT
};

/* Why achievements_init() failed, or ACH_INIT_OK. Readable at any time
 * through achievements_init_status(), and carried in every ach_status, so a
 * screen can say WHY there is no progress tracking instead of showing zeros
 * [design 7; R1 MED 4, LOW 13]. */
enum ach_init_status {
	ACH_INIT_OK = 0,
	ACH_INIT_NEVER_RUN,		 /* achievements_init() has not been called */
	ACH_INIT_NO_MUTEX,		 /* the one module mutex could not be created */
	ACH_INIT_BAD_ARG,		 /* NULL/empty data root, or a path that does not fit */
	ACH_INIT_BAD_STATS_DIR,	 /* <root>/stats is not a directory and cannot be made */
	ACH_INIT_NO_MEMORY,		 /* the module's one block could not be allocated */
	/* Re-init refused to discard dirty owners. Old root/state remain live,
	 * producers are closed; retry flush/init after the storage error clears. */
	ACH_INIT_SAVE_PENDING,
};

/* The toast [design 1.3, 7]. B4 ships achievements_notice_peek/ack on top of
 * this type; B2 only keeps the one slot per owner and stamps it.
 *
 * `hint_active` is NOT a toast (R1 LOW 11). Beta's
 * EntityPlayerSP.onLivingUpdate re-queues the openInventory hint every tick
 * while the achievement is locked; queueing it as a notice made
 * `notices_queued` a tick counter and reset `started_ms` 20 times a second, so
 * an acknowledgement could never match one. It is therefore a persistent
 * FLAG: set while openInventory is locked, cleared when it unlocks, never
 * counted, and it never touches `ach_id` or `started_ms`. */
struct ach_notice {
	int16_t ach_id;		/* 0..15, or -1 when there is no toast */
	uint32_t started_ms; /* module clock; see achievements_debug_now_ms() */
	bool hint_active;	/* draw the openInventory hint for this owner */
};

/* Per-slot diagnostic state. B4 exposes it as achievements_status(slot, &st)
 * for the screens; B2 fills it and reads it back through
 * achievements_debug_status(). */
struct ach_status {
	/* Bind-time state ------------------------------------------------- */
	bool bound;
	/* achievements_unbind() is draining/writing this slot right now: still
	 * bound, but no new event may enter [design 3, R1 MED 8]. */
	bool closing;
	uint8_t init_status;	 /* enum ach_init_status, module-wide */
	uint8_t mode;			 /* enum ach_bind_mode */
	uint64_t generation;	 /* the S1-a2 generation this slot is bound at */
	char profile_id[CW_PROFILE_ID_MAX + 1];
	/* Persistence ------------------------------------------------------ */
	uint8_t load;			 /* enum stats_load_status, from the bind load */
	uint8_t last_save_error; /* enum stats_save_status of the last attempt */
	uint32_t save_failures;
	/* Applied events not yet on disk. When !bound, this is a retained owner:
	 * its profile_id/error remain queryable, and bind refuses to overwrite it. */
	bool dirty;
	/* Event accounting ------------------------------------------------- */
	uint32_t accepted;
	uint32_t rejected[ACH_REJECT_COUNT];
	/* An achievement event whose PARENT was locked, which Beta drops entirely
	 * (design 1.2 point 1). Not a rejection and not a bug: it is the rule
	 * working. It used to be counted in rejected[ACH_REJECT_PARENT_LOCKED]
	 * even when the same event applied a counter and was therefore also
	 * counted in `accepted`, so one event showed up on both sides of the
	 * ledger (R2 LOW 11). `accepted` counts the counter that DID apply; this
	 * counts the award that did not. */
	uint32_t ach_dropped_parent_locked;
	/* Events Beta keeps no statistic for at all, dropped at emit before they
	 * ever queue: a GE_ITEM_PICKED_UP of anything but wood 17 or leather 334
	 * (Beta has no items-picked-up stat, so the pickup is only ever the
	 * trigger of mineWood / killCow). Not a rejection -- nothing was lost,
	 * there was nothing to keep -- and not `accepted` either (R2 HIGH 1). */
	uint32_t noop_events;
	uint32_t ring_folds;	 /* FOLDABLE events that DID fold (M6: folds only) */
	uint32_t coalesce_used;	 /* distinct keys held right now */
	uint32_t coalesce_peak;	 /* high-water mark of the above */
	/* THIS SLOT's entries in the shared ordered overflow ring: how many are
	 * queued right now and the high-water mark. The ring itself is one FIFO
	 * across both slots (like the main ring), so the two slots' `used` sum to
	 * its occupancy and each is <= CW_ACH_OVERFLOW_CAPACITY. */
	uint32_t overflow_used;
	uint32_t overflow_peak;
	/* Something this slot should hold is not in it: the coalescer ran out of
	 * keys, or an ORDERED event hit ACH_REJECT_OVERFLOW. Either way counts
	 * were lost and the file header records it -- so every site that sets this
	 * also marks the slot DIRTY, or the flag would never reach the file it is
	 * supposed to be recorded in (R2 MED 7). */
	bool progress_incomplete;
	/* Toast ------------------------------------------------------------ */
	uint32_t notices_queued; /* TOASTS queued; the info hint is not one */
};

/* Remember the data root (the directory that holds config.json), allocate the
 * module's one state block (below) and create `<data_root>/stats` if it is
 * missing -- the ONE place that creates it, since the B1 persist layer
 * deliberately never does and fails loudly into a missing directory
 * [achievements_persist.h header contract]. `<data_root>/stats` must be a
 * DIRECTORY: an EEXIST that is a regular file is a failure, not a success
 * (R1 LOW 13).
 *
 * Returns false on a bad argument, a stats directory that cannot be created,
 * or a block that cannot be allocated; the module is then DISABLED -- every
 * emit is dropped, every query is empty -- and achievements_init_status() says
 * which it was.
 *
 * Idempotent: calling it again flushes every dirty bound profile against the
 * OLD root first (a re-init never drops applied progress), then unbinds every
 * slot and clears the ring, the coalescers, the notices and the
 * diagnostics. A failed flush refuses re-init, retaining the OLD root and
 * all progress with producers closed (ACH_INIT_SAVE_PENDING). The root is
 * made absolute at init: call before changing the world cwd. B2 must retain
 * this startup root and reuse it for future sessions, never pass a world cwd.
 * The state block is allocated once and reused. */
bool achievements_init(const char* data_root);

/* Main-thread startup, before any provider starts. Keys are copied only when
 * both are valid and distinct. Invalid configuration disables lifecycle binds. */
bool achievements_set_profiles(const char* player1, const char* player2);
/* Main-thread Retry: retry retained profiles independently and reconcile routes. */
bool achievements_lifecycle_retry(void);
bool achievements_lifecycle_pending(void);
bool achievements_slot_save_failed(uint8_t slot);
/* Main-thread modal acknowledgement suppresses repeat terminal prompts only;
 * dirty state, HUD error and explicit Retry remain available. */
bool achievements_save_error_acknowledged(uint8_t slot);
void achievements_acknowledge_save_error(uint8_t slot);
/* Production frame budget: cooperative saves; legacy explicit flush APIs
 * remain synchronous and are reserved for off-frame teardown/tests. */
void achievements_set_cooperative_saves(bool enabled);
void achievements_request_save(uint8_t slot);
bool achievements_save_busy(uint8_t slot);
bool achievements_save_durable(uint8_t slot);
uint32_t achievements_save_serial(uint8_t slot);
/* Explicit user opt-out: best-effort .unsaved survivor, then retain RAM while
 * gameplay continues. Does not erase/replace any committed profile. */
/* During play, spread the independent survivor over ordinary main ticks. */
void achievements_request_rescue(uint8_t slot);
/* Synchronous best effort reserved for the final, quiescent exit choice. */
bool achievements_rescue_unsaved(uint8_t slot);
void achievements_shutdown_without_save(void);

/* Why the last achievements_init() failed, or ACH_INIT_OK. Always callable,
 * including before the first init and while the module is disabled. */
enum ach_init_status achievements_init_status(void);

/* Release the module's state block [design 5.2 "Memory placement", R1 MED 4].
 * Flushes every dirty bound profile first, exactly as a re-init does, then
 * frees the block; the module is disabled until the next achievements_init(),
 * which allocates a fresh one. Returns false if a flush failed: the block,
 * root and dirty progress stay resident with producers closed for retries.
 * B2 must report the storage error before allowing process exit. */
bool achievements_shutdown(void);

/* Runtime-armed allocator seam for the one state block
 * (docs/build_conventions.md: always compiled in, inert until armed). NULL/NULL
 * -- the default -- means the module's own pair: mem2_alloc/mem2_free, i.e. the
 * Wii MEM2 carve (never MEM1 .bss) and a plain aligned malloc on PC. Installing
 * a pair affects the NEXT allocation only, so arm it before achievements_init()
 * and pair it with achievements_shutdown() when a block is already held. */
void achievements_set_allocator(void* (*alloc_fn)(size_t),
								void (*free_fn)(void*));

/* Size of that one block, for the memory budget and the test that pins the
 * comment in achievements.c against the actual layout. */
size_t achievements_block_bytes(void);
uint32_t achievements_debug_coalescer_rebuilds(void);

/* Injected wall clock for the `saved` header of the .cwstats file; a straight
 * passthrough to the B1 persistence layer so a caller never has to know both
 * module names. NULL restores time(NULL). */
void achievements_set_clock(uint32_t (*now)(void));

/* Bind `slot` to `profile_id` for this session [design 3]. On OK the profile
 * file is loaded through the B1 persist layer, the slot's transient state
 * (ride origin, notice, coalescer) and its diagnostics are cleared, and every
 * later event must carry exactly this `generation`. */
enum ach_bind_status achievements_bind(uint8_t slot, uint64_t generation,
									   const char* profile_id,
									   enum ach_bind_mode mode);

/* Release `slot` [design 3 "Release / rebind order"]: under ONE lock hold mark
 * the slot CLOSING and drain every queued event into it, then (outside the
 * lock) flush the file if dirty, then clear the ride origin and the notice and
 * mark the slot unbound. Nothing can enter the ring or the coalescer between
 * the mark and the drain, so no event is ever discarded by the release without
 * being counted (R1 MED 8): an emit that races the unbind is either applied or
 * rejected as ACH_REJECT_CLOSING. A generation that does not match the current
 * binding changes nothing and is counted as a stale-generation rejection.
 * Returns false on failure. A failed save releases gameplay ownership but
 * retains the dirty profile in this slot; bind returns SAVE_PENDING until
 * flush_all succeeds. Pending notices are cleared on every release. */
bool achievements_unbind(uint8_t slot, uint64_t generation);

/* Pop `slot`'s FIFO front -- what B4's achievements_notice_ack() does once
 * a view has finished drawing it (Brief E2). Also clears the legacy B2
 * diagnostic snapshot, preserving its historical clear semantics. Does NOT touch
 * `hint_active`, which is a property of the profile, not of a toast: the
 * openInventory hint stays up until the achievement unlocks. A no-op for a bad
 * slot or a slot with no toast. Main thread only. */
void achievements_notice_clear(uint8_t slot);

/* B4's locked FIFO reader (Brief E2). Unlike debug_notice's historical
 * most-recent-unlock snapshot, this returns the oldest unacknowledged notice,
 * with the independent persistent hint filled even when the FIFO is empty. */
bool achievements_notice_front(uint8_t slot, struct ach_notice* out);

/* The main-thread drain [design 5.2 v4 "Drain order"]: the main ring in FIFO
 * order, then the ordered overflow ring in FIFO order (so every event that can
 * change an achievement or a notice is applied in emit order, whichever of the
 * two it queued in), then each slot's coalescer, which holds pure counters
 * only and whose position therefore cannot matter. Called once per client
 * display tick. */
void achievements_tick(void);

/* Drain, then write every dirty bound profile [design 5.2 "Lifecycle /
 * quiescence" step 2]. A failed write leaves the state dirty in memory and is
 * recorded in that slot's status. Includes dirty UNBOUND retained profiles.
 * Returns true only when all drained progress is durable. Producers may have
 * queued new facts since the drain: B2 must quiesce them at session boundaries.
 * Synchronous off-frame API; production save/leave uses request_save + tick. */
bool achievements_flush_all(void);

/* ---- B2-internal diagnostics --------------------------------------------
 * The public, owner-explicit screen/data API (achievements_status,
 * achievements_notice_peek/ack, the per-row queries) is brief B4. These four
 * exist so B2's own acceptance tests can assert what B2 is responsible for --
 * "dropped AND COUNTED", "the toast carries these fields", "slot 1's notice
 * never appears on slot 0" -- without B4 having to land first. B4 wraps the
 * same state; nothing here formats, iterates or sorts anything. */
bool achievements_debug_status(uint8_t slot, struct ach_status* out);
bool achievements_debug_notice(uint8_t slot, struct ach_notice* out);
const struct stats_profile* achievements_debug_profile(uint8_t slot);
/* The module's own millisecond clock, i.e. the stamp achievements_tick()
 * would put on a notice queued right now. */
uint32_t achievements_debug_now_ms(void);
/* Events emitted for a slot outside 0..CW_ACH_SLOT_COUNT-1: unaddressable, so
 * they are dropped at emit and cannot be counted per slot. */
uint32_t achievements_debug_bad_slot_drops(void);
/* Runtime-armed seam, inert (NULL) unless a test arms it: called ONCE per
 * flush, with s_lock HELD, in the middle of the status update that records the
 * result. It exists so a test can prove that a concurrent status snapshot
 * cannot observe a half-updated (save_failures, last_save_error) pair -- the
 * snapshot blocks on the lock instead of walking into the gap (R1 MED 7).
 * One-shot: cleared as soon as it fires. */
void achievements_debug_status_barrier(void (*fn)(void));
/* Runtime-armed seam, inert (NULL) unless a test arms it: called from inside
 * the drain, with s_lock HELD, ONCE PER TOAST QUEUED, in queue order.
 * The B2 diagnostic still snapshots the latest queued unlock, while B4 reads
 * the FIFO front (Brief E2). This observer verifies the complete sequence
 * independently of presentation acknowledgements -- exactly what the F2
 * reference comparison has to check ("the notice outcomes", design 5.2 v4
 * "Proof of equivalence"). Not one-shot: it stays armed until re-armed with
 * NULL. The openInventory hint is not a toast and never fires it. */
void achievements_debug_notice_hook(void (*fn)(uint8_t slot, int16_t ach_id));

#endif /* CW_ACHIEVEMENTS_H */
