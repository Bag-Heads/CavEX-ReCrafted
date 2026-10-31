/*
	Group A integration goldens for the 2026-06-19 mob-AI parity audit (the `on_ground`
	ordering bug). Two faithful behaviors that the prior pure-helper tests could NOT pin
	because the bug lived in server_tick_monster's integrated ordering, not in a value:

	1. GROUND FRICTION. entity_apply_gravity_and_friction selects the slip (0.6 ground /
	   1.0 air) from on_ground BEFORE gravity clears it. The old split form
	   `entity_apply_gravity(e); entity_apply_friction(e, e->on_ground?0.6:1.0)` read
	   on_ground AFTER entity_apply_gravity had zeroed it, so EVERY grounded mob silently
	   got air friction (1.0*0.8=0.8) instead of ground friction (0.6*0.8=0.48).
	   Beta: EntityLiving.moveEntityWithHeading picks the slipperiness from onGround
	   (the post-move grounded state) before the next move applies gravity.

	2. SPIDER POUNCE is reachable again. A grounded, in-band (4<d^2<36), aggro'd spider
	   with clear LOS leaps (motionY = SPIDER_LEAP_VEL_Y = 0.4) on EntitySpider.attackEntity's
	   1-in-10 roll. Before the fix the leap gate read the gravity-cleared on_ground (always
	   false) one line after entity_apply_gravity ran -> the pounce was DEAD CODE that could
	   never fire. Beta EntitySpider.attackEntity: when onGround, ...; motionY =
	   0.4, dispatched by EntityCreature.updatePlayerActionState BEFORE moveEntityWithHeading
	   applies gravity, so onGround is the prev-move grounded value (true for a grounded
	   spider) and the pounce fires.

	Decompile: 740c583:1.7.3-LTS EntityLiving.moveEntityWithHeading, EntitySpider.attackEntity.
*/

#include <assert.h>
#include <inttypes.h>
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
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_spider_leap_tmp' (a literal) */
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
static void set(int x, int y, int z, int type) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = 0}));
}

// --- 1. entity_apply_gravity_and_friction reads on_ground before clearing it ---------
static void test_gravity_friction_slip(void) {
	struct entity e;
	memset(&e, 0, sizeof e);

	// Grounded: GROUND slip 0.6 -> horizontal *= 0.6*0.8 = 0.48 (the bug gave 0.8).
	e.on_ground = true;
	e.vel[0] = 1.0f; e.vel[1] = 0.0f; e.vel[2] = 2.0f;
	entity_apply_gravity_and_friction(&e, 2.0f, 0.6f, 1.0f);
	assert(fabsf(e.vel[0] - 1.0f * 0.6f * 0.8f) < 1e-5f);  // 0.48
	assert(fabsf(e.vel[2] - 2.0f * 0.6f * 0.8f) < 1e-5f);  // 0.96
	assert(fabsf(e.vel[1] - (0.0f - 2.0f) * 0.9f) < 1e-5f); // gravity then *0.9 = -1.8
	assert(e.on_ground == false);                           // cleared AFTER the slip read

	// Airborne: AIR slip 1.0 -> horizontal *= 1.0*0.8 = 0.8.
	e.on_ground = false;
	e.vel[0] = 1.0f; e.vel[1] = 0.0f; e.vel[2] = 2.0f;
	entity_apply_gravity_and_friction(&e, 2.0f, 0.6f, 1.0f);
	assert(fabsf(e.vel[0] - 1.0f * 1.0f * 0.8f) < 1e-5f);  // 0.8
	assert(fabsf(e.vel[2] - 2.0f * 1.0f * 0.8f) < 1e-5f);  // 1.6

	// The two slips MUST differ (else the grounded/air distinction was lost, the bug).
	assert(0.48f < 0.8f);
	printf("  gravity+friction: grounded->0.48 slip, airborne->0.8 slip, on_ground cleared OK\n");
}

