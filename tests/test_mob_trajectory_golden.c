/*
	Mob motion units S2 (docs/intent/mob-motion-units.md; Astra rulings
	2026-09-27): per-tick TRAJECTORY goldens through REAL production mob
	ticks (server_local_test_tick_entities), compared byte-for-byte against
	Beta's own per-tick arithmetic evaluated independently in this file.

	The reference integrator (ref_tick) is transcribed from the decompile
	(740c583, ../mcb173-retromcp src), casts spelled out:
	  EntityLiving.onLivingUpdate:629-642 jumps when grounded and requested:
	      vertical motion becomes double-promoted 0.42F [EntityLiving.java:659-661].
	  EntityLiving.moveEntityWithHeading:479-535 (dry branch)
	      Ground friction uses slipperiness times 0.91F; airborne friction is 0.91F.
	      Read ground state BEFORE movement, from the preceding move.
	      Move by the current motion, then apply vertical gravity 0.08 and 0.98F drag.
	      Apply the earlier horizontal friction to both horizontal motion components.
	  Entity.moveEntity:252-434 over a flat floor (the only collider):
	      Clip downward travel to the floor clearance; offset the box.
	      Derive position from the box, with vertical offset and ySize both zero for pigs.
	      Grounding requires clipped downward travel; clipped vertical motion becomes zero.
	  Entity.updateFallState:505-515 + EntityLiving.fall accumulate downward travel
	      while airborne; landing damage is the ceiling of fall distance minus three.
	  EntityLiving.knockBack:392-405 (the creeper-exploder compose):
	      Halve all motion; subtract horizontal normalized attacker separation scaled
	      by double-promoted 0.4F; add that same amount vertically, capped at 0.4F.
	      The horizontal length is square-rooted and rounded through float.
	  Explosion.doExplosionA:100-103 deals damage (including knockback) FIRST,
	      THEN adds the directional blast impulse.

	      The damage impulse precedes the directional blast impulse.

	The blast impulse VALUE (exposure, distance) is pinned by
	test_explosion_beta_golden; here it is measured on a null-exploder TWIN
	(identical relative geometry, identical doubles) so the creeper case proves
	the ORDER (knockBack then impulse -- the lift is not clamped away) and the
	trajectory cases prove every tick after the hit through apex and landing.

	Cases (argv[1]): fall, tnt_up, tnt_down, creeper_up, repeated, jump,
	death_fall, death_slide.

	DEATH MOTION (death-motion-0927): EntityLiving.onUpdate:203-206 calls
	onLivingUpdate unconditionally; isMovementBlocked (:655-657, nonpositive health)
	only zeroes moveStrafing/moveForward/isJumping (:618-622), and
	movement with no steering input (:642) still runs the full dry tail -- so a
	mob in its 20-tick deathTime flop keeps falling and sliding exactly like
	a live body with no input. The death cases run the SAME ref_tick on the
	corpse, tick for tick, until it is reaped (after death tick 20, :155-159).
	RED on the S2/proof base 311a5a36 (AUDIT #18 froze the body where it
	died).
	All were RED on the S1 base 325a10e9 (engine GRAVITY 2.0 / *0.9 integrator,
	deferred knockback compose).
*/
#include <assert.h>
#include <math.h>
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
#include "entity/mob_motion.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"
#include "support/drain_quiesce.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 100 /* stone layer; walk surface at 101.0 */
#define FLOOR_TOP 101.0

static struct server_local s;
static int g_fail;

#define CHECK(c)                                                               \
	do {                                                                       \
		if(!(c)) {                                                             \
			fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail = 1;                                                        \
		}                                                                      \
	} while(0)

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

static void set(int x, int y, int z, int type) {
	server_world_set_block(&s.worlds[0], x, y, z,
						   (struct block_data) {.type = (uint8_t)type});
}

/* ---- the independent Beta reference ------------------------------------ */
struct ref {
	double pos[3], motion[3];
	double min[3], max[3];
	bool on_ground;
	float fall;
	int fall_damage; /* damage dealt on the last landing, 0 if none */
};

static void ref_from_mob(struct ref* r, const struct entity* e) {
	const struct mob_motion* m = &e->data.monster.motion_d;
	for(int i = 0; i < 3; i++) {
		r->pos[i] = m->pos[i];
		r->motion[i] = m->motion[i];
		r->min[i] = m->bb.min[i];
		r->max[i] = m->bb.max[i];
	}
	r->on_ground = e->on_ground;
	r->fall = e->data.monster.fall_distance;
	r->fall_damage = 0;
}

