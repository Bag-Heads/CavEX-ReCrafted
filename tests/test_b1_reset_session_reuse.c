/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * B-NOW (lane B): session identity across REAL world resets and a reconnect.
 *
 * Producers: server_local.c::server_local_load_world_impl_ex and
 * ::server_local_release_world_impl, the two local CRPC_WORLD_RESET emitters,
 * driven through server_local_load_world_impl / _unload_world_impl; then the
 * client-thread reconnect pair clin_force_world_reset (disconnect teardown) +
 * clin_force_world_reset_login (server_remote_protocol.c's LOGIN install).
 * Every reset travels the real queue (or the real direct path) into the real
 * CRPC_WORLD_RESET handler, which installs the client world context.
 *
 * GREEN (asserts):
 *  - every reset installs exactly one live context (single payload), and a
 *    real (unstamped, slot-0) WORLD_RESET retires a live non-payload slot;
 *  - the CLIENT epoch (ctx->generation) is fresh on every install and never
 *    reused: reload, release and reconnect all mint a new one;
 *  - the SERVER side mints a new world generation on every load.
 * RED (b1r_red, WILL_FAIL): the client context's SERVER identity follows the
 * producing world -- the load reset binds {world_index, generation}; two
 * successive loads (two server generations) give two different identities;
 * the reconnected session's identity differs from the dead session's. Today
 * every reset is unstamped, every context binds the unstamped stream
 * (server_generation 0), and the identity is REUSED across reloads and
 * reconnects -- so a message queued by a dead session matches the next one.
 * (The reconnect clause holds since b1stamp-remote-0926: the remote login
 * install mints a fresh session identity.) */
#include "test_b1_reset_harness.h"

struct identity {
	bool unstamped;
	uint32_t server_generation;
	uint32_t epoch;
};

static struct identity current_identity(void) {
	struct client_world_context* c = client_ctx_registry_current(b1r_reg, 0);
	assert(c && "a reset installed a current context");
	b1r_assert_single_payload();
	return (struct identity) {c->unstamped, c->server_generation, c->generation};
}

static bool same_server_identity(struct identity a, struct identity b) {
	return a.unstamped == b.unstamped && a.server_generation == b.server_generation;
}

int main(int argc, char** argv) {
	b1r_mode(argc, argv);
	b1r_client_start();
	b1r_capture(CRPC_WORLD_RESET);

	static struct b1r_server srv;
	b1r_server_create(&srv, "test_b1_reset_session_reuse");

	/* A live non-payload slot (what a second client payload would add):
	 * the real load reset must retire it -- the F1 single-payload rule. */
	assert(client_ctx_registry_install(b1r_reg, WORLD_DIM_NETHER, 1, 0));

	/* Load 1. */
	struct server_world* w = b1r_server_load(&srv);
	const uint8_t index_a = w->world_index;
	const uint32_t gen_a = w->generation;
	unsigned resets = 0;
	b1r_drain();
	const struct client_rpc* reset = b1r_captured(CRPC_WORLD_RESET, &resets);
	assert(resets == 1 && "the load emitted exactly one WORLD_RESET");
	struct client_rpc reset_a = *reset;
	assert(!client_ctx_registry_current(b1r_reg, 1)
		   && "the real reset retired the other slot (single payload)");
	struct identity a = current_identity();

	/* Release (Save and quit): its reset tears the client world down. */
	server_local_unload_world_impl(&srv.s, true);
	b1r_drain();
	(void)b1r_captured(CRPC_WORLD_RESET, &resets);
	assert(resets == 2 && "the release emitted its WORLD_RESET");
	struct identity released = current_identity();
	assert(released.epoch != a.epoch && "release mints a fresh client epoch");

	/* Load 2: the same save, a new server world generation. */
	w = b1r_server_load(&srv);
	const uint32_t gen_b = w->generation;
	assert(gen_b != gen_a && "the server mints a new world generation per load");
	b1r_drain();
	reset = b1r_captured(CRPC_WORLD_RESET, &resets);
	assert(resets == 3);
	struct client_rpc reset_b = *reset;
	struct identity b = current_identity();
	assert(b.epoch != a.epoch && b.epoch != released.epoch
		   && "reload mints a fresh client epoch");

	/* Reconnect: client-thread teardown, then the login install. */
	clin_force_world_reset();
	struct identity torn = current_identity();
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 0);
	struct identity r = current_identity();
	assert(torn.epoch != b.epoch && r.epoch != torn.epoch && r.epoch != b.epoch
		   && "disconnect and login each mint a fresh client epoch");

	printf("load-1 reset stamp {index %u, gen %u} (world {%u, %u}) -> context "
		   "{unstamped %d, server gen %u, epoch %u}\n",
		   reset_a.payload.world_reset.world_index,
		   (unsigned)reset_a.payload.world_reset.world_generation, index_a,
		   (unsigned)gen_a, (int)a.unstamped, (unsigned)a.server_generation,
		   (unsigned)a.epoch);
	printf("load-2 reset stamp {index %u, gen %u} (world gen %u) -> context "
		   "{unstamped %d, server gen %u, epoch %u}; reconnect -> {unstamped "
		   "%d, server gen %u, epoch %u}\n",
		   reset_b.payload.world_reset.world_index,
		   (unsigned)reset_b.payload.world_reset.world_generation,
		   (unsigned)gen_b, (int)b.unstamped, (unsigned)b.server_generation,
		   (unsigned)b.epoch, (int)r.unstamped, (unsigned)r.server_generation,
		   (unsigned)r.epoch);

	b1r_red(reset_a.payload.world_reset.world_generation == gen_a
			&& reset_a.payload.world_reset.world_index == index_a,
		"server_local.c::server_local_load_world_impl_ex (CRPC_WORLD_RESET "
		"payload.world_reset.world_index/world_generation)",
		"the load WORLD_RESET names the world it installs");
	b1r_red(!a.unstamped && a.server_generation == gen_a,
		"server_local.c::server_local_load_world_impl_ex (CRPC_WORLD_RESET)",
		"the client context binds the loaded world's server identity");
	b1r_red(!same_server_identity(a, b),
		"server_local.c::server_local_load_world_impl_ex + "
		"::server_local_release_world_impl (CRPC_WORLD_RESET)",
		"a reload (new server generation) never reuses the old session identity");
	b1r_red(!same_server_identity(b, r),
		"server_remote_protocol.c LOGIN -> clin_force_world_reset_login "
		"(the remote session identity minted per install, "
		"clin_remote_world_generation_mint)",
		"a reconnect never reuses the dead session's identity");

	clin_test_set_consume_observer(NULL, NULL);
	b1r_server_destroy(&srv);
	return b1r_finish("test_b1_reset_session_reuse");
}
