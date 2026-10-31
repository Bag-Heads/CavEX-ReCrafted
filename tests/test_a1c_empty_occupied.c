/* Copyright (c) 2026 CaveWii_v2 contributors
 * A1c (split-dimensions-plan-0924): EMPTY -> OCCUPIED behaviour of a second
 * live world is isolated from the routed world.
 *
 * PLAN-v3 ruling #10 (approved empty-dimension deviation): an empty world
 * retains live clocks, tick-queue/tile metadata and ZERO resident columns; it
 * has no permanently loaded spawn area. Beta's own per-World services do
 * nothing there: SpawnerAnimals.performSpawning builds eligibleChunksForSpawning
 * from the players IN that world (none -> no attempt, no draw);
 * TileEntityMobSpawner needs a player within 16 blocks; updateWeather is
 * inert without a sky (WorldProviderHell.hasNoSky); updateEntities walks
 * that world's loadedEntityList (empty). So every world operation run on an
 * EMPTY slot-1 world must be a strict no-op: no RNG draw on the shared
 * stream, no publication, no entity, no tracker row, no weather change, and
 * the routed world's skylight cache untouched.
 *
 * OCCUPIED: once slot 1 holds a resident column and an entity bound to it,
 * the entity pass on slot 1 ticks that entity and nothing else; the entity
 * pass on slot 0 never reaches it; the spawn passes on slot 1 still draw
 * nothing while no actor is committed there (the eligible-actor contract);
 * the skylight of a Nether world is 11 while the Overworld cache keeps its
 * own value. Discarding slot 1 leaves slot 0's operations intact.
 *
 * RED by construction until SERVER_LOCAL_A1C_WORLD_OPS exists. */
#include "support/split_residency_fixture.h"

#include "entity/monsters_object.h"
#include "network/server_weather.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

#ifndef SERVER_LOCAL_A1C_WORLD_OPS
int main(void) {
	puts("RED: SERVER_LOCAL_A1C_WORLD_OPS absent -- empty/occupied world "
		 "operations are not callable per struct server_world* yet");
	return 1;
}
#else

#define GROUND_Y 3

static void* entity_bound_world(const struct entity* e) {
	return *(void* const*)((const char*)e + offsetof(struct entity, world));
}

/* World-bound publications vs the ENTITY_* + cosmetic family the entity code
 * emits itself (B1's stamping rows; not routed by the A1b/A1c emitters). */
static unsigned long sends, world_sends, entity_sends;
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

static struct server_world* make_slot1(struct server_local* s,
									   enum world_dim dim) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, dim, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

static size_t entities_in(struct server_local* s, struct server_world* w) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && entity_bound_world(e) == w)
			n++;
	}
	return n;
}

static int spawner_rows(struct server_local* s) {
	int n = 0;
	for(int i = 0; i < MAX_ACTIVE_SPAWNERS; i++)
		if(s->active_spawners[i].used)
			n++;
	return n;
}

struct weather_snap {
	bool raining, thundering;
	int32_t rain_time, thunder_time, flash_ticks, sound_counter;
	float rain_strength, thunder_strength;
	uint32_t pos_lcg;
	size_t bolts;
};

static struct weather_snap weather_snap(const struct server_local* s) {
	return (struct weather_snap) {
		.raining = s->weather.raining,
		.thundering = s->weather.thundering,
		.rain_time = s->weather.rain_time,
		.thunder_time = s->weather.thunder_time,
		.flash_ticks = s->weather.flash_ticks,
		.sound_counter = s->weather.sound_counter,
		.rain_strength = s->weather.rain_strength,
		.thunder_strength = s->weather.thunder_strength,
		.pos_lcg = s->weather.pos_lcg,
		.bolts = s->weather.active_bolts_count,
	};
}

static bool weather_same(struct weather_snap a, struct weather_snap b) {
	return a.raining == b.raining && a.thundering == b.thundering
		&& a.rain_time == b.rain_time && a.thunder_time == b.thunder_time
		&& a.flash_ticks == b.flash_ticks && a.sound_counter == b.sound_counter
		&& a.rain_strength == b.rain_strength
		&& a.thunder_strength == b.thunder_strength && a.pos_lcg == b.pos_lcg
		&& a.bolts == b.bolts;
}

/* Every world operation, once, on `w`. */
static void run_all_ops(struct server_local* s, struct server_world* w) {
	server_weather_tick_w(s, w);
	(void)server_local_test_tick_entities_w(s, w);
	server_local_test_spawn_mobs_w(s, w);
	server_local_test_spawn_animals_w(s, w);
	server_local_test_spawn_squid_w(s, w);
	(void)server_local_test_tick_spawners_w(s, w);
	server_local_skylight_recompute_w(s, w);
	server_weather_effects_tick_w(s, w, 0, 0);
	server_weather_tick_lightning_bolts_w(s, w);
	server_local_test_tick_item_updates_w(s, w);
	(void)server_local_test_near_unload_send_w(w, 0, 0);
}

