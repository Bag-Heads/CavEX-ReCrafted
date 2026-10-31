/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924 lane C, C2 row #19: the nearest ELIGIBLE actor
 * of the mob's own world governs monster despawn, owner-swapped.
 *
 * Beta 1.7.3 (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityLiving.func_27021_X (despawnEntity), EntityLiving.java:667-685:
 *     the closest player (getClosestPlayerToEntity, any
 *     range); when canDespawn() and a player exists, with d2 its squared distance,
 *     d2 > 16384.0D despawns; otherwise, when entityAge > 600 and
 *     rand.nextInt(800) == 0, d2 < 1024.0D resets entityAge to 0, else
 *     it despawns.
 *   - World.getClosestPlayer, World.java:2166-2180: scans THIS world's
 *     playerEntities; keeps the first strict minimum (a strict less-than), so
 *     on a tie the earlier player (host) governs.
 *   => any player of the mob's world within 128 keeps it; a player in
 *      another world never counts; no player in the world = no despawn and
 *      no roll draw.
 *
 * One executable, one case per process (argv[1]):
 *   guest_near   host 200 blocks away, guest 5 away: the mob STAYS (RED on
 *                base: host-only distance despawned it)
 *   host_near    mirror: host 5, guest 200: stays
 *   both_far     both > 128: despawns silently; owned aged roll still occurs
 *   tie_rng      aged mob, both bodies at the same spot vs host alone: the
 *                same decision and the same RNG stream (one nextInt(800))
 *   no_actor     the mob's world holds no eligible actor (slot 1): no
 *                despawn at all (RED on base: the host in slot 0 despawned it) */
#include "support/owner_swap_harness.h"
#include "entity/combat.h"
#include <inttypes.h>

#define TEST_NAME "test_owner_swap_monster_despawn"

static struct entity* zombie_at(struct server_local* s, float x, float z) {
	struct entity* e = server_local_spawn_monster_at(
		(vec3) {x, 4.0F, z}, MONSTER_ZOMBIE, s);
	assert(e);
	e->data.monster.entity_age = 0;
	return e;
}
static void place(struct server_player* p, double x, double z) {
	p->x = x;
	p->z = z;
	p->y = 4.0 + PLAYER_EYE_HEIGHT;
}
static bool despawned(const struct entity* e) {
	return e->delay_destroy == 0;
}
static bool rng_equal(struct random_gen a, struct random_gen b) {
	return a.seed == b.seed && a.has_next_gaussian == b.has_next_gaussian
		&& a.next_gaussian == b.next_gaussian;
}
static bool mob_rng_equal(struct java_random a, struct java_random b) {
	return a.seed == b.seed && a.have_next_gaussian == b.have_next_gaussian
		&& a.next_gaussian_cached == b.next_gaussian_cached;
}
static size_t item_count(struct server_local* s) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM)
			count++;
		dict_entity_next(it);
	}
	return count;
}
static void tick_mob(struct server_local* s, struct entity* e) {
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	(void)e->tick_server(e, s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
}

static void case_near(bool guest_near) {
	struct server_local* s = admitted_fixture();
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, guest_near ? 208.5 : 13.5, 8.5);
	place(s->guest_actor, guest_near ? 13.5 : 208.5, 8.5);
	tick_mob(s, e);
	check(!despawned(e), guest_near
		? "guest 5 blocks away keeps the mob although the host is 200 away "
		  "(getClosestPlayer over the world's players)"
		: "host 5 blocks away keeps the mob although the guest is 200 away");
	teardown(s);
}

