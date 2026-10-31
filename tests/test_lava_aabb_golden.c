/* Beta 1.7.3 Entity.handleLavaMovement / World.isMaterialInBB golden.
 * Authored RED before the shared scanner port. Ground truth:
 * [740c583: Entity.java:566, Entity.java:234, World.java:1458]. */
#include <assert.h>
#include <math.h>
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "entity/combat.h"
#include "entity/entity_monster.h"
#include "item/inventory.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static struct server_local s;
static char fixture[128];
static volatile int draining = 1;
static int failures;

static void golden(const char* case_name, int actual, int expected) {
	if(actual != expected) {
		fprintf(stderr, "RED %s: actual=%d expected=%d\n", case_name,
			actual, expected);
		failures++;
	} else {
		printf("PASS %s: %d\n", case_name, actual);
	}
}

static void* drain(void* unused) {
	(void)unused;
	while(draining) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void put(int x, int y, int z, int block) {
	struct block_data expected = {.type = (uint8_t)block};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data current;
	assert(server_world_get_block(&s.world, x, y, z, &current));
	assert(current.type == expected.type);
}

/* Scenario prerequisites (ASTRA-SEED-POLICY-0925): the fixture asserts the
 * contact geometry it relies on, before and after tick 1, instead of letting a
 * seed choose it. Beta spawns mobs with zero motion (Entity/EntityLiving
 * constructors set no motionX/Y/Z); CaveWii's spawn helper adds a random
 * 0.05..0.25 velocity, so the spawn helpers below zero it. */
static void prereq(const char* what, bool ok, float value) {
	if(ok)
		printf("PASS prerequisite %s (%.4f)\n", what, value);
	else {
		fprintf(stderr, "PREREQ-FAIL %s (%.4f)\n", what, value);
		failures++;
	}
}

static struct entity* spawn_creeper(double x, double feet_y, double z) {
	vec3 pos = {(float)x, (float)feet_y, (float)z};
	struct entity* mob = server_local_spawn_monster(pos, MONSTER_CREEPER, &s);
	assert(mob && mob->tick_server);
	/* Preserve the requested doubles through the same float entity storage the
	 * production mob path uses. */
	mob->pos[0] = (float)x;
	mob->pos[1] = (float)feet_y;
	mob->pos[2] = (float)z;
	mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
	test_mob_sync(mob); /* motion units S2 */
	mob->health = 20;
	mob->data.monster.fire_time = 0;
	return mob;
}

static void reset_player(double x, double feet_y, double z) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = x;
	s.player.y = feet_y + (double)PLAYER_EYE_HEIGHT;
	s.player.z = z;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	s.player.oxygen = MAX_OXYGEN;
	s.player.fire = -PLAYER_FIRE_RESISTANCE;
	s.player.fall_distance = 0.0F;
	s.player.vel_x = 0.0F;
	s.player.vel_y = 0.0F;
	s.player.vel_z = 0.0F;
	s.player.old_vel_y = 0.0F;
	s.player.on_ground = true;
	s.player.spawn_x = (int)x;
	s.player.spawn_y = (int)feet_y;
	s.player.spawn_z = (int)z;
	s.player.active_inventory = &s.player.inventory;
	s.zombie_demo = false;
}

static void case_side_overlap_mob(void) {
	put(8, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_LAVA_STILL);
	struct entity* mob = spawn_creeper(8.85, 70.05, 8.5);
	struct AABB box;
	entity_make_world_bbox(mob, &box);
	prereq("A pre-tick: at rest, body strictly overlaps lava x=9 (maxX)",
		mob->vel[0] == 0.0F && mob->vel[1] == 0.0F && mob->vel[2] == 0.0F
			&& box.x2 > 9.0F,
		box.x2);
	assert(!mob->tick_server(mob, &s));
	entity_make_world_bbox(mob, &box);
	prereq("A after tick 1: body still strictly overlaps lava x=9 (maxX)",
		box.x2 > 9.0F, box.x2);
	golden("A side-overlap mob health", mob->health, 16);
	/* Entity.onEntityUpdate's lava 4 precedes moveEntity's burning-box 1.
	 * heartsLife absorbs the second hit, while fire advances 600 -> 601.
	 * [mcb173-retromcp: Entity.java:218-237,480-490;
	 * World.java:1395-1415; EntityLiving.java:attackEntityFrom] */
	golden("A side-overlap mob fire", mob->data.monster.fire_time, 601);
	golden("A side-overlap mob iframe", mob->data.monster.hurt_resist_time,
		9);
	for(int tick = 2; tick <= 11; tick++) {
		/* Keep the same contact geometry despite the mob's AI/physics. */
		mob->pos[0] = 8.85F;
		mob->pos[1] = 70.05F;
		mob->pos[2] = 8.5F;
		mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
		test_mob_sync(mob); /* motion units S2 */
		mob->on_ground = true;
		assert(!mob->tick_server(mob, &s));
		golden("A lava mob fire each tick", mob->data.monster.fire_time,
			601);
		golden("A lava mob health arbitration", mob->health,
			tick < 10 ? 16 : tick == 10 ? 15 : 12);
	}
	put(9, 70, 8, BLOCK_AIR);
}

