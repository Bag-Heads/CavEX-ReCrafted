/*
	FH-13: end-to-end proof that server_tick_monster's chase/wander branches
	actually route through Beta 1.7.3 A* pathfinding (source/entity/pathfinding.h),
	not the retired mob_chase_dir beeline -- and that the Beta recalc-gate RNG
	draws (EntityCreature.updatePlayerActionState [740c583]) land in the engine's
	seeded mob-owned gameplay_rng stream in the documented order.

	Harness shape from tests/test_mob_snow_walk_env.c (blocks_init/items_init,
	clin_init + drain thread, superflat server_world_create, seeded rand_src,
	settle-by-real-tick, m->tick_server(m,&s) driving the real production path).

	Production proofs:
	  (A) POSITION PROGRESSION AROUND A WALL. A creeper acquires a live visible
	      player before that target moves behind the wall; a beeline steer
	      would walk it straight into the
	      wall and it would sit there forever (mob_chase_dir has no notion of
	      routing). Run enough real ticks and assert the mob's FINAL position has
	      crossed to the player's side of the wall -- proof the engine is
	      following a real routed path, not a straight line into an obstacle.
	  (B) RNG CURSOR. Replay the branch's EXACT bounded-int/float sequence on
	      a scratch java_random initialized with the same declared seed. A
	      nextInt(non-power-of-two bound) can reject samples and advance the
	      LCG more than once, so neither call count nor arbitrary bounds can
	      predict the cursor. These tests measure the production branch's
	      consumption with jrand_next_int_bound/jrand_next_float; independent
	      real-JRE mixed value/state vectors in test_java_random validate the
	      primitives themselves, including rejection and float-bit handling.
	      A plain CREEPER at low entity_age, on dry land, unprimed, unhurt, is
	      confirmed (by reading server_tick_monster) to draw gameplay_rng NOWHERE
	      else this tick: the despawn roll short-circuits on entity_age>600 (not
	      reached), the undead sunburn ignition roll is zombie/skeleton-only, the
	      water-paddle/ghast-waypoint/squid-swim/spider-leap/wolf-leap rolls are
	      all gated behind their own species/state and never reached by a dry
	      creeper. The idle-voice ramp draws from the SEPARATE
	      server_local_sound_rng() stream, never gameplay_rng (S2 sound-stream
	      isolation, documented at its call site) -- so it cannot confound this
	      measurement either. This makes the creeper's owned draws THIS TICK
	      attributable to the recalc/follow gates and entered-path swim float.
	        (B1) Fresh acquire (path.length forced 0 beforehand): Beta's
	             `pathToEntity != null && rand.nextInt(20) != 0` short-circuits on
	             the LEFT operand when pathToEntity is null -- NO nextInt(20)
	             draw. Two draws remain: follow-tail nextInt(100), then the
	             entered-path float, even on dry ground.
	        (B2) Existing path (path.length forced >0 beforehand, far enough that
	             the arrival-advance loop does not consume it): the keep-path gate
	             now draws nextInt(20) unconditionally (has_path is true), so the
	             fixed seed keeps the path, misses background nextInt(80), and
	             passes follow nextInt(100), then draws the dry swim float.
	             Replay asserts these gates before checking the cursor, excluding
	             wander-candidate draws. This proves the
	             beeline-era's total silence (0 draws, always) is gone.
*/

