/* Copyright (c) 2026 CaveWii_v2 contributors
 * A1c (split-dimensions-plan-0924, Astra "A2: A1c" row): the world operations
 * that server_local_update runs every tick -- the entity pass, the item
 * onUpdate pass, the natural spawn passes (hostile / animal / water), the
 * dungeon-spawner pass, the skylight recompute, the weather tick, the
 * weather-effects ring, the lightning-bolt sweep and the near-unload send --
 * are INDEPENDENTLY CALLABLE per `struct server_world*`, consume A1b's `_w`
 * tile/effect APIs, and leave single-world behaviour byte-identical.
 *
 * PART 1 -- SLOT-0 ADAPTER EQUIVALENCE. Two identical sessions: one drives
 * the legacy `(s)` entry points, the other the `_w(s, &s->worlds[0])` ones.
 * Every fold (entity table, RNG stream, skylight, weather, chunk bytes,
 * publication count) must be identical -- the legacy names are exactly the
 * slot-0 adapters, nothing else. (Byte-identity against the PRE-A1c tree is
 * carried by the untouched whole-sim goldens: test_world_context_equiv
 * 0x518a97fc7f32ad65, test_split_off_equiv, test_host_damage_golden,
 * test_block_hooks_actor_golden, the column_dedup pair.)
 *
 * PART 2 -- SAME-COORDINATE ISOLATION. A second live world (slot 1, same
 * seed and dimension, populated at the SAME chunks) with its own bound
 * entity. Running every `_w` operation on slot 1 leaves slot 0's chunk
 * bytes, entity table and client stream untouched; the spawn/spawner passes
 * on a world with NO eligible actor draw NOTHING from the shared RNG; slot
 * 0's operations never touch slot 1's entity. Only the routed world (slot 0)
 * publishes.
 *
 * The A1c API is announced by SERVER_LOCAL_A1C_WORLD_OPS (server_local.h).
 * Until it exists this test is RED by construction (exit 1, registered
 * WILL_FAIL), the A1b RED-scaffold convention. */
#include "support/split_residency_fixture.h"

#include "entity/monsters_object.h"
#include "network/server_spawner.h"
#include "network/server_weather.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