static void case_below_contracted_body_mob(void) {
	put(8, 70, 8, BLOCK_LAVA_STILL);
	put(9, 70, 8, BLOCK_AIR);
	struct entity* mob = spawn_creeper(8.5, 70.70, 8.5);
	assert(!mob->tick_server(mob, &s));
	/* Lava is outside handleLavaMovement's .4 inset but inside the
	 * floor-based burning-box scan: the isolated +1 is observable. */
	golden("B below-body mob health", mob->health, 19);
	golden("B below-body mob fire", mob->data.monster.fire_time, 1);
	put(8, 70, 8, BLOCK_AIR);
}

static void case_side_overlap_host_control(void) {
	put(8, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_LAVA_STILL);
	reset_player(8.85, 70.05, 8.5);
	server_local_test_update_once(&s);
	golden("C side-overlap host health", s.player.health,
		MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	golden("C side-overlap host fire", s.player.fire, 601);
	golden("C side-overlap host strongest hit", s.player.last_damage,
		4 * (HEALTH_PER_HEART / 2));
	for(int tick = 2; tick <= 11; tick++) {
		server_local_test_update_once(&s);
		golden("C lava host fire each tick", s.player.fire, 601);
		/* G6b F3: Beta decrements heartsLife BETWEEN the lava 4
		 * (Entity.onEntityUpdate:218 via EntityLiving.onEntityUpdate:107) and
		 * the burning-box 1 (Entity.moveEntity:482), at EntityLiving.java:151.
		 * Tick k lava sees heartsLife 21-k (absorbed while > 10, :317); the
		 * tick-10 burn sees 10, NOT > 10, so it lands fresh: -5. Tick 11's
		 * lava then deals the excess 4-1=3 (:319-321): -8. */
		golden("C lava host health arbitration", s.player.health,
			MAX_PLAYER_HEALTH - (tick < 10 ? 4 : tick == 10 ? 5 : 8)
				* (HEALTH_PER_HEART / 2));
	}
	put(9, 70, 8, BLOCK_AIR);
}

static void case_below_contracted_body_host(void) {
	put(8, 70, 8, BLOCK_LAVA_STILL);
	reset_player(8.5, 70.70, 8.5);
	server_local_test_update_once(&s);
	golden("D burning-box-only host health", s.player.health,
		MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	golden("D burning-box-only host fire", s.player.fire,
		-PLAYER_FIRE_RESISTANCE + 1);
	put(8, 70, 8, BLOCK_AIR);
}

static void remove_mob(struct entity* mob) {
	uint32_t id = mob->id;
	free(mob);
	dict_entity_erase(s.entities, id);
}

/* The A/B creepers keep ticking under server_local_test_update_once; clear
 * them so a long host case cannot be ended by an unrelated explosion. */
static void remove_all_mobs(void) {
	for(;;) {
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		if(dict_entity_end_p(it))
			return;
		remove_mob(dict_entity_ref(it)->value);
	}
}

/* G6b F4: Entity.onEntityUpdate:203-215 runs the fire cadence FIRST and
 * unconditionally (only `fire > 0` gates it), before lava (:218) and before
 * moveEntity's burning box (:481-487). Standing in a fire block from -20 the
 * box ramps -19..-1, 0 -> 300 on tick 20 (:484-486). Tick 21: cadence
 * 300 % 20 == 0 hits and decrements to 299, the box re-increments to 300 --
 * fire PINS at 300 with a cadence hit each tick; it never climbs to 301. */
static void case_fire_block_host_cadence(void) {
	remove_all_mobs();
	put(8, 69, 8, BLOCK_STONE);
	put(9, 70, 8, BLOCK_AIR);
	reset_player(8.5, 70.05, 8.5);
	for(int tick = 1; tick <= 22; tick++) {
		put(8, 70, 8, BLOCK_FIRE);
		server_local_test_update_once(&s);
		if(tick == 19)
			golden("G fire-block host ramp tick 19", s.player.fire, -1);
		if(tick == 20)
			golden("G fire-block host ignites tick 20", s.player.fire, 300);
		if(tick >= 21)
			golden("G fire-block host pinned at 300", s.player.fire, 300);
	}
	put(8, 70, 8, BLOCK_AIR);
}

/* G6b F4: water with no fire. handleWaterMovement zeroes fire
 * (Entity.java:195), then moveEntity's box is not burning and fire <= 0, so
 * :489-490 resets it to -fireResistance (-20, EntityPlayer.java:50). */
static void case_water_host_resets_fire(void) {
	put(8, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_WATER_STILL);
	put(8, 71, 8, BLOCK_WATER_STILL);
	reset_player(8.5, 70.05, 8.5);
	s.player.fire = 20;
	server_local_test_update_once(&s);
	golden("H water host fire reset", s.player.fire, -PLAYER_FIRE_RESISTANCE);
	put(8, 70, 8, BLOCK_AIR);
	put(8, 71, 8, BLOCK_AIR);
}


/* G6b F1: a fire-immune mob (EntityPigZombie.java:15) in lava. Beta
 * EntityLiving.onEntityUpdate:117 zeroes fire every tick, and
 * Entity.onEntityUpdate:204-207 decays any leftover by 4 BEFORE
 * setEntityFlag(0, fire > 0) (Entity.java:226): the burning flag is never
 * set. setOnFireFromLava and dealFireDamage are no-ops (:235, :522). */
static void case_immune_pigman_in_lava(void) {
	put(8, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_LAVA_STILL);
	vec3 pos = {8.85F, 70.05F, 8.5F};
	struct entity* mob = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE,
		&s);
	assert(mob && mob->tick_server);
	const int health = mob->health;
	for(int tick = 1; tick <= 4; tick++) {
		mob->pos[0] = 8.85F;
		mob->pos[1] = 70.05F;
		mob->pos[2] = 8.5F;
		mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
		test_mob_sync(mob); /* motion units S2 */
		mob->on_ground = true;
		assert(!mob->tick_server(mob, &s));
		golden("E immune pigman in lava on_fire",
			mob->data.monster.fire_time > 0, 0);
		golden("E immune pigman in lava health", mob->health, health);
	}
	remove_mob(mob);
	put(9, 70, 8, BLOCK_AIR);
}

/* G6b F2: Entity.java:489-490 `else if(fire <= 0) fire = -fireResistance`
 * with the default fireResistance = 1 (Entity.java:52). A mob idling in air
 * rests at -1, so one fire-block tick increments -1 -> 0 -> 300 (:484-486),
 * and after leaving the fire it keeps burning down 299, 298 (:208-213). */
static void case_mob_fire_resistance_idle(void) {
	put(8, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_AIR);
	struct entity* mob = spawn_creeper(8.5, 70.0, 8.5);
	static const int expected[] = {-1, 300, 299, 298};
	static const char* names[] = {"F mob air tick -> -fireResistance",
		"F mob fire-block tick -> 300", "F mob keeps burning 299",
		"F mob keeps burning 298"};
	for(int tick = 0; tick < 4; tick++) {
		put(8, 70, 8, tick == 1 ? BLOCK_FIRE : BLOCK_AIR);
		mob->pos[0] = 8.5F;
		mob->pos[1] = 70.0F;
		mob->pos[2] = 8.5F;
		mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
		test_mob_sync(mob); /* motion units S2 */
		mob->on_ground = true;
		assert(!mob->tick_server(mob, &s));
		golden(names[tick], mob->data.monster.fire_time, expected[tick]);
	}
	golden("F mob still on fire after leaving", mob->data.monster.fire_time > 0,
		1);
	remove_mob(mob);
	put(8, 70, 8, BLOCK_AIR);
}

/* G6c: dedicated-server order (Astra ruling). MinecraftServer.doTick:333
 * updateEntities() ticks mobs first; the player's EntityLiving.onUpdate
 * (lava 4 -> :151 --heartsLife -> burning box) runs later, from
 * NetServerHandler.handleFlying:91 via doTick:337. A slime touching the
 * player (EntitySlime.onCollideWithPlayer -> attackEntityFrom(this, 2)) in
 * the same tick as a lava contact therefore lands FRESH, with knockback
 * (EntityLiving.java:325-345), and the lava 4 then applies only its excess
 * 4-2 = 2 (:317-321): same total, but the melee is not absorbed. */
static void case_melee_before_lava_same_tick(void) {
	remove_all_mobs();
	struct server_chunk* chunk = NULL;
	for(w_coord_t cx = -2; cx <= 2; cx++)
		for(w_coord_t cz = -2; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &chunk)
				|| server_world_is_chunk_loaded(&s.world, cx, cz));
	for(int x = 6; x <= 9; x++)
		put(x, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_AIR);
	put(7, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_LAVA_STILL);
	reset_player(8.85, 70.0, 8.5);
	vec3 spos = {8.05F, 70.0F, 8.5F};
	struct entity* sl = server_local_spawn_monster(spos, MONSTER_SLIME_MEDIUM,
		&s);
	assert(sl && sl->tick_server);
	/* The spawn centres the body; pin it clear of the lava cell (medium box
	 * half-width 0.6 -> x <= 8.65) but inside 0.6*size = 1.2 reach. */
	sl->pos[0] = 8.05F;
	sl->pos[1] = 70.0F;
	sl->pos[2] = 8.5F;
	sl->vel[0] = sl->vel[1] = sl->vel[2] = 0.0F;
	test_mob_sync(sl); /* motion units S2 */
	sl->on_ground = true;
	server_local_test_update_once(&s);
	golden("M melee+lava host health", s.player.health,
		MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	golden("M melee lands with knockback (x moved or vel_x > 0)",
		s.player.x > 8.85 || s.player.vel_x > 0.0F, 1);
	golden("M lava then applies only the excess (last_damage 4 pts)",
		s.player.last_damage, 4 * (HEALTH_PER_HEART / 2));
	remove_mob(sl);
	put(9, 70, 8, BLOCK_AIR);
}

/* G6c: the exact visible tick for an immune mob. A daylight roll
 * (EntityZombie.onLivingUpdate:12-16, inherited by EntityPigZombie:45) sets
 * fire = 300 in onLivingUpdate, AFTER that tick's onEntityUpdate already
 * published its flag. The NEXT tick's Entity.onEntityUpdate decays 300 -> 296
 * (:204-207) and publishes setEntityFlag(0, 296 > 0) (:226) before
 * EntityLiving.onEntityUpdate:117 zeroes fire in the same call; the tick after
 * that publishes false. Seed the post-ignition state (fire_time 300) under a
 * roof (no new roll can fire) and read the published flag, e->on_fire. */
static void case_immune_flag_tick_after_ignition(void) {
	remove_all_mobs();
	put(8, 69, 8, BLOCK_STONE);
	put(8, 70, 8, BLOCK_AIR);
	put(8, 71, 8, BLOCK_AIR);
	put(8, 72, 8, BLOCK_STONE);
	vec3 pos = {8.5F, 70.0F, 8.5F};
	struct entity* mob = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE,
		&s);
	assert(mob && mob->tick_server);
	mob->data.monster.fire_time = MOB_FIRE_TICKS; /* last tick's ignition */
	mob->on_fire = false; /* last tick published before the roll */
	static const int expected[] = {1, 0, 0};
	static const char* names[] = {"P immune flag tick after ignition",
		"P immune flag next tick", "P immune flag stays off"};
	for(int tick = 0; tick < 3; tick++) {
		mob->pos[0] = 8.5F;
		mob->pos[1] = 70.0F;
		mob->pos[2] = 8.5F;
		mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
		test_mob_sync(mob); /* motion units S2 */
		mob->on_ground = true;
		assert(!mob->tick_server(mob, &s));
		golden(names[tick], mob->on_fire, expected[tick]);
		golden("P immune fire zeroed same call", mob->data.monster.fire_time > 0,
			0);
	}
	remove_mob(mob);
	put(8, 72, 8, BLOCK_AIR);
}

