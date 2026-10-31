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

#ifndef RENDER_ENTITY_ANCHOR_H
#define RENDER_ENTITY_ANCHOR_H

// The mob BODY/HEAD matrix composition (M2 head-placement fix), shared by
// every per-mob render fn in render_entity.c AND bound directly by
// tests/test_head_anchor.c -- the drift-proof pattern: the test exercises the
// EXACT production composition, not a replica.
//
// Beta RenderLiving composes R_body(about the model origin axis) THEN
// R_headRelative(about the neck rotationPoint), so the neck point ORBITS with
// the body yaw. The old per-fn head blocks instead rotated an ABSOLUTE head
// yaw about a FIXED model-frame neck, composed with the raw view -- correct
// only when the body pivot and neck share the model's vertical axis (biped/
// creeper/chicken); for the off-axis quadrupeds + spider the head drifted
// 0-16px with facing, maxing out at the default south spawn pose (the
// "pig head buried in its body" footage). Numerically verified in the
// 2026-06-11 research pass (headcheck.c scratch program): pig 12px, cow/sheep
// 16px, spider 6px, chicken 8px error at yaw 0 under the old composition;
// zombie/skeleton/creeper 0.000px at every yaw.

#include "../cglm/cglm.h"

// The shared body matrix: recenter the 16px model on the entity origin
// (-0.5,-0.5), then yaw about `pivot` by bodyYawDeg + 180 (model front = -Z).
// For the head composition to be Beta-correct the pivot's x/z MUST be the
// model origin axis (8px,8px) -- Beta rotates the WHOLE model about its
// origin, never about a box centre. (pivot y is irrelevant for a pure yaw;
// kept as a parameter for readability at the call sites.)
static inline void render_entity_body_mv(mat4 view, vec3 pivot,
										 float bodyYawDeg, mat4 out) {
	mat4 m;
	glm_translate_make(m, (vec3) {-0.5f, 0.0f, -0.5f});
	glm_translate(m, pivot);
	glm_rotate_y(m, glm_rad(bodyYawDeg + 180.0f), m);
	glm_translate(m, (vec3) {-pivot[0], -pivot[1], -pivot[2]});
	glm_mat4_mul(view, m, out);
}

// The head matrix RIDES the body matrix: head_mv = body_mv * T(neck) *
// R_y(rad(headYawDeg - bodyYawDeg)) * R_x(rad(headPitchDeg)) * T(-neck).
// body_mv already carries the single -0.5 recenter AND the +180 forward
// offset (the documented double-recenter trap), so the head adds ONLY its
// yaw DELTA about the neck joint -- the neck stays welded to the body at
// every facing. `neck` is the Beta rotationPoint mapped v2 = (rp_x+8,
// 24-rp_y, rp_z+8) in px/16. For a mob whose body pivot and neck both sit on
// the model axis (x=z=8px) this is PROVABLY output-identical to the old
// absolute composition (two R_y about the same vertical axis fuse:
// (b+180)+(h-b) = h+180) -- test_head_anchor pins both that identity and the
// anchor property (with headPitchDeg=0.0F, matching every existing caller
// except the biped path -- see render_entity_biped).
//
// headPitchDeg is Beta's OWN rotationPitch in degrees (0 = level, ABSOLUTE --
// not relative to body, unlike yaw): ModelBiped.setRotationAngles sets
// bipedHead.rotateAngleX = rotationPitch/57.295776 directly, with no body
// subtraction [740c583 ModelBiped.setRotationAngles:70-71]; RenderLiving's
// own GL call order is Z, then Y, then X [740c583 ModelRenderer.render:103-
// 113], matching the Y-then-X order below. Rotating about the neck point
// (fixed at the local origin by the two translates) preserves the anchor
// property for ANY pitch value, so every caller may pass a nonzero pitch
// without re-deriving the anchor proof. Every non-biped caller passes 0.0F
// (Beta head pitch is unmodeled for those mobs in this engine today), for
// which R_x(0) is the identity matrix and the composition is unchanged.
static inline void render_entity_head_mv(mat4 body_mv, vec3 neck,
										 float headYawDeg, float bodyYawDeg,
										 float headPitchDeg, mat4 out) {
	mat4 m;
	glm_translate_make(m, neck);
	glm_rotate_y(m, glm_rad(headYawDeg - bodyYawDeg), m);
	glm_rotate_x(m, glm_rad(headPitchDeg), m);
	glm_translate(m, (vec3) {-neck[0], -neck[1], -neck[2]});
	glm_mat4_mul(body_mv, m, out);
}

// Beta death tilt placement (next-gaps-0924/G8). RenderLiving.doRenderLiving
// translates to the entity position (func_22012_b, RenderLiving.java:117-119),
// then rotateCorpse yaws by 180 - yawOffset and rolls about the YAWED frame's
// Z axis (RenderLiving.java:121-133) -- the body's own front-back axis through
// the feet, so the body falls sideways to its LEFT at any facing. `model` must
// be T(feet) (the per-mob fn composes render_entity_body_mv, which yaws the
// -Z-front model by bodyYawDeg + 180, on top of it). This conjugates the roll
// into that same yawed frame: model *= R_y(b+180) * R_z(roll) * R_y(-(b+180)).
// rollDeg 0 (a living body) leaves `model` bit-identical.
static inline void render_entity_death_tilt(mat4 model, float bodyYawDeg,
											float rollDeg) {
	if(rollDeg == 0.0f)
		return;
	float yaw = glm_rad(bodyYawDeg + 180.0f);
	glm_rotate_y(model, yaw, model);
	glm_rotate_z(model, glm_rad(rollDeg), model);
	glm_rotate_y(model, -yaw, model);
}

#endif
