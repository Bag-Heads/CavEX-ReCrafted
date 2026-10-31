/*
	next-gaps-0924 W1B -- Beta 1.7.3 mob movement parity goldens:
	ice friction (P2), ladder climbing (P11), suffocation (mob P1).

	Every expected value below is computed from the Beta 1.7.3 Java (the
	decompile at mcb173-retromcp/.../net/minecraft/src/), NOT snapshotted from
	CaveWii (CODESTYLE.md section 4.2, the poisoned-golden rule). Citations:

	  ICE / ground friction  EntityLiving.moveEntityWithHeading:479-489 (accel)
	                         and :491-497 + :534-535 (friction):
	                           friction f starts at 0.91F; on the ground it
	                           becomes 546.0F*0.1F*0.1F*0.1F, or slipperiness
	                           times 0.91F over a non-air block; accel scale
	                           a is 0.16277136F over f cubed; the push is
	                           0.1F*a grounded, 0.02F airborne; then both
	                           horizontal motions are multiplied by f.
	                         Block.java:143 slipperiness = 0.6F (every block),
	                         BlockIce.java:8 slipperiness = 0.98F.
	  LADDER                 EntityLiving.isOnLadder():550-555 (ladder id 65 at
	                         floor x / floor(bb.minY) / floor z);
	                         moveEntityWithHeading:499-524 pre-move clamp
	                         (+-0.15 X/Z, fallDistance = 0, motionY >= -0.15);
	                         :527-533 post-move: a horizontally collided
	                         mob on a ladder gets motionY 0.2D, then
	                         gravity takes 0.08D and drag scales by 0.98F -> the next
	                         move consumes (0.2-0.08)*0.98F = 0.1176 per tick.
	  SUFFOCATION            EntityLiving.onEntityUpdate:113-115
	                         a living mob inside an opaque block takes 1
	                         damage from a null source every tick;
	                         Entity.isEntityInsideOpaqueBlock:851-863 (8 corner
	                         samples in a 0.9*width x 0.1 x 0.9*width box at the
	                         eye, World.isBlockNormalCube); EntityLiving.
	                         getEyeHeight:89-91 = height*0.85 (biped 1.8 ->
	                         1.53); attackEntityFrom:317 absorbs an equal hit
	                         while heartsLife > heartsHalvesLife/2 (10 ticks),
	                         :151 decrements after -> 1 point every 10 ticks.

	ENGINE UNITS that are NOT Beta and are cited as such, never as the spec:
	  - the walk push CREEPER_ACCEL (entity_monster.c, 0.25/tick) and the Wii
	    playability cap MOB_MAX_SPEED_WII (combat.h, 0.18/tick -- CLAUDE.md
	    carve-out #2). The accel golden asserts the BETA RATIO between ice and
	    stone (a) on top of them; the slide goldens start from the cap.
	  - the vertical integrator (GRAVITY 2.0 then *0.9): Beta's -0.08 then *0.98 is
	    a different unit system (tests/test_mob_step_ascent_env.c). The ladder
	    goldens pin what Beta's arithmetic makes the NEXT move consume
	    (0.1176 up, -0.15 down), which is the observable per-tick displacement.

	Harness shape from tests/test_mob_step_ascent_env.c: real production
	tick_server on a superflat world, settle by production tick, pin only the
	drive inputs the scene needs. The mob is a PIG ZOMBIE: EntityLiving
	movement (no override), biped 0.6x1.8 / eye 1.53, fire-immune so the
	daylight burn never enters the health goldens.
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
#include "support/mob_motion_fixture.h"
#include "block/block_slipperiness.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h" /* MOB_MAX_SPEED_WII, MOB_HURT_RESIST_TICKS, mob_eye_height */
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "support/clin_drain.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture[128];
#define FLOOR_Y 69 /* platform blocks -> feet at 70.0 */

/* entity_monster.c CREEPER_ACCEL -- engine walk push, cited, not Beta. */
#define ENGINE_WALK_ACCEL 0.25f

/* Mob motion units S2: expectations run on Beta's doubles. Entity.moveEntity
 * offsets the BOX and derives posX = (minX + maxX) / 2.0D, so a free X move
 * is tracked on the box, not as pos + dx (they differ in the last bit). */
struct exp_x {
	double min, max, vel;
};
static struct exp_x exp_x_of(const struct entity* m, double vel) {
	const struct mob_motion* md = &m->data.monster.motion_d;
	return (struct exp_x) {md->bb.min[0], md->bb.max[0], vel};
}
static double exp_x_move(struct exp_x* x) {
	x->min += x->vel;
	x->max += x->vel;
	return (x->min + x->max) / 2.0;
}

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

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {
									  .type = (uint8_t)type,
									  .metadata = (uint8_t)(meta & 0x0F),
								  }));
}

static void platform(int type, int x0, int x1, int z0, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++)
			set(x, FLOOR_Y, z, type, 0);
}

/* Settle by the REAL production tick (never hand-place the resting Y --
 * an integer Y reads as already-intersecting and freezes). */
static struct entity* spawn_settled(float x, float z) {
	vec3 pos = {x, (float)FLOOR_Y + 2.0f, z};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server);
	float prev_y = m->pos[1];
	for(int i = 0; i < 120; i++) {
		m->vel[0] = 0.0f;
		m->vel[2] = 0.0f;
		m->pos[0] = x;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		m->data.monster.path.length = 0;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped && "mob must survive the settle");
		prev_y = m->pos[1];
	}
	assert(m->pos[1] == prev_y);
	if(!(floorf(m->pos[1]) == (float)(FLOOR_Y + 1) && m->pos[1] < (float)FLOOR_Y + 1.05f)) {
		printf("FAIL settle at x %.3f z %.3f: rest y %.9g (health %d)\n", x, z, m->pos[1],
			   (int)m->health);
		for(int cx = (int)floorf(x) - 1; cx <= (int)floorf(x) + 1; cx++)
			for(int cy = FLOOR_Y; cy <= FLOOR_Y + 2; cy++) {
				struct block_data b;
				bool ok = server_world_get_block(&s.worlds[0], cx, cy, (int)floorf(z), &b);
				printf("  cell (%d,%d,%d): %s type %d\n", cx, cy, (int)floorf(z),
					   ok ? "ok" : "MISSING", ok ? (int)b.type : -1);
			}
		assert(!"mob must come to rest on the platform (feet cell FLOOR_Y+1)");
	}
	assert(m->y_size == 0.0f);
	m->data.monster.path.length = 0;
	m->data.monster.direction[0] = 0.0f;
	m->data.monster.direction[1] = 0.0f;
	m->pos[0] = x;
	m->pos[2] = z;
	m->vel[0] = 0.0f;
	m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	return m;
}

