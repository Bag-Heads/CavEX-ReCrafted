/*
	test_piston_step_compare_golden -- the piston-push step route's acceptance
	test is Beta's double comparison (lever-piston-0925).

	Spec: Entity.moveEntity:398-402 [mcb173-retromcp src_original]
	    keep flat when its squared horizontal length >= the step's
	with both routes' x/z components held as doubles.

	pmove_step_route_wins (source/block/pmove_step_compare.h), used by
	redstone_piston_server.c's pmove_move_body, must equal that expression
	evaluated on the float components widened to double:
	  1. the two-axis inputs where the replaced float form disagreed
	     (flat (1,0) vs step (1-ulp, 2^-12*1.41); flat (0.25,0) vs step
	     (0.25, ~1e-5)) -- Beta takes the step, the float form kept flat;
	  2. an underflowing single-axis pair (1e-23 vs 2e-23): float squares are
	     both 0, Beta's double squares are not -- Beta takes the step;
	  3. ties keep the flat pass;
	  4. every adjacent-float single-axis pair sampled over (1e-3, 2): the
	     float and double forms agree, i.e. real piston pushes (single-axis
	     requests) are unchanged outside underflow.
	RED against the float form (measured by substituting it).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "block/pmove_step_compare.h"

static int s_checks, s_failures;

static void check(const char* what, bool ok) {
	s_checks++;
	if(!ok) {
		s_failures++;
		fprintf(stderr, "FAIL %s\n", what);
	}
}

/* The decompile's expression, verbatim, on doubles. */
static bool beta(double fx, double fz, double sx, double sz) {
	return !(fx * fx + fz * fz >= sx * sx + sz * sz);
}

/* The replaced form (float squares and sums, strict >). */
static bool float_form(float fx, float fz, float sx, float sz) {
	volatile float flat = fx * fx + fz * fz;
	volatile float step = sx * sx + sz * sz;
	return step > flat;
}

static void pin(const char* what, float fx, float fz, float sx, float sz,
				bool beta_wins, bool float_differs) {
	check(what, pmove_step_route_wins(fx, fz, sx, sz) == beta_wins);
	check("reference: decompile expression",
		  beta(fx, fz, sx, sz) == beta_wins);
	if(float_differs)
		check("reference: the float form disagrees here",
			  float_form(fx, fz, sx, sz) != beta_wins);
}

int main(void) {
	/* 1. two-axis disagreements (found by search; hex-exact inputs). */
	pin("flat (1,0) vs step (1-ulp, 0x1.6a3fd6p-12): step wins", 1.0F, 0.0F,
		0x1.fffffep-1F, 0x1.6a3fd6p-12F, true, true);
	pin("flat (0.25,0) vs step (0.25, 0x1.4f8b58p-17): step wins", 0.25F,
		0.0F, 0.25F, 0x1.4f8b58p-17F, true, true);

	/* 2. underflowing single-axis squares. */
	pin("underflow: flat 1e-23 vs step 2e-23: step wins", 1e-23F, 0.0F,
		2e-23F, 0.0F, true, true);

	/* 3. ties keep flat. */
	pin("tie (0.25,0) vs (0.25,0): flat kept", 0.25F, 0.0F, 0.25F, 0.0F, false,
		false);
	pin("tie (0,0.3) vs (0.3,0): flat kept", 0.0F, 0.3F, 0.3F, 0.0F, false,
		false);
	pin("zero vs zero: flat kept", 0.0F, 0.0F, 0.0F, 0.0F, false, false);

	/* 4. single-axis adjacent floats: both forms agree (both orders). */
	int pairs = 0;
	for(float a = 1e-3F; a < 2.0F; a = nextafterf(a, 3.0F)) {
		const float b = nextafterf(a, 3.0F);
		const bool up = pmove_step_route_wins(a, 0.0F, b, 0.0F);
		const bool down = pmove_step_route_wins(b, 0.0F, a, 0.0F);
		if(up != float_form(a, 0.0F, b, 0.0F) || !up
		   || down != float_form(b, 0.0F, a, 0.0F) || down) {
			check("single-axis adjacent pair agrees with the float form", false);
			fprintf(stderr, "  a=%a\n", (double)a);
			break;
		}
		pairs++;
		if(a > 0.01F)
			a *= 1.0001F;
	}
	check("single-axis sweep ran", pairs > 1000);
	printf("single-axis adjacent pairs checked: %d\n", pairs);

	if(s_failures) {
		fprintf(stderr, "test_piston_step_compare_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_piston_step_compare_golden (%d checks)\n", s_checks);
	return 0;
}
