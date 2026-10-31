/* Frozen pre-session camera transform for byte-equivalence checks.
 * Copied from source/game/camera.c at the start of the 2026-09-05 slice.
 * GPL-3.0-or-later. Do not update this reference to follow new code. */
static void camera_update_reference(struct camera* c, bool in_water) {
	assert(c);

	glm_perspective(glm_rad(gstate.config.fov)
						* (in_water ? 6.0F / 7.0F : 1.0F),
					(float)gfx_width() / (float)gfx_height(), 0.075F,
					gstate.config.render_distance, c->projection);

	glm_lookat((vec3) {c->x, c->y, c->z},
			   (vec3) {c->x + sinf(c->rx) * sinf(c->ry), c->y + cosf(c->ry),
					   c->z + cosf(c->rx) * sinf(c->ry)},
			   (vec3) {0, 1, 0}, c->view);

	/* Beta VIEW BOBBING [740c583 EntityRenderer.setupCameraTransform:285-299]:
	 * on the GL modelview, hurtCameraEffect then (viewBobbing-gated)
	 * setupViewBobbing run BEFORE orientCamera -- with GL post-multiply
	 * semantics that means the bob translate/rotates act in EYE space on top
	 * of the camera transform, i.e. view = Bob * lookat. Same composition
	 * here. Applied before the frustum planes are derived so culling sees the
	 * bobbed view. bob_active is armed by camera_attach, so the detached free
	 * camera (camera_physics) and menus never take this branch. */
	if(c->bob_active) {
		if(gstate.config.view_bobbing) {
			float t[5];
			camera_bob_terms(&c->bob, c->bob_partial, t);
			mat4 bob;
			glm_mat4_identity(bob);
			/* Order is Beta's exactly: translate, rotate Z, rotate X, rotate X
			 * [740c583 EntityRenderer.setupViewBobbing:178-182]. */
			glm_translate(bob, (vec3) {t[0], t[1], 0.0F});
			glm_rotate(bob, glm_rad(t[2]), (vec3) {0.0F, 0.0F, 1.0F});
			glm_rotate(bob, glm_rad(t[3]), (vec3) {1.0F, 0.0F, 0.0F});
			glm_rotate(bob, glm_rad(t[4]), (vec3) {1.0F, 0.0F, 0.0F});
			glm_mat4_mul(bob, c->view, c->view);
		}

		/* Beta HURT/DEATH CAMERA ROLL [740c583 EntityRenderer
		 * .setupCameraTransform:283-287]: hurtCameraEffect runs BEFORE
		 * setupViewBobbing and is NOT gated by the viewBobbing setting, so it
		 * left-multiplies on top of the bob (view = Hurt * Bob * lookat).
		 * Within hurtCameraEffect the death tilt precedes the hurt kick
		 * [:156-167]; attackedAtYaw's Y-conjugation collapses to the plain Z
		 * roll because our MP-architecture client never learns a damage
		 * direction (hud_hurt.h header comment). */
		float droll = hud_hurt_death_roll_deg(&c->hurt, c->bob_partial);
		float hroll = hud_hurt_camera_roll_deg(&c->hurt, c->bob_partial);
		if(droll != 0.0F || hroll != 0.0F) {
			mat4 h;
			glm_mat4_identity(h);
			if(droll != 0.0F)
				glm_rotate(h, glm_rad(droll), (vec3) {0.0F, 0.0F, 1.0F});
			if(hroll != 0.0F)
				glm_rotate(h, glm_rad(hroll), (vec3) {0.0F, 0.0F, 1.0F});
			glm_mat4_mul(h, c->view, c->view);
		}
	}
	c->bob_active = false;

	mat4 view_proj;
	glm_mat4_mul(c->projection, c->view, view_proj);
	glm_frustum_planes(view_proj, c->frustum_planes);
}