static void ref_tick(struct ref* r, bool jumping) {
	if(jumping && r->on_ground)
		r->motion[1] = (double)0.42F;
	/* Friction from the PREVIOUS onGround; stone slipperiness 0.6F. */
	float friction = r->on_ground ? 0.6F * 0.91F : 0.91F;
	double original_vertical_motion = r->motion[1];
	double dy = original_vertical_motion;
	if(dy < 0.0 && r->min[1] >= FLOOR_TOP) {
		double floor_clearance = FLOOR_TOP - r->min[1];
		if(floor_clearance > dy)
			dy = floor_clearance;
	}
	r->min[1] += dy;
	r->max[1] += dy;
	r->min[0] += r->motion[0];
	r->max[0] += r->motion[0];
	r->min[2] += r->motion[2];
	r->max[2] += r->motion[2];
	r->pos[0] = (r->min[0] + r->max[0]) / 2.0;
	r->pos[1] = r->min[1] + (double)0.0F - (double)0.0F;
	r->pos[2] = (r->min[2] + r->max[2]) / 2.0;
	r->on_ground = original_vertical_motion != dy && original_vertical_motion < 0.0;
	if(original_vertical_motion != dy)
		r->motion[1] = 0.0;
	r->fall_damage = 0;
	if(r->on_ground) {
		if(r->fall > 0.0F) {
			int d = (int)ceilf(r->fall - 3.0F);
			r->fall_damage = d > 0 ? d : 0;
		}
		r->fall = 0.0F;
	} else if(dy < 0.0) {
		r->fall = (float)((double)r->fall - dy);
	}
	r->motion[1] -= 0.08;
	r->motion[1] *= (double)0.98F;
	r->motion[0] *= (double)friction;
	r->motion[2] *= (double)friction;
}

static void ref_knockback(double motion[3], double dx, double dz) {
	float f = (float)sqrt(dx * dx + dz * dz);
	motion[0] /= 2.0;
	motion[1] /= 2.0;
	motion[2] /= 2.0;
	motion[0] -= dx / (double)f * (double)0.4F;
	motion[1] += (double)0.4F;
	motion[2] -= dz / (double)f * (double)0.4F;
	if(motion[1] > (double)0.4F)
		motion[1] = (double)0.4F;
}

static bool same_d(double a, double b) {
	return memcmp(&a, &b, sizeof a) == 0;
}

static uint64_t ulp_diff(double a, double b) {
	int64_t ia, ib;
	memcpy(&ia, &a, sizeof a);
	memcpy(&ib, &b, sizeof b);
	if(ia < 0)
		ia = INT64_MIN - ia;
	if(ib < 0)
		ib = INT64_MIN - ib;
	return ia > ib ? (uint64_t)(ia - ib) : (uint64_t)(ib - ia);
}

/* Compare the mob against the reference; returns false (and reports) on the
 * first differing component. */
static bool match(const char* tag, int t, const struct entity* e,
				  const struct ref* r) {
	const struct mob_motion* m = &e->data.monster.motion_d;
	for(int i = 0; i < 3; i++) {
		if(!same_d(m->pos[i], r->pos[i]) || !same_d(m->motion[i], r->motion[i])) {
			fprintf(stderr,
					"%s tick %d axis %d: mob pos %.17g motion %.17g, Beta pos "
					"%.17g motion %.17g\n",
					tag, t, i, m->pos[i], m->motion[i], r->pos[i],
					r->motion[i]);
			return false;
		}
	}
	if(e->on_ground != r->on_ground) {
		fprintf(stderr, "%s tick %d: onGround %d vs Beta %d\n", tag, t,
				(int)e->on_ground, (int)r->on_ground);
		return false;
	}
	return true;
}

/* ---- fixture ------------------------------------------------------------ */
static struct entity* spawn_pig(double x, double y, double z) {
	struct entity* e = server_local_spawn_monster(
		(vec3) {(float)x, (float)y, (float)z}, MONSTER_PIG, &s);
	assert(e);
	/* the natural-spawn random velocity is AI flavour; start at rest */
	mob_motion_set_velocity(e, &e->data.monster.motion_d, 0.0, 0.0, 0.0);
	return e;
}

/* Keep the pig's AI out of the motion: no path, no steer, and the knockback
 * steer-suppression window held open (no walk push). These are AI-field
 * writes only -- the motion is never touched by the fixture. */
