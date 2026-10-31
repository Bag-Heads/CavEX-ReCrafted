/*
	PIGZ-PACK: Beta 1.7.3 pig-zombie PACK anger on every player-attributed hit.

	Ground truth (mcb173-retromcp minecraft_server decompile):
	  EntityPigZombie.attackEntityFrom(attacker, damage)        [:49-64]
	      when the attacker is an EntityPlayer,
	          every other entity in its bounding box
	              expanded by 32 (getEntitiesWithinAABBExcludingEntity)
	          that is an EntityPigZombie
	              becomes angry at the attacker,
	          and so does the struck one;
	      (end of the player branch)
	      then the base attackEntityFrom result is returned.
	  becomeAngryAt(attacker)                                    [:67-71]
	      playerToAttack = attacker; angerLevel = 400 + rand.nextInt(400);
	      randomSoundDelay = rand.nextInt(40);       // EACH mob's own Entity.rand
	  Chunk.getEntitiesWithinAABBForEntity                       [Chunk.java:463]
	      candidate != excluded, candidate box intersects query  // strict, no
	                                                             // health filter
	  The attacker each hit passes:
	      melee  EntityPlayer.attackTargetEntityWithCurrentItem   [:468-475]
	             target.attackEntityFrom(player, dmg) -- no alive check
	      arrow  EntityArrow:147    attackEntityFrom(owner, 4)
	      snowball EntitySnowball:135 / egg EntityEgg:135 attackEntityFrom(thrower, 0)
	      fish   EntityFish:174     attackEntityFrom(angler, 0)
	  so the SHOOTER (not the projectile) decides: a player's projectile alerts
	  the pack; a skeleton's arrow or a dispensed (owner-null) projectile alerts
	  nobody -- not even the struck mob.
	  the base attackEntityFrom resets age then returns false at health <= 0
	  [EntityLiving.java:293-299] AFTER the pack anger: a dying (death-flopping)
	  pig zombie still angers its pack and itself when struck.

	Cases (each RED on the pre-PIGZ-PACK product unless marked guard):
	  M1 melee, alive: box edge in (x +32.5, y +33.7), box edge out (x +32.7,
	     y +33.9), dying neighbour in, zombie untouched, other-world (Nether
	     slot) neighbour untouched; struck draws anger THEN its hurt pitch.
	  M2 melee killing blow (guard): pack + self angered.
	  M3 melee on a dying pig zombie: pack + self angered, struck draws only
	     its two anger values (super returns before any pitch).
	  P1 player arrow -> pack; P2 skeleton arrow -> nobody (guard);
	  P3 player arrow on a dying pig zombie -> pack + self.
	  P4 player egg -> pack; P5 player snowball -> pack;
	  P6 dispensed snowball -> nobody (the struck one included).
	  P7 fishing hook -> pack.
	  P8 dispensed arrow / P9 reloaded arrow (from_player, null owner) ->
	     nobody: V5 arrow_owner_kind gates the pack (F3 merge 521b6425f).
	  G1 split guest attacker: the latch names the GUEST on every member.
*/
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "block/dispenser_logic.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_fish.h"
#include "entity/entity_monster.h"
#include "entity/mob_motion.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_guest.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"
#include "worldgen/java_random.h"
#include "support/drain_quiesce.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 100
#define SX 20.5f
#define SZ 20.5f

static struct server_local s;
static volatile int g_drain_run = 1;
static uint32_t g_next_id = 900000;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type) {
	server_world_set_block(&s.worlds[0], x, y, z,
						   (struct block_data) {.type = (uint8_t)type});
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s.entities);
	s.player.fish_entity_id = 0;
}

static void host_at(double x, double z) {
	s.player.x = x;
	s.player.y = (double)(FLOOR_Y + 1) + PLAYER_EYE_HEIGHT;
	s.player.z = z;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
}

static struct entity* mob_at(int id, float x, float y, float z) {
	struct entity* m = server_local_spawn_monster_at((vec3) {x, y, z}, id, &s);
	assert(m);
	mob_motion_set_velocity(m, &m->data.monster.motion_d, 0.0, 0.0, 0.0);
	assert(m->data.monster.motion_d.valid);
	m->data.monster.hurt_resist_time = 0;
	m->data.monster.anger_level = 0;
	m->data.monster.anger_sound_delay = 0;
	m->data.monster.aggro_time = 0;
	return m;
}

static struct entity* pz_at(float dx, float dy, float dz) {
	return mob_at(MONSTER_PIG_ZOMBIE, SX + dx, (float)(FLOOR_Y + 1) + dy,
				  SZ + dz);
}

/* becomeAngryAt replayed on a COPY of the member's own stream. */
static struct java_random expect_angry(const struct entity* e,
									   struct java_random r,
									   struct actor_ident who,
									   const char* what) {
	const int anger = 400 + jrand_next_int_bound(&r, 400);
	const int delay = jrand_next_int_bound(&r, 40);
	if(e->data.monster.anger_level != anger
	   || e->data.monster.anger_sound_delay != delay
	   || !actor_ident_equal(e->data.monster.target_actor, who)) {
		fprintf(stderr,
				"RED %s: anger %d/%d (want %d/%d) target slot %u gen %llu\n",
				what, e->data.monster.anger_level,
				e->data.monster.anger_sound_delay, anger, delay,
				(unsigned)e->data.monster.target_actor.slot,
				(unsigned long long)e->data.monster.target_actor.generation);
		assert(!"pig zombie not angered by Beta's becomeAngryAt");
	}
	return r;
}

