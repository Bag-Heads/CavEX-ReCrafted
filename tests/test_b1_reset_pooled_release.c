/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * B-NOW (lane B): pooled payloads from REAL producers are released exactly
 * once whatever the router decides. GREEN -- release does not depend on
 * stamps, and it must keep holding once A/C stamp (a stale drop is just
 * another verdict).
 *
 * Pooled payloads under test:
 *  - CRPC_ENTITY_DESTROY_BATCH: the malloc'd id array from
 *    server_local.c::server_local_try_release_chunk_entities (ownership
 *    transfers to the client at admission; clin_rpc_release_pooled frees it).
 *  - CRPC_CHUNK: a chunk_rpc_slot from the 16-slot pool, built exactly as the
 *    near send builds it (acquire + clin_chunk_slot_copy_local of the real
 *    server chunk; the emitter itself is inside the static
 *    server_local_update).
 * Verdicts exercised through the real consumer: APPLY (one live slot),
 * NO_CONTEXT (no live context), AMBIGUOUS (unstamped with two live slots),
 * and the headless discard path (clin_drain_discard). After every step the
 * destroy-batch release count advances by exactly one and the chunk pool is
 * back to full; finally a real (unstamped, slot-0) WORLD_RESET from
 * server_local_load_world_impl restores the single-payload invariant. */
#include "test_b1_reset_harness.h"

#include "network/entity_archive.h"

#define POOL_PROBE 64

static unsigned free_chunk_slots(void) {
	struct chunk_rpc_slot* held[POOL_PROBE];
	unsigned n = 0;
	while(n < POOL_PROBE && (held[n] = clin_chunk_slot_try_acquire()))
		n++;
	for(unsigned i = 0; i < n; i++)
		clin_chunk_slot_release(held[i]);
	return n;
}

static struct server_world* s_world;
static struct b1r_server srv;
static uint32_t next_id = 9000;

/* Queue one real destroy batch and one real chunk slot (not yet drained). */
static void produce(w_coord_t cx) {
	struct archived_entity e = {.source_id = next_id++};
	struct chunk_entities batch = {.entities = &e, .count = 1};
	assert(server_local_try_release_chunk_entities(&srv.s, &batch));
	struct server_chunk* sc
		= dict_server_chunks_get(s_world->chunks, S_CHUNK_ID(cx, 0));
	if(!sc) assert(server_world_load_chunk(s_world, cx, 0, &sc) && sc);
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot);
	clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, sizeof(slot->grass_i));
	memset(slot->grass_j, 0, sizeof(slot->grass_j));
	slot->piston_moving.count = 0;
	clin_rpc_send(&(struct client_rpc) {.type = CRPC_CHUNK,
		.payload.chunk = {.x = cx * CHUNK_SIZE, .y = 0, .z = 0,
			.sx = CHUNK_SIZE, .sy = WORLD_HEIGHT, .sz = CHUNK_SIZE,
			.slot = slot}});
}

static void expect_released(const char* step, size_t* releases,
							unsigned full_pool) {
	assert(clin_test_destroy_batch_releases() == *releases + 1
		   && "exactly one destroy-batch id-array release per envelope");
	*releases += 1;
	assert(free_chunk_slots() == full_pool && "the chunk slot came back");
	printf("  released once: %s\n", step);
}

int main(int argc, char** argv) {
	b1r_mode(argc, argv);
	b1r_client_start();
	b1r_server_create(&srv, "test_b1_reset_pooled_release");
	s_world = b1r_server_load(&srv);
	b1r_drain();
	assert(client_ctx_registry_current(b1r_reg, 0));
	const unsigned full_pool = free_chunk_slots();
	assert(full_pool >= 2);
	size_t releases = clin_test_destroy_batch_releases();

	/* APPLY: the single live (unstamped) slot. */
	uint32_t applied = b1r_reg->routed[CLIENT_CTX_ROUTE_APPLY];
	produce(0);
	b1r_drain();
	assert(b1r_reg->routed[CLIENT_CTX_ROUTE_APPLY] >= applied + 2);
	assert(world_find_chunk(&gstate.world, 0, 2, 0));
	expect_released("APPLY (single live slot)", &releases, full_pool);

	/* NO_CONTEXT: no live context at all. */
	client_ctx_registry_retire(b1r_reg, 0);
	uint32_t no_ctx = b1r_reg->routed[CLIENT_CTX_ROUTE_NO_CONTEXT];
	produce(1);
	b1r_drain();
	assert(b1r_reg->routed[CLIENT_CTX_ROUTE_NO_CONTEXT] == no_ctx + 2);
	expect_released("NO_CONTEXT (dropped)", &releases, full_pool);

	/* AMBIGUOUS: unstamped traffic while two slots are live. The chunk is
	 * still unstamped (AMBIGUOUS); the eviction DESTROY_BATCH is stamped by
	 * its producer (B1 local stamping) with the world these hand-installed
	 * unstamped contexts never bound, so it is STALE. Both are dropped and
	 * both release their pooled payload exactly once. */
	assert(client_ctx_registry_install(b1r_reg, WORLD_DIM_OVERWORLD, 0, 0));
	assert(client_ctx_registry_install(b1r_reg, WORLD_DIM_NETHER, 1, 0));
	uint32_t ambiguous = b1r_reg->routed[CLIENT_CTX_ROUTE_AMBIGUOUS];
	uint32_t stale = b1r_reg->routed[CLIENT_CTX_ROUTE_STALE];
	produce(2);
	b1r_drain();
	assert(b1r_reg->routed[CLIENT_CTX_ROUTE_AMBIGUOUS] == ambiguous + 1);
	assert(b1r_reg->routed[CLIENT_CTX_ROUTE_STALE] == stale + 1);
	assert(!world_find_chunk(&gstate.world, 2 * CHUNK_SIZE, 2, 0));
	expect_released("AMBIGUOUS (dropped)", &releases, full_pool);

	/* Discard: the headless drain recycles the envelope without processing. */
	produce(3);
	clin_drain_discard();
	expect_released("discarded (clin_drain_discard)", &releases, full_pool);

	/* A real load WORLD_RESET (unstamped -> slot 0) retires slot 1 again. */
	server_local_unload_world_impl(&srv.s, true);
	b1r_drain();
	s_world = b1r_server_load(&srv);
	b1r_drain();
	assert(client_ctx_registry_current(b1r_reg, 0)
		   && !client_ctx_registry_current(b1r_reg, 1)
		   && "the real reset restores exactly one live payload slot");
	b1r_assert_single_payload();
	produce(4);
	b1r_drain();
	expect_released("APPLY after the real reset", &releases, full_pool);

	b1r_server_destroy(&srv);
	assert(free_chunk_slots() == full_pool);
	puts("PASS test_b1_reset_pooled_release: destroy-batch arrays and chunk "
		 "slots released exactly once under APPLY / NO_CONTEXT / AMBIGUOUS / "
		 "discard; real reset restores the single payload");
	return 0;
}