static void quiet_ai(struct entity* e) {
	e->data.monster.path.length = 0;
	e->data.monster.path.index = 0;
	e->data.monster.direction[0] = 0.0f;
	e->data.monster.direction[1] = 0.0f;
	e->data.monster.knockback_time = 100;
}

static void host_near(double x, double z) {
	s.player.x = x + 20.0;
	s.player.y = FLOOR_TOP + 1.62;
	s.player.z = z;
	s.player.has_pos = true;
}

static bool tick_mob(struct entity* e) {
	assert(server_local_test_tick_entities(&s));
	struct entity** pp = dict_entity_get(s.entities, e->id);
	return pp && *pp == e;
}

/* Run up to `n` ticks comparing to the reference; stops after landing +2. */
static int run_compare(const char* tag, struct entity* e, struct ref* r,
					   int n, bool jumping, int* out_land_tick) {
	int landed = -1;
	for(int t = 0; t < n; t++) {
		quiet_ai(e);
		short hp_before = e->health;
		if(!tick_mob(e)) {
			fprintf(stderr, "%s: mob gone at tick %d\n", tag, t);
			return -1;
		}
		ref_tick(r, jumping);
		if(!match(tag, t, e, r))
			return -1;
		if(r->fall_damage > 0) {
			if(hp_before - e->health != r->fall_damage) {
				fprintf(stderr, "%s tick %d: fall damage %d vs Beta %d\n", tag,
						t, hp_before - e->health, r->fall_damage);
				return -1;
			}
		}
		if(landed < 0 && r->on_ground && t > 0)
			landed = t;
		if(!jumping && landed >= 0 && t >= landed + 2)
			break;
	}
	if(out_land_tick)
		*out_land_tick = landed;
	return 0;
}

/* ---- cases ------------------------------------------------------------------ */
static int case_fall(void) {
	host_near(20.5, 20.5);
	struct entity* e = spawn_pig(20.5, 110.0, 20.5);
	quiet_ai(e);
	struct ref r;
	ref_from_mob(&r, e);
	int land = -1;
	if(run_compare("fall", e, &r, 80, false, &land))
		return 1;
	CHECK(land > 0);
	printf("  fall: 9-block drop matches Beta tick for tick, landing @%d "
		   "(fall damage checked)\n",
		   land);
	return 0;
}

/* Blast geometry: the centre 2.5 blocks -X of the pig and `cy_rel` above
 * or below it (never in its own column: Beta's knockBack divides by the
 * horizontal distance), power 2 -- a ~6-point hit on the 10-point pig, and
 * a start low enough that the landing fall is survivable (a dead mob's body
 * kept falling on the death flop only from death-motion-0927 on; the cases
 * below were written against a survivable fall and still use one). */
#define BLAST_DX g_blast_dx
static double g_blast_dx = -2.5;
static int blast_case(const char* tag, double cy_rel) {
	host_near(20.5, 20.5);
	struct entity* e = spawn_pig(20.5, 104.0, 20.5);
	quiet_ai(e);
	/* two ticks of free fall first so the hit lands on a moving body */
	struct ref r;
	ref_from_mob(&r, e);
	for(int t = 0; t < 2; t++) {
		quiet_ai(e);
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, false);
		if(!match(tag, t, e, &r))
			return 1;
	}
	const struct mob_motion* m = &e->data.monster.motion_d;
	vec3 c = {(float)(m->pos[0] + BLAST_DX), (float)(m->pos[1] + cy_rel),
			  (float)m->pos[2]};
	double pre_my = m->motion[1];
	server_world_explode(&s.worlds[0], c, 2.0f);
	CHECK(cy_rel < 0.0 ? m->motion[1] > pre_my : m->motion[1] < pre_my);
	CHECK(m->motion[0] > 0.0); /* pushed away from the centre (+X) */
	CHECK(e->health > 0);
	/* the trajectory after the hit, through apex and landing */
	ref_from_mob(&r, e);
	int land = -1;
	if(run_compare(tag, e, &r, 120, false, &land))
		return 1;
	CHECK(land > 0);
	printf("  %s: post-blast trajectory matches Beta through %s and landing "
		   "@%d\n",
		   tag, cy_rel < 0.0 ? "the apex" : "the drop", land);
	return 0;
}

/* Creeper exploder: a twin pig takes the SAME blast geometry with a null
 * exploder; creeper motion must equal post-knockback motion plus the twin's blast increment.
 * The twin sits 8 blocks +Z (outside the power-2 blast box, same binade in
 * every coordinate), so every box offset rounds identically and the relative
 * geometry stays bit-identical through the shared history. */
