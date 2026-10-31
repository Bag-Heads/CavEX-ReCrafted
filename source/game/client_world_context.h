#ifndef CLIENT_WORLD_CONTEXT_H
#define CLIENT_WORLD_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

/* A context identifies one client's dimension and owns its terrain, meshes,
 * entities, particles, signs, moving pistons, clock, weather, and render state.
 * Those payloads are wired in after integration; this module owns routing and
 * lifetime metadata only. world_index is 0 or 1; view bits are 0 and 1.
 *
 * TWO identities, validated separately (ASTRA-B1-PHASE1-0925 #2):
 *  - server_generation: the AUTHORITATIVE server-world identity (struct
 *    server_world.generation, carried by every world-scoped RPC as
 *    world_generation). Bound once per context (client_ctx_bind_server, from
 *    the lifecycle message that created it); 0 = unbound, rejects every RPC.
 *    RPC identity is checked with client_ctx_accept_rpc.
 *  - generation: the client's monotonic context/work EPOCH, issued from a
 *    per-slot lifetime high-water mark (below). Captured by asynchronous work
 *    this client starts (mesher jobs, GPU uploads) and checked on completion
 *    with client_ctx_accept. It is NEVER mirrored from, compared to, or seeded
 *    from the server generation: the two counters advance independently (a
 *    scoped reset advances the epoch and leaves the server identity alone), so
 *    a numeric coincidence between them must not pass either check.
 * A scoped reset changes only this context's epoch, preserving bound views,
 * the server identity and outstanding leases. A successor in the same slot --
 * reconnect, respawn, world switch -- never shares an epoch with any
 * predecessor, so a predecessor's in-flight completions cannot publish into it
 * (ruling #3: overlapping lifetimes are permitted; each context keeps its own
 * leases and payloads, and the owner defers a successor's activation when the
 * overlap would not fit the aggregate memory/retirement budget).
 *
 * Identity acceptance is NOT payload readiness (ASTRA-B1-PHASE1-0925 #1): a
 * message may be for this context (accept_rpc / accept true) while its
 * payloads may not be mutated (client_ctx_payload_ready false) because the
 * context is quiescing after a reset, retiring, or not yet bound. Owners check
 * identity to ROUTE and readiness to APPLY.
 *
 * Leases are counted per kind, NOT per generation: after a reset the HELPER
 * quiesces producers (no new lease of any kind for this context) until every
 * lease count reaches 0 AND the owner has completed the old epoch's payload
 * cleanup (client_ctx_quiesce_complete); only then may producers resume. The
 * helper itself performs no cleanup. Every in-flight mesher job and retained
 * GPU resource needs one lease. Retirement rejects traffic, new binds, and new
 * leases. Existing leases may still release. try_free is a permission check,
 * not a call to free(3). The owner must stop producers before retirement and
 * free payloads only after try_free succeeds.
 *
 * THREADING: every operation, INCLUDING client_ctx_lease_release, runs on the
 * owner (client) thread. The counters are plain integers. A producer thread
 * (mesher worker, GPU completion) must marshal its release to the owner (the
 * completion queue the owner drains each frame) or hold the owner's explicit
 * synchronization; a direct concurrent release is unsupported and, with
 * asserts on, trips the re-entrancy tripwire in client_ctx_lease_release. */
enum client_ctx_lease_kind {
	CLIENT_CTX_LEASE_MESHER,
	CLIENT_CTX_LEASE_GPU,
	CLIENT_CTX_LEASE_COUNT
};

struct client_world_context {
	int dimension;
	uint8_t world_index;
	uint8_t bound_views;
	uint32_t generation;        /* client context/work epoch (per-slot mark) */
	uint32_t server_generation; /* authoritative server world identity; 0 = unbound */
	/* Bound to an UNSTAMPED server stream (B1 phase 2 transition): the
	 * lifecycle message that created this context carried no server world
	 * generation because its producer has not adopted the stamp yet (the
	 * stamps are applied inside the A/C server seams). Mutually exclusive
	 * with server_generation != 0. See client_ctx_registry_route. */
	bool unstamped;
	uint32_t leases[CLIENT_CTX_LEASE_COUNT];
	bool retiring;
	/* Set by a reset taken with leases outstanding; cleared only by
	 * client_ctx_quiesce_complete once every kind has drained. */
	bool quiescing;
	/* Debug-only re-entrancy tripwire for the threading rule above. */
	bool in_release;
};

bool client_ctx_init(struct client_world_context* ctx, int dimension,
					  unsigned world_index);
/* Bind the authoritative server-world identity. Exactly once per context
 * (a new server world = a new context); refuses 0, a retiring context, or a
 * context already bound. */
bool client_ctx_bind_server(struct client_world_context* ctx,
							uint32_t server_generation);
/* Bind the context to an UNSTAMPED server stream instead (same once-only and
 * retiring rules as client_ctx_bind_server). Such a context is payload-ready
 * like a bound one, but client_ctx_accept_rpc still matches nothing for it:
 * unstamped traffic is routed only by client_ctx_registry_route's
 * single-live-slot rule. */
