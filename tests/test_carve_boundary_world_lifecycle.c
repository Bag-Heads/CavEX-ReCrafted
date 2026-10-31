/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * The carve-attribution observer is armed at BOOT (source/main.c, before the
 * config load and before any world exists) and stays armed for the whole
 * process, while the server tick boundary that feeds it
 * (server_carve_boundary, source/network/server_local.c's tick loop) starts
 * firing as soon as the server thread starts. A struct server_world has NO
 * chunk dict in two real windows: from process start until the first
 * server_world_create, and from every server_world_destroy/_discard until the
 * next create (menu -> world -> menu -> world). m*lib's cleared dict leaves
 * index == NULL, so iterating it in those windows is a NULL dereference: on PC
 * it aborts on the dict's own assert, and on the Wii (asserts compiled out) it
 * killed the server thread on its first tick -- an armed --cw-carve-census run
 * never reached main_loop_enter and sent zero heartbeats in 45 s.
 *
 * This pins the contract: the boundary hook is inert -- and above all does not
 * touch the dict -- whenever server_world_chunks_ready() is false, and still
 * publishes its real labels when it is true. */
#include "support/split_residency_fixture.h"
#include "carve_observer.h"
#include <stdio.h>

int main(void) {
	assert(carve_observer_start());
	assert(server_carve_boundary);

	/* Window 1 -- boot. main.c's `server` is a zeroed static until
	 * server_local_init/server_world_create run; the server thread's first
	 * ticks land here. Old code: abort inside dict_server_chunks_it. */
	struct server_local* boot = calloc(1, sizeof(*boot));
	assert(boot);
	server_carve_boundary(boot);
	struct carve_totals t;
	carve_observer_snapshot(&t);
	assert(!t.errors);
	assert(t.attribution.retired == 0);
	free(boot);

	/* Live world: the same hook does its real work and publishes the server's
	 * startup-retirement counters through the ledger. */
	struct server_local* s = split_fixture();
	assert(server_world_chunks_ready(&s->world));
	split_populate(s, 0, 0);
	s->startup_retired = 7;
	s->startup_save_failed = 3;
	server_carve_boundary(s);
	carve_observer_snapshot(&t);
	assert(!t.errors);
	assert(t.attribution.retired == 7 && t.attribution.save_failed == 3);

	/* Window 2 -- unload. Every back-to-title clears the dict while the server
	 * thread keeps ticking. The hook must go inert again (published labels stay
	 * at their last real values) instead of walking a freed index. */
	server_world_discard(&s->world);
	assert(!server_world_chunks_ready(&s->world));
	s->startup_retired = 99;
	s->startup_save_failed = 99;
	server_carve_boundary(s);
	carve_observer_snapshot(&t);
	assert(!t.errors);
	assert(t.attribution.retired == 7 && t.attribution.save_failed == 3);

	/* Reload: the permission comes back with the new dict, and the boundary
	 * resumes publishing. (Also restores the live world split_destroy expects.) */
	string_t path;
	string_init_set_str(path, split_path);
	assert(server_world_create_with_populate_scratch(&s->world, path,
		WORLD_DIM_OVERWORLD, 12345, true, s->world_populate_scratch));
	string_clear(path);
	s->world.local = s;
	s->worlds[0].session = s; /* A1a (D1): owning session for services/callbacks */
	assert(server_world_chunks_ready(&s->world));
	server_carve_boundary(s);
	carve_observer_snapshot(&t);
	assert(!t.errors);
	assert(t.attribution.retired == 99 && t.attribution.save_failed == 99);

	split_destroy(s);
	assert(carve_observer_stop());
	printf("test_carve_boundary_world_lifecycle: OK\n");
	return 0;
}