static void case_both_far(void) {
	struct server_local* s = admitted_fixture();
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, 208.5, 8.5);
	place(s->guest_actor, 8.5, 208.5);
	tick_mob(s, e);
	check(despawned(e), "both bodies beyond 128: the mob despawns (> 16384)");
	/* EntityLiving:674-678 has sequential far-removal and aged-roll ifs.
	 * EntityWolf:62-64 permits untamed despawn, and its age has no EntityMob
	 * brightness +2: starting599/600 becomes exactly600/601. tick_mob calls
	 * the real callback without running the server reaper; inspect before teardown. */
	struct server_player* actors[2];
	assert(server_local_eligible_actors_w(s, &s->worlds[0], actors, 2) == 2);
	assert(s->player.health > 0 && s->guest_actor->health > 0);
	for(int age = 599; age <= 600; age++) {
		struct entity* wolf = server_local_spawn_monster_at(
			(vec3) {8.5F, 4.0F, 8.5F}, MONSTER_WOLF, s);
		assert(wolf && wolf->health > 0 && wolf_can_despawn(wolf->data.monster.wolf_flags));
		assert(!(wolf->data.monster.wolf_flags & WOLF_FLAG_TAMED));
		assert(wolf->data.monster.fire_time == 0 && wolf->data.monster.death_time == 0);
		wolf->data.monster.entity_age = age;
		wolf->gameplay_rng.have_next_gaussian = true;
		wolf->gameplay_rng.next_gaussian_cached = 0.375F;
		struct java_random before = wolf->gameplay_rng;
		struct java_random expected = before;
		/* LIVING-SOUND-RNG: EntityLiving.onEntityUpdate draws the living-sound
		 * roll on the wolf's own Random EVERY tick, before onLivingUpdate's
		 * despawnEntity [server EntityLiving.java:104-106:
		 * `if(this.rand.nextInt(1000) < this.livingSoundTime++)`]. A freshly
		 * spawned wolf has livingSoundTime 0, so the roll cannot fire (no
		 * getLivingSound nextInt(3), no pitch floats) -- pinned below. */
		assert(wolf->data.monster.living_sound_time == 0);
		(void)jrand_next_int_bound(&expected, 1000);
		if(age == 600)
			(void)jrand_next_int_bound(&expected, MOB_DESPAWN_ROLL_DEN);
		struct random_gen world_before = s->rand_src;
		size_t items_before = item_count(s);
		tick_mob(s, wolf);
		printf("far wolf age%d->%d: owned %012" PRIx64 "->%012" PRIx64
			" expected%012" PRIx64 ", cache%d/%g\n",
			age, wolf->data.monster.entity_age, before.seed, wolf->gameplay_rng.seed,
			expected.seed, wolf->gameplay_rng.have_next_gaussian,
			(double)wolf->gameplay_rng.next_gaussian_cached);
		check(wolf->data.monster.entity_age == age + 1, "far wolf ages once without reset");
		check(despawned(wolf), "far wolf is removed silently at either age");
		check(!wolf->data.monster.death_loot_dropped && item_count(s) == items_before,
			"far removal neither enters death loot nor creates items");
		check(wolf->data.monster.living_sound_time == 1,
			"the living-sound ramp ticked once without firing");
		check(mob_rng_equal(wolf->gameplay_rng, expected), age == 599
			? "age600 far removal: only the living-sound roll, zero despawn draws, preserving cache"
			: "age601 far removal: the living-sound roll then exactly one despawn count draw, preserving cache");
		check(rng_equal(s->rand_src, world_before),
			"this harmless far-return tick leaves the world seed/cache unchanged");
	}
	teardown(s);
}

static void case_tie_rng(void) {
	struct server_local* s = admitted_fixture();
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	e->data.monster.entity_age = 5000; /* aged: the 1/800 roll is drawn */
	place(&s->player, 48.5, 8.5);      /* 40 blocks: roll hit would despawn */
	place(s->guest_actor, 48.5, 8.5);
	struct entity snap = *e;
	struct random_gen rng0 = s->rand_src;
	struct server_player* guest = s->guest_actor;

	/* Host alone (the guest body removed from the scan for this tick). */
	s->guest_actor = NULL;
	tick_mob(s, e);
	struct random_gen rng_host = s->rand_src;
	struct java_random mob_rng_host = e->gameplay_rng;
	bool dead_host = despawned(e);
	int age_host = e->data.monster.entity_age;
	s->guest_actor = guest;

	/* Same state, both bodies at the same spot: the host wins the tie. */
	*e = snap;
	s->rand_src = rng0;
	tick_mob(s, e);
	check(s->rand_src.seed == rng_host.seed,
		"tie: the world RNG stream is identical to the host-only tick");
	check(mob_rng_equal(e->gameplay_rng, mob_rng_host),
		"tie: the mob RNG stream is identical to the host-only tick");
	check(despawned(e) == dead_host && e->data.monster.entity_age == age_host,
		"tie: the same despawn decision and age");
	teardown(s);
}

