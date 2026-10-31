/*
	next-gaps-0924 W1F (P10): Beta 1.7.3 difficulty goldens.

	Every expected value below is derived by hand from the decompile
	(mcb173-retromcp/minecraft/src_original/net/minecraft/...), never read back
	from CaveWii:

	  GameSettings.java:41      difficulty default 2 (Normal)
	  GameSettings.java:125     cycle (difficulty + step) & 3
	  GameSettings.java:234,287 persisted game option ("difficulty:<n>")
	  Minecraft.java:1129       world.difficultySetting = options value, every tick
	  Minecraft.java:1151       setAllowedMobSpawns(difficulty > 0, true)
	  SpawnerAnimals.java:47    monster category skipped when hostile spawns off
	  EntityMob.java:22         Peaceful: server-side setEntityDead in onUpdate
	  EntityGhast.java:32       same, in updatePlayerActionState (alive only,
	                            EntityLiving.java:618-624)
	  EntityLightningBolt.java:16  fire only when difficulty >= 2
	  EntityPlayer.java:173     Peaceful regen: health < 20 &&
	                            ticksExisted % 20 * 12 == 0 -> heal(1)
	  EntityLiving.java:296-305 heal(): health>0 only, cap 20,
	                            heartsLife(=hurtResistantTime) = 20/2
	  EntityPlayer.java:366-377 attacker instanceof EntityMob/EntityArrow:
	                            Peaceful 0, Easy dmg/3+1, Hard dmg*3/2;
	                            dmg == 0 -> return false (no i-frame/armor)
	  EntityArrow.java:163      attackEntityFrom(owner, 4): the SHOOTER is
	                            the attacker (skeleton = EntityMob, dispenser =
	                            null)

	CaveWii player health is Beta half-hearts x8 (MAX_PLAYER_HEALTH 160).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
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
#include "block/dispenser_logic.h" // DISPENSER_PROJECTILE_OWNER_SENTINEL
#include "block/piston_moving.h"
#include "config.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "game/difficulty.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"

/* CAVEWII_SERVER_LOCAL_TEST hook (server_local.c): request + latch now. */
void server_local_test_set_difficulty(int d);

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70 /* superflat: first air layer above the surface */

static struct server_local s;
static volatile int g_drain_run = 1;
static int g_failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* Soft check: report every mismatch (so the RED run lists them all), then
 * fail the process at the end. */
#define CHECK(cond, ...)                                                   \
	do {                                                                   \
		if(!(cond)) {                                                      \
			g_failures++;                                                  \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
			printf(__VA_ARGS__);                                           \
			printf("\n");                                                  \
		}                                                                  \
	} while(0)

static const char* const DNAME[] = {"Peaceful", "Easy", "Normal", "Hard"};

