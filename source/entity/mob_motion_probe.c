/* mob_motion_2m hardware proof checkers -- see mob_motion_probe.h. */

#include "mob_motion_probe.h"

#include <assert.h>
#include <math.h>

/* Beta's dry tail, verbatim: motionY -= 0.08D; motionY *= (double)0.98F. */
static double beta_dry_vy(double vy) {
	return (vy - 0.08) * (double)0.98F;
}

double mm_probe_jump_tick_vy(void) {
	return beta_dry_vy((double)0.42F);
}

int mm_probe_recur_check(const struct mm_sample* prev,
						 const struct mm_sample* cur) {
	assert(prev && cur);
	if(!prev->valid || !cur->valid)
		return 0;
	/* Clean = the tick ran the DRY branch with nothing but gravity on Y: not
	 * standing (no jump() possible), no landing/head bump (the clip zeroes
	 * motionY), no liquid or web branch, no fresh hit (knockBack / blast),
	 * alive. */
	if(prev->ground || cur->ground || cur->coll_v || prev->wet || cur->wet
	   || cur->web || cur->hurt_time > prev->hurt_time || cur->health <= 0)
		return 0;
	return cur->vy == beta_dry_vy(prev->vy) ? 1 : -1;
}

void mm_probe_recur_feed(struct mm_recur* r, const struct mm_sample* prev,
						 const struct mm_sample* cur) {
	assert(r);
	int v = mm_probe_recur_check(prev, cur);
	if(v > 0)
		r->ok++;
	else if(v < 0)
		r->bad++;
}

bool mm_probe_recur_pass(const struct mm_recur* r) {
	assert(r);
	return r->bad == 0 && r->ok >= MM_RECUR_MIN;
}

void mm_probe_lift_feed(struct mm_lift* l, const struct mm_sample* prev,
						const struct mm_sample* cur) {
	assert(l && prev && cur);
	if(!cur->valid) {
		if(l->hit_seen)
			l->died = true; /* gone mid-window: never a pass */
		return;
	}
	if(!l->hit_seen) {
		if(prev->valid && cur->hurt_time > prev->hurt_time) {
			l->hit_seen = true;
			l->pre_y = prev->y;
			l->hit_vy = cur->vy;
			l->apex = cur->y;
			l->ticks_after = 0;
			if(cur->health <= 0)
				l->died = true;
		}
		return;
	}
	if(cur->health <= 0)
		l->died = true;
	if(l->ticks_after < MM_LIFT_WINDOW) {
		l->ticks_after++;
		if(cur->y > l->apex)
			l->apex = cur->y;
	}
}

bool mm_probe_lift_pass(const struct mm_lift* l) {
	assert(l);
	return l->hit_seen && !l->died && l->apex - l->pre_y >= MM_LIFT_MIN_RISE
		&& l->hit_vy >= MM_LIFT_VY_MIN && l->hit_vy <= MM_LIFT_VY_MAX;
}

static uint32_t to_milli(double d) {
	if(!(d > 0.0))
		return 0;
	if(d > 4000000.0)
		return 4000000000u;
	return (uint32_t)lround(d * 1000.0);
}

uint32_t mm_probe_lift_rise_milli(const struct mm_lift* l) {
	assert(l);
	return l->hit_seen ? to_milli(l->apex - l->pre_y) : 0;
}

void mm_probe_step_feed(struct mm_step* st, const struct mm_sample* prev,
						const struct mm_sample* cur) {
	assert(st && prev && cur);
	if(!cur->valid || st->landed)
		return;
	if(!st->started) {
		/* y0 = the first tick the zombie is found resting on the trench floor. */
		if(cur->ground) {
			st->started = true;
			st->y0 = cur->y;
			st->apex = cur->y;
		}
		return;
	}
	if(!st->jump_seen) {
		if(prev->valid && prev->ground && !cur->ground
		   && cur->vy == mm_probe_jump_tick_vy()) {
			st->jump_seen = true;
			st->apex = cur->y;
		}
		return;
	}
	if(cur->y > st->apex)
		st->apex = cur->y;
	if(cur->ground && fabs(cur->y - (st->y0 + 1.0)) < 1.0e-9) {
		st->landed = true;
		st->land_y = cur->y;
	}
}

bool mm_probe_step_pass(const struct mm_step* st) {
	assert(st);
	double apex = st->apex - st->y0;
	return st->started && st->jump_seen && st->landed
		&& st->land_y == st->y0 + 1.0 && apex >= MM_STEP_APEX_MIN
		&& apex <= MM_STEP_APEX_MAX;
}

uint32_t mm_probe_step_milli(const struct mm_step* st) {
	assert(st);
	return st->landed ? to_milli(st->land_y - st->y0) : 0;
}
