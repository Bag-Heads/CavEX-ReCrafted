/* Split-dims B1 phase 2: the client world-context REGISTRY that client
 * routing is wired to (source/game/client_world_context.[ch]).
 *
 * Pins, against the ratified contracts (ASTRA-B1-RULINGS-0925 #1-#3,
 * ASTRA-B1-PHASE1-0925 #1/#2) and out/B1-rpc-audit.md:
 *  1. stamped routing: a message stamped with the slot's current server
 *     generation APPLIES; any other generation is STALE; a slot with no
 *     context is NO_CONTEXT -- including after a successor replaced the
 *     predecessor whose generation the message carries;
 *  2. unstamped routing (producers not converted yet): routed to the ONE live
 *     slot whatever world_index says; AMBIGUOUS (dropped, never guessed) as
 *     soon as two slots are live; a stamped message never matches an
 *     unstamped-bound context and vice versa;
 *  3. identity is not readiness: a quiescing context HOLDS (never APPLY,
 *     never drop) until its leases drain and the owner poll completes it;
 *  4. overlap: a predecessor retires with its leases, stays owned until they
 *     are released against IT, its epoch never passes client_ctx_accept in
 *     the successor, and it is reclaimed on the last release;
 *  5. pool bound: 2 current + 8 mesher-leased + 1 GPU-leased predecessors
 *     fit in CLIENT_CTX_POOL; a further install fails closed and counts. */
#include "client_context_red.h"

#include <string.h>

static enum client_ctx_route route(struct client_ctx_registry* reg,
								   unsigned idx, uint32_t gen) {
	return client_ctx_registry_route(reg, idx, gen, NULL);
}

static int stamped_routing(void) {
	static struct client_ctx_registry reg;
	memset(&reg, 0, sizeof reg);
	PRE(route(&reg, 0, 7) == CLIENT_CTX_ROUTE_NO_CONTEXT);
	struct client_world_context* a
		= client_ctx_registry_install(&reg, 0, 0, 7);
	PRE(a && a->server_generation == 7 && !a->unstamped);
	struct client_world_context* out = NULL;
	CONTRACT(client_ctx_registry_route(&reg, 0, 7, &out)
				 == CLIENT_CTX_ROUTE_APPLY && out == a,
			 "current server generation applies to its context");
	CONTRACT(route(&reg, 0, 8) == CLIENT_CTX_ROUTE_STALE,
			 "a foreign generation is stale");
	CONTRACT(route(&reg, 1, 7) == CLIENT_CTX_ROUTE_NO_CONTEXT,
			 "a slot with no context routes nowhere");
	CONTRACT(route(&reg, 5, 7) == CLIENT_CTX_ROUTE_NO_CONTEXT,
			 "an out-of-range slot routes nowhere");
	struct client_world_context* b
		= client_ctx_registry_install(&reg, -1, 0, 9);
	/* The lease-free predecessor was reclaimed at the install, so the
	 * successor may reuse its entry: identity is the generation, not the
	 * storage address. */
	PRE(b && b->server_generation == 9);
	CONTRACT(route(&reg, 0, 7) == CLIENT_CTX_ROUTE_STALE,
			 "the replaced world's traffic is stale after the successor installs");
	CONTRACT(route(&reg, 0, 9) == CLIENT_CTX_ROUTE_APPLY,
			 "the successor's traffic applies");
	CONTRACT(client_ctx_registry_in_use(&reg) == 1,
			 "a lease-free predecessor is reclaimed at once");
	CONTRACT(reg.routed[CLIENT_CTX_ROUTE_STALE] == 2
				 && reg.routed[CLIENT_CTX_ROUTE_APPLY] == 2,
			 "every outcome is counted exactly once");
	return 0;
}

