/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * B-NOW (lane B): stale chunk UNLOAD and entity DESTROY after a client world
 * reset, driven by the REAL producers into the real consumer.
 *
 * Production sequence (a real FIFO inversion, not a synthetic one): a live
 * local session's server has already admitted terrain/entity traffic to
 * clin_inbox when the client tears its world down on its own thread --
 * clin_force_world_reset() (main.c's non-graceful session end; the pause
 * screen's Leave dispatches the same WORLD_RESET straight into clin_process)
 * -- and the next session installs synchronously through
 * clin_force_world_reset_login() (server_remote_protocol.c's LOGIN path), whose
 * own terrain/entities arrive through clin_process_direct. The dead session's
 * queued messages then drain into the SUCCESSOR's world.
 *
 *  - UNLOAD: server_local.c::server_local_try_send_near_unload (the near
 *    evictor's sender, reached through its test seam) for chunk (0,0).
 *  - DESTROY: server_local.c::server_local_try_release_chunk_entities (the
 *    eviction destroy batch, the only production DESTROY_BATCH emitter).
 * The successor holds its own column at chunk (0,0) and its own item entity
 * under the SAME id the dead session destroys (runtime ids restart per
 * session, so a collision is the normal case, not a contrivance).
 *
 * GREEN (asserts): one live payload slot throughout; the pooled destroy-batch
 * id array is released exactly once; nothing is held.
 * RED (b1r_red, WILL_FAIL): each stale message carries its producing world's
 * stamp, and neither removes the successor's column / entity. Today both
 * producers emit world_generation 0, the single-live-slot rule routes them
 * APPLY, and both successor objects are destroyed. */
#include "test_b1_reset_harness.h"

#include "network/entity_archive.h"
#include "world.h"

bool server_local_test_near_unload_send_or_defer(struct server_local* s,
												 w_coord_t cx, w_coord_t cz);

#define STALE_EID 4242u

static bool client_has_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(gstate.entities, id);
	return pp && *pp;
}

/* The successor's own column at chunk (0,0), in the shape a remote chunk
 * producer publishes it (a pooled slot + CRPC_CHUNK through the direct path). */
static void successor_column(void) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	struct client_rpc m = {.type = CRPC_CHUNK,
		.payload.chunk = {.x = 0, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}};
	clin_process_direct(&m);
}

static void successor_entity(void) {
	struct client_rpc m = {.type = CRPC_SPAWN_ITEM,
		.payload.spawn_item = {.entity_id = STALE_EID,
			.item = {.id = 1, .durability = 0, .count = 1},
			.pos = {0.5F, 70.0F, 0.5F}}};
	clin_process_direct(&m);
}

int main(int argc, char** argv) {
	b1r_mode(argc, argv);
	b1r_client_start();
	b1r_capture(CRPC_UNLOAD_CHUNK);
	b1r_capture(CRPC_ENTITY_DESTROY_BATCH);

	/* Session 1: a real server_local, loaded by the production path; its
	 * WORLD_RESET + load burst drain through the real consumer. */
	static struct b1r_server srv;
	b1r_server_create(&srv, "test_b1_reset_stale_terrain");
	struct server_world* w = b1r_server_load(&srv);
	const uint8_t old_index = w->world_index;
	const uint32_t old_gen = w->generation;
	b1r_drain();
	assert(client_ctx_registry_current(b1r_reg, 0)
		   && "the real load WORLD_RESET installs a client world context");
	b1r_assert_single_payload();

	/* The dead session's producers admit their traffic (not yet drained). */
	assert(server_local_test_near_unload_send_or_defer(&srv.s, 0, 0));
	struct archived_entity evicted = {.source_id = STALE_EID};
	struct chunk_entities batch = {.entities = &evicted, .count = 1};
	assert(server_local_try_release_chunk_entities(&srv.s, &batch));

	/* Client-side teardown + reconnect, both on the client thread. */
	clin_force_world_reset();
	b1r_assert_single_payload();
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 0);
	b1r_assert_single_payload();
	struct client_world_context* succ = client_ctx_registry_current(b1r_reg, 0);
	assert(succ && "the login reset installs the successor context");
	successor_column();
	successor_entity();
	assert(world_find_chunk(&gstate.world, 0, 64, 0)
		   && "successor column installed");
	assert(client_has_entity(STALE_EID) && "successor entity installed");

	/* Now the dead session's queued traffic drains. */
	size_t releases = clin_test_destroy_batch_releases();
	b1r_drain();
	unsigned unload_seen = 0, destroy_seen = 0;
	const struct client_rpc* unload
		= b1r_captured(CRPC_UNLOAD_CHUNK, &unload_seen);
	const struct client_rpc* destroy
		= b1r_captured(CRPC_ENTITY_DESTROY_BATCH, &destroy_seen);
	assert(unload_seen == 1 && destroy_seen == 1
		   && "both stale messages reached the real consumer");
	/* GREEN: the pooled id array is released once, applied or dropped. */
	assert(clin_test_destroy_batch_releases() == releases + 1);
	assert(client_ctx_registry_current(b1r_reg, 0) == succ
		   && "stale traffic never replaces the successor context");
	b1r_assert_single_payload();

	struct server_world stamp_world = {.world_index = old_index};
	printf("stale UNLOAD stamp {index %u, gen %u}, DESTROY_BATCH stamp "
		   "{index %u, gen %u}; producing world {index %u, gen %u}\n",
		   unload->world_index, (unsigned)unload->world_generation,
		   destroy->world_index, (unsigned)destroy->world_generation,
		   old_index, (unsigned)old_gen);
	b1r_red(b1r_stamped_by(unload, &stamp_world, old_gen),
		"server_local.c::server_local_try_send_near_unload (CRPC_UNLOAD_CHUNK; "
		"it takes no world -- the evicting server_world must be passed down)",
		"the near-evict UNLOAD carries its producing world's stamp");
	b1r_red(b1r_stamped_by(destroy, &stamp_world, old_gen),
		"server_local.c::server_local_try_release_chunk_entities "
		"(CRPC_ENTITY_DESTROY_BATCH)",
		"the eviction DESTROY_BATCH carries its producing world's stamp");
	b1r_red(world_find_chunk(&gstate.world, 0, 64, 0) != NULL,
		"server_local.c::server_local_try_send_near_unload (CRPC_UNLOAD_CHUNK)",
		"a dead session's UNLOAD never removes the successor's column");
	b1r_red(client_has_entity(STALE_EID),
		"server_local.c::server_local_try_release_chunk_entities "
		"(CRPC_ENTITY_DESTROY_BATCH)",
		"a dead session's DESTROY_BATCH never removes the successor's entity");

	clin_test_set_consume_observer(NULL, NULL);
	b1r_server_destroy(&srv);
	return b1r_finish("test_b1_reset_stale_terrain");
}