// --- 2. a grounded, in-band, aggro'd spider with LOS can pounce (was dead code) -------
static void test_spider_leap_reachable(void) {
	// Fixed, nonzero seed (xorshift needs nonzero) -> reproducible roll sequence.
	s.rand_src.seed = 0x51DEB001u;

	const int PX = 8, PZ = 8;
	// 5-wide x 8-deep stone platform at Y=69; spider feet rest at Y=70, air above -> LOS clear.
	for(int dx = -2; dx <= 2; dx++)
		for(int dz = -2; dz <= 6; dz++)
			set(PX + dx, 69, PZ + dz, BLOCK_STONE);

	// Player 4 blocks +z: 2D d^2 = 16 -> in the leap band (4 < 16 < 36) and out of the
	// 2.0-block melee reach. player.y is the EYE (feet 70 + ~1.62); feet match the spider's.
	s.player.x = (double)PX + 0.5;
	s.player.y = 70.0 + (double)PLAYER_EYE_HEIGHT;
	s.player.z = (double)PZ + 0.5 + 4.0;
	s.player.health = 200;

	vec3 spos = { (float)PX + 0.5f, 70.0f, (float)PZ + 0.5f };
	struct entity* sp = server_local_spawn_monster(spos, MONSTER_SPIDER, &s);
	assert(sp);
	assert(sp->tick_server);

	int leaps = 0;
	const int TICKS = 600;
	for(int i = 0; i < TICKS; i++) {
		// Re-pin each tick to a grounded, in-band start: a small downward vel lands the
		// spider (entity_try_move sets on_ground on a coord==1 && vel[1]<0 intersection).
		sp->pos[0] = (float)PX + 0.5f;
		sp->pos[1] = 70.0f;
		sp->pos[2] = (float)PZ + 0.5f;
		sp->vel[0] = 0.0f; sp->vel[1] = -0.1f; sp->vel[2] = 0.0f;
		test_mob_sync(sp); /* motion units S2: the fixture pins the doubles */
		// Force the persistent target latch so the day-passive spider stays aggro'd
		// (the leap gate accepts can_acquire || latched).
		sp->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;

		bool reaped = sp->tick_server(sp, &s);
		assert(!reaped);

		// Mob motion units S2: the leap (motionY = (double)0.4F) is consumed by THIS
		// tick's move and then gravity runs, leaving (0.4F - 0.08D) * (double)0.98F
		// > 0; a non-leap grounded tick lands (motion zeroed) and ends at
		// (0 - 0.08D) * (double)0.98F < 0. So motionY > 0 uniquely marks a pounce.
		if(sp->data.monster.motion_d.motion[1] > 0.0)
			leaps++;
	}
	printf("  spider leap fired on %d/%d grounded in-band ticks (was 0 = dead code before fix)\n",
		   leaps, TICKS);

	// The headline: the pounce is REACHABLE (before the on_ground fix it was never true).
	assert(leaps > 0);
	// Sanity that it tracks the ~1-in-10 EntitySpider.attackEntity roll, not a stuck-on
	// bug: a very wide band (expect ~60) that p(outside) is negligible for 600 trials.
	assert(leaps > TICKS / 40);  // > 15
	assert(leaps < TICKS);       // not every tick (the roll gates it)
}