#include <assert.h>
#include <inttypes.h>
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
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/pathfinding.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_pathfinding_entity_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 68
#define FEET_Y 69
// Floor bounds: x in [FLOOR_X_MIN, FLOOR_X_MAX], z in [FLOOR_Z_MIN, FLOOR_Z_MAX].
// Sized to comfortably hold the wall test (spawn x=14.5, wall x=20, player
// x=26.5) and the RNG-cursor tests (spawn/player near x=8.5..12.5), entirely
// within the two loaded chunks (chunk_x 0..1, chunk_z 0 -> world x [0,31],
// z [0,15]) -- see main()'s server_world_load_chunk calls.
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

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {
									  .type = (uint8_t)type,
									  .metadata = (uint8_t)(meta & 0x0F),
								  }));
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
// (A) Position progression around a wall.
// ================================================================================
static void test_navigates_around_wall(void) {
	despawn_all_mobs();

	// A straight solid wall (x=20, z in [FEET_Y'S column -6..6]) spans the whole
	// approach column, 3 blocks tall (69..71) so a biped can't step over it.
	// Floor is already flat stone (laid in main()). The player stands PAST the
	// wall (x=26); the creeper spawns well short of it (x=14) -- a straight
	// beeline from spawn to player crosses x=20 dead-on and would wedge the old
	// mob_chase_dir steer against it forever.
	for(int z = -6; z <= 6; z++)
		for(int y = FEET_Y; y <= FEET_Y + 2; y++)
			set(20, y, 8 + z, BLOCK_STONE, 0);

	s.rand_src.seed = 0xA17CE55u;
	/* Acquire a live, visible target through the actual mob tick before it
	 * moves behind the wall. Hidden corpse wandering is not a chase proof. */
	s.player.x = 18.5;
	s.player.y = (double)FEET_Y;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = 20;

	vec3 pos = {14.5f, (float)FEET_Y, 8.5f};
	struct entity* m
		= server_local_spawn_monster(pos, MONSTER_CREEPER, &s);
	assert(m && m->tick_server);
	/* Keep the existing scenario's explicit seed on its actual owner. */
	jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)0xA17CE55u);
	m->pos[1] = (float)FEET_Y; // feet on the floor top, no settle-fall needed
	assert(!m->tick_server(m, &s));
	assert(m->data.monster.aggro_time > 0);
	assert(m->data.monster.path.length > 0);
	s.player.x = 26.5;

	bool crossed = false;
	int32_t saw_path = 0;
	for(int i = 0; i < 1200 && !crossed; i++) {
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		if(m->data.monster.path.length > 0)
			saw_path++;
		if(m->pos[0] > 20.5f)
			crossed = true;
	}

	if(!crossed) {
		fprintf(stderr,
				"FAIL: creeper never crossed the wall (final pos %.3f,%.3f,%.3f, "
				"path.length=%d) -- pathfinding did not route it around\n",
				(double)m->pos[0], (double)m->pos[1], (double)m->pos[2],
				m->data.monster.path.length);
	}
	assert(crossed && "the mob must route AROUND the wall to reach the player");
	assert(saw_path > 0 && "the mob must have held a real A* path at some point");

	despawn_all_mobs();
}

// ================================================================================
// (B) RNG cursor: the recalc-gate draw discipline.
// ================================================================================

// LIVING-SOUND-RNG: Beta draws the living-sound roll on the mob's own Random
// EVERY tick, in EntityLiving.onEntityUpdate -- before onLivingUpdate's AI
// gates below [server EntityLiving.java:104-106:
//   when rand.nextInt(1000) < livingSoundTime (post-incremented),
//       livingSoundTime resets to -getTalkInterval()
//       and playLivingSound runs].
// Every replay here uses a CREEPER, whose getLivingSound() is null, so a
// fired roll adds no pitch floats (EntityLiving.java:93-97): the roll is
// exactly one nextInt(1000) whatever living_sound_time holds.
static int replay_living_sound_roll(struct java_random* g) {
	return jrand_next_int_bound(g, 1000);
}

// server_tick_monster's latched branch, then mob_path_follow_tail. These
// fixed seeds keep any held path and miss the background wander search.
// Assert the gates so a changed seed cannot silently enter an unreplayed branch.
static uint64_t replay_entered_chase_path(uint32_t seed, bool existing_path) {
	struct java_random g;
	jrand_set_seed(&g, (int64_t)(uint64_t)seed);
	(void)replay_living_sound_roll(&g);
	if(existing_path) {
		assert(jrand_next_int_bound(&g, 20) != 0);
		assert(jrand_next_int_bound(&g, 80) != 0);
	}
	assert(jrand_next_int_bound(&g, 100) != 0);
	(void)jrand_next_float(&g); // entered path: unconditional, dry or wet
	return g.seed;
}

