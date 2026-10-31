/* split-dims B1 RED: stale-generation RPC rejection across context re-creation.
 *
 * Contract (PLAN-v3 B1, astra-plan-0925 "stale generations rejected"): every
 * RPC (CRPC_SOUND, SMOKE_PUFF, ENTITY_EXPLODE, SPAWN_*, terrain, time) carries
 * world_index + the generation its producer captured, and a context accepts
 * only its own current generation. A reset already bumps the generation, but
 * client_ctx_init() restarts every context at generation 1. When a slot is
 * torn down and re-created (disconnect/reconnect, death respawn into the same
 * dimension, single-view travel back), an RPC stamped by the PREVIOUS occupant
 * of that slot still in the queue matches (world_index, 1) and is accepted by
 * the new context. Generations must be monotonic per world_index slot for the
 * whole client lifetime, not per context object (promoted 2026-09-25: issued
 * from the per-slot high-water mark, ASTRA-B1-RULINGS-0925 #1). */
#include "client_context_red.h"

int main(void) {
	struct client_world_context first;
	PRE(client_ctx_init(&first, 0, 0));
	PRE(client_ctx_bind_view(&first, 0));

	/* The first occupant lives through two scoped resets; RPCs were stamped
	 * with each of these generations and may still be queued. */
	uint32_t stamped[3];
	stamped[0] = first.generation;
	PRE(client_ctx_reset(&first));
	stamped[1] = first.generation;
	PRE(client_ctx_reset(&first));
	stamped[2] = first.generation;
	PRE(!client_ctx_accept(&first, 0, stamped[0]));
	PRE(!client_ctx_accept(&first, 0, stamped[1]));
	PRE(client_ctx_accept(&first, 0, stamped[2]));

	/* Tear the slot down cleanly (no leases outstanding). */
	PRE(client_ctx_unbind_view(&first, 0));
	client_ctx_retire(&first);
	PRE(client_ctx_try_free(&first));
	PRE(!client_ctx_accept(&first, 0, stamped[2]));

	/* The other slot is untouched throughout. */
	struct client_world_context other;
	PRE(client_ctx_init(&other, -1, 1));
	uint32_t other_generation = other.generation;

	/* Re-create slot 0 (same dimension: reconnect / respawn). */
	struct client_world_context second;
	PRE(client_ctx_init(&second, 0, 0));
	PRE(client_ctx_bind_view(&second, 0));
	PRE(client_ctx_accept(&second, 0, second.generation));
	PRE(!client_ctx_accept(&second, 1, second.generation));

	for(int i = 0; i < 3; i++) {
		CONTRACT(!client_ctx_accept(&second, 0, stamped[i]),
			   "RPC stamped by the slot's previous occupant is accepted by "
			   "the re-created context (generation restarts at 1 on init)");
	}
	CONTRACT(second.generation > stamped[2],
		   "re-created context's generation is not past every generation "
		   "the slot ever issued");

	/* Scoped reset on the new occupant still rejects its own stale traffic,
	 * and never disturbs the other slot. */
	uint32_t before = second.generation;
	PRE(client_ctx_reset(&second));
	PRE(!client_ctx_accept(&second, 0, before));
	PRE(other.generation == other_generation);
	PRE(client_ctx_accept(&other, 1, other_generation));

	return b1_contract_finish("test_client_ctx_red_stale_gen");
}