static void expect_member(const struct entity* e, struct java_random r0,
						  struct actor_ident who, const char* what) {
	struct java_random r = expect_angry(e, r0, who, what);
	if(e->gameplay_rng.seed != r.seed) {
		fprintf(stderr, "RED %s: member drew != exactly nextInt(400),nextInt(40)\n",
				what);
		assert(!"pack member draw count");
	}
}

static void expect_untouched(const struct entity* e, struct java_random r0,
							 const char* what) {
	if(e->data.monster.anger_level != 0 || e->gameplay_rng.seed != r0.seed) {
		fprintf(stderr, "RED %s: angered/drew (anger %d)\n", what,
				e->data.monster.anger_level);
		assert(!"mob outside Beta's pack query was touched");
	}
}

static void attack(uint32_t id) {
	struct server_rpc rpc = {.type = SRPC_ENTITY_ATTACK,
							 .payload.entity_attack.entity_id = id};
	server_local_test_process_rpc(&s, &rpc);
}

static struct entity* new_entity(void) {
	uint32_t id = g_next_id++;
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = calloc(1, sizeof(struct entity));
	assert(*pp);
	(*pp)->id = id;
	return *pp;
}

/* A projectile 1.2 blocks -X of the struck body, flying +X into it. */
static const vec3 k_shot_pos = {SX - 1.2f, (float)(FLOOR_Y + 1) + 0.9f, SZ};
static const vec3 k_shot_vel = {1.5f, 0.0f, 0.0f};

/* FIRE_ARROW_OWNER_SELF: the archive-restore owner (owner id == the arrow's
 * own id), i.e. a reloaded arrow, whose Beta owner is null. */
#define FIRE_ARROW_OWNER_SELF UINT32_MAX
static bool fire_arrow(uint32_t owner, bool from_player, bool from_skeleton) {
	struct entity* a = new_entity();
	if(owner == FIRE_ARROW_OWNER_SELF)
		owner = a->id;
	entity_arrow(a->id, a, true, &s.worlds[0], owner, ITEM_ARROW, ARROW_DAMAGE,
				 from_player);
	a->data.arrow.from_skeleton = from_skeleton;
	a->teleport(a, (float*)k_shot_pos);
	glm_vec3_copy((float*)k_shot_vel, a->vel);
	a->data.arrow.age = 10;
	return a->tick_server(a, &s);
}

static void throw_thrown(bool egg, uint32_t owner) {
	struct entity* t = new_entity();
	if(egg)
		entity_egg(t->id, t, true, &s.worlds[0], owner);
	else
		entity_snowball(t->id, t, true, &s.worlds[0], owner);
	t->teleport(t, (float*)k_shot_pos);
	glm_vec3_copy((float*)k_shot_vel, t->vel);
	t->data.egg.age = 10;
	(void)t->tick_server(t, &s);
}

static void cast_hook(void) {
	struct entity* f = new_entity();
	entity_fish(f->id, f, true, &s.worlds[0]);
	f->teleport(f, (float*)k_shot_pos);
	glm_vec3_copy((float*)k_shot_vel, f->vel);
	s.player.fish_entity_id = f->id;
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = ITEM_FISHING_ROD, .count = 1});
	(void)f->tick_server(f, &s);
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {0});
}

/* Rebind a mob to the live Nether slot (the A1a/A2-E world binding: pointer
 * plus the (slot, generation) stamp server_local_entity_world resolves). */
static void move_to_nether(struct entity* e) {
	e->world = &s.worlds[1];
	if(e->world_generation != 0) {
		e->world_slot = 1;
		e->world_generation = s.worlds[1].generation;
	}
}

static void make_dying(struct entity* e) {
	e->health = 0;
	e->data.monster.death_time = 3;
}

