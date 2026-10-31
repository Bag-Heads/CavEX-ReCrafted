/*
	test_arrow_spread -- pins the Beta 1.7.3 skeleton arrow inaccuracy (M2).
	Binds the SHARED source/entity/combat.h, so the deviation constant, the
	per-deviation sigma and the apply-to-normalized-heading order are the
	exact ones the skeleton fire site executes.

	Beta setArrowHeading(d, d1, d2, speed, deviation): normalize the heading,
	add nextGaussian() * 0.0075 * deviation per axis, THEN scale by speed.
	Skeletons fire (0.6F, 12F): sigma 0.09 per axis -- the signature miss.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-5f;
}

int main(void) {
	// The Beta constants.
	assert(feq(SKELETON_ARROW_DEVIATION, 12.0f));
	assert(feq(ARROW_SPREAD_PER_DEVIATION, 0.0075f));
	assert(feq(ARROW_SPEED, 0.6f)); // setArrowHeading 4th arg, b1.7.3 skeleton
	// sigma per axis at deviation 12 = 0.09 -- the number that makes a
	// skeleton miss at range.
	assert(feq(ARROW_SPREAD_PER_DEVIATION * SKELETON_ARROW_DEVIATION, 0.09f));

	// Apply-order pin: spread perturbs the NORMALIZED heading (speed scaling
	// happens after, at the caller). Fixed gaussians -> exact arithmetic.
	{
		float dir[3] = {1.0f, 0.0f, 0.0f};
		arrow_apply_spread(dir, 1.0f, -1.0f, 0.5f, SKELETON_ARROW_DEVIATION);
		assert(feq(dir[0], 1.09f));
		assert(feq(dir[1], -0.09f));
		assert(feq(dir[2], 0.045f));
	}

	// Zero gaussians = a perfect shot (the spread is purely additive noise).
	{
		float dir[3] = {0.0f, 0.6f, 0.8f};
		arrow_apply_spread(dir, 0.0f, 0.0f, 0.0f, SKELETON_ARROW_DEVIATION);
		assert(feq(dir[0], 0.0f) && feq(dir[1], 0.6f) && feq(dir[2], 0.8f));
	}

	printf("test_arrow_spread: OK (deviation 12, sigma 0.09, "
		   "normalize-then-spread-then-speed)\n");
	return 0;
}
