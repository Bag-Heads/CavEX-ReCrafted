/*
	Spider jockey (wolf/jockey parity lane, spider half). Beta 1.7.3 mounts a
	skeleton on 1% of naturally-spawned spiders. Ground truth [740c583]:

	  SpawnerAnimals.creatureSpecificInit:
	    for an EntitySpider, when World.rand.nextInt(100) == 0,
	        an EntitySkeleton is built,
	        placed by setLocationAndAngles at the spawn x, y, z
	            with the spider's rotationYaw and pitch 0.0F,
	        joined to the world (entityJoinedWorld),
	        and mounted on the spider (mountEntity).
	     (end)
	  -- called from performSpawning right after EACH successfully placed
	  spider's own entityJoinedWorld (the pack anchor AND every pack member
	  independently, not once per pack), on the world's shared RNG stream.

	  EntitySpider.getMountedYOffset (OVERRIDES Entity's height*0.75D base):
	    height * 0.75D - 0.5D, in double.
	  Entity.updateRiderPosition:
	    `riddenByEntity.setPosition(posX, posY + getMountedYOffset()
	        + riddenByEntity.getYOffset(), posZ);`
	  Entity.onEntityUpdate / updateRidden's `if (ridingEntity.isDead)
	    ridingEntity = null;` -- auto-dismount on either half's death.

	Coverage (mirrors the test_pig_saddle_env.c S4/S5 pattern for a mob-on-
	mob mount instead of a player-on-mob one):
	  S1 -- the roll itself: seed-derived hit/miss, exactly one RNG draw,
	        hand-computed via rand_gen_int directly (never by running our own
	        higher-level code -- the poisoned-golden rule, CODESTYLE §4.2).
	  S2 -- a MISS consumes the one draw and mounts nothing.
	  S3 -- a HIT spawns a skeleton at the spider's own position/yaw and
	        mounts it (both linkage fields, both directions).
	  S4 -- RIDER FOLLOW: the skeleton is hard-snapped onto the spider every
	        tick at SPIDER_JOCKEY_MOUNTED_Y_OFFSET, velocity zeroed.
	  S5 -- auto-dismount when the MOUNT dies.
	  S6 -- auto-dismount when the RIDER dies (the reverse direction).
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

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

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e)
			free(e);
	}
	dict_entity_reset(s.entities);
}

static size_t count_hostile(int mob_id) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e && e->type == ENTITY_MONSTER && e->delay_destroy != 0
		   && e->data.monster.id == mob_id)
			n++;
	}
	return n;
}

// Lightweight fixture spawn (test_ghast_spawn_env.c's spawn_mob pattern):
// no CRPC_SPAWN_MONSTER, no wander-velocity RNG draw -- so the jockey roll's
// own single draw stays the ONLY thing touching s.rand_src around it.
static struct entity* spawn_fixture(int mob, float x, float y, float z) {
	uint32_t id = server_local_gen_id(&s);
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = calloc(1, sizeof(**pp));
	assert(*pp);
	entity_monster(id, *pp, true, &s.world, mob);
	(*pp)->teleport(*pp, (vec3) {x, y, z});
	assert(server_local_test_entity_join(&s, id));
	return *pp;
}

// Seed search (test_wolf_tame.c's seed_for_roll precedent): find a seed
// whose FIRST rand_gen_int(&r,100) draw is (==0) as requested, hand-computed
// directly against the primitive -- never by calling spider_jockey_roll
// itself (that would be the poisoned-golden trap).
static uint32_t seed_for_roll(int wanted_zero) {
	for(uint32_t seed = 1; seed < 200000; seed++) {
		struct random_gen r = {.seed = seed};
		if((rand_gen_int(&r, 100) == 0) == (wanted_zero != 0))
			return seed;
	}
	assert(!"reachable jockey-roll seed");
	return 0;
}

int main(void) {
	test_fixture_dir(
		"test_spider_jockey_spawn_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	/* ===== S1: the roll itself -- exactly one draw, hand-computed ========= */
	uint32_t miss_seed = seed_for_roll(0);
	struct random_gen miss_check = {.seed = miss_seed};
	assert(rand_gen_int(&miss_check, 100) != 0);
	struct random_gen miss_roll = {.seed = miss_seed};
	assert(!spider_jockey_roll(&miss_roll));
	assert(miss_roll.seed == miss_check.seed
		   && "a miss must consume exactly the one nextInt(100) draw");

	uint32_t hit_seed = seed_for_roll(1);
	struct random_gen hit_check = {.seed = hit_seed};
	assert(rand_gen_int(&hit_check, 100) == 0);
	struct random_gen hit_roll = {.seed = hit_seed};
	assert(spider_jockey_roll(&hit_roll));
	assert(hit_roll.seed == hit_check.seed
		   && "a hit must consume exactly the one nextInt(100) draw");
	puts("  S1: spider_jockey_roll is nextInt(100)==0, exactly one draw");

	/* ===== fixture world ==================================================*/
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	s.render_demo_mob = -1;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);
	int gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data probe;
		if(server_world_get_block(&s.world, 8, y, 8, &probe)
		   && probe.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	const float feet = (float) (gy + 1);
	s.player.x = 30.5; s.player.y = feet + PLAYER_EYE_HEIGHT; s.player.z = 8.5;
	// far from the spider so despawn/aggro logic can't disturb these ticks

	/* ===== S2: a MISS mounts nothing, consumes the one draw ================*/
	struct entity* spider = spawn_fixture(MONSTER_SPIDER, 8.5f, feet, 8.5f);
	assert(spider && spider->data.monster.jockey_rider_id == UINT32_MAX);
	s.rand_src.seed = miss_seed;
	server_local_test_spawn_spider_jockey(&s, spider);
	assert(spider->data.monster.jockey_rider_id == UINT32_MAX);
	assert(count_hostile(MONSTER_SKELETON) == 0);
	assert(s.rand_src.seed == miss_check.seed);
	puts("  S2: a miss mounts nothing and consumes exactly one draw");

	/* ===== S3: a HIT spawns + mounts a skeleton ============================*/
	spider->data.monster.body_yaw = 1.25f; // a non-zero yaw to prove the copy
	s.rand_src.seed = hit_seed;
	server_local_test_spawn_spider_jockey(&s, spider);
	assert(count_hostile(MONSTER_SKELETON) == 1);
	uint32_t skel_id = spider->data.monster.jockey_rider_id;
	assert(skel_id != UINT32_MAX);
	struct entity** skel_pp = dict_entity_get(s.entities, skel_id);
	assert(skel_pp && *skel_pp);
	struct entity* skeleton = *skel_pp;
	assert(skeleton->data.monster.id == MONSTER_SKELETON);
	assert(skeleton->data.monster.jockey_mount_id == spider->id);
	assert(skeleton->pos[0] == spider->pos[0] && skeleton->pos[1] == spider->pos[1]
		   && skeleton->pos[2] == spider->pos[2]
		   && "the skeleton spawns at the spider's OWN final position "
			  "[740c583: creatureSpecificInit's var2/var3/var4]");
	assert(skeleton->data.monster.body_yaw == spider->data.monster.body_yaw
		   && "the skeleton faces the spider's own yaw [740c583: "
			  "var0.rotationYaw]");
	puts("  S3: a hit spawns a skeleton at the spider's position/yaw and "
		 "mounts it (both linkage directions)");

	/* ===== S4: RIDER FOLLOW every tick ====================================*/
	skeleton->vel[0] = 5.0f; skeleton->vel[1] = 5.0f; skeleton->vel[2] = 5.0f;
	bool reaped = spider->tick_server(spider, &s);
	assert(!reaped);
	float expect_y = spider->pos[1] + SPIDER_JOCKEY_MOUNTED_Y_OFFSET;
	assert(skeleton->pos[0] == spider->pos[0]
		   && skeleton->pos[1] == expect_y
		   && skeleton->pos[2] == spider->pos[2]
		   && "the rider is hard-snapped onto the mount every tick "
			  "[740c583: Entity.updateRiderPosition, spider's overridden "
			  "getMountedYOffset]");
	assert(skeleton->vel[0] == 0.0f && skeleton->vel[1] == 0.0f
		   && skeleton->vel[2] == 0.0f
		   && "Entity.updateRidden zeroes the rider's own motion every tick");
	puts("  S4: the rider is force-followed onto the spider every tick "
		 "(velocity zeroed)");

	/* ===== S5: mount death auto-dismounts its rider ========================*/
	spider->health = 0;
	reaped = false;
	for(int i = 0; i < 25 && !reaped; i++)
		reaped = spider->tick_server(spider, &s);
	assert(reaped && "the spider must reap within the 20-tick flop + margin");
	assert(spider->data.monster.jockey_rider_id == UINT32_MAX);
	assert(skeleton->data.monster.jockey_mount_id == UINT32_MAX
		   && "a mount's death must auto-dismount its rider "
			  "[740c583: Entity.onEntityUpdate's ridingEntity.isDead check]");
	puts("  S5: a mount's death auto-dismounts its rider");

	/* ===== S6: rider death auto-clears the mount's rider slot (reverse) ===*/
	clear_entities();
	struct entity* spider2 = spawn_fixture(MONSTER_SPIDER, 8.5f, feet, 8.5f);
	s.rand_src.seed = hit_seed;
	server_local_test_spawn_spider_jockey(&s, spider2);
	assert(spider2->data.monster.jockey_rider_id != UINT32_MAX);
	struct entity** skel2_pp
		= dict_entity_get(s.entities, spider2->data.monster.jockey_rider_id);
	assert(skel2_pp && *skel2_pp);
	struct entity* skeleton2 = *skel2_pp;

	skeleton2->health = 0;
	reaped = false;
	for(int i = 0; i < 25 && !reaped; i++)
		reaped = skeleton2->tick_server(skeleton2, &s);
	assert(reaped && "the skeleton must reap within the 20-tick flop + margin");
	assert(skeleton2->data.monster.jockey_mount_id == UINT32_MAX);
	assert(spider2->data.monster.jockey_rider_id == UINT32_MAX
		   && "a rider's death must clear the mount's rider slot too "
			  "(the same rule, walked in the other direction)");
	puts("  S6: a rider's death auto-clears the mount's rider slot");

	clear_entities();
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_spider_jockey_spawn: PASS");
	return 0;
}