/* ---- 1. pure contract (hand tables) ------------------------------------ */
static void test_contract(void) {
	/* GameSettings.java:125: (d + 1) & 3 and (d - 1) & 3. */
	static const int next[] = {1, 2, 3, 0};
	static const int prev[] = {3, 0, 1, 2};
	for(int d = 0; d < 4; d++) {
		CHECK(difficulty_cycle(d, 1) == next[d], "cycle+1 from %d", d);
		CHECK(difficulty_cycle(d, -1) == prev[d], "cycle-1 from %d", d);
		CHECK(strcmp(difficulty_name(d), DNAME[d]) == 0, "name %d", d);
		CHECK(difficulty_sanitize(d) == d, "sanitize %d", d);
	}
	CHECK(DIFFICULTY_DEFAULT == 2, "default must be Normal (GameSettings:41)");
	CHECK(difficulty_sanitize(-1) == 2 && difficulty_sanitize(4) == 2,
		  "invalid -> Normal");

	/* EntityPlayer.java:366-377 by hand, half-hearts:
	 *   raw  : 1  2  4  5  50
	 *   P    : 0  0  0  0  0
	 *   E /3+1: 1  1  2  2  17
	 *   N    : 1  2  4  5  50
	 *   H *3/2: 1  3  6  7  75 */
	static const int raw[] = {1, 2, 4, 5, 50};
	static const int want[4][5] = {
		{0, 0, 0, 0, 0},
		{1, 1, 2, 2, 17},
		{1, 2, 4, 5, 50},
		{1, 3, 6, 7, 75},
	};
	for(int d = 0; d < 4; d++)
		for(int i = 0; i < 5; i++)
			CHECK(difficulty_scale_mob_damage(d, raw[i]) == want[d][i],
				  "scale %s raw %d = %d, want %d", DNAME[d], raw[i],
				  difficulty_scale_mob_damage(d, raw[i]), want[d][i]);

	/* instanceof EntityMob over the roster. */
	static const struct { int id; bool mob; } roster[] = {
		{MONSTER_ZOMBIE, true}, {MONSTER_CREEPER, true},
		{MONSTER_SKELETON, true}, {MONSTER_SPIDER, true},
		{MONSTER_PIG_ZOMBIE, true}, {MONSTER_GIANT, true},
		{MONSTER_SLIME_SMALL, false}, {MONSTER_SLIME_MEDIUM, false},
		{MONSTER_SLIME_BIG, false}, {MONSTER_GHAST, false},
		{MONSTER_WOLF, false}, {MONSTER_PIG, false}, {MONSTER_COW, false},
		{MONSTER_CHICKEN, false}, {MONSTER_SHEEP, false},
		{MONSTER_SQUID, false},
	};
	for(size_t i = 0; i < sizeof(roster) / sizeof(roster[0]); i++) {
		CHECK(monster_is_beta_entity_mob(roster[i].id) == roster[i].mob,
			  "EntityMob class of id %d", roster[i].id);
		/* Peaceful removal: every EntityMob (alive or dying), an ALIVE ghast. */
		bool gone_alive = roster[i].mob || roster[i].id == MONSTER_GHAST;
		CHECK(difficulty_removes_monster(0, roster[i].id, 10) == gone_alive,
			  "peaceful removal alive id %d", roster[i].id);
		CHECK(difficulty_removes_monster(0, roster[i].id, 0) == roster[i].mob,
			  "peaceful removal dying id %d", roster[i].id);
		for(int d = 1; d < 4; d++)
			CHECK(!difficulty_removes_monster(d, roster[i].id, 10),
				  "no removal on %s id %d", DNAME[d], roster[i].id);
	}
	CHECK(!difficulty_allows_hostile_spawns(0), "P: no hostile spawns");
	for(int d = 1; d < 4; d++)
		CHECK(difficulty_allows_hostile_spawns(d), "hostile spawns %d", d);
	CHECK(!difficulty_lightning_ignites(0) && !difficulty_lightning_ignites(1)
			  && difficulty_lightning_ignites(2) && difficulty_lightning_ignites(3),
		  "lightning fire only >= 2 (EntityLightningBolt:16)");

	/* ticksExisted % 20 * 12 == 0 <=> ticksExisted % 20 == 0. */
	for(int t = 0; t <= 100; t++) {
		bool due = (t % 20) == 0;
		CHECK(difficulty_peaceful_regen_due(0, 19, 20, t) == due, "regen t=%d", t);
		CHECK(!difficulty_peaceful_regen_due(2, 19, 20, t), "no regen N t=%d", t);
	}
	CHECK(!difficulty_peaceful_regen_due(0, 20, 20, 20), "full health: no heal");
	CHECK(!difficulty_peaceful_regen_due(0, 0, 20, 20), "dead: no heal");
	puts("  contract: cycle/names/scale/EntityMob/removal/regen tables");
}

