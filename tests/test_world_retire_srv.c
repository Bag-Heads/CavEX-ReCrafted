/* Copyright (c) 2026 CaveWii_v2 contributors
 * WORLD-RETIRE-SRV (Fable B2-A2T F1, the server half; Astra A2 plan: "an
 * unoccupied dimension normally remains LIVE with zero resident columns").
 *
 * A second LIVE server world (slot 1) that no actor is committed to -- the
 * world the host just left -- must:
 *   R1 drain to ZERO resident columns (saved, then evicted);
 *   R2 stay LIVE: same generation, clocks and tick queue untouched, slot 0
 *      untouched;
 *   R3 send exactly ONE stamped CRPC_WORLD_RELEASE {1, generation}, which the
 *      real client (world-bound payloads ON) turns into a FREED slot 1 while
 *      the host's slot 0 is kept; a second pass sends nothing;
 *   R4 rebuild cleanly on a switch back: the columns reload from disk with
 *      the same blocks, the client installs slot 1 again at the same
 *      generation, and a later vacancy retires again only after the world
 *      was shown again (re-arm);
 *   R5 the production update runs the sweep (a repopulated vacant world is
 *      drained by server_local_test_update_once).
 * RED by construction until SERVER_LOCAL_WORLD_RETIRE_SRV exists. */
#include "support/split_residency_fixture.h"

#include <assert.h>
#include <stdio.h>

#include "game/client_world_context.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_world_payload.h"
#include "world.h"

#ifndef SERVER_LOCAL_WORLD_RETIRE_SRV
int main(void) {
	puts("RED: SERVER_LOCAL_WORLD_RETIRE_SRV absent -- no server producer "
		 "retires a vacated world's client slot (Fable B2-A2T F1)");
	return 1;
}
#else

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static unsigned retires;
static uint8_t retire_index;
static uint32_t retire_gen;
static int32_t cols_at_release;
extern int32_t g_srv_slot1_cols;

/* The client's consumer: deliver queued RPCs to the real client. The
   observer runs just BEFORE its own call is queued, so the call itself is
   delivered by the next drain (the test drains explicitly after each server
   step, as the main thread would on its next frame). */
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_WORLD_RELEASE) {
		/* a drained vacant world stays LIVE: never a real retire */
		assert(!call->payload.world_release.retiring);
		retires++;
		retire_index = call->world_index;
		retire_gen = call->world_generation;
		cols_at_release = __atomic_load_n(&g_srv_slot1_cols, __ATOMIC_ACQUIRE);
	}
	clin_update_drain();
}

static void populate_w(struct server_local* s, struct server_world* w, int x,
					   int z) {
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000; n++) {
		server_world_begin_update(w);
		if(server_world_ensure_populated(w, s, x, z, &c))
			return;
	}
	assert(!"populate failed");
}

static uint64_t column_hash(struct server_world* w, int x, int z) {
	struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, z));
	assert(c);
	uint64_t h = 1469598103934665603ULL;
	for(size_t i = 0; i < sizeof(c->ids); i++) {
		h ^= c->ids[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static void client_reset(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = 77, .world_index = index,
			.world_generation = gen}});
}

