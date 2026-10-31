/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924 lane C, C2 row #2: lightning strike effects on
 * both actors, only in the strike's world; Nether suppression.
 *
 * Beta 1.7.3 (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityLightningBolt.onUpdate, EntityLightningBolt.java:62-69: while
 *     lightningState >= 0, every entity of the bolt's OWN world inside
 *     (posX+-3, posY-3 .. posY+6+3, posZ+-3) gets onStruckByLightning --
 *     players included, whoever they are.
 *   - Entity.onStruckByLightning, Entity.java:1044-1051: dealFireDamage(5);
 *     ++fire; if(fire == 0) fire = 300.
 *   - EntityLightningBolt.<init>, :10-34: the fire placement (difficulty >= 2)
 *     and its three nextInt(3) offsets x4, then nextInt(3)+1 living time.
 *   - World.updateWeather / updateBlocksAndPlayCaveSounds are gated on
 *     `!worldProvider.hasNoSky` (World.java:1787): no weather, no bolt, no
 *     weather RNG draw in the Nether.
 *
 * One executable, one case per process (argv[1]):
 *   guest_struck   host far, guest under the bolt: the guest takes 5 fire
 *                  damage and fire+1; the host untouched (RED on base: the
 *                  entity pass scanned the host only)
 *   host_struck    mirror: host under the bolt, guest far
 *   both_struck    both under the bolt: both hit once per active tick
 *   other_world    a bolt in another world (slot 1) at the bodies' cell
 *                  strikes neither
 *   nether_quiet   a Nether world with a thunderstorm set: the weather and
 *                  weather-effects ticks spawn no bolt and draw no RNG
 *   bbox           selection by bounding box (strict), not the point
 *   victims        dropped item destroyed; minecart and boat broken
 *   guest_ring     the weather ring is the union of one ring per player
 *                  (World.java:1869-1877): a far guest's chunk is rolled */
#include "support/owner_swap_harness.h"
#include "entity/combat.h"
#include "network/server_weather.h"
#include "entity/entity_boat.h"
#include "entity/entity_minecart.h"

#define TEST_NAME "test_owner_swap_lightning"
#define BX 4
#define BY 4
#define BZ 4

static void place(struct server_player* p, double x, double z) {
	p->x = x;
	p->z = z;
	p->y = BY + PLAYER_EYE_HEIGHT;
	p->fire = -PLAYER_FIRE_RESISTANCE;
	p->health = MAX_PLAYER_HEALTH;
	p->hurt_resist_time = 0;
}
/* One bolt in `w`, then its first onUpdate (the strike tick). */
static void strike(struct server_local* s, struct server_world* w) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	world_lightning_bolt_spawn_w(s, w, BX, BY, BZ);
	server_weather_tick_lightning_bolts_w(s, w);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}
static bool hit(const struct server_player* p) {
	return p->health == MAX_PLAYER_HEALTH - LIGHTNING_STRIKE_DAMAGE
		&& p->fire == -PLAYER_FIRE_RESISTANCE + 1;
}
static bool untouched(const struct server_player* p) {
	return p->health == MAX_PLAYER_HEALTH
		&& p->fire == -PLAYER_FIRE_RESISTANCE;
}

static void case_struck(bool host_near, bool guest_near) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	place(h, host_near ? BX + 0.5 : BX + 40.5, BZ + 0.5);
	place(g, guest_near ? BX + 1.5 : BX - 40.5, BZ + 0.5);
	strike(s, &s->worlds[0]);
	if(host_near)
		check(hit(h), "host under the bolt: 5 fire damage, fire + 1 "
					  "(Entity.onStruckByLightning)");
	else
		check(untouched(h), "host outside the bolt box: untouched");
	if(guest_near)
		check(hit(g), "guest under the bolt: 5 fire damage, fire + 1 -- the "
					  "bolt's AABB scan has no host-only leg");
	else
		check(untouched(g), "guest outside the bolt box: untouched");
	teardown(s);
}