/* ---- M1: melee alive, the full membership matrix ----------------------- */
static void case_m1(void) {
	clear_entities();
	host_at(SX, SZ - 2.5);
	struct entity* st = pz_at(0, 0, 0);
	struct entity* in_x = pz_at(32.5f, 0, 0);    /* box edge: inside */
	struct entity* out_x = pz_at(32.7f, 0, 0);   /* box edge: outside */
	struct entity* in_y = pz_at(0, 33.7f, 0);    /* 32 + height 1.8: inside */
	struct entity* out_y = pz_at(0, 33.9f, 0);
	struct entity* dying = pz_at(-6.0f, 0, 6.0f);
	make_dying(dying);
	struct entity* zom = mob_at(MONSTER_ZOMBIE, SX - 6.0f,
								(float)(FLOOR_Y + 1), SZ + 9.0f);
	struct entity* nether = pz_at(5.0f, 0, 5.0f);
	move_to_nether(nether);
	assert(server_local_entity_world(&s, nether) == &s.worlds[1]
		   && "precondition: the neighbour lives in the LIVE Nether slot");
	assert(server_local_entity_world(&s, st) == &s.worlds[0]);

	struct java_random r_st = st->gameplay_rng, r_inx = in_x->gameplay_rng,
					   r_outx = out_x->gameplay_rng, r_iny = in_y->gameplay_rng,
					   r_outy = out_y->gameplay_rng, r_dy = dying->gameplay_rng,
					   r_zom = zom->gameplay_rng, r_ne = nether->gameplay_rng;
	const int hp0 = st->health;
	attack(st->id);
	assert(st->health < hp0 && st->health > 0 && "precondition: fresh hit");
	struct java_random r = expect_angry(st, r_st, ACTOR_IDENT_HOST, "M1 struck");
	(void)jrand_next_float(&r); /* hurt pitch, AFTER becomeAngryAt */
	(void)jrand_next_float(&r);
	assert(st->gameplay_rng.seed == r.seed
		   && "struck: anger pair, then exactly its two hurt-pitch floats");
	expect_member(in_x, r_inx, ACTOR_IDENT_HOST, "M1 x+32.5 (box intersects)");
	expect_member(in_y, r_iny, ACTOR_IDENT_HOST, "M1 y+33.7 (box intersects)");
	expect_member(dying, r_dy, ACTOR_IDENT_HOST, "M1 dying neighbour");
	expect_untouched(out_x, r_outx, "M1 x+32.7");
	expect_untouched(out_y, r_outy, "M1 y+33.9");
	expect_untouched(zom, r_zom, "M1 zombie");
	expect_untouched(nether, r_ne, "M1 other-world neighbour");
	puts("  M1: melee alive -> Beta box membership, own-world only, exact draws");
}

/* ---- M2: melee killing blow ------------------------------------------- */
static void case_m2(void) {
	clear_entities();
	host_at(SX, SZ - 2.5);
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10.0f);
	st->health = 1;
	struct java_random r_st = st->gameplay_rng, r_nb = nb->gameplay_rng;
	attack(st->id);
	assert(st->health <= 0 && "precondition: the blow kills");
	(void)expect_angry(st, r_st, ACTOR_IDENT_HOST, "M2 struck");
	expect_member(nb, r_nb, ACTOR_IDENT_HOST, "M2 neighbour");
	puts("  M2: melee killing blow -> pack + self angered first");
}

/* ---- M3: melee on a dying pig zombie ----------------------------------- */
static void case_m3(void) {
	clear_entities();
	host_at(SX, SZ - 2.5);
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10.0f);
	make_dying(st);
	st->data.monster.entity_age = 77;
	struct java_random r_st = st->gameplay_rng, r_nb = nb->gameplay_rng;
	attack(st->id);
	assert(st->data.monster.entity_age == 0 && "age reset still runs");
	expect_member(st, r_st, ACTOR_IDENT_HOST,
				  "M3 dying struck (anger only, super returns before pitch)");
	expect_member(nb, r_nb, ACTOR_IDENT_HOST, "M3 neighbour");
	puts("  M3: melee on a dying pig zombie -> pack + self angered");
}

/* ---- projectile cases --------------------------------------------------- */
enum shot { SHOT_ARROW, SHOT_SKEL_ARROW, SHOT_EGG, SHOT_SNOWBALL,
			SHOT_DISPENSED_SNOWBALL, SHOT_HOOK, SHOT_DISPENSED_ARROW,
			SHOT_RELOADED_ARROW, SHOT_FIREBALL };

static void shoot(enum shot k) {
	switch(k) {
		case SHOT_ARROW: fire_arrow(PROJECTILE_OWNER_LOCAL_PLAYER, true, false); break;
		case SHOT_SKEL_ARROW: fire_arrow(777u, false, true); break;
		case SHOT_EGG: throw_thrown(true, PROJECTILE_OWNER_LOCAL_PLAYER); break;
		case SHOT_SNOWBALL: throw_thrown(false, PROJECTILE_OWNER_LOCAL_PLAYER); break;
		case SHOT_DISPENSED_SNOWBALL:
			throw_thrown(false, DISPENSER_PROJECTILE_OWNER_SENTINEL);
			break;
		case SHOT_HOOK: cast_hook(); break;
		/* V5 arrow_owner_kind x PIGZ-PACK (F3): BlockDispenser builds
		 * EntityArrow(world, x, y, z) -- owner null, doesArrowBelongToPlayer
		 * true -- and Beta never saves EntityArrow.owner, so a reloaded arrow
		 * is owner-null too. EntityArrow:147 passes that null owner, so
		 * EntityPigZombie:49 `var1 instanceof EntityPlayer` is false: the
		 * pickable from_player flag must NOT anger anyone. */
		case SHOT_DISPENSED_ARROW:
			fire_arrow(DISPENSER_PROJECTILE_OWNER_SENTINEL, true, false);
			break;
		case SHOT_RELOADED_ARROW:
			fire_arrow(FIRE_ARROW_OWNER_SELF, true, false);
			break;
		case SHOT_FIREBALL: assert(!"use world sweep fixture"); break;
	}
}