#ifndef SERVER_LOCAL_A1C_WORLD_OPS
int main(void) {
	puts("RED: SERVER_LOCAL_A1C_WORLD_OPS absent -- world operations are not "
		 "callable per struct server_world* yet");
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

/* The entity's bound world, read by offset so the lexical world-alias gate
 * does not mistake the entity field for the server_local alias. */
static void* entity_bound_world(const struct entity* e) {
	return *(void* const*)((const char*)e + offsetof(struct entity, world));
}

/* Publications split by producer family: the ENTITY_* movement family is
 * emitted by the entity code itself (entity_monster.c & co., not through the
 * A1b/A1c world-explicit emitters) and is B1's "entities" stamping row; every
 * other CRPC here is a world-bound publication that A1c gates on the client
 * route. */
static unsigned long sends, world_sends, entity_sends;
static int dbg_tick = -1, dbg_side = 0;
static bool rpc_is_entity_traffic(enum client_rpc_type t) {
	return t == CRPC_ENTITY_MOVE || t == CRPC_ENTITY_LOOK
		|| t == CRPC_ENTITY_VELOCITY || t == CRPC_ENTITY_FLAGS
		|| t == CRPC_ENTITY_ANIMATION || t == CRPC_ENTITY_PHYSICS_CORRECTION
		|| t == CRPC_ENTITY_EQUIPMENT || t == CRPC_ATTACH_ENTITY
		|| t == CRPC_ENTITY_DESTROY || t == CRPC_ENTITY_DESTROY_BATCH
		|| t == CRPC_SOUND || t == CRPC_SMOKE_PUFF || t == CRPC_WOLF_FX
		|| t == CRPC_WOLF_SPLASH;
}
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	sends++;
	if(getenv("A1C_TRACE"))
		fprintf(stderr, "S side=%d tick=%d type=%u snd=%u pos=%.2f,%.2f,%.2f\n",
				dbg_side, dbg_tick, (unsigned)call->type,
				call->type == CRPC_SOUND ? call->payload.sound.sound_id : 0u,
				call->type == CRPC_SOUND ? call->payload.sound.pos[0] : 0.0f,
				call->type == CRPC_SOUND ? call->payload.sound.pos[1] : 0.0f,
				call->type == CRPC_SOUND ? call->payload.sound.pos[2] : 0.0f);
	if(rpc_is_entity_traffic(call->type))
		entity_sends++;
	else
		world_sends++;
	clin_drain_discard();
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

static int compare_keys(const void* a, const void* b) {
	int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t chunk_hash(struct server_world* w) {
	size_t count = dict_server_chunks_size(w->chunks);
	int64_t* keys = malloc((count ? count : 1) * sizeof(*keys));
	assert(keys);
	size_t n = 0;
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it))
		keys[n++] = dict_server_chunks_ref(it)->key;
	qsort(keys, n, sizeof(*keys), compare_keys);
	uint64_t h = FNV_BASIS;
	for(size_t i = 0; i < n; i++) {
		struct server_chunk* c = dict_server_chunks_get(w->chunks, keys[i]);
		assert(c);
		h = hash_bytes(h, &keys[i], sizeof(keys[i]));
		for(w_coord_t y = 0; y < WORLD_HEIGHT; y++)
			for(w_coord_t z = 0; z < CHUNK_SIZE; z++)
				for(w_coord_t x = 0; x < CHUNK_SIZE; x++) {
					struct block_data b;
					assert(server_world_get_block(
						w, S_CHUNK_X(keys[i]) * CHUNK_SIZE + x, y,
						S_CHUNK_Z(keys[i]) * CHUNK_SIZE + z, &b));
					h = hash_bytes(h, &b, sizeof(b));
				}
	}
	free(keys);
	return h;
}

static int compare_u32(const void* a, const void* b) {
	uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

/* Entities bound to `w` only (NULL: every entity), in id order. */
static uint64_t entity_hash(struct server_local* s, struct server_world* w,
							size_t* out_count) {
	size_t count = dict_entity_size(s->entities);
	uint32_t* ids = malloc((count ? count : 1) * sizeof(*ids));
	assert(ids);
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && (!w || entity_bound_world(e) == w))
			ids[n++] = e->id;
	}
	qsort(ids, n, sizeof(*ids), compare_u32);
	uint64_t h = FNV_BASIS;
	for(size_t i = 0; i < n; i++) {
		struct entity** pp = dict_entity_get(s->entities, ids[i]);
		assert(pp && *pp);
		struct entity* e = *pp;
		h = hash_bytes(h, &e->id, sizeof(e->id));
		h = hash_bytes(h, &e->type, sizeof(e->type));
		h = hash_bytes(h, e->pos, sizeof(vec3));
		h = hash_bytes(h, e->vel, sizeof(vec3));
		h = hash_bytes(h, &e->health, sizeof(e->health));
		h = hash_bytes(h, &e->delay_destroy, sizeof(e->delay_destroy));
	}
	free(ids);
	if(out_count)
		*out_count = n;
	return h;
}

struct fold {
	uint64_t entities, chunks;
	size_t entity_count;
	uint32_t rand_seed;
	int skylight;
	bool raining, thundering;
	int32_t rain_time, thunder_time;
	float rain_strength, thunder_strength;
	uint32_t pos_lcg;
	size_t bolts;
	int spawner_rows;
	unsigned long sends;
};

