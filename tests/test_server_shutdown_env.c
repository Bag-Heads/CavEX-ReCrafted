/*
	Server-thread clean-quit lifecycle (the amahpour-shared shutdown race,
	2026-08-15). Before this fix, server_local_thread ran `while(1)` and
	server_local_start THREW AWAY the thread handle on a stack local -- so
	main() could only exit by abandoning a live thread, potentially
	mid-region-write (on Wii: a save-corruption window on exit-to-loader; the
	same defect docs/reference_forks.md records the amahpour sibling shipping).

	Contract under test (the FIRST test to drive the real threaded path --
	every other env test deliberately uses the threadless entry):
	  S1  a fresh init'd struct: shutdown before any start is a safe no-op.
	  S2  start arms thread_started; shutdown RETURNS (a while(1) regression
	      hangs right here and reds by ctest timeout -- that is the
	      enforcement), disarms, and is idempotent.
	  S3  the object is restartable after shutdown: a second start/shutdown
	      cycle behaves identically (the stop flag must be re-cleared by
	      start, not latched forever).

	The thread is given NO world on purpose: init+start with no world is
	exactly the production boot state (main.c starts the thread before the
	world-select screen), so the pre-world update path is what runs here.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <m-lib/m-string.h>

#include "block/blocks.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "platform/thread.h"
#include "platform/time.h"

static struct server_local s;
static int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(__atomic_load_n(&g_drain_run, __ATOMIC_ACQUIRE)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

int main(void) {
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	/* Poison the stack image like the app's unzeroed `struct server_local
	   server;` -- init must make the lifecycle fields safe from garbage. */
	memset(&s, 0xAA, sizeof(s));
	server_local_init(&s, false, false, false, false, false, false, false);
	assert(!server_local_light_stopped(&s));
	for(unsigned i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
		assert(!s.worlds[i].light_fault
			&& s.worlds[i].light_fault_reason == SERVER_LIGHT_FAULT_NONE);

	/* S1: shutdown before any start must be a no-op, not a garbage join. */
	assert(!s.thread_started);
	server_local_shutdown(&s);
	assert(!s.thread_started);
	puts("  S1: pre-start shutdown is a safe no-op");

	/* S2: the real thread starts, runs the pre-world tick loop, and JOINS. */
	assert(server_local_start(&s));
	assert(s.thread_started);
	thread_msleep(200); /* > one 50ms tick period: the loop provably cycles */
	server_local_shutdown(&s); /* while(1) regression = hang = timeout red */
	assert(!s.thread_started);
	server_local_shutdown(&s); /* idempotent second call */
	assert(!s.thread_started);
	puts("  S2: start -> join -> disarmed (idempotent)");

	/* S3: restartable -- start must re-clear the stop flag, not inherit it. */
	assert(server_local_start(&s));
	assert(s.thread_started && !s.thread_stop);
	thread_msleep(80);
	server_local_shutdown(&s);
	assert(!s.thread_started);
	puts("  S3: second start/shutdown cycle clean (stop flag not latched)");

	/* S4 (2026-08-20 provider-swap redesign, hostile-review finding 1):
	   shutdown while the server thread is BLOCKED inside clin_rpc_send's
	   token backpressure against a full, UNDRAINED inbox -- the exact
	   deadlock: the old blind thread_join waited on a thread waiting on
	   clin_update, which only the joining (main) thread can run. The
	   drain-join must return. Enforcement is double: ctest timeout on hang
	   plus a generous in-test wall-clock bound. */
	__atomic_store_n(&g_drain_run, 0, __ATOMIC_RELEASE);
	thread_join(&drain);
	server_local_test_set_tick_crpc_flood(8);
	assert(server_local_start(&s));
	/* > RPC_INBOX_SIZE/8 tick periods: the flood provably fills the 16-slot
	   token pool and wedges the server thread mid-send. */
	thread_msleep(400);
	ptime_t s4_t0 = time_get();
	server_local_shutdown(&s);
	assert(time_diff_ms(s4_t0, time_get()) < 20000);
	assert(!s.thread_started);
	server_local_test_set_tick_crpc_flood(0);
	clin_drain_discard(); /* leave no undrained tokens behind */
	puts("  S4: shutdown drain-join returns against a flooded inbox");

	/* Cancelled exhaustion closes publication only for the old producer.
	 * A healthy restart must not inherit that stop latch. */
	assert(!clin_server_publication_stopped());
	struct client_rpc rpc = {.type = CRPC_TIME_SET, .payload.time_set = 123};
	clin_rpc_send(&rpc);
	assert(clin_test_inbox_pop(&rpc, NULL));
	assert(rpc.type == CRPC_TIME_SET && rpc.payload.time_set == 123);
	clin_test_rpc_release(&rpc);
	puts("  S5: cancelled producer's publication latch clears after drain-join");

	puts("test_server_shutdown_env: ALL PASS");
	return 0;
}