/* ---- 0. The pure chain, pinned against the Java constants ---------------- */
static void test_chain_values(void) {
	/* var8 on a default block: 0.6F*0.91F; airborne 0.91F; no block under
	 * (id 0) is the literal 546.0F*0.1F*0.1F*0.1F -- same bits as 0.6F*0.91F. */
	float stone = 0.6F * 0.91F;
	float ice = 0.98F * 0.91F;
	float none = 546.0F * 0.1F * 0.1F * 0.1F;
	assert(mob_beta_ground_friction(true, BLOCK_STONE) == stone);
	assert(mob_beta_ground_friction(true, BLOCK_SAND) == stone);
	assert(mob_beta_ground_friction(true, BLOCK_ICE) == ice);
	assert(mob_beta_ground_friction(true, 0) == none);
	assert(none == stone); /* Beta's two spellings of the default agree */
	assert(mob_beta_ground_friction(false, BLOCK_ICE) == 0.91F);
	assert(mob_beta_ground_friction(false, BLOCK_STONE) == 0.91F);
	/* var9 = 0.16277136F / var8^3: NOT exactly 1 on stone (Notch's constant
	 * only approximates 0.546^3) -- transcribed, so pin the real float. */
	float var9_stone = 0.16277136F / (stone * stone * stone);
	float var9_ice = 0.16277136F / (ice * ice * ice);
	assert(mob_beta_land_accel_scale(stone) == var9_stone);
	assert(mob_beta_land_accel_scale(ice) == var9_ice);
	assert(var9_stone != 1.0f && var9_stone > 0.9999f && var9_stone < 1.0f);
	assert(var9_ice > 0.2294f && var9_ice < 0.2296f);
	/* Ladder: (0.2D - 0.08D) * (double)0.98F, then the float the move consumes. */
	double climb = 0.2;
	climb -= 0.08;
	climb *= (double)0.98F;
	assert(mob_ladder_climb_vel_y() == (float)climb);
	assert(mob_ladder_climb_vel_y() > 0.1175f && mob_ladder_climb_vel_y() < 0.1177f);
	/* Pre-move clamp: X/Z to +-0.15F, Y floor -0.15D, fallDistance = 0. */
	float vx = 0.3f, vy = -1.8f, vz = -0.3f, fall = 4.0f;
	mob_ladder_premove_clamp(&vx, &vy, &vz, &fall);
	assert(vx == 0.15F && vz == -0.15F && vy == (float)-0.15 && fall == 0.0f);
	vx = 0.1f, vy = 0.42f, vz = -0.1f, fall = 0.5f;
	mob_ladder_premove_clamp(&vx, &vy, &vz, &fall);
	assert(vx == 0.1f && vz == -0.1f && vy == 0.42f && fall == 0.0f);
	printf("PASS chain: var8 stone %.9g ice %.9g air 0.91, var9 stone %.9g ice %.9g, climb %.9g\n",
		   stone, ice, var9_stone, var9_ice, mob_ladder_climb_vel_y());
}

/* ---- 1. SLIDE: a coasting mob decays by var8 per tick, ice vs stone ------ */
static void test_slide(int under_block, float z, int ticks, const char* label) {
	struct entity* m = spawn_settled(8.5f, z);
	/* Coast from the walk cap with no steer input: per Beta tick, the move
	 * carries the current motion (pos += v) and friction then scales it
	 * (v *= var8). Both are single float ops on both sides, so exact.
	 * The stone run deliberately crosses 0.005 (0.18*0.546^6 = 0.0048 at the
	 * start of tick 7): the fork's top-of-tick entity_damp_velocity(e, 0.005f)
	 * used to zero the coast there, and Beta 1.7.3 has NO such motion kill
	 * (the only 0.005 in the decompile is the lava-texture RNG; the
	 * |motion| < 0.005 zeroing arrived after Beta) -- removed in W1B-b. */
	float var8 = block_slipperiness(under_block) * 0.91F;
	m->vel[0] = MOB_MAX_SPEED_WII;
	m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	struct exp_x ex = exp_x_of(m, (double)MOB_MAX_SPEED_WII);
	const struct mob_motion* md = &m->data.monster.motion_d;
	for(int t = 1; t <= ticks; t++) {
		/* Coast like a knocked-back mob: while knockback_time > 0 the walk
		 * push + Wii cap are skipped and only the friction chain acts on the
		 * carried motion -- Beta's own knockback decays under this same var8
		 * (EntityLiving.knockBack sets motionX/Z, moveEntityWithHeading scales
		 * them). Re-pinned far above MOB_KNOCKBACK_TICKS each tick so the
		 * first-tick vertical compose never fires and the idle wander AI's
		 * occasional 1-in-80 path cannot steer the golden. */
		m->data.monster.knockback_time = 100;
		m->data.monster.path.length = 0;
		m->pos[2] = z;
		m->vel[2] = 0.0f;
		test_mob_sync(m); /* motion units S2 */
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		double exp_pos = exp_x_move(&ex);
		ex.vel = ex.vel * (double)var8; /* motionX *= (double)var8 */
		double exp_vel = ex.vel;
		if(md->pos[0] != exp_pos || md->motion[0] != exp_vel) {
			printf("FAIL slide %s tick %d: pos %.17g vel %.17g, Beta expects pos %.17g vel %.17g (var8 %.9g)\n",
				   label, t, md->pos[0], md->motion[0], exp_pos, exp_vel, var8);
			assert(!"slide golden");
		}
	}
	printf("PASS slide %s: %d ticks from %.2f -> pos %.9g vel %.9g (var8 %.9g)\n", label, ticks,
		   MOB_MAX_SPEED_WII, m->pos[0], m->vel[0], var8);
}