static void case_no_actor(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	/* Both bodies are committed to slot 0 and 200 blocks away; the mob is
	 * evaluated for a world (slot 1) that has no eligible actor. */
	place(&s->player, 208.5, 8.5);
	place(s->guest_actor, 8.5, 208.5);
	struct server_player* none[2] = {0};
	check(server_local_eligible_actors_w(s, b, none, 2) == 0,
		"setup: slot 1 has no eligible actor");
	/* The mob belongs to slot 1 (young: only the >128 leg). A2-E resolves an
	 * entity's world by its slot+generation stamp, so the stamp moves with
	 * the pointer (as test_owner_swap_wolf_interest does). */
	uint8_t slot0 = e->world_slot;
	uint32_t gen0 = e->world_generation;
	e->world = b;
	e->world_slot = 1;
	e->world_generation = b->generation;
	check(server_local_entity_world(s, e) == b,
		"setup: the mob resolves to slot 1");
	tick_mob(s, e);
	check(!despawned(e),
		"a mob whose world holds no eligible actor never despawns "
		"(getClosestPlayer -> null; the slot-0 bodies do not count)");
	e->world = &s->worlds[0];
	e->world_slot = slot0;
	e->world_generation = gen0;
	server_world_discard(b);
	teardown(s);
}


/* A guest body without a position (not yet placed in the world) never
 * governs: the host 200 away despawns the mob although the unplaced guest's
 * stale coordinates sit next to it. */
static void case_guest_no_pos(void) {
	struct server_local* s = admitted_fixture();
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, 208.5, 8.5);
	place(s->guest_actor, 13.5, 8.5);
	s->guest_actor->has_pos = false;
	tick_mob(s, e);
	check(despawned(e),
		"a guest without a position is skipped: the far host governs and the "
		"mob despawns (> 16384)");
	s->guest_actor->has_pos = true;
	teardown(s);
}

/* Beta EntityMob.findPlayerToAttack asks its own World for the nearest
 * player. World.getClosestPlayer scans only that world's playerEntities.
 * A host in Nether at the same coordinates cannot displace the OW guest. */
static void case_world_local_acquire(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = s;
	nether->session = s;
	nether->world_index = 1;
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, 11.5, 8.5); /* closer, but in Nether */
	place(s->guest_actor, 13.5, 8.5); /* farther, in mob's OW */
	s->player.world_slot = 1;
	s->player.dimension = WORLD_DIM_NETHER;
	check(server_local_world_of(s, &s->player) == nether
			  && server_local_world_of(s, s->guest_actor) == &s->worlds[0],
		"setup: host committed to Nether, guest and zombie remain in OW");
	tick_mob(s, e);
	check(e->data.monster.aggro_time > 0,
		"the OW zombie acquires its visible nearby player");
	check(e->data.monster.target_actor.slot == ACTOR_SLOT_GUEST
			  && e->data.monster.target_actor.generation == s->guest_generation,
		"fresh acquisition selects the OW guest, never the closer Nether host");
	s->player.world_slot = 0;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	server_world_discard(nether);
	teardown(s);
}

static void case_no_local_acquire(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = s;
	nether->session = s;
	nether->world_index = 1;
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, 11.5, 8.5);
	place(s->guest_actor, 13.5, 8.5);
	uint8_t slot0 = e->world_slot;
	uint32_t gen0 = e->world_generation;
	e->world = nether;
	e->world_slot = 1;
	e->world_generation = nether->generation;
	check(server_local_eligible_actors_w(s, nether,
		(struct server_player* [2]) {0}, 2) == 0,
		"setup: Nether mob has no local player although OW bodies are nearby");
	tick_mob(s, e);
	check(e->data.monster.aggro_time == 0,
		"a mob with no player in its own world cannot freshly acquire either OW body");
	e->world = &s->worlds[0];
	e->world_slot = slot0;
	e->world_generation = gen0;
	server_world_discard(nether);
	teardown(s);
}