static void case_projectile(const char* tag, enum shot k, bool angers,
							bool struck_dying) {
	clear_entities();
	host_at(SX - 8.0, SZ + 12.0); /* the shooter/angler: off the flight line */
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10.0f);
	struct entity* far_nb = pz_at(0, 0, 32.7f);
	struct entity* nether = pz_at(0, 0, 6.0f);
	move_to_nether(nether);
	assert(server_local_entity_world(&s, nether) == &s.worlds[1]);
	if(struck_dying)
		make_dying(st);
	const int hurt0 = st->data.monster.hurt_time;
	struct java_random r_st = st->gameplay_rng, r_nb = nb->gameplay_rng,
					   r_far = far_nb->gameplay_rng, r_ne = nether->gameplay_rng;
	shoot(k);
	if(!struck_dying)
		assert(st->data.monster.hurt_time != hurt0
			   && "precondition: the projectile struck the pig zombie");
	char what[96];
	if(angers) {
		snprintf(what, sizeof what, "%s struck", tag);
		(void)expect_angry(st, r_st, ACTOR_IDENT_HOST, what);
		snprintf(what, sizeof what, "%s neighbour", tag);
		expect_member(nb, r_nb, ACTOR_IDENT_HOST, what);
	} else {
		snprintf(what, sizeof what, "%s struck", tag);
		assert(st->data.monster.anger_level == 0 && "non-player source: no anger");
		snprintf(what, sizeof what, "%s neighbour", tag);
		expect_untouched(nb, r_nb, what);
	}
	snprintf(what, sizeof what, "%s z+32.7", tag);
	expect_untouched(far_nb, r_far, what);
	snprintf(what, sizeof what, "%s other-world", tag);
	expect_untouched(nether, r_ne, what);
	printf("  %s: %s\n", tag, angers ? "pack + self angered" : "nobody angered");
}

/* ---- G1: split guest attacker ------------------------------------------ */
static struct server_rpc_source admit_guest(void) {
	clear_entities();
	/* Earlier egg cases may have queued chicks. Flush the real spawn queue
	 * before clearing this case's entities so no chick steals the bow hit. */
	assert(server_local_test_tick_entities(&s));
	clear_entities();
	host_at(SX, SZ - 2.5);
	assert(svin_init());
	string_init_set_str(s.level_name, TMP_WORLD);
	assert(level_archive_create_new(&s.level, s.level_name, 4242,
		(vec3){SX, FLOOR_Y + 1, SZ}));
	assert(server_local_set_split_enabled(&s, true));
	/* Real server admission: JOIN -> ACCEPT/publish -> CONFIRM -> GAMEPLAY.
	 * The headless consumer only drains output; no guest flags/identity are
	 * fabricated. The loaded column's sent bit represents terrain publication. */
	struct server_rpc control = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 1010, .action = LOCAL_GUEST_JOIN}};
	server_local_guest_control(&s, (struct server_rpc_source){0}, &control);
	assert(s.guest_owned && s.guest_actor && !s.guest_confirmed);
	struct server_rpc_source source = {.slot = 1,
		.generation = s.guest_generation};
	control.payload.local_guest.action = LOCAL_GUEST_ACCEPT;
	/* Nonblocking publication requires both the server pending ring and the
	 * client inbox drained, just as tick-start flush plus client consumption. */
	for(unsigned retry = 0; retry < 256; retry++) {
		test_drain_quiesce();
		if(clin_rpc_flush_pending()) break;
		assert(retry < 255);
	}
	test_drain_quiesce();
	server_local_guest_control(&s, source, &control);
	assert(s.guest_snapshot_sent);
	control.payload.local_guest.action = LOCAL_GUEST_CONFIRM;
	server_local_guest_control(&s, source, &control);
	assert(s.guest_confirmed && !s.guest_gameplay);
	struct server_chunk* home = dict_server_chunks_get(s.worlds[0].chunks,
		S_CHUNK_ID(1, 1));
	assert(home);
	home->sent = true;
	control.payload.local_guest.action = LOCAL_GUEST_GAMEPLAY;
	server_local_guest_control(&s, source, &control);
	assert(s.guest_gameplay && s.guest_actor->finished_loading);
	struct actor_ident guest = server_local_actor_ident(&s, s.guest_actor);
	assert(guest.slot == ACTOR_SLOT_GUEST && guest.generation == source.generation);
	return source;
}

static void release_guest(void) {
	clear_entities();
	server_local_guest_release_checkpointed(&s);
	assert(server_local_set_split_enabled(&s, false));
	level_archive_destroy(&s.level);
	string_clear(s.level_name);
}