// --- 3. LEAP RNG DRAW-ORDER FIX (2026-09-01) -------------------------------------
// Beta's EntitySpider.attackEntity draws rand.nextInt(10) WHENEVER the target is in
// the 2..6 distance band, decoupled from onGround; onGround is read AFTERWARD, only
// to decide whether the IMPULSE is applied [740c583: EntitySpider.attackEntity:
//   `if (var2>2.0F && var2<6.0F && this.rand.nextInt(10)==0) { if (this.onGround) {
//   ...impulse...} }` -- onGround nests INSIDE the already-rolled branch, so a failed
// onGround test never blocks the draw itself]. Before this fix CaveWii's leap
// condition short-circuited the roll BEHIND grounded_pre_gravity (`... &&
// grounded_pre_gravity && spider_leap_in_band(...) && rand_gen_int(...)==0`), so an
// AIRBORNE in-band spider drew ZERO leap rolls that tick -- silently under-consuming
// the shared rand_src stream relative to Beta.
//
// Proven here by running the IDENTICAL in-band/aggro/LOS setup from the SAME seed
// TWICE, sequentially, on two freshly-spawned spiders at the same spot on the
// leap-reachable platform above -- once GROUNDED every tick (as in
// test_spider_leap_reachable), once AIRBORNE every tick (a small upward vel never
// lands, so grounded_pre_gravity stays false all TICKS ticks; the spider's platform
// LOS/distance geometry is otherwise byte-identical). If the roll is drawn on every
// eligible tick regardless of ground state (the fix), the two independent TICKS-tick
// runs consume IDENTICAL mob-owned Java48 draws and land on the IDENTICAL final seed, even
// though the airborne run never actually applies the impulse. Under the pre-fix
// order the airborne run would draw nothing on any of these ticks, so the final
// seeds would diverge after the very first eligible tick.
static void test_spider_leap_roll_order(void) {
	const int PX = 8, PZ = 8; // reuse the leap-reachable platform (already built above)
	const uint32_t SEED = 0x5EED1EAFu;
	const int TICKS = 300; // well under MOB_DESPAWN_MIN_AGE (600) -> despawn roll silent

	vec3 spos = {(float)PX + 0.5f, 70.0f, (float)PZ + 0.5f};

	// Phase A: GROUNDED every tick (byte-identical setup to test_spider_leap_reachable).
	s.rand_src.seed = SEED;
	s.rand_src.has_next_gaussian = false;
	struct entity* spA = server_local_spawn_monster(spos, MONSTER_SPIDER, &s);
	assert(spA);
	jrand_set_seed(&spA->gameplay_rng, (int64_t)(uint64_t)SEED);
	int leaps_grounded = 0;
	for(int i = 0; i < TICKS; i++) {
		spA->pos[0] = (float)PX + 0.5f;
		spA->pos[1] = 70.0f;
		spA->pos[2] = (float)PZ + 0.5f;
		spA->vel[0] = 0.0f; spA->vel[1] = -0.1f; spA->vel[2] = 0.0f;
		test_mob_sync(spA);
		spA->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
		bool reaped = spA->tick_server(spA, &s);
		assert(!reaped);
		if(spA->data.monster.motion_d.motion[1] > 0.0)
			leaps_grounded++;
	}
	uint64_t seed_grounded = spA->gameplay_rng.seed;

	// Phase B: AIRBORNE every tick -- a FRESH spider (no state carried over from A).
	s.rand_src.seed = SEED;
	s.rand_src.has_next_gaussian = false;
	struct entity* spB = server_local_spawn_monster(spos, MONSTER_SPIDER, &s);
	assert(spB);
	jrand_set_seed(&spB->gameplay_rng, (int64_t)(uint64_t)SEED);
	int leaps_airborne = 0;
	for(int i = 0; i < TICKS; i++) {
		spB->pos[0] = (float)PX + 0.5f;
		spB->pos[1] = 70.0f;
		spB->pos[2] = (float)PZ + 0.5f;
		// Small UPWARD vel: entity_try_move only sets on_ground on a downward
		// (vel[1]<0) ground intersection (see the grounded phase's comment above),
		// so this keeps the spider consistently NOT on_ground every tick.
		spB->vel[0] = 0.0f; spB->vel[1] = 0.05f; spB->vel[2] = 0.0f;
		test_mob_sync(spB);
		spB->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
		bool reaped = spB->tick_server(spB, &s);
		assert(!reaped);
		if(spB->data.monster.motion_d.motion[1] > 0.1)
			leaps_airborne++;
	}
	uint64_t seed_airborne = spB->gameplay_rng.seed;

	printf("  leap draw-order: grounded %d leaps (seed 0x%012" PRIx64
		   "), airborne %d leaps (seed 0x%012" PRIx64 ")\n",
		   leaps_grounded, seed_grounded, leaps_airborne, seed_airborne);

	// Sanity: the grounded run still pounces (matches test_spider_leap_reachable).
	assert(leaps_grounded > 0);
	// The airborne spider NEVER actually applies the impulse (onGround gates it).
	assert(leaps_airborne == 0);
	// But the roll is drawn on the SAME ticks either way: two independent TICKS-tick
	// runs from the identical seed land on the identical final mob RNG state. (Before
	// the draw-order fix, the airborne run drew zero leap rolls and this would fail.)
	assert(seed_airborne == seed_grounded);
}