#define X1 200 /* slot 1's columns, far from anything slot 0 holds */

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_world_retire_srv");
	blocks_init();
	items_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	gstate.players[0].visible_window = -1;
	struct client_ctx_registry* reg = clin_world_contexts();
	assert(clin_world_payloads_set_enabled(true));

	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	populate_w(s, &s->worlds[0], 0, 0);
	size_t slot0_before = dict_server_chunks_size(s->worlds[0].chunks);

	/* Slot 1: a second LIVE world with resident columns and no actor. */
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, true);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	for(int x = X1 - 1; x <= X1 + 1; x++)
		populate_w(s, b, x, 0);
	assert(server_local_world_live(b));
	size_t cols = dict_server_chunks_size(b->chunks);
	assert(cols >= 3);
	uint64_t h_before = column_hash(b, X1, 0);
	uint32_t gen = b->generation;
	uint64_t tick_before = b->current_tick;
	size_t queue_before = b->scheduled_tick_count;

	/* Server traffic queued while the fixture populated both worlds is not
	   part of this scenario (the client below is built by hand). */
	clin_drain_discard();
	/* The client: the host left slot 1 (unviewed, body-less) for slot 0. */
	client_reset(1, gen);
	client_reset(0, s->worlds[0].generation);
	check(client_ctx_registry_current(reg, 1) != NULL
			  && clin_world_payload_primary_slot() == 0,
		  "setup: client shows slot 0; slot 1 installed but unviewed");

	clin_test_set_rpc_observer(observe, NULL);
	retires = 0;
	cols_at_release = -2;
	/* Reproduce a heartbeat that still holds the previous end-loop sample. */
	__atomic_store_n(&g_srv_slot1_cols, 1, __ATOMIC_RELAXED);
	/* The drain is bounded per pass (NEAR_EVICT_BUDGET): run passes until
	   nothing is left, as successive server updates would. */
	for(int pass = 0; pass < 16 && dict_server_chunks_size(b->chunks); pass++)
		server_local_retire_vacant_worlds(s);
	server_local_retire_vacant_worlds(s);
	clin_update_drain();

	check(dict_server_chunks_size(b->chunks) == 0,
		  "R1 the vacated world holds zero resident columns");
	check(server_local_world_live(b) && b->generation == gen
			  && b->current_tick == tick_before
			  && b->scheduled_tick_count == queue_before,
		  "R2 it stays LIVE: generation, clock and tick queue untouched");
	check(dict_server_chunks_size(s->worlds[0].chunks) == slot0_before,
		  "R2 the routed world is untouched");
	check(retires == 1 && retire_index == 1 && retire_gen == gen,
		  "R3 exactly one stamped CRPC_WORLD_RELEASE {1, generation}");
	check(cols_at_release == 0,
		  "R3 the verified zero-column snapshot precedes client RELEASE enqueue");
	check(client_ctx_registry_current(reg, 1) == NULL
			  && !clin_world_payload_ready(1)
			  && client_ctx_registry_current(reg, 0) != NULL,
		  "R3 the client freed slot 1 and kept the host's slot 0");
	server_local_retire_vacant_worlds(s);
	clin_update_drain();
	check(retires == 1, "R3 a second pass sends nothing");

	/* Switch back: the columns reload from disk; the client reinstalls. */
	struct server_chunk* c = NULL;
	check(server_world_load_chunk(b, X1, 0, &c) && c
			  && column_hash(b, X1, 0) == h_before,
		  "R4 a returning column reloads from disk with the same blocks");
	client_reset(1, gen);
	check(client_ctx_registry_current(reg, 1) != NULL
			  && client_ctx_registry_current(reg, 1)->server_generation == gen,
		  "R4 the client rebuilds slot 1 at the same generation");

	/* R5: the production update runs the sweep. Still no actor in slot 1,
	   already retired: the column drains again, no second retire. */
	client_reset(0, s->worlds[0].generation);
	for(int n = 0; n < 3 && dict_server_chunks_size(b->chunks); n++)
		server_local_test_update_once(s);
	clin_update_drain();
	check(dict_server_chunks_size(b->chunks) == 0 && retires == 1
			  && client_ctx_registry_current(reg, 1) != NULL,
		  "R5 the server update drains a vacant world (no repeat retire "
		  "until it is shown again)");

	/* Re-arm: a world marked shown again retires on its next vacancy. */
	b->client_retired = false;
	server_local_retire_vacant_worlds(s);
	clin_update_drain();
	check(retires == 2 && retire_gen == gen
			  && client_ctx_registry_current(reg, 1) == NULL,
		  "R4 a later vacancy retires again (and frees the rebuilt slot)");

	clin_test_set_rpc_observer(NULL, NULL);
	clin_update_drain();
	server_world_discard(b);
	split_destroy(s);
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	printf("%s test_world_retire_srv (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
#endif