static void case_g1(void) {
	struct server_rpc_source source = admit_guest();
	struct actor_ident guest = server_local_actor_ident(&s, s.guest_actor);
	/* Real guest queue/dispatch and melee attribution; host damage state stays
	 * byte-identical even when host and guest occupy the same firing position. */
	struct server_player host_before = s.player;
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10.0f);
	struct java_random r_st = st->gameplay_rng, r_nb = nb->gameplay_rng;
	struct server_rpc hit = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = st->id};
	assert(svin_rpc_send_from(source, &hit));
	server_local_test_process_messages(&s);
	assert(st->health < 20);
	struct java_random r = expect_angry(st, r_st, guest, "G1 melee struck");
	(void)jrand_next_float(&r);
	(void)jrand_next_float(&r);
	assert(st->gameplay_rng.seed == r.seed);
	expect_member(nb, r_nb, guest, "G1 neighbour");
	assert(memcmp(&host_before, &s.player, sizeof host_before) == 0);
	clear_entities();
	/* SRPC_USE_ITEM -> bow -> pending queue -> production flush stamps the
	 * admitted guest, before the arrow is aimed at the deterministic hit lane.
	 * Only flight geometry is adjusted; ownership is observed, never installed. */
	s.guest_actor->x = SX - 8.0;
	s.guest_actor->z = SZ + 12.0;
	inventory_set_slot(&s.guest_actor->inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data){.id = ITEM_BOW, .count = 1});
	inventory_set_slot(&s.guest_actor->inventory, INVENTORY_SLOT_MAIN,
		(struct item_data){.id = ITEM_ARROW, .count = 2});
	struct server_rpc use = {.type = SRPC_USE_ITEM};
	assert(svin_rpc_send_from(source, &use));
	server_local_test_process_messages(&s);
	assert(s.pending_arrow_count == 1);
	assert(actor_ident_equal(s.pending_arrows[0].shooter, guest));
	assert(server_local_test_tick_entities(&s));
	struct entity* arrow = NULL;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ARROW) arrow = e;
	}
	assert(arrow && arrow->data.arrow.owner_id == 0
		&& arrow->data.arrow.from_player
		&& actor_ident_equal(arrow->data.arrow.shooter_actor, guest));
	arrow->teleport(arrow, (float*)k_shot_pos);
	glm_vec3_copy((float*)k_shot_vel, arrow->vel);
	arrow->data.arrow.age = 10;
	st = pz_at(0, 0, 0);
	nb = pz_at(0, 0, 10.0f);
	r_st = st->gameplay_rng;
	r_nb = nb->gameplay_rng;
	assert(arrow->tick_server(arrow, &s));
	assert(st->health == 16);
	r = expect_angry(st, r_st, guest, "G1 bow struck");
	(void)jrand_next_float(&r);
	(void)jrand_next_float(&r);
	assert(st->gameplay_rng.seed == r.seed);
	expect_member(nb, r_nb, guest, "G1 bow neighbour");
	assert(memcmp(&host_before, &s.player, sizeof host_before) == 0);
	assert(s.guest_actor->health == MAX_PLAYER_HEALTH
		&& s.guest_actor->hurt_resist_time == 0);
	release_guest();
	puts("  G1: real admission + guest melee/bow owner stamping, host unchanged");
}

/* EntityArrow:120, EntityEgg/EntitySnowball:110, EntityFish:148 query
 * this.worldObj. A foreign-world body at the SAME coordinates cannot win. */
static void case_world(enum shot shot) {
	for(unsigned own_slot = 0; own_slot < 2; own_slot++) {
		clear_entities();
		host_at(SX - 8.0, SZ + 12.0);
		/* Keep the host in the Overworld so a Nether shot cannot accidentally
		 * use the host's world as its filter. The hook's angler stays with it. */
		s.player.world_slot = shot == SHOT_HOOK ? own_slot : 0;
		struct entity* foreign = pz_at(0, 0, 0);
		struct entity* neighbour = pz_at(0, 0, 10);
		if(own_slot == 0) {
			move_to_nether(foreign);
			move_to_nether(neighbour);
		}
		struct java_random r0 = foreign->gameplay_rng;
		struct java_random rn = neighbour->gameplay_rng;
		struct entity* p = new_entity();
		struct server_world* own = &s.worlds[own_slot];
		if(shot == SHOT_ARROW)
			entity_arrow(p->id, p, true, own, 0, ITEM_ARROW, ARROW_DAMAGE, true);
		else if(shot == SHOT_HOOK) {
			entity_fish(p->id, p, true, own);
			s.player.fish_entity_id = p->id;
			inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
				(struct item_data){.id = ITEM_FISHING_ROD, .count = 1});
		} else if(shot == SHOT_EGG)
			entity_egg(p->id, p, true, own, 0);
		else entity_snowball(p->id, p, true, own, 0);
		p->teleport(p, (float*)k_shot_pos);
		glm_vec3_copy((float*)k_shot_vel, p->vel);
		assert(!p->tick_server(p, &s));
		assert(foreign->health == 20 && foreign->data.monster.hurt_time == 0);
		expect_untouched(foreign, r0, "world overlap victim");
		expect_untouched(neighbour, rn, "world overlap pack");
		assert(p->pos[0] > SX && "foreign body did not block flight");
		/* Also require an own-world target to be selected at the very same
		 * coordinates, including a Nether projectile with an Overworld host. */
		struct entity* local = pz_at(0, 0, 0);
		if(own_slot == 1) move_to_nether(local);
		p->teleport(p, (float*)k_shot_pos);
		glm_vec3_copy((float*)k_shot_vel, p->vel);
		(void)p->tick_server(p, &s);
		assert(local->data.monster.hurt_time > 0
			&& local->data.monster.anger_level >= 400);
		expect_untouched(foreign, r0, "world overlap after own-world hit");
		expect_untouched(neighbour, rn, "foreign pack after own-world hit");
	}
	s.player.world_slot = 0;
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data){0});
	printf("  world overlap shot %d: both directions leave foreign victim/pack intact\n", shot);
}