/* ---- 2. ACCEL: first walk tick from rest scales by var9, ice vs stone ---- */
static void test_accel(int under_block, float z, const char* label) {
	struct entity* m = NULL;
	/* One waypoint 16 blocks east of the mob (wx = 24 + 0.5 for width 0.6,
	 * wz = 8 + 0.5 == mob z): ddx = 16 exactly, ddz = 0 -> unit heading (1,0)
	 * with no rounding, so the walk push lands entirely on X. The idle AI
	 * rolls a 1-in-80 wander re-path and a 1-in-100 follow skip on the shared
	 * RNG; a tick that steered elsewhere is not the scene, so retry on a
	 * fresh settled mob (this engine has no entity-entity push, P4) instead
	 * of pinning the golden to one RNG seed. */
	float var8 = block_slipperiness(under_block) * 0.91F;
	float var9 = 0.16277136F / (var8 * var8 * var8);
	/* Beta: moveFlying(.., 0.1F times the accel scale) -- here the engine push ENGINE_WALK_ACCEL
	 * times the same scale, then the Wii cap (carve-out #2), then the move and
	 * the friction factor. */
	/* motionX += (double)(accel * dir) with dir (1, 0) exact; the Wii cap in
	 * doubles (carve-out #2). */
	double v = (double)(ENGINE_WALK_ACCEL * var9 * 1.0f);
	if(v > (double)MOB_MAX_SPEED_WII)
		v = v * ((double)MOB_MAX_SPEED_WII / v);
	double exp_pos = 0.0, exp_vel = 0.0;
	int attempt;
	for(attempt = 0; attempt < 8; attempt++) {
		m = spawn_settled(8.5f, z);
		m->data.monster.path.points[0].x = 24;
		m->data.monster.path.points[0].y = FLOOR_Y + 1;
		m->data.monster.path.points[0].z = (int)floorf(z);
		m->data.monster.path.length = 1;
		m->data.monster.path.index = 0;
		struct exp_x ex = exp_x_of(m, m->data.monster.motion_d.motion[0] + v);
		exp_pos = exp_x_move(&ex);
		exp_vel = ex.vel * (double)var8;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		if(m->data.monster.direction[0] == 1.0f && m->data.monster.direction[1] == 0.0f)
			break; /* the hand-set path steered due east this tick */
	}
	assert(attempt < 8 && "the path must steer due east within 8 fresh attempts");
	if(m->data.monster.motion_d.pos[0] != exp_pos
	   || m->data.monster.motion_d.motion[0] != exp_vel) {
		printf("FAIL accel %s: pos %.17g vel %.17g, Beta expects pos %.17g vel %.17g (var9 %.9g var8 %.9g)\n",
			   label, m->data.monster.motion_d.pos[0],
			   m->data.monster.motion_d.motion[0], exp_pos, exp_vel, var9, var8);
		assert(!"accel golden");
	}
	printf("PASS accel %s: first tick pos %.9g vel %.9g (var9 %.9g, var8 %.9g)\n", label,
		   m->pos[0], m->vel[0], var9, var8);
}

/* ---- 3. LADDER CLIMB: pushed against a ladder, rises 0.1176 per tick ----- */
static void test_ladder_climb(float z) {
	int zc = (int)floorf(z);
	/* Wall at x=10 five high (the auto-jump needs a see-through cell above a
	 * blocked one, so a tall wall keeps the step hop out of the golden --
	 * Beta's own EntityCreature hop is onGround-gated the same way once the
	 * mob is climbing). Ladder on its west face, cells 70..73 (meta 4: slab at
	 * x 0.875..1.0, Beta BlockLadder bounds). */
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 5; y++)
		for(int dz = -2; dz <= 2; dz++)
			set(10, y, zc + dz, BLOCK_STONE, 0);
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 4; y++)
		set(9, y, zc, BLOCK_LADDER, 4);
	struct entity* m = spawn_settled(8.5f, z);
	float climb = mob_ladder_climb_vel_y();
	/* Mob motion units S2: on the doubles the climb value is exactly
	 * (0.2D - 0.08D) * (double)0.98F and the rise is the box's. */
	const double climb_d = (0.2 - 0.08) * (double)0.98F;
	const struct mob_motion* cmd = &m->data.monster.motion_d;
	int first_climb_tick = -1;
	double exp_y = 0.0;
	int climbed = 0;
	for(int t = 1; t <= 40 && climbed < 10; t++) {
		/* Drive as a shoved mob (knockback_time > 0 skips the walk push, so the
		 * pinned 0.18 east is the whole horizontal input and the idle wander
		 * AI's 1-in-80 re-path can't steer it off the wall) -- Beta applies
		 * the identical collided-horizontally-on-a-ladder rule to a
		 * knocked-back mob. */
		m->data.monster.knockback_time = 100;
		m->data.monster.path.length = 0;
		m->vel[0] = MOB_MAX_SPEED_WII; /* drive east into the ladder + wall */
		m->vel[2] = 0.0f;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		float y_before = m->pos[1];
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		if(first_climb_tick < 0) {
			if(cmd->motion[1] == climb_d) {
				first_climb_tick = t;
				exp_y = cmd->bb.min[1];
				assert(floorf(m->pos[0]) == 9.0f && "climb starts inside the ladder cell");
			}
			continue;
		}
		/* From the first collided-on-ladder tick on: every tick the move
		 * consumes exactly the Beta post-gravity 0.1176, then re-arms it. */
		exp_y = exp_y + climb_d;
		if(cmd->pos[1] != exp_y || cmd->motion[1] != climb_d
		   || m->data.monster.fall_distance != 0.0f) {
			printf("FAIL ladder climb tick %d: y %.9g -> %.17g vel_y %.17g fall %.9g, Beta expects y %.17g vel_y %.17g fall 0\n",
				   t, y_before, cmd->pos[1], cmd->motion[1],
				   m->data.monster.fall_distance, exp_y, climb_d);
			assert(!"ladder climb golden");
		}
		assert(floorf(m->pos[0]) == 9.0f);
		climbed++;
	}
	if(first_climb_tick < 0) {
		printf("FAIL ladder climb: vel_y never became the Beta climb value %.9g in 40 ticks (last vel_y %.9g y %.9g)\n",
			   climb, m->vel[1], m->pos[1]);
		assert(!"ladder climb never started");
	}
	assert(climbed == 10);
	printf("PASS ladder climb: first climb tick %d, then 10 ticks of exactly +%.9g/tick -> y %.9g\n",
		   first_climb_tick, climb, m->pos[1]);

	/* W1B-b item 4b, re-expressed for mob motion units S2: a hit while
	 * climbing. Beta's knockBack runs from the damage path AT THE HIT
	 * (EntityLiving.attackEntityFrom:336-346 -> knockBack:392-405): motionY =
	 * motionY / 2.0D + (double)0.4F, capped at (double)0.4F, before the mob's
	 * own move. The move then consumes that pop, and the SAME tick's ladder
	 * ascend (motionY = 0.2D, then gravity) re-arms the climb value -- the
	 * pre-S2 deferred compose (knockback_time + a post-gravity rewrite) is
	 * gone. The hit is applied through the production knockBack entry point
	 * with an attacker 1 block west (var3 = -1, var5 = 0). */
	struct mob_motion* md = &m->data.monster.motion_d;
	m->data.monster.path.length = 0;
	m->vel[0] = MOB_MAX_SPEED_WII;
	m->vel[2] = 0.0f;
	m->pos[2] = z;
	test_mob_sync(m); /* motion units S2 */
	double pre_y = md->motion[1];
	mob_motion_knockback(m, md, -1.0, 0.0, NULL);
	m->data.monster.knockback_time = MOB_KNOCKBACK_TICKS; /* armed by the hit site */
	double pop = pre_y / 2.0 + (double)0.4F;
	if(pop > (double)0.4F)
		pop = (double)0.4F;
	assert(md->motion[1] == pop && "knockBack composes the pop at the hit");
	double y_before_pop = md->pos[1];
	assert(!m->tick_server(m, &s));
	if(md->pos[1] != y_before_pop + pop || md->motion[1] != climb_d) {
		printf("FAIL ladder + knockback: y %.17g -> %.17g motion_y %.17g, Beta: "
			   "rise %.17g then the ascend re-arms %.17g\n",
			   y_before_pop, md->pos[1], md->motion[1], pop, climb_d);
		assert(!"ladder + knockback golden");
	}
	printf("PASS ladder + knockback: the hit pops %.9g, the move consumes it, "
		   "the ascend re-arms the climb the same tick\n", pop);
}

