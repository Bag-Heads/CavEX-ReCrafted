/*
	test_mp_visual_parity_pose -- golden tests for the PURE math added by the
	MP visual-parity pass (2026-08-31), mirroring test_head_anchor.c's own
	"bind the shared production composition directly" precedent:

	  1. SWING CURVE (item 3) -- render_biped_swing_pose.h's
	     render_biped_compute_swing_pose at swing_progress = 0.0/0.5/1.0,
	     is_sneak=false: sample points independently hand-computed from
	     [740c583 ModelBiped.setRotationAngles]'s swing-progress block,
	     including the REST-STATE identity property (sp=0 -> every delta
	     exactly 0/baseline, the property render_entity.c's render_entity_
	     biped relies on for "every OTHER biped caller is unaffected").

	  2. SNEAK POSE (item 2) -- the isSneak branch's body pitch (0.5F) / leg
	     rotation-point deltas (-3.0F Y, +4.0F Z) / head Y delta (+1.0F), AND
	     the STACKING property: is_sneak=true at swing_progress=0.5 adds
	     +0.4F onto BOTH arms' already-nonzero swing delta (Beta mutates the
	     SAME rotateAngleX field sequentially, never resets it).

	  3. PICKUP-FLIGHT LERP (item 4) -- entity_pickup_flight.h's ease-in-
	     quadratic interpolation [740c583 EntityPickupFX.renderParticle],
	     including the age==0/partial==0 exact-start property, the
	     age==max_age-1/partial->1 near-arrival property, and the defensive
	     over-range clamp.

	Links only the two pure-math headers (render_biped_swing_pose.h,
	entity_pickup_flight.h) -- no GX/game_state/entity.h dependency, so this
	target has zero coupling to the render/network layers it feeds.
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "graphics/render_biped_swing_pose.h"
#include "entity/entity_pickup_flight.h"

#define EPS 1e-4f

static void assert_close(float actual, float expect, const char* what) {
	if(fabsf(actual - expect) >= EPS) {
		fprintf(stderr, "%s: expected %.6f, got %.6f\n", what, expect,
			   actual);
		assert(0);
	}
}

// --- 1. swing curve ---------------------------------------------------

static void test_swing_rest_state_is_identity(void) {
	// [740c583]: at swing_progress==0 every field must equal Beta's own
	// static ModelBiped ctor baseline (rp(-5,2,0)/rp(5,2,0), all angles 0)
	// -- the property render_entity_biped's "unchanged for every other
	// biped caller" claim depends on.
	struct biped_swing_pose p
		= render_biped_compute_swing_pose(0.0f, false, 0.0f);

	assert_close(p.body_rot_y, 0.0f, "sp0 body_rot_y");
	assert_close(p.body_rot_x, 0.0f, "sp0 body_rot_x");
	assert_close(p.right_arm_point_x_px, -5.0f, "sp0 right_x");
	assert_close(p.right_arm_point_z_px, 0.0f, "sp0 right_z");
	assert_close(p.left_arm_point_x_px, 5.0f, "sp0 left_x");
	assert_close(p.left_arm_point_z_px, 0.0f, "sp0 left_z");
	assert_close(p.right_arm_rot_x_delta, 0.0f, "sp0 right_rot_x_delta");
	assert_close(p.left_arm_rot_x_delta, 0.0f, "sp0 left_rot_x_delta");
	assert_close(p.right_arm_rot_y, 0.0f, "sp0 right_rot_y");
	assert_close(p.left_arm_rot_y, 0.0f, "sp0 left_rot_y");
	assert_close(p.right_arm_rot_z, 0.0f, "sp0 right_rot_z");
	assert_close(p.leg_point_y_px_delta, 0.0f, "sp0 leg_y");
	assert_close(p.leg_point_z_px_delta, 0.0f, "sp0 leg_z");
	assert_close(p.head_point_y_px_delta, 0.0f, "sp0 head_y");

	// Full cycle (sp=1.0) returns to the SAME rest state -- sqrt(1)*2*PI ==
	// 2*PI, a full sine period.
	struct biped_swing_pose p1
		= render_biped_compute_swing_pose(1.0f, false, 0.0f);
	assert_close(p1.body_rot_y, 0.0f, "sp1 body_rot_y");
	assert_close(p1.right_arm_point_x_px, -5.0f, "sp1 right_x");
	assert_close(p1.right_arm_rot_x_delta, 0.0f, "sp1 right_rot_x_delta");
}

static void test_swing_midpoint_sample(void) {
	// Independently hand-computed (Python, math.sin/cos, fp64) from
	// [740c583 ModelBiped.setRotationAngles]'s exact formula at
	// swing_progress=0.5, head_pitch_rad=0.0, is_sneak=false.
	struct biped_swing_pose p
		= render_biped_compute_swing_pose(0.5f, false, 0.0f);

	assert_close(p.body_rot_y, -0.192781f, "sp0.5 body_rot_y");
	assert_close(p.right_arm_point_x_px, -4.907377f, "sp0.5 right_x");
	assert_close(p.right_arm_point_z_px, -0.957943f, "sp0.5 right_z");
	assert_close(p.left_arm_point_x_px, 4.907377f, "sp0.5 left_x");
	assert_close(p.left_arm_point_z_px, 0.957943f, "sp0.5 left_z");
	assert_close(p.right_arm_rot_x_delta, -0.759108f, "sp0.5 right_rot_x_delta");
	assert_close(p.left_arm_rot_x_delta, -0.192781f, "sp0.5 left_rot_x_delta");
	assert_close(p.right_arm_rot_y, -0.578342f, "sp0.5 right_rot_y");
	assert_close(p.left_arm_rot_y, -0.192781f, "sp0.5 left_rot_y");
	assert_close(p.right_arm_rot_z, -0.4f, "sp0.5 right_rot_z");
}

// --- 2. sneak pose + stacking -------------------------------------------

static void test_sneak_pose_at_rest(void) {
	// is_sneak=true, swing_progress=0.0 -- pure sneak pose, no swing
	// contribution. [740c583 ModelBiped.setRotationAngles isSneak branch]
	struct biped_swing_pose p
		= render_biped_compute_swing_pose(0.0f, true, 0.0f);

	assert_close(p.body_rot_x, 0.5f, "sneak body_rot_x");
	assert_close(p.right_arm_rot_x_delta, 0.4f, "sneak right_rot_x_delta");
	assert_close(p.left_arm_rot_x_delta, 0.4f, "sneak left_rot_x_delta");
	assert_close(p.leg_point_y_px_delta, -3.0f, "sneak leg_y"); // 9-12
	assert_close(p.leg_point_z_px_delta, 4.0f, "sneak leg_z");  // 4-0
	assert_close(p.head_point_y_px_delta, 1.0f, "sneak head_y"); // 1-0
	// Arm rotation POINT (shoulder) is untouched by the sneak block --
	// still the swing block's sp=0 baseline.
	assert_close(p.right_arm_point_x_px, -5.0f, "sneak right_x unchanged");
}

static void test_sneak_and_swing_stack(void) {
	// Beta mutates rotateAngleX SEQUENTIALLY (swing block, then sneak
	// block) -- a sneaking player mid-swing gets BOTH deltas ADDED, not one
	// replacing the other. Same swing_progress=0.5 sample as
	// test_swing_midpoint_sample, plus is_sneak=true's own +0.4F on each arm.
	struct biped_swing_pose swing_only
		= render_biped_compute_swing_pose(0.5f, false, 0.0f);
	struct biped_swing_pose stacked
		= render_biped_compute_swing_pose(0.5f, true, 0.0f);

	assert_close(stacked.right_arm_rot_x_delta,
				swing_only.right_arm_rot_x_delta + 0.4f, "stacked right arm");
	assert_close(stacked.left_arm_rot_x_delta,
				swing_only.left_arm_rot_x_delta + 0.4f, "stacked left arm");
	// The swing-only fields (body twist, shoulder points, rot_y/rot_z) are
	// IDENTICAL between the two -- the sneak block never touches them.
	assert_close(stacked.body_rot_y, swing_only.body_rot_y, "twist unaffected");
	assert_close(stacked.right_arm_point_x_px, swing_only.right_arm_point_x_px,
				"shoulder unaffected");
	assert_close(stacked.right_arm_rot_z, swing_only.right_arm_rot_z,
				"roll unaffected");
	// But the sneak-only fields now differ from the standing case.
	assert_close(stacked.body_rot_x, 0.5f, "stacked body pitch");
	assert_close(stacked.leg_point_z_px_delta, 4.0f, "stacked leg z");
}

// --- 3. pickup-flight lerp -----------------------------------------------

static void test_pickup_flight_lerp_samples(void) {
	vec3 start = {0.0f, 64.0f, 0.0f};
	vec3 collector = {10.0f, 65.0f, -4.0f};
	vec3 out;

	// age=0, partial=0 -> t=0 -> exactly start.
	entity_pickup_flight_lerp(start, collector, 0, 0.0f, 3, out);
	assert_close(out[0], 0.0f, "flight t0 x");
	assert_close(out[1], 64.0f, "flight t0 y");
	assert_close(out[2], 0.0f, "flight t0 z");

	// age=0, partial=0.5 -> t=1/6, t^2=1/36 (ease-in: barely moved yet).
	entity_pickup_flight_lerp(start, collector, 0, 0.5f, 3, out);
	assert_close(out[0], 0.277778f, "flight t=1/6 x");
	assert_close(out[1], 64.027778f, "flight t=1/6 y");
	assert_close(out[2], -0.111111f, "flight t=1/6 z");

	// age=1, partial=0 -> t=1/3, t^2=1/9.
	entity_pickup_flight_lerp(start, collector, 1, 0.0f, 3, out);
	assert_close(out[0], 1.111111f, "flight t=1/3 x");
	assert_close(out[1], 64.111111f, "flight t=1/3 y");
	assert_close(out[2], -0.444444f, "flight t=1/3 z");

	// age=2, partial=0 -> t=2/3, t^2=4/9 (past the halfway point --
	// confirms the EASE-IN shape: still well short of arrival).
	entity_pickup_flight_lerp(start, collector, 2, 0.0f, 3, out);
	assert_close(out[0], 4.444444f, "flight t=2/3 x");
	assert_close(out[1], 64.444444f, "flight t=2/3 y");
	assert_close(out[2], -1.777778f, "flight t=2/3 z");

	// age=2, partial=1.0 -> t=1.0 exactly -> lands ON the collector.
	entity_pickup_flight_lerp(start, collector, 2, 1.0f, 3, out);
	assert_close(out[0], 10.0f, "flight t1 x");
	assert_close(out[1], 65.0f, "flight t1 y");
	assert_close(out[2], -4.0f, "flight t1 z");
}

static void test_pickup_flight_lerp_clamps_over_range(void) {
	// Defensive clamp: age/max_age beyond 1.0 must not overshoot past the
	// collector (this engine reaps the entity the SAME tick age reaches
	// max_age, so this should never occur in production -- still must be
	// safe if it ever does).
	vec3 start = {0.0f, 0.0f, 0.0f};
	vec3 collector = {8.0f, 0.0f, 0.0f};
	vec3 out;

	entity_pickup_flight_lerp(start, collector, 5, 0.9f, 3, out);
	assert_close(out[0], 8.0f, "flight clamp x");
}

// --- 4. riding pose (animation-parity pass, 2026-09-03, item 4) --------

static void test_riding_pose_constants(void) {
	// Hand-transcribed from [740c583 ModelBiped.setRotationAngles]'s
	// isRiding block, literal-for-literal (never derived by running our own
	// code -- the poisoned-golden rule):
	//   bipedRightArm.rotateAngleX += -0.62831855F;
	//   bipedLeftArm.rotateAngleX  += -0.62831855F;
	//   bipedRightLeg.rotateAngleX  = -1.2566371F;
	//   bipedLeftLeg.rotateAngleX   = -1.2566371F;
	//   bipedRightLeg.rotateAngleY  =  0.31415927F;
	//   bipedLeftLeg.rotateAngleY   = -0.31415927F;
	struct biped_riding_pose p = render_biped_riding_pose();

	assert_close(p.arm_rot_x_delta, -0.62831855f, "riding arm_rot_x_delta");
	assert_close(p.leg_rot_x, -1.2566371f, "riding leg_rot_x");
	assert_close(p.right_leg_rot_y, 0.31415927f, "riding right_leg_rot_y");
	assert_close(p.left_leg_rot_y, -0.31415927f, "riding left_leg_rot_y");

	// leg_rot_x is exactly 2x arm_rot_x_delta, and right/left leg rotateY
	// are exact negations -- both true of Beta's own literals, cheap
	// properties to pin so a future typo (e.g. a dropped sign) trips here
	// even if the raw-literal assertions above were ever loosened.
	assert_close(p.leg_rot_x, 2.0f * p.arm_rot_x_delta, "riding leg==2*arm");
	assert_close(p.right_leg_rot_y, -p.left_leg_rot_y, "riding legs mirrored");
}

int main(void) {
	test_swing_rest_state_is_identity();
	test_swing_midpoint_sample();
	test_sneak_pose_at_rest();
	test_sneak_and_swing_stack();
	test_pickup_flight_lerp_samples();
	test_pickup_flight_lerp_clamps_over_range();
	test_riding_pose_constants();

	printf("test_mp_visual_parity_pose: all assertions passed\n");
	return 0;
}