/* ---- 2. config persistence ---------------------------------------------- */
static void test_config(void) {
	char path[256];
	snprintf(path, sizeof(path), "%s/difficulty_config.json", TMP_WORLD);
	FILE* f = fopen(path, "wb");
	assert(f);
	fputs("{}", f);
	fclose(f);

	struct config c;
	assert(config_create(&c, path));
	CHECK(config_read_difficulty(&c) == DIFFICULTY_NORMAL, "absent -> Normal");
	for(int d = 0; d < 4; d++) {
		assert(config_set_difficulty(&c, d));
		CHECK(config_read_difficulty(&c) == d, "round trip %d", d);
	}
	/* Invalid stored values fall back to the default. */
	assert(config_set_int(&c, DIFFICULTY_CONFIG_KEY, 7));
	CHECK(config_read_difficulty(&c) == DIFFICULTY_NORMAL, "7 -> Normal");
	assert(config_set_int(&c, DIFFICULTY_CONFIG_KEY, -1));
	CHECK(config_read_difficulty(&c) == DIFFICULTY_NORMAL, "-1 -> Normal");
	assert(json_object_dotset_number(json_object(c.root),
									 DIFFICULTY_CONFIG_KEY, 1.5) == JSONSuccess);
	CHECK(config_read_difficulty(&c) == DIFFICULTY_NORMAL, "1.5 -> Normal");
	assert(config_set_string(&c, DIFFICULTY_CONFIG_KEY, "hard"));
	CHECK(config_read_difficulty(&c) == DIFFICULTY_NORMAL, "string -> Normal");

	/* Persists across a restart: save, drop, reload from disk. */
	assert(config_set_difficulty(&c, DIFFICULTY_PEACEFUL));
	assert(config_save(&c, path));
	config_destroy(&c);
	struct config reloaded;
	assert(config_create(&reloaded, path));
	CHECK(config_read_difficulty(&reloaded) == DIFFICULTY_PEACEFUL,
		  "Peaceful survives save + reload");
	config_destroy(&reloaded);
	puts("  config: default Normal, round trip, invalid -> Normal, restart");
}

/* ---- 3. mob melee (EntityPlayer.attackEntityFrom:366-377) --------------- */
static void reset_player(double x, double y, double z) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	s.player.fire = 0;
	s.player.vel_x = s.player.vel_z = 0.0f;
	s.player.x = x;
	s.player.y = y;
	s.player.z = z;
	for(size_t slot = INVENTORY_SLOT_ARMOR;
		slot < INVENTORY_SLOT_ARMOR + INVENTORY_SIZE_ARMOR; slot++)
		inventory_clear_slot(&s.player.inventory, slot);
}

static void test_melee(void) {
	/* raw half-hearts: zombie 5, pigzombie 5, spider 2, skeleton 2 (EntityMob
	 * default attackStrength), medium slime 2 and wolf 2 (NOT EntityMob). */
	static const struct {
		int id;
		int loss[4]; /* x8 engine units per difficulty P,E,N,H */
	} rows[] = {
		{MONSTER_ZOMBIE, {0, 2 * 8, 5 * 8, 7 * 8}},
		{MONSTER_PIG_ZOMBIE, {0, 2 * 8, 5 * 8, 7 * 8}},
		{MONSTER_SPIDER, {0, 1 * 8, 2 * 8, 3 * 8}},
		{MONSTER_SKELETON, {0, 1 * 8, 2 * 8, 3 * 8}},
		{MONSTER_SLIME_MEDIUM, {2 * 8, 2 * 8, 2 * 8, 2 * 8}},
		{MONSTER_WOLF, {2 * 8, 2 * 8, 2 * 8, 2 * 8}},
	};
	for(size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++)
		for(int d = 0; d < 4; d++) {
			server_local_test_set_difficulty(d);
			reset_player(1000.0, 200.0, 1000.0);
			struct entity attacker = {0};
			attacker.type = ENTITY_MONSTER;
			attacker.data.monster.id = rows[r].id;
			attacker.pos[0] = 999.0f;
			attacker.pos[1] = 200.0f;
			attacker.pos[2] = 1000.0f;
			server_local_hurt_player(&s, &attacker);
			int loss = MAX_PLAYER_HEALTH - s.player.health;
			CHECK(loss == rows[r].loss[d], "melee id %d %s: lost %d, want %d",
				  rows[r].id, DNAME[d], loss, rows[r].loss[d]);
			if(rows[r].loss[d] == 0) {
				/* `if(var2 == 0) return false` precedes super.attackEntityFrom:
				 * no i-frame window, no stored hit, no knockback. */
				CHECK(s.player.hurt_resist_time == 0 && s.player.last_damage == 0
						  && s.player.vel_x == 0.0f && s.player.vel_z == 0.0f,
					  "peaceful melee id %d must not open i-frames/knockback",
					  rows[r].id);
			}
		}
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	puts("  melee: EntityMob scaled P0/E/3+1/N/H*3/2; slime + wolf unscaled");
}

