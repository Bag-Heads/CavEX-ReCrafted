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

#ifndef WORLDGEN_JMATH_H
#define WORLDGEN_JMATH_H

#include <math.h>
#include <stdint.h>

// Java's (int) cast of a double: truncate toward zero, NaN -> 0, and clamp to
// the int32 range. Beta 1.7.3's noise generators feed doubled-up world
// coordinates through this cast with NO modulo folding, which is precisely
// what produces the "farlands" precision artifacts at extreme coordinates --
// so reproducing the cast (clamp included) byte-for-byte is REQUIRED for
// faithful output. Both the Perlin and Simplex generators floor with this.
static inline int32_t java_double_to_int(double v) {
	if(isnan(v))
		return 0;
	if(v >= 2147483647.0)
		return INT32_MAX;
	if(v <= -2147483648.0)
		return INT32_MIN;
	// Java's (int) cast truncates TOWARD ZERO, which is exactly what a C double->int
	// conversion does (C99 6.3.1.4) -- one cvttsd2si/fctiwz instruction, vs the
	// floor()/ceil() this used to call 3x per Perlin sample (a libm call on PPC
	// where there is no inline round). BYTE-IDENTICAL: floor(v) for v>0 and ceil(v)
	// for v<0 both equal the truncating cast in the in-range case the guards above
	// leave; the determinism/lake/tree equivalence tests prove the worldgen output
	// is unchanged. The guards stay because the cast is UB for NaN/overflow.
	return (int32_t)v;
}

// Java's (int) cast of a float (same narrowing semantics as above). Used by the
// Beta sin/cos table index and the cave carve's floor math.
static inline int32_t java_float_to_int(float v) {
	if(isnan(v))
		return 0;
	if(v >= 2147483647.0f)
		return INT32_MAX;
	if(v <= -2147483648.0f)
		return INT32_MIN;
	return (int32_t)v; // truncate toward zero == floorf(v>0)/ceilf(v<0); see above
}

// Java's MathHelper.floor_double: a TRUE floor (toward -infinity), distinct
// from java_double_to_int's truncate-toward-zero above -- they differ by
// exactly one whenever v has a negative fractional part (tree.c's bt_floori
// doc comment states the same distinction). Composed from java_double_to_int
// (truncate) plus the one-step adjustment, matching the idiom already
// duplicated per-site in populate.c (pop_floor), minable.c (mn_floor),
// cave.c (cv_floor) and portal_teleporter.c (floor_d) -- this is the
// CODESTYLE 3.2 "third occurrence becomes the shared helper" extraction
// trigger, landed by the map-data slice's own (at least sixth) transcription
// of the identical idiom. The four call sites above predate this shared
// home; retrofitting them is a separate, out-of-scope refactor commit (not
// bundled with any single port slice) left for a future pass.
static inline int32_t java_floor_double(double v) {
	int32_t i = java_double_to_int(v);
	return v < (double)i ? i - 1 : i;
}

#endif