static int creeper_hit(struct entity* e, struct entity* twin, double cy_rel,
					   double pre[3]) {
	const struct mob_motion* m = &e->data.monster.motion_d;
	const struct mob_motion* tm = &twin->data.monster.motion_d;
	for(int i = 0; i < 3; i++)
		pre[i] = m->motion[i];
	for(int i = 0; i < 3; i++)
		if(!same_d(pre[i], tm->motion[i])) {
			fprintf(stderr, "twin diverged before the hit (axis %d)\n", i);
			return 1;
		}
	vec3 c = {(float)(m->pos[0] + BLAST_DX), (float)(m->pos[1] + cy_rel),
			  (float)m->pos[2]};
	vec3 tc = {(float)(tm->pos[0] + BLAST_DX), (float)(tm->pos[1] + cy_rel),
			   (float)tm->pos[2]};
	struct entity* creeper = malloc(sizeof(*creeper));
	assert(creeper);
	memset(creeper, 0, sizeof(*creeper));
	entity_monster(990777, creeper, true, &s.worlds[0], MONSTER_CREEPER);
	creeper->teleport(creeper, c);
	(void)server_world_try_explode_by(&s.worlds[0], creeper, c, 2.0f);
	server_world_explode(&s.worlds[0], tc, 2.0f);
	double impulse[3], want[3];
	for(int i = 0; i < 3; i++) {
		impulse[i] = tm->motion[i] - pre[i];
		want[i] = pre[i];
	}
	const struct mob_motion* cm = &creeper->data.monster.motion_d;
	ref_knockback(want, cm->pos[0] - m->pos[0], cm->pos[2] - m->pos[2]);
	for(int i = 0; i < 3; i++)
		want[i] += impulse[i];
	free(creeper);
	/* The impulse is MEASURED by subtracting the twin's pre-hit motion, which carries
	 * one double rounding of its own; the production sum of knockback and blast
	 * is compared within that single measurement rounding (<= 2 ulp), the
	 * ORDER being what this proves (a clamp-first order differs by ~0.1). */
	for(int i = 0; i < 3; i++)
		if(ulp_diff(m->motion[i], want[i]) > 2) {
			fprintf(stderr,
					"creeper hit axis %d: motion %.17g, Beta knockBack-then-"
					"impulse %.17g (pre %.17g, impulse %.17g)\n",
					i, m->motion[i], want[i], pre[i], impulse[i]);
			return 1;
		}
	return 0;
}

static int case_creeper_up(void) {
	host_near(20.5, 20.5);
	struct entity* e = spawn_pig(20.5, 104.0, 20.5);
	struct entity* twin = spawn_pig(20.5, 104.0, 28.5);
	struct ref r, rt;
	ref_from_mob(&r, e);
	ref_from_mob(&rt, twin);
	for(int t = 0; t < 2; t++) {
		quiet_ai(e);
		quiet_ai(twin);
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, false);
		ref_tick(&rt, false);
		if(!match("creeper_up", t, e, &r) || !match("creeper_twin", t, twin, &rt))
			return 1;
	}
	double pre[3];
	if(creeper_hit(e, twin, -1.5, pre))
		return 1;
	/* Beta: the knockBack pop and the blast lift both land -- a lift that the
	 * old deferred 0.4 clamp would have cut. */
	CHECK(e->data.monster.motion_d.motion[1] > (double)0.4F
		  || e->data.monster.motion_d.motion[1] > pre[1]);
	ref_from_mob(&r, e);
	int land = -1;
	if(run_compare("creeper_up", e, &r, 120, false, &land))
		return 1;
	printf("  creeper_up: knockBack then impulse (Explosion.doExplosionA order), "
		   "trajectory matches Beta, landing @%d\n",
		   land);
	return 0;
}

static void creeper_blast(struct entity* e, double cy_rel, uint32_t cid) {
	const struct mob_motion* m = &e->data.monster.motion_d;
	vec3 c = {(float)(m->pos[0] + BLAST_DX), (float)(m->pos[1] + cy_rel),
			  (float)m->pos[2]};
	struct entity* creeper = malloc(sizeof(*creeper));
	assert(creeper);
	memset(creeper, 0, sizeof(*creeper));
	entity_monster(cid, creeper, true, &s.worlds[0], MONSTER_CREEPER);
	creeper->teleport(creeper, c);
	(void)server_world_try_explode_by(&s.worlds[0], creeper, c, 2.0f);
	free(creeper);
}