/* ---- 4. LADDER DESCENT: free fall in a ladder column is -0.15 per tick ---- */
static void test_ladder_descent(float z) {
	int zc = (int)floorf(z);
	/* Supporting wall FIRST (a ladder whose wall is missing drops itself on
	 * the neighbour notify of the next placement -- BlockLadder rule), then
	 * the column on its west face: slab at x 8.875..9.0, clear of the mob box
	 * 8.2..8.8, so the fall is free of any collision. */
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 5; y++)
		for(int dz = -1; dz <= 1; dz++)
			set(9, y, zc + dz, BLOCK_STONE, 0);
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 5; y++)
		set(8, y, zc, BLOCK_LADDER, 4);
	vec3 pos = {8.5f, (float)FLOOR_Y + 5.5f, z};
	struct entity* m = server_local_spawn_monster_at(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server && m->pos[1] == pos[1]);
	assert(m->health == 20);
	float spawn_vel_y = m->vel[1];
	/* The spawn helper seeds a small initial motion; Beta's freshly placed
	 * entity starts at motion 0, which is the scene's premise. */
	m->vel[0] = 0.0f;
	m->vel[1] = 0.0f;
	m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	/* Mob motion units S2 -- Beta's own ramp on the doubles. Tick 1: motionY
	 * is 0, the clamp has nothing to floor, no move; gravity then leaves
	 * (0 - 0.08D) * (double)0.98F. Tick 2: that -0.0784 is above the -0.15D
	 * floor, so it is consumed as is. From tick 3 the gravity-pulled motion
	 * is below -0.15D and the pre-move clamp floors it: exactly -0.15 per
	 * tick until the feet land. (Pre-S2 the engine's GRAVITY 2.0 reached
	 * the clamp on tick 2 -- the vertical-units difference S2 retires.) The
	 * mob stays in the column on its own (zero horizontal motion, walk push
	 * suppressed), so no per-tick pin disturbs the doubles. */
	struct mob_motion* md = &m->data.monster.motion_d;
	const double step = -0.15;
	double exp_y = md->pos[1];
	int landed_tick = -1;
	for(int t = 1; t <= 60; t++) {
		m->data.monster.knockback_time = 100; /* no walk push: stay in the column */
		m->data.monster.path.length = 0;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		if(t == 1) {
			if(md->pos[1] != exp_y) {
				printf("FAIL ladder descent tick 1: y %.17g, expected no move from %.17g (spawn vel_y %.9g)\n",
					   md->pos[1], exp_y, spawn_vel_y);
				assert(!"ladder descent tick 1");
			}
			continue;
		}
		double this_step = t == 2 ? (0.0 - 0.08) * (double)0.98F : step;
		if(exp_y + this_step < (double)(FLOOR_Y + 1)) {
			/* landing tick: the floor stops the move exactly on its face */
			assert(md->pos[1] == (double)(FLOOR_Y + 1));
			landed_tick = t;
			break;
		}
		exp_y = exp_y + this_step;
		/* fallDistance: zeroed by the pre-move clamp, then Entity.updateFallState
		 * adds THIS tick's descent after the move -- so after any ladder tick it
		 * holds at most one 0.15 step and never accumulates (the engine measures
		 * the step as a position difference, hence the float hair above 0.15). */
		if(md->pos[1] != exp_y || m->data.monster.fall_distance > 0.16f) {
			printf("FAIL ladder descent tick %d: y %.17g fall %.9g, Beta expects y %.17g fall <= one 0.15 step\n",
				   t, md->pos[1], m->data.monster.fall_distance, exp_y);
			assert(!"ladder descent golden");
		}
	}
	assert(landed_tick > 0 && "mob must reach the floor within 60 ticks");
	/* fallDistance was zeroed on every ladder tick, so the 4.5-block column
	 * costs nothing (EntityLiving.fall would otherwise deal ceil(4.5-3) = 2). */
	assert(m->health == 20);
	printf("PASS ladder descent: -0.15/tick from %.2f, landed tick %d, health 20\n", pos[1], landed_tick);
}