// No target/path: two missed nextInt(80) gates; fallback swim only if wet.
static uint64_t replay_idle_no_path(uint32_t seed, bool wet) {
	struct java_random g;
	jrand_set_seed(&g, (int64_t)(uint64_t)seed);
	(void)replay_living_sound_roll(&g);
	assert(jrand_next_int_bound(&g, 80) != 0);
	assert(jrand_next_int_bound(&g, 80) != 0);
	if(wet)
		(void)jrand_next_float(&g);
	return g.seed;
}

static struct entity* spawn_dry_creeper(float x, float z) {
	vec3 pos = {x, (float)FEET_Y, z};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_CREEPER, &s);
	assert(m && m->tick_server);
	m->pos[1] = (float)FEET_Y;
	m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
	m->data.monster.entity_age = 0;   // < 600: despawn roll short-circuits
	m->data.monster.fuse = -1;        // not primed: no explosion-path draws
	return m;
}

/* Entity.java:50 owns one Random per entity. This existing-field witness
 * executes actual A and B ticks before any optional RNG-field assertions:
 * a harmless dry A path tick must not alter B's next chicken timer draw. */
static void test_mob_rng_behavioral_isolation(void) {
	despawn_all_mobs();
	s.player.x = 12.5;
	s.player.y = (double)FEET_Y;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	struct entity* a = spawn_dry_creeper(8.5f, 8.5f);
	struct entity* b = server_local_spawn_monster(
		(vec3) {26.5f, (float)FEET_Y, 8.5f}, MONSTER_CHICKEN, &s);
	assert(a && b && b->tick_server && b->data.monster.egg_timer == 0);
	/* Pin separate Java48 mob streams; A must not affect B's timer. */
	jrand_set_seed(&a->gameplay_rng, (int64_t)(uint64_t)0xC0FFEE01u);
	jrand_set_seed(&b->gameplay_rng, (int64_t)(uint64_t)0xBEAFE501u);
	/* Both executions retain the same population and B lifetime. Its inline
	 * path/state is a value snapshot; neither sentinel tick lays an egg. */
	struct entity b_before = *b;
	struct random_gen world_before = {
		.seed = 0xC0FFEE01u, .has_next_gaussian = true,
		.next_gaussian = 0.375F,
	};
	s.rand_src = world_before;
	assert(!b->tick_server(b, &s));
	int control_timer = b->data.monster.egg_timer;
	*b = b_before;
	s.rand_src = world_before;
	assert(!a->tick_server(a, &s));
	assert(a->data.monster.path.length > 0);
	struct random_gen world_after_a = s.rand_src;
	assert(!b->tick_server(b, &s));
	fprintf(stderr,
		"mob RNG witness: B timer control=%d extra-A=%d; "
		"world before=%08x after-A=%08x cache=%d/%g\n",
		control_timer, b->data.monster.egg_timer, world_before.seed,
		world_after_a.seed, world_after_a.has_next_gaussian,
		(double)world_after_a.next_gaussian);
	assert(b->data.monster.egg_timer == control_timer
		&& "A's dry path tick must not perturb B's actual next timer draw");
	assert(world_after_a.seed == world_before.seed
		&& world_after_a.has_next_gaussian == world_before.has_next_gaussian
		&& world_after_a.next_gaussian == world_before.next_gaussian);
	despawn_all_mobs();
}

