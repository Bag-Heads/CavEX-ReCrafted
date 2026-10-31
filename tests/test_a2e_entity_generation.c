/* Copyright (c) 2026 CaveWii_v2 contributors
 * A2-E (split-dimensions-plan-0924, Astra "A4: A2-E" row, design M6):
 * generation-safe entity bind/resolve. Every server entity is BOUND to the
 * world instance it was constructed into -- the (slot, generation) handle,
 * not just the slot pointer -- and resolves (server_local_entity_world) only
 * while that exact instance is live. A world's storage slot is reused by its
 * successor (same address, new generation), so a pointer compare cannot tell
 * a stale entity from a live one; the generation stamp can.
 *
 * What a retired generation must NOT be able to do to its successor:
 *   1. resolve into it (server_local_entity_world == NULL, never the successor);
 *   2. be ticked by its pass, or appear in its entity queries (sections are
 *      per world: the same-coordinate collide push is world-isolated);
 *   3. construct into it through a queued job (arrow / fireball / split
 *      monster stamped with the retired world: dropped at flush);
 *   4. be mutated through a target reference held by a successor-world entity
 *      (a jockey rider/mount lookup resolves only inside one live world);
 *   5. re-enter a chunk section (membership refresh on a stale entity
 *      detaches it).
 * What must keep working: same-world queries/pushes, jobs queued for a LIVE
 * world construct into THAT world (not the routed slot), legacy unbound
 * fixtures (NULL world) still mean the routed world. (The dead
 * entity_choose_random_direction, which cast e->world to struct
 * server_local*, is deleted by this slice -- no callers.)
 *
 * Single-world byte identity is carried by the untouched goldens
 * (test_world_context_equiv, test_split_off_equiv, test_host_damage_golden,
 * test_damage_parity_golden, test_a1c_world_ops_equiv part 1) and by
 * test_entity_query_local_equiv (legacy query == slot-0 query).
 *
 * The A2-E API is announced by ENTITY_A2E_GENERATION_BIND (entity/entity.h).
 * Until it exists this test is RED by construction (exit 1, WILL_FAIL). */
#include "support/split_residency_fixture.h"

#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/item_types.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#ifndef ENTITY_A2E_GENERATION_BIND
int main(void) {
	puts("RED: ENTITY_A2E_GENERATION_BIND absent -- entities are not bound by "
		 "(slot, generation) yet");
	return 1;
}
#else

#define FNV_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)
#define GROUND_Y 3
#define POP_R 2 /* 5x5 chunks: the entity pass needs +/-32 blocks resident */

static uint64_t hash_bytes(uint64_t hash, const void* data, size_t n) {
	const uint8_t* p = data;
	while(n--) {
		hash ^= *p++;
		hash *= FNV_PRIME;
	}
	return hash;
}

/* One entity's observable simulation state. */
static uint64_t hash_entity(const struct entity* e) {
	uint64_t h = FNV_BASIS;
	h = hash_bytes(h, &e->id, sizeof(e->id));
	h = hash_bytes(h, e->pos, sizeof(vec3));
	h = hash_bytes(h, e->vel, sizeof(vec3));
	h = hash_bytes(h, &e->health, sizeof(e->health));
	h = hash_bytes(h, &e->delay_destroy, sizeof(e->delay_destroy));
	h = hash_bytes(h, &e->added_to_chunk, sizeof(e->added_to_chunk));
	h = hash_bytes(h, &e->chunk_coord_x, sizeof(e->chunk_coord_x));
	h = hash_bytes(h, &e->chunk_coord_y, sizeof(e->chunk_coord_y));
	h = hash_bytes(h, &e->chunk_coord_z, sizeof(e->chunk_coord_z));
	return h;
}

static void populate(struct server_local* s, struct server_world* w, int x,
					 int z) {
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 1000; n++) {
		server_world_begin_update(w);
		if(server_world_ensure_populated(w, s, x, z, &c))
			return;
	}
	assert(!"populate failed");
}

static void populate_square(struct server_local* s, struct server_world* w) {
	for(int z = -POP_R; z <= POP_R; z++)
		for(int x = -POP_R; x <= POP_R; x++)
			populate(s, w, x, z);
}

