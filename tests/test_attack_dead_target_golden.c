/* Copyright (c) 2026 CaveWii_v2 contributors
 * laneC/C-DEADTARGET (2026-09-26): creatures stop attacking a dead target.
 *
 * THE BETA RULE (decompile, ../mcb173-retromcp/minecraft/src_original/
 * net/minecraft/src/):
 *   EntityCreature.updatePlayerActionState:19-33
 *       if(playerToAttack == null) {            // acquire tick
 *           playerToAttack = findPlayerToAttack(); ...getPathToEntity...
 *       } else if(!playerToAttack.isEntityAlive()) {
 *           playerToAttack = null;              // DROP tick
 *       } else { ... attackEntity / attackBlockedEntity ... }
 *   EntityLiving.isEntityAlive:575-577  = !isDead && health > 0
 *   EntityMob.findPlayerToAttack:28-31  = getClosestPlayerToEntity(16) + LOS;
 *   World.getClosestPlayer:2166-2179 has NO alive test, and a dead SP player
 *   is never removed from playerEntities (World.updateEntities:1211-1224 only
 *   drops it from loadedEntityList), so the corpse is RE-ACQUIRED on the next
 *   tick and DROPPED again on the one after: the target alternates
 *   drop / re-acquire, and attackEntity is reached on NEITHER tick.
 *   EntitySkeleton.attackEntity:34-53 is the only arrow spawn, EntityMob.
 *   attackEntity:49-55 the only attackTime=20 reset, EntitySpider.
 *   attackEntity:41-59 / EntityWolf.attackEntity:294-313 the only leap rolls,
 *   EntityCreeper.attackEntity:89-117 the only fuse increment -- so a dead
 *   target is never shot, swung at, pounced on or primed against.
 *   EntityCreeper.onUpdate:63-69: on the DROP tick (playerToAttack == null)
 *   a banked fuse deflates by one; on the re-acquire tick nothing touches it.
 *
 * WHAT THIS FILE PINS (the real server_tick_monster, one mob at a time):
 *   1. control: a skeleton with a LIVE host in range and LOS fires.
 *   2. skeleton, dead host: zero arrows over 200 ticks, and the Beta
 *      drop/re-acquire alternation (AI_IDLE with the latch cleared on the drop
 *      tick -> Beta's playerToAttack==null wander-roll branch; AI_CHASE on the
 *      re-acquire tick -> Beta's held-target path branch).
 *   3. creeper, dead host, primed at 2 blocks: deflates one step on every
 *      drop tick, holds on every re-acquire tick, never detonates.
 *   4. zombie / angry pig zombie, dead host in reach: attack_time is never
 *      re-armed (EntityMob.attackEntity never runs).
 *   5. spider / angry wolf, dead host pinned in the 2..6 leap band: never
 *      pounces (control: the same pin with a live host does).
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "network/inventory_logic.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_tmp_world[128];
static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* Superflat: bedrock 0, dirt 1-2, grass 3 -> a standing mob's feet at y 4. */
#define FEET_Y 4.0f
#define PX 24.5
#define PZ 24.5

static void host_at(double x, double z, bool alive) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = x;
	s.player.y = FEET_Y + PLAYER_EYE_HEIGHT;
	s.player.z = z;
	s.player.health = alive ? MAX_PLAYER_HEALTH : 0;
	s.player.hurt_resist_time = 0;
}

static struct entity* mob_at(int id, float x, float z) {
	vec3 pos = {x, FEET_Y, z};
	struct entity* e = server_local_spawn_monster(pos, id, &s);
	assert(e && e->tick_server);
	e->pos[0] = x;
	e->pos[1] = FEET_Y;
	e->pos[2] = z;
	e->vel[0] = e->vel[1] = e->vel[2] = 0.0f;
	e->on_ground = true;
	test_mob_sync(e); /* motion units S2: the fixture pins the doubles */
	return e;
}

static void mob_remove(struct entity* e) {
	uint32_t id = e->id;
	free(e);
	dict_entity_erase(s.entities, id);
}

/* One authoritative monster tick; returns the arrows it queued. */
static int tick_once(struct entity* e, bool* removed) {
	s.pending_arrow_count = 0;
	bool rm = e->tick_server(e, &s);
	if(removed)
		*removed = rm;
	int n = s.pending_arrow_count;
	s.pending_arrow_count = 0;
	return n;
}

static void test_skeleton_live_control(void) {
	host_at(PX, PZ, true);
	struct entity* sk = mob_at(MONSTER_SKELETON, (float)PX - 6.0f, (float)PZ);
	int arrows = 0;
	for(int t = 0; t < 60 && !arrows; t++)
		arrows += tick_once(sk, NULL);
	assert(arrows > 0); /* control: the harness really can see a shot */
	mob_remove(sk);
	printf("  skeleton live control: fired\n");
}