/* ---- 5. SUFFOCATION: head in an opaque cube, 1 point every 10 ticks ------ */
static void run_head_scene(float x, int block_cell_x, float z, int block_at_head, int y_off,
						   const char* label, bool expect_damage) {
	int zc = (int)floorf(z);
	struct entity* m = spawn_settled(x, z);
	assert(mob_eye_height(m->data.monster.id) == 1.8f * 0.85f
		   && "biped eye = height*0.85 (EntityLiving.getEyeHeight)");
	assert(monster_def_get(m->data.monster.id)->box_w == 0.6f);
	/* Eye samples: y = 70 + 1.53 +- 0.05 -> cell 71 only; x/z = x +- 0.27
	 * (0.6 * 0.9 * 0.5). At x = 8.5 that is cell 8 only; at x = 8.24 the
	 * corners straddle cells 7 (7.97) and 8 (8.51). y_off selects the head
	 * cell (+1) or the feet cell (0) relative to the feet cell FLOOR_Y+1. */
	set(block_cell_x, FLOOR_Y + 1 + y_off, zc, block_at_head, 0);
	for(int t = 1; t <= 31; t++) {
		m->data.monster.knockback_time = 100; /* no walk push: stay under the block */
		m->data.monster.path.length = 0;
		m->pos[0] = x;
		m->pos[1] = (float)(FLOOR_Y + 1);
		m->pos[2] = z;
		m->vel[0] = 0.0f;
		m->vel[2] = 0.0f;
		test_mob_sync(m); /* motion units S2 */
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		/* Beta cadence: a fresh 1 on tick 1 (heartsLife 20), absorbed while
		 * heartsLife > 10 after the per-tick decrement, fresh again on tick 11,
		 * 21, 31 -> health 20 - (1 + (t-1)/MOB_HURT_RESIST_TICKS). */
		int expect = expect_damage ? 20 - (1 + (t - 1) / MOB_HURT_RESIST_TICKS) : 20;
		if(m->health != expect) {
			printf("FAIL suffocation %s tick %d: health %d, Beta expects %d\n", label, t,
				   (int)m->health, expect);
			assert(!"suffocation golden");
		}
	}
	printf("PASS suffocation %s: health %d after 31 ticks\n", label, (int)m->health);
}

/* ---- W1B-b (Opus review) scenes ------------------------------------------ */

/* Coast a knocked mob (no walk push) and check var8 is chosen on BETA's tick:
 * from the PREVIOUS move's onGround and the block under the feet at the
 * tick-START position [EntityLiving.moveEntityWithHeading:479-497]. Each
 * entry of `expect_var8` is the slip the tick must apply. */
static void coast_expect(struct entity* m, float z, float v0, const float* expect_var8,
						 int n, const char* label) {
	m->vel[0] = v0;
	m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	struct exp_x ex = exp_x_of(m, (double)v0);
	const struct mob_motion* md = &m->data.monster.motion_d;
	for(int t = 1; t <= n; t++) {
		m->data.monster.knockback_time = 100;
		m->data.monster.path.length = 0;
		m->pos[2] = z;
		m->vel[2] = 0.0f;
		test_mob_sync(m); /* motion units S2 */
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		double exp_pos = exp_x_move(&ex);
		ex.vel = ex.vel * (double)expect_var8[t - 1];
		double exp_vel = ex.vel;
		if(md->pos[0] != exp_pos || md->motion[0] != exp_vel) {
			printf("FAIL %s tick %d: pos %.17g vel %.17g, Beta expects pos %.17g vel %.17g (var8 %.9g)\n",
				   label, t, md->pos[0], md->motion[0], exp_pos, exp_vel, expect_var8[t - 1]);
			assert(!"friction tick golden");
		}
	}
	printf("PASS %s: %d ticks, pos %.9g vel %.9g\n", label, n, m->pos[0], m->vel[0]);
}

/* Item 2a: stone -> ice crossing. Tick 1 starts over stone (cell 9) and ENDS
 * over ice; Beta still applies stone's 0.546 that tick, ice from tick 2. */
static void test_crossing(float z) {
	float stone = 0.6F * 0.91F, ice = 0.98F * 0.91F;
	struct entity* m = spawn_settled(9.7f, z);
	const float expect[4] = {stone, ice, ice, ice};
	coast_expect(m, z, 0.5f, expect, 4, "stone->ice crossing (friction on Beta's tick)");
	assert(floorf(m->pos[0]) >= 10.0f);
}

/* Item 2b: ledge step-off. Platform ends at x = 10. Tick 1 (start 9.6, ends
 * 10.1, box still on the ledge), tick 2 (Y move at 10.1 still lands, XZ ends
 * at 10.373 -- box clear), tick 3: Beta's onGround is STILL true (the Y move
 * of tick 2 landed), so ground friction applies once more while airborne;
 * tick 4 onward: air 0.91. (The block under at tick 3 is air -> Beta's
 * 546.0F*0.1F^3 default, the same 0.546.) */
static void test_step_off(float z) {
	float stone = 0.6F * 0.91F, air = 0.91F;
	struct entity* m = spawn_settled(9.6f, z);
	const float expect[5] = {stone, stone, stone, air, air};
	coast_expect(m, z, 0.5f, expect, 5, "ledge step-off (ground slip once past the edge)");
	assert(m->pos[1] < (float)FLOOR_Y + 1.0f && "the mob is falling after the edge");
}

