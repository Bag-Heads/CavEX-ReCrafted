/* split-dims B1: stale queued traffic across a stream reset and across a
 * reconnect, with the two identities validated separately.
 *
 * Contract (ASTRA-B1-PHASE1-0925 #2): the authoritative server-world identity
 * (world_index, server world_generation) and the client's monotonic
 * context/work epoch are different counters. RPC identity is validated
 * against the former (client_ctx_accept_rpc), asynchronous completions
 * against the latter (client_ctx_accept). Neither is mirrored from the
 * other, so a numeric coincidence between them passes NEITHER check.
 *
 * Stream-reset ordering (CRPC_STREAM_RESET, a scoped reset of ONE context):
 *  - the inbox is FIFO, so every RPC dequeued before the reset belongs to the
 *    predecessor epoch and is applied (or not) under the predecessor's
 *    readiness; every RPC dequeued after it carries the SAME server identity
 *    (the world did not change) and routes to the same context, but may not
 *    mutate payloads until the quiesce completes;
 *  - asynchronous completions started before the reset carry the OLD epoch
 *    and are rejected after it, while still releasing their leases.
 * Reconnect isolation (CRPC_WORLD_RESET creating a NEW context in the slot):
 *  - the server world has a NEW generation; anything stamped with the old one
 *    is rejected by the new context even when the world_index matches;
 *  - old work completions are rejected by epoch (per-slot high-water);
 *  - a stale RPC whose server generation happens to equal the new context's
 *    epoch is still rejected, and a stale work stamp equal to the server
 *    generation is still rejected. */
#include "client_context_red.h"

/* One queued message, as the owner sees it: identity stamps only. */
struct queued {
	bool is_work; /* async completion (epoch) vs. server RPC (server gen) */
	unsigned world_index;
	uint32_t stamp;
};

static bool routes_here(const struct client_world_context* ctx,
						const struct queued* q) {
	return q->is_work ? client_ctx_accept(ctx, q->world_index, q->stamp)
					  : client_ctx_accept_rpc(ctx, q->world_index, q->stamp);
}

