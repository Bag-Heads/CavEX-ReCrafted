/* split-dims B1: after a scoped reset, resumption waits for the owner's
 * old-payload cleanup, not just for the drain.
 *
 * Contract (ASTRA-B1-RULINGS-0925 #2): reset invalidates the old generation
 * and blocks ALL new lease kinds until ALL outstanding kinds drain. Releases
 * stay legal; retirement never resumes acquisition. "Serialized owner wiring
 * must finish old-payload cleanup/replacement before admitting resumed work.
 * Zero leases permits immediate resumption when payloads are ready."
 *
 * The drain happens on a producer's release (a mesher completion, a GPU
 * retire), NOT in the owner's frame. If acquisition re-opened on the release
 * itself, a producer could lease against the OLD payloads in the window
 * between the last release and the owner's cleanup pass. So the helper keeps
 * refusing after the drain until the owner acknowledges cleanup with
 * client_ctx_quiesce_complete(). client_ctx_quiesce_drained() is the owner's
 * poll: "every pre-reset lease is back; free/replace the old payloads now".
 * A reset with nothing outstanding has nothing to drain: the owner cleaned
 * up synchronously inside the reset, so producers resume at once.
 *
 * ASTRA-B1-PHASE1-0925 #1: identity acceptance is not payload readiness.
 * client_ctx_accept (work epoch) and client_ctx_accept_rpc (server identity)
 * keep saying "this message is for this context" while it is quiescing;
 * client_ctx_payload_ready must say NO until the owner completes cleanup. */
#include "client_context_red.h"

static void expect_refused(struct client_world_context* ctx,
						   enum client_ctx_lease_kind kind, const char* why) {
	bool granted = client_ctx_lease_acquire(ctx, kind);
	CONTRACT(!granted, why);
	if(granted) client_ctx_lease_release(ctx, kind);
}

int main(void) {
	struct client_world_context ctx;
	PRE(client_ctx_init(&ctx, 0, 0));
	PRE(client_ctx_bind_view(&ctx, 0));

	/* Unbound: routable by epoch, but no payload may be touched yet. */
	CONTRACT(!client_ctx_payload_ready(&ctx),
			 "payload ready before the server identity is bound");
	PRE(client_ctx_bind_server(&ctx, 7));
	CONTRACT(client_ctx_payload_ready(&ctx),
			 "payload not ready on a bound, idle context");

	/* Nothing to complete on a context that has not been reset. */
	CONTRACT(!client_ctx_quiesce_drained(&ctx),
			 "drained reported before any reset");
	CONTRACT(!client_ctx_quiesce_complete(&ctx),
			 "complete accepted with no quiesce pending");

	/* Old generation: one mesher job and one GPU reference outstanding. */
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU));
	uint32_t old_generation = ctx.generation;
	PRE(client_ctx_reset(&ctx));
	PRE(!client_ctx_accept(&ctx, 0, old_generation));

	/* Identity still routes here; payload mutation is NOT authorized. */
	PRE(client_ctx_accept(&ctx, 0, ctx.generation));
	PRE(client_ctx_accept_rpc(&ctx, 0, 7));
	CONTRACT(!client_ctx_payload_ready(&ctx),
			 "payload ready while quiescing (identity acceptance authorized "
			 "mutation before readiness)");

	/* Still draining: the owner may neither free nor complete. */
	CONTRACT(!client_ctx_quiesce_drained(&ctx),
			 "drained reported while pre-reset leases are outstanding");
	CONTRACT(!client_ctx_quiesce_complete(&ctx),
			 "complete accepted while pre-reset leases are outstanding");
	expect_refused(&ctx, CLIENT_CTX_LEASE_MESHER,
				   "mesher lease granted during drain");
	expect_refused(&ctx, CLIENT_CTX_LEASE_GPU,
				   "GPU lease granted during drain");

	/* One kind drains; the other still holds an old reference. */
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	CONTRACT(!client_ctx_quiesce_drained(&ctx),
			 "drained reported with the GPU kind still outstanding");
	CONTRACT(!client_ctx_quiesce_complete(&ctx),
			 "complete accepted with the GPU kind still outstanding");
	expect_refused(&ctx, CLIENT_CTX_LEASE_GPU,
				   "GPU lease granted after only the mesher kind drained");

	/* Last old reference back: the owner may free the old payloads. But the
	 * producers stay parked until the owner says the payloads are ready. */
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));
	PRE(ctx.leases[CLIENT_CTX_LEASE_MESHER] == 0);
	PRE(ctx.leases[CLIENT_CTX_LEASE_GPU] == 0);
	CONTRACT(client_ctx_quiesce_drained(&ctx),
			 "drained not reported once every pre-reset lease returned");
	CONTRACT(!client_ctx_payload_ready(&ctx),
			 "payload ready after the drain but before the owner's cleanup "
			 "(the last release re-opened admission)");
	expect_refused(&ctx, CLIENT_CTX_LEASE_MESHER,
				   "mesher lease granted after the drain but BEFORE the owner "
				   "finished old-payload cleanup (cleanup-before-resumption)");
	expect_refused(&ctx, CLIENT_CTX_LEASE_GPU,
				   "GPU lease granted after the drain but BEFORE the owner "
				   "finished old-payload cleanup (cleanup-before-resumption)");

	/* A second scoped reset while parked is legal and keeps the park. */
	uint32_t parked_generation = ctx.generation;
	PRE(client_ctx_reset(&ctx));
	PRE(ctx.generation > parked_generation);
	CONTRACT(client_ctx_quiesce_drained(&ctx),
			 "a reset during the parked state lost the drained status");
	expect_refused(&ctx, CLIENT_CTX_LEASE_MESHER,
				   "a second reset while parked re-opened acquisition");

	/* Owner finishes cleanup/replacement: producers resume on the new
	 * generation. Completing twice is a no-op that reports nothing pending. */
	CONTRACT(client_ctx_quiesce_complete(&ctx),
			 "complete refused after the drain");
	CONTRACT(!client_ctx_quiesce_drained(&ctx),
			 "drained still reported after complete");
	CONTRACT(client_ctx_payload_ready(&ctx),
			 "payload not ready after the owner completed cleanup");
	CONTRACT(client_ctx_accept_rpc(&ctx, 0, 7),
			 "scoped reset changed the server identity (must be epoch-only)");
	CONTRACT(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER),
			 "mesher lease refused after the owner completed cleanup");
	CONTRACT(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU),
			 "GPU lease refused after the owner completed cleanup");
	CONTRACT(!client_ctx_quiesce_complete(&ctx),
			 "complete accepted again with nothing pending");
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));

	/* Zero leases at reset: nothing to drain, nothing to acknowledge. */
	PRE(client_ctx_reset(&ctx));
	CONTRACT(!client_ctx_quiesce_drained(&ctx),
			 "drained reported for a reset with nothing outstanding");
	CONTRACT(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU),
			 "zero-lease reset stalled producers");
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));

	/* Retirement never resumes acquisition, even after the drain. */
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_reset(&ctx));
	PRE(client_ctx_unbind_view(&ctx, 0));
	client_ctx_retire(&ctx);
	PRE(!client_ctx_try_free(&ctx));
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	CONTRACT(!client_ctx_quiesce_complete(&ctx),
			 "complete accepted on a retiring context (retirement must never "
			 "resume acquisition)");
	PRE(!client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	CONTRACT(!client_ctx_payload_ready(&ctx),
			 "payload ready on a retiring context");
	PRE(client_ctx_try_free(&ctx));

	return b1_contract_finish("test_client_ctx_red_cleanup_resume");
}