/* Two creeper hits mid-air. e and e2 share an identical history through the
 * first hit (both creeper-exploded at identical relative geometry); at the
 * second hit e takes the creeper, e2 the null-exploder twin blast, so
 * creeper_hit proves blast impulse added after knockback on a body still carrying the
 * first hit's momentum (the halving keeps half of it -- never an overwrite). */
static int case_repeated(void) {
	host_near(20.5, 20.5);
	/* 3.2 blocks off: two ~3-point hits leave the 10-point pig alive */
	g_blast_dx = -3.2;
	struct entity* e = spawn_pig(20.5, 108.0, 20.5);
	struct entity* e2 = spawn_pig(20.5, 108.0, 28.5);
	creeper_blast(e, -1.5, 990778);
	creeper_blast(e2, -1.5, 990779);
	for(int i = 0; i < 3; i++)
		if(!same_d(e->data.monster.motion_d.motion[i],
				   e2->data.monster.motion_d.motion[i])) {
			fprintf(stderr, "repeated: first hits diverged (axis %d)\n", i);
			return 1;
		}
	struct ref r, r2;
	ref_from_mob(&r, e);
	ref_from_mob(&r2, e2);
	/* 11 airborne ticks: past the 10-tick hurt-resistance window */
	for(int t = 0; t < 11; t++) {
		quiet_ai(e);
		quiet_ai(e2);
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, false);
		ref_tick(&r2, false);
		if(!match("repeated", t, e, &r) || !match("repeated_b", t, e2, &r2))
			return 1;
	}
	CHECK(!e->on_ground);
	double pre[3];
	if(creeper_hit(e, e2, -1.5, pre))
		return 1;
	ref_from_mob(&r, e);
	/* 15 more airborne ticks: the two hits plus the landing fall would
	 * exceed the pig's 10 health (a dead body was frozen by the engine's
	 * death flop), so the arc is compared up to just before touchdown. */
	for(int t = 0; t < 15; t++) {
		quiet_ai(e);
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, false);
		if(!match("repeated_after", t, e, &r))
			return 1;
		if(r.on_ground)
			break;
	}
	printf("  repeated: a second creeper hit mid-air composes knockBack on the "
		   "residual motion, then the impulse; the arc matches Beta\n");
	return 0;
}

static int case_jump(void) {
	host_near(20.5, 20.5);
	struct entity* e = spawn_pig(20.5, FLOOR_TOP, 20.5);
	/* settle onto the floor (one tick) */
	/* Two ticks: at rest ON the face with motion 0 the first move is zero
	 * (unchanged zero vertical travel is not grounding); gravity's -0.0784 lands it on the
	 * second -- Beta's own resting state. */
	struct ref r;
	ref_from_mob(&r, e);
	for(int t = 0; t < 2; t++) {
		quiet_ai(e);
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, false);
		if(!match("jump_settle", t, e, &r))
			return 1;
	}
	CHECK(e->on_ground);
	/* A path whose current waypoint is one block above the floored sum of box bottom and 0.5:
	 * EntityCreature.updatePlayerActionState sets isJumping, and every
	 * grounded tick jumps with double-promoted 0.42F vertical motion. Two blocks off so the waypoint is
	 * never reached (the push is suppressed). */
	double peak = 0.0;
	int grounded_ticks = 0;
	/* Keep random path replacement/skips out of this input-driven arc;
	 * EntityCreature.updatePlayerActionState rolls before deciding to jump. */
	jrand_set_seed(&e->gameplay_rng, 3);
	for(int t = 1; t < 40; t++) {
		quiet_ai(e);
		e->data.monster.path.points[0] = (struct path_waypoint) {22, 102, 20};
		e->data.monster.path.length = 1;
		e->data.monster.path.index = 0;
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, true);
		if(!match("jump", t, e, &r))
			return 1;
		if(r.pos[1] - FLOOR_TOP > peak)
			peak = r.pos[1] - FLOOR_TOP;
		if(r.on_ground)
			grounded_ticks++;
	}
	CHECK(peak > 1.2 && peak < 1.3); /* Beta's 0.42F arc apex ~1.25 */
	CHECK(grounded_ticks >= 2);
	printf("  jump: (double)0.42F arc matches Beta tick for tick, apex %.4f\n",
		   peak);
	return 0;
}

/* ---- death motion ------------------------------------------------------
 * The corpse is compared against ref_tick every tick it still exists; the
 * reap after death tick 20 must come after the body has landed. */
