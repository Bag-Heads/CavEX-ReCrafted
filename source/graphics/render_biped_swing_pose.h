/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RENDER_BIPED_SWING_POSE_H
#define RENDER_BIPED_SWING_POSE_H

/*
	MP visual-parity pass (2026-08-31) items 2+3: PURE math for ModelBiped's
	swing-progress arm animation and sneak pose, transcribed from [740c583
	ModelBiped.setRotationAngles] in FULL, in SOURCE ORDER (the swing-progress
	block runs BEFORE the sneak-pose block; both mutate overlapping arm
	rotateAngleX fields, so order matters -- a sneaking player mid-swing gets
	BOTH deltas stacked, exactly like Beta's sequential field mutation):

	  1. Swing-progress block (gated on the swing field being above the -9990 sentinel -- here
	     ALWAYS taken: RenderLiving.doRenderLiving unconditionally sets
	     the model swing field to the entity's getSwingProgress at the partial tick, never
	     the -9990 sentinel, for every ModelBiped-driven RenderLiving/
	     RenderPlayer). Twists the body about Y, follows both shoulders'
	     rotation point with the SAME twist (sin/cos on a radius-5px circle),
	     and layers the classic attack-swing curve onto the RIGHT arm only
	     (X pitch via an ease-out-quartic sin blend, Y lean x3, Z roll).

	  2. Sneak-pose block (the sneaking / standing branches): forward
	     torso pitch, +0.4F onto BOTH arms' rotateAngleX (additive on top of
	     whatever the swing block left there), and the leg/head rotation-point
	     shifts that crouch the whole model.

	Factored out of render_entity.c's GX-coupled render_entity_biped (which
	cannot be linked/tested in isolation) into a pure, allocation-free struct
	return -- the render_entity_anchor.h / test_head_anchor.c precedent, so
	the sin-curve sample points are golden-tested directly.

	Units: rotations are RADIANS (matching cglm's glm_rotate_* and this
	engine's own walkAngle convention); *_point_*_px fields are Beta's raw
	rotationPoint PIXEL units (1 px == 1/16 block, the same scale
	render_entity_biped already divides every limbX0/Y0/Z0 constant by) --
	the caller divides by 16.0F when composing the model matrix, exactly like
	every other pixel constant in render_entity.c.
*/

#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct biped_swing_pose {
	float body_rot_y; // ModelBiped.bipedBody.rotateAngleY (swing twist)
	float body_rot_x; // ModelBiped.bipedBody.rotateAngleX (sneak pitch: 0.5
					  // or 0 -- [740c583] set outright by the sneak/standing branches)

	// Right/left arm rotationPoint X/Z, ABSOLUTE (replaces the -5/0 and
	// +5/0 baselines respectively -- [740c583] ModelBiped ctor rp(-5,2,0) /
	// rp(5,2,0), X/Z only; Y is never touched by either block).
	float right_arm_point_x_px, right_arm_point_z_px;
	float left_arm_point_x_px, left_arm_point_z_px;

	// rotateAngleX DELTA: ADD to the existing walk-swing rotateAngleX
	// (render_entity_biped's own limbAnim[] term) -- both blocks mutate the
	// SAME field additively on top of the walk swing, never replace it.
	float right_arm_rot_x_delta;
	float left_arm_rot_x_delta;

	// rotateAngleY, ABSOLUTE: [740c583] zeroes both arms' rotateAngleY
	// immediately before the swing block runs, so the block's own additive
	// chain is equivalent to an absolute value from zero.
	float right_arm_rot_y;
	float left_arm_rot_y;

	// rotateAngleZ, ABSOLUTE (right arm only -- left arm's Z stays the
	// ModelBiped-wide 0.0F set earlier in setRotationAngles, untouched by
	// either block here).
	float right_arm_rot_z;

	// Leg/head rotation-point DELTAS (sneak only; 0 when standing) -- ADD to
	// the existing static leg/head placement constants.
	float leg_point_y_px_delta; // BOTH legs: 9.0F-12.0F = -3.0F when sneak
	float leg_point_z_px_delta; // BOTH legs: 4.0F-0.0F = +4.0F when sneak
	float head_point_y_px_delta; // neck Y: 1.0F-0.0F = +1.0F when sneak
};