/* ---- 4. arrows (EntityArrow.java:163 passes the SHOOTER) ---------------- */
static uint32_t g_next_id = 7000;
static struct entity* spawn_arrow(vec3 pos, vec3 vel, uint32_t owner,
								  bool from_player, bool from_skeleton) {
	uint32_t id = g_next_id++;
	struct entity** e_ptr = dict_entity_safe_get(s.entities, id);
	*e_ptr = malloc(sizeof(struct entity));
	struct entity* e = *e_ptr;
	assert(e);
	entity_arrow(id, e, true, &s.world, owner, ITEM_ARROW, ARROW_DAMAGE,
				 from_player);
	e->data.arrow.from_skeleton = from_skeleton; /* the flush's fire-time stamp */
	e->teleport(e, pos);
	glm_vec3_copy(vel, e->vel);
	return e;
}

static void test_arrows(void) {
	/* Skeleton arrow: raw 4 -> P 0, E 4/3+1=2, N 4, H 4*3/2=6 half-hearts.
	 * Dispensed arrow: owner null -> unscaled 4 at every difficulty. */
	static const int skel_loss[4] = {0, 2 * 8, 4 * 8, 6 * 8};
	for(int d = 0; d < 4; d++) {
		for(int kind = 0; kind < 2; kind++) {
			bool skeleton = kind == 0;
			server_local_test_set_difficulty(d);
			reset_player(8.0, Y + PLAYER_EYE_HEIGHT, 30.0);
			vec3 pos = {8.0F, (float)Y + 0.85F, 28.5F};
			vec3 vel = {0.0F, 0.0F, 2.0F};
			struct entity* a = skeleton
				? spawn_arrow(pos, vel, 777u, false, true)
				: spawn_arrow(pos, vel, DISPENSER_PROJECTILE_OWNER_SENTINEL,
							  true, false);
			float vz0 = a->vel[2];
			bool consumed = a->tick_server(a, &s);
			int loss = MAX_PLAYER_HEALTH - s.player.health;
			int want = skeleton ? skel_loss[d] : 4 * 8;
			CHECK(loss == want, "%s arrow %s: lost %d, want %d",
				  skeleton ? "skeleton" : "dispenser", DNAME[d], loss, want);
			if(want == 0) {
				/* attackEntityFrom returned false: the arrow BOUNCES
				 * (EntityArrow.java:163-176 else-branch), no i-frame. */
				CHECK(!consumed && a->vel[2] < vz0
						  && s.player.hurt_resist_time == 0,
					  "peaceful skeleton arrow must bounce without i-frames");
			} else {
				CHECK(consumed, "accepted arrow hit must be consumed (%s)",
					  DNAME[d]);
			}
			if(!consumed) {
				struct entity** pp = dict_entity_get(s.entities, a->id);
				if(pp && *pp) {
					uint32_t id = a->id;
					free(*pp);
					dict_entity_erase(s.entities, id);
				}
			}
		}
	}
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	s.player.has_pos = false;
	s.player.finished_loading = false;
	puts("  arrows: skeleton shooter scaled (P bounces), dispenser unscaled");
}

/* ---- 4b. creeper blast (Explosion.java:100 passes the exploder) --------
 * Explosion.doExplosionA:100 each entity takes attackEntityFrom(exploder, damage):
 * a creeper (EntityCreeper extends EntityMob) is the attacker of its own
 * blast, so EntityPlayer.attackEntityFrom:366-377 scales the blast damage by
 * difficulty exactly like a melee hit; TNT, beds and fireballs pass null and
 * stay unscaled. Peaceful: dmg == 0 -> return false BEFORE
 * the base attackEntityFrom (no i-frames, no knockBack), but Explosion:101-103
 * still adds the motion impulse (unconditional after the attack call).
 * Geometry as test_explosion_beta_golden (h): player 4 blocks +x of the
 * centre at the centre's height, open air (density 1), power 3 (size2 6):
 * ratio 2/3, v 1/3, damage (int)((1/9+1/3)/2*8*6+1) = 11 half-hearts,
 * impulse 1/3. Scaled: P 0, E 11/3+1 = 4, N 11, H 11*3/2 = 16. */
