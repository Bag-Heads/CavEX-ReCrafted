/* View-bobbing constants pin (render/feel layer).
 *
 * Pins the ported Beta 1.7.3 view-bob math in source/game/camera_bob.h against
 * the decompile-cited formulas:
 *   [740c583 Entity.moveEntity:199,479]          dwm roll + 0.6 walk factor
 *   [740c583 EntityPlayer.onLivingUpdate:180-195] f cap 0.1, air/ground/dead
 *                                                 gates, smoothing 0.4 / 0.8
 *   [740c583 EntityRenderer.setupViewBobbing:174-182] the five transform
 *                                                 terms (0.5, 1.0, 3.0, 5.0,
 *                                                 -0.2 phase) and their lerp
 * A drifted constant fails here before it can drift the feel. */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../source/game/camera_bob.h"

#define FEQ(a, b) (fabsf((a) - (b)) < 1e-6f)

static void test_tick_walk_and_smoothing(void) {
	struct camera_bob_state b = {0};

	/* One grounded tick, alive, moving 0.2 blocks along x at 0.3 b/t. */
	camera_bob_tick(&b, 0.2F, 0.0F, 0.3F, 0.0F, 0.0F, true, true);

	/* dwm += sqrt(dx^2+dz^2) * 0.6 [Entity.moveEntity:479]; prev was rolled
	 * first [Entity.moveEntity:199]. */
	assert(FEQ(b.dwm_prev, 0.0F));
	assert(FEQ(b.dwm, 0.2F * 0.6F));

	/* f = sqrt(0.3^2) = 0.3 capped at 0.1 [onLivingUpdate:182-184];
	 * cameraYaw += (0.1 - 0) * 0.4 [onLivingUpdate:194]. */
	assert(FEQ(b.cam_yaw, 0.1F * 0.4F));
	/* grounded => pitch term zeroed [onLivingUpdate:190-192]. */
	assert(FEQ(b.cam_pitch, 0.0F));

	/* Second tick, airborne and falling: yaw target zeroed
	 * [onLivingUpdate:186-188], pitch = atan(-motionY*0.2)*15 smoothed by
	 * 0.8 -- NOT 0.4 [onLivingUpdate:195]. */
	float yaw_before = b.cam_yaw;
	camera_bob_tick(&b, 0.0F, 0.0F, 0.3F, -0.5F, 0.0F, false, true);
	assert(FEQ(b.cam_yaw_prev, yaw_before));
	assert(FEQ(b.cam_yaw, yaw_before + (0.0F - yaw_before) * 0.4F));
	float pitch_target = atanf(0.5F * 0.2F) * 15.0F;
	assert(FEQ(b.cam_pitch, pitch_target * 0.8F));

	/* Dead: BOTH terms zeroed regardless of motion [onLivingUpdate:186-192]. */
	struct camera_bob_state d = {0};
	d.cam_yaw = 0.08F;
	d.cam_pitch = 2.0F;
	camera_bob_tick(&d, 1.0F, 0.0F, 0.3F, -0.5F, 0.0F, true, false);
	assert(FEQ(d.cam_yaw, 0.08F + (0.0F - 0.08F) * 0.4F));
	assert(FEQ(d.cam_pitch, 2.0F * (1.0F - 0.8F)));
}

static void test_terms_formula(void) {
	/* Hand-pinned state; terms must match setupViewBobbing's formulas
	 * [740c583 EntityRenderer.setupViewBobbing:174-182] evaluated here with
	 * the same float ops. */
	struct camera_bob_state b = {
		.dwm = 1.30F,
		.dwm_prev = 1.24F,
		.cam_yaw = 0.09F,
		.cam_yaw_prev = 0.07F,
		.cam_pitch = 1.5F,
		.cam_pitch_prev = 1.0F,
	};
	float partial = 0.25F;
	float t[5];
	camera_bob_terms(&b, partial, t);

	float walk = b.dwm - b.dwm_prev;
	float f2 = -(b.dwm + walk * partial);
	float yaw = b.cam_yaw_prev + (b.cam_yaw - b.cam_yaw_prev) * partial;
	float pitch = b.cam_pitch_prev + (b.cam_pitch - b.cam_pitch_prev) * partial;

	assert(FEQ(t[0], sinf(f2 * (float)M_PI) * yaw * 0.5F));
	assert(FEQ(t[1], -fabsf(cosf(f2 * (float)M_PI) * yaw)));
	assert(FEQ(t[2], sinf(f2 * (float)M_PI) * yaw * 3.0F));
	assert(FEQ(t[3], fabsf(cosf(f2 * (float)M_PI - 0.2F) * yaw) * 5.0F));
	assert(FEQ(t[4], pitch));

	/* ty is a magnitude-drop, never a lift; rxa's -0.2 phase offset means it
	 * is NOT rxb-shaped -- both catch sign/order swaps in a port refactor. */
	assert(t[1] <= 0.0F);
}

static void test_rest_decays_to_zero(void) {
	/* Standing still (or no player): every term converges to 0 -- the bob
	 * must vanish at rest, not idle-sway. */
	struct camera_bob_state b = {0};
	camera_bob_tick(&b, 0.5F, 0.0F, 0.25F, 0.0F, 0.0F, true, true);
	for(int i = 0; i < 200; i++)
		camera_bob_tick(&b, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, true, true);
	float t[5];
	camera_bob_terms(&b, 0.0F, t);
	for(int i = 0; i < 5; i++)
		assert(fabsf(t[i]) < 1e-4F);
}

int main(void) {
	test_tick_walk_and_smoothing();
	test_terms_formula();
	test_rest_decays_to_zero();
	printf("test_view_bobbing: PASS\n");
	return 0;
}