static struct fold fold_of(struct server_local* s, struct server_world* w,
						   int spawner_rows) {
	struct fold f = {0};
	f.entities = entity_hash(s, NULL, &f.entity_count);
	f.chunks = chunk_hash(w);
	f.rand_seed = s->rand_src.seed;
	f.skylight = server_local_skylight_subtracted(s);
	f.raining = s->weather.raining;
	f.thundering = s->weather.thundering;
	f.rain_time = s->weather.rain_time;
	f.thunder_time = s->weather.thunder_time;
	f.rain_strength = s->weather.rain_strength;
	f.thunder_strength = s->weather.thunder_strength;
	f.pos_lcg = s->weather.pos_lcg;
	f.bolts = s->weather.active_bolts_count;
	f.spawner_rows = spawner_rows;
	/* World publications only. The ENTITY_* + cosmetic family is excluded on
	 * purpose: the idle-voice roll (EntityLiving.onEntityUpdate:105-111,
	 * entity_monster.c) draws from the PROCESS-GLOBAL sound RNG
	 * (server_local_sound_rng: cosmetic, never world-deterministic), so the
	 * second fixture of this process sees a different sound stream and may
	 * emit one more/less CRPC_SOUND -- found 2026-09-26 on the knockback
	 * branch (A1C_TRACE=1 prints every publication per side/tick). */
	f.sends = world_sends;
	return f;
}

static void assert_same_fold(const struct fold* a, const struct fold* b,
							 const char* what) {
	bool same = a->entities == b->entities && a->chunks == b->chunks
		&& a->entity_count == b->entity_count && a->rand_seed == b->rand_seed
		&& a->skylight == b->skylight && a->raining == b->raining
		&& a->thundering == b->thundering && a->rain_time == b->rain_time
		&& a->thunder_time == b->thunder_time
		&& a->rain_strength == b->rain_strength
		&& a->thunder_strength == b->thunder_strength
		&& a->pos_lcg == b->pos_lcg && a->bolts == b->bolts
		&& a->spawner_rows == b->spawner_rows && a->sends == b->sends;
	if(!same) {
		fprintf(stderr,
				"DIVERGENCE (%s): entities %016" PRIx64 "/%016" PRIx64
				" n %zu/%zu chunks %016" PRIx64 "/%016" PRIx64
				" seed %08x/%08x sky %d/%d rain %d/%d thunder %d/%d "
				"rt %d/%d tt %d/%d lcg %08x/%08x bolts %zu/%zu rows %d/%d "
				"sends %lu/%lu\n",
				what, a->entities, b->entities, a->entity_count,
				b->entity_count, a->chunks, b->chunks, a->rand_seed,
				b->rand_seed, a->skylight, b->skylight, a->raining,
				b->raining, a->thundering, b->thundering, a->rain_time,
				b->rain_time, a->thunder_time, b->thunder_time, a->pos_lcg,
				b->pos_lcg, a->bolts, b->bolts, a->spawner_rows,
				b->spawner_rows, a->sends, b->sends);
		abort();
	}
}

/* One deterministic scene: a 5x5 superflat square around the origin, a mob
 * spawner block with its tile row, a stone pillar (a dark cell for spawns),
 * a forced thunderstorm, four monsters and a pig. Identical on both sides. */
static void scene(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	populate_square(s, w);
	s->player.finished_loading = true;
	s->player.x = 0.5;
	s->player.z = 0.5;
	s->player.y = GROUND_Y + 1 + PLAYER_EYE_HEIGHT;
	s->player.spawn_x = 200; /* keep the 24-block world-spawn exclusion away */
	s->player.spawn_y = GROUND_Y + 1;
	s->player.spawn_z = 200;
	/* Weather: a thunderstorm in flight so the tick, the effects ring and the
	 * hostile light override all have work to do. */
	s->weather.raining = true;
	s->weather.thundering = true;
	s->weather.rain_time = 5000;
	s->weather.thunder_time = 5000;
	s->weather.rain_strength = 1.0F;
	s->weather.thunder_strength = 1.0F;
	s->weather.pos_lcg = 0x2b7e1516u;
	s->weather.sound_counter = 0;
	s->world_time = 18000; /* midnight */
	server_local_skylight_recompute(s);
	/* A dungeon spawner four blocks from the player with its tile row. */
	assert(server_world_set_block(w, 4, GROUND_Y + 1, 0,
		(struct block_data) {.type = BLOCK_MOB_SPAWNER}));
	assert(spawner_te_upsert_w(w, 4, GROUND_Y + 1, 0, "Zombie"));
	/* A roofed dark cell so the hostile pass can land. */
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 2; dz++)
			assert(server_world_set_block(w, 30 + dx, GROUND_Y + 4, 20 + dz,
				(struct block_data) {.type = BLOCK_STONE}));
	server_local_test_seed_math_random(4242);
	s->rand_src.seed = 0x00C0FFEEu;
	s->rand_src.has_next_gaussian = false;
	s->rand_src.next_gaussian = 0.0F;
	assert(server_local_spawn_monster_at_w(w, (vec3) {6.5F, GROUND_Y + 1, 6.5F}, MONSTER_ZOMBIE));
	assert(server_local_spawn_monster_at_w(w, (vec3) {-6.5F, GROUND_Y + 1, 6.5F}, MONSTER_SKELETON));
	assert(server_local_spawn_monster_at_w(w, (vec3) {6.5F, GROUND_Y + 1, -6.5F}, MONSTER_SPIDER));
	assert(server_local_spawn_monster_at_w(w, (vec3) {-6.5F, GROUND_Y + 1, -6.5F}, MONSTER_CREEPER));
	assert(server_local_spawn_monster_at_w(w, (vec3) {0.5F, GROUND_Y + 1, 9.5F}, MONSTER_PIG));
	clin_drain_discard();
}

