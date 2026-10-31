/* split-dims B1 RED: mesh leases drain across a world switch.
 *
 * Contract (PLAN-v3 B1 "drain CPU/GPU references before freeing, even when
 * stale results are rejected"; astra-plan-0925 "mesh leases drain"): a view
 * that travels Overworld -> Nether in a single-view session gives up its
 * slot-0 context and the Nether context takes slot 0. Mesher jobs and GPU
 * references the Overworld context leased keep running after the switch.
 * Their results arrive stamped (world_index 0, overworld generation); the
 * Nether context in the same slot must REJECT them (or Overworld chunks get
 * meshed into the Nether), while the Overworld context must still release
 * each lease and only become free-able once both kinds drain.
 *
 * The drain half held from the start. The rejection half was RED (both
 * contexts started at generation 1 in slot 0) and was promoted 2026-09-25:
 * the successor's generation comes from the slot high-water mark
 * (ASTRA-B1-RULINGS-0925 #1/#3, overlapping lifetimes with distinct
 * generations and separate lease accounting). */
#include "client_context_red.h"

int main(void) {
	struct client_world_context overworld;
	PRE(client_ctx_init(&overworld, 0, 0));
	PRE(client_ctx_bind_view(&overworld, 0));
	uint32_t overworld_generation = overworld.generation;

	/* Two mesher jobs in flight and one display list queued for draw. */
	PRE(client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_GPU));

	/* World switch: the view leaves, the Overworld context retires. */
	PRE(client_ctx_unbind_view(&overworld, 0));
	client_ctx_retire(&overworld);
	PRE(!client_ctx_try_free(&overworld));
	PRE(!client_ctx_accept(&overworld, 0, overworld_generation));
	PRE(!client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_MESHER));

	struct client_world_context nether;
	PRE(client_ctx_init(&nether, -1, 0));
	PRE(client_ctx_bind_view(&nether, 0));
	PRE(nether.dimension == -1 && nether.world_index == 0);

	/* First Overworld mesh result lands after the switch. */
	CONTRACT(!client_ctx_accept(&nether, 0, overworld_generation),
		   "successor in the same slot accepts the predecessor's in-flight "
		   "mesh result (Overworld chunk meshed into the Nether)");
	CONTRACT(nether.generation != overworld_generation,
		   "successor reuses the predecessor's generation in the same slot");
	/* Rejected or not, the job's lease is still returned to its owner. */
	PRE(client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_MESHER));
	PRE(!client_ctx_try_free(&overworld));

	/* Successor produces its own work meanwhile; its leases are its own. */
	PRE(client_ctx_lease_acquire(&nether, CLIENT_CTX_LEASE_MESHER));
	PRE(overworld.leases[CLIENT_CTX_LEASE_MESHER] == 1);

	/* Remaining Overworld job and the GPU reference drain. */
	PRE(client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_MESHER));
	PRE(!client_ctx_try_free(&overworld));
	PRE(client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_GPU));
	PRE(!client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_GPU));
	PRE(client_ctx_try_free(&overworld));

	/* Successor unaffected by the predecessor's drain. */
	PRE(nether.leases[CLIENT_CTX_LEASE_MESHER] == 1);
	PRE(nether.bound_views == 1);
	PRE(client_ctx_accept(&nether, 0, nether.generation));
	PRE(client_ctx_lease_release(&nether, CLIENT_CTX_LEASE_MESHER));

	return b1_contract_finish("test_client_ctx_red_switch_drain");
}