static int unstamped_routing(void) {
	static struct client_ctx_registry reg;
	memset(&reg, 0, sizeof reg);
	PRE(route(&reg, 0, 0) == CLIENT_CTX_ROUTE_NO_CONTEXT);
	struct client_world_context* a
		= client_ctx_registry_install(&reg, 0, 0, 0);
	PRE(a && a->unstamped && a->server_generation == 0);
	CONTRACT(client_ctx_payload_ready(a),
			 "an unstamped-bound context is payload-ready");
	CONTRACT(route(&reg, 0, 0) == CLIENT_CTX_ROUTE_APPLY,
			 "unstamped traffic applies while exactly one slot is live");
	CONTRACT(route(&reg, 1, 0) == CLIENT_CTX_ROUTE_APPLY,
			 "unstamped traffic ignores its (defaulted) world_index");
	CONTRACT(route(&reg, 0, 3) == CLIENT_CTX_ROUTE_STALE,
			 "a stamped message never matches an unstamped-bound context");
	CONTRACT(!client_ctx_bind_server(a, 3),
			 "an unstamped-bound context cannot be rebound to a generation");
	struct client_world_context* n
		= client_ctx_registry_install(&reg, -1, 1, 4);
	PRE(n);
	CONTRACT(route(&reg, 0, 0) == CLIENT_CTX_ROUTE_AMBIGUOUS,
			 "unstamped traffic is ambiguous with two live slots");
	CONTRACT(route(&reg, 1, 4) == CLIENT_CTX_ROUTE_APPLY,
			 "stamped traffic still routes with two live slots");
	client_ctx_registry_retire(&reg, 1);
	CONTRACT(route(&reg, 0, 0) == CLIENT_CTX_ROUTE_APPLY,
			 "back to one live slot, unstamped traffic applies again");
	return 0;
}

static int hold_until_quiesced(void) {
	static struct client_ctx_registry reg;
	memset(&reg, 0, sizeof reg);
	struct client_world_context* a
		= client_ctx_registry_install(&reg, 0, 0, 11);
	PRE(a && client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_GPU));
	uint32_t old_epoch = a->generation;
	PRE(client_ctx_reset(a) && a->quiescing);
	CONTRACT(route(&reg, 0, 11) == CLIENT_CTX_ROUTE_HOLD,
			 "identity matches but a quiescing context is held, not applied");
	CONTRACT(!client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_MESHER),
			 "producers are parked while quiescing");
	CONTRACT(client_ctx_registry_poll(&reg),
			 "poll reports the quiesce as pending while leases are out");
	CONTRACT(!client_ctx_accept(a, 0, old_epoch),
			 "an old-epoch completion is rejected after the reset");
	PRE(client_ctx_registry_release(&reg, a, CLIENT_CTX_LEASE_MESHER));
	CONTRACT(route(&reg, 0, 11) == CLIENT_CTX_ROUTE_HOLD,
			 "the last kind still out keeps the hold");
	PRE(client_ctx_registry_release(&reg, a, CLIENT_CTX_LEASE_GPU));
	CONTRACT(route(&reg, 0, 11) == CLIENT_CTX_ROUTE_HOLD,
			 "the last release alone never re-opens admission");
	CONTRACT(!client_ctx_registry_poll(&reg),
			 "the owner poll completes a drained quiesce");
	CONTRACT(route(&reg, 0, 11) == CLIENT_CTX_ROUTE_APPLY,
			 "after completion the held traffic applies");
	CONTRACT(client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_MESHER),
			 "producers resume after completion");
	return 0;
}

static int predecessor_overlap(void) {
	static struct client_ctx_registry reg;
	memset(&reg, 0, sizeof reg);
	struct client_world_context* a
		= client_ctx_registry_install(&reg, 0, 0, 0);
	PRE(a && client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_GPU));
	uint32_t a_epoch = a->generation;
	struct client_world_context* b
		= client_ctx_registry_install(&reg, -1, 0, 0);
	PRE(b && b != a);
	CONTRACT(a->retiring && client_ctx_registry_owns(&reg, a),
			 "a leased predecessor retires but stays owned");
	CONTRACT(client_ctx_registry_in_use(&reg) == 2,
			 "predecessor and successor coexist");
	CONTRACT(b->generation > a_epoch,
			 "the successor's epoch is above every predecessor epoch");
	CONTRACT(!client_ctx_accept(b, 0, a_epoch),
			 "a predecessor completion cannot publish into the successor");
	CONTRACT(!client_ctx_lease_acquire(a, CLIENT_CTX_LEASE_MESHER),
			 "a retiring predecessor grants no new lease");
	CONTRACT(!client_ctx_registry_release(&reg, b, CLIENT_CTX_LEASE_MESHER),
			 "a release is refused against a context that holds no such lease");
	PRE(client_ctx_registry_release(&reg, a, CLIENT_CTX_LEASE_MESHER));
	CONTRACT(client_ctx_registry_owns(&reg, a),
			 "still owned while its GPU lease is out");
	PRE(client_ctx_registry_release(&reg, a, CLIENT_CTX_LEASE_GPU));
	CONTRACT(!client_ctx_registry_owns(&reg, a)
				 && client_ctx_registry_in_use(&reg) == 1,
			 "reclaimed on its last release");
	CONTRACT(!client_ctx_registry_release(&reg, a, CLIENT_CTX_LEASE_GPU),
			 "a release against a reclaimed entry is refused");
	CONTRACT(client_ctx_registry_current(&reg, 0) == b,
			 "the successor stays current");
	return 0;
}