#define TICKS 40

static struct fold drive_legacy(struct server_local* s) {
	int rows = 0;
	dbg_side = 1;
	for(unsigned t = 0; t < TICKS; t++) {
		dbg_tick = (int)t;
		server_weather_tick(s);
		s->world_time++;
		(void)server_local_test_tick_entities(s);
		server_local_test_spawn_mobs(s);
		server_local_test_spawn_animals(s);
		server_local_test_spawn_squid(s);
		rows = server_local_test_tick_spawners(s);
		server_local_skylight_recompute(s);
		server_weather_effects_tick(s, 0, 0);
		server_local_test_tick_item_updates(s);
		(void)server_local_hostile_spawn_light(s, 30, GROUND_Y + 1, 20);
		(void)world_can_block_be_rained_on(s, 1, GROUND_Y + 1, 1);
		(void)server_local_test_near_unload_send_or_defer(s, 7, 7);
	}
	return fold_of(s, &s->worlds[0], rows);
}

static struct fold drive_w(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	int rows = 0;
	dbg_side = 2;
	for(unsigned t = 0; t < TICKS; t++) {
		dbg_tick = (int)t;
		server_weather_tick_w(s, w);
		s->world_time++;
		(void)server_local_test_tick_entities_w(s, w);
		server_local_test_spawn_mobs_w(s, w);
		server_local_test_spawn_animals_w(s, w);
		server_local_test_spawn_squid_w(s, w);
		rows = server_local_test_tick_spawners_w(s, w);
		server_local_skylight_recompute_w(s, w);
		server_weather_effects_tick_w(s, w, 0, 0);
		server_local_test_tick_item_updates_w(s, w);
		(void)server_local_hostile_spawn_light_w(s, w, 30, GROUND_Y + 1, 20);
		(void)world_can_block_be_rained_on_w(s, w, 1, GROUND_Y + 1, 1);
		(void)server_local_test_near_unload_send_w(w, 7, 7);
	}
	return fold_of(s, w, rows);
}

static void part1_adapter_equivalence(void) {
	struct fold legacy, explicit_w;
	{
		struct server_local* s = split_fixture();
		clin_test_set_rpc_observer(observe, NULL);
		sends = world_sends = entity_sends = 0;
		scene(s);
		sends = world_sends = entity_sends = 0;
		legacy = drive_legacy(s);
		clin_test_set_rpc_observer(NULL, NULL);
		clin_drain_discard();
		split_destroy(s);
	}
	{
		struct server_local* s = split_fixture();
		clin_test_set_rpc_observer(observe, NULL);
		sends = world_sends = entity_sends = 0;
		scene(s);
		sends = world_sends = entity_sends = 0;
		explicit_w = drive_w(s);
		clin_test_set_rpc_observer(NULL, NULL);
		clin_drain_discard();
		split_destroy(s);
	}
	assert_same_fold(&legacy, &explicit_w, "legacy vs _w(slot 0)");
	/* The scene must have exercised something: entities ticked, draws made. */
	assert(legacy.entity_count >= 5);
	assert(legacy.rand_seed != 0x00C0FFEEu);
	printf("PASS adapter equivalence: %u ticks, entities=%016" PRIx64
		   " chunks=%016" PRIx64 " seed=%08x sends=%lu rows=%d\n",
		   (unsigned)TICKS, legacy.entities, legacy.chunks, legacy.rand_seed,
		   legacy.sends, legacy.spawner_rows);
}

