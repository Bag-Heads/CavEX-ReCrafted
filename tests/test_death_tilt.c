/*
	test_death_tilt -- next-gaps-0924/G8: Beta 1.7.3 death tilt
	(RenderLiving.rotateCorpse) for mobs and player bodies.

	Beta source (mcb173 src_original/net/minecraft/src):
	  RenderLiving.java:36-39   doRenderLiving: glTranslatef(entity pos) (func_22012_b,
	                            :117-119), then rotateCorpse(entity, ..., yawOffset, partial)
	  RenderLiving.java:121-133 rotateCorpse:
	      glRotatef(180 - yawOffset, 0,1,0);
	      if(deathTime > 0) {
	          f = ((float)deathTime + partial - 1.0F) / 20.0F * 1.6F;
	          f = MathHelper.sqrt_float(f); if(f > 1.0F) f = 1.0F;
	          glRotatef(f * getDeathMaxRotation(entity), 0,0,1);
	      }
	  RenderLiving.java:154-156 getDeathMaxRotation = 90
	  RenderSpider.java:11-13,31-33 spider override = 180
	  RenderSquid.java:14-22,40-42 squid overrides rotateCorpse with NO deathTime term
	  RenderPlayer.java:238-247,265-267 awake player -> super.rotateCorpse (90)

	Pinned here (binding the shared production helpers directly):
	  1. ANGLE GOLDEN: the sqrt curve per mob type vs deathTime + partial.
	  2. PIVOT + AXIS: through the production body composition
	     (render_entity_body_mv on top of render_entity_death_tilt), the feet
	     centre never moves, a point on the forward axis only follows the
	     roll's own axis (stays on it), and the head swings toward the body's
	     LEFT (Beta: +angle about the yawed frame's Z moves +Y toward -X; the
	     frame faces -Z) -- at every body yaw.
	  3. A living mob (roll 0) keeps its matrix bit-identical.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "entity/combat.h"
#include "entity/monsters_object.h"
#include "graphics/render_entity_anchor.h"

static int failures = 0;

static void check_close(float got, float want, float tol, const char* what) {
	if(!(fabsf(got - want) <= tol)) {
		printf("FAIL %s: got %.6f want %.6f\n", what, got, want);
		failures++;
	}
}

// Java-float transcription of RenderLiving.rotateCorpse's angle, kept as an
// independent reference next to the hand-derived anchors below.
static float beta_reference(int deathTime, float partial, float maxRot) {
	if(deathTime <= 0)
		return 0.0F;
	float f = ((float)deathTime + partial - 1.0F) / 20.0F * 1.6F;
	f = (float)sqrt((double)f); // MathHelper.sqrt_float
	if(f > 1.0F)
		f = 1.0F;
	return f * maxRot;
}

static void test_angle_golden(void) {
	char what[96];
	// Hand-derived anchors (default getDeathMaxRotation 90):
	//   dt 1 td 0   -> f 0                    -> 0
	//   dt 1 td .5  -> sqrt(0.04)  = 0.2      -> 18
	//   dt 6 td 0   -> sqrt(0.4)   = 0.632456 -> 56.921
	//   dt 11 td 0  -> sqrt(0.8)   = 0.894427 -> 80.498
	//   dt 13 td 0  -> sqrt(0.96)  = 0.979796 -> 88.182
	//   dt 13 td .5 -> sqrt(1.0)              -> 90 (flat at 12.5 ticks)
	//   dt 14 td 0  -> sqrt(1.04) clamped 1   -> 90
	static const struct { int dt; float td; float deg90; } a[] = {
		{0, 0.0F, 0.0F},     {0, 0.9F, 0.0F},     {1, 0.0F, 0.0F},
		{1, 0.5F, 18.0F},    {6, 0.0F, 56.92100F}, {11, 0.0F, 80.49845F},
		{13, 0.0F, 88.18163F}, {13, 0.5F, 90.0F}, {14, 0.0F, 90.0F},
		{20, 0.99F, 90.0F},
	};
	for(size_t i = 0; i < sizeof(a) / sizeof(a[0]); i++) {
		snprintf(what, sizeof(what), "zombie dt=%d td=%.2f", a[i].dt, (double)a[i].td);
		check_close(mob_death_roll_deg_for(MONSTER_ZOMBIE, a[i].dt, a[i].td),
					a[i].deg90, 1e-3F, what);
		snprintf(what, sizeof(what), "player dt=%d td=%.2f", a[i].dt, (double)a[i].td);
		check_close(mob_death_roll_deg(a[i].dt, a[i].td), a[i].deg90, 1e-3F, what);
		snprintf(what, sizeof(what), "spider dt=%d td=%.2f", a[i].dt, (double)a[i].td);
		check_close(mob_death_roll_deg_for(MONSTER_SPIDER, a[i].dt, a[i].td),
					a[i].deg90 * 2.0F, 2e-3F, what);
		snprintf(what, sizeof(what), "squid dt=%d td=%.2f", a[i].dt, (double)a[i].td);
		check_close(mob_death_roll_deg_for(MONSTER_SQUID, a[i].dt, a[i].td),
					0.0F, 0.0F, what);
	}

	// getDeathMaxRotation per type.
	check_close(mob_death_max_rotation_deg(MONSTER_SPIDER), 180.0F, 0.0F, "max spider");
	check_close(mob_death_max_rotation_deg(MONSTER_SQUID), 0.0F, 0.0F, "max squid");
	static const int ninety[] = {
		MONSTER_ZOMBIE, MONSTER_CREEPER, MONSTER_SKELETON, MONSTER_PIG,
		MONSTER_COW, MONSTER_CHICKEN, MONSTER_SHEEP, MONSTER_SLIME_SMALL,
		MONSTER_SLIME_MEDIUM, MONSTER_SLIME_BIG, MONSTER_PIG_ZOMBIE,
		MONSTER_GHAST, MONSTER_WOLF, MONSTER_GIANT,
	};
	for(size_t i = 0; i < sizeof(ninety) / sizeof(ninety[0]); i++) {
		snprintf(what, sizeof(what), "max id %d", ninety[i]);
		check_close(mob_death_max_rotation_deg(ninety[i]), 90.0F, 0.0F, what);
	}

	// Dense sweep against the Java-float transcription (every tick, 8 partials).
	for(int dt = 0; dt <= 21; dt++) {
		for(int k = 0; k < 8; k++) {
			float td = (float)k / 8.0F;
			snprintf(what, sizeof(what), "sweep zombie dt=%d td=%.3f", dt, (double)td);
			check_close(mob_death_roll_deg_for(MONSTER_ZOMBIE, dt, td),
						beta_reference(dt, td, 90.0F), 1e-4F, what);
			snprintf(what, sizeof(what), "sweep spider dt=%d td=%.3f", dt, (double)td);
			check_close(mob_death_roll_deg_for(MONSTER_SPIDER, dt, td),
						beta_reference(dt, td, 180.0F), 2e-4F, what);
		}
	}
}

static void xform(mat4 m, float x, float y, float z, vec3 out) {
	vec4 v = {x, y, z, 1.0F}, r;
	glm_mat4_mulv(m, v, r);
	out[0] = r[0];
	out[1] = r[1];
	out[2] = r[2];
}

static void check_vec(const vec3 got, const vec3 want, const char* what) {
	float d = fabsf(got[0] - want[0]) + fabsf(got[1] - want[1])
		+ fabsf(got[2] - want[2]);
	if(!(d <= 1e-4F)) {
		printf("FAIL %s: got (%.4f,%.4f,%.4f) want (%.4f,%.4f,%.4f)\n", what,
			   got[0], got[1], got[2], want[0], want[1], want[2]);
		failures++;
	}
}

// Production composition: entity_*_render builds model = T(feet) * tilt,
// then the per-mob fn composes render_entity_body_mv on top (view = I here).
static void body_matrix(vec3 feet, float bodyYawDeg, float rollDeg, mat4 out) {
	mat4 model;
	glm_translate_make(model, feet);
	render_entity_death_tilt(model, bodyYawDeg, rollDeg);
	render_entity_body_mv(model, (vec3) {0.5F, 0.75F, 0.5F}, bodyYawDeg, out);
}

static void test_pivot_and_axis(void) {
	static const float yaws[] = {0.0F, 37.0F, 90.0F, 180.0F, 263.5F, -45.0F};
	static const float rolls[] = {18.0F, 56.921F, 90.0F, 180.0F};
	vec3 feet = {10.25F, 64.0F, -3.5F};
	vec3 up = {0.0F, 1.0F, 0.0F};
	char what[128];
	for(size_t yi = 0; yi < sizeof(yaws) / sizeof(yaws[0]); yi++) {
		float b = yaws[yi];
		// The body's forward in world space, from the untilted composition:
		// model front is -Z (render_entity_anchor.h), feet centre (0.5,0,0.5).
		mat4 upright;
		body_matrix(feet, b, 0.0F, upright);
		vec3 fwd_pt, fwd;
		xform(upright, 0.5F, 0.0F, -0.5F, fwd_pt);
		glm_vec3_sub(fwd_pt, feet, fwd);
		vec3 left;
		// Beta's roll axis is the yawed frame's +Z = -forward, so a +angle
		// takes up toward (-fwd) x up = up x fwd: facing -Z, +Y up -> -X.
		glm_vec3_cross(up, fwd, left);
		glm_vec3_normalize(left);
		for(size_t ri = 0; ri < sizeof(rolls) / sizeof(rolls[0]); ri++) {
			float th = glm_rad(rolls[ri]);
			mat4 m;
			body_matrix(feet, b, rolls[ri], m);
			vec3 got, want;

			// Pivot: the feet centre (Beta's glTranslatef(entity pos) origin).
			xform(m, 0.5F, 0.0F, 0.5F, got);
			snprintf(what, sizeof(what), "feet pivot yaw=%.1f roll=%.1f", (double)b, (double)rolls[ri]);
			check_vec(got, feet, what);

			// Head top 1.5 above the feet swings toward the body's left.
			xform(m, 0.5F, 1.5F, 0.5F, got);
			for(int k = 0; k < 3; k++)
				want[k] = feet[k] + 1.5F * (cosf(th) * up[k] + sinf(th) * left[k]);
			snprintf(what, sizeof(what), "head swing yaw=%.1f roll=%.1f", (double)b, (double)rolls[ri]);
			check_vec(got, want, what);

			// A point on the forward (roll) axis at feet height does not move.
			xform(m, 0.5F, 0.0F, -0.5F, got);
			snprintf(what, sizeof(what), "fwd axis yaw=%.1f roll=%.1f", (double)b, (double)rolls[ri]);
			check_vec(got, fwd_pt, what);
		}
	}

	// A living mob (roll 0) is untouched: bit-identical matrix.
	mat4 model, ref;
	glm_translate_make(model, feet);
	glm_mat4_copy(model, ref);
	render_entity_death_tilt(model, 123.0F, 0.0F);
	if(memcmp(model, ref, sizeof(mat4)) != 0) {
		printf("FAIL roll 0 changed the model matrix\n");
		failures++;
	}
}

int main(void) {
	test_angle_golden();
	test_pivot_and_axis();
	if(failures) {
		printf("test_death_tilt: %d FAILED\n", failures);
		return 1;
	}
	printf("test_death_tilt: PASS\n");
	return 0;
}