static struct entity* world_shot(enum shot shot, unsigned slot,
								 struct server_player* angler) {
	struct entity* p = new_entity();
	struct server_world* own = &s.worlds[slot];
	if(shot == SHOT_ARROW) {
		entity_arrow(p->id, p, true, own, 999u, ITEM_ARROW, ARROW_DAMAGE, false);
		p->data.arrow.age = 10;
	} else if(shot == SHOT_HOOK) {
		entity_fish(p->id, p, true, own);
		p->data.fish.angler_actor = server_local_actor_ident(&s, angler);
		p->data.fish.ticks_in_air = 10;
		angler->fish_entity_id = p->id;
		inventory_set_slot(&angler->inventory, INVENTORY_SLOT_HOTBAR,
			(struct item_data){.id = ITEM_FISHING_ROD, .count = 1});
	} else if(shot == SHOT_FIREBALL) {
		entity_fireball(p->id, p, true, own, 999u, k_shot_vel);
		p->data.fireball.age = 25;
	} else {
		if(shot == SHOT_EGG) entity_egg(p->id, p, true, own, 999u);
		else entity_snowball(p->id, p, true, own, 999u);
		p->data.egg.age = 10;
	}
	p->teleport(p, (float*)k_shot_pos);
	glm_vec3_copy((float*)k_shot_vel, p->vel);
	assert(server_local_entity_world(&s, p) == own);
	return p;
}

/* EntityFireball.java:82 queries worldObj, exactly like the other four kinds.
 * Foreign mobs cannot detonate the shot; an own-world body still must. */
static void case_fireball_world(void) {
	for(unsigned slot = 0; slot < 2; slot++) {
		clear_entities();
		host_at(SX - 8, SZ + 12);
		struct entity* foreign = pz_at(0, 0, 0);
		if(slot == 0) move_to_nether(foreign);
		struct java_random r0 = foreign->gameplay_rng;
		struct entity* p = world_shot(SHOT_FIREBALL, slot, &s.player);
		assert(!p->tick_server(p, &s) && "foreign mob detonated fireball");
		assert(p->pos[0] > SX);
		assert(foreign->health == 20 && foreign->data.monster.hurt_time == 0);
		expect_untouched(foreign, r0, "fireball foreign mob");
		struct entity* local = pz_at(0, 0, 0);
		if(slot == 1) move_to_nether(local);
		p = world_shot(SHOT_FIREBALL, slot, &s.player);
		assert(p->tick_server(p, &s) && "own-world mob must detonate fireball");
		assert(local->data.monster.hurt_time > 0);
		assert(foreign->health == 20 && foreign->data.monster.hurt_time == 0);
		expect_untouched(foreign, r0, "fireball foreign mob after local hit");
	}
	puts("  fireball mob worlds: both foreign misses and own-world impacts PASS");
}

/* EntityArrow:120, Egg/Snowball:110, Fish:148, Fireball:82 query worldObj
 * for ALL victims. Real admission supplies the guest, then vary only geometry
 * and committed dimensions. Include host victims and both flight directions. */
static void case_player_world(enum shot shot) {
	(void)admit_guest();
	for(unsigned slot = 0; slot < 2; slot++) {
		for(int guest_victim = 1; guest_victim >= 0; guest_victim--) {
			clear_entities();
			host_at(SX - 8, SZ + 12);
			struct server_player* target = guest_victim ? s.guest_actor : &s.player;
			struct server_player* other = guest_victim ? &s.player : s.guest_actor;
			target->x = SX; target->y = FLOOR_Y + 1 + PLAYER_EYE_HEIGHT;
			target->z = SZ;
			other->x = SX - 8; other->y = target->y; other->z = SZ + 12;
			target->has_pos = other->has_pos = true;
			target->finished_loading = other->finished_loading = true;
			target->health = other->health = MAX_PLAYER_HEALTH;
			target->hurt_resist_time = target->last_damage = 0;
			target->world_slot = 1 - slot;
			other->world_slot = slot;
			assert(server_local_damage_candidate(&s, guest_victim) == target);
			struct entity* p = world_shot(shot, slot, other);
			assert(!p->tick_server(p, &s) && "foreign player consumed projectile");
			assert(target->health == MAX_PLAYER_HEALTH && target->hurt_resist_time == 0);
			if(shot == SHOT_HOOK) assert(!p->data.fish.bobber_hooked);
			assert(p->pos[0] > SX && "foreign player blocked flight");
			/* Positive control: identical coordinates, now in projectile world. */
			target->world_slot = slot;
			p = world_shot(shot, slot, other);
			bool consumed = p->tick_server(p, &s);
			assert(consumed == (shot != SHOT_HOOK));
			assert(target->hurt_resist_time > 0);
			if(shot == SHOT_HOOK) {
				/* Existing player damage core arms the zero-hit window but
				 * returns accepted=false at apply<=0, so no player latch.
				 * The window above proves selection; keep this behavior. */
				assert(!p->data.fish.bobber_hooked);
			}
		}
	}
	s.player.world_slot = s.guest_actor->world_slot = 0;
	release_guest();
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR, (struct item_data){0});
	printf("  player worlds shot %d: admitted guest/host, both dimensions, miss/hit PASS\n", shot);
}

/* EntityPigZombie:20-24 runs bark BEFORE super.onUpdate, even on corpses.
 * Independent Java Random replay, seed 34: first hit 526/3; quiet rolls
 * 347,309,666 (two bark floats before the third); second hit 749/25.
 * Check first corpse tick (deathTime 0) and subsequent flop (deathTime 3). */
