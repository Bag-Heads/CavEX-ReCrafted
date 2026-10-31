/*
	test_head_anchor -- pins the M2 head-placement fix (the "pig head buried
	in its body" hardware footage). Binds the SHARED production composition
	(source/graphics/render_entity_anchor.h) directly -- drift-proof.

	Pinned:
	  1. ANCHOR PROPERTY (the fix): for every mob's (bodyPivot, neck)
	     constants, the model-frame neck point transformed by head_mv lands
	     EXACTLY where body_mv puts it -- at every body yaw, for equal AND
	     differing head yaw. The head can no longer drift off the body.
	  2. ON-AXIS IDENTITY (the no-regression guarantee): for the zombie/
	     skeleton biped and the creeper -- whose body pivot and neck both sit
	     on the model axis x=z=8px -- the new composition's matrix is element-
	     wise IDENTICAL to the old absolute-yaw-about-fixed-neck composition,
	     so their vertex streams are byte-identical by construction.
	  3. THE OLD BUG, QUANTIFIED: under the old composition the pig's neck
	     point misses the body-frame neck by ~12px at yaw 0 (south, the
	     default spawn facing) -- the exact figure the 2026-06-11 research
	     pass measured. Guards against anyone "simplifying" back.

	Neck constants are the Beta rotationPoints mapped v2 = (rp_x+8, 24-rp_y,
	rp_z+8), decompile-verified 2026-06-11: biped rp(0,0,0), creeper rp(0,6,0),
	pig rp(0,12,-6), cow rp(0,4,-8), sheep rp(0,6,-8), chicken rp(0,15,-4),
	spider rp(0,15,-3).
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "graphics/render_entity_anchor.h"

// The OLD composition (pre-fix), kept here as the historical reference:
// absolute head yaw about the fixed model-frame neck, composed with view.
static void old_head_mv(mat4 view, vec3 neck, float headYawDeg, mat4 out) {
	mat4 m;
	glm_translate_make(m, (vec3) {-0.5f, 0.0f, -0.5f});
	glm_translate(m, neck);
	glm_rotate_y(m, glm_rad(headYawDeg + 180.0f), m);
	glm_translate(m, (vec3) {-neck[0], -neck[1], -neck[2]});
	glm_mat4_mul(view, m, out);
}

struct mob_anchor {
	const char* name;
	vec3 pivot; // body yaw pivot (x/z on the model axis 8px after the fix)
	vec3 neck;  // Beta rotationPoint, v2-mapped, px/16
};

int main(void) {
	const struct mob_anchor mobs[] = {
		{"biped", {8 / 16.0f, 18 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 24 / 16.0f, 8 / 16.0f}},
		{"creeper", {8 / 16.0f, 12 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 18 / 16.0f, 8 / 16.0f}},
		{"pig", {8 / 16.0f, 10 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 12 / 16.0f, 2 / 16.0f}},
		{"cow", {8 / 16.0f, 17 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 20 / 16.0f, 0 / 16.0f}},
		{"sheep", {8 / 16.0f, 17 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 18 / 16.0f, 0 / 16.0f}},
		{"chicken", {8 / 16.0f, 8 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 9 / 16.0f, 4 / 16.0f}},
		{"spider", {8 / 16.0f, 9 / 16.0f, 8 / 16.0f},
		 {8 / 16.0f, 9 / 16.0f, 5 / 16.0f}},
	};
	const float yaws[] = {0.0f, 90.0f, 180.0f, -90.0f, 37.5f};
	const float head_deltas[] = {0.0f, 25.0f, -60.0f};

	// An arbitrary non-trivial view matrix so the equalities are tested
	// through a realistic transform, not the identity.
	mat4 view;
	glm_translate_make(view, (vec3) {3.25f, -1.5f, 7.75f});
	glm_rotate_y(view, glm_rad(23.0f), view);

	// --- 1. anchor property: the neck point rides the body, always ----------
	for(size_t i = 0; i < sizeof(mobs) / sizeof(mobs[0]); i++) {
		for(size_t yi = 0; yi < sizeof(yaws) / sizeof(yaws[0]); yi++) {
			for(size_t di = 0; di < sizeof(head_deltas) / sizeof(head_deltas[0]);
				di++) {
				float by = yaws[yi];
				float hy = by + head_deltas[di];
				mat4 body_mv, head_mv;
				render_entity_body_mv(view, (float*)mobs[i].pivot, by, body_mv);
				render_entity_head_mv(body_mv, (float*)mobs[i].neck, hy, by,
									  0.0f, head_mv);
				vec4 n = {mobs[i].neck[0], mobs[i].neck[1], mobs[i].neck[2],
						  1.0f};
				// body_mv expects the neck in the PRE-recenter model frame
				// minus... both matrices consume the same model-frame point:
				// the neck as authored (px/16), recenter included inside.
				vec4 via_body, via_head;
				// body_mv includes the -0.5 recenter, so feed the raw neck.
				glm_mat4_mulv(body_mv, n, via_body);
				glm_mat4_mulv(head_mv, n, via_head);
				float dx = via_body[0] - via_head[0];
				float dy = via_body[1] - via_head[1];
				float dz = via_body[2] - via_head[2];
				float err = sqrtf(dx * dx + dy * dy + dz * dz);
				assert(err < 1.0e-4f); // the neck is WELDED to the body
			}
		}
	}

	// --- 2. on-axis identity: biped + creeper matrices byte-equal old code --
	for(size_t i = 0; i < 2; i++) { // mobs[0] = biped, mobs[1] = creeper
		for(size_t yi = 0; yi < sizeof(yaws) / sizeof(yaws[0]); yi++) {
			for(size_t di = 0; di < sizeof(head_deltas) / sizeof(head_deltas[0]);
				di++) {
				float by = yaws[yi];
				float hy = by + head_deltas[di];
				mat4 body_mv, head_new, head_old;
				render_entity_body_mv(view, (float*)mobs[i].pivot, by, body_mv);
				render_entity_head_mv(body_mv, (float*)mobs[i].neck, hy, by,
									  0.0f, head_new);
				old_head_mv(view, (float*)mobs[i].neck, hy, head_old);
				for(int r = 0; r < 4; r++)
					for(int c = 0; c < 4; c++)
						assert(fabsf(head_new[r][c] - head_old[r][c])
							   < 1.0e-5f);
			}
		}
	}

	// --- 3. the old bug, quantified: pig at yaw 0 missed by ~12px ------------
	{
		mat4 body_mv, head_old;
		render_entity_body_mv(view, (float*)mobs[2].pivot, 0.0f, body_mv);
		old_head_mv(view, (float*)mobs[2].neck, 0.0f, head_old);
		vec4 n = {mobs[2].neck[0], mobs[2].neck[1], mobs[2].neck[2], 1.0f};
		vec4 via_body, via_old;
		glm_mat4_mulv(body_mv, n, via_body);
		glm_mat4_mulv(head_old, n, via_old);
		float dx = via_body[0] - via_old[0];
		float dz = via_body[2] - via_old[2];
		float err_px = sqrtf(dx * dx + dz * dz) * 16.0f;
		// research-measured 12.00px at yaw 0 (south) for the pig
		assert(err_px > 11.5f && err_px < 12.5f);
	}

	printf("test_head_anchor: OK (neck welded on all 7, biped/creeper "
		   "byte-identical, old pig error = 12px pinned)\n");
	return 0;
}
