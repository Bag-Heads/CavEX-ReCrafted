/*
	world.c's translucent-pass texture-animation offset used to compute:

		int anim = (time_diff_ms(w->anim_timer, time_get()) * 7 / 800) % 28;

	time_diff_ms() returns int32_t, so `* 7` is 32-bit arithmetic: once a
	world/session has been open past INT32_MAX/7 ~= 306,783,378 ms (~85.2
	hours), the multiply overflows (signed overflow is UB in C -- in practice,
	on every toolchain this project ships on, it silently wraps and can even
	corrupt the value in ways that survive the later `% 28`, producing an
	out-of-texture-animation-cycle index or a UBSan trap under the project's
	asan/ubsan CI leg).

	The fix widens the SAME formula (same operator order: multiply, divide,
	modulo) to int64_t before the multiply, so it never overflows for any
	int32_t input, and is otherwise byte-identical for every value that fit
	in 32 bits before. This test mirrors the fixed formula exactly and:
	  1) cross-checks it against the pre-fix 32-bit formula for a spread of
	     "small" (non-overflowing) deltas -- proving zero behaviour change in
	     the range that mattered before.
	  2) exercises deltas at and beyond the old 32-bit overflow boundary
	     (up to INT32_MAX) and proves the widened formula (a) never invokes
	     signed-overflow UB (computed in int64_t, which cannot overflow for
	     ANY int32_t input multiplied by 7 -- max magnitude ~1.5e10, far
	     inside int64_t's ~9.2e18 range -- verified against an independent
	     double-precision reference, exact here since 1.5e10 is well under
	     double's 2^53 exact-integer ceiling) and (b) always lands in the
	     valid [0, 27] cycle range the caller indexes with (anim/14, anim%14).

	If you touch world.c's `int anim = ...` line, update the mirror here too.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Mirrors the FIXED world.c line exactly (int64_t widened multiply/divide/mod,
   same operator order as the original 32-bit formula). */
static int anim_new(int32_t diff_ms) {
	return (int)((int64_t)diff_ms * 7 / 800 % 28);
}

/* Mirrors the PRE-FIX 32-bit formula, used ONLY on inputs small enough that
   `diff_ms * 7` cannot overflow int32_t (diff_ms <= INT32_MAX/7), where its
   behaviour is well-defined and must match the widened version exactly. */
static int anim_old_safe_range(int32_t diff_ms) {
	assert(diff_ms <= INT32_MAX / 7 && diff_ms >= INT32_MIN / 7);
	int32_t product = diff_ms * 7; /* well-defined here by the assert above */
	return (product / 800) % 28;
}

/* Independent double-precision reference for the widened path (avoids relying
   on int64_t arithmetic to check itself). Exact for every value exercised
   here: diff_ms is non-negative and <= INT32_MAX, so diff_ms*7 <= ~1.5e10,
   comfortably inside double's exact-integer range (2^53 ~= 9.0e15). trunc()
   matches C's truncate-toward-zero integer division for non-negative operands. */
static int anim_reference_double(int32_t diff_ms) {
	double product = (double)diff_ms * 7.0;
	double divided = trunc(product / 800.0);
	double modded = fmod(divided, 28.0);
	return (int)modded;
}

static void test_matches_old_formula_in_safe_range(void) {
	static const int32_t deltas[] = {
		0, 1, 7, 100, 799, 800, 801, 12345, 999999,
		1000000, 50000000, 306783378 /* INT32_MAX/7, the last safe value */
	};
	for(size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
		int32_t d = deltas[i];
		int want = anim_old_safe_range(d);
		int got = anim_new(d);
		assert(got == want);
		assert(got >= 0 && got < 28);
	}
	printf("test_world_anim_timer_overflow: widened formula matches the "
		   "original 32-bit formula for every non-overflowing delta tested "
		   "(up to INT32_MAX/7 ms, the exact old overflow boundary)\n");
}

static void test_no_overflow_past_old_boundary(void) {
	/* One past the old boundary, a session at ~90h/~150h, and INT32_MAX
	   itself (~24.86 days) -- all of these overflowed `diff_ms * 7` in
	   int32_t before the fix (UB in C; observed to silently wrap on this
	   project's toolchains, corrupting the /800 % 28 result). */
	static const int32_t deltas[] = {
		306783379,			  /* INT32_MAX/7 + 1: the first value that overflowed */
		324000000,			  /* ~90 hours */
		540000000,			  /* ~150 hours */
		INT32_MAX,			  /* ~24.86 days, the extreme case */
	};
	for(size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
		int32_t d = deltas[i];
		int got = anim_new(d);
		int want = anim_reference_double(d);
		/* Correct against an independent wide-arithmetic oracle. */
		assert(got == want);
		/* Always a valid animation-cycle index -- the caller does
		   (anim/14, anim%14) with no further clamping. */
		assert(got >= 0 && got < 28);
	}
	printf("test_world_anim_timer_overflow: widened formula stays correct "
		   "(matches an independent double-precision reference) and in-range "
		   "[0,28) "
		   "for deltas from just past the old 32-bit overflow boundary "
		   "(%d ms, ~85.2h) up to INT32_MAX (~24.86 days) -- no more "
		   "signed-overflow UB on a long-running session\n",
		   306783379);
}

int main(void) {
	test_matches_old_formula_in_safe_range();
	test_no_overflow_past_old_boundary();
	printf("test_world_anim_timer_overflow: all assertions passed\n");
	return 0;
}
