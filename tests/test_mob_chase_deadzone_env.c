/*
	Reproduces + guards the mob-AI pathfinding DEAD-ZONE bug (filed by the entity
	lane, task_958b34f1, while developing test_wolf_follow_sit.c's LOS-gate test --
	see that file's "chasing this close to a target sits inside a genuine dead
	zone" comment, which flagged this as "an unrelated engine bug, out of scope").

	ROOT CAUSE [740c583: EntityCreature.updatePlayerActionState]: Beta's node-
	arrival radius is `width * 2.0F` (path_follow_steer / entity_monster.c's
	arrive_sq, cited verbatim from the decompile), independent of
	MOB_ATTACK_REACH (2.0F, EntityMob.attackEntity's `f < 2.0F`) and the wolf's
	own tighter 1.5F bite gate (entity_wolf_melee_in_range). For any mob whose
	width*2 EXCEEDS its own melee reach -- the WOLF (0.8 wide -> 1.6 arrival vs
	1.5F bite reach) and the SPIDER (1.4 wide -> 2.8 arrival vs the shared 2.0F
	reach) on this roster -- EntityCreature's recalc-gate re-acquires a fresh
	chase path the SAME tick the old one empties (`pathToEntity != null &&
	rand.nextInt(20) != 0` short-circuits to the unconditional re-acquire branch
	whenever pathToEntity is null and the mob still has a target), and that
	fresh path is immediately re-consumed by the SAME arrival check before the
	mob has moved an inch -- so the path empties again, every tick, forever,
	the instant the mob first gets this close. Beta itself relies on
	EntityLiving.onLivingUpdate's UNCONDITIONAL 0.98F moveForward decay --
	applied even on ticks where the arrival loop nulled pathToEntity mid-branch
	and so never got to explicitly reset moveForward=moveSpeed OR explicitly
	zero it via super.updatePlayerActionState() (that reset belongs to a
	DIFFERENT, mutually exclusive branch: "no active path at all this tick") --
	to coast the mob the last fraction of a block into reach. The pre-fix
	CaveWii code (mob_path_follow_tail) hard-zeroed the AI heading on EVERY
	arrival exhaustion, indistinguishable from the true no-path case, which
	killed that residual coast and froze the mob there PERMANENTLY.

	FIX: entity_monster.c's mob_path_follow_tail now decays the existing
	heading by 0.98 (matching Beta's own moveForward/moveStrafing decay) when
	the path empties mid-tick, instead of hard-zeroing it, while still
	hard-zeroing on the genuine "no active path this tick at all" case (the
	1-in-100 skip roll, or never having had a target).

	Harness shape from tests/test_pathfinding_entity_env.c (superflat-flagged
	server_world_create + a hand-placed flat floor, seeded rand_src, real
	m->tick_server(m,&s) driving the actual production path) and
	tests/test_wolf_follow_sit.c (wolf hostility/target-latch setup:
	WOLF_FLAG_ANGRY + wolf_target_id=0 = "attack the player" per
	wolf_target_is_hostile_player).

	Three mobs, one shared instrumented run_chase() helper:
	  - ZOMBIE (width 0.6 -> arrival 1.2 < reach 2.0F): CONTROL. Never enters
	    the dead zone (narrow mobs hit MOB_ATTACK_REACH_SQ's `in_reach` stop
	    before the arrival radius even triggers) -- must already pass
	    unconditionally, proving this fix does not regress the common case.
	  - SPIDER (width 1.4 -> arrival 2.8 > reach 2.0F): the dead-zone mob with
	    the WIDEST gap (0.8 blocks) on this roster. aggro_time is force-armed
	    every tick (spider acquisition is night-gated; the AI mechanism under
	    test is chase/pathing, not day/night light acquisition).
	  - WOLF (width 0.8 -> arrival 1.6 > its own 1.5F bite reach): the dead-zone
	    mob with the NARROWEST gap (0.1 blocks) -- the exact geometry
	    test_wolf_follow_sit.c's comment measured empirically. A wild
	    (untamed), angry wolf hunting the player.

	A pig-following-wheat scenario was in the original bug filing's suggested
	test matrix ("a pig following wheat if that path is shared") -- grepped:
	no wheat/breeding/lure-follow mechanic exists anywhere in entity_monster.c
	or entity.c yet, so there is no such path to share and no test for it here.

	Each run tracks the mob-to-target HORIZONTAL distance every tick (the same
	metric server_tick_monster's own chase_dist_sq/in_reach gate uses) and
	fails loudly, with the stalled distance/tick printed, if MAX_STALLED_WINDOWS
	consecutive WINDOW-tick windows pass with no measurable progress while still
	outside reach -- exactly the permanent-freeze symptom the pre-fix code
	exhibited.

	Verified by hand-reverting entity_monster.c/combat.h/entity_wolf.c to the
	pre-fix state and re-running this exact file: the WOLF sub-test is the
	clean, deterministic repro -- it STALLS at distance 1.7433 (inside the
	generic 2.0F reach, outside its own 1.5F bite gate) for the entire budget
	pre-fix, and REACHES at tick 17 post-fix, every run, same seed. The SPIDER
	sub-test exercises the IDENTICAL fixed code path (path_follow_steer's
	arrival-radius decay) but did not reproduce a clean multi-window freeze in
	this harness even pre-fix across several spawn geometries/seeds tried --
	the spider's raised Wii-playability speed cap (mob_speed_cap_wii; see
	combat.h) and the background 1-in-80 wander-path interleave (a real Beta
	idiosyncrasy EntityCreature.updatePlayerActionState mixes into an active
	chase, see mob_acquire_wander_path's call site) both give it real,
	if partly incidental, forward motion even under the old hard-zero code, so
	it happened to recover within a handful of ticks in every configuration
	tried. The spider sub-test therefore stands as a regression guard on the
	same mechanism/fix, not a second independent fail-before/pass-after proof
	-- the wolf sub-test carries that burden.
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
#include "entity/entity_wolf.h"
#include "entity/pathfinding.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h" // INVENTORY_SIZE
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

extern struct inventory_logic inventory_logic_player;

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 69
#define FEET_Y 70
#define FLOOR_X_MIN 0
#define FLOOR_X_MAX 30
#define FLOOR_Z_MIN 0
#define FLOOR_Z_MAX 15

static struct server_local s;

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void set_block(int x, int y, int z, int type) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = 0}));
}

static void despawn_all_mobs(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MONSTER) {
				uint32_t id = e->id;
				dict_entity_erase(s.entities, id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

// ================================================================================
// Shared instrumented chase runner.
// ================================================================================
#define TICK_BUDGET       4000 // 200s @ 20 TPS -- generous; a healthy mob closes
                                // a single-digit-block gap in well under 1s
#define STALL_WINDOW      50   // 2.5s per progress check
#define MAX_STALLED_WINDOWS 8  // 20s of zero measurable progress -> declare a
                                // permanent freeze, not just a slow approach
#define PROGRESS_EPS      0.01f

// force_aggro: keep e->data.monster.aggro_time pinned so the run measures pure
// chase/pathing behaviour, independent of species-specific acquisition/latch
// timing (spider's day/night gate, the 60-tick MOB_TARGET_RETENTION_TICKS
// patience clock) -- those are separately tested elsewhere (test_mob_target_latch,
// the spider light-gate tests) and are not what this bug is about.
static void run_chase(struct entity* m, float reach, bool force_aggro,
					  const char* label) {
	float dx0 = (float)s.player.x - m->pos[0];
	float dz0 = (float)s.player.z - m->pos[2];
	float dist = sqrtf(dx0 * dx0 + dz0 * dz0);
	float window_start_dist = dist;
	float min_dist = dist;
	int stalled_windows = 0;
	bool reached = false;
	int reached_tick = -1;
	int i;

	for(i = 0; i < TICK_BUDGET; i++) {
		if(force_aggro)
			m->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;

		bool reaped = m->tick_server(m, &s);
		assert(!reaped);

		float dx = (float)s.player.x - m->pos[0];
		float dz = (float)s.player.z - m->pos[2];
		dist = sqrtf(dx * dx + dz * dz);
		if(dist < min_dist)
			min_dist = dist;

		if(!reached && dist < reach) {
			reached = true;
			reached_tick = i;
		}

		if(getenv("CW_DEADZONE_DEBUG") && i < 520 && (i % 20 == 0)) {
			fprintf(stderr,
					"    [%s] tick=%d pos=(%.4f,%.4f,%.4f) vel=(%.4f,%.4f,%.4f) "
					"dir=(%.4f,%.4f) on_ground=%d path.len=%d path.idx=%d "
					"dist=%.4f ai_state=%d wolf_flags=%02x wolf_target_id=%u "
					"aggro=%d\n",
					label, i, (double)m->pos[0], (double)m->pos[1],
					(double)m->pos[2], (double)m->vel[0], (double)m->vel[1],
					(double)m->vel[2], (double)m->data.monster.direction[0],
					(double)m->data.monster.direction[1], m->on_ground,
					m->data.monster.path.length, m->data.monster.path.index,
					(double)dist, m->ai_state, m->data.monster.wolf_flags,
					m->data.monster.wolf_target_id, m->data.monster.aggro_time);
		}

		if((i + 1) % STALL_WINDOW == 0) {
			if(!reached && window_start_dist - dist < PROGRESS_EPS) {
				stalled_windows++;
			} else {
				stalled_windows = 0;
			}
			window_start_dist = dist;
		}

		if(reached || stalled_windows > MAX_STALLED_WINDOWS)
			break;
	}

	printf("  %s: %s at tick %d (final dist %.4f, min dist %.4f, reach %.4f, "
		   "stalled_windows=%d, path.length=%d)\n",
		   label, reached ? "REACHED" : "STALLED", reached ? reached_tick : i,
		   (double)dist, (double)min_dist, (double)reach, stalled_windows,
		   m->data.monster.path.length);

	if(stalled_windows > MAX_STALLED_WINDOWS)
		fprintf(stderr,
				"FAIL: %s stalled at distance %.4f (reach %.4f, gap %.4f) for "
				"%d consecutive %d-tick windows with no measurable progress -- "
				"this is the pathfinding dead-zone freeze (width*2 arrival "
				"radius > melee reach; see path_follow_steer/mob_path_follow_tail "
				"in source/entity/entity_monster.c)\n",
				label, (double)dist, (double)reach, (double)(dist - reach),
				stalled_windows, STALL_WINDOW);
	assert(stalled_windows <= MAX_STALLED_WINDOWS
		   && "mob must not freeze permanently just outside its own attack reach");
	assert(reached
		   && "mob must eventually close to its Beta attack/approach distance");
}

// ================================================================================
// (A) ZOMBIE -- CONTROL. Narrow biped (0.6 wide); arrival radius 1.2 < the
// shared 2.0F reach, so `in_reach` always stops it before arrival ever fires.
// Must pass unconditionally, with or without the fix, proving the fix changes
// nothing for the common (narrow-mob) case.
// ================================================================================
static void test_zombie_never_deadzones(void) {
	despawn_all_mobs();
	s.rand_src.seed = 0x2E0B1E01u;
	s.player.x = 20.5;
	s.player.y = (double)FEET_Y;
	s.player.z = 7.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	vec3 pos = {10.5f, (float)FEET_Y, 7.5f}; // 10 blocks out, open flat ground
	struct entity* z = server_local_spawn_monster(pos, MONSTER_ZOMBIE, &s);
	assert(z && z->tick_server);
	z->pos[1] = (float)FEET_Y;
	z->vel[0] = z->vel[1] = z->vel[2] = 0.0f;

	run_chase(z, sqrtf(MOB_ATTACK_REACH_SQ), /*force_aggro=*/true,
			 "zombie (control, width 0.6)");

	despawn_all_mobs();
}

