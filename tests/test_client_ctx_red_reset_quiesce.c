/* split-dims B1 RED: a scoped reset quiesces producers before old payloads
 * are freed (leases are per kind, not per generation).
 *
 * Contract (client_world_context.h batch-I note; astra-plan-0925 "scoped
 * resets quiesce producers"): lease counts are per kind, so after a reset
 * the owner may free the OLD generation's payloads only once every kind's
 * count reaches 0. If a producer is allowed to take a new lease in the
 * meantime, the count mixes old-generation and new-generation references and
 * "count == 0" no longer means the old payloads are unreferenced -- the free
 * either never becomes legal (starvation) or is taken while an old job still
 * reads them. The helper therefore has to refuse new leases of EVERY kind
 * while any lease taken before the reset is outstanding, and resume once
 * all kinds have drained and the owner has completed cleanup (promoted
 * 2026-09-25: client_ctx_lease_acquire() refuses while `quiescing`). */
#include "client_context_red.h"

/* Try to start a producer during quiesce. A granted lease is a RED failure;
 * hand it straight back so the drain bookkeeping below stays exact. */
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

	/* Old generation: one mesher job in flight, one GPU reference queued. */
	uint32_t old_generation = ctx.generation;
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU));

	PRE(client_ctx_reset(&ctx));
	PRE(ctx.bound_views == 1);
	PRE(!client_ctx_accept(&ctx, 0, old_generation));
	PRE(client_ctx_accept(&ctx, 0, ctx.generation));
	PRE(ctx.leases[CLIENT_CTX_LEASE_MESHER] == 1);
	PRE(ctx.leases[CLIENT_CTX_LEASE_GPU] == 1);

	expect_refused(&ctx, CLIENT_CTX_LEASE_MESHER,
				   "new mesher job leased while pre-reset leases are "
				   "outstanding (producers not quiesced by reset)");
	expect_refused(&ctx, CLIENT_CTX_LEASE_GPU,
				   "new GPU upload leased while pre-reset leases are "
				   "outstanding (producers not quiesced by reset)");

	/* Per kind: the old mesher job finishes (its result is rejected as
	 * stale) but the old GPU reference is still live, so the old payloads
	 * are still referenced and quiesce must continue for every kind. */
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(ctx.leases[CLIENT_CTX_LEASE_MESHER] == 0);
	expect_refused(&ctx, CLIENT_CTX_LEASE_MESHER,
				   "mesher kind drained but GPU kind still holds a pre-reset "
				   "lease; quiesce ended early (drain must cover all kinds)");

	/* Last old reference drains: old payloads are now free-able. The owner
	 * frees/replaces them and completes the quiesce; only then do producers
	 * resume on the new generation (test_client_ctx_red_cleanup_resume pins
	 * the parked window between the drain and the completion). */
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));
	PRE(ctx.leases[CLIENT_CTX_LEASE_MESHER] == 0);
	PRE(ctx.leases[CLIENT_CTX_LEASE_GPU] == 0);
	PRE(client_ctx_quiesce_drained(&ctx));
	PRE(client_ctx_quiesce_complete(&ctx));
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU));
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));

	/* A reset with nothing outstanding has nothing to drain and must not
	 * stall producers. */
	PRE(client_ctx_reset(&ctx));
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU));
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));

	/* Retirement still refuses new leases and gates the free on the drain. */
	PRE(client_ctx_unbind_view(&ctx, 0));
	client_ctx_retire(&ctx);
	PRE(!client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_try_free(&ctx));

	return b1_contract_finish("test_client_ctx_red_reset_quiesce");
}