static void test_creeper_blast(void) {
	static const int loss[4] = {0, 4 * 8, 11 * 8, 16 * 8};
	const float cy = (float)Y + 6.0F; /* everything above the superflat top */
	vec3 cpos = {8.5F, cy, 40.5F};
	struct entity* creeper = server_local_spawn_monster_at(cpos, MONSTER_CREEPER, &s);
	assert(creeper && creeper->health == 20);
	server_local_refresh_entity_membership(&s, creeper);
	for(int d = 0; d < 4; d++) {
		server_local_test_set_difficulty(d);
		/* server_player.y IS Beta posY (eye level): put the eye on the
		 * centre's height so the distance is exactly 4. */
		reset_player(12.5, (double)cy, 40.5);
		vec3 boom = {creeper->pos[0], creeper->pos[1], creeper->pos[2]};
		server_world_begin_update(&s.world);
		bool fired = server_world_try_explode_by(&s.worlds[0], creeper, boom, 3.0F);
		CHECK(fired, "%s: creeper blast must not be deferred", DNAME[d]);
		int got = MAX_PLAYER_HEALTH - s.player.health;
		CHECK(got == loss[d], "creeper blast %s: lost %d, want %d", DNAME[d],
			  got, loss[d]);
		/* The exploder never enters its own entity pass. */
		CHECK(creeper->health == 20, "%s: creeper must not hit itself", DNAME[d]);
		if(loss[d] == 0) {
			/* attackEntityFrom returned false: no i-frame window, no stored
			 * hit, no knockBack -- only the blast's own +x impulse (1/3). */
			CHECK(s.player.hurt_resist_time == 0 && s.player.last_damage == 0,
				  "peaceful creeper blast must not open i-frames");
			CHECK(fabs((double)s.player.vel_x - 1.0 / 3.0) < 1.0e-4
					  && s.player.vel_z == 0.0f,
				  "peaceful creeper blast: vel_x %.5f, want impulse 1/3 only",
				  (double)s.player.vel_x);
		} else {
			/* Fresh hit: knockBack 0.4 away from the creeper + impulse 1/3. */
			CHECK(fabs((double)s.player.vel_x - (KNOCKBACK_H + 1.0 / 3.0)) < 1.0e-4,
				  "%s creeper blast: vel_x %.5f, want 0.4 + 1/3", DNAME[d],
				  (double)s.player.vel_x);
		}
	}
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	s.player.has_pos = false;
	s.player.finished_loading = false;
	s.player.vel_x = s.player.vel_z = 0.0f;
	puts("  creeper blast: exploder is an EntityMob -> scaled P0/E/3+1/N/H*3/2; impulse kept");
}

/* ---- 5. Peaceful removal through the real entity pass ------------------- */
static void clear_entities(void) {
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it);
		dict_entity_next(it))
		free(dict_entity_ref(it)->value);
	dict_entity_reset(s.entities);
}

static struct entity* pinned(int id, float x, float z) {
	struct entity* e = server_local_spawn_monster((vec3) {x, (float)Y, z}, id, &s);
	assert(e);
	return e;
}

static bool present(uint32_t id) {
	struct entity** pp = dict_entity_get(s.entities, id);
	return pp && *pp;
}

static void test_removal(void) {
	static const int ids[] = {
		MONSTER_ZOMBIE, MONSTER_CREEPER, MONSTER_SKELETON, MONSTER_SPIDER,
		MONSTER_PIG_ZOMBIE, MONSTER_GHAST, MONSTER_SLIME_BIG, MONSTER_WOLF,
		MONSTER_PIG,
	};
	const size_t n = sizeof(ids) / sizeof(ids[0]);
	for(int d = 0; d < 4; d++) {
		clear_entities();
		server_local_test_set_difficulty(d);
		/* Player far away (no aggro), but the mobs' +/-32 area is resident. */
		reset_player(8.0, Y + 40 + PLAYER_EYE_HEIGHT, 8.0);
		/* Loaded chunks are -2..2, so a +/-32 area is complete only for
		 * 0 <= x,z <= 15 (World.updateEntityWithOptionalForce freezes the
		 * rest -- and a frozen entity is never removed, see below). */
		uint32_t handle[16];
		for(size_t i = 0; i < n; i++)
			handle[i] = pinned(ids[i], (float)(2 + (int)i), 4.0F)->id;
		/* A DYING ghast (health 0) is not reached by the ghast check. */
		struct entity* dying_ghast = pinned(MONSTER_GHAST, 8.0F, 12.0F);
		/* An EntityMob outside the complete area is not updated, so Beta
		 * never runs its onUpdate removal either. */
		uint32_t frozen_id = pinned(MONSTER_ZOMBIE, -12.0F, 4.0F)->id;
		dying_ghast->health = 0;
		uint32_t dying_id = dying_ghast->id;

		assert(server_local_test_tick_entities(&s));
		for(size_t i = 0; i < n; i++) {
			bool gone_expected = d == 0
				&& (monster_is_beta_entity_mob(ids[i]) || ids[i] == MONSTER_GHAST);
			CHECK(present(handle[i]) == !gone_expected,
				  "%s: monster id %d %s after one tick", DNAME[d], ids[i],
				  present(handle[i]) ? "present" : "removed");
		}
		CHECK(present(dying_id), "%s: dying ghast must finish its flop",
			  DNAME[d]);
		CHECK(present(frozen_id), "%s: a frozen (not updated) zombie stays",
			  DNAME[d]);
	}
	clear_entities();
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	puts("  removal: Peaceful removes EntityMob + live ghast; slime/wolf/pig stay");
}