// --- 4. SPIDER ACQUISITION WITHOUT LOS (fix 2026-09-01) --------------------------
// Beta's EntitySpider OVERRIDES findPlayerToAttack and never calls canEntityBeSeen:
//   `float var1 = getEntityBrightness(1.0F); if (var1 < 0.5F) return worldObj.
//   getClosestPlayerToEntity(this, 16.0D); else return null;`
//   [740c583: EntitySpider.findPlayerToAttack]
// -- contrast EntityMob.findPlayerToAttack (inherited unmodified by zombie/skeleton/
// creeper): `return var1 != null && canEntityBeSeen(var1) ? var1 : null;`
// [740c583: EntityMob.findPlayerToAttack]. So a spider in the dark ACQUIRES a player
// behind a solid wall purely on distance + darkness; a zombie in the byte-identical
// geometry must NOT.
//
// Builds a sealed, roofed corridor (no sky exposure -> eff light 0 -> can_acquire
// true regardless of world_time) split by a full-height/width blocking wall, so
// mob_can_see_player is FALSE for both mobs, then ticks a spider and a zombie from
// the byte-identical near side and asserts: spider ARMS aggro_time (bypasses LOS,
// the fix); zombie does NOT (LOS is still required there, unaffected by this fix).
static void test_spider_acquire_no_los(void) {
	// Kept inside the loaded chunk range (chunks (0,0)/(1,0)/(0,1)/(1,1) -> world
	// blocks x,z in [0,32)) and clear of the other platforms in this file (the
	// leap-reachable platform sits around x 6-15/z 6-14; the creeper aging test at
	// x=8,z=22).
	const int X0 = 16, X1 = 24; // corridor x in [X0, X1)
	const int Z0 = 16, Z1 = 19; // corridor z in [Z0, Z1), 3 wide
	const int YF = 69, YR = 73; // floor at YF, roof at YR; interior y in (YF, YR)
	const int WALL_X = 20;      // the LOS-blocking partition, between mob and player

	// Floor + roof.
	for(int x = X0; x < X1; x++)
		for(int z = Z0; z < Z1; z++) {
			set(x, YF, z, BLOCK_STONE);
			set(x, YR, z, BLOCK_STONE);
		}
	// Side walls (seal light out) + end walls.
	for(int x = X0; x < X1; x++)
		for(int y = YF; y <= YR; y++) {
			set(x, y, Z0 - 1, BLOCK_STONE);
			set(x, y, Z1, BLOCK_STONE);
		}
	for(int z = Z0 - 1; z <= Z1; z++)
		for(int y = YF; y <= YR; y++) {
			set(X0 - 1, y, z, BLOCK_STONE);
			set(X1, y, z, BLOCK_STONE);
		}
	// The LOS-blocking partition: full width/height at WALL_X, between mob (near end)
	// and player (far end). y=YF and y=YR at this x are ALREADY stone from the
	// floor+roof loop above (WALL_X sits inside the corridor's own x-range) -- only
	// the INTERIOR rows (YF, YR) need filling in to seal the partition; re-writing the
	// floor/roof rows here would be a same-value no-op (server_world_set_block returns
	// false for an unchanged id+metadata, matching Beta's setBlockAndMetadataWithNotify
	// short-circuit), which the shared set() helper's assert correctly treats as a bug
	// were it ever hit for a genuinely new write -- so this loop simply never revisits
	// them.
	for(int z = Z0; z < Z1; z++)
		for(int y = YF + 1; y < YR; y++)
			set(WALL_X, y, z, BLOCK_STONE);

	// Player on the FAR side of the wall, mob on the NEAR side: distance well within
	// the 16-block follow range (dx=5, same y/z -> dist^2=25), LOS blocked by WALL_X.
	s.player.x = (double)(X1 - 2) + 0.5;
	s.player.y = 70.0 + (double)PLAYER_EYE_HEIGHT;
	s.player.z = (double)Z0 + 1.0 + 0.5;
	s.player.health = 200;
	s.player.has_pos = true;
	s.player.finished_loading = true;

	vec3 mpos = {(float)(X0 + 1) + 0.5f, 70.0f, (float)Z0 + 1.0f + 0.5f};

	// SPIDER: dark + in range + LOS blocked -> must still ARM (the fix). The arm site
	// sets aggro_time to MOB_TARGET_RETENTION_TICKS, but the SAME tick's chase-branch
	// selection (latched -> AI_CHASE) then decays patience by exactly one on every
	// tick that doesn't ALSO refresh it a second time -- the same "armed then decayed
	// once" contract test_mob_target_latch.c's test_fresh_acquire_arms pins for the
	// generic (LOS-required) case -- so one full tick from a cold (aggro_time==0)
	// start lands on RETENTION-1, not RETENTION.
	struct entity* sp = server_local_spawn_monster(mpos, MONSTER_SPIDER, &s);
	assert(sp);
	sp->data.monster.aggro_time = 0;
	bool reaped = sp->tick_server(sp, &s);
	assert(!reaped);
	printf("  spider (no LOS, dark, in-range) aggro_time after tick: %d "
		   "(expect %d)\n",
		   sp->data.monster.aggro_time, MOB_TARGET_RETENTION_TICKS - 1);
	assert(sp->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS - 1);

	// ZOMBIE: byte-identical geometry/distance -- LOS is STILL required (this fix is
	// spider-only, EntityMob.findPlayerToAttack keeps its canEntityBeSeen call), so
	// it must NOT arm.
	struct entity* zo = server_local_spawn_monster(mpos, MONSTER_ZOMBIE, &s);
	assert(zo);
	zo->data.monster.aggro_time = 0;
	reaped = zo->tick_server(zo, &s);
	assert(!reaped);
	printf("  zombie (no LOS, in-range) aggro_time after tick: %d (expect 0)\n",
		   zo->data.monster.aggro_time);
	assert(zo->data.monster.aggro_time == 0);
}