static int run_corpse(const char* tag, struct entity* e, struct ref* r,
					  int* out_land, int* out_ticks) {
	int land = -1, t = 0;
	for(; t < 40; t++) {
		if(!tick_mob(e))
			break; /* reaped */
		ref_tick(r, false);
		if(!match(tag, t, e, r))
			return 1;
		CHECK(e->health <= 0 && e->data.monster.death_time == t + 1);
		if(land < 0 && r->on_ground)
			land = t;
	}
	*out_land = land;
	*out_ticks = t;
	return 0;
}

static int case_death_fall(void) {
	host_near(20.5, 20.5);
	struct entity* e = spawn_pig(20.5, 107.0, 20.5);
	quiet_ai(e);
	struct ref r;
	ref_from_mob(&r, e);
	/* killed between ticks, in mid-air (the lethal hit's own knockBack is
	 * the slide case) */
	e->health = 0;
	int land = -1, ticks = 0;
	if(run_corpse("death_fall", e, &r, &land, &ticks))
		return 1;
	CHECK(land > 0 && land < ticks);
	CHECK(ticks == 20); /* 20 moving flop ticks, reaped on deathTime 21 */
	CHECK(r.pos[1] == FLOOR_TOP);
	printf("  death_fall: a corpse falls 6 blocks tick for tick like Beta, "
		   "lands @%d, reaped after %d flop ticks\n", land, ticks);
	return 0;
}

static int case_death_slide(void) {
	host_near(20.5, 20.5);
	struct entity* e = spawn_pig(20.5, FLOOR_TOP, 20.5);
	quiet_ai(e);
	/* settle onto the floor (a body at rest on a face needs two ticks) */
	struct ref r;
	ref_from_mob(&r, e);
	for(int t = 0; t < 3; t++) {
		quiet_ai(e);
		if(!tick_mob(e))
			return 1;
		ref_tick(&r, false);
		if(!match("death_slide settle", t, e, &r))
			return 1;
	}
	CHECK(e->on_ground);
	/* the lethal hit: knockBack from an attacker 1 block -X, then death */
	struct mob_motion* m = &e->data.monster.motion_d;
	mob_motion_knockback(e, m, -1.0, 0.0, NULL);
	ref_knockback(r.motion, -1.0, 0.0);
	CHECK(same_d(m->motion[0], r.motion[0]) && same_d(m->motion[1], r.motion[1]));
	e->health = 0;
	double x0 = m->pos[0];
	int land = -1, ticks = 0;
	if(run_corpse("death_slide", e, &r, &land, &ticks))
		return 1;
	CHECK(land > 0);
	CHECK(r.pos[0] - x0 > 0.5); /* the corpse was carried by the hit */
	printf("  death_slide: the killing hit's knockBack carries the corpse "
		   "%.3f blocks (hop + Beta friction), lands @%d\n", r.pos[0] - x0,
		   land);
	return 0;
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "";
	char tag[64];
	snprintf(tag, sizeof tag, "test_mob_traj_%s_tmp", which);
	test_fixture_dir(tag, s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x7A1EC7EDu;
	dict_entity_init(s.entities);
	for(int cz = -2; cz <= 4; cz++)
		for(int cx = -2; cx <= 16; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	/* a stone floor under every test column, open air above */
	for(int x = 0; x < 240; x++)
		for(int z = 10; z < 32; z++) {
			set(x, FLOOR_Y, z, BLOCK_STONE);
			for(int y = FLOOR_Y + 1; y < 124; y++)
				set(x, y, z, BLOCK_AIR);
		}

	int rc = 0;
	if(!strcmp(which, "fall"))
		rc = case_fall();
	else if(!strcmp(which, "tnt_up"))
		rc = blast_case("tnt_up", -1.5);
	else if(!strcmp(which, "tnt_down"))
		rc = blast_case("tnt_down", 1.5);
	else if(!strcmp(which, "creeper_up"))
		rc = case_creeper_up();
	else if(!strcmp(which, "repeated"))
		rc = case_repeated();
	else if(!strcmp(which, "jump"))
		rc = case_jump();
	else if(!strcmp(which, "death_fall"))
		rc = case_death_fall();
	else if(!strcmp(which, "death_slide"))
		rc = case_death_slide();
	else {
		fprintf(stderr, "unknown case '%s'\n", which);
		rc = 2;
	}
	CHECK(mob_motion_bypass_count() == 0);

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	if(rc)
		return rc;
	if(g_fail)
		return 1;
	printf("test_mob_trajectory_golden %s: PASS\n", which);
	return 0;
}