static int pool_bound(void) {
	static struct client_ctx_registry reg;
	memset(&reg, 0, sizeof reg);
	/* Slot 1 current throughout, lease-free. */
	PRE(client_ctx_registry_install(&reg, -1, 1, 1));
	/* 8 predecessors each keep one mesher lease, 1 keeps the GPU lease. */
	for(int i = 0; i < 9; i++) {
		struct client_world_context* c
			= client_ctx_registry_install(&reg, 0, 0, 0);
		PRE(c);
		PRE(client_ctx_lease_acquire(c, i < 8 ? CLIENT_CTX_LEASE_MESHER
											  : CLIENT_CTX_LEASE_GPU));
	}
	struct client_world_context* cur
		= client_ctx_registry_install(&reg, 0, 0, 0);
	CONTRACT(cur != NULL, "2 current + 9 leased predecessors fit the pool");
	CONTRACT(client_ctx_registry_in_use(&reg) == 11, "11 entries in use");
	PRE(cur && client_ctx_lease_acquire(cur, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_registry_install(&reg, 0, 0, 0));
	PRE(client_ctx_registry_in_use(&reg) == CLIENT_CTX_POOL);
	/* Beyond the proven bound: fail closed, slot left without a context. */
	struct client_world_context* over = client_ctx_registry_current(&reg, 0);
	PRE(over && client_ctx_lease_acquire(over, CLIENT_CTX_LEASE_MESHER));
	CONTRACT(client_ctx_registry_install(&reg, 0, 0, 0) == NULL,
			 "an install past the pool fails closed");
	CONTRACT(reg.install_failures == 1, "the failure is counted");
	CONTRACT(client_ctx_registry_current(&reg, 0) == NULL
				 && route(&reg, 0, 0) == CLIENT_CTX_ROUTE_APPLY,
			 "the slot has no context (unstamped traffic now reaches slot 1 only)");
	return 0;
}

static int payload_route(void) {
	static struct client_ctx_registry reg;
	memset(&reg, 0, sizeof reg);
	PRE(client_ctx_registry_install(&reg, 0, 0, 5));
	PRE(client_ctx_registry_install(&reg, -1, 1, 6));
	CONTRACT(client_ctx_registry_route_payload(&reg, 1, 1, 6, NULL)
				 == CLIENT_CTX_ROUTE_APPLY,
			 "the payload slot's own traffic applies");
	CONTRACT(client_ctx_registry_route_payload(&reg, 1, 0, 5, NULL)
				 == CLIENT_CTX_ROUTE_FOREIGN,
			 "a live non-payload slot's traffic is foreign, never applied");
	CONTRACT(reg.routed[CLIENT_CTX_ROUTE_FOREIGN] == 1
				 && reg.routed[CLIENT_CTX_ROUTE_APPLY] == 1,
			 "counted once under the final verdict");
	return 0;
}

int main(void) {
	int rc;
	if((rc = stamped_routing()) != 0) return rc;
	if((rc = unstamped_routing()) != 0) return rc;
	if((rc = hold_until_quiesced()) != 0) return rc;
	if((rc = predecessor_overlap()) != 0) return rc;
	if((rc = pool_bound()) != 0) return rc;
	if((rc = payload_route()) != 0) return rc;
	return b1_contract_finish("test_b1_registry");
}