// --- 5. WALL-CLIMB UNCONDITIONAL ON COLLISION (fix 2026-09-01) -------------------
// Beta's EntitySpider.isOnLadder() returns isCollidedHorizontally UNCONDITIONALLY --
// no AI-state gate anywhere in the method [740c583: EntitySpider.isOnLadder]. Before
// this fix CaveWii only climbed `if (collision_xz && e->ai_state == AI_CHASE)`, so a
// merely-WANDERING spider that bumped a wall fell through to, at best, the
// (now-removed) negative-clamped ladder-descent branch instead of climbing.
//
// Pens a spider (player parked outside the 16-block follow range but well inside the
// 128-block instant-despawn radius, so can_acquire/latched can never arm -- the
// spider stays genuinely AI_IDLE/wandering for the whole run, never AI_CHASE) inside a
// small walled+roofed room and drives it into a wall via a sustained directional
// push (what a real wander target chosen toward the wall would do -- isolates the
// collision -> climb response without depending on the RNG-gated wander/A* candidate
// search actually rolling a target there). If ANY tick shows ai_state==AI_IDLE with
// vel[1] spiking to the climb constant, the wall-climb fired while wandering --
// impossible under the old AI_CHASE-gated code.
static void test_spider_wander_wall_climb(void) {
	// Kept inside the loaded chunk range ([0,32) in x/z) and clear of the other
	// platforms in this file (the leap-reachable platform sits around x 6-15/z 6-14;
	// the acquisition corridor at x 16-24/z 16-19; the creeper aging test at x=8,z=22).
	const int X0 = 25, X1 = 31; // 6x6 room; interior x in [X0+1, X1-1) = 4 wide
	const int Z0 = 16, Z1 = 22; // interior z in [Z0+1, Z1-1) = 4 wide
	const int YF = 69, YR = 74; // floor/roof; interior y in (YF, YR)

	// Single pass over every cell in the box, each visited EXACTLY once (a cell that
	// is both floor/roof AND perimeter -- a bottom/top edge -- would otherwise get a
	// same-value second set() call, which server_world_set_block correctly reports as
	// an unchanged no-op (false) and the shared set() helper's assert would trip on).
	for(int y = YF; y <= YR; y++)
		for(int x = X0; x < X1; x++)
			for(int z = Z0; z < Z1; z++) {
				bool floor_or_roof = (y == YF || y == YR);
				bool perimeter = (x == X0 || x == X1 - 1 || z == Z0 || z == Z1 - 1);
				if(floor_or_roof || perimeter)
					set(x, y, z, BLOCK_STONE);
			}

	// Fixed, nonzero seed -> reproducible (though unused by the direct-drive push
	// below, kept for consistency with the rest of this file's goldens).
	s.rand_src.seed = 0xA17DEC0Du;
	s.rand_src.has_next_gaussian = false;

	// Player parked outside the 16-block follow range but well inside the 128-block
	// instant-despawn radius (mob_despawn_action: dist_sq > 128^2 -> DESPAWN,
	// age-independent -- parking it thousands of blocks away would silently despawn
	// the spider on tick 1 via e->delay_destroy, which halts ALL AI/physics before
	// tick_server ever reaches the movement code, without tripping a `reaped` return
	// -- so 60 blocks (>16, <128) is deliberate, not arbitrary): can_acquire/latched
	// can never arm at that range, so the spider is genuinely AI_IDLE/wandering for
	// the entire run -- never chasing -- so any climb observed cannot be attributed
	// to AI_CHASE.
	s.player.x = (double)(X0 + X1) * 0.5 + 60.0;
	s.player.y = 70.0;
	s.player.z = (double)(Z0 + Z1) * 0.5;
	s.player.health = 200;

	// Spawn clear of the WEST wall (X0) -- the spider's 1.4-wide box (half-width 0.7)
	// needs its bounding box outside the wall's face at X0+1, so center >= X0+2.7 for
	// real clearance; X0+3.5 leaves 1.8 blocks to accelerate across, comfortably
	// inside the room (interior spans X0+1..X1-2) -- before driving it WEST into the
	// wall.
	vec3 mpos = {(float)X0 + 3.5f, 70.0f, (float)(Z0 + Z1) * 0.5f};
	struct entity* sp = server_local_spawn_monster(mpos, MONSTER_SPIDER, &s);
	assert(sp);

	int climbs_while_idle = 0;
	int idle_ticks = 0;
	// Kept comfortably under MOB_DESPAWN_MIN_AGE (600).
	const int TICKS = 200;
	for(int i = 0; i < TICKS; i++) {
		// Drive the spider WEST into the X0 wall every tick: a real player-issued
		// "walk into a wall" is exactly this -- a sustained directional push, not a
		// one-off velocity blip. The wander/chase AI computed its own (zero, since no
		// path -- see mob_path_follow_tail's fallback) direction earlier THIS SAME
		// function, in the section above the walk-push; overriding vel directly here
		// mirrors what that push would do for a real wander target chosen inside the
		// wall. tick_server's own walk-push (entity_move_in_direction with a zero AI
		// direction) adds nothing on top, and entity_clamp_speed only shrinks an
		// over-cap vector -- it does not zero this push.
		sp->vel[0] = -0.3f;
		sp->vel[2] = 0.0f;
		test_mob_sync(sp);
		bool reaped = sp->tick_server(sp, &s);
		assert(!reaped);
		if(sp->ai_state == AI_IDLE)
			idle_ticks++;
		// Mob motion units S2: the climb ascend (motionY = 0.2D) runs between the move
		// and gravity, so a climbing tick ends at exactly (0.2D - 0.08D) * (double)
		// 0.98F -- distinct from a leap (unreachable here, no target) and from
		// ordinary gravity (negative).
		if(sp->ai_state == AI_IDLE
		   && sp->data.monster.motion_d.motion[1] == (0.2 - 0.08) * (double)0.98F)
			climbs_while_idle++;
	}
	printf("  spider wander-climb: %d/%d ticks show a climb ascend while AI_IDLE "
		   "(%d/%d ticks genuinely AI_IDLE; was categorically 0 before the "
		   "AI_CHASE-gate fix)\n",
		   climbs_while_idle, TICKS, idle_ticks, TICKS);
	// Sanity: the whole run stayed non-aggro/non-chasing (player parked far out of
	// range), so any climb we counted truly happened OFF the AI_CHASE path.
	assert(idle_ticks == TICKS);
	assert(climbs_while_idle > 0);
}