int main(void) {
	/* ---- Stream reset: same server world, new client epoch. ---- */
	struct client_world_context ctx;
	PRE(client_ctx_init(&ctx, 0, 0));
	PRE(client_ctx_bind_view(&ctx, 0));
	const uint32_t server_gen = 42;
	PRE(client_ctx_bind_server(&ctx, server_gen));
	PRE(client_ctx_payload_ready(&ctx));
	CONTRACT(!client_ctx_bind_server(&ctx, 43),
			 "server identity re-bound on a live context (a new server world "
			 "is a new context)");
	CONTRACT(ctx.server_generation == server_gen,
			 "refused re-bind changed the server identity");

	uint32_t epoch_a = ctx.generation;
	/* In flight at reset time: an RPC ahead of the reset in the FIFO, a
	 * mesher job (epoch A), and an RPC that will land after the reset. */
	struct queued rpc_before = {false, 0, server_gen};
	struct queued work_a = {true, 0, epoch_a};
	struct queued rpc_after = {false, 0, server_gen};
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_MESHER));

	/* FIFO: rpc_before is dequeued first, under the predecessor epoch. */
	PRE(routes_here(&ctx, &rpc_before) && client_ctx_payload_ready(&ctx));

	/* CRPC_STREAM_RESET dequeued: scoped reset, lease outstanding. */
	PRE(client_ctx_reset(&ctx));
	uint32_t epoch_b = ctx.generation;
	PRE(epoch_b > epoch_a);
	CONTRACT(ctx.server_generation == server_gen,
			 "stream reset changed the server identity");
	CONTRACT(!client_ctx_accept_rpc(&ctx, 0, epoch_a)
				 && !client_ctx_accept_rpc(&ctx, 0, epoch_b),
			 "an RPC stamped with a client epoch passed the server-identity "
			 "check (counters not separated)");
	CONTRACT(!client_ctx_accept(&ctx, 0, server_gen),
			 "a completion stamped with the server generation passed the "
			 "epoch check (counters not separated)");

	/* rpc_after: same world, routes here, but must not mutate yet. */
	CONTRACT(routes_here(&ctx, &rpc_after),
			 "post-reset RPC for the same server world no longer routes to "
			 "its context");
	CONTRACT(!client_ctx_payload_ready(&ctx),
			 "post-reset RPC allowed to mutate payloads before the quiesce "
			 "completed");

	/* work_a completes: stale by epoch, lease still returned. */
	CONTRACT(!routes_here(&ctx, &work_a),
			 "pre-reset mesher completion accepted after the stream reset");
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_MESHER));
	PRE(client_ctx_quiesce_drained(&ctx));
	PRE(client_ctx_quiesce_complete(&ctx));
	CONTRACT(routes_here(&ctx, &rpc_after) && client_ctx_payload_ready(&ctx),
			 "post-reset RPC not applicable after the owner completed cleanup");
	struct queued work_b = {true, 0, epoch_b};
	CONTRACT(routes_here(&ctx, &work_b),
			 "completion started in the new epoch rejected");

	/* ---- Reconnect: new server world in the same slot. ---- */
	struct queued old_rpc = {false, 0, server_gen};
	struct queued old_work = {true, 0, epoch_b};
	PRE(client_ctx_lease_acquire(&ctx, CLIENT_CTX_LEASE_GPU));
	PRE(client_ctx_unbind_view(&ctx, 0));
	client_ctx_retire(&ctx);
	PRE(!routes_here(&ctx, &old_rpc) && !routes_here(&ctx, &old_work));
	CONTRACT(!client_ctx_payload_ready(&ctx),
			 "retiring predecessor still payload-ready");

	struct client_world_context fresh;
	PRE(client_ctx_init(&fresh, 0, 0));
	PRE(client_ctx_bind_view(&fresh, 0));
	uint32_t epoch_c = fresh.generation;
	PRE(epoch_c > epoch_b);
	CONTRACT(!routes_here(&fresh, &old_rpc),
			 "unbound successor accepted an RPC stamped with the old server "
			 "generation");
	CONTRACT(!client_ctx_payload_ready(&fresh),
			 "unbound successor payload-ready");

	/* The new server world's generation coincides numerically with the
	 * successor's epoch: the trap the rejected mirroring would have opened. */
	const uint32_t server_gen2 = epoch_c;
	PRE(client_ctx_bind_server(&fresh, server_gen2));
	CONTRACT(!routes_here(&fresh, &old_rpc),
			 "successor accepted an RPC stamped with the PREVIOUS server "
			 "world's generation (reconnect isolation)");
	CONTRACT(!routes_here(&fresh, &old_work),
			 "successor accepted a completion from the predecessor's epoch");
	struct queued coincident_work = {true, 0, server_gen};
	struct queued coincident_rpc = {false, 0, epoch_b};
	CONTRACT(!routes_here(&fresh, &coincident_work)
				 && !routes_here(&fresh, &coincident_rpc),
			 "a stamp from one counter was accepted by the other counter's "
			 "check");
	struct queued new_rpc = {false, 0, server_gen2};
	struct queued new_work = {true, 0, epoch_c};
	CONTRACT(routes_here(&fresh, &new_rpc) && routes_here(&fresh, &new_work)
				 && client_ctx_payload_ready(&fresh),
			 "successor rejects its own server identity / epoch");
	struct queued wrong_slot = {false, 1, server_gen2};
	CONTRACT(!routes_here(&fresh, &wrong_slot),
			 "server identity matched across world_index slots");

	/* Predecessor drains and frees on its own; successor unaffected. */
	PRE(client_ctx_lease_release(&ctx, CLIENT_CTX_LEASE_GPU));
	PRE(client_ctx_try_free(&ctx));
	PRE(fresh.bound_views == 1 && fresh.server_generation == server_gen2);

	return b1_contract_finish("test_client_ctx_red_stale_queue");
}