static struct server_world* make_slot1(struct server_local* s) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_OVERWORLD, 12345, true);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

/* Entities whose bound world RESOLVES to `w` (NULL: unresolvable ones). */
static size_t count_resolving(struct server_local* s, struct server_world* w) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && server_local_entity_world(s, e) == w)
			n++;
	}
	return n;
}

static size_t count_type_resolving(struct server_local* s,
								   struct server_world* w, enum entity_type t) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == t && server_local_entity_world(s, e) == w)
			n++;
	}
	return n;
}

/* The local Beta query over the whole populated square of `w`: which entity
 * ids does it answer with, and does it include the host player? */
#define QMAX 64
struct qans {
	size_t n;
	uint32_t ids[QMAX];
	bool player;
};
static struct qans query_all(struct server_local* s, struct server_world* w) {
	struct qans q = {0};
	assert(server_local_entity_query_local_w(s, w, -POP_R, -POP_R, POP_R,
											 POP_R, 0, 7));
	size_t n = 0;
	const struct server_entity_query_ref* refs
		= server_local_entity_query_snapshot(s, &n);
	assert(n <= QMAX);
	for(size_t i = 0; i < n; i++) {
		if(refs[i].player)
			q.player = true;
		else
			q.ids[q.n++] = refs[i].id;
	}
	return q;
}
static bool qans_has(const struct qans* q, uint32_t id) {
	for(size_t i = 0; i < q->n; i++)
		if(q->ids[i] == id)
			return true;
	return false;
}

static void observe(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
	clin_drain_discard();
}