/* Item 2c: landing. Spawned in the air 0.2 above the floor with 0.5 east:
 * ticks 1-2 airborne (0.91), tick 3 is the LANDING tick -- Beta's onGround is
 * still the previous move's false, so 0.91 again -- tick 4 is the first ground
 * tick. Mob motion units S2: Beta's fall from rest moves 0 on tick 1 (motion
 * starts at 0; gravity runs after the move), 0.0784 on tick 2 and would reach
 * 0.2336 cumulative on tick 3, so a 0.2 drop lands on tick 3 exactly as the
 * scene intends (the pre-S2 3-block drop landed on tick 3 only under the
 * retired GRAVITY 2.0). */
static void test_landing(float z) {
	float stone = 0.6F * 0.91F, air = 0.91F;
	vec3 pos = {8.5f, (float)FLOOR_Y + 1.2f, z};
	struct entity* m = server_local_spawn_monster_at(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server);
	m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	const float expect[5] = {air, air, air, stone, stone};
	coast_expect(m, z, 0.5f, expect, 5, "landing (air slip on the landing tick)");
	assert(floorf(m->pos[1]) == (float)(FLOOR_Y + 1) && "landed on the platform");
}

/* Item 3: airborne push. A mob in the air with a path pushes with Beta's
 * flat 0.02F where the grounded push is 0.1F*var9 -- on the engine's push
 * unit that is CREEPER_ACCEL * (0.02F/0.1F), no Wii cap hit, then air 0.91. */
static void test_airborne_push(float z) {
	float air = 0.91F;
	double v = (double)(ENGINE_WALK_ACCEL * (0.02F / 0.1F) * 1.0f);
	struct entity* m = NULL;
	int attempt;
	double exp_pos = 0.0, exp_vel = 0.0;
	for(attempt = 0; attempt < 8; attempt++) {
		vec3 pos = {8.5f, (float)FLOOR_Y + 4.0f, z};
		m = server_local_spawn_monster_at(pos, MONSTER_PIG_ZOMBIE, &s);
		assert(m && m->tick_server);
		m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
		test_mob_sync(m); /* motion units S2 */
		m->data.monster.path.points[0].x = 24;
		m->data.monster.path.points[0].y = FLOOR_Y + 1;
		m->data.monster.path.points[0].z = (int)floorf(z);
		m->data.monster.path.length = 1;
		m->data.monster.path.index = 0;
		struct exp_x ex = exp_x_of(m, v);
		exp_pos = exp_x_move(&ex);
		exp_vel = v * (double)air;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		if(m->data.monster.direction[0] == 1.0f && m->data.monster.direction[1] == 0.0f)
			break;
	}
	assert(attempt < 8);
	if(m->data.monster.motion_d.pos[0] != exp_pos
	   || m->data.monster.motion_d.motion[0] != exp_vel) {
		printf("FAIL airborne push: pos %.17g vel %.17g, Beta expects pos %.17g vel %.17g\n",
			   m->data.monster.motion_d.pos[0], m->data.monster.motion_d.motion[0],
			   exp_pos, exp_vel);
		assert(!"airborne push golden");
	}
	printf("PASS airborne push: first tick pos %.9g vel %.9g (0.02F/0.1F of the ground push)\n",
		   m->pos[0], m->vel[0]);
}

/* Item 4a: EntitySpider.isOnLadder() == isCollidedHorizontally, so the tick
 * AFTER a wall collision takes the pre-move clamp: the along-wall Z motion
 * (0.2 pinned) is cut to 0.15 from then on. */
static void test_spider_clamp(float z) {
	int zc = (int)floorf(z);
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 6; y++)
		for(int dz = -4; dz <= 6; dz++)
			set(10, y, zc + dz, BLOCK_STONE, 0);
	vec3 pos = {8.5f, (float)FLOOR_Y + 2.0f, z};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_SPIDER, &s);
	assert(m && m->tick_server);
	for(int i = 0; i < 120; i++) { /* settle */
		m->data.monster.knockback_time = 100;
		m->vel[0] = m->vel[2] = 0.0f;
		m->pos[0] = 8.5f;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		assert(!m->tick_server(m, &s));
	}
	assert(floorf(m->pos[1]) == (float)(FLOOR_Y + 1));
	bool collided = false;
	int clamped_ticks = 0;
	for(int t = 1; t <= 12; t++) {
		m->data.monster.knockback_time = 100;
		m->data.monster.path.length = 0;
		m->vel[0] = 0.252f; /* spider Wii cap, into the wall */
		m->vel[2] = 0.2f;   /* along the wall: above the 0.15 ladder clamp */
		test_mob_sync(m); /* motion units S2 */
		float z_before = m->pos[2];
		/* Mob motion units S2: expected positions on the doubles -- the box
		 * offset by the Z motion, posZ = (minZ + maxZ) / 2.0D. */
		const struct mob_motion* smd = &m->data.monster.motion_d;
		double zmin = smd->bb.min[2], zmax = smd->bb.max[2];
		double vz_free = smd->motion[2];
		double exp_free = ((zmin + vz_free) + (zmax + vz_free)) / 2.0;
		double exp_clamped = ((zmin + (double)0.15F) + (zmax + (double)0.15F)) / 2.0;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		if(!collided) {
			if(m->data.monster.prev_collided_xz) {
				collided = true; /* this tick hit the wall; clamp starts NEXT tick */
				assert(smd->pos[2] == exp_free && "collision tick itself is not clamped (flag is pre-move)");
			} else
				assert(smd->pos[2] == exp_free);
			continue;
		}
		if(smd->pos[2] != exp_clamped) {
			printf("FAIL spider ladder clamp tick %d: z %.9g -> %.17g, Beta expects %.17g (dz 0.15, isOnLadder == isCollidedHorizontally)\n",
				   t, z_before, smd->pos[2], exp_clamped);
			assert(!"spider clamp golden");
		}
		clamped_ticks++;
	}
	assert(collided && clamped_ticks >= 4);
	printf("PASS spider ladder clamp: %d post-collision ticks at dz 0.15\n", clamped_ticks);
}

