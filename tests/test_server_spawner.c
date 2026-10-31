/*
	FH-17: proves the runtime MobSpawner tile-entity store (source/network/
	server_spawner.c) -- the exact seam server_local_tick_spawners uses to
	resolve a spawner's mob type -- actually drives spawn type end to end:
	spawner_te_upsert writes a row, spawner_te_lookup finds it by position, and
	feeding its persisted EntityId through spawner_mob_from_id (spawner_mob_
	id.c) yields the right engine monster id. Also covers the table-lifecycle
	guarantees the position-keyed dispenser/noteblock tables already prove
	(server_dispenser.c / server_noteblock.c): overwrite-by-position (not
	duplication), table-full refusal, and free-then-inert.

	No world/thread/region I/O: a zero-initialised struct server_local is
	enough, since these functions touch only its spawner_te_pos[]/
	spawner_tes[] arrays. Links only spawner_mob_id.c (dependency-light) +
	server_spawner.c -- NOT spawner.c's RNG burst core, which this test never
	exercises and which would otherwise pull in the block registry/util.c for
	nothing (mirrors test_spawner.c's own minimal-link philosophy one layer
	down, but even lighter thanks to the spawner.c / spawner_mob_id.c split).
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entity/monsters_object.h" // MONSTER_*
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_spawner.h"
#include "network/spawner_mob_id.h"

static struct server_local s;

/* IMP-TE-CAPS: this dependency-light test links no tile_entities_store.c, so
 * it hand-allocates a FIXED (non-growing) spawner table of the historical
 * MAX_SPAWNER_TES rows; the full-table refusal below is that fixed view's. */
static void alloc_spawner_rows(struct server_local* x) {
	x->te_rows[TE_KIND_SPAWNER] = MAX_SPAWNER_TES;
	x->spawner_te_pos = malloc(MAX_SPAWNER_TES * sizeof(*x->spawner_te_pos));
	x->spawner_tes = calloc(MAX_SPAWNER_TES, sizeof(*x->spawner_tes));
	x->te_owners.spawner_slot = calloc(MAX_SPAWNER_TES, 1);
	x->te_owners.spawner_gen = calloc(MAX_SPAWNER_TES, sizeof(uint32_t));
	assert(x->spawner_te_pos && x->spawner_tes && x->te_owners.spawner_slot
		   && x->te_owners.spawner_gen);
}

int main(void) {
	memset(&s, 0, sizeof(s));
	/* A1b: the legacy (s, ...) adapters are slot 0's view; give slot 0 a live
	 * instance identity (the tables are owner-keyed). */
	s.worlds[0].session = &s;
	s.worlds[0].generation = 1;
	alloc_spawner_rows(&s);
	for(int i = 0; i < MAX_SPAWNER_TES; i++)
		s.spawner_te_pos[i].y = -1; // all free (server_local_reset_positional_te_tables's own convention)

	// (1) No TE at a position: lookup returns NULL, and the resolution chain
	// server_local_tick_spawners uses (lookup -> spawner_mob_from_id) is
	// therefore inert -- proves the runtime no longer has a metadata
	// fallback to silently succeed through.
	assert(spawner_te_lookup(&s, 5, 64, 5) == NULL);

	// (2) upsert creates a row; lookup finds it; the persisted EntityId
	// resolves to the correct engine monster id via spawner_mob_from_id --
	// THE seam that proves "the store drives spawn type".
	assert(spawner_te_upsert(&s, 5, 64, 5, "Spider"));
	struct spawner_te* te = spawner_te_lookup(&s, 5, 64, 5);
	assert(te && strcmp(te->mob_id, "Spider") == 0);
	assert(te->delay == 20); // Beta TileEntityMobSpawner() ctor default
	assert(spawner_mob_from_id(te->mob_id) == MONSTER_SPIDER);

	// (3) A different position is untouched.
	assert(spawner_te_lookup(&s, 6, 64, 5) == NULL);

	// (4) upsert at an EXISTING position overwrites in place -- not a second
	// row (mirrors dispenser/noteblock's own overwrite-by-position contract).
	assert(spawner_te_upsert(&s, 5, 64, 5, "Zombie"));
	int occupied = 0;
	for(int i = 0; i < MAX_SPAWNER_TES; i++)
		if(s.spawner_te_pos[i].y >= 0)
			occupied++;
	assert(occupied == 1);
	te = spawner_te_lookup(&s, 5, 64, 5);
	assert(te && strcmp(te->mob_id, "Zombie") == 0);
	assert(spawner_mob_from_id(te->mob_id) == MONSTER_ZOMBIE);

	// (5) An oversized EntityId (can only arrive via a corrupt/hand-edited
	// save -- worldgen and the migration only ever write the 3 short Beta
	// literals) is truncated, never overflowed.
	char long_id[SPAWNER_MOB_ID_MAX + 16];
	memset(long_id, 'A', sizeof(long_id) - 1);
	long_id[sizeof(long_id) - 1] = '\0';
	assert(spawner_te_upsert(&s, 7, 64, 7, long_id));
	te = spawner_te_lookup(&s, 7, 64, 7);
	assert(te && strlen(te->mob_id) == SPAWNER_MOB_ID_MAX - 1);
	assert(spawner_mob_from_id(te->mob_id) == -1); // unrecognised -> inert, not a crash

	// (6) free clears the slot -- block_spawner.c's onBreak contract -- and
	// the position becomes reusable.
	spawner_te_free(&s, 5, 64, 5);
	assert(spawner_te_lookup(&s, 5, 64, 5) == NULL);
	assert(spawner_te_upsert(&s, 9, 64, 9, "Skeleton")); // reuses the freed slot
	int occupied2 = 0;
	for(int i = 0; i < MAX_SPAWNER_TES; i++)
		if(s.spawner_te_pos[i].y >= 0)
			occupied2++;
	// (5,64,5) was freed above; only (7,64,7) [step 5] and the fresh
	// (9,64,9) remain -- the untouched-position check in (3) never added a
	// row, and it never will (spawner_te_lookup is read-only).
	assert(occupied2 == 2);

	// (7) Fill the table, then prove a genuinely full table refuses a NEW
	// position (an existing one still overwrites).
	struct server_local full;
	memset(&full, 0, sizeof(full));
	full.worlds[0].session = &full; /* A1b: explicit live slot-0 identity */
	full.worlds[0].generation = 2;
	alloc_spawner_rows(&full);
	for(int i = 0; i < MAX_SPAWNER_TES; i++)
		full.spawner_te_pos[i].y = -1;
	for(int i = 0; i < MAX_SPAWNER_TES; i++)
		assert(spawner_te_upsert(&full, i, 64, 0, "Zombie"));
	assert(!spawner_te_upsert(&full, 1000, 64, 0, "Zombie")); // table full
	assert(spawner_te_upsert(&full, 0, 64, 0, "Skeleton"));   // existing position still overwrites
	te = spawner_te_lookup(&full, 0, 64, 0);
	assert(te && strcmp(te->mob_id, "Skeleton") == 0);

	printf("test_server_spawner: upsert/lookup/free + store-drives-spawn-type "
		   "+ overwrite/truncate/full-table all PASS\n");
	return 0;
}