static void test_mob_rng_lifetime(void) {
	struct entity a, b, client;
	struct random_gen before = s.rand_src;
	memset(&a, 0xBE, sizeof(a));
	memset(&b, 0xBE, sizeof(b));
	memset(&client, 0xBE, sizeof(client));
	entity_monster(1201, &a, true, &s.worlds[0], MONSTER_CREEPER);
	entity_monster(1202, &b, true, &s.worlds[0], MONSTER_CREEPER);
	entity_monster(1201, &client, false, &s.worlds[0], MONSTER_CREEPER);
	struct java_random expected_a, expected_b;
	entity_lifetime_rng_seed(&expected_a, s.worlds[0].seed, s.worlds[0].dimension, 1201);
	entity_lifetime_rng_seed(&expected_b, s.worlds[0].seed, s.worlds[0].dimension, 1202);
	assert(a.gameplay_rng.seed == expected_a.seed);
	assert(b.gameplay_rng.seed == expected_b.seed);
	assert(a.gameplay_rng.seed && b.gameplay_rng.seed);
	assert(a.gameplay_rng.seed != b.gameplay_rng.seed);
	assert(!a.gameplay_rng.have_next_gaussian && a.gameplay_rng.next_gaussian_cached == 0);
	assert(!b.gameplay_rng.have_next_gaussian && b.gameplay_rng.next_gaussian_cached == 0);
	assert(client.gameplay_rng.seed == 0 && !client.gameplay_rng.have_next_gaussian
		&& client.gameplay_rng.next_gaussian_cached == 0);
	assert(s.rand_src.seed == before.seed
		&& s.rand_src.has_next_gaussian == before.has_next_gaussian
		&& s.rand_src.next_gaussian == before.next_gaussian);
	a.gameplay_rng.have_next_gaussian = true;
	a.gameplay_rng.next_gaussian_cached = 0.625F;
	struct java_random state = a.gameplay_rng;
	a.teleport(&a, (vec3) {10.5F, (float)FEET_Y, 8.5F});
	assert(a.gameplay_rng.seed == state.seed
		&& a.gameplay_rng.have_next_gaussian == state.have_next_gaussian
		&& a.gameplay_rng.next_gaussian_cached == state.next_gaussian_cached);
	puts("  RNG lifetime: poisoned server/client init and teleport cache OK");
}

static void test_rng_cursor_fresh_acquire(void) {
	despawn_all_mobs();
	// Open ground, no wall: the player is directly reachable so the search this
	// tick is guaranteed to succeed (has_path_now becomes true).
	s.player.x = 8.5 + 4.0;
	s.player.y = (double)FEET_Y;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	struct entity* m = spawn_dry_creeper(8.5f, 8.5f);
	m->data.monster.path.length = 0; // force the "no path yet" precondition

	uint32_t seed_before = 0xC0FFEE01u;
	jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)seed_before);

	bool reaped = m->tick_server(m, &s);
	assert(!reaped);

	// Beta: pathToEntity==null short-circuits the WHOLE nextInt(20) term (its
	// left operand is false) -- zero draws from the keep-path gate. The
	// unconditional re-acquire branch (mob_acquire_chase_path) itself never
	// touches gameplay_rng (the A* search has no randomness in Beta). The
	// follow-tail nextInt(100), then entered-path nextFloat, PROVIDED the
	// fresh search succeeded and the follow gate passes (asserted in replay).
	assert(m->data.monster.path.length > 0
		   && "the fresh acquire must have found a path (open ground, 4 blocks)");
	uint64_t expect_three_draws = replay_entered_chase_path(seed_before, false);
	if(m->gameplay_rng.seed != expect_three_draws) {
		fprintf(stderr,
				"FAIL: fresh-acquire tick did not consume exactly 3 owned draws "
				"(declared_seed=%08x seed_after=%012" PRIx64
				" expected=%012" PRIx64 ")\n",
				seed_before, m->gameplay_rng.seed, expect_three_draws);
	}
	assert(m->gameplay_rng.seed == expect_three_draws
		   && "fresh acquire (no prior path): the living-sound nextInt(1000), "
			  "ZERO from the nextInt(20) keep-path gate, ONE follow-gate draw, "
			  "then the unconditional path swim float");

	despawn_all_mobs();
}