static void case_corpse_bark(int death_time, bool order_probe) {
	clear_entities();
	host_at(SX, SZ - 2.5);
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10);
	st->health = 0;
	entity_monster_begin_death(st, &s, st->pos);
	st->data.monster.death_time = death_time;
	st->data.monster.living_sound_time = 0;
	jrand_set_seed(&st->gameplay_rng, 34);
	struct java_random expected = st->gameplay_rng;
	attack(st->id);
	expected = expect_angry(st, expected, ACTOR_IDENT_HOST, "corpse initial hit");
	assert(st->data.monster.anger_level == 526 && st->data.monster.anger_sound_delay == 3);
	bool ticks_exact = true;
	for(int tick = 1; tick <= 3; tick++) {
		if(tick == 3) {
			/* With bark first the living roll is 666 > 200 (quiet).
			 * Reversing them gives 113 < 200 and two extra voice floats. */
			if(order_probe) st->data.monster.living_sound_time = 200;
			(void)jrand_next_float(&expected);
			(void)jrand_next_float(&expected);
		}
		(void)jrand_next_int_bound(&expected, 1000);
		assert(!st->tick_server(st, &s));
		ticks_exact = ticks_exact && st->data.monster.anger_sound_delay == 3 - tick
			&& st->gameplay_rng.seed == expected.seed
			&& st->data.monster.living_sound_time == (tick == 3 && order_probe ? 201 : tick)
			&& st->data.monster.death_time == death_time + tick;
	}
	struct java_random rn = nb->gameplay_rng;
	attack(st->id);
	expect_member(st, expected, ACTOR_IDENT_HOST, "corpse bark then second hit");
	expect_member(nb, rn, ACTOR_IDENT_HOST, "corpse bark re-anger neighbour");
	assert(ticks_exact && "corpse bark/living/death countdown order");
	assert(st->data.monster.anger_level == 749 && st->data.monster.anger_sound_delay == 25);
	printf("  corpse bark deathTime %d order %d: 526/3 -> bark tick 3 -> second hit 749/25 PASS\n", death_time, order_probe);
}

/* EntityLiving:104 draws nextInt(1000) BEFORE :151-153 ++deathTime, even
 * at health<=0. :93-96 draws two pitch floats only if the scheduler fires.
 * EntityPigZombie:67-70 then draws re-anger from that advanced Random. */
static void case_corpse(bool voice, int death_time) {
	clear_entities();
	host_at(SX, SZ - 2.5);
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10);
	st->health = 1;
	attack(st->id);
	assert(st->health <= 0);
	entity_monster_begin_death(st, &s, st->pos);
	st->data.monster.death_time = death_time;
	st->data.monster.anger_sound_delay = 0; /* isolate the living scheduler */
	st->data.monster.living_sound_time = voice ? 1000 : 0;
	struct java_random expected = st->gameplay_rng;
	(void)jrand_next_int_bound(&expected, 1000);
	if(voice) {
		(void)jrand_next_float(&expected);
		(void)jrand_next_float(&expected);
	}
	assert(!st->tick_server(st, &s));
	assert(st->data.monster.death_time == death_time + 1);
	const int sound_time = st->data.monster.living_sound_time;
	struct java_random rn = nb->gameplay_rng;
	attack(st->id);
	expect_member(st, expected, ACTOR_IDENT_HOST, "corpse tick then re-anger");
	expect_member(nb, rn, ACTOR_IDENT_HOST, "corpse re-anger neighbour");
	assert(sound_time == (voice ? -80 : 1));
	printf("  corpse deathTime %d voice %d: living roll/pitch before exact re-anger pair\n",
		death_time, voice);
}

/* EntityPigZombie:49-64 alerts before EntityLiving:293-345 damage gate.
 * Absorbed/excess/lethal hits must all propagate, with exact own RNG draws. */
static void case_arrow_gate(unsigned gate) {
	clear_entities();
	host_at(SX - 8.0, SZ + 12.0);
	struct entity* st = pz_at(0, 0, 0);
	struct entity* nb = pz_at(0, 0, 10);
	if(gate < 2) {
		st->data.monster.hurt_resist_time = MOB_HURT_RESIST_TICKS;
		st->data.monster.last_damage = gate == 0 ? 4 : 2;
	} else st->health = 4;
	struct java_random rs = st->gameplay_rng, rn = nb->gameplay_rng;
	bool consumed = fire_arrow(0, true, false);
	assert(consumed == (gate != 0));
	assert(st->health == (gate == 0 ? 20 : gate == 1 ? 18 : 0));
	assert(st->data.monster.last_damage == 4);
	assert(st->data.monster.hurt_resist_time == MOB_HURT_RESIST_TICKS);
	assert(st->data.monster.hurt_time == (gate == 2 ? MOB_HURT_ANIMATION_TICKS : 0));
	struct java_random r = expect_angry(st, rs, ACTOR_IDENT_HOST, "arrow gate struck");
	if(gate == 2) {
		(void)jrand_next_float(&r);
		(void)jrand_next_float(&r);
	}
	assert(st->gameplay_rng.seed == r.seed);
	expect_member(nb, rn, ACTOR_IDENT_HOST, "arrow gate neighbour");
	printf("  arrow gate %u: exact absorbed/excess/lethal pack anger\n", gate);
}