bool client_ctx_bind_unstamped(struct client_world_context* ctx);
bool client_ctx_bind_view(struct client_world_context* ctx, unsigned view);
bool client_ctx_unbind_view(struct client_world_context* ctx, unsigned view);
/* RPC identity: is this (world_index, server world_generation)-stamped
 * message for this context? Routing only; see client_ctx_payload_ready. */
bool client_ctx_accept_rpc(const struct client_world_context* ctx,
						   unsigned world_index, uint32_t server_generation);
/* Work identity: is this (world_index, epoch)-stamped asynchronous completion
 * for this context's CURRENT epoch? Routing only. */
bool client_ctx_accept(const struct client_world_context* ctx,
				   unsigned world_index, uint32_t generation);
/* May this context's payloads be mutated right now? False while quiescing,
 * retiring, or unbound. Owners gate every payload write on this, after the
 * identity check that routed the message here. */
bool client_ctx_payload_ready(const struct client_world_context* ctx);
bool client_ctx_reset(struct client_world_context* ctx);
bool client_ctx_lease_acquire(struct client_world_context* ctx,
					   enum client_ctx_lease_kind kind);
bool client_ctx_lease_release(struct client_world_context* ctx,
					   enum client_ctx_lease_kind kind);
void client_ctx_retire(struct client_world_context* ctx);
bool client_ctx_try_free(const struct client_world_context* ctx);

/* Per-slot lifetime epoch high-water mark (ASTRA-B1-RULINGS-0925 #1).
 * Every epoch init/reset issues for a world_index slot is greater than every
 * epoch that slot ever issued in this client lifetime, across the destruction
 * of the context that issued it. Never wraps, never reuses: when the slot has
 * issued UINT32_MAX, init and reset fail and leave the context they were
 * given unchanged. _high_water reads the mark (0 for an unused or
 * out-of-range slot). _high_water_raise only ever RAISES the mark (a floor,
 * so it cannot break monotonicity): it returns false and changes nothing for
 * an out-of-range slot or a value not above the current mark. Live contexts
 * are untouched by a raise; they keep accepting their own epoch.
 * _raise is NOT a mirroring mechanism (ASTRA-B1-PHASE1-0925 #2 rejected
 * raise(server_generation - 1)): the epoch is never made to equal the server
 * generation. Its uses are the exhaustion test and, if ever needed, carrying
 * a persisted client epoch across a client restart. */
uint32_t client_ctx_slot_high_water(unsigned world_index);
bool client_ctx_slot_high_water_raise(unsigned world_index, uint32_t floor);

/* Reset quiesce (ASTRA-B1-RULINGS-0925 #2, ratified ASTRA-B1-PHASE1-0925 #1).
 * A reset taken while any lease of any kind is outstanding parks the context:
 * every lease kind is refused until every kind has drained AND the owner has
 * finished cleaning up / replacing the old epoch's payloads. The last release
 * never re-opens admission. _quiesce_drained is the owner's poll: true once
 * every pre-reset lease is back (the old payloads are now unreferenced and
 * may be freed). _quiesce_complete is the owner's acknowledgement that the
 * payloads are ready: it re-admits producers and returns true; it returns
 * false and changes nothing while leases are still outstanding, when no
 * quiesce is pending, or on a retiring context (retirement never resumes
 * acquisition -- the owner uses try_free instead). A reset with zero leases
 * outstanding has nothing to drain: the owner cleans up synchronously inside
 * its own serialized reset/install step, so producers resume at once. */
bool client_ctx_quiesce_drained(const struct client_world_context* ctx);
bool client_ctx_quiesce_complete(struct client_world_context* ctx);

/* ---- Registry: the client owner's context storage and RPC routing ---------
 * (B1 phase 2.) One CURRENT context per world_index slot plus retiring
 * predecessors that still hold leases (ruling #3: a successor may start before
 * its predecessor drains). Entries are stable storage: a lease holder keeps a
 * pointer to the exact context it leased from, and the entry is reclaimed only
 * once that context is retiring with every lease back (client_ctx_try_free).
 *
 * POOL BOUND. Every retiring entry holds >= 1 lease. Mesher leases are capped
 * by the mesher token pool (CHUNK_MESHER_QLENGTH == 8; the bound is a
 * _Static_assert in client_world_context.c) and at most one GPU lease exists
 * per world at a
 * time (the previous frame's render hand-off), so at most 8 + 1 retiring
 * contexts coexist with the 2 current ones: 12 entries always suffice. An
 * install that still finds no free entry fails closed (NULL) and the owner
 * treats it like any other world-install allocation failure.
 *
 * THREADING: owner thread only, like every context operation. */
#define CLIENT_CTX_SLOT_COUNT 2
#define CLIENT_CTX_POOL 12