static void test_rng_cursor_existing_path_bounded(void) {
	despawn_all_mobs();
	s.player.x = 8.5 + 4.0;
	s.player.y = (double)FEET_Y;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	struct entity* m = spawn_dry_creeper(8.5f, 8.5f);
	// Force a pre-existing, far-from-arrival path so the arrival-advance loop
	// inside path_follow_steer does not immediately consume it this tick.
	m->data.monster.path.length = 2;
	m->data.monster.path.index = 0;
	m->data.monster.path.points[0]
		= (struct path_waypoint) {(int32_t)9, (int32_t)FEET_Y, (int32_t)8};
	m->data.monster.path.points[1]
		= (struct path_waypoint) {(int32_t)30, (int32_t)FEET_Y, (int32_t)8};

	uint32_t seed_before = 0x51DEB123u;
	jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)seed_before);

	bool reaped = m->tick_server(m, &s);
	assert(!reaped);

	// This declared seed keeps the existing path: nextInt(20) != 0,
	// nextInt(80) != 0 (no candidate search), nextInt(100) != 0, nextFloat.
	// A public nextInt may consume several LCG steps; replay the actual bounds.
	uint64_t expected = replay_entered_chase_path(seed_before, true);
	if(m->gameplay_rng.seed != expected) {
		fprintf(stderr,
				"FAIL: existing-path tick did not replay the kept-path sequence "
				"(declared_seed=%08x seed_after=%012" PRIx64
				" expected=%012" PRIx64 ")\n",
				seed_before, m->gameplay_rng.seed, expected);
	}
	assert(m->gameplay_rng.seed == expected
		   && "existing path must consume the living-sound nextInt(1000), nextInt(20), nextInt(80), "
			  "nextInt(100), then the entered-path nextFloat");

	despawn_all_mobs();
}

/* Derived branch counts from EntityCreature.updatePlayerActionState: keep
 * nextInt20, background nextInt80, follow nextInt100, then path float.
 * Idle fallback misses BOTH nextInt80 gates, then draws only when liquid.
 * These are actual ticks/terrain/path handling, not raw-generator-only tests. */
static void test_path_swim_branch_counts(void) {
	const uint32_t seed = 0xC0FFEE01u;
	for(int wet = 0; wet <= 1; wet++) {
		despawn_all_mobs();
		s.player.x = 12.5; s.player.y = (double)FEET_Y; s.player.z = 8.5;
		s.player.has_pos = true; s.player.finished_loading = true;
		struct entity* m = spawn_dry_creeper(8.5F, 8.5F);
		jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)seed);
		assert(!m->tick_server(m, &s));
		assert(m->data.monster.path.length > 0);
		if(wet)
			for(int y = FEET_Y; y <= FEET_Y + 1; y++)
				set(8, y, 8, BLOCK_WATER_STILL, 0);
		jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)seed);
		uint32_t world_seed = s.rand_src.seed;
		assert(!m->tick_server(m, &s));
		fprintf(stderr, "  held path wet=%d owned=%012" PRIx64
			" expected=%012" PRIx64 " length=%d\n",
			wet, m->gameplay_rng.seed, replay_entered_chase_path(seed, true),
			(int)m->data.monster.path.length);
		assert(m->gameplay_rng.seed == replay_entered_chase_path(seed, true));
		assert(s.rand_src.seed == world_seed);
		if(wet)
			for(int y = FEET_Y; y <= FEET_Y + 1; y++)
				set(8, y, 8, BLOCK_AIR, 0);
	}
	for(int wet = 0; wet <= 1; wet++) {
		despawn_all_mobs();
		s.player.has_pos = false;
		struct entity* m = spawn_dry_creeper(8.5F, 8.5F);
		if(wet)
			for(int y = FEET_Y; y <= FEET_Y + 1; y++)
				set(8, y, 8, BLOCK_WATER_STILL, 0);
		jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)seed);
		assert(!m->tick_server(m, &s));
		assert(m->data.monster.path.length == 0);
		fprintf(stderr, "  fallback wet=%d owned=%012" PRIx64
			" expected=%012" PRIx64 "\n",
			wet, m->gameplay_rng.seed, replay_idle_no_path(seed, wet));
		assert(m->gameplay_rng.seed == replay_idle_no_path(seed, wet));
		if(wet)
			for(int y = FEET_Y; y <= FEET_Y + 1; y++)
				set(8, y, 8, BLOCK_AIR, 0);
	}
	despawn_all_mobs();
	s.player.has_pos = true;
	struct entity* m = spawn_dry_creeper(8.5F, 8.5F);
	m->data.monster.path.length = 1;
	m->data.monster.path.index = 0;
	m->data.monster.path.points[0] = (struct path_waypoint) {8, FEET_Y, 8};
	jrand_set_seed(&m->gameplay_rng, (int64_t)(uint64_t)seed);
	assert(!m->tick_server(m, &s));
	assert(m->data.monster.path.length == 0);
	assert(m->gameplay_rng.seed == replay_entered_chase_path(seed, true));
	puts("  path swim: dry/wet, fallback and final-waypoint draw counts OK");
	despawn_all_mobs();
}