/* Item 4c: Beta's lava branch (moveEntityWithHeading:467-476) has no ladder
 * rule. Feet in a ladder cell (72) with lava in the body cell above (73). Mob
 * motion units S2: the lava branch itself is now Beta's -- the flat 0.02F
 * moveFlying, the move, motion *= 0.5D, motionY -= 0.02D -- with the lava
 * isJumping roll (a rand float below 0.8F adds (double)0.04F before the move).
 * So on every tick the post-move motionY must be (v * 0.5D - 0.02D) for the
 * consumed v in {pre, pre + 0.04F}, never the ladder clamp's -0.15D floor nor
 * the dry (v - 0.08D) * 0.98F. The pig zombie is fire-immune, so this also
 * exercises the lava scan for immune mobs. */
static void test_ladder_in_lava(float z) {
	int zc = (int)floorf(z);
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 5; y++)
		for(int dz = -1; dz <= 1; dz++)
			set(9, y, zc + dz, BLOCK_STONE, 0);
	for(int y = FLOOR_Y + 1; y <= FLOOR_Y + 5; y++)
		set(8, y, zc, BLOCK_LADDER, 4);
	set(8, FLOOR_Y + 4, zc, BLOCK_LAVA_STILL, 0);
	vec3 pos = {8.5f, (float)FLOOR_Y + 3.5f, z};
	struct entity* m = server_local_spawn_monster_at(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server);
	m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	const struct mob_motion* md = &m->data.monster.motion_d;
	for(int t = 1; t <= 4; t++) {
		m->data.monster.knockback_time = 100;
		m->data.monster.path.length = 0;
		double pre = md->motion[1];
		assert(!m->tick_server(m, &s));
		double post = md->motion[1];
		double lava_plain = pre * 0.5 - 0.02;
		double lava_jump = (pre + (double)0.04F) * 0.5 - 0.02;
		if(post != lava_plain && post != lava_jump) {
			printf("FAIL ladder in lava tick %d: motionY %.17g -> %.17g, Beta's lava branch gives %.17g or %.17g\n",
				   t, pre, post, lava_plain, lava_jump);
			assert(!"ladder-in-lava golden");
		}
	}
	assert(m->health == 20 && "fire-immune: no lava damage");
	printf("PASS ladder in lava: the lava branch (0.5D drag, -0.02D) runs, no ladder clamp, y %.9g\n",
		   m->pos[1]);
}

/* W1B-c: Beta saves onGround (Entity.writeToNBT:757 "OnGround") and reads it
 * back (readFromNBT:788) -- the previous move's verdict, i.e. prev_on_ground
 * for a monster. A grounded mob saved and reloaded must take GROUND slip (and
 * the grounded push) on its very first tick, not one tick of air. The scene
 * owns chunk (2,0) (x 32..47, z 0..15) so evicting it touches no other lane. */
static void test_onground_reload(void) {
	platform(BLOCK_STONE, 34, 46, 6, 10);
	struct entity* m = spawn_settled(40.5f, 8.5f);
	assert(m->data.monster.prev_on_ground && "settled mob is grounded before the save");
	/* quiesce-0926: prove the inbox empty, then one strict attempt (was a
	 * 500 x 2 ms retry racing the async drain). */
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	test_clin_drain();
	bool saved = server_world_save_chunk(&s.worlds[0], true /*erase*/, 2, 0);
	assert(saved && !server_world_is_chunk_loaded(&s.worlds[0], 2, 0));
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 2, 0, &sc) && sc);
	struct entity* r = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_PIG_ZOMBIE
		   && fabsf(e->pos[0] - 40.5f) < 0.01f && fabsf(e->pos[2] - 8.5f) < 0.01f)
			r = e;
	}
	assert(r && "the pig zombie must come back from the chunk archive");
	if(!r->data.monster.prev_on_ground) {
		printf("FAIL OnGround reload: prev_on_ground false after save/evict/reload (Entity.java:757/788)\n");
		assert(!"OnGround must restore into prev_on_ground");
	}
	float stone = 0.6F * 0.91F;
	const float expect[2] = {stone, stone};
	coast_expect(r, 8.5f, 0.5f, expect, 2, "OnGround reload: first tick uses ground slip");
}

/* Items 8/9 measurements (printed; the RED run prints them before its first
 * golden abort, so before/after numbers come from the two builds). */
static int count_sends_resting(struct entity* m, float x, float z, int ticks) {
	int sends = 0;
	for(int t = 0; t < ticks; t++) {
		m->data.monster.knockback_time = 100;
		m->data.monster.path.length = 0;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		(void)x;
		assert(!m->tick_server(m, &s));
		if(m->data.monster.ticks_since_move_send == 0)
			sends++;
	}
	return sends;
}

static void measure_move_rate(float z) {
	struct entity* m = spawn_settled(8.5f, z);
	m->vel[0] = MOB_MAX_SPEED_WII; /* one shove, then rest */
	test_mob_sync(m); /* motion units S2 */
	int decay = count_sends_resting(m, 8.5f, z, 40);   /* the coast tail */
	int steady = count_sends_resting(m, 8.5f, z, 160); /* at rest */
	printf("MEASURE move-packet rate: mob after one 0.18 shove: %d CRPC_ENTITY_MOVE sends in ticks 1-40 (decay), %d in ticks 41-200 (rest; floor = 160/%d = %d resends)\n",
		   decay, steady, MOB_MOVE_RESEND_INTERVAL_TICKS, 160 / MOB_MOVE_RESEND_INTERVAL_TICKS);
	/* next-gaps-0924/W2 P4: retire this mob (Beta isDead -> the reaper's
	 * delay_destroy 0). test_slide spawns its subject on this very spot, and
	 * since EntityLiving.onLivingUpdate:641-651 landed two overlapping mobs
	 * push each other apart (Entity.applyEntityCollision) -- the slide golden
	 * assumes a lone mob, as Beta's would. Values unchanged. */
	m->delay_destroy = 0;
}