// ================================================================================
// (B) SPIDER -- the widest dead-zone gap on the roster (arrival 2.8 vs reach
// 2.0F), exercising the SAME path_follow_steer arrival-radius fix as the wolf
// sub-test below. Regression guard, not the fail-before/pass-after proof (see
// the file header for why this specific scenario recovered even pre-fix in
// every geometry/seed tried here -- the wolf sub-test is the clean repro).
// ================================================================================
static void test_spider_closes_dead_zone(void) {
	despawn_all_mobs();
	s.rand_src.seed = 0x59D1DE02u;
	s.player.x = 20.5;
	s.player.y = (double)FEET_Y;
	s.player.z = 7.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	vec3 pos = {18.0f, (float)FEET_Y, 7.5f}; // 2.5 blocks out -- spawned
	                                        // DIRECTLY inside the [reach 2.0,
	                                        // arrival 2.8) dead band with zero
	                                        // initial velocity, rather than
	                                        // approaching from farther out
	                                        // (which let the OLD hard-zero
	                                        // code's built-up running speed
	                                        // carry it straight through the
	                                        // band before the freeze could ever
	                                        // set in -- observed empirically).
	                                        // spawn_monster_at (not
	                                        // spawn_monster) so this exact
	                                        // fractional distance survives --
	                                        // the cell-centering variant would
	                                        // round it onto the reach boundary.
	struct entity* spider = server_local_spawn_monster_at(pos, MONSTER_SPIDER, &s);
	assert(spider && spider->tick_server);
	spider->pos[1] = (float)FEET_Y;
	spider->vel[0] = spider->vel[1] = spider->vel[2] = 0.0f;
	// Force past the day/night findPlayerToAttack light gate (EntitySpider
	// only self-acquires when dark) so this run measures chase/pathing, not
	// spider-specific light acquisition -- see run_chase's force_aggro doc.
	spider->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;

	run_chase(spider, sqrtf(MOB_ATTACK_REACH_SQ), /*force_aggro=*/true,
			 "spider (dead-zone, width 1.4, gap 0.8)");

	despawn_all_mobs();
}