// ================================================================================
/* L01: CFR server EntityCreature.java:54-68,116-121 and
 * EntityLiving.java:652-655; Jad EntityCreature.java:67-74,128 and
 * EntityLiving.java:747-751 agree. Entered paths draw nextFloat even dry,
 * including the tick which consumes the final waypoint. No-path/skip
 * fallback draws that float only in water/lava. Pin the actual jump as
 * well as the cursor: moving a float ahead of single-draw int gates can
 * preserve the cursor while changing which tick jumps. The idle replay
 * covers server locomotion: the existing mob_path_follow_tail maps base
 * head-turn RNG to the client, so these checks do not certify the whole
 * EntityLiving idle Random stream against Beta. */
static void test_swim_roll_all_path_branches(void) {
	for(int liquid = 0; liquid < 3; liquid++) {
		for(int branch = 0; branch < 6; branch++) {
			for(int jump = 0; jump < 2; jump++) {
				/* chase, wander, no path, skipped chase, exhausted chase,
				 * exhausted wander. Select seeds from the Beta gate replay,
				 * never from production results. */
				bool chase = branch == 0 || branch == 3 || branch == 4;
				bool has_path = branch != 2;
				bool entered_path = has_path && branch != 3;
				struct java_random expected;
				uint32_t seed;
				float roll = 0.0F;
				int sound_roll = 0;
				for(seed = 1; seed < 100000; seed++) {
					jrand_set_seed(&expected, (int64_t)seed);
					/* Lava: Entity.onEntityUpdate's setOnFireFromLava ->
					 * attackEntityFrom(null, 4) lands a FRESH hit on the
					 * unhurt creeper, whose hurt sound is non-null, so its
					 * pitch draws 2 floats on this.rand BEFORE the
					 * living-sound roll [server EntityLiving.java:103 super
					 * call, :345 hurt pitch, then :104]. Water's entry splash
					 * draws are an unmodelled server residual
					 * (LIVING-SOUND-RESULT.md) and are not replayed. */
					if(liquid == 2) {
						(void)jrand_next_float(&expected);
						(void)jrand_next_float(&expected);
					}
					sound_roll = replay_living_sound_roll(&expected);
					if(chase && jrand_next_int_bound(&expected, 20) == 0)
						continue;
					if(jrand_next_int_bound(&expected, 80) == 0)
						continue;
					if(!has_path && jrand_next_int_bound(&expected, 80) == 0)
						continue;
					if(has_path) {
						bool follows = jrand_next_int_bound(&expected, 100) != 0;
						if(follows != entered_path)
							continue;
					}
					if(entered_path || liquid) {
						roll = jrand_next_float(&expected);
						if((roll < 0.8F) != (jump != 0))
							continue;
					}
					break;
				}
				assert(seed < 100000);
				despawn_all_mobs();
				s.player.has_pos = chase;
				s.player.finished_loading = true;
				s.player.health = 160;
				s.player.x = 12.5;
				s.player.y = (double)FEET_Y;
				s.player.z = 8.5;
				struct entity* m = spawn_dry_creeper(8.5F, 8.5F);
				if(has_path) {
					m->data.monster.path.length = 1;
					m->data.monster.path.index = 0;
					m->data.monster.path.points[0] = (struct path_waypoint){
						branch >= 4 ? 8 : 12, FEET_Y, 8};
				}
				if(liquid)
					for(int y = FEET_Y; y <= FEET_Y + 1; y++)
						set(8, y, 8, liquid == 1 ? BLOCK_WATER_STILL
							: BLOCK_LAVA_STILL, 0);
				jrand_set_seed(&m->gameplay_rng, (int64_t)seed);
				/* Pin the living-sound roll's VALUE, not just its count: with
				 * the ramp at 500 the roll fires iff nextInt(1000) < 500, which
				 * depends on where in the stream it is drawn (after the lava
				 * hurt pitch, before the AI gates). Creeper: getTalkInterval()
				 * 80 [EntityLiving.java:89-91, :104-106]. */
				m->data.monster.living_sound_time = 500;
				const int expect_lst = sound_roll < 500 ? -80 : 501;
				unsigned char world_before[sizeof(s.rand_src)];
				memcpy(world_before, &s.rand_src, sizeof(world_before));
				assert(!m->tick_server(m, &s));
				fprintf(stderr, "  L01 swim liquid=%d branch=%d jump=%d seed=%u\n",
					liquid, branch, jump, seed);
				assert(m->gameplay_rng.seed == expected.seed);
				assert(m->data.monster.living_sound_time == expect_lst
					   && "the living-sound roll is drawn at Beta's stream position");
				assert(memcmp(&s.rand_src, &world_before, sizeof(world_before)) == 0);
				assert((m->data.monster.path.length > 0) == (branch < 2));
				if(liquid) {
					double drag = liquid == 1 ? (double)0.8F : 0.5;
					double motion_y = (jump ? (double)0.04F : 0.0) * drag - 0.02;
					assert(m->data.monster.motion_d.motion[1] == motion_y);
				}
				if(liquid)
					for(int y = FEET_Y; y <= FEET_Y + 1; y++)
						set(8, y, 8, BLOCK_AIR, 0);
			}
		}
	}
	despawn_all_mobs();
	puts("  PASS L01 swim: 36 dry/water/lava chase/wander/no-path/skip/"
		 "exhausted ticks; exact owned cursor, jump order and unchanged world RNG");
}