int main(int argc, char** argv) {
	test_fixture_dir("test_pig_zombie_pack_anger_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	MKDIR(test_fixture_join(TMP_WORLD, "/DIM-1"));
	MKDIR(test_fixture_join(TMP_WORLD, "/DIM-1/region"));
	string_t level, nether_level;
	string_init_set_str(level, TMP_WORLD);
	string_init_set_str(nether_level, test_fixture_join(TMP_WORLD, "/DIM-1"));

	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	/* A LIVE second world slot (the split-dimensions Nether) at the SAME
	   coordinates: a pig zombie there must never join an Overworld pack. */
	server_world_create(&s.worlds[1], nether_level, WORLD_DIM_NETHER, 4242,
						true);
	s.worlds[1].session = &s;
	s.worlds[1].local = &s;
	s.worlds[1].world_index = 1;
	if(s.worlds[1].generation == 0)
		s.worlds[1].generation = 1;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x9123A11Bu;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	inventory_set_hotbar(&s.player.inventory, 0);
	for(int cz = -2; cz <= 4; cz++)
		for(int cx = -2; cx <= 4; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	for(int x = -12; x < 56; x++)
		for(int z = -12; z < 56; z++) {
			set(x, FLOOR_Y, z, BLOCK_STONE);
			for(int y = FLOOR_Y + 1; y < FLOOR_Y + 40; y++)
				set(x, y, z, BLOCK_AIR);
		}

	puts("test_pig_zombie_pack_anger:");
	/* A named regression can run independently so one RED does not conceal
	 * another. The default still executes every original and new case. */
	if(argc > 1) {
		if(strcmp(argv[1], "corpse-bark-first") == 0) case_corpse_bark(0, false);
		else if(strcmp(argv[1], "corpse-bark-flop") == 0) case_corpse_bark(3, false);
		else if(strcmp(argv[1], "corpse-bark-order") == 0) case_corpse_bark(3, true);
		else if(strcmp(argv[1], "player-arrow") == 0) case_player_world(SHOT_ARROW);
		else if(strcmp(argv[1], "player-egg") == 0) case_player_world(SHOT_EGG);
		else if(strcmp(argv[1], "player-snowball") == 0) case_player_world(SHOT_SNOWBALL);
		else if(strcmp(argv[1], "player-hook") == 0) case_player_world(SHOT_HOOK);
		else if(strcmp(argv[1], "player-fireball") == 0) case_player_world(SHOT_FIREBALL);
		else if(strcmp(argv[1], "world-fireball") == 0) case_fireball_world();
		else if(strcmp(argv[1], "world-arrow") == 0) case_world(SHOT_ARROW);
		else if(strcmp(argv[1], "world-egg") == 0) case_world(SHOT_EGG);
		else if(strcmp(argv[1], "world-snowball") == 0) case_world(SHOT_SNOWBALL);
		else if(strcmp(argv[1], "world-hook") == 0) case_world(SHOT_HOOK);
		else if(strcmp(argv[1], "corpse-first") == 0) case_corpse(false, 0);
		else if(strcmp(argv[1], "corpse-quiet") == 0) case_corpse(false, 1);
		else if(strcmp(argv[1], "corpse-voice") == 0) case_corpse(true, 1);
		else if(strcmp(argv[1], "absorbed") == 0) case_arrow_gate(0);
		else if(strcmp(argv[1], "excess") == 0) case_arrow_gate(1);
		else if(strcmp(argv[1], "lethal") == 0) case_arrow_gate(2);
		else if(strcmp(argv[1], "guest") == 0) case_g1();
		else assert(!"unknown regression case");
		goto cleanup;
	}
	case_m1();
	case_m2();
	case_m3();
	case_projectile("P1 player arrow", SHOT_ARROW, true, false);
	case_projectile("P2 skeleton arrow", SHOT_SKEL_ARROW, false, false);
	case_projectile("P3 player arrow on dying", SHOT_ARROW, true, true);
	case_projectile("P4 player egg", SHOT_EGG, true, false);
	case_projectile("P5 player snowball", SHOT_SNOWBALL, true, false);
	case_projectile("P6 dispensed snowball", SHOT_DISPENSED_SNOWBALL, false,
					false);
	case_projectile("P7 fishing hook", SHOT_HOOK, true, false);
	case_projectile("P8 dispensed arrow", SHOT_DISPENSED_ARROW, false, false);
	case_projectile("P9 reloaded arrow", SHOT_RELOADED_ARROW, false, false);
	case_g1();
	case_world(SHOT_ARROW);
	case_world(SHOT_EGG);
	case_world(SHOT_SNOWBALL);
	case_world(SHOT_HOOK);
	case_corpse(false, 0);
	case_corpse(false, 1);
	case_corpse(true, 1);
	case_arrow_gate(0);
	case_arrow_gate(1);
	case_arrow_gate(2);
	case_corpse_bark(0, false);
	case_corpse_bark(3, false);
	case_corpse_bark(3, true);
	case_fireball_world();
	case_player_world(SHOT_ARROW);
	case_player_world(SHOT_EGG);
	case_player_world(SHOT_SNOWBALL);
	case_player_world(SHOT_HOOK);
	case_player_world(SHOT_FIREBALL);

cleanup:
	clear_entities();
	g_drain_run = 0;
	thread_join(&drain);
	string_clear(level);
	string_clear(nether_level);
	test_fixture_dir_cleanup(TMP_WORLD);
	puts("test_pig_zombie_pack_anger: ALL PASS");
	return 0;
}