static struct server_world* make_slot1(struct server_local* s,
									   enum world_dim dim) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, dim, 12345, dim == WORLD_DIM_OVERWORLD);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

static void part2_same_coordinate_isolation(void) {
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_slot1(s, WORLD_DIM_OVERWORLD);
	populate_square(s, a);
	populate_square(s, b);
	s->player.finished_loading = true;
	s->player.x = 0.5;
	s->player.z = 0.5;
	s->player.y = GROUND_Y + 1 + PLAYER_EYE_HEIGHT;
	s->world_time = 18000;
	s->weather.raining = true;
	s->weather.thundering = true;
	s->weather.rain_strength = 1.0F;
	s->weather.thunder_strength = 1.0F;
	server_local_skylight_recompute(s);
	s->rand_src.seed = 0x5EEDu;
	s->rand_src.has_next_gaussian = false;

	/* Eligibility: both actors are committed to slot 0; nobody is in slot 1. */
	assert(server_local_actor_in_world(s, &s->player, a));
	assert(!server_local_actor_in_world(s, &s->player, b));
	struct server_player* elig[SERVER_LOCAL_MAX_PRESENT_ACTORS];
	assert(server_local_eligible_actors_w(s, a, elig, 2) == 1 && elig[0] == &s->player);
	assert(server_local_eligible_actors_w(s, b, elig, 2) == 0);

	/* One monster in each world at the SAME position. */
	struct entity* ea = server_local_spawn_monster_at_w(a, (vec3) {5.5F, GROUND_Y + 1, 5.5F}, MONSTER_ZOMBIE);
	struct entity* eb = server_local_spawn_monster_at_w(b, (vec3) {5.5F, GROUND_Y + 1, 5.5F}, MONSTER_ZOMBIE);
	assert(ea && eb && ea->id != eb->id);
	assert(server_local_entity_world(s, ea) == a);
	assert(server_local_entity_world(s, eb) == b);
	clin_drain_discard();
	clin_test_set_rpc_observer(observe, NULL);
	sends = world_sends = entity_sends = 0;

	/* ---- operations on slot 1 leave slot 0 untouched ---- */
	size_t na = 0, nb = 0;
	uint64_t a_chunks = chunk_hash(a);
	uint64_t a_entities = entity_hash(s, a, &na);
	uint64_t b_entities = entity_hash(s, b, &nb);
	assert(na == 1 && nb == 1);
	int sky_cached = server_local_skylight_subtracted(s);

	/* No eligible actor in slot 1: the spawn passes and the spawner pass
	 * draw nothing (Beta: no player -> no eligible chunks -> no attempt). */
	uint32_t seed = s->rand_src.seed;
	server_local_test_spawn_mobs_w(s, b);
	server_local_test_spawn_animals_w(s, b);
	server_local_test_spawn_squid_w(s, b);
	assert(server_local_test_tick_spawners_w(s, b) == 0);
	server_local_test_tick_item_updates_w(s, b);
	assert(s->rand_src.seed == seed);
	assert(entity_hash(s, b, &nb) == b_entities && nb == 1);
	assert(entity_hash(s, a, &na) == a_entities && na == 1);
	assert(sends == 0);

	/* The entity pass on slot 1 ticks slot 1's monster only. A1c PINNED the
	 * one cross-world leak it found and could not close from its files: the
	 * entity SECTIONS were keyed by chunk section only (world-blind), so slot
	 * 1's zombie's EntityLiving collide-with-nearby push also shoved slot 0's
	 * zombie standing at the SAME coordinates. A2-E keyed the section index
	 * per world (s->entity_sections[slot], server_local_entity_query_local_w)
	 * -- the pins are FLIPPED to isolation asserts (test_a2e_entity_generation
	 * carries the per-world query/push evidence in detail). */
	assert(server_local_test_tick_entities_w(s, b));
	assert(entity_hash(s, b, &nb) != b_entities && nb == 1); /* it ticked */
	bool push_leak_1_to_0 = entity_hash(s, a, &na) != a_entities;
	assert(na == 1); /* never a new or lost slot-0 entity */
	assert(!push_leak_1_to_0
		   && "slot-0 isolation: a slot-1 tick never touches slot 0's entity");
	assert(world_sends == 0); /* a slot-1 tick publishes no world traffic */
	a_entities = entity_hash(s, a, &na);
	b_entities = entity_hash(s, b, &nb);

	/* Skylight for slot 1 is computed for slot 1 (same dimension -> same
	 * value here) without disturbing slot 0's cache. */
	assert(server_local_skylight_subtracted_w(s, b) == sky_cached);
	server_local_skylight_recompute_w(s, b);
	assert(server_local_skylight_subtracted(s) == sky_cached);

	/* Weather effects, the bolt sweep and the near-unload send on slot 1:
	 * slot 0's bytes and the client stream stay untouched (RNG: one shared
	 * stream until A2 -- the contract doc records this; only bytes and
	 * publications are asserted). */
	server_weather_effects_tick_w(s, b, 0, 0);
	server_weather_tick_lightning_bolts_w(s, b);
	assert(server_local_test_near_unload_send_w(b, 1, 1));
	assert(chunk_hash(a) == a_chunks);
	assert(entity_hash(s, a, &na) == a_entities && na == 1);
	assert(world_sends == 0); /* slot 1 has no client route */
	unsigned long slot1_entity_traffic = entity_sends;

	/* ---- operations on slot 0 leave slot 1's entity untouched (the same
	 * isolation in this direction) ---- */
	assert(server_local_test_tick_entities_w(s, a));
	assert(entity_hash(s, a, &na) != a_entities && na >= 1); /* it ticked */
	bool push_leak_0_to_1 = entity_hash(s, b, &nb) != b_entities;
	assert(nb == 1);
	assert(!push_leak_0_to_1
		   && "slot-1 isolation: a slot-0 tick never touches slot 1's entity");
	assert(server_local_test_near_unload_send_w(a, 1, 1));
	assert(world_sends >= 1); /* slot 0 publishes */
	printf("NOTE B1 producer identity: %lu ENTITY_*/cosmetic publications "
		   "left slot-1 entity ticks without a world stamp (entity code, not "
		   "the _w emitters)\n",
		   slot1_entity_traffic);
	printf("A2-E: same-coordinate collide push is world-isolated "
		   "(1->0 %d, 0->1 %d) -- entity sections are per world\n",
		   (int)push_leak_1_to_0, (int)push_leak_0_to_1);

	/* A world that is not live (a stray instance) is refused, never slot 0. */
	struct server_world stray = {0};
	assert(!server_local_world_live(&stray));
	assert(server_local_eligible_actors_w(s, &stray, elig, 2) == 0);
	assert(server_local_entity_world(s, ea) == a);

	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	server_world_discard(b);
	/* After discard slot 1's entity is orphaned (A2-E owns the retirement);
	 * the accessor must not resolve it to a live world. */
	assert(server_local_entity_world(s, eb) == NULL);
	split_destroy(s);
	puts("PASS same-coordinate isolation: slot-1 operations never touch slot 0 "
		 "(chunk bytes, entity table, world publications); no eligible actor -> "
		 "no draws; the entity-section push leak is pinned for A2-E");
}

int main(void) {
	assert(clin_init());
	part1_adapter_equivalence();
	part2_same_coordinate_isolation();
	return 0;
}
#endif
