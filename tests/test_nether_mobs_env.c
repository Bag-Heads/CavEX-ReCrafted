/*
	Batch 1b (2026-08-07 hardware-video caveats): Nether-native mobs + the
	dimension-split hostile spawn table, over the live superflat harness.

	Ground truth [740c583]:
	  - BiomeGenHell clears every spawn list and adds ONLY ghast(10) +
	    pig-zombie(10) -- the video's Overworld skeleton in the Nether was the
	    dimension-blind table.
	  - EntityPigZombie: neutral until a player hit; attackEntityFrom loops
	    every pig zombie in expand(32,32,32) through becomeAngryAt (angerLevel
	    = 400+rand(400), randomSoundDelay = rand(40)) BEFORE super runs, and
	    b1.7.3 NEVER decrements angerLevel; moveSpeed 0.95F with a target vs
	    0.5F; fire-immune; drops cooked porkchop; persists "Anger".
	  - EntityGhast: EntityFlying (no gravity), waypoint wander, attackCounter
	    10 = charge (fire texture > 10), 20 = EntityFireball + reset -40;
	    fire-immune; drops gunpowder.
	  - EntityFireball: accel = dir/|dir|*0.1 each tick then *0.95 drag, zero
	    direct hit damage, strength-1.0 FLAMING explosion on any impact,
	    punch-deflection sets motion = attacker look + accel = look*0.1.
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
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "world.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_nether_mobs_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70

static struct server_local s;
static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, uint8_t type, uint8_t meta) {
	bool changed = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = (uint8_t)(meta & 0x0F)});
	if(!changed) {
		struct block_data live;
		assert(server_world_get_block(&s.worlds[0], x, y, z, &live));
		assert(live.type == type);
	}
}

static int gid(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}

static struct entity* find_type(enum entity_type t) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e && e->type == t && e->delay_destroy != 0)
			return e;
	}
	return NULL;
}

static size_t count_type(enum entity_type t) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e && e->type == t && e->delay_destroy != 0)
			n++;
	}
	return n;
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

static struct entity* spawn_mob(int mob, float x, float y, float z) {
	uint32_t id = server_local_gen_id(&s);
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(**pp));
	assert(*pp);
	entity_monster(id, *pp, true, &s.worlds[0], mob);
	(*pp)->teleport(*pp, (vec3) {x, y, z});
	assert(server_local_test_entity_join(&s, id));
	return *pp;
}

int main(void) {
	test_fixture_dir("test_nether_mobs_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	/* ============ S1: the dimension-split hostile spawn table ============ */
	{
		/* Overworld column bit-identical to the historic table: total 50,
		 * walk order zombie/creeper/skeleton/spider/slime. */
		assert(monster_spawn_total_weight_dim(WORLD_DIM_OVERWORLD) == 50);
		assert(monster_spawn_total_weight() == 50); /* legacy wrapper == dim 0 */
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 0) == MONSTER_ZOMBIE);
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 9) == MONSTER_ZOMBIE);
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 10) == MONSTER_CREEPER);
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 20) == MONSTER_SKELETON);
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 30) == MONSTER_SPIDER);
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 40) == MONSTER_SLIME_SMALL);
		assert(monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, 49) == MONSTER_SLIME_SMALL);
		for(int r = 0; r < 50; r++) {
			int m = monster_spawn_pick_dim(WORLD_DIM_OVERWORLD, r);
			assert(m == monster_spawn_pick(r)); /* wrapper equivalence */
			assert(m != MONSTER_PIG_ZOMBIE && m != MONSTER_GHAST
				   && "Nether natives must never enter the overworld table");
		}
		/* Nether column = BiomeGenHell verbatim: ONLY ghast+pigman, 10/10. */
		assert(monster_spawn_total_weight_dim(WORLD_DIM_NETHER) == 20);
		for(int r = 0; r < 20; r++) {
			int m = monster_spawn_pick_dim(WORLD_DIM_NETHER, r);
			assert((m == MONSTER_PIG_ZOMBIE || m == MONSTER_GHAST)
				   && "the Nether table holds ONLY ghast + pig zombie");
			assert(m != MONSTER_SKELETON && m != MONSTER_ZOMBIE
				   && "the video's Nether-skeleton bug");
		}
		assert(monster_spawn_pick_dim(WORLD_DIM_NETHER, 0) == MONSTER_PIG_ZOMBIE);
		assert(monster_spawn_pick_dim(WORLD_DIM_NETHER, 9) == MONSTER_PIG_ZOMBIE);
		assert(monster_spawn_pick_dim(WORLD_DIM_NETHER, 10) == MONSTER_GHAST);
		assert(monster_spawn_pick_dim(WORLD_DIM_NETHER, 19) == MONSTER_GHAST);
		/* Def-table invariants the spawn pass relies on. */
		assert(monster_def_get(MONSTER_PIG_ZOMBIE)->fire_immune);
		assert(monster_def_get(MONSTER_GHAST)->fire_immune);
		assert(monster_def_get(MONSTER_PIG_ZOMBIE)->no_light_gate);
		assert(monster_def_get(MONSTER_GHAST)->no_light_gate);
		assert(monster_def_get(MONSTER_PIG_ZOMBIE)->drop == ITEM_PORKCHOP_COOKED);
		assert(monster_def_get(MONSTER_GHAST)->drop == ITEM_GUNPOWDER);
		assert(monster_def_get(MONSTER_GHAST)->box_w == 4.0f
			   && monster_def_get(MONSTER_GHAST)->box_h == 4.0f);
		assert(mob_melee_damage(MONSTER_PIG_ZOMBIE) == 40); /* strength 5 * 8 */
		assert(mob_melee_damage(MONSTER_GHAST) == 0); /* no contact melee */
		puts("  S1: overworld table byte-stable; Nether = BiomeGenHell 10/10");
	}

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 777,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x1234567u;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	/* The entity scenarios occupy chunk (0,0). Beta's entity activation gate
	 * additionally requires the complete position +/-32 block neighborhood. */
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	/* A stone platform for the AI sections. */
	for(int x = 0; x <= 15; x++)
		for(int z = 0; z <= 15; z++)
			set(x, Y - 1, z, BLOCK_STONE, 0);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 8.5;
	s.player.y = Y + PLAYER_EYE_HEIGHT;
	s.player.z = 2.5;
	s.player.health = MAX_PLAYER_HEALTH;

	/* ============ S2: pig zombie neutrality + group anger ================= */
	{
		struct entity* near_pz = spawn_mob(MONSTER_PIG_ZOMBIE, 8.5f, Y, 8.5f);
		struct entity* pack_pz = spawn_mob(MONSTER_PIG_ZOMBIE, 12.5f, Y, 8.5f);
		/* A third pigman far outside the 32-box must stay calm. Chunk (-1,*)
		 * is resident (loaded above), x = -8.5 -> 17 blocks + change... use
		 * a >32 offset inside resident chunks: z = 8.5 + 33 is chunk z=2 --
		 * NOT resident. Distance is checked per-axis (Beta expand box), so
		 * x-distance alone can exceed 32 only beyond resident chunks; use the
		 * y axis instead (the box is expand(32,32,32) -- 3D). */
		struct entity* far_pz = spawn_mob(MONSTER_PIG_ZOMBIE, 8.5f, Y + 40.0f,
										  8.5f);
		assert(near_pz->data.monster.anger_level == 0);
		assert(pack_pz->data.monster.anger_level == 0);

		/* Neutral: several server ticks near the player -> no latch, no swing. */
		for(int t = 0; t < 10; t++) {
			server_world_begin_update(&s.worlds[0]);
			assert(server_local_test_tick_entities(&s));
		}
		assert(near_pz->data.monster.aggro_time == 0
			   && "a calm pig zombie must not acquire");
		assert(s.player.health == MAX_PLAYER_HEALTH
			   && "a calm pig zombie must not melee");

		/* Punch the near one: BOTH nearby pigmen anger (400..799 + sound
		 * delay 0..39); the 40-up one stays calm. Re-teleport the far one
		 * NOW: it fell (gravity) during the neutral ticks above, and the
		 * expand(32,32,32) box is evaluated at punch time. */
		far_pz->teleport(far_pz, (vec3) {8.5f, Y + 40.0f, 8.5f});
		/* GB-06: the authority gate (func_6006_a: canEntityBeSeen &&
		 * distSq < 36.0, STRICT) now rejects out-of-reach punches -- and the
		 * section's player baseline (z 2.5) is exactly 6.0 blocks from the
		 * spawn column BEFORE the calm pigman wander-drifts even farther.
		 * Step next to the mob's PUNCH-TIME position (drift-proof), as the
		 * real Beta server handler would require of a real player. */
		s.player.x = near_pz->pos[0];
		s.player.z = near_pz->pos[2] - 3.0;
		struct server_rpc atk = {
			.type = SRPC_ENTITY_ATTACK,
			.payload.entity_attack.entity_id = near_pz->id,
		};
		server_local_test_process_rpc(&s, &atk);
		s.player.x = 8.5; /* restore the section baseline */
		s.player.z = 2.5;
		assert(near_pz->data.monster.anger_level >= 400
			   && near_pz->data.monster.anger_level <= 799);
		assert(pack_pz->data.monster.anger_level >= 400
			   && pack_pz->data.monster.anger_level <= 799);
		assert(near_pz->data.monster.anger_sound_delay >= 0
			   && near_pz->data.monster.anger_sound_delay <= 39);
		assert(far_pz->data.monster.anger_level == 0
			   && "outside expand(32,32,32): stays calm");
		assert(near_pz->data.monster.aggro_time > 0 && "angry -> latched");
		puts("  S2: neutral until hit; group anger 400..799 within the 32-box");

		/* Anger survives the archive (Beta's own \"Anger\" short). */
		int saved_anger = near_pz->data.monster.anger_level;
		assert(saved_anger > 0);
		server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
		test_drain_quiesce();
		assert(server_world_save_chunk(&s.worlds[0], true, 0, 0));
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
		struct entity* back = NULL;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(e && e->type == ENTITY_MONSTER
			   && e->data.monster.id == MONSTER_PIG_ZOMBIE
			   && e->data.monster.anger_level == saved_anger)
				back = e;
		}
		assert(back && "Anger must round-trip the chunk archive");
		puts("  S2: Anger survives the disk round trip");
		clear_entities();
	}

	/* ============ S3: ghast flight + attack cycle + fireball ============= */
	{
		s.player.x = 8.5;
		s.player.y = Y + PLAYER_EYE_HEIGHT;
		s.player.z = 2.5;
		struct entity* ghast = spawn_mob(MONSTER_GHAST, 8.5f, Y + 6.0f, 10.5f);
		/* Flight: with EntityFlying physics the ghast must NOT free-fall.
		 * (Its waypoint steering may drift it slowly; gravity at 2.0/tick
		 * would drop it >5 blocks in 10 ticks.) */
		float y0 = ghast->pos[1];
		for(int t = 0; t < 10; t++) {
			server_world_begin_update(&s.worlds[0]);
			assert(server_local_test_tick_entities(&s));
		}
		assert(ghast->pos[1] > y0 - 2.0f && "a ghast must not free-fall");

		/* Attack cycle: LOS to the player is open; force the counter near the
		 * trigger and tick once -- the fireball entity must appear (via the
		 * post-tick flush) and the counter must reset to -40. */
		ghast->data.monster.ghast_attack_counter = 19;
		server_world_begin_update(&s.worlds[0]);
		assert(server_local_test_tick_entities(&s));
		assert(ghast->data.monster.ghast_attack_counter == -40
			   && "fireball launch resets the counter to -40");
		struct entity* fb = find_type(ENTITY_FIREBALL);
		assert(fb && "the launch must spawn ENTITY_FIREBALL");
		/* Acceleration points from the ghast toward the player (z shrinks). */
		assert(fb->data.fireball.accel[2] < 0.0f);
		float an = sqrtf(fb->data.fireball.accel[0] * fb->data.fireball.accel[0]
						 + fb->data.fireball.accel[1] * fb->data.fireball.accel[1]
						 + fb->data.fireball.accel[2] * fb->data.fireball.accel[2]);
		assert(fabsf(an - 0.1f) < 1.0e-3f && "|accel| == 0.1 (Beta)");
		puts("  S3: no free-fall; counter 20 -> fireball spawn + reset -40");

		/* Deflection [EntityFireball.attackEntityFrom]: punching the fireball
		 * re-aims it along the player look (rx=0, ry=0 -> +Z). */
		struct server_rpc atk = {
			.type = SRPC_ENTITY_ATTACK,
			.payload.entity_attack.entity_id = fb->id,
		};
		s.player.rx = 0.0f;
		s.player.ry = 0.0f;
		/* Move the fireball into punch reach first. */
		fb->teleport(fb, (vec3) {8.5f, Y + 1.0f, 4.5f});
		server_local_test_process_rpc(&s, &atk);
		assert(fb->vel[2] > 0.9f && "deflection sets motion = look (+Z)");
		assert(fb->data.fireball.accel[2] > 0.09f
			   && "deflection sets accel = look * 0.1");
		puts("  S3: punch-deflection re-aims motion + accel");
		clear_entities();
	}

	/* ============ S4: fireball impact = flaming explosion ================ */
	{
		/* Aim a fireball straight down at the platform far from the player;
		 * on impact the strength-1.0 flaming explosion must detonate. Beta
		 * observable (next-gaps-0924 G2, decompile-literal): power 1 never
		 * breaks stone (resistance 30), and the fire doExplosionA:107-122 lit
		 * over the crater is removed again by doExplosionB:157-160 (the lit
		 * cells stay in destroyedBlockPositions), so the neighbourhood is
		 * byte-identical afterwards. The detonation is pinned through the
		 * explosion cost record instead: the block collection ran. */
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		float down[3] = {0.0f, -1.0f, 0.0f};
		entity_fireball(id, *pp, true, &s.worlds[0], 0, down);
		(*pp)->teleport(*pp, (vec3) {3.5f, Y + 2.0f, 12.5f});
		glm_vec3_copy((vec3) {0.0f, -1.0f, 0.0f}, (*pp)->vel);
		assert(server_local_test_entity_join(&s, id));
		assert(gid(3, Y - 1, 12) == BLOCK_STONE);
		for(int t = 0; t < 6 && count_type(ENTITY_FIREBALL) > 0; t++) {
			server_world_begin_update(&s.worlds[0]);
			assert(server_local_test_tick_entities(&s));
		}
		assert(count_type(ENTITY_FIREBALL) == 0
			   && "the fireball must die on impact");
		struct server_world_explosion_cost cost;
		server_world_explosion_last_cost(&cost);
		assert(cost.rays > 0 && cost.cells > 0
			   && "the flaming explosion must have detonated (block collection)");
		for(int dx = -2; dx <= 2; dx++)
			for(int dz = -2; dz <= 2; dz++) {
				assert(gid(3 + dx, Y - 1, 12 + dz) == BLOCK_STONE
					   && "power 1 cannot break stone");
				assert(gid(3 + dx, Y, 12 + dz) != BLOCK_FIRE
					   && "the blast's own fire is removed by doExplosionB");
			}
		puts("  S4: fireball impact detonates the flaming explosion");
		clear_entities();
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	puts("test_nether_mobs_env: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
