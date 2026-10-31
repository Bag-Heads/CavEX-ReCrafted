/* Goldens: [740c583: EntityWolf.getPathOrWalkableBlock/interact/onUpdate]. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
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
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h" // MAX_PLAYER_HEALTH
#include "network/server_world.h"
#include "platform/thread.h"
#include "wolf_test_common.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_wolf_follow_sit_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct inventory_logic inventory_logic_player;

struct query_log { int calls; int xyz[8][3]; };
static bool normal_cube(int x, int y, int z, void* context) {
	struct query_log* q = context;
	if(q->calls < 8) {
		q->xyz[q->calls][0] = x;
		q->xyz[q->calls][1] = y;
		q->xyz[q->calls][2] = z;
	}
	q->calls++;
	return y == 63 && x == 8 && z == 9;
}

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

static void set_block(struct server_local* s, int x, int y, int z, int type) {
	assert(server_world_set_block(
		&s->worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = 0}));
}

// PARITY FIX 2026-09-02: EntityCreature.updatePlayerActionState dispatches
// attackEntity() ONLY under canEntityBeSeen(playerToAttack) (decompile-verified,
// 740c583:EntityCreature.java: `if (this.canEntityBeSeen(this.playerToAttack))
// this.attackEntity(...) else this.attackBlockedEntity(...)`), and EntityWolf's
// own attackEntity override (implementing BOTH the leap band 2..6 and the bite
// <1.5 [740c583: EntityWolf.attackEntity]) never re-checks LOS itself -- the
// base-class dispatch IS the gate. Before this fix, entity_monster.c's wolf
// leap/bite blocks had no LOS term at all (unlike the zombie/spider melee and
// spider leap, which already AND `can_see`/mob_can_see_player into their
// dispatch), so a wolf bit/leapt straight through a solid wall.
//
// Proves BOTH halves against a real server world with an actual stone wall:
//   1. BITE (deterministic, no RNG): a hostile-player-targeting wolf 1.45
//      blocks from the player, wall between -- must NOT hit (health unchanged,
//      windup re-armed); the SAME geometry with the wall removed MUST hit.
//   2. LEAP (RNG-gated, 1-in-10/eligible tick): the same wolf held in the 2..6
//      leap band for 600 grounded, aggro'd ticks -- with the wall present the
//      leap must NEVER fire (p(0 hits | unblocked) ~= (0.9)^600, indistinguish-
//      able from proof); with the wall removed the identical setup DOES leap,
//      proving the negative result above is the LOS gate, not an unreachable
//      band/aggro condition.
static void test_wolf_attack_los_gate(struct server_local* s) {
	// Room clear of the follow/teleport geometry above (this file's other
	// tests never touch the world) and inside the loaded chunk range (chunks
	// (0,0)/(1,0)/(0,1)/(1,1) -> world x,z in [0,32)).
	const int X0 = 14, X1 = 26;   // room x in [X0, X1)
	const int WALL_X = 20;       // the LOS-blocking partition
	const int Z = 5;             // fixed z for wolf, player, and the wall
	const int ZLO = 3, ZHI = 8;  // a 5-wide floor -- the pathfinder needs real
								  // room either side of a single-file line, not
								  // a 1-tile tightrope over open void (a bare
								  // 1-wide ribbon left mob_acquire_chase_path
								  // unable to find any path at all: path.length
								  // stayed 0 and the wolf never moved a step).
	const int YF = 69;           // floor
	const int WALL_Y0 = 70, WALL_Y1 = 74; // wall spans feet..well-above-eye

	for(int x = X0; x < X1; x++)
		for(int z = ZLO; z < ZHI; z++)
			set_block(s, x, YF, z, BLOCK_STONE);
	for(int y = WALL_Y0; y < WALL_Y1; y++)
		for(int z = ZLO; z < ZHI; z++)
			set_block(s, WALL_X, y, z, BLOCK_STONE);

	s->player.has_pos = true;
	s->player.finished_loading = true;

	// --- 1. BITE: wolf 1.45 blocks from the player, wall between -------------
	// Freeze the wolf's own chase MOVEMENT (wolf_shake_active=true forces
	// `latched=false` in the is_wolf override, entity_monster.c) so this test
	// can pin an exact, unmoving geometry rather than fight it: chasing this
	// close to a target sits inside a genuine dead zone (< wolf width*2, here
	// 1.6 blocks) where mob_acquire_chase_path's best-effort search always
	// returns a path whose every waypoint already satisfies
	// path_follow_steer's own arrival radius, so the path is exhausted (and
	// cleared) in the SAME tick it is computed, tick after tick -- observed
	// empirically while developing this test (nailed down by tracing
	// path.length/pos across hundreds of ticks) and flagged separately; it is
	// an unrelated engine bug, out of scope for the LOS-gate fix here.
	// wolf_shake_active does NOT gate the melee block itself (only the leap
	// block below requires !wolf_shake_active, and the leap sub-test uses its
	// own per-tick position pin instead), so it is a clean, surgical way to
	// hold the wolf still without touching the thing under test.
	{
		// The player MUST sit at or past the wall's far face (x >= WALL_X+1):
		// embedding the player's point INSIDE the wall's own cell made an
		// earlier version of this test a false negative -- the DDA LOS walk
		// stops at the destination cell without testing ITS occupancy, so a
		// destination point inside solid rock reads as "clear" (the ray never
		// passes THROUGH a solid cell before arriving, it merely arrives
		// inside one). Kept outside it, the wall is a genuine obstruction the
		// ray must cross.
		s->player.x = (double)WALL_X + 1.0; // 21.0, exactly at the far face
		s->player.y = (double)YF + 1.0 + (double)PLAYER_EYE_HEIGHT;
		s->player.z = (double)Z + 0.5;
		s->player.health = MAX_PLAYER_HEALTH;
		s->player.hurt_resist_time = 0;

		// wolf half-width 0.4 (width 0.8): edge at 19.95, 0.05 clear of the
		// wall's near face at 20.0 -- not embedded, so no collision push-out.
		// Distance to the player: 21.0 - 19.55 = 1.45, under the STRICT
		// 1.5-block bite gate (entity_wolf_melee_in_range's `< 1.5F`); with
		// movement frozen this stays EXACT for the whole run.
		vec3 wpos = {(float)WALL_X - 0.45f, (float)YF + 1.0f, (float)Z + 0.5f}; // 19.55
		struct entity* wolf = server_local_spawn_monster_at(wpos, MONSTER_WOLF, s);
		assert(wolf && wolf->tick_server);
		wolf_flag_set(&wolf->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
		wolf->data.monster.wolf_target_id = 0; // hostile-player target
		wolf->data.monster.wolf_shake_active = true; // freeze chase movement
		wolf->vel[0] = wolf->vel[1] = wolf->vel[2] = 0.0f; // clear the random spawn kick
		test_mob_sync(wolf); /* motion units S2: the fixture pins the doubles */

		const int RUN_TICKS = 20; // several times MOB_ATTACK_WINDUP_TICKS (8);
								  // stays under the ~40-tick auto-clear of a
								  // (never-armed) wolf_shake_active
		for(int i = 0; i < RUN_TICKS; i++) {
			bool reaped = wolf->tick_server(wolf, s);
			assert(!reaped);
		}
		printf("  wolf bite, LOS BLOCKED (%d ticks): player health %d "
			   "(expect unchanged %d), wolf x=%.3f windup=%d\n",
			   RUN_TICKS, s->player.health, MAX_PLAYER_HEALTH,
			   (double)wolf->pos[0], wolf->data.monster.attack_windup);
		assert(s->player.health == MAX_PLAYER_HEALTH);
		assert(fabsf(wolf->pos[0] - wpos[0]) < 1.0e-4f
			   && "movement must stay frozen (wolf_shake_active) for this "
				  "geometry to hold");
		// !in_reach keeps re-arming the windup every walled tick.
		assert(wolf->data.monster.attack_windup == MOB_ATTACK_WINDUP_TICKS);

		// Remove the wall: the IDENTICAL wolf, at the IDENTICAL distance,
		// now bites within RUN_TICKS -- proving the negative result above
		// came from LOS, not the distance/height/windup gates.
		for(int y = WALL_Y0; y < WALL_Y1; y++)
			for(int z = ZLO; z < ZHI; z++)
				set_block(s, WALL_X, y, z, BLOCK_AIR);
		int i;
		for(i = 0; i < RUN_TICKS && s->player.health == MAX_PLAYER_HEALTH; i++) {
			bool reaped = wolf->tick_server(wolf, s);
			assert(!reaped);
		}
		printf("  wolf bite, LOS CLEAR (bit after %d ticks): player health %d "
			   "(expect %d)\n",
			   i, s->player.health, MAX_PLAYER_HEALTH - 16);
		// server_local_hurt_player's mob_melee_damage(MONSTER_WOLF) == 16.
		assert(s->player.health == MAX_PLAYER_HEALTH - 16);

		// Re-seal the wall for the leap sub-test below.
		for(int y = WALL_Y0; y < WALL_Y1; y++)
			for(int z = ZLO; z < ZHI; z++)
				set_block(s, WALL_X, y, z, BLOCK_STONE);
	}

	// --- 2. LEAP: wolf held in the 2..6 band, wall between -------------------
	{
		s->rand_src.seed = 0x0B0BF00Du; // fixed, nonzero (xorshift needs nonzero)

		s->player.x = (double)(WALL_X + 3) + 0.5; // 23.5: 3.6 blocks from the wolf
		s->player.y = (double)YF + 1.0 + (double)PLAYER_EYE_HEIGHT;
		s->player.z = (double)Z + 0.5;
		s->player.health = MAX_PLAYER_HEALTH;

		vec3 wpos = {(float)WALL_X - 1.0f - 0.5f, (float)YF + 1.0f, (float)Z + 0.5f}; // x=18.5
		struct entity* wolf = server_local_spawn_monster_at(wpos, MONSTER_WOLF, s);
		assert(wolf && wolf->tick_server);
		jrand_set_seed(&wolf->gameplay_rng, (int64_t)(uint64_t)0x0B0BF00Du);
		wolf_flag_set(&wolf->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
		wolf->data.monster.wolf_target_id = 0;

		int leaps = 0;
		const int TICKS = 600;
		for(int i = 0; i < TICKS; i++) {
			wolf->pos[0] = wpos[0]; wolf->pos[1] = wpos[1]; wolf->pos[2] = wpos[2];
			wolf->vel[0] = 0.0f; wolf->vel[1] = -0.1f; wolf->vel[2] = 0.0f;
			test_mob_sync(wolf);
			bool reaped = wolf->tick_server(wolf, s);
			assert(!reaped);
			/* motion units S2: a leap tick ends at (0.4F - 0.08D) * 0.98F > 0 */
			if(wolf->data.monster.motion_d.motion[1] > 0.0)
				leaps++;
		}
		printf("  wolf leap, LOS BLOCKED: %d/%d in-band ticks leapt "
			   "(expect 0)\n", leaps, TICKS);
		assert(leaps == 0);
		assert(s->player.health == MAX_PLAYER_HEALTH); // never hurt either

		// Remove the wall: the IDENTICAL band/aggro/seed setup DOES leap --
		// proves the band/aggro/RNG plumbing is genuinely reachable and the
		// zero above is the LOS gate, not a mis-tuned test fixture.
		for(int y = WALL_Y0; y < WALL_Y1; y++)
			for(int z = ZLO; z < ZHI; z++)
				set_block(s, WALL_X, y, z, BLOCK_AIR);
		s->rand_src.seed = 0x0B0BF00Du; // same world setup; mob replay is below
		struct entity* wolf2 = server_local_spawn_monster_at(wpos, MONSTER_WOLF, s);
		assert(wolf2 && wolf2->tick_server);
		jrand_set_seed(&wolf2->gameplay_rng, (int64_t)(uint64_t)0x0B0BF00Du);
		wolf_flag_set(&wolf2->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
		wolf2->data.monster.wolf_target_id = 0;

		leaps = 0;
		for(int i = 0; i < TICKS; i++) {
			wolf2->pos[0] = wpos[0]; wolf2->pos[1] = wpos[1]; wolf2->pos[2] = wpos[2];
			wolf2->vel[0] = 0.0f; wolf2->vel[1] = -0.1f; wolf2->vel[2] = 0.0f;
			test_mob_sync(wolf2);
			bool reaped = wolf2->tick_server(wolf2, s);
			assert(!reaped);
			/* motion units S2: a leap tick ends at (0.4F - 0.08D) * 0.98F > 0 */
			if(wolf2->data.monster.motion_d.motion[1] > 0.0)
				leaps++;
		}
		printf("  wolf leap, LOS CLEAR: %d/%d in-band ticks leapt "
			   "(expect > 0)\n", leaps, TICKS);
		assert(leaps > 0);

		// Re-seal for cleanliness (no further geometry tests follow).
		for(int y = WALL_Y0; y < WALL_Y1; y++)
			for(int z = ZLO; z < ZHI; z++)
				set_block(s, WALL_X, y, z, BLOCK_STONE);
	}
}

int main(void) {
	test_fixture_dir("test_wolf_follow_sit_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0); // unbuffered: printfs must survive an abort()
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

	struct server_local s = {0};
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 5150, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	// Real armor inventory: server_local_hurt_player's armor-mitigation path
	// (server_local_apply_player_armor) dereferences s.player.inventory
	// unconditionally, so the wolf-bite LOS test below (which routes through
	// that same function) needs a real one, not the zeroed default.
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	struct entity* wolf = wolf_test_make(&s, 1, MONSTER_WOLF);
	wolf->data.monster.wolf_flags = WOLF_FLAG_TAMED;
	strcpy(wolf->data.monster.wolf_owner, WOLF_LOCAL_PLAYER_NAME);
	struct item_data empty = {0};
	// [740c583: EntityWolf.interact] sit-toggle branch is setWolfSitting +
	// isJumping=false + setPathToEntity(null) ONLY -- no motionY write, so a
	// wolf toggled mid-jump/mid-fall keeps its existing vertical momentum.
	wolf->vel[1] = 0.37F;
	assert(entity_wolf_interact(wolf, &s, &empty) == WOLF_INTERACT_HANDLED);
	assert(wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING));
	assert(wolf->vel[1] == 0.37F);
	assert(entity_wolf_interact(wolf, &s, &empty) == WOLF_INTERACT_HANDLED);
	assert(!wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING));
	assert(wolf->vel[1] == 0.37F);
	assert(!wolf_should_follow(25.0F) && wolf_should_follow(25.001F));
	assert(!wolf_should_teleport(144.0F) && wolf_should_teleport(144.001F));

	struct query_log q = {0};
	vec3 out;
	assert(wolf_find_teleport_site(10.2F, 64.9F, 10.7F,
		normal_cube, &q, out));
	assert(fabsf(out[0] - 8.5F) < 1e-6F && out[1] == 64.0F
		&& fabsf(out[2] - 9.5F) < 1e-6F);
	assert(q.xyz[0][0] == 8 && q.xyz[0][1] == 63 && q.xyz[0][2] == 8);
	assert(q.xyz[1][0] == 8 && q.xyz[1][1] == 63 && q.xyz[1][2] == 9);

	wolf_flag_set(&wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING, true);
	wolf_update_automatic_sit(&wolf->data.monster.wolf_flags, true, true, false);
	assert(!wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING));
	wolf_update_automatic_sit(&wolf->data.monster.wolf_flags, false, false, false);
	assert(wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING));

	// [740c583: EntityWolf.onLivingUpdate] looksWithInterest gate:
	// hasCurrentTarget() && !hasPath() && !isWolfAngry() (&& holding the
	// favorite item, folded into held_favorite_item by the caller).
	assert(wolf_looks_with_interest(true, false, false, true));
	assert(!wolf_looks_with_interest(false, false, false, true)); // no latch
	assert(!wolf_looks_with_interest(true, true, false, true));   // has a path
	assert(!wolf_looks_with_interest(true, false, true, true));   // angry
	assert(!wolf_looks_with_interest(true, false, false, false)); // no favorite item held

	// [740c583: EntityCreature.updatePlayerActionState -- the "no active
	// path this tick" fallback that reaches EntityLiving's currentTarget
	// latch] wolf_update_current_target_latch coverage.
	{
		struct entity* w2 = wolf_test_make(&s, 2, MONSTER_WOLF);
		struct java_random g;
		jrand_set_seed(&g, INT64_C(0xB0BACAFE));

		// (1) wolf_has_path==true: the ambient latch must not even be
		// evaluated -- neither the latch state NOR the RNG stream moves.
		uint64_t seed_before = g.seed;
		wolf_update_current_target_latch(w2, &g, /*wolf_has_path=*/true,
			/*player_has_pos=*/true, /*player_dist_sq=*/1.0F);
		assert(!w2->data.monster.wolf_has_current_target);
		assert(g.seed == seed_before);

		// (2) Player in range, no path: eventually the 2%/tick roll acquires
		// (bounded reachability under the fixed Java48 seed). Each miss
		// draws nextFloat; acquisition then draws nextInt(20).
		// Bounded loop so a broken roll fails the test instead
		// of hanging.
		int ticks = 0;
		while(!w2->data.monster.wolf_has_current_target && ticks < 4000) {
			wolf_update_current_target_latch(w2, &g, false, true, 1.0F);
			ticks++;
		}
		assert(w2->data.monster.wolf_has_current_target);
		int hold = w2->data.monster.wolf_current_target_hold;
		// [740c583: numTicksToChaseTarget = 10 + rand.nextInt(20)] -> [10,29]
		assert(hold >= 10 && hold <= 29);

		// (3) Countdown determinism: once latched, the hold branch draws NO
		// RNG at all, so it must survive EXACTLY `hold`-1 more in-range
		// calls and drop on the `hold`-th.
		for(int i = 0; i < hold - 1; i++) {
			wolf_update_current_target_latch(w2, &g, false, true, 1.0F);
			assert(w2->data.monster.wolf_has_current_target);
		}
		wolf_update_current_target_latch(w2, &g, false, true, 1.0F);
		assert(!w2->data.monster.wolf_has_current_target);

		// (4) Range drop: re-acquire, then move the player past the 8-block
		// (64 sq) range -- drops immediately, even with hold ticks left.
		ticks = 0;
		while(!w2->data.monster.wolf_has_current_target && ticks < 4000) {
			wolf_update_current_target_latch(w2, &g, false, true, 1.0F);
			ticks++;
		}
		assert(w2->data.monster.wolf_has_current_target);
		wolf_update_current_target_latch(w2, &g, false, true,
			WOLF_CURRENT_TARGET_RANGE_SQ + 1.0F);
		assert(!w2->data.monster.wolf_has_current_target);

		// (5) has_pos drop: re-acquire, then the player goes out of scope
		// (has_pos=false) -- drops immediately too.
		ticks = 0;
		while(!w2->data.monster.wolf_has_current_target && ticks < 4000) {
			wolf_update_current_target_latch(w2, &g, false, true, 1.0F);
			ticks++;
		}
		assert(w2->data.monster.wolf_has_current_target);
		wolf_update_current_target_latch(w2, &g, false, false, 1.0F);
		assert(!w2->data.monster.wolf_has_current_target);
	}

	test_wolf_attack_los_gate(&s);

	wolf_test_clear(&s);
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_wolf_follow_sit: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
