/*
	Pure Beta 1.7.3 EntityTNTPrimed behavior goldens.

	Ground truth: decompile 740c583 EntityTNTPrimed.onUpdate and
	RenderTNTPrimed.func_153_a. The fuse test is deliberately post-decrement:
	updates 1..80 leave fuse 79..0 alive; update 81 explodes.
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "entity/entity_primed_tnt.h"

static bool near(float a, float b) {
	return fabsf(a - b) < 1.0e-6f;
}

static void test_fuse_post_decrement(void) {
	int fuse = PRIMED_TNT_FUSE_TICKS;
	assert(fuse == 80);
	for(int update = 1; update <= 80; update++) {
		assert(!primed_tnt_fuse_update_should_explode(&fuse));
		assert(fuse == 80 - update);
	}
	assert(fuse == 0);
	assert(primed_tnt_fuse_update_should_explode(&fuse));
	assert(fuse == -1);

	fuse = 0;
	assert(primed_tnt_fuse_update_should_explode(&fuse));
	assert(fuse == -1);
}

static void test_launch(void) {
	vec3 vel;
	primed_tnt_launch_velocity(0.0, vel);
	assert(near(vel[0], 0.0f));
	assert(near(vel[1], 0.2f));
	assert(near(vel[2], -0.02f));

	/* The horizontal launch always retains magnitude 0.02. */
	primed_tnt_launch_velocity(0.75, vel);
	assert(near(sqrtf(vel[0] * vel[0] + vel[2] * vel[2]), 0.02f));

	/*
	 * Java seed 20938 produces this exact Math.random() double. Beta narrows
	 * only AFTER multiplying by the double PI literal and 2.0, selecting
	 * MathHelper table entry 752. Narrowing the sample first selects 753.
	 * [740c583: EntityTNTPrimed.<init>]
	 */
	primed_tnt_launch_velocity(0.6583208773019011, vel);
	assert(vel[0] == -0.0014406929f); /* -SIN_TABLE[752] * .02F */
}

static void test_render_scale(void) {
	assert(near(primed_tnt_render_scale(80, 0.0f), 1.0f));
	assert(near(primed_tnt_render_scale(9, 0.0f), 1.0f));
	assert(primed_tnt_render_scale(8, 0.0f) > 1.0f);
	assert(near(primed_tnt_render_scale(0, 0.0f), 1.19683f));
	assert(primed_tnt_render_scale(0, 0.5f)
		   > primed_tnt_render_scale(0, 0.0f));
}

static void test_render_flash(void) {
	/* Beta toggles in five-fuse-value bands, not the old two-tick snow swap. */
	assert(primed_tnt_flash_active(80));
	assert(!primed_tnt_flash_active(79));
	assert(!primed_tnt_flash_active(75));
	assert(primed_tnt_flash_active(74));
	assert(primed_tnt_flash_active(70));
	assert(primed_tnt_flash_active(4));
	assert(primed_tnt_flash_active(0));
	assert(!primed_tnt_flash_active(-1));

	assert(near(primed_tnt_flash_amount(80, 0.0f), 0.152f));
	assert(near(primed_tnt_flash_amount(79, 0.0f), 0.0f));
	assert(near(primed_tnt_flash_amount(0, 0.0f), 0.792f));
}

int main(void) {
	test_fuse_post_decrement();
	test_launch();
	test_render_scale();
	test_render_flash();
	printf("test_tnt_fuse: OK (entity post-decrement fuse + launch/render goldens)\n");
	return 0;
}