// (C) Squid never wanders an A* path (idle-locomotion audit finding).
// ================================================================================
// Beta EntitySquid.updatePlayerActionState [740c583] is a COMPLETE override
// that never calls super.updatePlayerActionState() -- it has no relationship
// whatsoever to EntityCreature's pathToEntity/updateWanderPath machinery, so
// a real squid draws ZERO nextInt(80) wander rolls and runs ZERO A* searches,
// ever. can_acquire is forced false for every passive id (monsters_object.c:
// squid .passive=true), so a squid always takes server_tick_monster's WANDER
// branch -- before the fix, that branch ran mob_acquire_wander_path +
// mob_path_follow_tail unconditionally for every non-latched mob, so a squid
// (whose own dedicated swim branch never reads path/direction) still silently
// accrued a real A* path. Proven behaviourally (not by RNG-cursor matching,
// since the squid's own swim code legitimately draws gameplay_rng too): path.length
// must stay exactly 0 for a beached squid across many ticks, in a scene where
// (B) below proves the SAME harness DOES observe a wandering land animal
// acquire a path -- so a persistently-zero path.length is a real behavioural
// difference, not a harness artifact.
static void test_squid_never_wanders_path(void) {
	despawn_all_mobs();
	s.rand_src.seed = 0x5411D001u;
	// Player 30 blocks off: outside the 16-block follow/acquire range (moot
	// anyway -- can_acquire is forced false for every passive id, so
	// `latched` stays false and every mob here takes the WANDER branch
	// regardless of distance) but well INSIDE Beta's unconditional 128-block
	// despawn radius (EntityLiving.despawnEntity: `if (dist_sq > 16384.0)
	// setEntityDead()`, MOB_DESPAWN_FAR_SQ in combat.h) -- a farther player
	// would despawn-flag the mob on tick 1 and short-circuit BEFORE the
	// wander-AI block ever runs, making this test pass for the wrong reason.
	s.player.x = 38.5;
	s.player.y = (double)FEET_Y;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	vec3 pos = {8.5f, (float)FEET_Y, 8.5f};
	struct entity* squid = server_local_spawn_monster(pos, MONSTER_SQUID, &s);
	assert(squid && squid->tick_server);
	squid->pos[1] = (float)FEET_Y;
	assert(squid->data.monster.path.length == 0); // FH-13 spawn invariant

	for(int i = 0; i < 500; i++) {
		bool reaped = squid->tick_server(squid, &s);
		assert(!reaped);
		if(squid->data.monster.path.length != 0) {
			fprintf(stderr,
					"FAIL: squid acquired an A* path at tick %d (length=%d) -- "
					"EntitySquid.updatePlayerActionState never does this in "
					"Beta\n",
					i, squid->data.monster.path.length);
		}
		assert(squid->data.monster.path.length == 0
			   && "a squid must NEVER acquire an A* wander path");
	}

	despawn_all_mobs();
}