static struct server_world* make_world(struct server_local* s,
									   enum world_dim dim) {
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, dim, 12345, dim == WORLD_DIM_OVERWORLD);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 200 && !c; n++) {
		server_world_begin_update(b);
		(void)server_world_load_chunk(b, 0, 0, &c);
	}
	assert(c);
	return b;
}

static void case_other_world(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* b = make_world(s, WORLD_DIM_OVERWORLD);
	place(&s->player, BX + 0.5, BZ + 0.5);
	place(s->guest_actor, BX + 1.5, BZ + 0.5);
	strike(s, b);
	check(untouched(&s->player) && untouched(s->guest_actor),
		"a bolt in world slot 1 strikes neither body (both are committed to "
		"slot 0; the bolt scans only its own world)");
	server_world_discard(b);
	teardown(s);
}

static void case_nether_quiet(void) {
	struct server_local* s = admitted_fixture();
	struct server_world* b = make_world(s, WORLD_DIM_NETHER);
	s->weather.raining = s->weather.thundering = true;
	s->weather.rain_strength = s->weather.thunder_strength = 1.0f;
	s->weather.prev_rain_strength = s->weather.prev_thunder_strength = 1.0f;
	struct random_gen rng0 = s->rand_src;
	size_t bolts0 = s->weather.active_bolts_count;
	for(int t = 0; t < 2000; t++) {
		server_weather_tick_w(s, b);
		server_weather_effects_tick_w(s, b, 0, 0);
	}
	check(s->weather.active_bolts_count == bolts0,
		"Nether: no bolt spawns in 2000 thunderstorm ticks (hasNoSky)");
	check(s->rand_src.seed == rng0.seed,
		"Nether: the weather ticks draw no RNG (World.java:1787 gate)");
	server_world_discard(b);
	teardown(s);
}


/* ---- follow-up: bounding-box selection, non-mob victims, per-player rings */
static struct entity* add_entity(struct server_local* s, int kind, float x,
								 float z) {
	uint32_t id = server_local_gen_id(s);
	struct entity** slot = dict_entity_safe_get(s->entities, id);
	*slot = calloc(1, sizeof(struct entity));
	assert(*slot);
	struct entity* e = *slot;
	if(kind == ENTITY_ITEM)
		entity_item(id, e, true, &s->worlds[0],
			(struct item_data) {.id = BLOCK_DIRT, .count = 1});
	else if(kind == ENTITY_MINECART)
		entity_minecart(id, e, true, &s->worlds[0]);
	else
		entity_boat(id, e, true, &s->worlds[0]);
	e->teleport(e, (vec3) {x, (float)BY, z});
	return e;
}

static void case_bbox(void) {
	struct server_local* s = admitted_fixture();
	/* Point 0.2 beyond the bolt box's +X face, but the 0.6-wide body box
	 * reaches 0.1 inside it: Beta's boundingBox.intersectsWith strikes. */
	place(&s->player, BX + 3.2, BZ + 0.5);
	place(s->guest_actor, BX + 3.3, BZ + 0.5); /* box edge == face: strict miss */
	strike(s, &s->worlds[0]);
	check(hit(&s->player),
		"a body whose BOX overlaps the bolt box is struck although its point "
		"is outside (Chunk.getEntitiesWithinAABBForEntity :500)");
	check(untouched(s->guest_actor),
		"a body box touching the face exactly is not (strict intersectsWith)");
	teardown(s);
}

