/*
	Regression guard for the 2026-08-14 world-load crash: the app's
	`struct server_local` lives on main()'s STACK (main.c `struct server_local
	server;`), so any field server_local_init does not explicitly initialize
	holds stack garbage. The weather slice's bolt list (weather.active_bolts /
	_count / _cap) relied on zero-init that never happens in the app, and the
	first world load's server_weather_bolts_destroy() then free()d garbage --
	a c0000374 heap-corruption abort on PC, killing the app on EVERY world
	load. The 236-test suite stayed green the whole time because every test
	zeroes its struct (static storage or = {0}).

	This test closes that class-hole: it POISONS the struct with 0xAA first --
	a strictly harsher starting state than any real stack -- then runs the real
	init and the exact teardown call the load path makes. Pre-fix this free()s
	0xAAAA... and dies; post-fix it is free(NULL) + field asserts.

	Deliberately narrow: init + the crashing teardown sequence only, not a full
	world load (which needs a save dir and drags in unrelated machinery).
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/network/server_local.h"
#include "../source/network/server_local_state.h" // full server state (split from server_local.h)
#include "../source/network/server_weather.h"
#include "../source/network/server_world.h" // struct server_chunk, server_chunk_init

/* Boot-hang audit (2026-09-02, agreed with peer session), same class-hole as
 * the struct server_local sweep above: struct server_chunk's near_send_fail_
 * streak/near_send_fail_seq (35af34a, the near-send first-failure-backoff
 * fix) are read by server_local.c's near-send scan on every candidate this
 * chunk is considered for. Every current construction site was independently
 * confirmed to already reach zero for these two fields (the disk-load site's
 * designated-initializer compound literal zero-fills every unlisted member
 * per C11 6.7.9p19-21; the generation and far-skin-extract sites now call
 * server_chunk_init or memset(0) explicitly) -- but that guarantee is only as
 * good as server_chunk_init's own implementation staying a full memset. This
 * pins the class the same way the server_local sweep above does: poison with
 * 0xAA, run the real constructor, assert every field -- not just the two new
 * ones, so a FUTURE field added to struct server_chunk without updating
 * server_chunk_init also fails here instead of shipping silently garbage. */
static void test_server_chunk_init_poison(void) {
	struct server_chunk sc;
	memset(&sc, 0xAA, sizeof sc);

	server_chunk_init(&sc);

	assert(sc.ids == NULL);
	assert(sc.metadata == NULL);
	assert(sc.lighting_sky == NULL);
	assert(sc.lighting_torch == NULL);
	assert(sc.heightmap == NULL);
	assert(!sc.modified);
	assert(!sc.persisted_tile_entities_complete);
	assert(!sc.persisted_entities_nonempty);
	assert(!sc.populated);
	assert(!sc.sent);
	assert(!sc.load_ticks_rearmed);
	assert(sc.load_tick_rearm_cursor == 0);
	/* The two fields the boot-hang report named directly: a poisoned streak
	 * (0xAA = 170, already >= NEAR_SEND_FAIL_STREAK_THRESHOLD) would escalate
	 * a chunk into the bounded backoff table on its very first REAL failure
	 * instead of its third; a poisoned seq would make the "failed once this
	 * same tick" same-tick exclusion compare against garbage instead of the
	 * live near_send_seq. Neither can spin (every consumer bounds its loop by
	 * a fixed table size or a fixed view-distance grid, never by these
	 * field's own values -- audited, not assumed), but both are silently
	 * wrong until this asserts they start at 0. */
	assert(sc.near_send_fail_streak == 0);
	assert(sc.near_send_fail_seq == 0);

	printf("  server_chunk_init: 0xAA-poisoned struct server_chunk zeroes "
		   "every field (near_send_fail_streak/seq included)\n");
}