int main(void) {
	test_fixture_dir("test_lava_aabb_golden_tmp", fixture, sizeof fixture);
	assert(clin_init());
	assert(svin_init());
	struct thread worker;
	assert(thread_create(&worker, drain, NULL, 64));
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	/* Pin the shared World.rand stream: with the clock seed (time(NULL) | 1)
	 * the creeper's tick-1 draws on it (spawn/AI) decide whether
	 * moveEntity's burning-box +1 lands in case A, which then read fire 600
	 * instead of 601 (22.5% of seeds 1..399; 0/780 seeds in the
	 * 11:14-11:40 window, 7/57 in the 14:00 hour -- time-of-day dependent).
	 * Same class as 24aca0d6 / 90795096. No assertion or expected value
	 * changed. */
	s.rand_src.seed = rand_test_scenario_seed(0x1A7AAB01u); /* pinned; CW_TEST_SEED_FORCE replays */
	s.rand_src.has_next_gaussian = false;
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &chunk) && chunk);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.weather.rain_strength = s.weather.prev_rain_strength = 0.0F;

	case_side_overlap_mob();
	case_below_contracted_body_mob();
	case_side_overlap_host_control();
	case_below_contracted_body_host();
	case_fire_block_host_cadence();
	case_water_host_resets_fire();
	case_melee_before_lava_same_tick();
	case_immune_pigman_in_lava();
	case_immune_flag_tick_after_ignition();
	case_mob_fire_resistance_idle();

	draining = 0;
	thread_join(&worker);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures != 0) {
		fprintf(stderr, "test_lava_aabb_golden: FAIL (%d assertions)\n",
			failures);
		return 1;
	}
	puts("test_lava_aabb_golden: PASS");
	return 0;
}