static void case_world_local_tie(void) {
	struct server_local* s = admitted_fixture();
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, 13.5, 8.5);
	place(s->guest_actor, 13.5, 8.5);
	tick_mob(s, e);
	check(e->data.monster.aggro_time > 0,
		"both same-world actors are visible at the same distance");
	check(actor_ident_is_host(e->data.monster.target_actor),
		"Beta World.getClosestPlayer's strict minimum keeps the first host on a tie");
	teardown(s);
}

/* The local SSP/base EntityPlayer target position uses the player eye Y.
 * From mob feet Y=4: host feet=4 and dx=2 gives eye-distance^2 ~=6.62;
 * guest feet=5 and dx=1 gives ~=7.86. A feet-only metric would choose the
 * guest instead. EntityPlayerMP overrides yOffset=0; this test claims only
 * the local single-player body used by CaveWii's split selector. */
static void case_world_local_eye_distance(void) {
	struct server_local* s = admitted_fixture();
	struct entity* e = zombie_at(s, 8.5F, 8.5F);
	place(&s->player, 10.5, 8.5);
	place(s->guest_actor, 9.5, 8.5);
	s->guest_actor->y += 1.0;
	check(s->player.y == 4.0 + PLAYER_EYE_HEIGHT
			  && s->guest_actor->y == 5.0 + PLAYER_EYE_HEIGHT,
		"setup: host feet 4/horizontal 2, guest feet 5/horizontal 1");
	tick_mob(s, e);
	check(e->data.monster.aggro_time > 0,
		"vertical separation fixture still performs a real visible acquisition");
	check(actor_ident_is_host(e->data.monster.target_actor),
		"SSP eye-position 3D nearest selects host; feet-only nearest would select guest");
	teardown(s);
}

static void case_ghast_world_local(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = s;
	nether->session = s;
	nether->world_index = 1;
	struct entity* e = server_local_spawn_monster_at(
		(vec3) {8.5F, 4.0F, 8.5F}, MONSTER_GHAST, s);
	assert(e);
	place(&s->player, 11.5, 8.5);
	place(s->guest_actor, 80.5, 8.5); /* outside ghast fire range, inside despawn radius */
	e->data.monster.ghast_attack_counter = 0;
	tick_mob(s, e);
	check(e->data.monster.ghast_attack_counter == 1,
		"control: a nearby host in the ghast's OW advances its firing counter");
	s->player.world_slot = 1;
	s->player.dimension = WORLD_DIM_NETHER;
	e->data.monster.ghast_attack_counter = 1;
	tick_mob(s, e);
	check(e->data.monster.ghast_attack_counter == 0,
		"OW ghast does not continue targeting the nearby Nether host; distant OW guest is out of range");
	s->player.world_slot = 0;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	server_world_discard(nether);
	teardown(s);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "";
	if(!strcmp(which, "guest_near"))
		case_near(true);
	else if(!strcmp(which, "host_near"))
		case_near(false);
	else if(!strcmp(which, "both_far"))
		case_both_far();
	else if(!strcmp(which, "tie_rng"))
		case_tie_rng();
	else if(!strcmp(which, "no_actor"))
		case_no_actor();
	else if(!strcmp(which, "guest_no_pos"))
		case_guest_no_pos();
	else if(!strcmp(which, "world_local_acquire"))
		case_world_local_acquire();
	else if(!strcmp(which, "no_local_acquire"))
		case_no_local_acquire();
	else if(!strcmp(which, "world_local_tie"))
		case_world_local_tie();
	else if(!strcmp(which, "world_local_eye_distance"))
		case_world_local_eye_distance();
	else if(!strcmp(which, "ghast_world_local"))
		case_ghast_world_local();
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