int main(void) {
	assert(clin_init());
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	populate(s, a, 0, 0);
	s->player.finished_loading = true;
	s->player.x = 0.5;
	s->player.z = 0.5;
	s->player.y = GROUND_Y + 1 + PLAYER_EYE_HEIGHT;
	s->world_time = 6000; /* noon: Overworld subtract 0, Nether 11 */
	s->weather.raining = true;
	s->weather.thundering = true;
	s->weather.rain_time = 4000;
	s->weather.thunder_time = 4000;
	s->weather.rain_strength = 1.0F;
	s->weather.thunder_strength = 1.0F;
	s->weather.sound_counter = 0;
	server_local_skylight_recompute(s);
	int sky_a = server_local_skylight_subtracted(s);
	assert(sky_a != 11); /* noon, weather-darkened: still not the Nether 11 */

	struct server_world* b = make_slot1(s, WORLD_DIM_NETHER);
	assert(server_local_world_live(b));
	assert(dict_server_chunks_size(b->chunks) == 0); /* EMPTY: no columns */

	clin_drain_discard();
	clin_test_set_rpc_observer(observe, NULL);
	sends = world_sends = entity_sends = 0;
	s->rand_src.seed = 0x0A1Cu;
	s->rand_src.has_next_gaussian = false;

	/* ---- EMPTY slot 1: every operation is a strict no-op ---- */
	uint32_t seed = s->rand_src.seed;
	size_t entities = dict_entity_size(s->entities);
	int rows = spawner_rows(s);
	struct weather_snap wx = weather_snap(s);
	for(unsigned t = 0; t < 25; t++)
		run_all_ops(s, b);
	assert(s->rand_src.seed == seed);
	assert(dict_entity_size(s->entities) == entities);
	assert(spawner_rows(s) == rows);
	assert(weather_same(wx, weather_snap(s)));
	assert(sends == 0);
	assert(server_local_skylight_subtracted(s) == sky_a); /* slot-0 cache kept */
	assert(server_local_skylight_subtracted_w(s, b) == 11); /* Nether */
	assert(dict_server_chunks_size(b->chunks) == 0); /* still no forced load */
	puts("PASS empty world: 25 rounds of every world operation on an empty "
		 "Nether drew nothing, published nothing, loaded nothing");

	/* ---- OCCUPIED slot 1 ---- */
	populate(s, b, 0, 0);
	assert(dict_server_chunks_size(b->chunks) >= 1);
	/* A body-less Nether column: the natural passes still draw nothing
	 * because no actor is committed to that world. */
	seed = s->rand_src.seed;
	server_local_test_spawn_mobs_w(s, b);
	server_local_test_spawn_animals_w(s, b);
	server_local_test_spawn_squid_w(s, b);
	assert(server_local_test_tick_spawners_w(s, b) == 0);
	assert(s->rand_src.seed == seed);
	assert(dict_entity_size(s->entities) == entities);

	/* An entity bound to slot 1 (a pigman at the column top). */
	uint8_t top = 0;
	assert(server_world_get_height(b, 8, 8, &top));
	struct entity* eb = server_local_spawn_monster_at_w(
		b, (vec3) {8.5F, (float)top + 1.0F, 8.5F}, MONSTER_PIG_ZOMBIE);
	assert(eb && entity_bound_world(eb) == b);
	assert(server_local_entity_world(s, eb) == b);
	assert(sends == 0); /* no client route for slot 1: no CRPC_SPAWN_MONSTER */
	assert(entities_in(s, b) == 1 && entities_in(s, a) == 0);
	vec3 before;
	glm_vec3_copy(eb->pos, before);
	int hp = eb->health;

	/* Slot 0's entity pass never reaches slot 1's entity... */
	assert(server_local_test_tick_entities_w(s, a));
	assert(eb->pos[0] == before[0] && eb->pos[1] == before[1]
		   && eb->pos[2] == before[2] && eb->health == hp);
	/* ...and slot 1's pass does tick it (the area around it is resident only
	 * once the +/-32 box exists; populate the ring first). */
	for(int z = -2; z <= 2; z++)
		for(int x = -2; x <= 2; x++)
			if(x || z)
				populate(s, b, x, z);
	bool moved = false;
	for(unsigned t = 0; t < 20 && !moved; t++) {
		assert(server_local_test_tick_entities_w(s, b));
		moved = eb->pos[0] != before[0] || eb->pos[1] != before[1]
			|| eb->pos[2] != before[2] || eb->health != hp;
	}
	assert(moved);
	assert(entities_in(s, b) == 1 && entities_in(s, a) == 0);
	assert(world_sends == 0); /* slot-1 ticks publish no world traffic */
	printf("NOTE B1 producer identity: %lu ENTITY_*/cosmetic publications left "
		   "slot-1 entity ticks without a world stamp\n",
		   entity_sends);
	puts("PASS occupied world: slot-1 entity ticks only under slot-1's pass; "
		 "no actor there -> the natural passes still draw nothing");

	/* Discarding slot 1 leaves slot 0's operations intact. */
	server_world_discard(b);
	assert(!server_local_world_live(b));
	assert(server_local_entity_world(s, eb) == NULL);
	seed = s->rand_src.seed;
	server_local_test_spawn_mobs_w(s, a); /* the host IS eligible here */
	assert(s->rand_src.seed != seed);
	assert(server_local_skylight_subtracted(s) == sky_a);

	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	split_destroy(s);
	puts("PASS discard: slot-0 operations unaffected by slot 1's lifecycle");
	return 0;
}
#endif
