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

#ifndef WORLDGEN_BETA_MATH_H
#define WORLDGEN_BETA_MATH_H

// Minecraft Beta MathHelper.sin/cos: a 65536-entry float lookup table indexed
// by angle * 10430.378. Beta's world generation (cave/ravine/decoration tunnel
// paths) accumulates sin/cos of yaw/pitch over many segments, so reproducing
// the table-based result EXACTLY -- not libm sin/cos -- is required for
// byte-faithful generation. Vanilla builds the table with double-precision
// arguments; this matches that.
void beta_math_init(void);

float mh_sin(float x);
float mh_cos(float x);

#endif