static void test_skeleton_dead_host(void) {
	host_at(PX, PZ, false);
	struct entity* sk = mob_at(MONSTER_SKELETON, (float)PX - 6.0f, (float)PZ);
	/* Held target at tick start (Beta playerToAttack != null), bow ready. */
	sk->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
	sk->ai_timer = 0.0f;
	int arrows = 0;
	for(int t = 1; t <= 8; t++) {
		arrows += tick_once(sk, NULL);
		if(t & 1) { /* DROP tick: EntityCreature:24-25 */
			assert(sk->data.monster.aggro_time == 0);
			assert(sk->ai_state == AI_IDLE);
		} else { /* RE-ACQUIRE tick: EntityCreature:19-23 */
			assert(sk->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS - 1);
			assert(sk->ai_state == AI_CHASE);
		}
	}
	for(int t = 9; t <= 200; t++)
		arrows += tick_once(sk, NULL);
	assert(arrows == 0);
	mob_remove(sk);
	printf("  skeleton dead host: 0 arrows / 200 ticks, drop/re-acquire alternation\n");
}

static void test_creeper_dead_host(void) {
	host_at(PX, PZ, false);
	struct entity* cr = mob_at(MONSTER_CREEPER, (float)PX - 2.0f, (float)PZ);
	cr->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
	cr->data.monster.fuse = 10;
	cr->data.monster.creeper_state = 1;
	/* drop: 9, hold: 9, drop: 8, hold: 8 ... (EntityCreeper.onUpdate:63-69) */
	static const int expect[6] = {9, 9, 8, 8, 7, 7};
	for(int t = 0; t < 6; t++) {
		bool rm = false;
		tick_once(cr, &rm);
		assert(!rm);
		assert(cr->data.monster.fuse == expect[t]);
		assert(cr->data.monster.creeper_state == -1);
	}
	for(int t = 6; t < 120; t++) {
		bool rm = false;
		tick_once(cr, &rm);
		assert(!rm); /* never detonates next to a corpse */
		assert(cr->data.monster.fuse < CREEPER_FUSE_TICKS);
		assert(!cr->data.monster.explosion_pending);
	}
	mob_remove(cr);
	printf("  creeper dead host: deflates on drop ticks, never detonates\n");
}

static void test_melee_dead_host(int id) {
	host_at(PX, PZ, false);
	struct entity* m = mob_at(id, (float)PX - 1.0f, (float)PZ);
	m->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
	if(id == MONSTER_PIG_ZOMBIE)
		m->data.monster.anger_level = 400;
	m->data.monster.attack_time = 0;
	m->data.monster.attack_windup = 0;
	for(int t = 0; t < 60; t++) {
		tick_once(m, NULL);
		assert(m->data.monster.attack_time == 0); /* attackTime never re-armed */
	}
	assert(s.player.health == 0);
	mob_remove(m);
	printf("  %s dead host: no swing\n",
		   id == MONSTER_ZOMBIE ? "zombie" : "pig zombie");
}

/* Pin the mob 4 blocks west of the host, grounded and at rest, before every
 * tick so it stays in the 2..6 band where Beta rolls nextInt(10). */
static int count_leaps(int id, bool alive, int ticks) {
	host_at(PX, PZ, alive);
	struct entity* m = mob_at(id, (float)PX - 4.0f, (float)PZ);
	if(id == MONSTER_WOLF)
		wolf_flag_set(&m->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
	int leaps = 0;
	for(int t = 0; t < ticks; t++) {
		/* horizontal pin only: y / vel[1] / on_ground stay physical */
		m->pos[0] = (float)PX - 4.0f;
		m->pos[2] = (float)PZ;
		m->vel[0] = m->vel[2] = 0.0f;
		test_mob_sync(m);
		m->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
		tick_once(m, NULL);
		/* Motion units S2: the pounce (motionY = (double)0.4F) is consumed by this
		 * tick's move, then gravity: it ends at (0.4F - 0.08D) * 0.98F > 0; a
		 * grounded non-leap tick ends negative. */
		if(m->data.monster.motion_d.motion[1] > 0.0)
			leaps++;
		if(alive)
			s.player.health = MAX_PLAYER_HEALTH; /* keep the control alive */
	}
	mob_remove(m);
	return leaps;
}

static void test_leapers(void) {
	assert(count_leaps(MONSTER_SPIDER, true, 150) > 0);  /* control */
	assert(count_leaps(MONSTER_SPIDER, false, 150) == 0);
	assert(count_leaps(MONSTER_WOLF, true, 150) > 0);    /* control */
	assert(count_leaps(MONSTER_WOLF, false, 150) == 0);
	printf("  spider / angry wolf dead host: no pounce (live controls pounce)\n");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_attack_dead_target_golden_tmp", s_tmp_world,
					 sizeof s_tmp_world);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(s_tmp_world);
	MKDIR(test_fixture_join(s_tmp_world, "/region"));
	remove(test_fixture_join(s_tmp_world, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_tmp_world);
	memset(&s, 0, sizeof(s));
	/* Overworld-only fixture: the one live world context. */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	/* Midnight: no undead sunburn, and the spider is hostile (light <= 10). */
	s.world_time = 18000;
	s.skylight_cached = false;
	s.rand_src.seed = 12345u; /* fixed stream: deterministic run */
	s.rand_src.has_next_gaussian = false;
	dict_entity_init(s.entities);
	/* the wolf's looksWithInterest reads the host's held item */
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	printf("test_attack_dead_target_golden:\n");
	test_skeleton_live_control();
	test_skeleton_dead_host();
	test_creeper_dead_host();
	test_melee_dead_host(MONSTER_ZOMBIE);
	test_melee_dead_host(MONSTER_PIG_ZOMBIE);
	test_leapers();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_attack_dead_target_golden\n");
	test_fixture_dir_cleanup(s_tmp_world);
	return 0;
}
