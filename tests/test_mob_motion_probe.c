/* test_mob_motion_probe -- the mob_motion_2m checkers (lane C, mob motion
 * units S2 proof; source/entity/mob_motion_probe.[ch]). Each check is fed a
 * trace built from Beta's own arithmetic that must PASS, and a bad trace that
 * must FAIL (the Lead's rule: every new check has a selftest that fails on a
 * bad trace).
 *
 *   B  the dry recurrence: vy' == (vy - 0.08D) * (double)0.98F bit-for-bit
 *      [EntityLiving.moveEntityWithHeading :531-534]; a 1-ulp drift is a miss,
 *      and the old engine's (vy - GRAVITY*dt) shape is a miss.
 *   A  the TNT lift: survive, rise >= 0.5, hit-tick vy in [0.10, 0.60].
 *   C  the step jump: jump tick vy == ((double)0.42F - 0.08D) * (double)0.98F
 *      [EntityLiving.jump :592-595], rest at exactly y0 + 1.0. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "entity/mob_motion_probe.h"

static unsigned failures;
static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static struct mm_sample air(double y, double vy) {
	return (struct mm_sample) {.valid = true, .y = y, .vy = vy, .health = 20};
}

static double dry(double vy) {
	return (vy - 0.08) * (double)0.98F;
}

/* A Beta free fall from rest at y: n samples of the airborne dry branch. */
static void fall_trace(struct mm_sample* out, int n, double y) {
	double vy = 0.0;
	for(int i = 0; i < n; i++) {
		vy = dry(vy);
		y += vy; /* position is informational for B */
		out[i] = air(y, vy);
	}
}

static void test_recurrence(void) {
	struct mm_sample t[20];
	fall_trace(t, 20, 80.0);
	struct mm_recur r = {0};
	for(int i = 1; i < 20; i++)
		mm_probe_recur_feed(&r, &t[i - 1], &t[i]);
	check(r.ok == 19 && r.bad == 0 && mm_probe_recur_pass(&r),
		  "B good: a Beta free fall is 19 exact clean ticks");

	/* bad: one tick 1 ulp off */
	struct mm_sample u[20];
	memcpy(u, t, sizeof u);
	u[10].vy = nextafter(u[10].vy, 0.0);
	struct mm_recur rb = {0};
	for(int i = 1; i < 20; i++)
		mm_probe_recur_feed(&rb, &u[i - 1], &u[i]);
	check(rb.bad >= 1 && !mm_probe_recur_pass(&rb),
		  "B bad: a 1-ulp drift on one tick is a recurrence miss");

	/* bad: the retired engine shape (gravity-then-drag, 25x units) */
	struct mm_sample e0 = air(80.0, -0.2), e1 = air(79.8, (-0.2 - 0.08) * 0.98);
	check(mm_probe_recur_check(&e0, &e1) == -1,
		  "B bad: (vy-0.08)*0.98 in DOUBLE 0.98 (not (double)0.98F) is a miss");
	e1.vy = -0.2 * 0.91 - 0.08;
	check(mm_probe_recur_check(&e0, &e1) == -1,
		  "B bad: drag-then-gravity is a miss");

	/* bad: too few clean ticks */
	struct mm_recur rf = {0};
	for(int i = 1; i < 8; i++)
		mm_probe_recur_feed(&rf, &t[i - 1], &t[i]);
	check(rf.ok == 7 && !mm_probe_recur_pass(&rf),
		  "B bad: 7 clean ticks is not enough (>= 8)");

	/* skips: never counted either way */
	struct mm_sample a = t[3], b = t[4];
	b.ground = true;
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: landing tick");
	b = t[4];
	b.coll_v = true;
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: vertical collision");
	b = t[4];
	b.wet = true;
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: liquid branch");
	b = t[4];
	b.web = true;
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: web");
	b = t[4];
	b.hurt_time = 10;
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: a fresh hit");
	a.ground = true;
	b = t[4];
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: jump possible (was grounded)");
	a = t[3];
	b = t[4];
	b.valid = false;
	check(mm_probe_recur_check(&a, &b) == 0, "B skip: mob gone");
}

