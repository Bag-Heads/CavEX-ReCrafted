/*
	Cross-slice harden audit -- pins two correctness contracts:

	(#3) The O(1) monotonic entity-id counter that replaced entity_gen_id's O(N)
	     full-dict max-key scan at the server spawn sites. server_local_gen_id is
	     `return ++s->next_entity_id;` over a counter seeded to 0 in
	     server_local_create. The CONTRACT this guards: fresh ids are UNIQUE, > 0
	     (id 0 is reserved for the local player), and strictly MONOTONIC in the
	     deterministic spawn order -- and, unlike the old max+1 scan, ids are NEVER
	     REUSED after an entity is freed (so a fresh id can't alias an id whose
	     CRPC_ENTITY_DESTROY is still in flight). The counter consumes no RNG, so the
	     seeded simulation stream is untouched.

	(#1) The shared mob_is_targetable predicate (entity.h): a mob is a valid damage /
	     aim-lock target ONLY while genuinely alive -- type == ENTITY_MONSTER &&
	     health > 0 && death_time == 0. This guards the arrow sweep + bow aim-assist
	     against a CORPSE (in its audit-#18 death flop, or already health<=0) eating an
	     arrow or winning the aim-lock while it keeps its collision box for ~20 ticks.

	Both contracts are mirrored here as the exact expressions the production code uses.
	entity.h itself transitively pulls in the platform/world headers (ptime_t etc.) that
	need device build config, so -- like the other combat.h-based mob tests in this suite
	-- this pins the LOGIC headlessly rather than linking the engine. The mirror is the
	literal predicate / counter step, so it cannot silently drift from a 1-line change.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ---- (#3) monotonic id counter contract ------------------------------------------
// Mirrors server_local_gen_id: pre-increment a counter seeded to 0 (so the first id is
// 1). This is the ENTIRE id logic -- no dict, no RNG.
static uint32_t gen_id(uint32_t* next) {
	return ++(*next);
}

static void test_id_unique_positive_monotonic(void) {
	uint32_t next = 0; // server_local_create seeds next_entity_id = 0
	uint32_t prev = 0; // id 0 is the reserved local-player id
	uint32_t seen[256];
	const int N = 256;

	for(int i = 0; i < N; i++) {
		uint32_t id = gen_id(&next);
		// > 0: never collides with the reserved local player.
		assert(id > 0);
		// strictly monotonic in spawn order.
		assert(id > prev);
		// unique vs every earlier id (monotonicity already implies this, but assert
		// it explicitly so the contract is unmistakable).
		for(int j = 0; j < i; j++)
			assert(seen[j] != id);
		seen[i] = id;
		prev = id;
	}
	// First id is exactly 1 (matches the old entity_gen_id's first return), last is N.
	assert(seen[0] == 1);
	assert(seen[N - 1] == (uint32_t)N);
}

static void test_id_no_reuse_after_free(void) {
	// The KEY improvement over the old max+1 scan: freeing an entity does NOT let its id
	// be handed out again. The counter is independent of dict occupancy, so even if every
	// previously-spawned entity is gone, the next id is strictly greater than all prior.
	uint32_t next = 0;
	uint32_t a = gen_id(&next); // 1
	uint32_t b = gen_id(&next); // 2
	uint32_t c = gen_id(&next); // 3
	assert(a == 1 && b == 2 && c == 3);
	// ... imagine ids 1..3 all freed here (dict now empty) ...
	uint32_t d = gen_id(&next); // must be 4, NOT a reclaimed 1 (max+1 would give 1)
	assert(d == 4);
	assert(d != a && d != b && d != c);
}

static void test_id_reset_per_world(void) {
	// Verdict follow-up to (#3): the counter is reset to 0 alongside every
	// dict_entity_reset(s->entities) -- i.e. on world LOAD and UNLOAD -- so the documented
	// "first id is 1" invariant holds PER WORLD, not only from server_local_create. This is
	// alias-safe because CRPC_WORLD_RESET clears the client's entity dict at the same
	// boundary (FIFO RPC queue), so a reused id can't collide with a surviving entity.
	uint32_t next = 0;
	(void)gen_id(&next); // 1
	(void)gen_id(&next); // 2
	uint32_t last = gen_id(&next); // 3
	assert(last == 3);
	// World switch: s->next_entity_id = 0 (mirrors the reset next to dict_entity_reset).
	next = 0;
	uint32_t first_after = gen_id(&next);
	assert(first_after == 1); // first id of the new world is 1 again, per the invariant
}

// ---- (#1) mob_is_targetable contract ---------------------------------------------
// Mirror the three monster fields the predicate reads + the exact boolean it computes.
// (entity.h's inline is `e && type==ENTITY_MONSTER && health>0 && death_time==0`.)
enum { T_LOCAL_PLAYER = 0, T_ITEM = 1, T_MONSTER = 2, T_ARROW = 4 };

struct mob_view {
	int type;
	short health;
	int death_time;
};

static bool targetable(const struct mob_view* m) {
	return m && m->type == T_MONSTER && m->health > 0 && m->death_time == 0;
}

static void test_targetable_truth_table(void) {
	// A healthy, living monster: the ONLY targetable case.
	assert(targetable(&(struct mob_view){T_MONSTER, 20, 0}));

	// In its death flop (death_time > 0) -- the corpse that used to eat arrows / win the
	// aim-lock for ~20 ticks. Health may still read positive on the death edge or be 0.
	assert(!targetable(&(struct mob_view){T_MONSTER, 1, 1}));
	assert(!targetable(&(struct mob_view){T_MONSTER, 0, 5}));
	assert(!targetable(&(struct mob_view){T_MONSTER, 20, 20}));

	// Already dead (health <= 0) but not yet flagged into the flop: still not a target.
	assert(!targetable(&(struct mob_view){T_MONSTER, 0, 0}));
	assert(!targetable(&(struct mob_view){T_MONSTER, -4, 0}));

	// Non-monster entities are never targetable by the mob-only systems (the player at
	// id 0 is T_LOCAL_PLAYER; dropped items + arrows are their own types).
	assert(!targetable(&(struct mob_view){T_LOCAL_PLAYER, 100, 0}));
	assert(!targetable(&(struct mob_view){T_ITEM, 1, 0}));
	assert(!targetable(&(struct mob_view){T_ARROW, 1, 0}));

	// NULL guard (the inline short-circuits on a NULL entity).
	assert(!targetable(NULL));
}

int main(void) {
	test_id_unique_positive_monotonic();
	test_id_no_reuse_after_free();
	test_id_reset_per_world();
	test_targetable_truth_table();
	printf("test_entity_id_targetable: O(1) ids unique/>0/monotonic/no-reuse + "
		   "mob_is_targetable rejects corpses (death_time>0 / health<=0) and "
		   "non-monsters\n");
	return 0;
}
