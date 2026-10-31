/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * B-NOW (lane B): a pinned REAL mesher job across REAL world resets. GREEN --
 * this isolation is client-side (lease + client epoch) and does not rely on
 * producer stamps, so it already holds with today's unstamped traffic.
 *
 * Producers: the column is published exactly as server_local's near send
 * builds it (clin_chunk_slot_acquire + clin_chunk_slot_copy_local from the
 * real server chunk + CRPC_CHUNK; the emitter itself lives inside the static
 * server_local_update); the resets are the real CRPC_WORLD_RESET emitters
 * server_local_release_world_impl (Save and quit) and
 * server_local_load_world_impl_ex (reload), consumed by the real handler.
 *
 * Contract (ASTRA-B1-RULINGS-0925 #2/#3, Option A overlap):
 *  - the job's mesher lease pins its context: both resets install successors
 *    while the pinned predecessor stays owned (retiring) until completion;
 *  - the completion releases against that exact predecessor, is rejected by
 *    epoch (mesh_stale_discards +1, nothing published into the successor),
 *    and the predecessor is reclaimed only then;
 *  - one live payload slot throughout.
 * The printed reset stamps show the resets themselves are still UNSTAMPED;
 * nothing here depends on them (test_b1_reset_session_reuse owns that RED). */
#include "test_b1_reset_harness.h"

#include "chunk.h"
#include "chunk_mesher.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

static void publish_column(struct server_world* w, w_coord_t cx) {
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, 0));
	if(!sc) assert(server_world_load_chunk(w, cx, 0, &sc) && sc);
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

int main(int argc, char** argv) {
	b1r_mode(argc, argv);
	b1r_client_start();
	b1r_capture(CRPC_WORLD_RESET);

	static struct b1r_server srv;
	b1r_server_create(&srv, "test_b1_reset_mesher_lease");
	struct server_world* w = b1r_server_load(&srv);
	b1r_drain();
	struct client_world_context* pinned = client_ctx_registry_current(b1r_reg, 0);
	assert(pinned);

	publish_column(w, 0);
	b1r_drain();
	struct chunk* c = world_find_chunk(&gstate.world, 0, 2, 0);
	assert(c && "the real column installed");

	/* Pin one real mesher job: its lease is taken against `pinned`. */
	assert(chunk_mesher_init());
	chunk_mesher_test_hold_worker(true);
	uint32_t leases = pinned->leases[CLIENT_CTX_LEASE_MESHER];
	assert(chunk_mesher_send(c));
	c->mesh_in_flight = true;
	ptime_t start = time_get();
	while(!chunk_mesher_test_worker_held()) {
		assert(time_diff_s(start, time_get()) < 5);
		thread_msleep(1);
	}
	assert(pinned->leases[CLIENT_CTX_LEASE_MESHER] == leases + 1
		   && "the real job holds a mesher lease on its context");

	/* Real reset #1: Save and quit. */
	server_local_unload_world_impl(&srv.s, true);
	b1r_drain();
	struct client_world_context* after_release
		= client_ctx_registry_current(b1r_reg, 0);
	assert(after_release && after_release != pinned);
	const uint32_t release_epoch = after_release->generation;
	assert(pinned->retiring && client_ctx_registry_owns(b1r_reg, pinned)
		   && "the pinned predecessor survives the release reset");
	b1r_assert_single_payload();

	/* Real reset #2: reload (a new server world generation). */
	w = b1r_server_load(&srv);
	b1r_drain();
	struct client_world_context* succ = client_ctx_registry_current(b1r_reg, 0);
	/* Pool entries are recycled, so identity is the epoch, not the pointer. */
	assert(succ && succ != pinned);
	assert(succ->generation != pinned->generation
		   && succ->generation != release_epoch && "fresh client epoch");
	assert(client_ctx_registry_owns(b1r_reg, pinned)
		   && pinned->leases[CLIENT_CTX_LEASE_MESHER] == leases + 1
		   && "the lease still pins the predecessor across the second reset");
	assert(client_ctx_registry_in_use(b1r_reg) == 2
		   && "only the pinned predecessor and the successor are owned: the "
			  "lease-free intermediate context was reclaimed at once");
	b1r_assert_single_payload();

	/* The job completes: its result must be rejected, the predecessor freed. */
	uint32_t discards = __atomic_load_n(&mesh_stale_discards, __ATOMIC_RELAXED);
	chunk_mesher_test_hold_worker(false);
	start = time_get();
	while(chunk_mesher_pending() && client_ctx_registry_owns(b1r_reg, pinned)) {
		chunk_mesher_receive();
		assert(time_diff_s(start, time_get()) < 10);
		thread_msleep(1);
	}
	chunk_mesher_receive();
	assert(!client_ctx_registry_owns(b1r_reg, pinned)
		   && "the predecessor is reclaimed on its last lease");
	assert(__atomic_load_n(&mesh_stale_discards, __ATOMIC_RELAXED)
			   == discards + 1
		   && "the old-epoch completion was discarded, never published");
	assert(client_ctx_registry_current(b1r_reg, 0) == succ
		   && client_ctx_registry_in_use(b1r_reg) == 1);
	assert(succ->leases[CLIENT_CTX_LEASE_MESHER] == 0
		   && "the old completion never touched the successor's leases");

	unsigned resets = 0;
	const struct client_rpc* last = b1r_captured(CRPC_WORLD_RESET, &resets);
	printf("note: %u real WORLD_RESETs consumed; last stamp {index %u, gen %u} "
		   "(world gen %u) -- unstamped, and this contract does not rely on it\n",
		   resets, last->payload.world_reset.world_index,
		   (unsigned)last->payload.world_reset.world_generation,
		   (unsigned)w->generation);

	chunk_mesher_shutdown();
	clin_test_set_consume_observer(NULL, NULL);
	b1r_server_destroy(&srv);
	puts("PASS test_b1_reset_mesher_lease: pinned predecessor survives two real "
		 "resets, old completion rejected by epoch, reclaimed on drain");
	return 0;
}