int main(void) {
	assert(clin_init());
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_slot1(s);
	populate_square(s, a);
	populate_square(s, b);
	s->player.finished_loading = true;
	s->player.x = 0.5;
	s->player.z = 0.5;
	s->player.y = GROUND_Y + 1 + PLAYER_EYE_HEIGHT;
	s->world_time = 18000;
	server_local_skylight_recompute(s);
	s->rand_src.seed = 0x5EEDu;
	s->rand_src.has_next_gaussian = false;
	server_local_refresh_player_membership(s);
	clin_drain_discard();

	/* ---- 1. bind = (slot, generation) stamp; resolve = that live instance */
	struct entity* ea = server_local_spawn_monster_at_w(
		a, (vec3) {5.5F, GROUND_Y + 1, 5.5F}, MONSTER_ZOMBIE);
	struct entity* eb = server_local_spawn_monster_at_w(
		b, (vec3) {5.5F, GROUND_Y + 1, 5.5F}, MONSTER_ZOMBIE);
	struct entity* eb2 = server_local_spawn_monster_at_w(
		b, (vec3) {5.8F, GROUND_Y + 1, 5.5F}, MONSTER_ZOMBIE);
	assert(ea && eb && eb2);
	clin_drain_discard();
	clin_test_set_rpc_observer(observe, NULL);
	assert(a->generation != 0 && b->generation != 0);
	assert(ea->world_slot == 0 && ea->world_generation == a->generation);
	assert(eb->world_slot == 1 && eb->world_generation == b->generation);
	assert(server_local_entity_world(s, ea) == a);
	assert(server_local_entity_world(s, eb) == b);
	assert(server_local_entity_world(s, eb2) == b);
	/* Legacy unbound fixture entity (NULL world): the routed world, as every
	 * hand-built test entity has always meant. */
	struct entity unbound = {0};
	unbound.on_server = true;
	assert(server_local_entity_world(s, &unbound) == a);
	/* A never-live stray instance is refused, never slot 0. */
	struct server_world stray = {0};
	struct entity strayed = {0};
	strayed.on_server = true;
	entity_bind_world(&strayed, &stray);
	assert(server_local_entity_world(s, &strayed) == NULL);

	/* ---- 2. per-world sections: same-coordinate query/push isolation ---- */
	assert(server_local_test_tick_entities_w(s, a));
	assert(server_local_test_tick_entities_w(s, b));
	assert(ea->added_to_chunk && eb->added_to_chunk && eb2->added_to_chunk);
	struct qans qa = query_all(s, a), qb = query_all(s, b);
	assert(qa.player && qa.n == 1 && qans_has(&qa, ea->id));
	assert(!qb.player && qb.n == 2 && qans_has(&qb, eb->id)
		   && qans_has(&qb, eb2->id));
	/* The legacy (world-blind) spelling is exactly the slot-0 query. */
	assert(server_local_entity_query_local(s, -POP_R, -POP_R, POP_R, POP_R, 0, 7));
	size_t nleg = 0;
	server_local_entity_query_snapshot(s, &nleg);
	assert(nleg == 2); /* player + ea */
	/* Ticking slot 1 (two overlapping zombies push each other) leaves the
	 * slot-0 zombie at the same coordinates untouched -- the A1c PIN, flipped. */
	uint64_t ha = hash_entity(ea);
	for(int t = 0; t < 5; t++)
		assert(server_local_test_tick_entities_w(s, b));
	assert(hash_entity(ea) == ha);
	uint64_t hb = hash_entity(eb), hb2 = hash_entity(eb2);
	for(int t = 0; t < 5; t++)
		assert(server_local_test_tick_entities_w(s, a));
	assert(hash_entity(eb) == hb && hash_entity(eb2) == hb2);
	assert(hash_entity(ea) != ha); /* slot 0 did tick its own */

	/* ---- 3. queued jobs bind to the SPAWNING world (positive control) ---- */
	size_t before = dict_entity_size(s->entities);
	server_local_spawn_arrow((vec3) {5.5F, GROUND_Y + 3, 5.5F},
							 (vec3) {0.0F, 0.05F, 0.0F}, eb->id, ITEM_ARROW, 2,
							 false, s);
	server_local_spawn_fireball((vec3) {5.5F, GROUND_Y + 3, 5.5F},
								(float[3]) {1.0F, 0.0F, 0.0F}, eb->id, s);
	server_local_queue_monster_w(s, b, (vec3) {7.5F, GROUND_Y + 1, 7.5F},
								 MONSTER_PIG);
	assert(s->pending_arrow_count == 1 && s->pending_fireball_count == 1
		   && s->pending_monster_count == 1);
	/* Flushed by whichever pass runs (the routed world's here): they are
	 * constructed into the world they were queued for, not the routed slot. */
	assert(server_local_test_tick_entities_w(s, a));
	assert(dict_entity_size(s->entities) == before + 3);
	assert(count_resolving(s, a) == 1);
	assert(count_resolving(s, b) == 5);
	assert(count_type_resolving(s, b, ENTITY_ARROW) == 1);
	assert(count_type_resolving(s, b, ENTITY_FIREBALL) == 1);
	assert(count_type_resolving(s, a, ENTITY_ARROW) == 0);
	assert(count_type_resolving(s, a, ENTITY_FIREBALL) == 0);
	/* A slot-0 queue (host shooter, owner 0) still lands in slot 0. */
	server_local_spawn_arrow((vec3) {0.5F, GROUND_Y + 3, 0.5F},
							 (vec3) {0.0F, 0.05F, 0.0F}, 0, ITEM_ARROW, 2,
							 true, s);
	assert(server_local_test_tick_entities_w(s, b));
	assert(count_type_resolving(s, a, ENTITY_ARROW) == 1);
	assert(count_resolving(s, b) == 5);

	/* ---- 4. the retired generation cannot reach its successor ---- */
	/* Jobs queued for `b` while it is live, held across its retirement. */
	server_local_spawn_arrow((vec3) {5.5F, GROUND_Y + 3, 5.5F},
							 (vec3) {0.0F, 0.05F, 0.0F}, eb->id, ITEM_ARROW, 2,
							 false, s);
	server_local_spawn_fireball((vec3) {5.5F, GROUND_Y + 3, 5.5F},
								(float[3]) {1.0F, 0.0F, 0.0F}, eb->id, s);
	server_local_queue_monster_w(s, b, (vec3) {7.5F, GROUND_Y + 1, 7.5F},
								 MONSTER_PIG);
	assert(s->pending_arrow_count == 1 && s->pending_fireball_count == 1
		   && s->pending_monster_count == 1);
	/* A spider in slot 0 that believes the slot-1 zombie is its rider. */
	struct entity* spider = server_local_spawn_monster_at_w(
		a, (vec3) {-5.5F, GROUND_Y + 1, -5.5F}, MONSTER_SPIDER);
	assert(spider);
	spider->data.monster.jockey_rider_id = eb->id;
	eb->data.monster.jockey_mount_id = spider->id;

	uint32_t gen_old = b->generation;
	server_world_discard(b);
	assert(b->generation == 0);
	assert(server_local_entity_world(s, eb) == NULL);
	struct server_world* b2 = make_slot1(s);
	assert(b2 == b && b2->generation != 0 && b2->generation != gen_old);
	populate_square(s, b2);
	/* Same slot address, new generation: stale, never the successor. */
	assert(eb->world_slot == 1 && eb->world_generation == gen_old);
	assert(server_local_entity_world(s, eb) == NULL);
	assert(server_local_entity_world(s, eb2) == NULL);
	assert(count_resolving(s, b2) == 0);
	size_t stale = count_resolving(s, NULL);
	assert(stale == 5);
	/* A job queued NOW for a stale owner is refused at the queue. */
	server_local_spawn_arrow((vec3) {5.5F, GROUND_Y + 3, 5.5F},
							 (vec3) {0.0F, 0.05F, 0.0F}, eb->id, ITEM_ARROW, 2,
							 false, s);
	server_local_spawn_fireball((vec3) {5.5F, GROUND_Y + 3, 5.5F},
								(float[3]) {1.0F, 0.0F, 0.0F}, eb->id, s);
	assert(s->pending_arrow_count == 1 && s->pending_fireball_count == 1);

	struct entity* ec = server_local_spawn_monster_at_w(
		b2, (vec3) {5.5F, GROUND_Y + 1, 5.5F}, MONSTER_ZOMBIE);
	assert(ec && server_local_entity_world(s, ec) == b2);
	assert(ec->world_generation == b2->generation);
	before = dict_entity_size(s->entities);
	hb = hash_entity(eb);
	hb2 = hash_entity(eb2);
	uint64_t hsp = hash_entity(spider);
	/* Both passes: the stale jobs are dropped at flush (no construction into
	 * a or b2), the stale entities are neither ticked nor queried, and the
	 * spider's dangling rider reference is released without touching eb. */
	assert(server_local_test_tick_entities_w(s, a));
	assert(server_local_test_tick_entities_w(s, b2));
	assert(s->pending_arrow_count == 0 && s->pending_fireball_count == 0
		   && s->pending_monster_count == 0);
	assert(dict_entity_size(s->entities) == before);
	assert(count_resolving(s, b2) == 1);
	assert(count_resolving(s, NULL) == stale);
	assert(hash_entity(eb) == hb && hash_entity(eb2) == hb2);
	assert(hash_entity(spider) != hsp); /* the spider itself ticked */
	assert(spider->data.monster.jockey_rider_id == UINT32_MAX);
	assert(eb->data.monster.jockey_mount_id == spider->id); /* untouched */
	struct qans qb2 = query_all(s, b2);
	assert(!qb2.player && qb2.n == 1 && qans_has(&qb2, ec->id));
	assert(!qans_has(&qb2, eb->id) && !qans_has(&qb2, eb2->id));
	qa = query_all(s, a);
	assert(qa.player && !qans_has(&qa, eb->id) && !qans_has(&qa, ec->id));
	/* Membership refresh on a stale entity detaches it from every section. */
	eb->pos[0] += 16.0F; /* force a tuple change */
	server_local_refresh_entity_membership(s, eb);
	assert(!eb->added_to_chunk);
	/* A stale mover pushes nothing (its collide pass resolves no world). */
	uint64_t hc = hash_entity(ec);
	entity_living_collide_with_nearby(eb2, s);
	assert(hash_entity(ec) == hc);

	printf("PASS a2e entity generation: a gen %" PRIu32 ", b gen %" PRIu32
		   " -> %" PRIu32 "; stale=%zu inert, successor ticks %zu\n",
		   a->generation, gen_old, b2->generation, stale,
		   count_resolving(s, b2));

	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	server_world_discard(b2);
	split_destroy(s);
	return 0;
}
#endif
