/*
	Drain quiesce handshake for headless server_world tests (quiesce-0926).

	Most env tests run an ASYNCHRONOUS drain thread in the client's consumer
	role:  while(run) { clin_drain_discard(); thread_msleep(1); }.  Blocking
	clin_rpc_send tolerates its latency (it waits for a token). A NONBLOCKING
	admission does not: an EVICTING server_world_save_chunk admits the chunk's
	entity-destroy batch with clin_try_rpc_send (S5,
	server_local_try_release_chunk_entities) and correctly REFUSES the eviction
	(stage=entity_retire) when all RPC_INBOX_SIZE tokens are queued. The drain
	sleeps ~15 ms per pass on Windows (timer granularity) and longer on a loaded
	host, so a burst of RPC_INBOX_SIZE sends just before the save leaves the
	pool empty at admission often enough to flake (measured on
	test_minecart_types_env: 7/300 on main d6c1cbda, 3/200 on G3).

	A test that asserts ONE evicting save succeeds must establish the
	precondition it depends on -- an empty inbox -- instead of racing the drain:

	  - the drain thread calls test_drain_pass() where it called
	    clin_drain_discard() in its loop;
	  - the test thread calls test_drain_quiesce() right before the save.

	test_drain_quiesce() waits for a drain pass that STARTED after every RPC the
	calling thread has sent. The calling thread must be the only producer while
	it waits (true for every single-threaded fixture: it is parked here), so
	every token is free when it returns. A drain thread that is not running
	would hang the wait: a generous ceiling turns that harness bug into a loud
	abort (after 60000 1-ms sleeps, i.e. >= 60 s) rather than a silent hang.
	The ceiling is never the pass condition -- no correct run comes near it.

	Header-only, static state: include it from exactly one TU per test binary.
	Test-only; not linked into the game.
*/
#ifndef CAVEWII_TEST_DRAIN_QUIESCE_H
#define CAVEWII_TEST_DRAIN_QUIESCE_H

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "network/client_interface.h"
#include "platform/thread.h"

static atomic_uint test_drain_req_;
static atomic_uint test_drain_done_;

/* Drain-thread side: one full discard pass, then acknowledge every quiesce
 * request that was already raised when the pass began. */
static inline void test_drain_pass(void) {
	unsigned req = atomic_load(&test_drain_req_);
	clin_drain_discard();
	atomic_store(&test_drain_done_, req);
}

/* Producer side: returns once the client inbox is empty (all tokens free). */
static inline void test_drain_quiesce(void) {
	unsigned want = atomic_fetch_add(&test_drain_req_, 1) + 1;
	for(unsigned waited_ms = 0; atomic_load(&test_drain_done_) != want;
		waited_ms++) {
		if(waited_ms >= 60000) {
			fprintf(stderr, "test_drain_quiesce: no drain pass after 60000 sleeps "
							"(>= 60 s) -- is "
							"the drain thread running test_drain_pass()?\n");
			abort();
		}
		thread_msleep(1);
	}
}

#endif