static inline struct biped_swing_pose render_biped_compute_swing_pose(
	float swing_progress, bool is_sneak, float head_pitch_rad) {
	struct biped_swing_pose pose = {0};

	// --- 1. swing-progress block [740c583 ModelBiped.setRotationAngles] ---
	float sp = swing_progress;
	float body_rot_y = sinf(sqrtf(sp) * 2.0f * (float) M_PI) * 0.2f;
	pose.body_rot_y = body_rot_y;

	pose.right_arm_point_x_px = -cosf(body_rot_y) * 5.0f;
	pose.right_arm_point_z_px = sinf(body_rot_y) * 5.0f;
	pose.left_arm_point_x_px = cosf(body_rot_y) * 5.0f;
	pose.left_arm_point_z_px = -sinf(body_rot_y) * 5.0f;

	float right_arm_rot_y = body_rot_y;	  // right arm Y gains the body twist
	float left_arm_rot_y = body_rot_y;		  // left arm Y gains the body twist
	float left_arm_rot_x_delta = body_rot_y; // left arm X gains the body twist

	// ease-out-quartic: 1-(1-x)^4, matching Beta's one-minus, square, square
	// again, one-minus sequence exactly (two squarings == ^4).
	float one_minus = 1.0f - sp;
	float ease = 1.0f - (one_minus * one_minus) * (one_minus * one_minus);
	float swing_wave = sinf(ease * (float) M_PI);
	float head_pitch_term = sinf(sp * (float) M_PI) * -(head_pitch_rad - 0.7f) * 0.75f;
	// Beta subtracts (swing wave x 1.2 + head term) from right arm X -> delta is the negation.
	float right_arm_rot_x_delta = -(swing_wave * 1.2f + head_pitch_term);
	right_arm_rot_y += body_rot_y * 2.0f; // plus twice the body twist
	float right_arm_rot_z = sinf(sp * (float) M_PI) * -0.4f;

	pose.right_arm_rot_x_delta = right_arm_rot_x_delta;
	pose.right_arm_rot_y = right_arm_rot_y;
	pose.right_arm_rot_z = right_arm_rot_z;
	pose.left_arm_rot_x_delta = left_arm_rot_x_delta;
	pose.left_arm_rot_y = left_arm_rot_y;

	// --- 2. sneak-pose block [740c583 ModelBiped.setRotationAngles], AFTER
	// the swing block in source order -- additive on the arm X deltas
	// above, absolute/overwriting for body pitch and leg/head points. ---
	if(is_sneak) {
		pose.body_rot_x = 0.5f;
		pose.right_arm_rot_x_delta += 0.4f;
		pose.left_arm_rot_x_delta += 0.4f;
		pose.leg_point_y_px_delta = 9.0f - 12.0f;
		pose.leg_point_z_px_delta = 4.0f - 0.0f;
		pose.head_point_y_px_delta = 1.0f - 0.0f;
	} else {
		pose.body_rot_x = 0.0f;
		pose.leg_point_y_px_delta = 0.0f;
		pose.leg_point_z_px_delta = 0.0f;
		pose.head_point_y_px_delta = 0.0f;
	}

	return pose;
}

/*
	Animation-parity pass (2026-09-03) item 4: ModelBiped's isRiding pose
	[740c583 ModelBiped.setRotationAngles], expressed as pure
	constants -- this block runs BEFORE the swing-progress and sneak blocks
	above in Beta's own method body, but every field it touches is either an
	ADDITIVE delta on a field the blocks above only ever add to (both arms'
	rotateAngleX -- addition commutes, so application order does not change
	the sum) or an ABSOLUTE assignment to a field NEITHER block above ever
	touches (both legs' rotateAngleX/rotateAngleY -- legs get no swing/sneak
	rotation in ModelBiped, only translation deltas), so folding this in
	AFTER render_biped_compute_swing_pose's own block order is output-
	identical to Beta's true evaluation order:

	  while riding: both arms' rotateAngleX gain -0.62831855 (-pi/5), both
	    legs' rotateAngleX are SET to -1.2566371 (-2pi/5), and the right and
	    left legs' rotateAngleY are SET to +0.31415927 and -0.31415927 (+-pi/10).






	No continuous input: the caller's own `is_riding` bool is the only gate,
	so this returns fixed constants rather than taking a parameter. Callers
	ignore this struct entirely when is_riding is false -- the legs then keep
	their normal walk-swing rotateAngleX and 0.0F rotateAngleY, exactly the
	pre-animation-parity-pass behaviour.
*/
struct biped_riding_pose {
	float arm_rot_x_delta; // ADD to both arms' existing rotateAngleX
	float leg_rot_x;		// ABSOLUTE, replaces both legs' walk-swing rotateAngleX
	float right_leg_rot_y; // ABSOLUTE
	float left_leg_rot_y;	// ABSOLUTE
};

static inline struct biped_riding_pose render_biped_riding_pose(void) {
	return (struct biped_riding_pose) {
		.arm_rot_x_delta = -0.62831855f,
		.leg_rot_x = -1.2566371f,
		.right_leg_rot_y = 0.31415927f,
		.left_leg_rot_y = -0.31415927f,
	};
}

#endif