/* ---- 6. hostile spawn pass skipped on Peaceful (no draws) --------------- */
static void test_spawn_gate(void) {
	for(int d = 0; d < 4; d++) {
		clear_entities();
		server_local_test_set_difficulty(d);
		reset_player(8.0, Y + PLAYER_EYE_HEIGHT, 8.0);
		s.world_time = 18000; /* midnight */
		s.rand_src.seed = 0x5EED1234u;
		s.rand_src.has_next_gaussian = false;
		uint32_t before = s.rand_src.seed;
		server_local_test_spawn_mobs(&s);
		bool drew = s.rand_src.seed != before;
		CHECK(drew == (d != 0),
			  "%s: hostile spawn pass %s rand_src (Peaceful must skip the "
			  "whole monster category)",
			  DNAME[d], drew ? "drew from" : "did not touch");
		if(d == 0) {
			size_t n = 0;
			dict_entity_it_t it;
			for(dict_entity_it(it, s.entities); !dict_entity_end_p(it);
				dict_entity_next(it))
				n++;
			CHECK(n == 0, "Peaceful spawned %zu monsters", n);
		}
	}
	clear_entities();
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	s.player.has_pos = false;
	s.player.finished_loading = false;
	puts("  spawn: Peaceful skips the hostile pass entirely");
}

/* ---- 7. Peaceful regen (EntityPlayer.java:173) -------------------------- */
static void test_regen(void) {
	for(int d = 0; d < 4; d++) {
		server_local_test_set_difficulty(d);
		reset_player(8.0, Y + PLAYER_EYE_HEIGHT, 8.0);
		s.player.ticks_existed = 0; /* a fresh EntityPlayer */
		s.player.health = 96;       /* 12 half-hearts */
		bool ok = true;
		for(int t = 1; t <= 60; t++) {
			if(t == 40)
				s.player.hurt_resist_time = 5; /* inside a hurt window */
			server_local_tick_actor_regen(&s);
			/* heals on ticksExisted 20, 40, 60 on Peaceful only */
			int want = 96 + (d == 0 ? 8 * (t / 20) : 0);
			if(s.player.health != want) {
				ok = false;
				CHECK(false, "%s tick %d: health %d, want %d", DNAME[d], t,
					  s.player.health, want);
				break;
			}
			if(d == 0 && t == 40)
				/* heal(): heartsLife = heartsHalvesLife / 2 = 10, which is
				 * NOT > 10 -> the hurt window is over (engine counter 0). */
				CHECK(s.player.hurt_resist_time == 0,
					  "heal must close the hurt window (got %d)",
					  s.player.hurt_resist_time);
		}
		CHECK(s.player.ticks_existed == 60, "ticks_existed advanced %d",
			  s.player.ticks_existed);
		(void)ok;
	}
	/* Cap and dead body. */
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL);
	reset_player(8.0, Y + PLAYER_EYE_HEIGHT, 8.0);
	s.player.ticks_existed = 19;
	s.player.health = MAX_PLAYER_HEALTH - 3;
	server_local_tick_actor_regen(&s);
	CHECK(s.player.health == MAX_PLAYER_HEALTH, "heal caps at max (%d)",
		  s.player.health);
	s.player.ticks_existed = 19;
	server_local_tick_actor_regen(&s);
	CHECK(s.player.health == MAX_PLAYER_HEALTH, "full health unchanged");
	s.player.ticks_existed = 19;
	s.player.health = 0;
	server_local_tick_actor_regen(&s);
	CHECK(s.player.health == 0, "a corpse is not healed");
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	s.player.has_pos = false;
	s.player.finished_loading = false;
	puts("  regen: Peaceful +1 half-heart every 20 ticksExisted, closes i-frames");
}