// Beta EntityMob.onLivingUpdate: a bright (getEntityBrightness > 0.5) EntityMob hostile
// ages +2 EXTRA per tick on top of EntityLiving's +1, so it despawns FASTER in daylight.
// Parity fix 2026-06-19 (the +2 was previously wrongly omitted as a non-Beta embellishment).
// Use a CREEPER (EntityMob, no sun-burn complications) on an open-sky block; flip world_time
// noon<->midnight to make the SAME torso cell bright (light 15) vs dark (15-11=4).
static void test_bright_mob_ages_faster(void) {
	const int CX = 8, CZ = 22; // clear of the spider-leap platform
	set(CX, 69, CZ, BLOCK_STONE); // creeper feet at 70, torso ~71 sees open sky (light 15)
	vec3 cpos = {(float)CX + 0.5f, 70.0f, (float)CZ + 0.5f};
	struct entity* cr = server_local_spawn_monster(cpos, MONSTER_CREEPER, &s);
	assert(cr && cr->tick_server);
	// Player ~20 blocks away: within the 128-block despawn radius (no far-despawn) but
	// outside the 16-block acquire + 3-block prime, so the creeper just wanders + ages.
	s.player.has_pos = true; s.player.finished_loading = true;
	s.player.x = (double)CX + 20.0; s.player.y = 71.62; s.player.z = (double)CZ;
	s.player.health = 200;

	// NOON: bright -> +3/tick (EntityMob +2 in light, +1 EntityLiving).
	s.world_time = 6000;
	cr->pos[0] = (float)CX + 0.5f; cr->pos[1] = 70.0f; cr->pos[2] = (float)CZ + 0.5f;
	cr->vel[0] = 0.0f; cr->vel[1] = -0.1f; cr->vel[2] = 0.0f;
	test_mob_sync(cr);
	cr->data.monster.entity_age = 0;
	cr->tick_server(cr, &s);
	int bright_gain = cr->data.monster.entity_age;

	// MIDNIGHT: dark (sky 15 - subtract 11 = 4 < 12) -> only the +1 EntityLiving bump.
	s.world_time = 18000;
	cr->pos[0] = (float)CX + 0.5f; cr->pos[1] = 70.0f; cr->pos[2] = (float)CZ + 0.5f;
	cr->vel[0] = 0.0f; cr->vel[1] = -0.1f; cr->vel[2] = 0.0f;
	test_mob_sync(cr);
	cr->data.monster.entity_age = 0;
	cr->tick_server(cr, &s);
	int dark_gain = cr->data.monster.entity_age;

	printf("  EntityMob aging: +%d/tick bright (noon) vs +%d/tick dark (midnight)\n",
		   bright_gain, dark_gain);
	assert(dark_gain == 1);              // EntityLiving +1 only
	assert(bright_gain == 3);            // +2 (EntityMob bright) + 1
	assert(bright_gain == dark_gain + 2); // the recovered onLivingUpdate +2-in-light bonus
}

int main(void) {
	test_fixture_dir("test_spider_leap_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	printf("test_spider_leap_env:\n");
	test_gravity_friction_slip();
	test_spider_leap_reachable();
	test_spider_leap_roll_order();
	test_spider_acquire_no_los();
	test_spider_wander_wall_climb();
	test_bright_mob_ages_faster();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_spider_leap_env\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
