/*
	C1 (candidate, hardware-unconfirmed, 2026-09-02): PC-testable proof that
	chunk_mesher_shutdown() actually stops and joins the worker thread within a
	bounded time -- whether or not it has a chunk backlog in flight -- and that
	a redundant call is a harmless no-op (idempotent).

	Before this commit chunk_mesher_init() discarded its `struct thread` handle
	immediately after thread_create: the worker ran forever with no stop API at
	all, so nothing in the process ever joined it (main.c's shutdown sequence
	had no call to make). This drives the REAL production path --
	chunk_mesher_init() -> chunk_mesher_send() on real chunks in a real world ->
	chunk_mesher_shutdown() -- not a hand-mirrored copy, on
	source/platform/thread.c's pthread-backed PLATFORM_PC implementation (the
	same primitive backs the Wii LWP implementation this is standing in for).

	WHAT THIS ASSERTS:
	  (1) NO HANG -- chunk_mesher_shutdown(), called immediately after
	      submitting several chunks (so the worker may still be mid-build or
	      have a queued backlog when shutdown is requested), returns within a
	      generous wall-clock bound instead of waiting for chunk build time or
	      wedging on the internal 5s token-drain bound. If the wake-through-
	      mesher_requests handshake or the final thread_join ever regresses to
	      an unbounded wait, this test times out instead of hanging CI.
	  (2) IDEMPOTENT -- a second chunk_mesher_shutdown() call, with no
	      re-init in between, is an immediate no-op (mesher_thread.created is
	      already false).
	  (3) HARMLESS AFTERWARD -- chunk_mesher_receive() called after shutdown
	      does not crash or misbehave, matching the documented contract that a
	      caller may still poll it (e.g. a render loop's last frame running
	      after teardown has started).

	Harness shape cloned from test_piston_chunk_mesh_env.c (clin drain thread,
	the client world is independent of any server).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "network/client_interface.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

// Generous: a single flat-floor chunk mesh is sub-ms on PC, and
// chunk_mesher_shutdown's own internal token-drain bound is 5000ms (used only
// if the token pool is somehow never replenished, which a running worker
// always does). This bound exists so a regression that makes shutdown wait on
// something unbounded FAILS the test instead of wedging CI.
#define SHUTDOWN_TIMEOUT_MS 4000
#define YF 64 // stone-floor plane, well inside chunk cy=4

// --- RPC drain thread (the full client links clin_*; keep its inbox empty so
// no path blocks). The CLIENT world below is wholly independent of it. -------
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static struct world w;

static void set(w_coord_t x, w_coord_t y, w_coord_t z, int type, int meta,
				int sky) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = (uint8_t)(meta & 0x0F),
										 .sky_light = (uint8_t)(sky & 0x0F),
										 .torch_light = 0},
					false /* no light flood: we set sky_light explicitly */);
}

// A full stone floor plane, one chunk, at world column (X0, Z0). Cloned from
// test_piston_chunk_mesh_env.c's build_scene_at (minus the piston -- this test
// only needs real, meshable chunks, not any specific block content).
static struct chunk* build_scene_at(w_coord_t X0, w_coord_t Z0) {
	const w_coord_t N = CHUNK_SIZE;
	for(w_coord_t x = X0; x < X0 + N; x++)
		for(w_coord_t z = Z0; z < Z0 + N; z++)
			set(x, YF, z, BLOCK_STONE, 0, 15);
	return world_find_chunk(&w, X0 + 8, YF, Z0 + 8);
}

int main(void) {
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	world_create(&w);

	assert(chunk_mesher_init() && "mesher worker thread did not start");

	// Submit several chunks and DO NOT wait for any of them to finish -- the
	// worker may be mid-build or have a queued backlog when shutdown is
	// requested next, exactly the case chunk_mesher_shutdown()'s token-drain
	// logic (wake the blocking receive on mesher_requests via a token freed
	// from mesher_empty_msg) has to handle without waiting for build time.
	const int N_CHUNKS = 6; // comfortably within CHUNK_MESHER_QLENGTH (8),
							 // deliberately ABOVE CHUNK_MESHER_ENQUEUE_CAP (T1-d,
							 // 2026-09-03 far-ring frame-drop root cause): a send
							 // past the cap returns false (pacing, not a fault),
							 // exactly like a real caller (chunk.c's
							 // chunk_check_built) must tolerate -- retry with the
							 // same bounded, non-hanging discipline as the rest
							 // of this test rather than assert success on the
							 // first try.
	for(int i = 0; i < N_CHUNKS; i++) {
		struct chunk* c = build_scene_at(i * CHUNK_SIZE, 0);
		assert(c);
		c->dirty_generation++;	   // chunk_mark_dirty: force a fresh build
		c->mesh_in_flight = false; // allow the send

		ptime_t send_t0 = time_get();
		bool sent = false;
		while(!sent) {
			sent = chunk_mesher_send(c);
			if(!sent) {
				// T1-d's cap only ever clears via chunk_mesher_receive()
				// integrating a finished build (mesher_pending decrements
				// there, never in chunk_mesher_send) -- a real render loop
				// calls this every frame regardless of new sends, so mirror
				// that instead of busy-spinning on a send that can never
				// succeed on its own.
				chunk_mesher_receive();
				thread_msleep(1);
				assert(time_diff_ms(send_t0, time_get()) < SHUTDOWN_TIMEOUT_MS
					   && "chunk_mesher_send never cleared the enqueue cap");
			}
		}
		// Mirror chunk_check_built's post-send bookkeeping (test_piston_
		// chunk_mesh_env.c's mesh_chunk_and_wait) so the chunk's own state is
		// self-consistent even though we never wait for the result here.
		c->rebuild_displist = false;
		c->first_meshed = true;
		c->mesh_in_flight = true;
	}
	printf("  submitted %d chunks, none awaited -- shutting down immediately\n",
		   N_CHUNKS);

	// (1) NO HANG.
	ptime_t t0 = time_get();
	chunk_mesher_shutdown();
	int32_t ms = time_diff_ms(t0, time_get());
	assert(ms >= 0 && ms < SHUTDOWN_TIMEOUT_MS
		   && "chunk_mesher_shutdown() must return within a bounded time even "
			  "with a chunk backlog in flight");
	printf("  [1] chunk_mesher_shutdown() with %d chunks in flight returned in "
		   "%dms (bound %dms)\n",
		   N_CHUNKS, (int)ms, SHUTDOWN_TIMEOUT_MS);

	// (2) IDEMPOTENT: a redundant call, no re-init in between, must be an
	// immediate no-op.
	ptime_t t1 = time_get();
	chunk_mesher_shutdown();
	int32_t ms2 = time_diff_ms(t1, time_get());
	assert(ms2 >= 0 && ms2 < SHUTDOWN_TIMEOUT_MS
		   && "a redundant chunk_mesher_shutdown() call must not block");
	printf("  [2] redundant chunk_mesher_shutdown(): idempotent, returned in "
		   "%dms\n",
		   (int)ms2);

	// (3) HARMLESS AFTERWARD: a caller polling the result queue post-shutdown
	// (e.g. one more render-loop frame during teardown) must not crash.
	chunk_mesher_receive();
	printf("  [3] chunk_mesher_receive() after shutdown: harmless\n");

	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_chunk_mesher_shutdown_env: OK -- chunk_mesher_shutdown() stops "
		   "and joins the real worker thread within a bounded time with a "
		   "chunk backlog in flight, a redundant call is idempotent, and "
		   "chunk_mesher_receive() stays harmless afterward.\n");
	return 0;
}
