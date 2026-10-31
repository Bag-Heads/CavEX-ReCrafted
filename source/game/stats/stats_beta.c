/* Copyright (c) 2026 CaveWii_v2 contributors */
/* Beta 1.7.3 statistic rounding [docs/achievements_design.md 1.4]. The header
 * carries the decompile citations and the reason each chain is written the
 * way it is; this file is nothing but those chains. */

#include <math.h>

#include "stats_beta.h"

/* ---- Java's THREE conversions, none of which is C's cast [R2 LOW 10] -----
 *
 * `(int32_t)floorf(v + 0.5F)` is undefined behaviour in C the moment the
 * operand is a NaN or outside the destination range (C11 6.3.1.4p1: "the
 * behavior is undefined" -- in practice INT_MIN on x86, 0 or a saturated
 * value elsewhere, and a trap is permitted). Java's conversions are TOTAL and
 * specified, and Beta's numbers are Java's, so the three below reproduce the
 * JLS rules instead of hoping the host agrees with them. They are the only
 * thing between a corrupt float in a movement payload and a garbage counter
 * (or a trap) on the Wii. */

/* JLS 5.1.3, floating point -> int: NaN becomes 0; otherwise the value is
 * rounded toward zero, and anything too large in magnitude SATURATES at
 * Integer.MAX_VALUE / Integer.MIN_VALUE (which covers the infinities). This
 * is the conversion `Math.round(float)` itself performs -- it returns an
 * `int`. Both bounds are exact doubles and the argument is always integral
 * here (it comes out of floor), so the compare needs no epsilon. */
static int32_t java_to_int(double v) {
	if(!(v == v))
		return 0; /* NaN */
	if(v >= 2147483648.0)
		return INT32_MAX;
	if(v <= -2147483649.0)
		return INT32_MIN;
	return (int32_t)v;
}

/* JLS 5.1.3, floating point -> long: the same rule at 64 bits. This is what
 * `Math.round(double)` performs -- it returns a `long`. */
static int64_t java_to_long(double v) {
	if(!(v == v))
		return 0; /* NaN */
	if(v >= 9223372036854775808.0) /* 2^63, exactly representable */
		return INT64_MAX;
	if(v < -9223372036854775808.0)
		return INT64_MIN;
	return (int64_t)v;
}

/* JLS 5.1.3, long -> int: a NARROWING primitive conversion, which "simply
 * discards all but the n lowest order bits" -- it does NOT saturate. That is
 * Beta's own `(int)` at the two `(int)Math.round(...)` call sites, and it is
 * why the double chain and the float chain disagree at the extremes: Java
 * gives `(int)Math.round(3e9d)` == -1294967296 and `Math.round(3e9f)` ==
 * Integer.MAX_VALUE. Both are reproduced exactly rather than reconciled --
 * the decompile is the spec. Written through uint32_t so the wrap is defined
 * in C too. */
static int32_t java_long_to_int(int64_t v) {
	uint32_t low = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
	if(low <= (uint32_t)INT32_MAX)
		return (int32_t)low;
	return (int32_t)(low - 2147483648u) + INT32_MIN;
}

/* Java 6/7 `Math.round(float)` == `(int)Math.floor(a + 0.5f)`. The addition
 * is FLOAT: doing it in double would round a different value. */
static int32_t beta_round_float(float v) {
	return java_to_int(floorf(v + 0.5F));
}

/* Java `Math.round(double)` == `(long)Math.floor(a + 0.5d)`, then Beta's own
 * `(int)` narrowing at the two call sites that use it. */
static int32_t beta_round_double(double v) {
	return java_long_to_int(java_to_long(floor(v + 0.5)));
}

/* MathHelper.sqrt_double: `return (float)Math.sqrt(d)` -- the square root is
 * taken in double and the RESULT is truncated to float. */
static float beta_sqrt_double(double d) {
	return (float)sqrt(d);
}

int32_t stats_beta_horizontal_cm(double dx, double dz) {
	return beta_round_float(beta_sqrt_double(dx * dx + dz * dz) * 100.0F);
}

int32_t stats_beta_3d_cm(double dx, double dy, double dz) {
	return beta_round_float(beta_sqrt_double(dx * dx + dy * dy + dz * dz)
							* 100.0F);
}

int32_t stats_beta_climb_cm(double dy) {
	return beta_round_double(dy * 100.0);
}

int32_t stats_beta_fall_cm(float f) {
	/* The `>= 2.0F` compare is on the float fall distance, before the
	 * widening [EntityPlayer.fall]. */
	if(!(f >= 2.0F))
		return 0;
	return beta_round_double((double)f * 100.0);
}

bool stats_beta_fly_counts(int32_t cm) {
	return cm > 25;
}

bool stats_beta_walk_counts(int32_t cm) {
	return cm > 0;
}