int main(void) {
	test_server_chunk_init_poison();

	/* Heap-alloc: the struct is large, and a poisoned heap block models the
	 * app's uninitialized stack exactly as well. */
	struct server_local* s = malloc(sizeof *s);
	assert(s);
	memset(s, 0xAA, sizeof *s);

	/* The production init path (thread spawn deliberately excluded -- same
	 * seam every other env test uses). */
	server_local_test_create_without_thread(s, false, false, false, false,
											false, false, false);

	/* The invariant the load path depends on: a fresh server_local must have
	 * an empty, free()-safe bolt list BEFORE the first world load runs
	 * server_weather_bolts_destroy. */
	assert(s->weather.active_bolts == NULL);
	assert(s->weather.active_bolts_count == 0);
	assert(s->weather.active_bolts_cap == 0);
	/* Clean-quit lifecycle (the amahpour-shutdown fix): poisoned
	 * thread_started/thread_stop would make server_local_shutdown join a
	 * GARBAGE thread handle on quit, or the thread exit on its first tick. */
	assert(!s->thread_stop);
	assert(!s->thread_started);

	/* Full pointer-field sweep (2026-08-14 audit): every pointer member of
	 * struct server_local must leave init in a deref/free-safe state, because
	 * NONE of them can rely on zero-init on the app's stack struct. This list
	 * is the audited roster at the time of the bolt-list fix -- if you add a
	 * pointer field, add its post-init invariant here (and if you REMOVE an
	 * init line, this is the test that names the regression). */
	assert(s->portal_job == NULL);
	assert(s->death_drop_journal == NULL);
	assert(s->entity_tick_keys == NULL);
	assert(s->entity_query_refs == NULL);
	assert(s->player.active_inventory == &s->player.inventory);
	assert(s->player.fish_entity_id == 0);
	/* Audit 2026-09-01 #1 (REGRESSION-TODAY, CONFIRMED), FIX 1(b): the SAME
	 * uninitialized-automatic hazard as fish_entity_id/riding_boat above --
	 * struct server_player.rider_vel_x/z had no initializer at all. The
	 * 0xAA poison pattern reinterpreted as float is never bit-identical to
	 * 0.0f, so this fails pre-fix and passes post-fix. */
	assert(s->player.rider_vel_x == 0.0f);
	assert(s->player.rider_vel_z == 0.0f);
	/* Deviation-#18 death latch: a garbage-true has_death_pos on the app's
	 * unzeroed stack struct would snap the first respawn onto garbage
	 * coordinates (the GB-01 class). */
	assert(!s->player.has_death_pos);
	/* world_populate_scratch is the one pointer init ALLOCATES (not NULLs);
	 * poisoned bytes must have been replaced by a real allocation or NULL
	 * (alloc-failure path) -- either is free-safe, garbage is neither. A
	 * poisoned survivor would compare equal to the 0xAA fill pattern. */
	assert((uintptr_t)s->world_populate_scratch != (uintptr_t)0xAAAAAAAAAAAAAAAAull
		   && (uintptr_t)s->world_populate_scratch != (uintptr_t)0xAAAAAAAAull);

	/* Positional TE tables (2026-08-14 dispenser-init hole): every table the
	 * shared reset helper (server_local_reset_positional_te_tables) owns must
	 * leave init in the EXACT memset state. Assert the exact -1 fill, not the
	 * "y < 0 == free" contract: the 0xAA poison reads as a NEGATIVE int32
	 * (0xAAAAAAAA), so a y<0 check passes on poison and hides the hole --
	 * only the byte-exact fill distinguishes initialized from garbage. When
	 * this sweep was added, dispensers were MISSING from init AND from the
	 * world-load boundary clear (stack-garbage y >= 0 makes every free-slot
	 * scan see occupied slots in real play; the suite never saw it because
	 * tests zero the struct -- this test's whole reason to exist). */
	for(int i = 0; i < MAX_FURNACES; i++)
		assert(s->furnace_pos[i].x == -1 && s->furnace_pos[i].y == -1
			   && s->furnace_pos[i].z == -1);
	for(int i = 0; i < MAX_DISPENSERS; i++) {
		assert(s->dispenser_pos[i].x == -1 && s->dispenser_pos[i].y == -1
			   && s->dispenser_pos[i].z == -1);
		assert(s->dispensers[i].items[0].id == 0
			   && s->dispensers[i].items[DISPENSER_TE_SLOTS - 1].id == 0);
	}
	for(int i = 0; i < MAX_NOTEBLOCKS; i++) {
		assert(s->note_pos[i].x == -1 && s->note_pos[i].y == -1
			   && s->note_pos[i].z == -1);
		assert(s->notes[i] == 0);
		assert(!s->note_prev_powered[i]);
	}
	for(int i = 0; i < MAX_JUKEBOXES; i++) {
		assert(s->record_player_pos[i].x == -1
			   && s->record_player_pos[i].y == -1
			   && s->record_player_pos[i].z == -1);
		assert(s->records[i] == 0);
	}

	/* Replay the exact call that crashed: load-path teardown on a
	 * never-populated list. Post-fix this is free(NULL) -- must be a no-op. */
	server_weather_bolts_destroy(s);
	assert(s->weather.active_bolts == NULL);
	assert(s->weather.active_bolts_count == 0);
	assert(s->weather.active_bolts_cap == 0);
	/* Clean-quit lifecycle (the amahpour-shutdown fix): poisoned
	 * thread_started/thread_stop would make server_local_shutdown join a
	 * GARBAGE thread handle on quit, or the thread exit on its first tick. */
	assert(!s->thread_stop);
	assert(!s->thread_started);

	/* And twice, as a dimension switch would: destroy must be idempotent. */
	server_weather_bolts_destroy(s);
	assert(s->weather.active_bolts == NULL);

	free(s);
	printf("test_server_struct_poison_env: OK (0xAA-poisoned server_local "
		   "survives init + load-path weather teardown)\n");
	return 0;
}