enum client_ctx_route {
	/* Identity matches the slot's current context and its payloads may be
	 * mutated: apply the message. */
	CLIENT_CTX_ROUTE_APPLY,
	/* Identity matches but the context is quiescing after a scoped reset: the
	 * owner must HOLD the message (stop draining, keep FIFO order) until it
	 * completes the quiesce, never drop it and never apply it early. */
	CLIENT_CTX_ROUTE_HOLD,
	/* Stamped for a server world generation that is not the slot's current
	 * one (a retired predecessor, or a successor this client never saw):
	 * drop, releasing any pooled payload. */
	CLIENT_CTX_ROUTE_STALE,
	/* No current context for the slot (before the first world install, after
	 * teardown, or an out-of-range slot): drop. */
	CLIENT_CTX_ROUTE_NO_CONTEXT,
	/* Unstamped while more than one slot is live: the producer did not say
	 * which world it belongs to and the client cannot guess. Drop and count --
	 * a contract violation by the producer, never a fallback. */
	CLIENT_CTX_ROUTE_AMBIGUOUS,
	/* Routed to a live context that does not own the client's (single) world
	 * payload (client_ctx_registry_route_payload): applying it would write
	 * another world's traffic into this payload. Drop and count. */
	CLIENT_CTX_ROUTE_FOREIGN,
	CLIENT_CTX_ROUTE_COUNT
};

struct client_ctx_registry {
	struct client_world_context pool[CLIENT_CTX_POOL];
	bool used[CLIENT_CTX_POOL];
	/* 1-based pool index of each slot's current context; 0 = none, so a
	 * zero-initialized registry is valid and empty. */
	uint8_t current[CLIENT_CTX_SLOT_COUNT];
	/* Routing outcomes, for tests and telemetry (never reset by install). */
	uint32_t routed[CLIENT_CTX_ROUTE_COUNT];
	uint32_t install_failures;
};

/* Install a new current context for world_index: retire the slot's previous
 * current context (reclaiming it at once when it holds nothing), take a free
 * entry, init it (fresh epoch from the per-slot mark) and bind it -- to
 * server_generation when nonzero, else to the unstamped stream. Returns NULL
 * and leaves the slot with NO current context when no entry is free or the
 * slot's epochs are exhausted (the predecessor is retired either way: a new
 * server world has replaced it). */
struct client_world_context* client_ctx_registry_install(
	struct client_ctx_registry* reg, int dimension, unsigned world_index,
	uint32_t server_generation);
struct client_world_context* client_ctx_registry_current(
	struct client_ctx_registry* reg, unsigned world_index);
/* Retire the slot's current context (teardown with no successor). */
void client_ctx_registry_retire(struct client_ctx_registry* reg,
								unsigned world_index);
/* Is ctx one of this registry's in-use entries (current or retiring)? */
bool client_ctx_registry_owns(const struct client_ctx_registry* reg,
							  const struct client_world_context* ctx);
/* Release one lease against the exact context it was acquired from and
 * reclaim that entry if it is now retiring and fully drained. Returns false
 * (and changes nothing) for a context this registry does not own or a lease
 * that is not outstanding. */
bool client_ctx_registry_release(struct client_ctx_registry* reg,
								 struct client_world_context* ctx,
								 enum client_ctx_lease_kind kind);
/* Owner's per-frame poll: complete the quiesce of every current context that
 * has drained (the owner performs its old-payload cleanup synchronously in
 * the reset step itself, so drained == cleaned up) and reclaim every
 * retiring entry that try_free allows. Returns true while any current
 * context is still quiescing (the owner keeps its lease holders draining). */
bool client_ctx_registry_poll(struct client_ctx_registry* reg);
/* Number of in-use entries (current + retiring). */
unsigned client_ctx_registry_in_use(const struct client_ctx_registry* reg);
/* Route one world-scoped message stamped (world_index, world_generation).
 * world_generation != 0: the slot's current context must accept it
 * (client_ctx_accept_rpc), else STALE / NO_CONTEXT. world_generation == 0
 * (UNSTAMPED, producer not converted yet): routed to the ONE live slot's
 * current context, whatever world_index says, and AMBIGUOUS when two slots
 * are live. A routed message is APPLY when the context is payload-ready and
 * HOLD otherwise. *out receives the context for APPLY/HOLD, NULL otherwise.
 * Every outcome is counted in reg->routed. */
enum client_ctx_route client_ctx_registry_route(
	struct client_ctx_registry* reg, unsigned world_index,
	uint32_t world_generation, struct client_world_context** out);
/* Single-payload owner's route (Fable B1-phase2 F1): as
 * client_ctx_registry_route, but an APPLY/HOLD verdict whose context is not
 * payload_slot's CURRENT context becomes FOREIGN (dropped): while the client
 * owns one world payload only that payload's context may mutate it. Counted
 * once, under the final verdict. */
enum client_ctx_route client_ctx_registry_route_payload(
	struct client_ctx_registry* reg, unsigned payload_slot,
	unsigned world_index, uint32_t world_generation,
	struct client_world_context** out);

#endif