// Negative control for (C): the exact same scene, but a passive LAND animal
// (pig) -- which DOES inherit the generic EntityCreature/EntityLiving wander
// machinery in Beta -- must actually DRAW from its gameplay_rng via the
// wander-roll gate every tick it has no path (FH-13's `do_wander_roll`,
// unconditional when has_path is false: `nextInt(80)==0`, redrawn once more
// on a miss -- see mob_acquire_wander_path's call site). This is checked via
// RNG-cursor advancement rather than requiring the A* search to actually
// SUCCEED (which depends on synthetic-floor pathability the way
// test_navigates_around_wall's open-ground chase does not) -- it still
// proves test_squid_never_wanders_path's persistently-zero path.length is a
// real per-species behavioural difference (the wander machinery is reached
// and DOES draw for a land animal), not e.g. a dead wander-roll that never
// fires for anyone in this harness.
static void test_pig_does_wander_gate_control(void) {
	despawn_all_mobs();
	s.rand_src.seed = 0x519D0001u;
	// Same 30-block-off, despawn-safe placement as test_squid_never_wanders_path
	// above (a farther player would despawn-flag the mob before the wander-AI
	// block ever runs -- see that test's comment).
	s.player.x = 38.5;
	s.player.y = (double)FEET_Y;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	vec3 pos = {8.5f, (float)FEET_Y, 8.5f};
	struct entity* pig = server_local_spawn_monster(pos, MONSTER_PIG, &s);
	assert(pig && pig->tick_server);
	pig->pos[1] = (float)FEET_Y;
	pig->data.monster.path.length = 0; // force the "no path yet" precondition

	uint64_t seed_before = pig->gameplay_rng.seed;
	bool reaped = pig->tick_server(pig, &s);
	assert(!reaped);
	if(pig->gameplay_rng.seed == seed_before)
		fprintf(stderr,
				"FAIL: control pig's gameplay_rng did not advance at all this "
				"tick -- the wander-roll gate never fired, so the squid test "
				"above would be untrustworthy\n");
	assert(pig->gameplay_rng.seed != seed_before
		   && "control: a wandering pig's gameplay_rng MUST advance -- the "
			  "wander-roll gate draws unconditionally every tick it holds "
			  "no path (nextInt(80) advances Java48 at least once)");

	despawn_all_mobs();
}

int main(void) {
	test_fixture_dir("test_pathfinding_entity_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.rand_src.seed = 0x51DEB001u;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 1, 0, &sc) && sc);

	for(int x = FLOOR_X_MIN; x <= FLOOR_X_MAX; x++)
		for(int z = FLOOR_Z_MIN; z <= FLOOR_Z_MAX; z++)
			set(x, FLOOR_Y, z, BLOCK_STONE, 0);

	test_mob_rng_behavioral_isolation();
	test_mob_rng_lifetime();
	test_navigates_around_wall();
	test_rng_cursor_fresh_acquire();
	test_rng_cursor_existing_path_bounded();
	test_path_swim_branch_counts();
	test_swim_roll_all_path_branches();
	test_squid_never_wanders_path();
	test_pig_does_wander_gate_control();

	printf("test_pathfinding_entity_env: server_tick_monster routes A* around a "
		   "wall (not the retired beeline), the recalc-gate RNG cursor "
		   "matches Beta's exact short-circuit draw discipline (0 draws on a "
		   "null-path keep gate, nextInt(20) once a path exists, then an "
		   "entered-path float even when dry), and a "
		   "squid never acquires an A* wander path (control: a pig does)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