static void case_victims(void) {
	struct server_local* s = admitted_fixture();
	place(&s->player, BX + 40.5, BZ + 0.5);
	place(s->guest_actor, BX - 40.5, BZ + 0.5);
	struct entity* item = add_entity(s, ENTITY_ITEM, BX + 0.5F, BZ + 0.5F);
	struct entity* cart = add_entity(s, ENTITY_MINECART, BX + 1.5F, BZ + 0.5F);
	struct entity* boat = add_entity(s, ENTITY_BOAT, BX - 0.5F, BZ + 1.5F);
	strike(s, &s->worlds[0]);
	check(item->health <= 0,
		"a dropped item under the bolt is destroyed (EntityItem.dealFireDamage "
		":83-85 -> attackEntityFrom :87-93, health 5 - 5)");
	check(cart->data.minecart.blast_break_pending,
		"a minecart under the bolt breaks (EntityMinecart.attackEntityFrom "
		":73-79, damage 5*10 > 40)");
	check(boat->data.boat.blast_break_pending,
		"a boat under the bolt breaks (EntityBoat.attackEntityFrom :63-69)");
	teardown(s);
}

/* Beta's weather rolls cover a ring around EACH player (World.java:1869-1877).
 * With rain on, every processed chunk draws from the world RNG, so a far
 * guest's own resident chunk changes the stream exactly when it is rolled. */
static void case_guest_ring(void) {
	struct server_local* s = admitted_fixture();
	struct server_chunk* far = NULL;
	for(unsigned n = 0; n < 200 && !far; n++) {
		server_world_begin_update(&s->worlds[0]);
		(void)server_world_load_chunk(&s->worlds[0], 30, 0, &far);
	}
	assert(far);
	place(&s->player, 0.5, 0.5);
	place(s->guest_actor, 30 * 16 + 8.5, 8.5); /* chunk (30,0): 30 from host */
	s->weather.raining = s->weather.thundering = true;
	s->weather.rain_strength = s->weather.thunder_strength = 1.0f;
	s->weather.prev_rain_strength = s->weather.prev_thunder_strength = 1.0f;
	s->weather.sound_counter = 1000; /* keep the cave-sound branch out */
	s->rand_src.seed = 0x5EED1234u;
	/* Every run starts from the SAME weather + RNG state. */
	struct random_gen rng0 = s->rand_src;
	uint32_t lcg0 = s->weather.pos_lcg;
	int32_t sound0 = s->weather.sound_counter;
	size_t bolts0 = s->weather.active_bolts_count;
	struct server_player* guest = s->guest_actor;
#define RESET() do { s->rand_src = rng0; s->weather.pos_lcg = lcg0; \
	s->weather.sound_counter = sound0; s->weather.active_bolts_count = bolts0; \
	} while(0)

	RESET();
	s->guest_actor = NULL; /* host ring only */
	server_weather_effects_tick_w(s, &s->worlds[0], 0, 0);
	uint32_t seed_host = s->rand_src.seed;
	s->guest_actor = guest;
	RESET();
	server_weather_effects_tick_w(s, &s->worlds[0], 0, 0);
	check(s->rand_src.seed != seed_host,
		"the far guest's resident chunk is rolled too (its own radius-9 ring)");

	/* Same ring (guest beside the host): identical stream to host-only. */
	place(s->guest_actor, 1.5, 0.5);
	RESET();
	s->guest_actor = NULL;
	server_weather_effects_tick_w(s, &s->worlds[0], 0, 0);
	seed_host = s->rand_src.seed;
	s->guest_actor = guest;
	RESET();
	server_weather_effects_tick_w(s, &s->worlds[0], 0, 0);
	check(s->rand_src.seed == seed_host,
		"a guest inside the host's ring adds no chunk: same RNG stream");
#undef RESET
	teardown(s);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "";
	if(!strcmp(which, "guest_struck"))
		case_struck(false, true);
	else if(!strcmp(which, "host_struck"))
		case_struck(true, false);
	else if(!strcmp(which, "both_struck"))
		case_struck(true, true);
	else if(!strcmp(which, "other_world"))
		case_other_world();
	else if(!strcmp(which, "nether_quiet"))
		case_nether_quiet();
	else if(!strcmp(which, "bbox"))
		case_bbox();
	else if(!strcmp(which, "victims"))
		case_victims();
	else if(!strcmp(which, "guest_ring"))
		case_guest_ring();
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