static void measure_getblock(void) {
	/* 100 pig zombies resting on a stone platform, chunks (1..3, 2..3). */
	platform(BLOCK_STONE, 20, 60, 36, 52);
	struct entity* mobs[100];
	int n = 0;
	for(int i = 0; i < 10; i++)
		for(int j = 0; j < 10; j++) {
			vec3 pos = {24.5f + (float)i * 3.0f, (float)FLOOR_Y + 2.0f, 38.5f + (float)j * 1.0f};
			mobs[n] = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
			assert(mobs[n] && mobs[n]->tick_server);
			n++;
		}
	for(int t = 0; t < 40; t++)
		for(int k = 0; k < n; k++) {
			mobs[k]->data.monster.knockback_time = 100;
			mobs[k]->vel[0] = mobs[k]->vel[2] = 0.0f;
			assert(!mobs[k]->tick_server(mobs[k], &s));
		}
	int us, sn, smax, gn, gmax;
	/* resting, knocked (no walk push) */
	server_world_perf_tick_reset();
	for(int k = 0; k < n; k++) {
		mobs[k]->data.monster.knockback_time = 100;
		assert(!mobs[k]->tick_server(mobs[k], &s));
	}
	server_world_perf_tick_reset();
	server_world_edit_stats(&us, &sn, &smax, &gn, &gmax);
	int resting = gn;
	/* idle, not knocked (AI + walk push run, direction zero) */
	server_world_perf_tick_reset();
	for(int k = 0; k < n; k++) {
		mobs[k]->data.monster.knockback_time = 0;
		mobs[k]->data.monster.path.length = 0;
		assert(!mobs[k]->tick_server(mobs[k], &s));
	}
	server_world_perf_tick_reset();
	server_world_edit_stats(&us, &sn, &smax, &gn, &gmax);
	printf("MEASURE get_block reads: 100 resting mobs, one tick: knocked %d (%.2f/mob), idle-AI %d (%.2f/mob)\n",
		   resting, resting / 100.0, gn, gn / 100.0);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* keep the FAIL line ahead of the assert abort in a log */
	test_fixture_dir("test_mob_move_parity_golden_tmp", s_fixture, sizeof s_fixture);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	blocks_init();
	items_init();

	MKDIR(s_fixture);
	MKDIR(test_fixture_join(s_fixture, "/region"));
	remove(test_fixture_join(s_fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_fixture);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.rand_src.seed = 0x51DEB002u; /* xorshift needs nonzero; reproducible */
	/* Beta despawnEntity: a mob farther than 128 blocks from the player is
	 * removed at once (the production tick applies it before any movement).
	 * The lanes run to z = 116.5, so park the (positionless, has_pos false --
	 * no AI targeting) player mid-way; every scene stays within ~60 blocks. */
	s.player.x = 8.5;
	s.player.y = (double)(FLOOR_Y + 1) + 1.62;
	s.player.z = 100.5; /* lanes now run z 8.5 .. 188.5: all within ~92 blocks */

	/* The production mob tick has Beta's position +/-32 checkChunksExist gate
	 * (World.updateEntityWithOptionalForce): a mob whose 32-block
	 * neighbourhood is not fully resident does not tick at all. The lanes run
	 * to z = 188.5, so load out to cz 14 (and cx 4) with margin. */
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -3; cx <= 4; cx++)
		for(w_coord_t cz = -3; cz <= 14; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	test_chain_values();

	/* One lane per scene, 12 blocks apart in z, each on its own platform. */
	platform(BLOCK_STONE, 4, 30, 6, 10);
	platform(BLOCK_ICE, 4, 30, 18, 22);
	platform(BLOCK_STONE, 4, 30, 30, 34);
	platform(BLOCK_ICE, 4, 30, 42, 46);
	platform(BLOCK_STONE, 4, 14, 54, 58);
	platform(BLOCK_STONE, 4, 14, 66, 70);
	platform(BLOCK_STONE, 4, 14, 78, 82);
	platform(BLOCK_STONE, 4, 14, 90, 94);
	platform(BLOCK_STONE, 4, 14, 102, 106);
	/* W1B-b lanes */
	platform(BLOCK_STONE, 4, 9, 126, 130);  /* crossing: stone x<=9 ... */
	platform(BLOCK_ICE, 10, 30, 126, 130);  /* ... ice from x=10 */
	platform(BLOCK_STONE, 4, 9, 138, 142);  /* step-off: ledge ends at x=10 */
	platform(BLOCK_STONE, 4, 30, 150, 154); /* landing */
	platform(BLOCK_STONE, 4, 30, 162, 166); /* airborne push */
	platform(BLOCK_STONE, 4, 14, 170, 184); /* spider clamp (moves along z) */
	platform(BLOCK_STONE, 4, 14, 186, 190); /* ladder in lava */

	/* Measurements first: a RED run prints them before its first abort. */
	measure_move_rate(8.5f);
	measure_getblock();

	test_slide(BLOCK_STONE, 8.5f, 12, "stone"); /* past 0.005: no fork damp (W1B-b) */
	test_slide(BLOCK_ICE, 20.5f, 8, "ice");
	test_accel(BLOCK_STONE, 32.5f, "stone");
	test_accel(BLOCK_ICE, 44.5f, "ice");
	test_ladder_climb(56.5f);
	test_ladder_descent(68.5f);
	run_head_scene(8.5f, 8, 80.5f, BLOCK_SAND, 1, "sand at the head", true);
	run_head_scene(8.5f, 8, 92.5f, BLOCK_GLASS, 1, "glass at the head (translucent material)", false);
	run_head_scene(8.5f, 8, 104.5f, BLOCK_SAND, 0, "sand at the feet only (eye-only test)", false);
	/* Cell-boundary head: the eye corners straddle cells 7 and 8, sand only in
	 * cell 7 -- pins that every DISTINCT corner cell is still read (the cell
	 * de-duplication in mob_inside_opaque_block may skip repeats, never a new
	 * cell), i.e. Entity.java:851-863's corner sweep, not a centre-cell test. */
	platform(BLOCK_STONE, 4, 14, 114, 118);
	run_head_scene(8.24f, 7, 116.5f, BLOCK_SAND, 1, "sand in the west corner cell only (boundary)", true);

	/* W1B-b (Opus review of cbeb3751) */
	test_crossing(128.5f);
	test_step_off(140.5f);
	test_landing(152.5f);
	test_airborne_push(164.5f);
	test_spider_clamp(176.5f);
	test_ladder_in_lava(188.5f);

	/* W1B-c (Opus re-review of 1750c16f) */
	test_onground_reload();

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(s_fixture);
	puts("PASS test_mob_move_parity_golden");
	return 0;
}