// ================================================================================
// (C) WOLF -- the tightest dead-zone gap on the roster (arrival 1.6 vs its own
// 1.5F bite reach, a mere 0.1-block gap) -- the EXACT geometry
// test_wolf_follow_sit.c's LOS-gate test comment measured empirically and
// flagged as "an unrelated engine bug, out of scope for the LOS-gate fix
// here". A wild, angry (untamed) wolf hunting the player
// [740c583: wolf_target_is_hostile_player: target_id==0 && ANGRY && !TAMED].
// ================================================================================
static void test_wolf_closes_dead_zone(void) {
	despawn_all_mobs();
	s.rand_src.seed = 0x0B0BF00Eu;
	s.player.x = 20.5;
	s.player.y = (double)FEET_Y + (double)PLAYER_EYE_HEIGHT;
	s.player.z = 7.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	vec3 pos = {14.5f, (float)FEET_Y, 7.5f}; // 6 blocks out
	struct entity* wolf = server_local_spawn_monster_at(pos, MONSTER_WOLF, &s);
	assert(wolf && wolf->tick_server);
	wolf->vel[0] = wolf->vel[1] = wolf->vel[2] = 0.0f;
	wolf_flag_set(&wolf->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
	wolf->data.monster.wolf_target_id = 0; // hostile-player target (untamed+angry)

	run_chase(wolf, 1.5f, /*force_aggro=*/false, // aggro_time isn't in the
											     // wolf latch formula
			 "wolf (dead-zone, width 0.8, gap 0.1)");

	despawn_all_mobs();
}

int main(void) {
	test_fixture_dir("test_mob_chase_deadzone_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0); // printfs must survive an abort()
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 9797,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.rand_src.seed = 0x51DEB001u;
	// The wolf-bite scenario routes a real hit through server_local_hurt_player,
	// whose armor-mitigation path dereferences s.player.inventory unconditionally
	// (same requirement as test_wolf_follow_sit.c) -- a zeroed default crashes.
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++) {
		sc = NULL;
		assert(server_world_load_chunk(&s.world, cx, 0, &sc) && sc);
	}

	for(int x = FLOOR_X_MIN; x <= FLOOR_X_MAX; x++)
		for(int z = FLOOR_Z_MIN; z <= FLOOR_Z_MAX; z++)
			set_block(x, FLOOR_Y, z, BLOCK_STONE);

	printf("test_mob_chase_deadzone_env: reproducing + guarding the pathfinding "
		   "dead-zone (width*2 arrival radius > melee reach) fix\n");
	test_zombie_never_deadzones();
	test_spider_closes_dead_zone();
	test_wolf_closes_dead_zone();

	printf("test_mob_chase_deadzone_env: PASS -- no mob froze permanently just "
		   "outside its own attack reach\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