/* ---- 8. lightning fire gate (EntityLightningBolt.java:16) --------------- */
static void test_lightning(void) {
	for(int d = 0; d < 4; d++) {
		server_local_test_set_difficulty(d);
		reset_player(1000.0, Y, 1000.0);
		const int bx = -8 + 4 * d, bz = -8;
		for(int dx = -2; dx <= 2; dx++)
			for(int dz = -2; dz <= 2; dz++) {
				struct block_data stone = {.type = BLOCK_STONE};
				struct block_data air = {.type = BLOCK_AIR};
				server_world_set_block(&s.worlds[0], bx + dx, Y - 1, bz + dz, stone);
				for(int dy = 0; dy < 4; dy++)
					server_world_set_block(&s.worlds[0], bx + dx, Y + dy, bz + dz, air);
			}
		s.rand_src.seed = 0xABCD1234u;
		struct random_gen world_before = s.rand_src;
		/* CFR EntityLightningBolt.java:35-36,46-47: own Random;
		 * nextLong + lifetime precede the difficulty-gated offset draws. */
		struct java_random mirror = {0};
		entity_lifetime_rng_seed(&mirror, s.worlds[0].seed, s.worlds[0].dimension,
			s.next_entity_id + 1);
		(void)jrand_next_long(&mirror);
		int living_time = jrand_next_int_bound(&mirror, 3) + 1;
		if(difficulty_lightning_ignites(d))
			for(int i = 0; i < 12; i++)
				(void)jrand_next_int_bound(&mirror, 3); /* fire-offset draws */
		world_lightning_bolt_spawn(&s, bx, Y, bz);
		struct block_data at = {0};
		assert(server_world_get_block(&s.world, bx, Y, bz, &at));
		bool fire = at.type == BLOCK_FIRE;
		CHECK(fire == (d >= 2), "%s: strike cell %s fire", DNAME[d],
			  fire ? "has" : "has no");
		CHECK(memcmp(&world_before, &s.rand_src, sizeof(world_before)) == 0,
			  "%s: bolt construction must preserve the world stream", DNAME[d]);
		CHECK(s.weather.active_bolts_count == 1,
			  "%s: constructor must retain the bolt", DNAME[d]);
		if(s.weather.active_bolts_count == 1) {
			CHECK(s.weather.active_bolts[0].gameplay_rng.seed == mirror.seed,
				  "%s: own constructor draws match the gated Beta order", DNAME[d]);
			CHECK(s.weather.active_bolts[0].bolt_living_time == living_time,
				  "%s: lifetime roll precedes the fire gate", DNAME[d]);
		}
		s.weather.active_bolts_count = 0;
	}
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	puts("  lightning: fire (and its draws) only on Normal/Hard");
}

int main(void) {
	test_fixture_dir("test_difficulty_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	puts("test_difficulty_golden: Beta 1.7.3 difficulty (next-gaps-0924 W1F)");
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0xD1FF0924u;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -2; cx <= 2; cx++)
		for(w_coord_t cz = -2; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	/* The static default is the Beta default before anything requests. */
	CHECK(server_local_difficulty(&s) == DIFFICULTY_NORMAL,
		  "server default must be Normal");

	test_contract();
	test_config();
	test_melee();
	test_arrows();
	test_creeper_blast();
	test_removal();
	test_spawn_gate();
	test_regen();
	test_lightning();

	clear_entities();
	inventory_destroy(&s.player.inventory, &s.player);
	g_drain_run = 0;
	thread_join(&drain);
	if(g_failures) {
		printf("test_difficulty_golden: FAIL (%d mismatches)\n", g_failures);
		return 1;
	}
	puts("test_difficulty_golden: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
