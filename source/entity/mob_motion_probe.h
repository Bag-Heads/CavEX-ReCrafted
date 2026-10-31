#ifndef MOB_MOTION_PROBE_H
#define MOB_MOTION_PROBE_H

/* mob_motion_2m hardware proof (lane C, docs/intent/mob-motion-units.md; plan
 * tmp/jobs/split-dimensions-plan-0924/out/MOTION-S2-WII-PROOF-PLAN.md).
 *
 * Pure checkers over per-tick samples of a watched mob's AUTHORITATIVE doubles
 * (struct mob_motion) plus the flags Beta's moveEntityWithHeading branches on.
 * No world access, no allocation: the server_local rig feeds samples, the
 * verdict rides the heartbeat, and tests/test_mob_motion_probe.c feeds good AND
 * bad traces. Three recorded checks:
 *
 *   A  LIFT   a TNT blast under a swimming pigman lifts it: the pigman survives,
 *             rises >= MM_LIFT_MIN_RISE above its pre-blast y, and its motionY
 *             on the hit tick is in [MM_LIFT_VY_MIN, MM_LIFT_VY_MAX] (block/
 *             tick units, not the retired engine units).
 *   B  RECUR  on every clean airborne-dry tick, motionY follows Beta's dry
 *             integrator BIT-FOR-BIT: vy' == (vy - 0.08D) * (double)0.98F
 *             [EntityLiving.moveEntityWithHeading :531-534]. >= MM_RECUR_MIN
 *             clean ticks, zero misses.
 *   C  STEP   a path-driven zombie climbs a one-block step through Beta's
 *             jump(): the jump tick's post-tick motionY is exactly
 *             ((double)0.42F - 0.08D) * (double)0.98F, it later rests grounded
 *             at exactly y0 + 1.0, and its apex above y0 is in [1.0, 1.35]. */

#include <stdbool.h>
#include <stdint.h>

#define MM_PROBE_BIT_LIFT 1u
#define MM_PROBE_BIT_RECUR 2u
#define MM_PROBE_BIT_STEP 4u
#define MM_PROBE_BITS_ALL 7u

/* Evidence states, the TNT fixture's vocabulary. */
enum mm_probe_state {
	MM_PROBE_INACTIVE = 0,
	MM_PROBE_PENDING = 1,
	MM_PROBE_PASS = 2,
	MM_PROBE_FAIL = 3,
	MM_PROBE_SKIP = 4,
};

/* The lift floor. The water tail after the blast (0.8F drag, and Beta's 0.3F
 * climb-out whenever the body is pushed into the tube wall) makes the rise a
 * distribution, not a constant: 0.76..1.39 blocks over 14 PC runs of the
 * windowed prime (server_local.c mob_motion_2m rig). 0.5 keeps a 0.26 margin
 * under the observed minimum. Telling a blast from the surface bob is the hit
 * vy band's job below (a blast: 0.27..0.38 measured; the bob: under 0.05). */
#define MM_LIFT_MIN_RISE 0.5
/* The hit-tick motionY band is a UNITS check, not a value pin: it rejects the
 * retired engine-unit impulse (x25 / GRAVITY-2.0 scale) while tolerating the
 * pigman's bob phase at the blast (the distance, hence the impulse, moves
 * with it: measured 0.244 in the PC rig env run, 0.319 in the c9b022c9 PC
 * capture). The exact per-tick blast arithmetic is pinned byte-exact by
 * test_mob_trajectory_golden (tnt_up / tnt_down). */
#define MM_LIFT_VY_MIN 0.10
#define MM_LIFT_VY_MAX 0.60
#define MM_LIFT_WINDOW 30 /* ticks after the hit that the apex is sought in */
#define MM_RECUR_MIN 8u
#define MM_STEP_APEX_MIN 1.0
#define MM_STEP_APEX_MAX 1.35

/* One tick's post-tick view of a watched mob. */
struct mm_sample {
	bool valid;     /* the mob exists this tick */
	double y, vy;   /* motion_d.pos[1], motion_d.motion[1] */
	bool ground;    /* onGround as this tick's moveEntity left it */
	bool coll_v;    /* isCollidedVertically this tick */
	bool wet;       /* inWater / in lava this tick */
	bool web;       /* isInWeb this tick */
	int hurt_time;  /* hurtTime (a rise = a fresh hit) */
	int health;
};

/* B. (prev, cur) one tick apart. 0 = not a clean tick (skipped), 1 = exact,
 * -1 = a clean tick whose motionY is NOT Beta's dry recurrence. */
int mm_probe_recur_check(const struct mm_sample* prev,
						 const struct mm_sample* cur);

struct mm_recur {
	uint32_t ok, bad;
};
void mm_probe_recur_feed(struct mm_recur* r, const struct mm_sample* prev,
						 const struct mm_sample* cur);
bool mm_probe_recur_pass(const struct mm_recur* r);

/* A. */
struct mm_lift {
	bool hit_seen, died;
	double pre_y, hit_vy, apex;
	int ticks_after;
};
void mm_probe_lift_feed(struct mm_lift* l, const struct mm_sample* prev,
						const struct mm_sample* cur);
bool mm_probe_lift_pass(const struct mm_lift* l);
/* (apex - pre_y) in milli-blocks, 0 before the hit. */
uint32_t mm_probe_lift_rise_milli(const struct mm_lift* l);

/* C. */
struct mm_step {
	bool started, jump_seen, landed;
	double y0, apex, land_y;
};
void mm_probe_step_feed(struct mm_step* st, const struct mm_sample* prev,
						const struct mm_sample* cur);
bool mm_probe_step_pass(const struct mm_step* st);
/* (land_y - y0) in milli-blocks, 0 before landing. */
uint32_t mm_probe_step_milli(const struct mm_step* st);

/* The post-tick motionY of Beta's jump tick (jump() then move then gravity). */
double mm_probe_jump_tick_vy(void);

#endif
