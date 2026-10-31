/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * G6 S1 (b1stamp-remote-0926): the local server's world generations and the
 * remote adapter's session identities share the client registry's slot-0
 * namespace. Both minters used to start at 1, so a dead session's queued,
 * stamped traffic matched the NEXT session's identity and applied. The ranges
 * are now disjoint (WORLD_GENERATION_REMOTE_BASE, client_interface.h); this
 * test drives both orders through the REAL producers into the real consumer:
 *
 *  R->L  remote login first (clin_force_world_reset_login mints the identity;
 *        its traffic is stamped by clin_process_direct_remote), a remote
 *        UNLOAD for chunk (0,0) stamped with that identity is still in hand
 *        when the client tears down and a REAL server_local loads a local
 *        world; the local session owns chunk (0,0); the stale remote UNLOAD
 *        arrives -> dropped, the local column stays.
 *  L->R  a REAL server_local session's near-evict UNLOAD for chunk (0,0) is
 *        queued in clin_inbox when the client tears down and logs in
 *        remotely; the remote session owns chunk (0,0); the queued UNLOAD
 *        drains -> dropped, the remote column stays.
 *
 * Both orders run in ONE fresh process, the worst case: each minter's first
 * identity is its very first value. Before the partition both were 1 and
 * both stale UNLOADs removed the successor's column. */
#include "test_b1_reset_harness.h"

#include "world.h"

bool server_local_test_near_unload_send_or_defer(struct server_local* s,
												 w_coord_t cx, w_coord_t cz);

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static bool has_column(void) {
	return world_find_chunk(&gstate.world, 0, 64, 0) != NULL;
}

/* A column at chunk (0,0) delivered to the current session, stamped as its
 * producer stamps it. */
static void column(uint32_t generation, bool remote) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	struct client_rpc m = {.type = CRPC_CHUNK,
		.payload.chunk = {.x = 0, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}};
	if(remote) {
		clin_process_direct_remote(&m); /* the remote adapter's own stamp */
	} else {
		clin_rpc_stamp(&m, 0, generation);
		clin_process(&m);
	}
}

int main(int argc, char** argv) {
	b1r_mode(argc, argv);
	b1r_client_start();

	/* ---- R->L: remote first ------------------------------------------ */
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 0);
	const uint32_t remote_gen = clin_remote_world_generation();
	check(world_generation_is_remote(remote_gen)
			  && remote_gen == WORLD_GENERATION_REMOTE_BASE,
		  "R->L the first remote identity is the base of the remote half");
	column(0, true);
	assert(has_column());
	/* A remote UNLOAD stamped by the live remote session, not yet applied. */
	struct client_rpc stale_remote = {.type = CRPC_UNLOAD_CHUNK,
		.payload.unload_chunk = {.x = 0, .z = 0}};
	clin_rpc_stamp(&stale_remote, 0, remote_gen);

	clin_force_world_reset(); /* disconnect */
	static struct b1r_server local;
	b1r_server_create(&local, "test_b1_stamp_namespace_rl");
	struct server_world* w = b1r_server_load(&local);
	b1r_drain();
	const uint32_t local_gen = w->generation;
	check(world_generation_is_local(local_gen) && local_gen != remote_gen,
		  "R->L the first local world identity is in the local half");
	struct client_world_context* ctx = client_ctx_registry_current(b1r_reg, 0);
	check(ctx && ctx->server_generation == local_gen,
		  "R->L the local load binds the client context to its own identity");
	column(local_gen, false);
	check(has_column(), "R->L the local session owns chunk (0,0)");
	clin_process(&stale_remote);
	check(has_column(),
		  "R->L a dead remote session's UNLOAD never removes the local column");

	/* ---- L->R: local first ------------------------------------------- */
	/* The live local session's near evictor queues an UNLOAD for (0,0). */
	assert(server_local_test_near_unload_send_or_defer(&local.s, 0, 0));
	clin_force_world_reset(); /* the session ends on the client thread */
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 0);
	const uint32_t remote_gen2 = clin_remote_world_generation();
	check(world_generation_is_remote(remote_gen2) && remote_gen2 != local_gen,
		  "L->R the next remote identity stays in the remote half");
	column(0, true);
	check(has_column(), "L->R the remote session owns chunk (0,0)");
	b1r_capture(CRPC_UNLOAD_CHUNK);
	b1r_drain(); /* the dead local session's queued UNLOAD arrives now */
	unsigned seen = 0;
	const struct client_rpc* unload = b1r_captured(CRPC_UNLOAD_CHUNK, &seen);
	check(seen == 1 && unload->world_generation == local_gen,
		  "L->R the stale UNLOAD reached the consumer with its local stamp");
	check(has_column(),
		  "L->R a dead local session's UNLOAD never removes the remote column");

	clin_test_set_consume_observer(NULL, NULL);
	b1r_server_destroy(&local);
	printf("%s test_b1_stamp_namespace (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
