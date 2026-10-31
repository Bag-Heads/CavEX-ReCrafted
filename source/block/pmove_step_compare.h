/*
	Beta 1.7.3 step-route acceptance, Entity.moveEntity:398-402
	[../mcb173-retromcp/minecraft/src_original/net/minecraft/src]:

	    the flat pass is kept when flat_x^2 + flat_z^2 >= step_x^2 + step_z^2
	    (restoring the saved pre-step state)


	flat_x/flat_z are the flat pass's resolved x/z, step_x/step_z the step
	route's, all DOUBLES: the squares and the sum are double arithmetic and
	a tie keeps the flat pass. This port's resolved components are floats;
	widening each to double before squaring makes every square exact (24-bit
	significands square into 48 bits) and leaves only the two sums to round,
	as in Beta.

	The float form this replaced (step_d2 > flat_d2 on float squares) differs
	on two-axis inputs, e.g. flat (0.25, 0) vs step (0.25, 1e-5): the float
	sum rounds the step's 1e-10 contribution away and keeps the flat pass,
	where Beta takes the step. It also differs once a square underflows
	(|component| < ~1e-19). For a single-axis request -- every piston push --
	adjacent floats never collapse to the same float square, so the two forms
	agree outside underflow. tests/test_piston_step_compare_golden.c.

	Pure: no globals, no I/O.
*/

#ifndef PMOVE_STEP_COMPARE_H
#define PMOVE_STEP_COMPARE_H

#include <stdbool.h>

/* True when the step route wins (Beta keeps the flat pass on >=). */
static inline bool pmove_step_route_wins(float flat_x, float flat_z,
										 float step_x, float step_z) {
	const double flat_d2 = (double)flat_x * (double)flat_x
		+ (double)flat_z * (double)flat_z;
	const double step_d2 = (double)step_x * (double)step_x
		+ (double)step_z * (double)step_z;
	return !(flat_d2 >= step_d2);
}

#endif