/* The PC capture (c9b022c9): pre 6.022451, hit vy 0.318832, apex 7.165150. */
static void lift_trace(struct mm_sample* s, int* n, double hit_vy, int hp_after,
					   double rise) {
	int k = 0;
	struct mm_sample w = air(6.0, 0.0);
	w.wet = true;
	for(; k < 5; k++)
		s[k] = w;
	s[k] = air(6.022451, 0.006925);
	s[k].wet = true;
	k++;
	struct mm_sample h = air(6.069375, hit_vy);
	h.hurt_time = 10;
	h.health = hp_after;
	s[k++] = h;
	double y = 6.069375, vy = hit_vy;
	const double cap = 6.022451 + rise; /* the trace's apex is the requested rise */
	for(int i = 0; i < 6; i++) {
		vy = dry(vy);
		y += vy;
		struct mm_sample a = air(y < cap ? y : cap, vy);
		a.hurt_time = 9 - i;
		a.health = hp_after;
		s[k++] = a;
	}
	/* force the apex to the requested rise above pre_y */
	s[k] = air(6.022451 + rise, 0.0);
	s[k].health = hp_after;
	k++;
	*n = k;
}

static bool lift_run(double hit_vy, int hp_after, double rise, bool vanish) {
	struct mm_sample s[32];
	int n = 0;
	lift_trace(s, &n, hit_vy, hp_after, rise);
	if(vanish)
		s[n++] = (struct mm_sample) {0};
	struct mm_lift l = {0};
	struct mm_sample prev = {0};
	for(int i = 0; i < n; i++) {
		mm_probe_lift_feed(&l, &prev, &s[i]);
		prev = s[i];
	}
	return mm_probe_lift_pass(&l);
}

static void test_lift(void) {
	check(lift_run(0.318832, 7, 1.142699, false), "A good: the PC capture's lift");
	check(!lift_run(0.318832, 7, 0.499, false), "A bad: rise 0.499 < 0.5");
	check(!lift_run(0.318832, -1, 1.142699, false), "A bad: the pigman died");
	check(lift_run(0.244262, 7, 0.981661, false),
		  "A good: the PC rig env run's lift (bob phase: hit vy 0.244, rise 0.98)");
	check(!lift_run(0.61, 7, 1.142699, false), "A bad: hit vy 0.61 > 0.60");
	check(!lift_run(0.09, 7, 1.142699, false), "A bad: hit vy 0.09 < 0.10");
	check(!lift_run(0.318832 * 25.0, 7, 1.142699, false),
		  "A bad: an engine-unit (x25) impulse is out of band");
	check(!lift_run(0.318832, 7, 1.142699, true), "A bad: mob vanished mid-window");
	struct mm_lift none = {0};
	check(!mm_probe_lift_pass(&none), "A bad: no hit ever seen");
}

static bool step_run(double land_dy, bool exact_jump) {
	struct mm_step st = {0};
	struct mm_sample prev = {0};
	const double y0 = 4.0;
	struct mm_sample g = air(y0, (0.0 - 0.08) * (double)0.98F);
	g.ground = true;
	g.coll_v = true;
	struct mm_sample seq[20];
	int n = 0;
	seq[n++] = g;
	seq[n++] = g;
	double y = y0, vy = exact_jump ? mm_probe_jump_tick_vy() : 0.42 * 0.98;
	y += (double)0.42F;
	seq[n++] = air(y, vy);
	for(int i = 0; i < 4; i++) {
		y += vy;
		vy = dry(vy);
		seq[n++] = air(y, vy);
	}
	struct mm_sample land = air(y0 + land_dy, 0.0);
	land.ground = true;
	land.coll_v = true;
	seq[n++] = land;
	for(int i = 0; i < n; i++) {
		mm_probe_step_feed(&st, &prev, &seq[i]);
		prev = seq[i];
	}
	return mm_probe_step_pass(&st) && mm_probe_step_milli(&st) == 1000;
}

static void test_step(void) {
	check(mm_probe_jump_tick_vy() == ((double)0.42F - 0.08) * (double)0.98F,
		  "C: jump tick vy is Beta's jump() then the dry tail");
	check(step_run(1.0, true), "C good: jump() arc, rest at exactly y0+1");
	check(!step_run(1.0, false), "C bad: a non-Beta jump impulse is not a jump");
	check(!step_run(1.0 + 1.0e-6, true), "C bad: rest 1e-6 off the step top");
	check(!step_run(0.0, true), "C bad: fell back, never climbed");
	struct mm_step none = {0};
	check(!mm_probe_step_pass(&none), "C bad: nothing observed");
}

int main(void) {
	test_recurrence();
	test_lift();
	test_step();
	if(failures) {
		printf("FAIL test_mob_motion_probe (%u failures)\n", failures);
		return 1;
	}
	puts("PASS test_mob_motion_probe");
	return 0;
}
